/*
 * Jailhouse, a Linux-based partitioning hypervisor
 *
 * Copyright (c) Siemens AG, 2016
 *
 * Author:
 *  Jan Kiszka <jan.kiszka@siemens.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include <jailhouse/control.h>
#include <jailhouse/ivshmem.h>
#include <asm/irqchip.h>

void arch_ivshmem_write_doorbell(struct ivshmem_endpoint *ive)
{
	struct ivshmem_endpoint *remote = ive;//->remote;
	struct cell *remote_cell = remote->device->cell;
	u8 pin = remote->cspace[PCI_CFG_INT/4] >> 8;
	unsigned int first_cpu, spi_base;

	for_each_cpu(first_cpu, remote_cell->cpu_set)
		break;

	spi_base = remote_cell->config->vpci_irq_base;

	irqchip_set_pending(per_cpu(first_cpu), spi_base + pin - 1);
}

int arch_ivshmem_update_msix(struct pci_device *device)
{
	return 0;
}
