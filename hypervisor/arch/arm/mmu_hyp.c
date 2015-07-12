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

#include <asm/control.h>
#include <asm/setup.h>
#include <asm/setup_mmu.h>
#include <asm/sysregs.h>
#include <jailhouse/paging.h>
#include <jailhouse/printk.h>

/*
 * Two identity mappings need to be created for enabling the MMU: one for the
 * code and one for the stack.
 * There should not currently be any conflict with the existing mappings, but we
 * still make sure not to override anything by using the 'conflict' flag.
 */
static struct {
	unsigned long addr;
	unsigned long flags;
	bool conflict;
} id_maps[2];

extern unsigned long trampoline_start, trampoline_end;

/* When disabling Jailhouse, we will need to restore the Linux stub */
static unsigned long saved_vectors = 0;

static int set_id_map(int i, unsigned long address, unsigned long size)
{
	if (i >= ARRAY_SIZE(id_maps))
		return -ENOMEM;

	/* The trampoline code should be contained in one page. */
	if ((address & PAGE_MASK) != ((address + size - 1) & PAGE_MASK)) {
		printk("FATAL: Unable to IDmap more than one page at at time.\n");
		return -E2BIG;
	}

	id_maps[i].addr = address;
	id_maps[i].conflict = false;
	id_maps[i].flags = PAGE_DEFAULT_FLAGS;

	return 0;
}

static void create_id_maps(void)
{
	unsigned long i;
	bool conflict;

	for (i = 0; i < ARRAY_SIZE(id_maps); i++) {
		conflict = (paging_virt2phys(&hv_paging_structs,
				id_maps[i].addr, PAGE_PRESENT_FLAGS) !=
				INVALID_PHYS_ADDR);
		if (conflict) {
			/*
			 * TODO: Get the flags, and update them if they are
			 * insufficient. Save the current flags in id_maps.
			 * This extraction should be implemented in the core.
			 */
		} else {
			paging_create(&hv_paging_structs, id_maps[i].addr,
				PAGE_SIZE, id_maps[i].addr, id_maps[i].flags,
				PAGING_NON_COHERENT);
		}
		id_maps[i].conflict = conflict;
	}
}

static void destroy_id_maps(void)
{
	unsigned long i;

	for (i = 0; i < ARRAY_SIZE(id_maps); i++) {
		if (id_maps[i].conflict) {
			/* TODO: Switch back to the original flags */
		} else {
			paging_destroy(&hv_paging_structs, id_maps[i].addr,
				       PAGE_SIZE, PAGING_NON_COHERENT);
		}
	}
}

static void __attribute__((naked)) __attribute__((noinline))
cpu_switch_el2(unsigned long phys_bootstrap, virt2phys_t virt2phys)
{
	asm volatile(
		/*
		 * The linux hyp stub allows to install the vectors with a
		 * single hvc. The vector base address is in r0
		 * (phys_bootstrap).
		 */
		"hvc	#0\n\t"

		/*
		 * Now that the bootstrap vectors are installed, call setup_el2
		 * with the translated physical values of lr and sp as
		 * arguments.
		 */
		"mov	r0, sp\n\t"
		"push	{lr}\n\t"
		"blx	%0\n\t"
		"pop	{lr}\n\t"
		"push	{r0}\n\t"
		"mov	r0, lr\n\t"
		"blx	%0\n\t"
		"pop	{r1}\n\t"
		"hvc	#0\n\t"
		:
		: "r" (virt2phys)
		/*
		 * The call to virt2phys may clobber all temp registers. This
		 * list ensures that the compiler uses a decent register for
		 * hvirt2phys.
		 */
		: "cc", "memory", "r0", "r1", "r2", "r3");
}

/*
 * This code is put in the id-mapped `.trampoline' section, allowing to enable
 * and disable the MMU in a readable and portable fashion.
 * This process makes the following function quite fragile: cpu_switch_phys2virt
 * attempts to translate LR and SP using a call to the virtual address of
 * phys2virt.
 * Those two registers are thus supposed to be left intact by the whole MMU
 * setup. The stack is all the same usable, since it is id-mapped as well.
 */
static void __attribute__((naked)) __attribute__((section(".trampoline")))
setup_mmu_el2(struct per_cpu *cpu_data, phys2virt_t phys2virt, u64 ttbr)
{
	u32 tcr = T0SZ
		| (TCR_RGN_WB_WA << TCR_IRGN0_SHIFT)
		| (TCR_RGN_WB_WA << TCR_ORGN0_SHIFT)
		| (TCR_INNER_SHAREABLE << TCR_SH0_SHIFT)
		| HTCR_RES1;
	u32 sctlr_el1, sctlr_el2;

	/* Ensure that MMU is disabled. */
	arm_read_sysreg(SCTLR_EL2, sctlr_el2);
	if (sctlr_el2 & SCTLR_M_BIT)
		return;

	/*
	 * This setup code is always preceded by a complete cache flush, so
	 * there is already a few memory barriers between the page table writes
	 * and here.
	 */
	isb();
	arm_write_sysreg(HMAIR0, DEFAULT_HMAIR0);
	arm_write_sysreg(HMAIR1, DEFAULT_HMAIR1);
	arm_write_sysreg(TTBR0_EL2, ttbr);
	arm_write_sysreg(TCR_EL2, tcr);

	/*
	 * Flush HYP TLB. It should only be necessary if a previous hypervisor
	 * was running.
	 */
	arm_write_sysreg(TLBIALLH, 1);
	dsb(nsh);

	/*
	 * We need coherency with the kernel in order to use the setup
	 * spinlocks: only enable the caches if they are enabled at EL1.
	 */
	arm_read_sysreg(SCTLR_EL1, sctlr_el1);
	sctlr_el1 &= (SCTLR_I_BIT | SCTLR_C_BIT);

	/* Enable stage-1 translation */
	arm_read_sysreg(SCTLR_EL2, sctlr_el2);
	sctlr_el2 |= SCTLR_M_BIT | sctlr_el1;
	arm_write_sysreg(SCTLR_EL2, sctlr_el2);
	isb();

	/*
	 * Inlined epilogue that returns to switch_exception_level.
	 * Must not touch anything else than the stack
	 */
	cpu_switch_phys2virt(phys2virt);

	/* Not reached (cannot be a while(1), it confuses the compiler) */
	asm volatile("b	.");
}

/*
 * Shutdown the MMU and returns to EL1 with the kernel context stored in `regs'
 */
static void __attribute__((naked)) __attribute__((section(".trampoline")))
shutdown_el2(struct registers *regs, unsigned long vectors)
{
	u32 sctlr_el2;

	/* Disable stage-1 translation, caches must be cleaned. */
	arm_read_sysreg(SCTLR_EL2, sctlr_el2);
	sctlr_el2 &= ~(SCTLR_M_BIT | SCTLR_C_BIT | SCTLR_I_BIT);
	arm_write_sysreg(SCTLR_EL2, sctlr_el2);
	isb();

	/* Clean the MMU registers */
	arm_write_sysreg(HMAIR0, 0);
	arm_write_sysreg(HMAIR1, 0);
	arm_write_sysreg(TTBR0_EL2, 0);
	arm_write_sysreg(TCR_EL2, 0);
	isb();

	/* Reset the vectors as late as possible */
	arm_write_sysreg(HVBAR, vectors);

	vmreturn(regs);
}

static void check_mmu_map(unsigned long virt_addr, unsigned long phys_addr)
{
	unsigned long phys_base;
	u64 par;

	arm_write_sysreg(ATS1HR, virt_addr);
	isb();
	arm_read_sysreg(PAR_EL1, par);
	phys_base = (unsigned long)(par & PAR_PA_MASK);
	if ((par & PAR_F_BIT) || (phys_base != phys_addr)) {
		printk("VA->PA check failed, expected %x, got %x\n",
				phys_addr, phys_base);
		while (1);
	}
}

/*
 * Jumping to EL2 in the same C code represents an interesting challenge, since
 * it will switch from virtual addresses to physical ones, and then back to
 * virtual after setting up the EL2 MMU.
 * To this end, the setup_mmu_el2 and cpu_switch_el2 functions are naked and
 * must handle the stack themselves.
 */
int switch_exception_level(struct per_cpu *cpu_data)
{
	extern unsigned long bootstrap_vectors;
	extern unsigned long hyp_vectors;

	/* Save the virtual address of the phys2virt function for later */
	phys2virt_t phys2virt = paging_phys2hvirt;
	virt2phys_t virt2phys = paging_hvirt2phys;
	unsigned long phys_bootstrap = virt2phys(&bootstrap_vectors);
	struct per_cpu *phys_cpu_data = (struct per_cpu *)virt2phys(cpu_data);
	unsigned long trampoline_phys = virt2phys((void *)&trampoline_start);
	unsigned long trampoline_size = &trampoline_end - &trampoline_start;
	unsigned long stack_virt = (unsigned long)cpu_data->stack;
	unsigned long stack_phys = virt2phys((void *)stack_virt);
	u64 ttbr_el2;

	/* Check the paging structures as well as the MMU initialisation */
	unsigned long jailhouse_base_phys =
		paging_virt2phys(&hv_paging_structs, JAILHOUSE_BASE,
				 PAGE_DEFAULT_FLAGS);

	/*
	 * The hypervisor stub allows to fetch its current vector base by doing
	 * an HVC with r0 = -1. They will need to be restored when disabling
	 * jailhouse.
	 */
	if (saved_vectors == 0)
		saved_vectors = hvc(-1);

	/*
	 * paging struct won't be easily accessible when initializing el2, only
	 * the CPU datas will be readable at their physical address
	 */
	ttbr_el2 = (u64)virt2phys(hv_paging_structs.root_table) & TTBR_MASK;

	/*
	 * Mirror the mmu setup code, so that we are able to jump to the virtual
	 * address after enabling it.
	 * Those regions must fit on one page.
	 */

	if (set_id_map(0, trampoline_phys, trampoline_size) != 0)
		return -E2BIG;
	if (set_id_map(1, stack_phys, PAGE_SIZE) != 0)
		return -E2BIG;
	create_id_maps();

	/*
	 * Before doing anything hairy, we need to sync the caches with memory:
	 * they will be off at EL2. From this point forward and until the caches
	 * are re-enabled, we cannot write anything critical to memory.
	 */
	arch_cpu_dcaches_flush(CACHES_CLEAN);

	cpu_switch_el2(phys_bootstrap, virt2phys);
	/*
	 * At this point, we are at EL2, and we work with physical addresses.
	 * The MMU needs to be initialised and execution must go back to virtual
	 * addresses before returning, or else we are pretty much doomed.
	 */

	setup_mmu_el2(phys_cpu_data, phys2virt, ttbr_el2);

	/* Sanity check */
	check_mmu_map(JAILHOUSE_BASE, jailhouse_base_phys);

	/* Set the new vectors once we're back to a sane, virtual state */
	arm_write_sysreg(HVBAR, &hyp_vectors);

	/* Remove the identity mapping */
	destroy_id_maps();

	return 0;
}

void __attribute__((noreturn)) arch_shutdown_mmu(struct per_cpu *cpu_data)
{
	static DEFINE_SPINLOCK(map_lock);

	virt2phys_t virt2phys = paging_hvirt2phys;
	void *stack_virt = cpu_data->stack;
	unsigned long stack_phys = virt2phys((void *)stack_virt);
	unsigned long trampoline_phys = virt2phys((void *)&trampoline_start);
	struct registers *regs_phys =
			(struct registers *)virt2phys(guest_regs(cpu_data));

	/* Jump to the identity-mapped trampoline page before shutting down */
	void (*shutdown_fun_phys)(struct registers*, unsigned long);
	shutdown_fun_phys = (void*)virt2phys(shutdown_el2);

	/*
	 * No need to check for size or overlapping here, it has already be
	 * done, and the paging structures will soon be deleted. However, the
	 * cells' CPUs may execute this concurrently.
	 */
	spin_lock(&map_lock);
	paging_create(&hv_paging_structs, stack_phys, PAGE_SIZE, stack_phys,
		      PAGE_DEFAULT_FLAGS, PAGING_NON_COHERENT);
	paging_create(&hv_paging_structs, trampoline_phys, PAGE_SIZE,
		      trampoline_phys, PAGE_DEFAULT_FLAGS,
		      PAGING_NON_COHERENT);
	spin_unlock(&map_lock);

	arch_cpu_dcaches_flush(CACHES_CLEAN);

	/*
	 * Final shutdown:
	 * - disable the MMU whilst inside the trampoline page
	 * - reset the vectors
	 * - return to EL1
	 */
	shutdown_fun_phys(regs_phys, saved_vectors);

	__builtin_unreachable();
}

int arch_map_device(void *paddr, void *vaddr, unsigned long size)
{
	return paging_create(&hv_paging_structs, (unsigned long)paddr, size,
			(unsigned long)vaddr,
			PAGE_DEFAULT_FLAGS | S1_PTE_FLAG_DEVICE,
			PAGING_NON_COHERENT);
}

int arch_unmap_device(void *vaddr, unsigned long size)
{
	return paging_destroy(&hv_paging_structs, (unsigned long)vaddr, size,
			PAGING_NON_COHERENT);
}
