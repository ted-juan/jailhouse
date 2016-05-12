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

#ifndef _JAILHOUSE_ASM_PERCPU_H
#define _JAILHOUSE_ASM_PERCPU_H

#include <jailhouse/types.h>
#include <asm/paging.h>

/* Keep in sync with struct per_cpu! */
#define PERCPU_SIZE_SHIFT		13
#define PERCPU_STACK_END		PAGE_SIZE
#define PERCPU_LINUX_SAVED_VECTORS	PERCPU_STACK_END

#ifndef __ASSEMBLY__

#include <jailhouse/printk.h>
#include <asm/cell.h>
#include <asm/irqchip.h>
#include <asm/psci.h>
#include <asm/spinlock.h>

struct pending_irq;

struct per_cpu {
	u8 stack[PAGE_SIZE];
	unsigned long saved_vectors;

	/* common fields */
	unsigned int cpu_id;
	struct cell *cell;
	u32 stats[JAILHOUSE_NUM_CPU_STATS];
	int shutdown_state;
	bool failed;

	/* synchronizes parallel insertions of SGIs into the pending ring */
	spinlock_t pending_irqs_lock;
	u16 pending_irqs[MAX_PENDING_IRQS];
	unsigned int pending_irqs_head;
	/* removal from the ring happens lockless, thus tail is volatile */
	volatile unsigned int pending_irqs_tail;
	/* Only GICv3: redistributor base */
	void *gicr_base;

	bool flush_vcpu_caches;

	__attribute__((aligned(16))) struct psci_mbox psci_mbox;
	struct psci_mbox guest_mbox;

	unsigned long mpidr;
} __attribute__((aligned(PAGE_SIZE)));

static inline struct per_cpu *this_cpu_data(void)
{
	struct per_cpu *cpu_data;

	arm_read_sysreg(TPIDR_EL2, cpu_data);
	return cpu_data;
}

#define DEFINE_PER_CPU_ACCESSOR(field)					\
static inline typeof(((struct per_cpu *)0)->field) this_##field(void)	\
{									\
	return this_cpu_data()->field;					\
}

DEFINE_PER_CPU_ACCESSOR(cpu_id)
DEFINE_PER_CPU_ACCESSOR(cell)

static inline struct per_cpu *per_cpu(unsigned int cpu)
{
	extern u8 __page_pool[];

	return (struct per_cpu *)(__page_pool + (cpu << PERCPU_SIZE_SHIFT));
}

static inline struct registers *guest_regs(struct per_cpu *cpu_data)
{
	/* assumes that the cell registers are at the beginning of the stack */
	return (struct registers *)(cpu_data->stack + PERCPU_STACK_END
			- sizeof(struct registers));
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
unsigned int arm_cpu_phys2virt(unsigned int cpu_id);

/* Validate defines */
#define CHECK_ASSUMPTION(assume)	((void)sizeof(char[1 - 2*!(assume)]))

static inline void __check_assumptions(void)
{
	struct per_cpu cpu_data;

	CHECK_ASSUMPTION(sizeof(unsigned long) == (8));
	CHECK_ASSUMPTION(sizeof(struct per_cpu) == (1 << PERCPU_SIZE_SHIFT));
	CHECK_ASSUMPTION(sizeof(cpu_data.stack) == PERCPU_STACK_END);
	CHECK_ASSUMPTION(__builtin_offsetof(struct per_cpu, saved_vectors) ==
			 PERCPU_LINUX_SAVED_VECTORS);
}
#endif /* !__ASSEMBLY__ */

#endif /* !_JAILHOUSE_ASM_PERCPU_H */
