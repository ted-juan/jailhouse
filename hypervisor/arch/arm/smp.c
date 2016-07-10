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

#include <jailhouse/mmio.h>
#include <asm/smp.h>

const unsigned int __attribute__((weak)) smp_mmio_regions;

int __attribute__((weak)) smp_init(struct cell *cell)
{
	return 0;
}
