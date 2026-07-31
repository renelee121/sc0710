// SPDX-License-Identifier: GPL-2.0

#include <linux/kernel.h>
#include <linux/pci.h>

#include "sc0710.h"
#include "sc0710-hd60pro.h"

int sc0710_hd60pro_probe(struct sc0710_dev *dev)
{
        struct pci_dev *pci_dev = dev->pci;

        dev->observational_only = true;

        /*
         * The HD60 Pro may retain DMA state across driver sessions.
         * Keep bus mastering disabled until its reset and DMA protocols
         * are understood.
         */
        pci_clear_master(pci_dev);

        printk(KERN_INFO
               "%s: HD60 Pro attached in observational-only mode\n",
               dev->name);
        printk(KERN_INFO
               "%s: BAR0 size=0x%llx, BAR5 size=0x%x\n",
               dev->name,
               (unsigned long long)pci_resource_len(pci_dev, 0),
               dev->bar1_size);
        printk(KERN_INFO
               "%s: bus mastering, IRQ, DMA, I2C and media nodes disabled\n",
               dev->name);

        return 0;
}

void sc0710_hd60pro_remove(struct sc0710_dev *dev)
{
        pci_clear_master(dev->pci);

        printk(KERN_INFO
               "%s: HD60 Pro observational backend detached\n",
               dev->name);
}