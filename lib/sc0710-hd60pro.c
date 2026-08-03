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

struct hd60pro_mailbox_snapshot {
	u16 pci_command;
	u32 mailbox_status;
	u32 irq_status;
	u32 irq_tag;
};

static const struct sc0710_hd60pro_reg hd60pro_readable_regs[] = {
	{
		.bar = 0,
		.offset = HD60PRO_BAR0_MAILBOX_STATUS,
		.name = "mailbox_status_observed",
	},
	{
		.bar = 0,
		.offset = HD60PRO_BAR0_IRQ_STATUS,
		.name = "irq_status_observed",
	},
	{
		.bar = 0,
		.offset = HD60PRO_BAR0_IRQ_TAG,
		.name = "irq_tag_observed",
	},
};

static int
sc0710_hd60pro_read_pci_command(struct pci_dev *pci_dev, u16 *command)
{
	int ret;

	ret = pci_read_config_word(pci_dev, PCI_COMMAND, command);
	if (ret)
		return pcibios_err_to_errno(ret);

	return 0;
}

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

	*value = readl((u8 __iomem *)base + reg->offset);

	return 0;
}

static int
sc0710_hd60pro_read_bar0(struct sc0710_dev *dev,
			 u32 offset,
			 u32 *value)
{
	const struct sc0710_hd60pro_reg reg = {
		.bar = 0,
		.offset = offset,
		.name = NULL,
	};

	return sc0710_hd60pro_read_reg(dev, &reg, value);
}

static int
sc0710_hd60pro_take_mailbox_snapshot(
	struct sc0710_dev *dev,
	struct hd60pro_mailbox_snapshot *snapshot)
{
	int ret;

	ret = sc0710_hd60pro_read_pci_command(dev->pci,
					      &snapshot->pci_command);
	if (ret)
		return ret;

	ret = sc0710_hd60pro_read_bar0(
		dev,
		HD60PRO_BAR0_MAILBOX_STATUS,
		&snapshot->mailbox_status);
	if (ret)
		return ret;

	ret = sc0710_hd60pro_read_bar0(
		dev,
		HD60PRO_BAR0_IRQ_STATUS,
		&snapshot->irq_status);
	if (ret)
		return ret;

	return sc0710_hd60pro_read_bar0(
		dev,
		HD60PRO_BAR0_IRQ_TAG,
		&snapshot->irq_tag);
}

static int
sc0710_hd60pro_registers_show(struct seq_file *s, void *unused)
{
	struct sc0710_dev *dev = s->private;
	unsigned int i;

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

static int
sc0710_hd60pro_mailbox_snapshot_show(struct seq_file *s, void *unused)
{
	struct sc0710_dev *dev = s->private;
	struct hd60pro_mailbox_snapshot snapshot;
	int ret;

	ret = sc0710_hd60pro_take_mailbox_snapshot(dev, &snapshot);
	if (ret)
		return ret;

	seq_printf(s, "pci_command=0x%04x\n",
		   snapshot.pci_command);
	seq_printf(s, "memory_space=%u\n",
		   !!(snapshot.pci_command & PCI_COMMAND_MEMORY));
	seq_printf(s, "bus_master=%u\n",
		   !!(snapshot.pci_command & PCI_COMMAND_MASTER));

	seq_printf(s, "mailbox_status=0x%08x\n",
		   snapshot.mailbox_status);
	seq_printf(s, "mailbox_complete=%u\n",
		   !!(snapshot.mailbox_status &
		      HD60PRO_MAILBOX_STATUS_COMPLETE));

	seq_printf(s, "irq_status=0x%08x\n",
		   snapshot.irq_status);
	seq_printf(s, "irq_mailbox_complete=%u\n",
		   !!(snapshot.irq_status &
		      HD60PRO_IRQ_STATUS_MAILBOX_COMPLETE));

	seq_printf(s, "irq_tag=0x%08x\n",
		   snapshot.irq_tag);
	seq_printf(s, "irq_tag_index=%u\n",
		   snapshot.irq_tag &
		   HD60PRO_IRQ_TAG_INDEX_MASK);

	return 0;
}

static int
sc0710_hd60pro_mailbox_snapshot_open(struct inode *inode,
				     struct file *file)
{
	return single_open(file,
			   sc0710_hd60pro_mailbox_snapshot_show,
			   inode->i_private);
}

static const struct file_operations
sc0710_hd60pro_mailbox_snapshot_fops = {
	.owner		= THIS_MODULE,
	.open		= sc0710_hd60pro_mailbox_snapshot_open,
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

	ret = sc0710_hd60pro_read_pci_command(pci_dev, &command);
	if (ret)
		return ret;

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
	seq_puts(s, "mailbox_protocol=reverse-engineered\n");
	seq_puts(s, "mailbox_writes=disabled\n");

	seq_printf(s, "signal_hdmi_hpd=%u\n",
		   HD60PRO_SIGNAL_HDMI_HPD);
	seq_printf(s, "signal_frontend_reset_n=%u\n",
		   HD60PRO_SIGNAL_FRONTEND_RESET_N);
	seq_printf(s, "video_frontend_i2c_addr_8bit=0x%02x\n",
		   HD60PRO_I2C_VIDEO_FRONTEND_ADDR_8BIT);
	seq_printf(s, "video_frontend_i2c_addr_7bit=0x%02x\n",
		   HD60PRO_I2C_VIDEO_FRONTEND_ADDR_7BIT);

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

	entry = debugfs_create_file(
				"mailbox_snapshot",
				0444,
				dev->hd60pro_debugfs_dir,
				dev,
				&sc0710_hd60pro_mailbox_snapshot_fops);
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
	pr_info("%s: whitelist MMIO reads enabled; "
		"bus mastering, IRQ, DMA, I2C and media nodes disabled\n",
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