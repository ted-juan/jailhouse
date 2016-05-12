/*
 * Jailhouse AArch64 support
 *
 * Copyright (C) 2015 Huawei Technologies Duesseldorf GmbH
 *
 * Authors:
 *  Antonios Motakis <antonios.motakis@huawei.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include <jailhouse/control.h>
#include <jailhouse/printk.h>
#include <jailhouse/string.h>
#include <asm/control.h>
#include <asm/irqchip.h>
#include <asm/platform.h>
#include <asm/traps.h>

static void arch_reset_el1(struct registers *regs)
{
	/* put the cpu in a reset state */
	/* AARCH64_TODO: handle big endian support */
	arm_write_sysreg(SPSR_EL2, RESET_PSR);
	arm_write_sysreg(SCTLR_EL1, SCTLR_EL1_RES1);
	arm_write_sysreg(CNTKCTL_EL1, 0);
	arm_write_sysreg(PMCR_EL0, 0);

	/* wipe any other state to avoid leaking information accross cells */
	memset(regs, 0, sizeof(struct registers));

	/* AARCH64_TODO: wipe floating point registers */

	/* wipe special registers */
	arm_write_sysreg(SP_EL0, 0);
	arm_write_sysreg(SP_EL1, 0);
	arm_write_sysreg(SPSR_EL1, 0);

	/* wipe the system registers */
	arm_write_sysreg(AFSR0_EL1, 0);
	arm_write_sysreg(AFSR1_EL1, 0);
	arm_write_sysreg(AMAIR_EL1, 0);
	arm_write_sysreg(CONTEXTIDR_EL1, 0);
	arm_write_sysreg(CPACR_EL1, 0);
	arm_write_sysreg(CSSELR_EL1, 0);
	arm_write_sysreg(ESR_EL1, 0);
	arm_write_sysreg(FAR_EL1, 0);
	arm_write_sysreg(MAIR_EL1, 0);
	arm_write_sysreg(PAR_EL1, 0);
	arm_write_sysreg(TCR_EL1, 0);
	arm_write_sysreg(TPIDRRO_EL0, 0);
	arm_write_sysreg(TPIDR_EL0, 0);
	arm_write_sysreg(TPIDR_EL1, 0);
	arm_write_sysreg(TTBR0_EL1, 0);
	arm_write_sysreg(TTBR1_EL1, 0);
	arm_write_sysreg(VBAR_EL1, 0);

	/* wipe timer registers */
	arm_write_sysreg(CNTP_CTL_EL0, 0);
	arm_write_sysreg(CNTP_CVAL_EL0, 0);
	arm_write_sysreg(CNTP_TVAL_EL0, 0);
	arm_write_sysreg(CNTV_CTL_EL0, 0);
	arm_write_sysreg(CNTV_CVAL_EL0, 0);
	arm_write_sysreg(CNTV_TVAL_EL0, 0);

	/* AARCH64_TODO: handle PMU registers */
	/* AARCH64_TODO: handle debug registers */
	/* AARCH64_TODO: handle system registers for AArch32 state */
}

void arch_reset_self(struct per_cpu *cpu_data)
{
	int err = 0;
	unsigned long reset_address = 0;
	struct cell *cell = cpu_data->cell;
	struct registers *regs = guest_regs(cpu_data);
	bool is_shutdown = cpu_data->shutdown;

	if (!is_shutdown)
		err = arch_mmu_cpu_cell_init(cpu_data);
	if (err)
		printk("MMU setup failed\n");

	/*
	 * Note: D-cache cleaning and I-cache invalidation is done on driver
	 * level after image is loaded.
	 */

	/*
	 * We come from the IRQ handler, but we won't return there, so the IPI
	 * is deactivated here.
	 */
	irqchip_eoi_irq(SGI_CPU_OFF, true);

	if (is_shutdown) {
		if (cell != &root_cell) {
			irqchip_cpu_shutdown(cpu_data);

			smc(PSCI_CPU_OFF, 0, 0, 0);
			panic_printk("FATAL: PSCI_CPU_OFF failed\n");
			panic_stop();
		}
		/* arch_shutdown_self resets the GIC on all remaining CPUs. */
	} else {
		err = irqchip_cpu_reset(cpu_data);
		if (err)
			printk("IRQ setup failed\n");
	}

	/* All but the first CPU at reset are waiting for a PSCI resume */
	if (cpu_data->cpu_id != first_cpu(cell->cpu_set))
		reset_address = psci_emulate_spin(cpu_data);

	/* Restore an empty context */
	arch_reset_el1(regs);

	arm_write_sysreg(ELR_EL2, reset_address);

	if (is_shutdown)
		/* Won't return here. */
		arch_shutdown_self(cpu_data);

	vmreturn(regs);
}

int arch_cell_create(struct cell *cell)
{
	int err;

	err = arch_mmu_cell_init(cell);
	if (err)
		return err;

	err = irqchip_cell_init(cell);
	if (err)
		arch_mmu_cell_destroy(cell);
	return err;
}

void arch_flush_cell_vcpu_caches(struct cell *cell)
{
	unsigned int cpu;

	for_each_cpu(cpu, cell->cpu_set)
		if (cpu == this_cpu_id())
			arch_cpu_tlb_flush(per_cpu(cpu));
		else
			per_cpu(cpu)->flush_vcpu_caches = true;
}

void arch_cell_destroy(struct cell *cell)
{
	unsigned int cpu;

	for_each_cpu(cpu, cell->cpu_set)
		arch_reset_cpu(cpu);

	irqchip_cell_exit(cell);

	arch_mmu_cell_destroy(cell);
}

void arch_config_commit(struct cell *cell_added_removed)
{
}

void arch_shutdown(void)
{
	unsigned int cpu;

	/* turn off the hypervisor when we return from the exit handler */
	if (root_cell.cpu_set)
		for_each_cpu(cpu, root_cell.cpu_set)
			per_cpu(cpu)->shutdown = true;
}

void arch_suspend_cpu(unsigned int cpu_id)
{
	/* TODO: the assumption that cpu_id == gic interface number
	 * should be eliminated from both the ARMv8 and ARMv7 ports */
	struct sgi sgi;

	if (psci_cpu_stopped(cpu_id))
		return;

	sgi.routing_mode = 0;
	sgi.aff1 = 0;
	sgi.aff2 = 0;
	sgi.aff3 = 0;
	sgi.targets = 1 << cpu_id;
	sgi.id = SGI_CPU_OFF;

	irqchip_send_sgi(&sgi);

	psci_wait_cpu_stopped(cpu_id);
}

void arch_resume_cpu(unsigned int cpu_id)
{
	/*
	 * Simply get out of the spin loop by returning to handle_sgi
	 * If the CPU is being reset, it already has left the PSCI idle loop.
	 */
	if (psci_cpu_stopped(cpu_id))
		psci_resume(cpu_id);
}

void arch_reset_cpu(unsigned int cpu_id)
{
	unsigned long cpu_data = (unsigned long)per_cpu(cpu_id);

	if (psci_cpu_on(cpu_id, (unsigned long)arch_reset_self, cpu_data))
		printk("ERROR: unable to reset CPU%d (was running)\n", cpu_id);
}

void arch_park_cpu(unsigned int cpu_id)
{
	struct per_cpu *cpu_data = per_cpu(cpu_id);

	/*
	 * Reset always follows park_cpu, so we just need to make sure that the
	 * CPU is suspended
	 */
	if (psci_wait_cpu_stopped(cpu_id) != 0)
		printk("ERROR: CPU%d is supposed to be stopped\n", cpu_id);
	else
		cpu_data->cell->arch.needs_flush = true;
}

void arch_shutdown_cpu(unsigned int cpu_id)
{
	struct per_cpu *cpu_data = per_cpu(cpu_id);

	cpu_data->shutdown = true;

	if (psci_wait_cpu_stopped(cpu_id))
		printk("FATAL: unable to stop CPU%d\n", cpu_id);

	arch_reset_cpu(cpu_id);
}

void __attribute__((noreturn)) arch_panic_stop(void)
{
	psci_cpu_off(this_cpu_data());
	__builtin_unreachable();
}

void arch_panic_park(void)
{
	trace_error(-EINVAL);
	while (1);
}

static void arch_suspend_self(struct per_cpu *cpu_data)
{
	psci_suspend(cpu_data);

	if (cpu_data->flush_vcpu_caches)
		arch_cpu_tlb_flush(cpu_data);
}

void arch_handle_sgi(struct per_cpu *cpu_data, u32 irqn)
{
	cpu_data->stats[JAILHOUSE_CPU_STAT_VMEXITS_MANAGEMENT]++;

	switch (irqn) {
	case SGI_INJECT:
		irqchip_inject_pending(cpu_data);
		break;
	case SGI_CPU_OFF:
		arch_suspend_self(cpu_data);
		break;
	default:
		printk("WARN: unknown SGI received %d\n", irqn);
	}
}

/*
 * Handle the maintenance interrupt, the rest is injected into the cell.
 * Return true when the IRQ has been handled by the hyp.
 */
bool arch_handle_phys_irq(struct per_cpu *cpu_data, u32 irqn)
{
	if (irqn == MAINTENANCE_IRQ) {
		cpu_data->stats[JAILHOUSE_CPU_STAT_VMEXITS_MAINTENANCE]++;

		irqchip_inject_pending(cpu_data);
		return true;
	}

	cpu_data->stats[JAILHOUSE_CPU_STAT_VMEXITS_VIRQ]++;

	irqchip_set_pending(cpu_data, irqn);

	return false;
}

/*
 * We get rid of the virt_id in the AArch64 implementation, since it
 * doesn't really fit with the MPIDR CPU identification scheme on ARM.
 *
 * Until the GICv3 and ARMv7 code has been properly refactored to
 * support this scheme, we stub this call so we can share the GICv2
 * code with ARMv7.
 *
 * TODO: implement MPIDR support in the GICv3 code, so it can be
 * used on AArch64.
 * TODO: refactor out virt_id from the AArch7 port as well.
 */
unsigned int arm_cpu_phys2virt(unsigned int cpu_id)
{
	panic_printk("FATAL: we shouldn't reach here\n");
	panic_stop();

	return -EINVAL;
}
