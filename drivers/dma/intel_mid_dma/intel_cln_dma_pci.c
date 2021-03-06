/*
 * Copyright(c) 2013 Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Contact Information:
 * Intel Corporation
 */

/*
 *  intel_mid_dma.c - Intel Langwell DMA Drivers
 *
 *  Author: Vinod Koul <bryan.odonoghue@linux.intel.com>
 *  This is an entry point for Intel Clanton based DMAC on Clanton's UART
 *  specifically we don't have a dedicated PCI function, instead we have DMAC
 *  regs hung off of a PCI BAR. This entry/exit allows re-use of the core
 *  DMA API for MID devices manipulated to suit our BAR setup
 *  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/pm_runtime.h>
#include <linux/intel_mid_dma.h>
#include <linux/module.h>

#include "intel_mid_dma_core.h"
#include "intel_mid_dma_regs.h"

/**
 * intel_mid_dma_probe -	PCI Probe
 * @pdev: Controller PCI device structure
 * @id: pci device id structure
 *
 * Initialize the PCI device, map BARs, query driver data.
 * Call intel_setup_dma to complete contoller and chan initilzation
 */
int __devinit intel_cln_dma_probe(struct pci_dev *pdev,
					struct middma_device *device)
{
	u32 base_addr, bar_size;
	int err;

	pr_debug("MDMA: probe for %x\n", pdev->device);
	pr_debug("MDMA: CH %d, base %d, block len %d, Periphral mask %x\n",
				device->max_chan, device->chan_base,
				device->block_size, device->pimr_mask);

	device->pdev = pci_dev_get(pdev);
	device->ispci_fn = true;

	base_addr = pci_resource_start(pdev, 1);
	bar_size  = pci_resource_len(pdev, 1);
	device->dma_base = ioremap_nocache(base_addr, DMA_REG_SIZE);
	if (!device->dma_base) {
		pr_err("ERR_MDMA:ioremap failed\n");
		err = -ENOMEM;
		goto err_ioremap;
	}

	err = intel_mid_dma_setup(pdev, device, false);
	if (err)
		goto err_dma;

	return 0;

err_dma:
	iounmap(device->dma_base);
err_ioremap:
	pr_err("ERR_MDMA:Probe failed %d\n", err);
	return err;
}
EXPORT_SYMBOL(intel_cln_dma_probe);

/**
 * intel_mid_dma_remove -	PCI remove
 * @pdev: Controller PCI device structure
 *
 * Free up all resources and data
 * Call shutdown_dma to complete contoller and chan cleanup
 */
void __devexit intel_cln_dma_remove(struct pci_dev *pdev, struct middma_device *device)
{
	intel_mid_dma_shutdown(pdev, device);
}
EXPORT_SYMBOL(intel_cln_dma_remove);

/* Power Management */
/*
* dma_suspend - PCI suspend function
*
* @pci: PCI device structure
* @state: PM message
*
* This function is called by OS when a power event occurs
*/
int intel_cln_dma_suspend(struct middma_device *device)
{
	int i = 0;
	pr_debug("MDMA: dma_suspend called\n");

	for (i = 0; i < device->max_chan; i++) {
		if (device->ch[i].in_use)
			return -EAGAIN;
	}
	dmac1_mask_periphral_intr(device);
	device->state = SUSPENDED;
	return 0;
}
EXPORT_SYMBOL(intel_cln_dma_suspend);

/**
* dma_resume - PCI resume function
*
* @pci:	PCI device structure
*
* This function is called by OS when a power event occurs
*/
int intel_cln_dma_resume(struct middma_device *device)
{
	pr_debug("MDMA: dma_resume called\n");
	device->state = RUNNING;
	iowrite32(REG_BIT0, device->dma_base + DMA_CFG);
	return 0;
}
EXPORT_SYMBOL(intel_cln_dma_resume);

static int intel_cln_dma_runtime_suspend(struct middma_device *device)
{
	device->state = SUSPENDED;
	return 0;
}
EXPORT_SYMBOL(intel_cln_dma_runtime_suspend);

static int intel_cln_dma_runtime_resume(struct middma_device *device)
{
	device->state = RUNNING;
	iowrite32(REG_BIT0, device->dma_base + DMA_CFG);
	return 0;
}
EXPORT_SYMBOL(intel_cln_dma_runtime_resume);


