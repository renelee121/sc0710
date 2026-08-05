// SPDX-License-Identifier: GPL-2.0

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "sc0710.h"
#include "sc0710-hd60pro.h"

#define HD60PRO_MAILBOX_POLL_COUNT		50U
#define HD60PRO_MAILBOX_POLL_MIN_US		1000U
#define HD60PRO_MAILBOX_POLL_MAX_US		1500U

static bool hd60pro_experimental_mailbox;
module_param_named(hd60pro_experimental_mailbox,
		   hd60pro_experimental_mailbox, bool, 0400);
MODULE_PARM_DESC(hd60pro_experimental_mailbox,
		 "Allow one manual HD60 Pro mailbox experiment per probe");

struct sc0710_hd60pro_reg {
	u8 bar;
	u32 offset;
	const char *name;
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
sc0710_hd60pro_write_bar0(struct sc0710_dev *dev, u32 offset, u32 value)
{
	resource_size_t size;

	if (!dev || !dev->pci || !dev->lmmio[0])
		return -ENODEV;

	switch (offset) {
	case HD60PRO_BAR0_MAILBOX_TRIGGER:
	case HD60PRO_BAR0_MAILBOX_OPCODE:
	case HD60PRO_BAR0_MAILBOX_WORD2:
	case HD60PRO_BAR0_MAILBOX_RESPONSE0:
	case HD60PRO_BAR0_MAILBOX_STATUS:
		break;
	default:
		return -EPERM;
	}

	size = pci_resource_len(dev->pci, 0);
	if (size < sizeof(value) || offset > size - sizeof(value))
		return -ERANGE;

	writel(value, (u8 __iomem *)dev->lmmio[0] + offset);
	return 0;
}

static int
sc0710_hd60pro_take_mailbox_snapshot(
	struct sc0710_dev *dev,
	struct sc0710_hd60pro_mailbox_snapshot *snapshot)
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

/*
 * Serialise every debugfs path that touches PCI configuration or MMIO with
 * the experimental mailbox operations. The debugfs core protects the file
 * operation's private data lifetime; this lock additionally guarantees that
 * observational reads cannot sample a partially-written mailbox command.
 *
 * Return with mailbox_lock held on success.
 */
static int sc0710_hd60pro_debugfs_hw_lock(struct sc0710_dev *dev)
{
	if (!dev)
		return -ENODEV;

	mutex_lock(&dev->hd60pro_state.mailbox_lock);

	if (READ_ONCE(dev->disconnected) || !dev->pci) {
		mutex_unlock(&dev->hd60pro_state.mailbox_lock);
		return -ENODEV;
	}

	return 0;
}

static void sc0710_hd60pro_debugfs_hw_unlock(struct sc0710_dev *dev)
{
	mutex_unlock(&dev->hd60pro_state.mailbox_lock);
}

static int
sc0710_hd60pro_registers_show(struct seq_file *s, void *unused)
{
	struct sc0710_dev *dev = s->private;
	unsigned int i;
	int ret;

	ret = sc0710_hd60pro_debugfs_hw_lock(dev);
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(hd60pro_readable_regs); i++) {
		const struct sc0710_hd60pro_reg *reg;
		u32 value;
		int read_ret;

		reg = &hd60pro_readable_regs[i];

		read_ret = sc0710_hd60pro_read_reg(dev, reg, &value);
		if (read_ret) {
			seq_printf(s,
				   "BAR%u[0x%08x] %-24s error=%d\n",
				   reg->bar,
				   reg->offset,
				   reg->name,
				   read_ret);
			continue;
		}

		seq_printf(s,
			   "BAR%u[0x%08x] %-24s = 0x%08x\n",
			   reg->bar,
			   reg->offset,
			   reg->name,
			   value);
	}

	sc0710_hd60pro_debugfs_hw_unlock(dev);
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
	struct sc0710_hd60pro_mailbox_snapshot snapshot;
	int ret;

	ret = sc0710_hd60pro_debugfs_hw_lock(dev);
	if (ret)
		return ret;

	ret = sc0710_hd60pro_take_mailbox_snapshot(dev, &snapshot);
	if (ret) {
		sc0710_hd60pro_debugfs_hw_unlock(dev);
		return ret;
	}

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

	sc0710_hd60pro_debugfs_hw_unlock(dev);
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

static void
sc0710_hd60pro_reset_experiment_result(struct sc0710_hd60pro_state *state)
{
	state->in_progress = false;
	state->completed = false;
	state->signal_value_valid = false;
	state->late_completion = false;
	state->last_error = 0;
	state->signal_index = HD60PRO_SIGNAL_HDMI_HPD;
	state->signal_value = 0;
	state->polls = 0;
	state->last_poll_status = 0;
	state->response = 0;
	state->elapsed_ns = 0;
	memset(&state->before, 0, sizeof(state->before));
	memset(&state->after, 0, sizeof(state->after));
}

static void
sc0710_hd60pro_reset_clear_result(struct sc0710_hd60pro_state *state)
{
	state->clear_in_progress = false;
	state->clear_completed = false;
	state->clear_last_error = 0;
	state->clear_elapsed_ns = 0;
	memset(&state->clear_before, 0, sizeof(state->clear_before));
	memset(&state->clear_after, 0, sizeof(state->clear_after));
}

static int
sc0710_hd60pro_validate_experiment(struct sc0710_dev *dev,
				   struct sc0710_hd60pro_state *state)
{
	int ret;

	if (!READ_ONCE(hd60pro_experimental_mailbox))
		return -EPERM;

	if (!dev || !dev->pci || !state || READ_ONCE(dev->disconnected))
		return -ENODEV;

	if (dev->board != SC0710_BOARD_ELGATO_HD60_PRO ||
	    dev->hw_ops != &sc0710_hd60pro_ops ||
	    !dev->observational_only ||
	    dev->pci->vendor != 0x12ab || dev->pci->device != 0x0380 ||
	    dev->pci->subsystem_vendor != 0x1cfa ||
	    dev->pci->subsystem_device != 0x0006)
		return -EPERM;

	if (dev->irq_requested || dev->kthread_dma || dev->kthread_hdmi)
		return -EBUSY;

	if (state->attempt_consumed)
		return -EALREADY;

	if (state->clear_attempt_consumed)
		return -EBUSY;

	ret = sc0710_hd60pro_take_mailbox_snapshot(dev, &state->before);
	if (ret)
		return ret;

	state->after = state->before;
	state->last_poll_status = state->before.mailbox_status;

	if (!(state->before.pci_command & PCI_COMMAND_MEMORY))
		return -EIO;

	if (state->before.pci_command & PCI_COMMAND_MASTER) {
		pci_clear_master(dev->pci);
		return -EIO;
	}

	if (state->before.mailbox_status != 0 || state->before.irq_status != 0)
		return -EBUSY;

	return 0;
}

static int
sc0710_hd60pro_run_signal_read_experiment(struct sc0710_dev *dev)
{
	struct sc0710_hd60pro_state *state;
	u64 started_ns = 0;
	u32 status = 0;
	u32 response = 0;
	unsigned int i;
	int ret;
	int snapshot_ret;

	if (!dev)
		return -ENODEV;

	state = &dev->hd60pro_state;

	mutex_lock(&state->mailbox_lock);
	if (state->attempt_consumed) {
		ret = -EALREADY;
		goto out_preserve_result;
	}

	sc0710_hd60pro_reset_experiment_result(state);

	ret = sc0710_hd60pro_validate_experiment(dev, state);
	if (ret)
		goto out;

	/*
	 * Consume the only active attempt before the first MMIO write. A partial
	 * command, timeout or unexpected state must require an unload/reload.
	 */
	state->attempt_consumed = true;
	state->in_progress = true;
	started_ns = ktime_get_ns();

	ret = sc0710_hd60pro_write_bar0(dev,
					HD60PRO_BAR0_MAILBOX_STATUS, 0);
	if (ret)
		goto out_after_attempt;

	ret = sc0710_hd60pro_write_bar0(dev,
					HD60PRO_BAR0_MAILBOX_OPCODE,
					HD60PRO_CMD_SIGNAL_READ);
	if (ret)
		goto out_after_attempt;

	ret = sc0710_hd60pro_write_bar0(dev,
					HD60PRO_BAR0_MAILBOX_WORD2,
					BIT(HD60PRO_SIGNAL_HDMI_HPD));
	if (ret)
		goto out_after_attempt;

	/* request[3] is explicitly zero before it becomes response word 0. */
	ret = sc0710_hd60pro_write_bar0(dev,
					HD60PRO_BAR0_MAILBOX_RESPONSE0, 0);
	if (ret)
		goto out_after_attempt;

	ret = sc0710_hd60pro_write_bar0(dev,
					HD60PRO_BAR0_MAILBOX_TRIGGER,
					HD60PRO_MAILBOX_TRIGGER_VALUE);
	if (ret)
		goto out_after_attempt;

	/*
	 * writel() preserves MMIO write ordering. The first readl() also flushes
	 * posted writes before completion is evaluated.
	 */
	for (i = 0; i < HD60PRO_MAILBOX_POLL_COUNT; i++) {
		ret = sc0710_hd60pro_read_bar0(
			dev, HD60PRO_BAR0_MAILBOX_STATUS, &status);
		if (ret)
			goto out_after_attempt;

		state->polls = i + 1;
		state->last_poll_status = status;

		if (status & HD60PRO_MAILBOX_STATUS_COMPLETE) {
			ret = sc0710_hd60pro_read_bar0(
				dev, HD60PRO_BAR0_MAILBOX_RESPONSE0, &response);
			if (ret)
				goto out_after_attempt;

			state->completed = true;
			state->signal_value_valid = true;
			state->response = response;
			state->signal_value =
				!!(response & BIT(HD60PRO_SIGNAL_HDMI_HPD));
			ret = 0;
			goto out_after_attempt;
		}

		usleep_range(HD60PRO_MAILBOX_POLL_MIN_US,
			     HD60PRO_MAILBOX_POLL_MAX_US);
	}

	ret = -ETIMEDOUT;

out_after_attempt:
	state->elapsed_ns = ktime_get_ns() - started_ns;

	/* Read-only after-snapshot; no clear, ACK, retry or reset is attempted. */
	snapshot_ret = sc0710_hd60pro_take_mailbox_snapshot(dev, &state->after);
	if (snapshot_ret) {
		if (!ret)
			ret = snapshot_ret;
	} else {
		state->late_completion =
			!state->completed &&
			!!(state->after.mailbox_status &
			   HD60PRO_MAILBOX_STATUS_COMPLETE);

		if (!state->completed) {
			if (!sc0710_hd60pro_read_bar0(
				    dev, HD60PRO_BAR0_MAILBOX_RESPONSE0,
				    &response))
				state->response = response;
		}

		if (state->after.pci_command & PCI_COMMAND_MASTER) {
			pci_clear_master(dev->pci);
			ret = -EIO;
		}

		if (!ret && state->after.irq_status != 0)
			ret = -EIO;
	}

out:
	state->last_error = ret;
	state->in_progress = false;
out_preserve_result:
	mutex_unlock(&state->mailbox_lock);
	return ret;
}

static int
sc0710_hd60pro_validate_clear_experiment(
	struct sc0710_dev *dev,
	struct sc0710_hd60pro_state *state)
{
	int ret;

	if (!READ_ONCE(hd60pro_experimental_mailbox))
		return -EPERM;

	if (!dev || !dev->pci || !state || READ_ONCE(dev->disconnected))
		return -ENODEV;

	if (dev->board != SC0710_BOARD_ELGATO_HD60_PRO ||
	    dev->hw_ops != &sc0710_hd60pro_ops ||
	    !dev->observational_only ||
	    dev->pci->vendor != 0x12ab || dev->pci->device != 0x0380 ||
	    dev->pci->subsystem_vendor != 0x1cfa ||
	    dev->pci->subsystem_device != 0x0006)
		return -EPERM;

	if (dev->irq_requested || dev->kthread_dma || dev->kthread_hdmi)
		return -EBUSY;

	if (state->clear_attempt_consumed)
		return -EALREADY;

	if (state->attempt_consumed)
		return -EBUSY;

	ret = sc0710_hd60pro_take_mailbox_snapshot(
		dev, &state->clear_before);
	if (ret)
		return ret;

	state->clear_after = state->clear_before;

	if (!(state->clear_before.pci_command & PCI_COMMAND_MEMORY))
		return -EIO;

	if (state->clear_before.pci_command & PCI_COMMAND_MASTER) {
		pci_clear_master(dev->pci);
		return -EIO;
	}

	if (state->clear_before.irq_status != 0)
		return -EBUSY;

	/* Clear only the exact stale completion state observed in G1-A. */
	if (state->clear_before.mailbox_status !=
	    HD60PRO_MAILBOX_STATUS_COMPLETE)
		return -EBUSY;

	return 0;
}

static int
sc0710_hd60pro_run_clear_experiment(struct sc0710_dev *dev)
{
	struct sc0710_hd60pro_state *state;
	u64 started_ns = 0;
	u32 status = 0;
	int ret;
	int snapshot_ret;

	if (!dev)
		return -ENODEV;

	state = &dev->hd60pro_state;

	mutex_lock(&state->mailbox_lock);
	if (state->clear_attempt_consumed) {
		ret = -EALREADY;
		goto out_preserve_result;
	}

	sc0710_hd60pro_reset_clear_result(state);

	ret = sc0710_hd60pro_validate_clear_experiment(dev, state);
	if (ret)
		goto out;

	/*
	 * Consume the sole active action for this probe before the write. The
	 * Windows polling path performs the same write before every command.
	 */
	state->clear_attempt_consumed = true;
	state->clear_in_progress = true;
	started_ns = ktime_get_ns();

	ret = sc0710_hd60pro_write_bar0(
		dev, HD60PRO_BAR0_MAILBOX_STATUS, 0);
	if (ret)
		goto out_after_attempt;

	/* readl() flushes the posted clear and verifies its visible result. */
	ret = sc0710_hd60pro_read_bar0(
		dev, HD60PRO_BAR0_MAILBOX_STATUS, &status);
	if (ret)
		goto out_after_attempt;

	if (status != 0) {
		ret = -EIO;
		goto out_after_attempt;
	}

	ret = 0;

out_after_attempt:
	state->clear_elapsed_ns = ktime_get_ns() - started_ns;

	snapshot_ret = sc0710_hd60pro_take_mailbox_snapshot(
		dev, &state->clear_after);
	if (snapshot_ret) {
		if (!ret)
			ret = snapshot_ret;
	} else {
		if (state->clear_after.pci_command & PCI_COMMAND_MASTER) {
			pci_clear_master(dev->pci);
			ret = -EIO;
		}

		if (!ret && (state->clear_after.mailbox_status != 0 ||
		             state->clear_after.irq_status != 0))
			ret = -EIO;
	}

	if (!ret)
		state->clear_completed = true;

out:
	state->clear_last_error = ret;
	state->clear_in_progress = false;
out_preserve_result:
	mutex_unlock(&state->mailbox_lock);
	return ret;
}

static int
sc0710_hd60pro_experimental_mailbox_clear_show(
	struct seq_file *s, void *unused)
{
	struct sc0710_dev *dev = s->private;
	struct sc0710_hd60pro_state *state = &dev->hd60pro_state;

	mutex_lock(&state->mailbox_lock);

	seq_printf(s, "enabled=%u\n",
		   READ_ONCE(hd60pro_experimental_mailbox));
	seq_printf(s, "clear_attempt_consumed=%u\n",
		   state->clear_attempt_consumed);
	seq_printf(s, "clear_in_progress=%u\n",
		   state->clear_in_progress);
	seq_printf(s, "clear_completed=%u\n",
		   state->clear_completed);
	seq_printf(s, "clear_last_error=%d\n",
		   state->clear_last_error);
	seq_printf(s, "clear_elapsed_us=%llu\n",
		   (unsigned long long)(state->clear_elapsed_ns / 1000));
	seq_printf(s, "pci_command_before=0x%04x\n",
		   state->clear_before.pci_command);
	seq_printf(s, "mailbox_status_before=0x%08x\n",
		   state->clear_before.mailbox_status);
	seq_printf(s, "irq_status_before=0x%08x\n",
		   state->clear_before.irq_status);
	seq_printf(s, "irq_tag_before=0x%08x\n",
		   state->clear_before.irq_tag);
	seq_printf(s, "pci_command_after=0x%04x\n",
		   state->clear_after.pci_command);
	seq_printf(s, "mailbox_status_after=0x%08x\n",
		   state->clear_after.mailbox_status);
	seq_printf(s, "irq_status_after=0x%08x\n",
		   state->clear_after.irq_status);
	seq_printf(s, "irq_tag_after=0x%08x\n",
		   state->clear_after.irq_tag);

	mutex_unlock(&state->mailbox_lock);
	return 0;
}

static int
sc0710_hd60pro_experimental_mailbox_clear_open(
	struct inode *inode, struct file *file)
{
	return single_open(file,
			   sc0710_hd60pro_experimental_mailbox_clear_show,
			   inode->i_private);
}

static ssize_t
sc0710_hd60pro_experimental_mailbox_clear_write(
	struct file *file, const char __user *user_buf,
	size_t count, loff_t *ppos)
{
	struct seq_file *seq = file->private_data;
	struct sc0710_dev *dev = seq->private;
	char buf[8];
	char *command;
	int ret;

	if (count == 0 || count >= sizeof(buf))
		return -EINVAL;

	if (copy_from_user(buf, user_buf, count))
		return -EFAULT;

	buf[count] = '\0';
	command = strim(buf);
	if (strcmp(command, "1") != 0)
		return -EINVAL;

	ret = sc0710_hd60pro_run_clear_experiment(dev);
	if (ret)
		return ret;

	return count;
}

static const struct file_operations
sc0710_hd60pro_experimental_mailbox_clear_fops = {
	.owner		= THIS_MODULE,
	.open		= sc0710_hd60pro_experimental_mailbox_clear_open,
	.read		= seq_read,
	.write		= sc0710_hd60pro_experimental_mailbox_clear_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int
sc0710_hd60pro_experimental_signal_read_show(struct seq_file *s, void *unused)
{
	struct sc0710_dev *dev = s->private;
	struct sc0710_hd60pro_state *state = &dev->hd60pro_state;

	mutex_lock(&state->mailbox_lock);

	seq_printf(s, "enabled=%u\n",
		   READ_ONCE(hd60pro_experimental_mailbox));
	seq_printf(s, "attempt_consumed=%u\n", state->attempt_consumed);
	seq_printf(s, "in_progress=%u\n", state->in_progress);
	seq_printf(s, "completed=%u\n", state->completed);
	seq_printf(s, "late_completion=%u\n", state->late_completion);
	seq_printf(s, "last_error=%d\n", state->last_error);
	seq_printf(s, "signal_index=%u\n", state->signal_index);
	seq_puts(s, "signal_name=HDMI_HPD\n");
	seq_printf(s, "signal_value_valid=%u\n", state->signal_value_valid);
	seq_printf(s, "signal_value=%u\n", state->signal_value);
	seq_printf(s, "polls=%u\n", state->polls);
	seq_printf(s, "elapsed_us=%llu\n",
		   (unsigned long long)(state->elapsed_ns / 1000));
	seq_printf(s, "last_poll_status=0x%08x\n",
		   state->last_poll_status);
	seq_printf(s, "response=0x%08x\n", state->response);
	seq_printf(s, "pci_command_before=0x%04x\n",
		   state->before.pci_command);
	seq_printf(s, "mailbox_status_before=0x%08x\n",
		   state->before.mailbox_status);
	seq_printf(s, "irq_status_before=0x%08x\n",
		   state->before.irq_status);
	seq_printf(s, "irq_tag_before=0x%08x\n",
		   state->before.irq_tag);
	seq_printf(s, "pci_command_after=0x%04x\n",
		   state->after.pci_command);
	seq_printf(s, "mailbox_status_after=0x%08x\n",
		   state->after.mailbox_status);
	seq_printf(s, "irq_status_after=0x%08x\n",
		   state->after.irq_status);
	seq_printf(s, "irq_tag_after=0x%08x\n",
		   state->after.irq_tag);

	mutex_unlock(&state->mailbox_lock);
	return 0;
}

static int
sc0710_hd60pro_experimental_signal_read_open(struct inode *inode,
					     struct file *file)
{
	return single_open(file,
			   sc0710_hd60pro_experimental_signal_read_show,
			   inode->i_private);
}

static ssize_t
sc0710_hd60pro_experimental_signal_read_write(struct file *file,
					      const char __user *user_buf,
					      size_t count, loff_t *ppos)
{
	struct seq_file *seq = file->private_data;
	struct sc0710_dev *dev = seq->private;
	char buf[8];
	char *command;
	int ret;

	if (count == 0 || count >= sizeof(buf))
		return -EINVAL;

	if (copy_from_user(buf, user_buf, count))
		return -EFAULT;

	buf[count] = '\0';
	command = strim(buf);
	if (strcmp(command, "1") != 0)
		return -EINVAL;

	ret = sc0710_hd60pro_run_signal_read_experiment(dev);
	if (ret)
		return ret;

	return count;
}

static const struct file_operations
sc0710_hd60pro_experimental_signal_read_fops = {
	.owner		= THIS_MODULE,
	.open		= sc0710_hd60pro_experimental_signal_read_open,
	.read		= seq_read,
	.write		= sc0710_hd60pro_experimental_signal_read_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int sc0710_hd60pro_status_show(struct seq_file *s, void *unused)
{
	struct sc0710_dev *dev = s->private;
	struct pci_dev *pci_dev;
	u16 command;
	int ret;

	ret = sc0710_hd60pro_debugfs_hw_lock(dev);
	if (ret)
		return ret;

	pci_dev = dev->pci;

	ret = sc0710_hd60pro_read_pci_command(pci_dev, &command);
	if (ret) {
		sc0710_hd60pro_debugfs_hw_unlock(dev);
		return ret;
	}

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

	if (READ_ONCE(hd60pro_experimental_mailbox)) {
		seq_puts(s, "mode=observational-with-manual-mailbox-opt-in\n");
		seq_puts(s, "mmio_writes=experimental-manual-only\n");
		seq_puts(s, "mailbox_writes=experimental-manual-only\n");
	} else {
		seq_puts(s, "mode=observational-only\n");
		seq_puts(s, "mmio_writes=disabled\n");
		seq_puts(s, "mailbox_writes=disabled\n");
	}
	seq_puts(s, "mmio_reads=whitelist-only\n");
	seq_puts(s, "mailbox_protocol=reverse-engineered\n");
	seq_printf(s, "experimental_mailbox_opt_in=%u\n",
		   READ_ONCE(hd60pro_experimental_mailbox));

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

	sc0710_hd60pro_debugfs_hw_unlock(dev);
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
	 * Clear bus mastering before any allocation or early-return path.
	 */
	pci_clear_master(pci_dev);

	mutex_init(&dev->hd60pro_state.mailbox_lock);
	sc0710_hd60pro_reset_experiment_result(&dev->hd60pro_state);
	sc0710_hd60pro_reset_clear_result(&dev->hd60pro_state);

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

	entry = debugfs_create_file(
				"experimental_mailbox_clear",
				0600,
				dev->hd60pro_debugfs_dir,
				dev,
				&sc0710_hd60pro_experimental_mailbox_clear_fops);
	if (IS_ERR_OR_NULL(entry)) {
		ret = entry ? PTR_ERR(entry) : -ENOMEM;
		goto err_debugfs;
	}

	entry = debugfs_create_file(
				"experimental_signal_read",
				0600,
				dev->hd60pro_debugfs_dir,
				dev,
				&sc0710_hd60pro_experimental_signal_read_fops);
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
	pr_info("%s: experimental mailbox opt-in is %s\n",
		dev->name,
		READ_ONCE(hd60pro_experimental_mailbox) ? "enabled" : "disabled");

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

static int
sc0710_hd60pro_capture_unsupported(struct sc0710_dev *dev)
{
	if (!dev || !dev->pci)
		return -ENODEV;

	/* The passive backend must never grant the endpoint DMA ownership. */
	pci_clear_master(dev->pci);

	return -EOPNOTSUPP;
}

static void
sc0710_hd60pro_capture_stop(struct sc0710_dev *dev)
{
	if (!dev || !dev->pci)
		return;

	/* Capture cannot start in this backend; preserve the safe state. */
	pci_clear_master(dev->pci);
}

static int
sc0710_hd60pro_capture_service(struct sc0710_dev *dev)
{
	if (!dev || !dev->pci)
		return -ENODEV;

	if (!dev->observational_only)
		return -EPERM;

	pci_clear_master(dev->pci);

	return 0;
}

const struct sc0710_hw_ops sc0710_hd60pro_ops = {
	.init			= sc0710_hd60pro_probe,
	.fini			= sc0710_hd60pro_remove,
	.capture_prepare	= sc0710_hd60pro_capture_unsupported,
	.capture_start		= sc0710_hd60pro_capture_unsupported,
	.capture_stop		= sc0710_hd60pro_capture_stop,
	.capture_service	= sc0710_hd60pro_capture_service,
};