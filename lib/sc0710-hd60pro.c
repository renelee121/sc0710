// SPDX-License-Identifier: GPL-2.0

#include <linux/debugfs.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/seq_file.h>

#include "sc0710.h"
#include "sc0710-hd60pro.h"

struct sc0710_hd60pro_reg {
	u8 bar;
	u32 offset;
	const char *name;
};

static const struct sc0710_hd60pro_reg hd60pro_readable_regs[] = {
	{
		.bar = 0,
		.offset = 0x0030,
		.name = "irq_status_observed",
	},
	{
		.bar = 0,
		.offset = 0x0040,
		.name = "irq_tag_observed",
	},
};

static int
sc0710_hd60pro_read_reg(struct sc0710_dev *dev,
			const struct sc0710_hd60pro_reg *reg,
			u32 *value)
{
	void __iomem *base;
	resource_size_t size;

	switch (reg->bar) {
	case 0:
		base = dev->lmmio[0];
		size = pci_resource_len(dev->pci, 0);
		break;

	case 5:
		base = dev->lmmio[1];
		size = pci_resource_len(dev->pci, 5);
		break;

	default:
		return -EINVAL;
	}

	if (!base)
		return -ENODEV;

	if (size < sizeof(*value) ||
	reg->offset > size - sizeof(*value))
		return -ERANGE;

	*value = readl(base + reg->offset);

	return 0;
}

static int
sc0710_hd60pro_registers_show(struct seq_file *s, void *unused)
{
	struct sc0710_dev *dev = s->private;
	unsigned int i;

	if (!ARRAY_SIZE(hd60pro_readable_regs)) {
		seq_puts(s, "# No MMIO registers whitelisted yet\n");
		return 0;
	}

	for (i = 0; i < ARRAY_SIZE(hd60pro_readable_regs); i++) {
		const struct sc0710_hd60pro_reg *reg;
		u32 value;
		int ret;

		reg = &hd60pro_readable_regs[i];

		ret = sc0710_hd60pro_read_reg(dev, reg, &value);
		if (ret) {
			seq_printf(s,
				"BAR%u[0x%08x] %-24s error=%d\n",
				reg->bar,
				reg->offset,
				reg->name,
				ret);
			continue;
		}

		seq_printf(s,
			"BAR%u[0x%08x] %-24s = 0x%08x\n",
			reg->bar,
			reg->offset,
			reg->name,
			value);
	}

	return 0;
}

static int
sc0710_hd60pro_registers_open(struct inode *inode, struct file *file)
{
	return single_open(file,
			sc0710_hd60pro_registers_show,
			inode->i_private);
}

static const struct file_operations sc0710_hd60pro_registers_fops = {
	.owner		= THIS_MODULE,
	.open		= sc0710_hd60pro_registers_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int sc0710_hd60pro_status_show(struct seq_file *s, void *unused)
{
	struct sc0710_dev *dev = s->private;
	struct pci_dev *pci_dev = dev->pci;
	u16 command;
	int ret;

	ret = pci_read_config_word(pci_dev, PCI_COMMAND, &command);
	if (ret)
		return pcibios_err_to_errno(ret);

	seq_printf(s, "device=%s\n", dev->name);
	seq_printf(s, "pci=%04x:%04x\n",
		pci_dev->vendor,
		pci_dev->device);
	seq_printf(s, "subsystem=%04x:%04x\n",
		pci_dev->subsystem_vendor,
		pci_dev->subsystem_device);
	seq_printf(s, "bar0_size=0x%llx\n",
		(unsigned long long)pci_resource_len(pci_dev, 0));
	seq_printf(s, "bar5_size=0x%llx\n",
		(unsigned long long)pci_resource_len(pci_dev, 5));
	seq_printf(s, "memory_space=%u\n",
		!!(command & PCI_COMMAND_MEMORY));
	seq_printf(s, "bus_master=%u\n",
		!!(command & PCI_COMMAND_MASTER));
	seq_puts(s, "mode=observational-only\n");
	seq_puts(s, "mmio_reads=whitelist-only\n");
	seq_puts(s, "mmio_writes=disabled\n");
	seq_puts(s, "irq=disabled\n");
	seq_puts(s, "dma=disabled\n");

	return 0;
}

static int
sc0710_hd60pro_status_open(struct inode *inode, struct file *file)
{
	return single_open(file,
			sc0710_hd60pro_status_show,
			inode->i_private);
}

static const struct file_operations sc0710_hd60pro_status_fops = {
	.owner		= THIS_MODULE,
	.open		= sc0710_hd60pro_status_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

int sc0710_hd60pro_probe(struct sc0710_dev *dev)
{
	struct pci_dev *pci_dev = dev->pci;
	struct dentry *entry;
	int ret;

	dev->observational_only = true;

	/*
	 * The HD60 Pro may retain DMA state across driver sessions.
	 * Keep bus mastering disabled until its reset and DMA protocols
	 * are understood.
	 */
	pci_clear_master(pci_dev);

	dev->hd60pro_debugfs_dir =
		debugfs_create_dir(dev->name, NULL);

	if (IS_ERR_OR_NULL(dev->hd60pro_debugfs_dir)) {
		ret = dev->hd60pro_debugfs_dir
			? PTR_ERR(dev->hd60pro_debugfs_dir)
			: -ENOMEM;

		dev->hd60pro_debugfs_dir = NULL;
		return ret;
	}

	entry = debugfs_create_file("status",
				0444,
				dev->hd60pro_debugfs_dir,
				dev,
				&sc0710_hd60pro_status_fops);
	if (IS_ERR_OR_NULL(entry)) {
		ret = entry ? PTR_ERR(entry) : -ENOMEM;
		goto err_debugfs;
	}

	entry = debugfs_create_file("registers",
				0444,
				dev->hd60pro_debugfs_dir,
				dev,
				&sc0710_hd60pro_registers_fops);
	if (IS_ERR_OR_NULL(entry)) {
		ret = entry ? PTR_ERR(entry) : -ENOMEM;
		goto err_debugfs;
	}

	pr_info("%s: HD60 Pro attached in observational-only mode\n",
		dev->name);
	pr_info("%s: BAR0 size=0x%llx, BAR5 size=0x%llx\n",
		dev->name,
		(unsigned long long)pci_resource_len(pci_dev, 0),
		(unsigned long long)pci_resource_len(pci_dev, 5));
	pr_info("%s: whitelist MMIO reads enabled; bus mastering, IRQ, DMA, I2C and media nodes disabled\n",
		dev->name);

	return 0;

err_debugfs:
	debugfs_remove_recursive(dev->hd60pro_debugfs_dir);
	dev->hd60pro_debugfs_dir = NULL;

	return ret;
}

void sc0710_hd60pro_remove(struct sc0710_dev *dev)
{
	debugfs_remove_recursive(dev->hd60pro_debugfs_dir);
	dev->hd60pro_debugfs_dir = NULL;

	pci_clear_master(dev->pci);

	pr_info("%s: HD60 Pro observational backend detached\n",
		dev->name);
}