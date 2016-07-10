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

#ifndef JAILHOUSE_ASM_SMP_H_
#define JAILHOUSE_ASM_SMP_H_

#ifndef __ASSEMBLY__

struct cell;

extern const unsigned int smp_mmio_regions;

int smp_init(struct cell *cell);

#endif /* !__ASSEMBLY__ */
#endif /* !JAILHOUSE_ASM_SMP_H_ */
