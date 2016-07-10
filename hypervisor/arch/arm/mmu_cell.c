/*
 * Jailhouse, a Linux-based partitioning hypervisor
 *
 * Copyright (c) ARM Limited, 2014
 *
 * Authors:
 *  Jean-Philippe Brucker <jean-philippe.brucker@arm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include <jailhouse/control.h>
#include <jailhouse/paging.h>
#include <jailhouse/printk.h>
#include <asm/sysregs.h>
#include <asm/control.h>

int arch_map_memory_region(struct cell *cell,
			   const struct jailhouse_memory *mem)
{
	u64 phys_start = mem->phys_start;
	u32 flags = PTE_FLAG_VALID | PTE_ACCESS_FLAG;

	if (mem->flags & JAILHOUSE_MEM_READ)
		flags |= S2_PTE_ACCESS_RO;
	if (mem->flags & JAILHOUSE_MEM_WRITE)
		flags |= S2_PTE_ACCESS_WO;
	if (mem->flags & JAILHOUSE_MEM_IO)
		flags |= S2_PTE_FLAG_DEVICE;
	else
		flags |= S2_PTE_FLAG_NORMAL;
	if (mem->flags & JAILHOUSE_MEM_COMM_REGION)
		phys_start = paging_hvirt2phys(&cell->comm_page);
	/*
	if (!(mem->flags & JAILHOUSE_MEM_EXECUTE))
		flags |= S2_PAGE_ACCESS_XN;
	*/

	return paging_create(&cell->arch.mm, phys_start, mem->size,
		mem->virt_start, flags, PAGING_COHERENT);
}

int arch_unmap_memory_region(struct cell *cell,
			     const struct jailhouse_memory *mem)
{
	return paging_destroy(&cell->arch.mm, mem->virt_start, mem->size,
			      PAGING_COHERENT);
}

unsigned long arch_paging_gphys2phys(struct per_cpu *cpu_data,
				     unsigned long gphys, unsigned long flags)
{
	/* Translate IPA->PA */
	return paging_virt2phys(&cpu_data->cell->arch.mm, gphys, flags);
}

int arch_mmu_cell_init(struct cell *cell)
{
	if (cell->id > 0xff)
		return trace_error(-E2BIG);

	cell->arch.mm.root_paging = cell_paging;
	cell->arch.mm.root_table =
		page_alloc_aligned(&mem_pool, ARM_CELL_ROOT_PT_SZ);

	if (!cell->arch.mm.root_table)
		return -ENOMEM;

	return 0;
}

void arch_mmu_cell_destroy(struct cell *cell)
{
	page_free(&mem_pool, cell->arch.mm.root_table, ARM_CELL_ROOT_PT_SZ);
}

void arm_mmu_cpu_cell_init(struct paging_structures *pg_structs)
{
	unsigned long cell_table = paging_hvirt2phys(pg_structs->root_table);
	u64 vttbr = 0;

	vttbr |= (u64)this_cell()->id << VTTBR_VMID_SHIFT;
	vttbr |= (u64)(cell_table & TTBR_MASK);

	arm_write_sysreg(VTTBR_EL2, vttbr);
	arm_write_sysreg(VTCR_EL2, VTCR_CELL);

	/* Ensure that the new VMID is present before flushing the caches */
	isb();
	/*
	 * At initialisation, arch_config_commit does not act on other CPUs,
	 * since they register themselves to the root cpu_set afterwards. It
	 * means that this unconditionnal flush is redundant on master CPU.
	 */
	arm_cpu_tlb_flush();
}

void arm_cpu_tlb_flush(void)
{
	/*
	 * Invalidate all stage-1 and 2 TLB entries for the current VMID
	 * ERET will ensure completion of these ops
	 */
	tlb_flush_guest();
	dsb(nsh);
}

void arch_cell_caches_flush(struct cell *cell)
{
	this_cpu_data()->flush_dcache = true;

	/*
	 * New instructions may have been written, so the I-cache needs to be
	 * invalidated even though the VMID is different.
	 * A complete invalidation is the only way to ensure all virtual aliases
	 * of these memory locations are invalidated, whatever the cache type.
	 */
	arch_cpu_icache_flush();

	/* ERET will ensure context synchronization */
}
