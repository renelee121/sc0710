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
#define HD60PRO_BOOTSTRAP_SETTLE_MS		256U

#define HD60PRO_FW_VERSION_IRQ_TIMEOUT_MS	5000U
#define HD60PRO_FW_VERSION_SETTLE_MS		100U
#define HD60PRO_FW_VERSION_STATUS_MAGIC		0xAAAAAAAAU
#define HD60PRO_FW_VERSION_EXPECTED_MAJOR	1U
#define HD60PRO_FW_VERSION_EXPECTED_MINOR	11U
#define HD60PRO_I2C_EXPERIMENT_REG_04             0x04U
#define HD60PRO_I2C_EXPERIMENT_REG_11             0x11U
#define HD60PRO_I2C_EXPERIMENT_REG_19             0x19U
#define HD60PRO_I2C_EXPERIMENT_REG_3D             0x3dU
#define HD60PRO_I2C_EXPERIMENT_REG_73             0x73U
#define HD60PRO_I2C_EXPERIMENT_DEFAULT_REG        HD60PRO_I2C_EXPERIMENT_REG_11

static bool hd60pro_experimental_mailbox;
module_param_named(hd60pro_experimental_mailbox,
		   hd60pro_experimental_mailbox, bool, 0400);
MODULE_PARM_DESC(hd60pro_experimental_mailbox,
		 "Allow one manual HD60 Pro mailbox experiment per probe");

static bool hd60pro_active_control;
module_param_named(hd60pro_active_control,
		   hd60pro_active_control, bool, 0400);
MODULE_PARM_DESC(hd60pro_active_control,
		 "Enable one-shot HD60 Pro P1 bootstrap/version control; DMA and IRQ remain disabled");


struct sc0710_hd60pro_reg {
	u8 bar;
	u32 offset;
	const char *name;
};

struct sc0710_hd60pro_mailbox_request {
	u32 command;
	u32 word2;
	u32 word3;
	u32 response_offset;
};

struct sc0710_hd60pro_mailbox_result {
        bool completed;
        bool late_completion;
        bool after_valid;

        u32 response;

        u32 polls;
        u32 last_poll_status;

        u64 elapsed_ns;

        struct sc0710_hd60pro_mailbox_snapshot after;
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
	case HD60PRO_BAR0_MAILBOX_RESPONSE1:
	case HD60PRO_BAR0_MAILBOX_WORD5:
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

/*
 * Specialized Windows rearm primitives.
 *
 * Reachable only through the guarded P1 rearm paths.
 * Keep them narrower than the generic mailbox writer so the reconstructed
 * IRQ rearm sequence cannot grow into an unrestricted MMIO write surface.
 */
static int __maybe_unused
sc0710_hd60pro_clear_irq_status(struct sc0710_dev *dev)
{
	resource_size_t size;

	if (!dev || !dev->pci || !dev->lmmio[0])
		return -ENODEV;

	size = pci_resource_len(dev->pci, 0);
	if (size < sizeof(u32) ||
	    HD60PRO_BAR0_IRQ_STATUS > size - sizeof(u32))
		return -ERANGE;

	writel(0, (u8 __iomem *)dev->lmmio[0] +
	       HD60PRO_BAR0_IRQ_STATUS);

	return 0;
}

static int __maybe_unused
sc0710_hd60pro_ack_bar5_irq(struct sc0710_dev *dev)
{
	resource_size_t size;

	if (!dev || !dev->pci || !dev->lmmio[1])
		return -ENODEV;

	size = pci_resource_len(dev->pci, 5);
	if (size < sizeof(u32) ||
	    HD60PRO_BAR5_IRQ_ACK > size - sizeof(u32))
		return -ERANGE;

	writel(HD60PRO_BAR5_IRQ_ACK_VALUE,
	       (u8 __iomem *)dev->lmmio[1] + HD60PRO_BAR5_IRQ_ACK);

	return 0;
}

/*
 * Validate that the Windows bootstrap BAR5 setup can be represented safely
 * on this PCI assignment.
 *
 * This helper is read-only: it inspects PCI resource metadata and mappings
 * only. Keep these checks in preflight so deterministic platform/resource
 * incompatibilities cannot consume the bootstrap one-shot or follow any
 * MMIO write.
 */
static int
sc0710_hd60pro_validate_bootstrap_bar5_setup(struct sc0710_dev *dev)
{
	resource_size_t bar0_phys;
	resource_size_t bar0_size;
	resource_size_t bar5_size;

	if (!dev || !dev->pci || !dev->lmmio[1])
		return -ENODEV;

	bar0_phys = pci_resource_start(dev->pci, 0);
	bar0_size = pci_resource_len(dev->pci, 0);
	bar5_size = pci_resource_len(dev->pci, 5);

	if (bar0_size <= HD60PRO_BOOTSTRAP_BAR0_PHYS_PLUS5F)
		return -ERANGE;

	if (bar5_size < sizeof(u32) ||
	    HD60PRO_BAR5_BOOTSTRAP_REG38 > bar5_size - sizeof(u32))
		return -ERANGE;

	if (bar0_phys >
	    0xffffffffULL - HD60PRO_BOOTSTRAP_BAR0_PHYS_PLUS5F)
		return -ERANGE;

	return 0;
}

/*
 * Program the two BAR5 DWORDs written by the Windows MZ0380 bootstrap path.
 *
 * FUN_14028d254 stores the physical start of Windows MEMORY #0 at ctx+0x80.
 * FUN_140278bb0 then writes:
 *
 *   BAR5+0x30 <- low32(ctx+0x80 + 0x04)
 *   BAR5+0x38 <- low32(ctx+0x80 + 0x5f)
 *
 * For the HD60 Pro, Windows MEMORY #0 is PCI BAR0 and MEMORY #1 is PCI
 * BAR5. Derive the values from the current PCI BAR0 assignment rather than
 * hard-coding an observed physical address.
 *
 * Windows performs DWORD writes, so fail closed if BAR0+0x5f cannot be
 * represented in 32 bits. Do not silently truncate a >4 GiB resource.
 *
 * Caller must hold state->mailbox_lock.
 * Invoked only as part of the guarded one-shot bootstrap sequence.
 */
static int __maybe_unused
sc0710_hd60pro_program_bootstrap_bar5_locked(struct sc0710_dev *dev)
{
	resource_size_t bar0_phys;
	int ret;

	ret = sc0710_hd60pro_validate_bootstrap_bar5_setup(dev);
	if (ret)
		return ret;

	bar0_phys = pci_resource_start(dev->pci, 0);

	writel((u32)(bar0_phys + HD60PRO_BOOTSTRAP_BAR0_PHYS_PLUS4),
	       (u8 __iomem *)dev->lmmio[1] +
	       HD60PRO_BAR5_BOOTSTRAP_REG30);

	writel((u32)(bar0_phys + HD60PRO_BOOTSTRAP_BAR0_PHYS_PLUS5F),
	       (u8 __iomem *)dev->lmmio[1] +
	       HD60PRO_BAR5_BOOTSTRAP_REG38);

	return 0;
}

/*
 * Reconstructed Windows IRQ rearm sequence.
 *
 * Caller must hold state->mailbox_lock.
 * Invoked by guarded bootstrap PRE/POST rearm and by the firmware-version
 * completion path after a real mailbox-complete indication.
 */
static int __maybe_unused
sc0710_hd60pro_rearm_irq_locked(struct sc0710_dev *dev)
{
	int ret;

	ret = sc0710_hd60pro_ack_bar5_irq(dev);
	if (ret)
		return ret;

	ret = sc0710_hd60pro_clear_irq_status(dev);
	if (ret)
		return ret;

	return sc0710_hd60pro_write_bar0(
		dev,
		HD60PRO_BAR0_MAILBOX_TRIGGER,
		HD60PRO_IRQ_ACK_TRIGGER_VALUE);
}

/*
 * Reconstructed Windows MZ0380 bootstrap request.
 *
 * The Windows helper writes only the opcode to BAR0+0x04 and then the
 * trigger to BAR0+0x00. There is no BAR0+0x08 or BAR0+0x0c payload.
 *
 * Caller must hold state->mailbox_lock.
 * Invoked only after guarded bootstrap admission and PRE rearm.
 */
static int __maybe_unused
sc0710_hd60pro_bootstrap_request_locked(struct sc0710_dev *dev)
{
	int ret;

	ret = sc0710_hd60pro_write_bar0(
		dev,
		HD60PRO_BAR0_MAILBOX_OPCODE,
		HD60PRO_CMD_BOOTSTRAP);
	if (ret)
		return ret;

	return sc0710_hd60pro_write_bar0(
		dev,
		HD60PRO_BAR0_MAILBOX_TRIGGER,
		HD60PRO_MAILBOX_TRIGGER_VALUE);
}


/*
 * Reconstructed Windows MZ0380 firmware-version request.
 *
 * Exact request footprint:
 *
 *   BAR0+0x04 <- 0x0000000a
 *   BAR0+0x08 <- 0x00000000
 *   BAR0+0x0c <- 0x00000000
 *   BAR0+0x00 <- 0x00000800
 *
 * This primitive deliberately does not clear BAR0+0x2c, wait for
 * completion, ACK/rearm interrupt state, retry the request, update
 * firmware, or modify PCI bus-master state.
 *
 * Caller must hold state->mailbox_lock.
 * Runtime use is restricted to the complete P1 one-shot path.
 */
static int __maybe_unused
sc0710_hd60pro_firmware_version_request_locked(struct sc0710_dev *dev)
{
	int ret;

	ret = sc0710_hd60pro_write_bar0(
		dev,
		HD60PRO_BAR0_MAILBOX_OPCODE,
		HD60PRO_CMD_FIRMWARE_VERSION);
	if (ret)
		return ret;

	ret = sc0710_hd60pro_write_bar0(
		dev,
		HD60PRO_BAR0_MAILBOX_WORD2,
		0);
	if (ret)
		return ret;

	ret = sc0710_hd60pro_write_bar0(
		dev,
		HD60PRO_BAR0_MAILBOX_RESPONSE0,
		0);
	if (ret)
		return ret;

	return sc0710_hd60pro_write_bar0(
		dev,
		HD60PRO_BAR0_MAILBOX_TRIGGER,
		HD60PRO_MAILBOX_TRIGGER_VALUE);
}


/*
 * Wait for the physical mailbox-complete indication used by the Windows
 * semaphore path for MZ0380_SendCommand(..., mode=1, ...).
 *
 * Windows waits on ctx+0x69d0. That semaphore is released by the DPC after
 * observing BAR0+0x30 bit 11. Linux does not have the HD60 Pro IRQ path yet,
 * so this helper observes the same physical completion source using
 * bounded read-only polling.
 *
 * This helper deliberately does not ACK/rearm the interrupt, inspect
 * BAR0+0x2c, retry the command, or modify PCI state.
 *
 * Caller must hold state->mailbox_lock.
 * Runtime use is restricted to the complete P1 one-shot path.
 */
static int __maybe_unused
sc0710_hd60pro_wait_firmware_version_irq_locked(struct sc0710_dev *dev)
{
	u64 deadline_ns;
	u32 irq_status;
	int ret;

	if (!dev || !dev->pci || !dev->lmmio[0])
		return -ENODEV;

	deadline_ns = ktime_get_ns() +
		(u64)HD60PRO_FW_VERSION_IRQ_TIMEOUT_MS * NSEC_PER_MSEC;

	do {
		ret = sc0710_hd60pro_read_bar0(
			dev,
			HD60PRO_BAR0_IRQ_STATUS,
			&irq_status);
		if (ret)
			return ret;

		if (irq_status & HD60PRO_IRQ_STATUS_MAILBOX_COMPLETE)
			return 0;

		usleep_range(HD60PRO_MAILBOX_POLL_MIN_US,
			     HD60PRO_MAILBOX_POLL_MAX_US);
	} while (ktime_get_ns() < deadline_ns);

	return -ETIMEDOUT;
}

/*
 * Validate and collect the MZ0380 firmware-version response after mailbox
 * completion has already been observed and the IRQ state has been rearmed
 * by the P1 orchestrator.
 *
 * Windows requires BAR0+0x2c == 0xAAAAAAAA, waits 100 ms, then reads the
 * two version DWORDs from BAR0+0x08 and BAR0+0x0c.
 *
 * No retries, ACK/rearm, firmware update, DMA, START, or PCI Bus Mastering
 * are performed here.
 *
 * Caller must hold state->mailbox_lock.
 * Runtime use is restricted to the complete P1 one-shot path.
 */
static int __maybe_unused
sc0710_hd60pro_read_firmware_version_locked(
	struct sc0710_dev *dev,
	u32 *version_major,
	u32 *version_minor)
{
	u32 status;
	int ret;

	if (!dev || !dev->pci || !dev->lmmio[0])
		return -ENODEV;

	if (!version_major || !version_minor)
		return -EINVAL;

	ret = sc0710_hd60pro_read_bar0(
		dev,
		HD60PRO_BAR0_MAILBOX_STATUS,
		&status);
	if (ret)
		return ret;

	if (status != HD60PRO_FW_VERSION_STATUS_MAGIC)
		return -EIO;

	msleep(HD60PRO_FW_VERSION_SETTLE_MS);

	ret = sc0710_hd60pro_read_bar0(
		dev,
		HD60PRO_BAR0_MAILBOX_WORD2,
		version_major);
	if (ret)
		return ret;

	return sc0710_hd60pro_read_bar0(
		dev,
		HD60PRO_BAR0_MAILBOX_RESPONSE0,
		version_minor);
}


/*
 * P1 firmware-version compatibility gate for the MZ0380.
 *
 * Expected version provenance:
 *
 *   MZ0380.FW.TXT from the Windows package containing the matching
 *   e60MZ0380.X64.SYS:
 *
 *       "01.11\n"
 *
 * Linux intentionally fails closed on a version mismatch and never
 * proceeds to the Windows firmware-download opcodes 0x0b/0x0c.
 *
 * Sequence:
 *
 *   - require PCI memory decode enabled and Bus Master disabled
 *   - require a clean BAR0 IRQ-status baseline
 *   - issue one opcode 0x0a request
 *   - observe BAR0+0x30 bit 11, bounded to 5 seconds
 *   - perform the reconstructed IRQ ACK/rearm
 *   - require BAR0+0x2c == 0xAAAAAAAA
 *   - wait 100 ms
 *   - read BAR0+0x08 / BAR0+0x0c
 *   - require version 1.11
 *   - verify Bus Master remains disabled
 *
 * No retries, firmware writes, DMA, START, or pci_set_master().
 *
 * Caller must hold state->mailbox_lock.
 * Runtime use is restricted to the complete P1 one-shot path.
 */
static int __maybe_unused
sc0710_hd60pro_firmware_version_gate_locked(
	struct sc0710_dev *dev,
	u32 *actual_major,
	u32 *actual_minor)
{
	u16 command;
	u32 irq_status;
	int pci_ret;
	int rearm_ret;
	int ret;

	if (!dev || !dev->pci || !dev->lmmio[0] || !dev->lmmio[1])
		return -ENODEV;

	if (!actual_major || !actual_minor)
		return -EINVAL;

	*actual_major = 0;
	*actual_minor = 0;

	ret = sc0710_hd60pro_read_pci_command(dev->pci, &command);
	if (ret)
		return ret;

	if (!(command & PCI_COMMAND_MEMORY))
		return -EIO;

	if (command & PCI_COMMAND_MASTER) {
		pci_clear_master(dev->pci);
		return -EIO;
	}

	/*
	 * Do not consume a stale mailbox completion from the preceding
	 * bootstrap. Admission here remains read-only.
	 */
	ret = sc0710_hd60pro_read_bar0(
		dev,
		HD60PRO_BAR0_IRQ_STATUS,
		&irq_status);
	if (ret)
		goto out_verify_pci;

	if (irq_status != 0) {
		ret = -EBUSY;
		goto out_verify_pci;
	}

	ret = sc0710_hd60pro_firmware_version_request_locked(dev);
	if (ret)
		goto out_verify_pci;

	ret = sc0710_hd60pro_wait_firmware_version_irq_locked(dev);
	if (ret)
		goto out_verify_pci;

	/*
	 * Rearm only after mailbox completion has actually been observed.
	 *
	 * In the reconstructed Windows mode=1 path, the ISR performs this
	 * sequence after a real interrupt and before the DPC releases the
	 * semaphore. A timeout in the waiter does not synthesize an ISR.
	 *
	 * Linux therefore fails stop on timeout/read failure instead of
	 * issuing BAR0/BAR5 cleanup writes against a command whose completion
	 * was never observed.
	 */
	rearm_ret = sc0710_hd60pro_rearm_irq_locked(dev);
	if (rearm_ret) {
		ret = rearm_ret;
		goto out_verify_pci;
	}

	ret = sc0710_hd60pro_read_firmware_version_locked(
		dev,
		actual_major,
		actual_minor);
	if (ret)
		goto out_verify_pci;

	if (*actual_major != HD60PRO_FW_VERSION_EXPECTED_MAJOR ||
	    *actual_minor != HD60PRO_FW_VERSION_EXPECTED_MINOR)
		ret = -EPROTO;

out_verify_pci:
	pci_ret = sc0710_hd60pro_read_pci_command(dev->pci, &command);
	if (pci_ret) {
		if (!ret)
			ret = pci_ret;
		return ret;
	}

	if (command & PCI_COMMAND_MASTER) {
		pci_clear_master(dev->pci);
		if (!ret)
			ret = -EIO;
	}

	if (!(command & PCI_COMMAND_MEMORY) && !ret)
		ret = -EIO;

	return ret;
}


/*
 * P2.1A: raw MZ0380 mode-0 target/register command primitive.
 *
 * Recovered Windows request shapes:
 *
 *   0x1a: [800, 1a, target, reg, 0]
 *   0x1b: [800, 1b, target, reg, value]
 *   0x1c: [800, 1c, target]
 *   0x1d: [800, 1d, target, reg, {1|8}, value]
 *
 * Mode 0 clears BAR0+0x2c, writes request dwords 1..N-1 starting
 * at BAR0+0x04, triggers BAR0+0x00 with 0x800, and polls
 * BAR0+0x2c bit 0.
 *
 * No retry, IRQ rearm, DMA programming, firmware update, capture
 * START or PCI bus-master enable occurs here.
 *
 * Caller must hold state->mailbox_lock.
 *
 * P2.1A is definition-only: no runtime caller is added in this step.
 */

/*
 * P2.1B paged-register scope.
 *
 * The FA:1C HD60 Pro path uses target 0x9c with page selections
 * 0x00, 0x01, 0x02 and 0x80.  Keep the Linux reconstruction closed
 * over only that proven set for now.
 */
#define HD60PRO_FRONTEND_PAGED_TARGET       0x9cU
#define HD60PRO_PAGED_PAGE_CACHE_INVALID    0xffU

static int __maybe_unused
sc0710_hd60pro_mode0_target_command_locked(
	struct sc0710_dev *dev,
	const u32 *request,
	unsigned int word_count,
	u32 *response)
{
	u32 status = 0;
	u32 offset;
	unsigned int i;
	int ret;

	if (!dev || !request)
		return -EINVAL;

	if (word_count < 3 || word_count > 6)
		return -EINVAL;

	if (request[0] != HD60PRO_MAILBOX_TRIGGER_VALUE)
		return -EINVAL;

	/*
	 * Keep the primitive closed over only the four request shapes
	 * recovered for P2.
	 */
	switch (request[1]) {
	case HD60PRO_CMD_I2C_READ_REG8:
		if (word_count != 5 || request[4] != 0 || !response)
			return -EINVAL;
		break;

	case HD60PRO_CMD_I2C_WRITE_REG8:
		if (word_count != 5 || response)
			return -EINVAL;
		break;

	case HD60PRO_CMD_TARGET_READ32:
		if (word_count != 3 || !response)
			return -EINVAL;
		break;

	case HD60PRO_CMD_TARGET_REG_EXTENDED:
		if (word_count != 6 || response ||
		    (request[4] != 1 && request[4] != 8))
			return -EINVAL;
		break;

	default:
		return -EOPNOTSUPP;
	}

	ret = sc0710_hd60pro_write_bar0(
		dev,
		HD60PRO_BAR0_MAILBOX_STATUS,
		0);
	if (ret)
		return ret;

	for (i = 1; i < word_count; i++) {
		offset = HD60PRO_BAR0_MAILBOX_OPCODE +
			 (i - 1) * sizeof(u32);

		ret = sc0710_hd60pro_write_bar0(
			dev,
			offset,
			request[i]);
		if (ret)
			return ret;
	}

	ret = sc0710_hd60pro_write_bar0(
		dev,
		HD60PRO_BAR0_MAILBOX_TRIGGER,
		HD60PRO_MAILBOX_TRIGGER_VALUE);
	if (ret)
		return ret;

	for (i = 0; i < HD60PRO_MAILBOX_POLL_COUNT; i++) {
		ret = sc0710_hd60pro_read_bar0(
			dev,
			HD60PRO_BAR0_MAILBOX_STATUS,
			&status);
		if (ret)
			return ret;

		if (status & HD60PRO_MAILBOX_STATUS_COMPLETE) {
			if (!response)
				return 0;

			return sc0710_hd60pro_read_bar0(
				dev,
				HD60PRO_BAR0_MAILBOX_RESPONSE1,
				response);
		}

		usleep_range(
			HD60PRO_MAILBOX_POLL_MIN_US,
			HD60PRO_MAILBOX_POLL_MAX_US);
	}

	return -ETIMEDOUT;
}


/*
 * FUN_1402777e4
 *
 * [800, 1a, target, reg & 0xff, 0]
 * success -> BAR0+0x10
 */
static int __maybe_unused
sc0710_hd60pro_target_reg_read_locked(
	struct sc0710_dev *dev,
	u8 target,
	u8 reg,
	u32 *value)
{
	const u32 request[] = {
		HD60PRO_MAILBOX_TRIGGER_VALUE,
		HD60PRO_CMD_I2C_READ_REG8,
		target,
		reg,
		0,
	};

	if (!value)
		return -EINVAL;

	return sc0710_hd60pro_mode0_target_command_locked(
		dev,
		request,
		ARRAY_SIZE(request),
		value);
}


/*
 * FUN_1402851cc
 *
 * [800, 1b, target, reg & 0xff, value]
 */
static int __maybe_unused
sc0710_hd60pro_target_reg_write_locked(
	struct sc0710_dev *dev,
	u8 target,
	u8 reg,
	u8 value)
{
	const u32 request[] = {
		HD60PRO_MAILBOX_TRIGGER_VALUE,
		HD60PRO_CMD_I2C_WRITE_REG8,
		target,
		reg,
		value,
	};

	return sc0710_hd60pro_mode0_target_command_locked(
		dev,
		request,
		ARRAY_SIZE(request),
		NULL);
}


/*
 * FUN_140277c78
 *
 * [800, 1c, target]
 * success -> BAR0+0x10
 */
static int __maybe_unused
sc0710_hd60pro_target_read32_locked(
	struct sc0710_dev *dev,
	u8 target,
	u32 *value)
{
	const u32 request[] = {
		HD60PRO_MAILBOX_TRIGGER_VALUE,
		HD60PRO_CMD_TARGET_READ32,
		target,
	};

	if (!value)
		return -EINVAL;

	return sc0710_hd60pro_mode0_target_command_locked(
		dev,
		request,
		ARRAY_SIZE(request),
		value);
}


/*
 * FUN_140287b54
 *
 * [800, 1d, target, reg & 0xff, 1, byte_value]
 */
static int __maybe_unused
sc0710_hd60pro_extended_reg_write_u8_locked(
	struct sc0710_dev *dev,
	u8 target,
	u8 reg,
	u8 value)
{
	const u32 request[] = {
		HD60PRO_MAILBOX_TRIGGER_VALUE,
		HD60PRO_CMD_TARGET_REG_EXTENDED,
		target,
		reg,
		1,
		value,
	};

	return sc0710_hd60pro_mode0_target_command_locked(
		dev,
		request,
		ARRAY_SIZE(request),
		NULL);
}


/*
 * FUN_140287bd8
 *
 * [800, 1d, target, reg & 0xff, 8, dword_value]
 *
 * Keep 8 described only as the observed subtype/format selector.
 * Its firmware-side semantics are not proven yet.
 */
static int __maybe_unused
sc0710_hd60pro_extended_reg_write_u32_locked(
	struct sc0710_dev *dev,
	u8 target,
	u8 reg,
	u32 value)
{
	const u32 request[] = {
		HD60PRO_MAILBOX_TRIGGER_VALUE,
		HD60PRO_CMD_TARGET_REG_EXTENDED,
		target,
		reg,
		8,
		value,
	};

	return sc0710_hd60pro_mode0_target_command_locked(
		dev,
		request,
		ARRAY_SIZE(request),
		NULL);
}



/*
 * Return whether a page is part of the statically recovered FA:1C
 * frontend path.
 *
 * This is intentionally not a general MZ0380 page whitelist.
 */
static bool __maybe_unused
sc0710_hd60pro_paged_page_allowed(u8 page)
{
	switch (page) {
	case 0x00:
	case 0x01:
	case 0x02:
	case 0x80:
		return true;
	default:
		return false;
	}
}


/*
 * Select one page of the target-0x9c frontend.
 *
 * Recovered Windows transport:
 *
 *   [800, 1b, 9c, 00, page]
 *
 * Windows stores the requested page in private+0x2090 before learning
 * whether the selector command succeeded. Linux deliberately does not
 * reproduce that cache-poisoning behavior: cached_page is changed only
 * after the mailbox command succeeds.
 *
 * A caller starts a sequence with:
 *
 *   u8 cached_page = HD60PRO_PAGED_PAGE_CACHE_INVALID;
 *
 * Caller must hold state->mailbox_lock.
 * Definition only: P2.1B adds no runtime caller.
 */
static int __maybe_unused
sc0710_hd60pro_paged_select_locked(
	struct sc0710_dev *dev,
	u8 *cached_page,
	u8 page)
{
	int ret;

	if (!dev || !cached_page)
		return -EINVAL;

	if (!sc0710_hd60pro_paged_page_allowed(page))
		return -EPERM;

	if (*cached_page == page)
		return 0;

	/*
	 * Once the selector command is issued, a transport error or timeout
	 * leaves the physical page ambiguous: the device may have accepted
	 * the write even though Linux did not observe completion.  Never
	 * preserve stale certainty across that boundary.
	 */
	*cached_page = HD60PRO_PAGED_PAGE_CACHE_INVALID;

	ret = sc0710_hd60pro_target_reg_write_locked(
		dev,
		HD60PRO_FRONTEND_PAGED_TARGET,
		0x00,
		page);
	if (ret)
		return ret;

	*cached_page = page;
	return 0;
}


/*
 * FUN_140277884 specialized to the target-0x9c FA:1C path.
 *
 * Select the requested page if needed, then issue:
 *
 *   [800, 1a, 9c, reg, 0]
 *
 * The successful response is the full DWORD from BAR0+0x10.
 *
 * Caller must hold state->mailbox_lock.
 * Definition only: no runtime caller in P2.1B.
 */
static int __maybe_unused
sc0710_hd60pro_paged_reg_read_locked(
	struct sc0710_dev *dev,
	u8 *cached_page,
	u8 page,
	u8 reg,
	u32 *value)
{
	int ret;

	if (!value)
		return -EINVAL;

	ret = sc0710_hd60pro_paged_select_locked(
		dev,
		cached_page,
		page);
	if (ret)
		return ret;

	ret = sc0710_hd60pro_target_reg_read_locked(
		dev,
		HD60PRO_FRONTEND_PAGED_TARGET,
		reg,
		value);
	if (ret && cached_page)
		*cached_page = HD60PRO_PAGED_PAGE_CACHE_INVALID;

	return ret;
}


/*
 * FUN_14028658c specialized to the target-0x9c FA:1C path.
 *
 * Select the requested page if needed, then issue:
 *
 *   [800, 1b, 9c, reg, value]
 *
 * Register 0x00 is reserved for the explicit page-selector helper and is
 * rejected here so an ordinary paged write cannot silently desynchronize
 * the physical selector from the Linux page cache.
 *
 * Caller must hold state->mailbox_lock.
 * Definition only: no runtime caller in P2.1B.
 */
static int __maybe_unused
sc0710_hd60pro_paged_reg_write_locked(
	struct sc0710_dev *dev,
	u8 *cached_page,
	u8 page,
	u8 reg,
	u8 value)
{
	int ret;

	/*
	 * Register 0x00 is the recovered page selector itself.  It may only
	 * be written by sc0710_hd60pro_paged_select_locked(), which uses the
	 * raw target/register primitive and updates the software cache with
	 * fail-safe semantics.
	 */
	if (reg == 0x00)
		return -EPERM;

	ret = sc0710_hd60pro_paged_select_locked(
		dev,
		cached_page,
		page);
	if (ret)
		return ret;

	ret = sc0710_hd60pro_target_reg_write_locked(
		dev,
		HD60PRO_FRONTEND_PAGED_TARGET,
		reg,
		value);
	if (ret && cached_page)
		*cached_page = HD60PRO_PAGED_PAGE_CACHE_INVALID;

	return ret;
}



/*
 * P2.2A: deterministic fragments of the FA:1C frontend program.
 *
 * These tables intentionally stop at every recovered ordering barrier:
 * helper call, register read/modify/write, property-derived value or
 * device/readback-dependent decision.
 *
 * They are not yet a runnable initialization sequence.
 */
struct sc0710_hd60pro_paged_write8 {
	u8 page;
	u8 reg;
	u8 value;
};


/*
 * Apply one already-audited table through the P2.1B paged transport.
 *
 * Caller must hold state->mailbox_lock.
 * Definition only: P2.2A adds no runtime caller.
 */
static int __maybe_unused
sc0710_hd60pro_apply_paged_write_table_locked(
	struct sc0710_dev *dev,
	u8 *cached_page,
	const struct sc0710_hd60pro_paged_write8 *table,
	unsigned int count)
{
	unsigned int i;
	int ret;

	if (!dev || !cached_page || (!table && count))
		return -EINVAL;

	for (i = 0; i < count; i++) {
		ret = sc0710_hd60pro_paged_reg_write_locked(
			dev,
			cached_page,
			table[i].page,
			table[i].reg,
			table[i].value);
		if (ret)
			return ret;
	}

	return 0;
}


/*
 * FUN_14024dc28:
 *
 *   page0:13 <- 08
 *
 * Ordering barrier immediately afterward:
 * FUN_14024eeb8(ctx, 0).
 */
static const struct sc0710_hd60pro_paged_write8
sc0710_hd60pro_fa1c_seq_before_control_line[] __maybe_unused = {
	{ 0x00, 0x13, 0x08 },
};


/*
 * Continues after FUN_14024eeb8(ctx, 0).
 *
 * Ordering barrier afterward:
 * property-derived page1:17/18/19 programming.
 */
static const struct sc0710_hd60pro_paged_write8
sc0710_hd60pro_fa1c_seq_after_control_line[] __maybe_unused = {
	{ 0x00, 0x41, 0x6f },
	{ 0x00, 0xb8, 0x00 },

	{ 0x01, 0x0f, 0x02 },
	{ 0x01, 0x16, 0x30 },

	{ 0x00, 0x64, 0x02 },
	{ 0x00, 0x65, 0xff },
	{ 0x00, 0x66, 0x00 },
	{ 0x00, 0x67, 0x02 },
};


/*
 * After the page1:17/18/19 decision:
 *
 *   page1:1a <- 50
 *
 * Ordering barrier afterward:
 * RMW page1:2a |= 07.
 */
static const struct sc0710_hd60pro_paged_write8
sc0710_hd60pro_fa1c_seq_after_eq[] __maybe_unused = {
	{ 0x01, 0x1a, 0x50 },
};


/*
 * The RMW page1:2a is followed by:
 *
 *   page2:08 <- 03
 *
 * The next barrier is the FA:1C detection/readback section which
 * eventually programs page1:24 and may touch page1:25/26/27.
 */
static const struct sc0710_hd60pro_paged_write8
sc0710_hd60pro_fa1c_seq_after_rmw_page1_2a[] __maybe_unused = {
	{ 0x02, 0x08, 0x03 },
};


/*
 * Continues after page1:24/readback handling.
 *
 * Ordering barrier afterward:
 * RMW page0:ae |= 04, followed by the NativeColorSpace-derived page0:ad.
 */
static const struct sc0710_hd60pro_paged_write8
sc0710_hd60pro_fa1c_seq_after_page1_24_block[] __maybe_unused = {
	{ 0x01, 0x30, 0x80 },
	{ 0x01, 0x31, 0x00 },
	{ 0x01, 0x32, 0x00 },

	{ 0x00, 0xb0, 0x14 },
};


/*
 * After page0:ae RMW, page0:ad property programming, page0:b1/b2 and
 * the board-specific page0:b3 value, Windows writes page0:b4 <- 55
 * before an immediate read/clear-bits RMW of b4.
 *
 * Keep only the deterministic fixed writes on the far side of that
 * dynamic barrier here.
 */
static const struct sc0710_hd60pro_paged_write8
sc0710_hd60pro_fa1c_seq_page2_prefix[] __maybe_unused = {
	{ 0x02, 0x01, 0x61 },
	{ 0x02, 0x02, 0xf5 },
};


/*
 * Continues after RMW page2:03 |= 02.
 *
 * Ordering barrier afterward:
 * RMW page2:25, page2:02 and page2:07.
 */
static const struct sc0710_hd60pro_paged_write8
sc0710_hd60pro_fa1c_seq_page2_body_a[] __maybe_unused = {
	{ 0x02, 0x04, 0x01 },
	{ 0x02, 0x05, 0x00 },
	{ 0x02, 0x06, 0x08 },

	{ 0x02, 0x1c, 0x1a },
	{ 0x02, 0x1d, 0x00 },
	{ 0x02, 0x1e, 0x00 },
	{ 0x02, 0x1f, 0x00 },
};


/*
 * Continues after RMW page2:25 / 02 / 07.
 *
 * Ordering barrier afterward:
 * RMW page2:21 &= fc.
 */
static const struct sc0710_hd60pro_paged_write8
sc0710_hd60pro_fa1c_seq_page2_body_b[] __maybe_unused = {
	{ 0x02, 0x17, 0xc0 },
	{ 0x02, 0x19, 0xff },
	{ 0x02, 0x1a, 0xff },
	{ 0x02, 0x1b, 0xfc },
	{ 0x02, 0x20, 0x00 },
};


/*
 * After RMW page2:21:
 *
 *   page2:22 <- 26
 *
 * The next value, page2:27, depends on AudioInputProperty, so it is
 * intentionally excluded from the static table.
 */
static const struct sc0710_hd60pro_paged_write8
sc0710_hd60pro_fa1c_seq_page2_after_rmw21[] __maybe_unused = {
	{ 0x02, 0x22, 0x26 },
};



/*
 * P2.2B: exact 8-bit read/modify/write operations recovered from the
 * FA:1C frontend path.
 *
 * Windows reads a full DWORD from BAR0+0x10 but truncates it to the low
 * byte before applying the observed transform:
 *
 *     new_value = (old_low8 & and_mask) | or_mask;
 *
 * No semantic meaning is assigned to the individual bits here.
 */
struct sc0710_hd60pro_paged_rmw8 {
	u8 page;
	u8 reg;
	u8 and_mask;
	u8 or_mask;
};


/*
 * Perform one exact low-byte RMW through the P2.1B transport.
 *
 * Register 0 is forbidden here because it is the page selector and must
 * remain under sc0710_hd60pro_paged_select_locked().
 *
 * Caller must hold state->mailbox_lock.
 * Definition only: P2.2B adds no runtime caller.
 */
static int __maybe_unused
sc0710_hd60pro_paged_rmw8_locked(
	struct sc0710_dev *dev,
	u8 *cached_page,
	u8 page,
	u8 reg,
	u8 and_mask,
	u8 or_mask)
{
	u32 response;
	u8 value;
	int ret;

	if (!dev || !cached_page)
		return -EINVAL;

	if (reg == 0x00)
		return -EPERM;

	ret = sc0710_hd60pro_paged_reg_read_locked(
		dev,
		cached_page,
		page,
		reg,
		&response);
	if (ret)
		return ret;

	value = ((u8)response & and_mask) | or_mask;

	return sc0710_hd60pro_paged_reg_write_locked(
		dev,
		cached_page,
		page,
		reg,
		value);
}


/*
 * Apply one contiguous RMW fragment.  Arrays remain split wherever the
 * Windows path contains an intervening fixed write, helper, property
 * decision or readback-dependent branch.
 *
 * Caller must hold state->mailbox_lock.
 * Definition only: no runtime caller in P2.2B.
 */
static int __maybe_unused
sc0710_hd60pro_apply_paged_rmw_table_locked(
	struct sc0710_dev *dev,
	u8 *cached_page,
	const struct sc0710_hd60pro_paged_rmw8 *table,
	unsigned int count)
{
	unsigned int i;
	int ret;

	if (!dev || !cached_page || (!table && count))
		return -EINVAL;

	for (i = 0; i < count; i++) {
		ret = sc0710_hd60pro_paged_rmw8_locked(
			dev,
			cached_page,
			table[i].page,
			table[i].reg,
			table[i].and_mask,
			table[i].or_mask);
		if (ret)
			return ret;
	}

	return 0;
}


/*
 * After:
 *   page1:1a <- 50
 *
 * Before:
 *   page2:08 <- 03
 */
static const struct sc0710_hd60pro_paged_rmw8
sc0710_hd60pro_fa1c_rmw_after_eq[] __maybe_unused = {
	{ 0x01, 0x2a, 0xff, 0x07 },
};


/*
 * After:
 *   page1:30 <- 80
 *   page1:31 <- 00
 *   page1:32 <- 00
 *   page0:b0 <- 14
 *
 * Before the NativeColorSpace-derived page0:ad write.
 */
static const struct sc0710_hd60pro_paged_rmw8
sc0710_hd60pro_fa1c_rmw_after_page1_24_block[] __maybe_unused = {
	{ 0x00, 0xae, 0xff, 0x04 },
};


/*
 * Windows first seeds:
 *
 *   page0:b4 <- 55
 *
 * then immediately clears bits 1:0 from the readback.
 */
static const struct sc0710_hd60pro_paged_rmw8
sc0710_hd60pro_fa1c_rmw_after_b4_seed[] __maybe_unused = {
	{ 0x00, 0xb4, 0xfc, 0x00 },
};


/*
 * After:
 *   page2:01 <- 61
 *   page2:02 <- f5
 *
 * Before the deterministic page2 body.
 */
static const struct sc0710_hd60pro_paged_rmw8
sc0710_hd60pro_fa1c_rmw_page2_after_prefix[] __maybe_unused = {
	{ 0x02, 0x03, 0xff, 0x02 },
};


/*
 * Three consecutive RMWs between the two deterministic page2 bodies.
 */
static const struct sc0710_hd60pro_paged_rmw8
sc0710_hd60pro_fa1c_rmw_page2_middle[] __maybe_unused = {
	{ 0x02, 0x25, 0xff, 0xa2 },
	{ 0x02, 0x02, 0xff, 0x80 },
	{ 0x02, 0x07, 0xff, 0x04 },
};


/*
 * After page2 body B and before:
 *
 *   page2:22 <- 26
 */
static const struct sc0710_hd60pro_paged_rmw8
sc0710_hd60pro_fa1c_rmw_page2_after_body_b[] __maybe_unused = {
	{ 0x02, 0x21, 0xfc, 0x00 },
};


/*
 * Final recovered RMW trio after the AudioInput-derived page2:27 write.
 *
 * Order is significant:
 *
 *   page2:2e = old | a1
 *   page0:ab = (old & 95) | 15
 *   page0:ac = (old & d5) | 15
 */
static const struct sc0710_hd60pro_paged_rmw8
sc0710_hd60pro_fa1c_rmw_tail[] __maybe_unused = {
	{ 0x02, 0x2e, 0xff, 0xa1 },
	{ 0x00, 0xab, 0x95, 0x15 },
	{ 0x00, 0xac, 0xd5, 0x15 },
};



/*
 * P2.2C: values derived from the proven FA:1C first-initialization
 * baseline.
 *
 * These helpers deliberately do not generalize Windows property/config
 * behavior. They model only the baseline already recovered for:
 *
 *   PCI 12ab:0380
 *   subsystem 1cfa:0006
 *   property-absent/default first initialization
 *
 * They remain definition-only in P2.2C.
 */


/*
 * Property baseline:
 *
 *   ctx+97d8 CustomCompanyAlconProperty = 0
 *   ctx+81d8 CustomAnalogVideoInputEqProperty = 1
 *
 * Windows therefore programs:
 *
 *   page1:17 <- 00
 *   page1:18 <- 00
 *   page1:19 <- 00
 *
 * Caller must hold state->mailbox_lock.
 */
static int __maybe_unused
sc0710_hd60pro_fa1c_program_eq_baseline_locked(
	struct sc0710_dev *dev,
	u8 *cached_page)
{
	int ret;

	ret = sc0710_hd60pro_paged_reg_write_locked(
		dev, cached_page, 0x01, 0x17, 0x00);
	if (ret)
		return ret;

	ret = sc0710_hd60pro_paged_reg_write_locked(
		dev, cached_page, 0x01, 0x18, 0x00);
	if (ret)
		return ret;

	return sc0710_hd60pro_paged_reg_write_locked(
		dev, cached_page, 0x01, 0x19, 0x00);
}


/*
 * FA:1C first-init baseline around page1:24.
 *
 * FUN_14024ea94 is an ordering barrier immediately before this block and
 * is intentionally NOT reproduced here; P2.2D will model that helper
 * separately.
 *
 * Proven first-init software baseline:
 *
 *   ctx+6a34 = 0
 *
 * therefore:
 *
 *   page1:24 <- 40
 *
 * Windows then reads page1:24.  If physical readback bit 0 is set:
 *
 *   page1:25 <- 00
 *   page1:26 <- 00
 *   page1:27 <- 00   (five times)
 *
 * The conditional remains dependent on physical readback; Linux must not
 * predict it from the value just written.
 *
 * Caller must hold state->mailbox_lock.
 */
static int __maybe_unused
sc0710_hd60pro_fa1c_program_page1_24_baseline_locked(
	struct sc0710_dev *dev,
	u8 *cached_page)
{
	u32 response;
	unsigned int i;
	int ret;

	ret = sc0710_hd60pro_paged_reg_write_locked(
		dev, cached_page, 0x01, 0x24, 0x40);
	if (ret)
		return ret;

	ret = sc0710_hd60pro_paged_reg_read_locked(
		dev, cached_page, 0x01, 0x24, &response);
	if (ret)
		return ret;

	if (!(response & BIT(0)))
		return 0;

	ret = sc0710_hd60pro_paged_reg_write_locked(
		dev, cached_page, 0x01, 0x25, 0x00);
	if (ret)
		return ret;

	ret = sc0710_hd60pro_paged_reg_write_locked(
		dev, cached_page, 0x01, 0x26, 0x00);
	if (ret)
		return ret;

	for (i = 0; i < 5; i++) {
		ret = sc0710_hd60pro_paged_reg_write_locked(
			dev, cached_page, 0x01, 0x27, 0x00);
		if (ret)
			return ret;
	}

	return 0;
}


/*
 * Baseline block following page0:ae RMW.
 *
 * NativeColorSpaceProperty default:
 *
 *   ctx+81f8 = 0
 *   -> page0:ad <- 05
 *
 * FA:1C board identity forces the local bVar10 used for page0:b3 to 0:
 *
 *   page0:b1 <- c0
 *   page0:b2 <- 00
 *   page0:b3 <- 00
 *   page0:b4 <- 55
 *
 * The immediate read/clear-bits operation on page0:b4 is P2.2B and is
 * intentionally not duplicated here.
 *
 * Caller must hold state->mailbox_lock.
 */
static int __maybe_unused
sc0710_hd60pro_fa1c_program_native_board_baseline_locked(
	struct sc0710_dev *dev,
	u8 *cached_page)
{
	int ret;

	ret = sc0710_hd60pro_paged_reg_write_locked(
		dev, cached_page, 0x00, 0xad, 0x05);
	if (ret)
		return ret;

	ret = sc0710_hd60pro_paged_reg_write_locked(
		dev, cached_page, 0x00, 0xb1, 0xc0);
	if (ret)
		return ret;

	ret = sc0710_hd60pro_paged_reg_write_locked(
		dev, cached_page, 0x00, 0xb2, 0x00);
	if (ret)
		return ret;

	ret = sc0710_hd60pro_paged_reg_write_locked(
		dev, cached_page, 0x00, 0xb3, 0x00);
	if (ret)
		return ret;

	return sc0710_hd60pro_paged_reg_write_locked(
		dev, cached_page, 0x00, 0xb4, 0x55);
}


/*
 * AudioInputProperty baseline:
 *
 *   ctx+73bc = 3
 *
 * Windows expression:
 *
 *   ~-(ctx+73bc == 0)
 *
 * therefore yields low byte ff for this baseline:
 *
 *   page2:27 <- ff
 *
 * Caller must hold state->mailbox_lock.
 */
static int __maybe_unused
sc0710_hd60pro_fa1c_program_audio_input_baseline_locked(
	struct sc0710_dev *dev,
	u8 *cached_page)
{
	return sc0710_hd60pro_paged_reg_write_locked(
		dev, cached_page, 0x02, 0x27, 0xff);
}



/*
 * P2.2D1: two small ordered helper sequences recovered from the
 * FA:1C FUN_14024dc28 path.
 *
 * These remain definition-only.  They are intentionally kept separate
 * from FUN_14024eeb8, FUN_14024ea94 and FUN_14024d2ec, whose ordering and
 * side effects are modeled in later P2.2D steps.
 */


/*
 * FUN_14024d2a4:
 *
 *   page0:b8 <- 10
 *   page0:b8 <- 00
 *
 * Preserve both writes and their order.  Do not collapse them into a
 * final-state assignment; the intermediate transition may be meaningful
 * to the device.
 */
static const struct sc0710_hd60pro_paged_write8
sc0710_hd60pro_fa1c_b8_write_pair[] __maybe_unused = {
        { 0x00, 0xb8, 0x10 },
        { 0x00, 0xb8, 0x00 },
};


/*
 * FUN_14024db30:
 *
 *   page2:07 <- f4
 *   page2:07 <- 04
 *
 * As above, preserve the ordered pair exactly.
 */
static const struct sc0710_hd60pro_paged_write8
sc0710_hd60pro_fa1c_page2_07_write_pair[] __maybe_unused = {
        { 0x02, 0x07, 0xf4 },
        { 0x02, 0x07, 0x04 },
};


/*
 * Caller must hold state->mailbox_lock.
 * Definition only: no runtime caller in P2.2D1.
 */
static int __maybe_unused
sc0710_hd60pro_fa1c_apply_b8_pair_locked(
        struct sc0710_dev *dev,
        u8 *cached_page)
{
        return sc0710_hd60pro_apply_paged_write_table_locked(
                dev,
                cached_page,
                sc0710_hd60pro_fa1c_b8_write_pair,
                ARRAY_SIZE(sc0710_hd60pro_fa1c_b8_write_pair));
}


/*
 * Caller must hold state->mailbox_lock.
 * Definition only: no runtime caller in P2.2D1.
 */
static int __maybe_unused
sc0710_hd60pro_fa1c_apply_page2_07_pair_locked(
        struct sc0710_dev *dev,
        u8 *cached_page)
{
        return sc0710_hd60pro_apply_paged_write_table_locked(
                dev,
                cached_page,
                sc0710_hd60pro_fa1c_page2_07_write_pair,
                ARRAY_SIZE(sc0710_hd60pro_fa1c_page2_07_write_pair));
}



/*
 * P2.2D2: hardware-visible portion of FUN_14024ea94.
 *
 * Recovered ordering:
 *
 *   page1:25 <- 00
 *   page1:26 <- 00
 *
 *   repeat 5 times:
 *       delay ~20 ms
 *       read page1:27
 *
 * Windows stores the samples in software/global state.  Linux P2 keeps
 * them caller-owned instead; this helper models the hardware interaction
 * without introducing another persistent state machine.
 *
 * This is distinct from the later page1:24 readback path, where Windows
 * may WRITE page1:27 <- 00 five times.
 */
#define HD60PRO_FA1C_PAGE1_27_SAMPLE_COUNT       5U
#define HD60PRO_FA1C_PAGE1_27_SAMPLE_DELAY_MS    20U


/*
 * Caller must hold state->mailbox_lock.
 * Definition only: P2.2D2 adds no runtime caller.
 */
static int __maybe_unused
sc0710_hd60pro_fa1c_sample_page1_27_locked(
        struct sc0710_dev *dev,
        u8 *cached_page,
        u32 samples[HD60PRO_FA1C_PAGE1_27_SAMPLE_COUNT])
{
        unsigned int i;
        int ret;

        if (!dev || !cached_page || !samples)
                return -EINVAL;

        ret = sc0710_hd60pro_paged_reg_write_locked(
                dev,
                cached_page,
                0x01,
                0x25,
                0x00);
        if (ret)
                return ret;

        ret = sc0710_hd60pro_paged_reg_write_locked(
                dev,
                cached_page,
                0x01,
                0x26,
                0x00);
        if (ret)
                return ret;

        for (i = 0; i < HD60PRO_FA1C_PAGE1_27_SAMPLE_COUNT; i++) {
                msleep(HD60PRO_FA1C_PAGE1_27_SAMPLE_DELAY_MS);

                ret = sc0710_hd60pro_paged_reg_read_locked(
                        dev,
                        cached_page,
                        0x01,
                        0x27,
                        &samples[i]);
                if (ret)
                        return ret;
        }

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
 * Wait for completion of the exact Windows MZ0380 bootstrap request.
 *
 * Caller must hold state->mailbox_lock and must already have issued
 * sc0710_hd60pro_bootstrap_request_locked().
 *
 * This helper is deliberately read-only. It must not clear mailbox status,
 * ACK/rearm IRQ state, retry the request, reset hardware or modify PCI state.
 *
 * Linux currently uses bounded polling of BAR0 mailbox status bit 0 as the
 * bootstrap completion observation. IRQ mailbox-complete remains diagnostic
 * only at this stage.
 */
static int __maybe_unused
sc0710_hd60pro_wait_bootstrap_completion_locked(
	struct sc0710_dev *dev,
	struct sc0710_hd60pro_mailbox_result *result)
{
	u64 started_ns;
	u32 status = 0;
	unsigned int i;
	int ret;
	int snapshot_ret;

	if (!result)
		return -EINVAL;

	memset(result, 0, sizeof(*result));

	if (!dev || !dev->pci || !dev->lmmio[0])
		return -ENODEV;

	started_ns = ktime_get_ns();

	for (i = 0; i < HD60PRO_MAILBOX_POLL_COUNT; i++) {
		ret = sc0710_hd60pro_read_bar0(
			dev, HD60PRO_BAR0_MAILBOX_STATUS, &status);
		if (ret)
			goto out;

		result->polls = i + 1;
		result->last_poll_status = status;

		if (status & HD60PRO_MAILBOX_STATUS_COMPLETE) {
			result->completed = true;
			ret = 0;
			goto out;
		}

		usleep_range(HD60PRO_MAILBOX_POLL_MIN_US,
			     HD60PRO_MAILBOX_POLL_MAX_US);
	}

	ret = -ETIMEDOUT;

out:
	result->elapsed_ns = ktime_get_ns() - started_ns;

	/*
	 * Diagnostic-only snapshot. Do not clear status, ACK IRQ state,
	 * retry, reset, or repair PCI configuration here.
	 */
	snapshot_ret =
		sc0710_hd60pro_take_mailbox_snapshot(dev, &result->after);

	/*
	 * The after-snapshot is diagnostic only. Failure to collect it must not
	 * alter the primary polling outcome.
	 */
	if (!snapshot_ret) {
		result->after_valid = true;
		result->late_completion =
			!result->completed &&
			!!(result->after.mailbox_status &
			   HD60PRO_MAILBOX_STATUS_COMPLETE);
	}

	return ret;
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
	state->signal.completed = false;
	state->signal.signal_value_valid = false;
	state->signal.late_completion = false;
	state->signal.last_error = 0;
	state->signal.signal_index = HD60PRO_SIGNAL_HDMI_HPD;
	state->signal.signal_value = 0;
	state->signal.polls = 0;
	state->signal.last_poll_status = 0;
	state->signal.response = 0;
	state->signal.elapsed_ns = 0;
	memset(&state->signal.before, 0, sizeof(state->signal.before));
	memset(&state->signal.after, 0, sizeof(state->signal.after));
}

static void
sc0710_hd60pro_reset_clear_result(struct sc0710_hd60pro_state *state)
{
	state->clear_in_progress = false;
	state->clear.completed = false;
	state->clear.last_error = 0;
	state->clear.elapsed_ns = 0;
	memset(&state->clear.before, 0, sizeof(state->clear.before));
	memset(&state->clear.after, 0, sizeof(state->clear.after));
}

static void
sc0710_hd60pro_reset_i2c_result(struct sc0710_hd60pro_state *state)
{
	state->i2c_in_progress = false;
	memset(&state->i2c, 0, sizeof(state->i2c));
	state->i2c.address_8bit = HD60PRO_I2C_VIDEO_FRONTEND_ADDR_8BIT;
	state->i2c.reg = HD60PRO_I2C_EXPERIMENT_DEFAULT_REG;
}

static const char *
sc0710_hd60pro_control_phase_name(enum sc0710_hd60pro_control_phase phase)
{
	switch (phase) {
	case HD60PRO_CONTROL_IDLE:
		return "idle";
	case HD60PRO_CONTROL_BOOTSTRAP:
		return "bootstrap";
	case HD60PRO_CONTROL_RESET:
		return "reset";
	case HD60PRO_CONTROL_FRONTEND_CONFIG:
		return "frontend-config";
	case HD60PRO_CONTROL_READY:
		return "ready";
	case HD60PRO_CONTROL_FAILED:
		return "failed";
	default:
		return "unknown";
	}
}

static bool __maybe_unused
sc0710_hd60pro_control_transition_allowed(
	enum sc0710_hd60pro_control_phase from,
	enum sc0710_hd60pro_control_phase to)
{
	switch (from) {
	case HD60PRO_CONTROL_IDLE:
		return to == HD60PRO_CONTROL_BOOTSTRAP;

	case HD60PRO_CONTROL_BOOTSTRAP:
		return to == HD60PRO_CONTROL_RESET ||
		       to == HD60PRO_CONTROL_FAILED;

	case HD60PRO_CONTROL_RESET:
		return to == HD60PRO_CONTROL_FRONTEND_CONFIG ||
		       to == HD60PRO_CONTROL_FAILED;

	case HD60PRO_CONTROL_FRONTEND_CONFIG:
		return to == HD60PRO_CONTROL_READY ||
		       to == HD60PRO_CONTROL_FAILED;

	case HD60PRO_CONTROL_READY:
	case HD60PRO_CONTROL_FAILED:
	default:
		return false;
	}
}

/*
 * Caller must hold state->mailbox_lock.
 *
 * Future active control-plane operations will keep the state transition in
 * the same critical section as the mailbox transaction that justifies it.
 * This helper deliberately does not acquire the mutex itself.
 */
static int __maybe_unused
sc0710_hd60pro_control_transition_locked(
	struct sc0710_hd60pro_state *state,
	enum sc0710_hd60pro_control_phase to,
	int last_error)
{
	enum sc0710_hd60pro_control_phase from;

	if (!state)
		return -EINVAL;

	from = state->control.phase;

	if (!sc0710_hd60pro_control_transition_allowed(from, to))
		return -EPERM;

	if (to == HD60PRO_CONTROL_FAILED) {
		if (last_error >= 0)
			return -EINVAL;
	} else if (last_error) {
		return -EINVAL;
	}

	/*
	 * phase is the commit point for the new control-plane state.
	 * Readers of this state already serialize on mailbox_lock.
	 */
	state->control.last_error = last_error;
	state->control.phase = to;

	return 0;
}

static void
sc0710_hd60pro_reset_control_state(struct sc0710_hd60pro_state *state)
{
	state->control.phase = HD60PRO_CONTROL_IDLE;
	state->control.last_error = 0;
}

static void
sc0710_hd60pro_reset_bootstrap_state(struct sc0710_hd60pro_state *state)
{
	memset(&state->bootstrap, 0, sizeof(state->bootstrap));
}

static void
sc0710_hd60pro_reset_firmware_version_state(
	struct sc0710_hd60pro_state *state)
{
	memset(&state->firmware_version, 0,
	       sizeof(state->firmware_version));
}

/*
 * Copy the hardware-observation result into bootstrap diagnostics.
 *
 * Caller must hold state->mailbox_lock. This helper is software-only:
 * no MMIO, PCI access or control-plane transition is performed here.
 */
static void __maybe_unused
sc0710_hd60pro_record_bootstrap_result_locked(
	struct sc0710_hd60pro_state *state,
	const struct sc0710_hd60pro_mailbox_result *result)
{
	if (!state || !result)
		return;

	state->bootstrap.mailbox_completed = result->completed;
	state->bootstrap.late_completion = result->late_completion;
	state->bootstrap.after_valid = result->after_valid;
	state->bootstrap.polls = result->polls;
	state->bootstrap.last_poll_status = result->last_poll_status;
	state->bootstrap.elapsed_ns = result->elapsed_ns;
	state->bootstrap.after = result->after;
}

/*
 * Begin the Linux-side bootstrap attempt.
 *
 * Caller must hold state->mailbox_lock. This helper changes only software
 * bookkeeping and control-plane state; it performs no PCI or MMIO access.
 *
 * The one-shot is consumed only after IDLE -> BOOTSTRAP succeeds, but still
 * before any future hardware action can be issued by the caller.
 */
static int __maybe_unused
sc0710_hd60pro_begin_bootstrap_locked(struct sc0710_hd60pro_state *state)
{
	int ret;

	if (!state)
		return -EINVAL;

	if (state->bootstrap.in_progress)
		return -EBUSY;

	if (state->bootstrap.attempt_consumed)
		return -EALREADY;

	ret = sc0710_hd60pro_control_transition_locked(
		state, HD60PRO_CONTROL_BOOTSTRAP, 0);
	if (ret)
		return ret;

	state->bootstrap.attempt_consumed = true;
	state->bootstrap.in_progress = true;
	state->bootstrap.completed = false;
	state->bootstrap.last_error = 0;

	return 0;
}

/*
 * Finish the bootstrap substage of Linux P1 after its hardware outcome is
 * known.
 *
 * Caller must hold state->mailbox_lock. This helper changes only software
 * bookkeeping and control-plane state; it performs no PCI or MMIO access.
 *
 * Failure commits the global control plane:
 *
 *     BOOTSTRAP -> FAILED
 *
 * Success deliberately remains in BOOTSTRAP. It records only that opcode
 * 0x01 completed successfully; the firmware-version gate must still pass
 * before the P1 orchestrator may advance the global phase to RESET.
 */
static int __maybe_unused
sc0710_hd60pro_finish_bootstrap_locked(
	struct sc0710_hd60pro_state *state,
	int result)
{
	int ret;

	if (!state)
		return -EINVAL;

	if (!state->bootstrap.attempt_consumed ||
	    !state->bootstrap.in_progress)
		return -EPERM;

	if (state->control.phase != HD60PRO_CONTROL_BOOTSTRAP)
		return -EPERM;

	if (result > 0)
		return -EINVAL;

	if (result) {
		ret = sc0710_hd60pro_control_transition_locked(
			state, HD60PRO_CONTROL_FAILED, result);
		if (ret)
			return ret;
	}

	state->bootstrap.completed = !result;
	state->bootstrap.last_error = result;
	state->bootstrap.in_progress = false;

	return result;
}


/*
 * Begin the firmware-version substage of P1.
 *
 * Software-only bookkeeping: no MMIO or PCI access is performed here.
 *
 * P1 integration keeps control.phase at BOOTSTRAP after the opcode-0x01
 * substage succeeds. Only then may this helper be called.
 *
 * Caller must hold state->mailbox_lock.
 * Runtime use is restricted to the complete P1 one-shot path.
 */
static int __maybe_unused
sc0710_hd60pro_begin_firmware_version_locked(
	struct sc0710_hd60pro_state *state)
{
	if (!state)
		return -EINVAL;

	if (state->control.phase != HD60PRO_CONTROL_BOOTSTRAP)
		return -EPERM;

	if (!state->bootstrap.attempt_consumed ||
	    state->bootstrap.in_progress ||
	    !state->bootstrap.completed)
		return -EPERM;

	if (state->firmware_version.in_progress)
		return -EBUSY;

	if (state->firmware_version.attempted)
		return -EALREADY;

	state->firmware_version.attempted = true;
	state->firmware_version.in_progress = true;
	state->firmware_version.completed = false;
	state->firmware_version.actual_major = 0;
	state->firmware_version.actual_minor = 0;
	state->firmware_version.last_error = 0;

	return 0;
}

/*
 * Finish the firmware-version substage and commit the global P1 outcome.
 *
 * Success:
 *
 *     BOOTSTRAP -> RESET
 *
 * Failure:
 *
 *     BOOTSTRAP -> FAILED
 *
 * This helper is software-only. The actual opcode-0x0a transaction remains
 * isolated inside sc0710_hd60pro_firmware_version_gate_locked().
 *
 * Caller must hold state->mailbox_lock.
 * Runtime use is restricted to the complete P1 one-shot path.
 */
static int __maybe_unused
sc0710_hd60pro_finish_firmware_version_locked(
	struct sc0710_hd60pro_state *state,
	int result,
	u32 actual_major,
	u32 actual_minor)
{
	enum sc0710_hd60pro_control_phase next;
	int ret;

	if (!state)
		return -EINVAL;

	if (!state->firmware_version.attempted ||
	    !state->firmware_version.in_progress)
		return -EPERM;

	if (state->control.phase != HD60PRO_CONTROL_BOOTSTRAP)
		return -EPERM;

	if (result > 0)
		return -EINVAL;

	next = result ?
		HD60PRO_CONTROL_FAILED :
		HD60PRO_CONTROL_RESET;

	ret = sc0710_hd60pro_control_transition_locked(
		state, next, result);
	if (ret)
		return ret;

	state->firmware_version.actual_major = actual_major;
	state->firmware_version.actual_minor = actual_minor;
	state->firmware_version.completed = !result;
	state->firmware_version.last_error = result;
	state->firmware_version.in_progress = false;

	return result;
}

/*
 * Caller must hold state->mailbox_lock.
 *
 * This is a pure active-control admission check. It may read PCI
 * configuration state, but it must not repair or modify hardware state.
 */
static int
sc0710_hd60pro_validate_active_context_locked(
	struct sc0710_dev *dev,
	struct sc0710_hd60pro_state *state)
{
	u16 command;
	int ret;

	if (!READ_ONCE(hd60pro_active_control))
		return -EPERM;

	if (!dev || !dev->pci || !state || READ_ONCE(dev->disconnected))
		return -ENODEV;

	if (dev->board != SC0710_BOARD_ELGATO_HD60_PRO ||
	    dev->hw_ops != &sc0710_hd60pro_ops ||
	    dev->observational_only ||
	    dev->pci->vendor != 0x12ab || dev->pci->device != 0x0380 ||
	    dev->pci->subsystem_vendor != 0x1cfa ||
	    dev->pci->subsystem_device != 0x0006)
		return -EPERM;

	if (dev->irq_requested || dev->kthread_dma || dev->kthread_hdmi)
		return -EBUSY;

	if (state->control.phase != HD60PRO_CONTROL_IDLE)
		return -EBUSY;

	ret = sc0710_hd60pro_read_pci_command(dev->pci, &command);
	if (ret)
		return ret;

	if (!(command & PCI_COMMAND_MEMORY))
		return -EIO;

	if (command & PCI_COMMAND_MASTER)
		return -EIO;

	return 0;
}

/*
 * Validate the passive snapshot required before the first bootstrap write.
 *
 * This is deliberately read-only. In particular, unlike the manual mailbox
 * snapshot validator, this function must not clear PCI bus mastering or
 * repair any other hardware state.
 *
 * Require an entirely quiescent mailbox/IRQ state so that a later observed
 * mailbox COMPLETE bit can be attributed to the bootstrap request rather
 * than to stale state that existed before it.
 */
static int
sc0710_hd60pro_validate_bootstrap_snapshot(
	const struct sc0710_hd60pro_mailbox_snapshot *snapshot)
{
	if (!snapshot)
		return -EINVAL;

	if (!(snapshot->pci_command & PCI_COMMAND_MEMORY))
		return -EIO;

	if (snapshot->pci_command & PCI_COMMAND_MASTER)
		return -EIO;

	if (snapshot->irq_status != 0)
		return -EBUSY;

	if (snapshot->mailbox_status != 0)
		return -EBUSY;

	return 0;
}

/*
 * Perform the read-only admission/preflight for a bootstrap attempt.
 *
 * Caller must hold state->mailbox_lock. This function may read PCI/MMIO
 * state and update Linux-side diagnostics, but it must not consume the
 * one-shot bootstrap attempt, transition control state or write hardware.
 */
static int __maybe_unused
sc0710_hd60pro_preflight_bootstrap_locked(
	struct sc0710_dev *dev,
	struct sc0710_hd60pro_state *state)
{
	int ret;

	if (!state)
		return -EINVAL;

	ret = sc0710_hd60pro_validate_active_context_locked(dev, state);
	if (ret)
		return ret;

	if (state->bootstrap.in_progress)
		return -EBUSY;

	if (state->bootstrap.attempt_consumed)
		return -EALREADY;

	ret = sc0710_hd60pro_validate_bootstrap_bar5_setup(dev);
	if (ret)
		return ret;

	/*
	 * Do not expose partially-populated snapshot data as valid if any
	 * individual read fails.
	 */
	state->bootstrap.before_valid = false;
	memset(&state->bootstrap.before, 0,
	       sizeof(state->bootstrap.before));

	ret = sc0710_hd60pro_take_mailbox_snapshot(
		dev, &state->bootstrap.before);
	if (ret)
		return ret;

	state->bootstrap.before_valid = true;

	return sc0710_hd60pro_validate_bootstrap_snapshot(
		&state->bootstrap.before);
}

/*
 * Persist the final P1 state in the kernel log.
 *
 * A failed active probe tears debugfs down during unwind, so this record is
 * intentionally emitted while the backend state and BAR mappings are still
 * alive. Caller must hold state->mailbox_lock.
 *
 * The top-level result is the complete P1 result. Bootstrap and firmware
 * version retain their own independent substage diagnostics below.
 */
static void __maybe_unused
sc0710_hd60pro_log_p1_result_locked(
	struct sc0710_dev *dev,
	struct sc0710_hd60pro_state *state,
	int result)
{
	const struct sc0710_hd60pro_firmware_version_state *firmware_version;
	const struct sc0710_hd60pro_bootstrap_state *bootstrap;
	u16 command = 0;
	int command_ret;

	if (!dev || !dev->pci || !state)
		return;

	bootstrap = &state->bootstrap;
	firmware_version = &state->firmware_version;
	command_ret = sc0710_hd60pro_read_pci_command(dev->pci, &command);

	pr_info("%s: HD60 Pro P1 result=%d phase=%s control_error=%d\n",
		dev->name,
		result,
		sc0710_hd60pro_control_phase_name(state->control.phase),
		state->control.last_error);

	pr_info("%s: HD60 Pro P1 bootstrap attempt_consumed=%u "
		"in_progress=%u completed=%u last_error=%d "
		"mailbox_completed=%u late_completion=%u polls=%u "
		"last_poll_status=0x%08x elapsed_us=%llu\n",
		dev->name,
		bootstrap->attempt_consumed,
		bootstrap->in_progress,
		bootstrap->completed,
		bootstrap->last_error,
		bootstrap->mailbox_completed,
		bootstrap->late_completion,
		bootstrap->polls,
		bootstrap->last_poll_status,
		(unsigned long long)(bootstrap->elapsed_ns / 1000));

	pr_info("%s: HD60 Pro P1 bootstrap before_valid=%u "
		"pci=0x%04x mailbox=0x%08x irq=0x%08x tag=0x%08x\n",
		dev->name,
		bootstrap->before_valid,
		bootstrap->before.pci_command,
		bootstrap->before.mailbox_status,
		bootstrap->before.irq_status,
		bootstrap->before.irq_tag);

	pr_info("%s: HD60 Pro P1 bootstrap after_valid=%u "
		"pci=0x%04x mailbox=0x%08x irq=0x%08x tag=0x%08x\n",
		dev->name,
		bootstrap->after_valid,
		bootstrap->after.pci_command,
		bootstrap->after.mailbox_status,
		bootstrap->after.irq_status,
		bootstrap->after.irq_tag);

	pr_info("%s: HD60 Pro P1 firmware_version expected=%u.%u "
		"attempted=%u in_progress=%u completed=%u "
		"actual=%u.%u last_error=%d\n",
		dev->name,
		HD60PRO_FW_VERSION_EXPECTED_MAJOR,
		HD60PRO_FW_VERSION_EXPECTED_MINOR,
		firmware_version->attempted,
		firmware_version->in_progress,
		firmware_version->completed,
		firmware_version->actual_major,
		firmware_version->actual_minor,
		firmware_version->last_error);

	if (!command_ret)
		pr_info("%s: HD60 Pro P1 final_pci_command=0x%04x "
			"memory_space=%u bus_master=%u\n",
			dev->name,
			command,
			!!(command & PCI_COMMAND_MEMORY),
			!!(command & PCI_COMMAND_MASTER));
	else
		pr_info("%s: HD60 Pro P1 final_pci_command_error=%d\n",
			dev->name, command_ret);
}

/*
 * Compose one complete Linux-side HD60 Pro bootstrap attempt.
 *
 * Caller must hold state->mailbox_lock.
 *
 * The preflight remains read-only and occurs before the one-shot is consumed.
 * Once begin_bootstrap_locked() succeeds there is no rollback to IDLE.
 *
 * Linux deliberately performs one bootstrap request only. The Windows driver
 * retries the request inside a bounded loop, but the current Linux policy is
 * fail-stop: one committed hardware attempt per driver lifetime.
 *
 * Once the PRE rearm completes, always attempt the matching POST rearm before
 * finishing the software state, even when BAR5 programming, request issue or
 * completion polling fails. If PRE rearm itself fails, do not invent a second
 * rearm as recovery for a partially-completed sequence.
 *
 * Completion diagnostics are recorded before POST rearm so timeout/error
 * evidence cannot be destroyed by the cleanup/rearm writes.
 *
 * Runtime entry is restricted to the explicit active-control bring-up path.
 */
static int __maybe_unused
sc0710_hd60pro_bootstrap_once_locked(
	struct sc0710_dev *dev,
	struct sc0710_hd60pro_state *state)
{
	struct sc0710_hd60pro_mailbox_result result;
	int post_ret;
	int ret;

	/*
	 * FUN_140278bb0 begins with sleep_ms(0x100) before its PRE rearm.
	 * Preserve that 256 ms settling interval before Linux takes its
	 * read-only preflight snapshot. The one-shot remains unconsumed
	 * throughout the delay.
	 */
	msleep(HD60PRO_BOOTSTRAP_SETTLE_MS);

	ret = sc0710_hd60pro_preflight_bootstrap_locked(dev, state);
	if (ret)
		goto out;

	ret = sc0710_hd60pro_begin_bootstrap_locked(state);
	if (ret)
		goto out;

	/*
	 * PRE rearm is the first hardware operation after consuming the
	 * one-shot. Failure here commits the attempt to FAILED, but we do not
	 * synthesize a second rearm for an incompletely-executed PRE sequence.
	 */
	ret = sc0710_hd60pro_rearm_irq_locked(dev);
	if (ret)
		goto finish;

	ret = sc0710_hd60pro_program_bootstrap_bar5_locked(dev);
	if (ret)
		goto post_rearm;

	ret = sc0710_hd60pro_bootstrap_request_locked(dev);
	if (ret)
		goto post_rearm;

	ret = sc0710_hd60pro_wait_bootstrap_completion_locked(dev, &result);
	sc0710_hd60pro_record_bootstrap_result_locked(state, &result);

post_rearm:
	/*
	 * Preserve the primary bootstrap error. A POST-rearm failure becomes
	 * the result only when every earlier stage succeeded.
	 */
	post_ret = sc0710_hd60pro_rearm_irq_locked(dev);
	if (!ret && post_ret)
		ret = post_ret;

finish:
	ret = sc0710_hd60pro_finish_bootstrap_locked(state, ret);

out:
	return ret;
}

/*
 * Execute the complete non-DMA P1 admission sequence.
 *
 * One driver-lifetime bootstrap attempt owns the whole sequence:
 *
 *   1. bootstrap opcode 0x01
 *   2. firmware-version opcode 0x0a
 *   3. strict compatibility gate against expected version 1.11
 *
 * Bootstrap success is only a substage result and deliberately leaves
 * control.phase at BOOTSTRAP. The firmware-version finalizer is the only
 * success path that advances BOOTSTRAP -> RESET.
 *
 * Any bootstrap failure or firmware-version failure permanently commits the
 * control plane to FAILED. There is no retry, firmware download, DMA, START
 * command or pci_set_master() operation in P1.
 *
 * Caller must hold state->mailbox_lock.
 */
static int __maybe_unused
sc0710_hd60pro_p1_once_locked(
	struct sc0710_dev *dev,
	struct sc0710_hd60pro_state *state)
{
	u32 actual_major = 0;
	u32 actual_minor = 0;
	int transition_ret;
	int ret;

	ret = sc0710_hd60pro_bootstrap_once_locked(dev, state);
	if (ret)
		return ret;

	/*
	 * Bootstrap success closes only that substage. P1 itself remains in
	 * BOOTSTRAP until the firmware-version gate succeeds.
	 */
	if (state->control.phase != HD60PRO_CONTROL_BOOTSTRAP ||
	    !state->bootstrap.completed ||
	    state->bootstrap.in_progress) {
		ret = -EPROTO;
		goto fail_control;
	}

	ret = sc0710_hd60pro_begin_firmware_version_locked(state);
	if (ret)
		goto fail_control;

	ret = sc0710_hd60pro_firmware_version_gate_locked(
		dev,
		&actual_major,
		&actual_minor);

	return sc0710_hd60pro_finish_firmware_version_locked(
		state,
		ret,
		actual_major,
		actual_minor);

fail_control:
	/*
	 * Bootstrap has succeeded but Linux could not coherently enter the
	 * version substage. Permanently fail the control plane rather than
	 * leaving a retryable/zombie BOOTSTRAP state.
	 */
	state->firmware_version.last_error = ret;

	transition_ret = sc0710_hd60pro_control_transition_locked(
		state,
		HD60PRO_CONTROL_FAILED,
		ret);
	if (transition_ret)
		return transition_ret;

	return ret;
}

static int
sc0710_hd60pro_validate_manual_context_locked(
	struct sc0710_dev *dev,
	struct sc0710_hd60pro_state *state)
{
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

	return 0;
}

static int
sc0710_hd60pro_validate_mailbox_snapshot_locked(
	struct sc0710_dev *dev,
	const struct sc0710_hd60pro_mailbox_snapshot *snapshot,
	u32 expected_mailbox_status)
{
	if (!(snapshot->pci_command & PCI_COMMAND_MEMORY))
		return -EIO;

	if (snapshot->pci_command & PCI_COMMAND_MASTER) {
		pci_clear_master(dev->pci);
		return -EIO;
	}

	if (snapshot->irq_status != 0)
		return -EBUSY;

	if (snapshot->mailbox_status != expected_mailbox_status)
		return -EBUSY;

	return 0;
}

static int
sc0710_hd60pro_validate_experiment(struct sc0710_dev *dev,
				   struct sc0710_hd60pro_state *state)
{
	int ret;

	ret = sc0710_hd60pro_validate_manual_context_locked(dev, state);
	if (ret)
		return ret;

	if (state->attempt_consumed)
		return -EALREADY;

	if (state->clear_attempt_consumed || state->i2c_attempt_consumed)
		return -EBUSY;

	ret = sc0710_hd60pro_take_mailbox_snapshot(dev, &state->signal.before);
	if (ret)
		return ret;

	state->signal.after = state->signal.before;
	state->signal.last_poll_status = state->signal.before.mailbox_status;

	return sc0710_hd60pro_validate_mailbox_snapshot_locked(
		dev, &state->signal.before, 0);
}

static bool sc0710_hd60pro_i2c_reg_is_whitelisted(u32 reg)
{
        switch (reg) {
        case HD60PRO_I2C_EXPERIMENT_REG_04:
        case HD60PRO_I2C_EXPERIMENT_REG_11:
        case HD60PRO_I2C_EXPERIMENT_REG_19:
        case HD60PRO_I2C_EXPERIMENT_REG_3D:
        case HD60PRO_I2C_EXPERIMENT_REG_73:
                return true;
        default:
                return false;
        }
}

static int
sc0710_hd60pro_validate_mailbox_request(
	const struct sc0710_hd60pro_mailbox_request *request)
{
	if (!request)
		return -EINVAL;

	/*
	 * Keep the active protocol surface restricted to transactions recovered
	 * from the Windows driver and explicitly selected for one-shot testing.
	 */
	switch (request->command) {
	case HD60PRO_CMD_SIGNAL_READ:
		if (request->word2 != BIT(HD60PRO_SIGNAL_HDMI_HPD) ||
		    request->word3 != 0 ||
		    request->response_offset != HD60PRO_BAR0_MAILBOX_RESPONSE0)
			return -EPERM;
		break;

	case HD60PRO_CMD_I2C_READ_REG8:
		if (request->word2 != HD60PRO_I2C_VIDEO_FRONTEND_ADDR_8BIT ||
		    !sc0710_hd60pro_i2c_reg_is_whitelisted(request->word3) ||
		    request->response_offset != HD60PRO_BAR0_MAILBOX_RESPONSE1)
			return -EPERM;
		break;

	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int
sc0710_hd60pro_mailbox_transaction_locked(
	struct sc0710_dev *dev,
	const struct sc0710_hd60pro_mailbox_request *request,
	const struct sc0710_hd60pro_mailbox_snapshot *before,
	struct sc0710_hd60pro_mailbox_result *result)
{
	u64 started_ns;
	u32 status = 0;
	u32 response = 0;
	unsigned int i;
	int ret;
	int snapshot_ret;

	if (!result)
		return -EINVAL;

	memset(result, 0, sizeof(*result));

	if (!dev || !request || !before)
		return -EINVAL;

	result->after = *before;
	result->last_poll_status = before->mailbox_status;

	ret = sc0710_hd60pro_validate_mailbox_request(request);
	if (ret)
		return ret;

	started_ns = ktime_get_ns();

	ret = sc0710_hd60pro_write_bar0(dev,
					HD60PRO_BAR0_MAILBOX_STATUS, 0);
	if (ret)
		goto out;

	ret = sc0710_hd60pro_write_bar0(dev,
					HD60PRO_BAR0_MAILBOX_OPCODE,
					request->command);
	if (ret)
		goto out;

	ret = sc0710_hd60pro_write_bar0(dev,
					HD60PRO_BAR0_MAILBOX_WORD2,
					request->word2);
	if (ret)
		goto out;

	ret = sc0710_hd60pro_write_bar0(dev,
					HD60PRO_BAR0_MAILBOX_RESPONSE0,
					request->word3);
	if (ret)
		goto out;

	if (request->response_offset == HD60PRO_BAR0_MAILBOX_RESPONSE1) {
		ret = sc0710_hd60pro_write_bar0(
			dev, HD60PRO_BAR0_MAILBOX_RESPONSE1, 0);
		if (ret)
			goto out;
	}

	ret = sc0710_hd60pro_write_bar0(dev,
					HD60PRO_BAR0_MAILBOX_TRIGGER,
					HD60PRO_MAILBOX_TRIGGER_VALUE);
	if (ret)
		goto out;

	/*
	 * writel() preserves MMIO write ordering. The first readl() also flushes
	 * posted writes before completion is evaluated.
	 */
	for (i = 0; i < HD60PRO_MAILBOX_POLL_COUNT; i++) {
		ret = sc0710_hd60pro_read_bar0(
			dev, HD60PRO_BAR0_MAILBOX_STATUS, &status);
		if (ret)
			goto out;

		result->polls = i + 1;
		result->last_poll_status = status;

		if (status & HD60PRO_MAILBOX_STATUS_COMPLETE) {
			ret = sc0710_hd60pro_read_bar0(
				dev, request->response_offset, &response);
			if (ret)
				goto out;

			result->completed = true;
			result->response = response;
			ret = 0;
			goto out;
		}

		usleep_range(HD60PRO_MAILBOX_POLL_MIN_US,
			     HD60PRO_MAILBOX_POLL_MAX_US);
	}

	ret = -ETIMEDOUT;

out:
	result->elapsed_ns = ktime_get_ns() - started_ns;

	/* Read-only after-snapshot; no clear, ACK, retry or reset is attempted. */
	snapshot_ret = sc0710_hd60pro_take_mailbox_snapshot(dev, &result->after);
	if (snapshot_ret) {
		if (!ret)
			ret = snapshot_ret;
	} else {
		result->after_valid = true;
		result->late_completion =
			!result->completed &&
			!!(result->after.mailbox_status &
			   HD60PRO_MAILBOX_STATUS_COMPLETE);

		if (!result->completed) {
			if (!sc0710_hd60pro_read_bar0(
				    dev, request->response_offset,
				    &response))
				result->response = response;
		}

		if (result->after.pci_command & PCI_COMMAND_MASTER) {
			pci_clear_master(dev->pci);
			ret = -EIO;
		}

		if (!ret && result->after.irq_status != 0)
			ret = -EIO;
	}

	return ret;
}

static int
sc0710_hd60pro_signal_read_poll_locked(
	struct sc0710_dev *dev,
	u8 signal_index,
	const struct sc0710_hd60pro_mailbox_snapshot *before,
	struct sc0710_hd60pro_mailbox_result *result)
{
	const struct sc0710_hd60pro_mailbox_request request = {
		.command = HD60PRO_CMD_SIGNAL_READ,
		.word2 = BIT(HD60PRO_SIGNAL_HDMI_HPD),
		.word3 = 0,
		.response_offset = HD60PRO_BAR0_MAILBOX_RESPONSE0,
	};

	/* Keep the semantic wrapper restricted to the signal proven in G1-A. */
	if (signal_index != HD60PRO_SIGNAL_HDMI_HPD)
		return -EPERM;

	return sc0710_hd60pro_mailbox_transaction_locked(
		dev, &request, before, result);
}

static int
sc0710_hd60pro_run_signal_read_experiment(struct sc0710_dev *dev)
{
	struct sc0710_hd60pro_mailbox_result result;
	struct sc0710_hd60pro_state *state;
	int ret;

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

	ret = sc0710_hd60pro_signal_read_poll_locked(
		dev,
		HD60PRO_SIGNAL_HDMI_HPD,
		&state->signal.before,
		&result);

	state->signal.completed = result.completed;
	state->signal.signal_value_valid = result.completed;
	state->signal.late_completion = result.late_completion;
	state->signal.signal_value = result.completed &&
		!!(result.response & BIT(HD60PRO_SIGNAL_HDMI_HPD));
	state->signal.polls = result.polls;
	state->signal.last_poll_status = result.last_poll_status;
	state->signal.response = result.response;
	state->signal.elapsed_ns = result.elapsed_ns;
	state->signal.after = result.after;

out:
	state->signal.last_error = ret;
	state->in_progress = false;
out_preserve_result:
	mutex_unlock(&state->mailbox_lock);
	return ret;
}

static int
sc0710_hd60pro_validate_i2c_experiment(
	struct sc0710_dev *dev,
	struct sc0710_hd60pro_state *state)
{
	int ret;

	ret = sc0710_hd60pro_validate_manual_context_locked(dev, state);
	if (ret)
		return ret;


	if (!sc0710_hd60pro_i2c_reg_is_whitelisted(state->i2c.reg))
		return -EPERM;

	if (state->i2c_attempt_consumed)
		return -EALREADY;
	if (state->attempt_consumed || state->clear_attempt_consumed)
		return -EBUSY;

	ret = sc0710_hd60pro_take_mailbox_snapshot(dev, &state->i2c.before);
	if (ret)
		return ret;

	state->i2c.after = state->i2c.before;
	state->i2c.last_poll_status = state->i2c.before.mailbox_status;

	return sc0710_hd60pro_validate_mailbox_snapshot_locked(
		dev, &state->i2c.before, 0);
}

static int
sc0710_hd60pro_run_i2c_read_experiment(struct sc0710_dev *dev, u8 reg)
{
	const struct sc0710_hd60pro_mailbox_request request = {
		.command = HD60PRO_CMD_I2C_READ_REG8,
		.word2 = HD60PRO_I2C_VIDEO_FRONTEND_ADDR_8BIT,
		.word3 = reg,
		.response_offset = HD60PRO_BAR0_MAILBOX_RESPONSE1,
	};
	struct sc0710_hd60pro_mailbox_result result;
	struct sc0710_hd60pro_state *state;
	int ret;

	if (!dev)
		return -ENODEV;

	state = &dev->hd60pro_state;

	mutex_lock(&state->mailbox_lock);
	if (state->i2c_attempt_consumed) {
		ret = -EALREADY;
		goto out_preserve_result;
	}

	sc0710_hd60pro_reset_i2c_result(state);
	state->i2c.reg = reg;

	ret = sc0710_hd60pro_validate_i2c_experiment(dev, state);
	if (ret)
		goto out;

	state->i2c_attempt_consumed = true;
	state->i2c_in_progress = true;

	ret = sc0710_hd60pro_mailbox_transaction_locked(
		dev, &request, &state->i2c.before, &result);

	state->i2c.completed = result.completed;
	state->i2c.value_valid = result.completed;
	state->i2c.late_completion = result.late_completion;
	state->i2c.value = result.response & 0xff;
	state->i2c.polls = result.polls;
	state->i2c.last_poll_status = result.last_poll_status;
	state->i2c.response = result.response;
	state->i2c.elapsed_ns = result.elapsed_ns;
	state->i2c.after = result.after;

out:
	state->i2c.last_error = ret;
	state->i2c_in_progress = false;
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

	ret = sc0710_hd60pro_validate_manual_context_locked(dev, state);
	if (ret)
		return ret;

	if (state->clear_attempt_consumed)
		return -EALREADY;

	if (state->attempt_consumed || state->i2c_attempt_consumed)
		return -EBUSY;

	ret = sc0710_hd60pro_take_mailbox_snapshot(
		dev, &state->clear.before);
	if (ret)
		return ret;

	state->clear.after = state->clear.before;

	/* Clear only the exact stale completion state observed in G1-A. */
	return sc0710_hd60pro_validate_mailbox_snapshot_locked(
		dev,
		&state->clear.before,
		HD60PRO_MAILBOX_STATUS_COMPLETE);
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
	state->clear.elapsed_ns = ktime_get_ns() - started_ns;

	snapshot_ret = sc0710_hd60pro_take_mailbox_snapshot(
		dev, &state->clear.after);
	if (snapshot_ret) {
		if (!ret)
			ret = snapshot_ret;
	} else {
		if (state->clear.after.pci_command & PCI_COMMAND_MASTER) {
			pci_clear_master(dev->pci);
			ret = -EIO;
		}

		if (!ret && (state->clear.after.mailbox_status != 0 ||
		             state->clear.after.irq_status != 0))
			ret = -EIO;
	}

	if (!ret)
		state->clear.completed = true;

out:
	state->clear.last_error = ret;
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
		   state->clear.completed);
	seq_printf(s, "clear_last_error=%d\n",
		   state->clear.last_error);
	seq_printf(s, "clear_elapsed_us=%llu\n",
		   (unsigned long long)(state->clear.elapsed_ns / 1000));
	seq_printf(s, "pci_command_before=0x%04x\n",
		   state->clear.before.pci_command);
	seq_printf(s, "mailbox_status_before=0x%08x\n",
		   state->clear.before.mailbox_status);
	seq_printf(s, "irq_status_before=0x%08x\n",
		   state->clear.before.irq_status);
	seq_printf(s, "irq_tag_before=0x%08x\n",
		   state->clear.before.irq_tag);
	seq_printf(s, "pci_command_after=0x%04x\n",
		   state->clear.after.pci_command);
	seq_printf(s, "mailbox_status_after=0x%08x\n",
		   state->clear.after.mailbox_status);
	seq_printf(s, "irq_status_after=0x%08x\n",
		   state->clear.after.irq_status);
	seq_printf(s, "irq_tag_after=0x%08x\n",
		   state->clear.after.irq_tag);

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
sc0710_hd60pro_experimental_i2c_read_show(struct seq_file *s, void *unused)
{
	struct sc0710_dev *dev = s->private;
	struct sc0710_hd60pro_state *state = &dev->hd60pro_state;

	mutex_lock(&state->mailbox_lock);

	seq_printf(s, "enabled=%u\n",
		   READ_ONCE(hd60pro_experimental_mailbox));
	seq_printf(s, "attempt_consumed=%u\n", state->i2c_attempt_consumed);
	seq_printf(s, "in_progress=%u\n", state->i2c_in_progress);
	seq_printf(s, "completed=%u\n", state->i2c.completed);
	seq_printf(s, "late_completion=%u\n", state->i2c.late_completion);
	seq_printf(s, "last_error=%d\n", state->i2c.last_error);
	seq_printf(s, "address_8bit=0x%02x\n", state->i2c.address_8bit);
	seq_printf(s, "address_7bit=0x%02x\n", state->i2c.address_8bit >> 1);
	seq_printf(s, "register=0x%02x\n", state->i2c.reg);
	seq_printf(s, "register_whitelist=0x%02x,0x%02x,0x%02x,0x%02x,0x%02x\n",
		   HD60PRO_I2C_EXPERIMENT_REG_04,
		   HD60PRO_I2C_EXPERIMENT_REG_11,
		   HD60PRO_I2C_EXPERIMENT_REG_19,
		   HD60PRO_I2C_EXPERIMENT_REG_3D,
		   HD60PRO_I2C_EXPERIMENT_REG_73);
	seq_printf(s, "value_valid=%u\n", state->i2c.value_valid);
	seq_printf(s, "value=0x%02x\n", state->i2c.value);
	seq_printf(s, "polls=%u\n", state->i2c.polls);
	seq_printf(s, "elapsed_us=%llu\n",
		   (unsigned long long)(state->i2c.elapsed_ns / 1000));
	seq_printf(s, "last_poll_status=0x%08x\n",
		   state->i2c.last_poll_status);
	seq_printf(s, "response_word4=0x%08x\n", state->i2c.response);
	seq_printf(s, "pci_command_before=0x%04x\n",
		   state->i2c.before.pci_command);
	seq_printf(s, "mailbox_status_before=0x%08x\n",
		   state->i2c.before.mailbox_status);
	seq_printf(s, "irq_status_before=0x%08x\n",
		   state->i2c.before.irq_status);
	seq_printf(s, "irq_tag_before=0x%08x\n",
		   state->i2c.before.irq_tag);
	seq_printf(s, "pci_command_after=0x%04x\n",
		   state->i2c.after.pci_command);
	seq_printf(s, "mailbox_status_after=0x%08x\n",
		   state->i2c.after.mailbox_status);
	seq_printf(s, "irq_status_after=0x%08x\n",
		   state->i2c.after.irq_status);
	seq_printf(s, "irq_tag_after=0x%08x\n",
		   state->i2c.after.irq_tag);

	mutex_unlock(&state->mailbox_lock);
	return 0;
}

static int
sc0710_hd60pro_experimental_i2c_read_open(struct inode *inode,
					  struct file *file)
{
	return single_open(file,
			   sc0710_hd60pro_experimental_i2c_read_show,
			   inode->i_private);
}

static ssize_t
sc0710_hd60pro_experimental_i2c_read_write(struct file *file,
					   const char __user *user_buf,
					   size_t count, loff_t *ppos)
{
	struct seq_file *seq = file->private_data;
	struct sc0710_dev *dev = seq->private;
	char buf[16];
	char *command;
	u8 reg;
	int ret;

	if (count == 0 || count >= sizeof(buf))
		return -EINVAL;

	if (copy_from_user(buf, user_buf, count))
		return -EFAULT;

	buf[count] = '\0';
	command = strim(buf);

	ret = kstrtou8(command, 0, &reg);
	if (ret)
		return ret;

	if (!sc0710_hd60pro_i2c_reg_is_whitelisted(reg))
		return -EPERM;

	ret = sc0710_hd60pro_run_i2c_read_experiment(dev, reg);
	if (ret)
		return ret;

	return count;
}

static const struct file_operations
sc0710_hd60pro_experimental_i2c_read_fops = {
	.owner		= THIS_MODULE,
	.open		= sc0710_hd60pro_experimental_i2c_read_open,
	.read		= seq_read,
	.write		= sc0710_hd60pro_experimental_i2c_read_write,
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
	seq_printf(s, "completed=%u\n", state->signal.completed);
	seq_printf(s, "late_completion=%u\n", state->signal.late_completion);
	seq_printf(s, "last_error=%d\n", state->signal.last_error);
	seq_printf(s, "signal_index=%u\n", state->signal.signal_index);
	seq_puts(s, "signal_name=HDMI_HPD\n");
	seq_printf(s, "signal_value_valid=%u\n", state->signal.signal_value_valid);
	seq_printf(s, "signal_value=%u\n", state->signal.signal_value);
	seq_printf(s, "polls=%u\n", state->signal.polls);
	seq_printf(s, "elapsed_us=%llu\n",
		   (unsigned long long)(state->signal.elapsed_ns / 1000));
	seq_printf(s, "last_poll_status=0x%08x\n",
		   state->signal.last_poll_status);
	seq_printf(s, "response=0x%08x\n", state->signal.response);
	seq_printf(s, "pci_command_before=0x%04x\n",
		   state->signal.before.pci_command);
	seq_printf(s, "mailbox_status_before=0x%08x\n",
		   state->signal.before.mailbox_status);
	seq_printf(s, "irq_status_before=0x%08x\n",
		   state->signal.before.irq_status);
	seq_printf(s, "irq_tag_before=0x%08x\n",
		   state->signal.before.irq_tag);
	seq_printf(s, "pci_command_after=0x%04x\n",
		   state->signal.after.pci_command);
	seq_printf(s, "mailbox_status_after=0x%08x\n",
		   state->signal.after.mailbox_status);
	seq_printf(s, "irq_status_after=0x%08x\n",
		   state->signal.after.irq_status);
	seq_printf(s, "irq_tag_after=0x%08x\n",
		   state->signal.after.irq_tag);

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

	seq_printf(s, "control_phase=%s\n",
			   sc0710_hd60pro_control_phase_name(
				   dev->hd60pro_state.control.phase));
	seq_printf(s, "control_last_error=%d\n",
			   dev->hd60pro_state.control.last_error);

	seq_printf(s, "bootstrap_attempt_consumed=%u\n",
		   dev->hd60pro_state.bootstrap.attempt_consumed);
	seq_printf(s, "bootstrap_in_progress=%u\n",
		   dev->hd60pro_state.bootstrap.in_progress);
	seq_printf(s, "bootstrap_completed=%u\n",
		   dev->hd60pro_state.bootstrap.completed);
	seq_printf(s, "bootstrap_mailbox_completed=%u\n",
		   dev->hd60pro_state.bootstrap.mailbox_completed);
	seq_printf(s, "bootstrap_late_completion=%u\n",
		   dev->hd60pro_state.bootstrap.late_completion);
	seq_printf(s, "bootstrap_last_error=%d\n",
		   dev->hd60pro_state.bootstrap.last_error);

	seq_printf(s, "bootstrap_before_valid=%u\n",
		   dev->hd60pro_state.bootstrap.before_valid);
	seq_printf(s, "bootstrap_before_pci_command=0x%04x\n",
		   dev->hd60pro_state.bootstrap.before.pci_command);
	seq_printf(s, "bootstrap_before_mailbox_status=0x%08x\n",
		   dev->hd60pro_state.bootstrap.before.mailbox_status);
	seq_printf(s, "bootstrap_before_irq_status=0x%08x\n",
		   dev->hd60pro_state.bootstrap.before.irq_status);
	seq_printf(s, "bootstrap_before_irq_tag=0x%08x\n",
		   dev->hd60pro_state.bootstrap.before.irq_tag);

	seq_printf(s, "bootstrap_polls=%u\n",
		   dev->hd60pro_state.bootstrap.polls);
	seq_printf(s, "bootstrap_last_poll_status=0x%08x\n",
		   dev->hd60pro_state.bootstrap.last_poll_status);
	seq_printf(s, "bootstrap_elapsed_us=%llu\n",
		   (unsigned long long)
		   (dev->hd60pro_state.bootstrap.elapsed_ns / 1000));

	seq_printf(s, "bootstrap_after_valid=%u\n",
		   dev->hd60pro_state.bootstrap.after_valid);
	seq_printf(s, "bootstrap_after_pci_command=0x%04x\n",
		   dev->hd60pro_state.bootstrap.after.pci_command);
	seq_printf(s, "bootstrap_after_mailbox_status=0x%08x\n",
		   dev->hd60pro_state.bootstrap.after.mailbox_status);
	seq_printf(s, "bootstrap_after_irq_status=0x%08x\n",
		   dev->hd60pro_state.bootstrap.after.irq_status);
	seq_printf(s, "bootstrap_after_irq_tag=0x%08x\n",
		   dev->hd60pro_state.bootstrap.after.irq_tag);


	seq_printf(s, "firmware_version_expected=%u.%u\n",
		   HD60PRO_FW_VERSION_EXPECTED_MAJOR,
		   HD60PRO_FW_VERSION_EXPECTED_MINOR);
	seq_printf(s, "firmware_version_attempted=%u\n",
		   dev->hd60pro_state.firmware_version.attempted);
	seq_printf(s, "firmware_version_in_progress=%u\n",
		   dev->hd60pro_state.firmware_version.in_progress);
	seq_printf(s, "firmware_version_completed=%u\n",
		   dev->hd60pro_state.firmware_version.completed);
	seq_printf(s, "firmware_version_actual=%u.%u\n",
		   dev->hd60pro_state.firmware_version.actual_major,
		   dev->hd60pro_state.firmware_version.actual_minor);
	seq_printf(s, "firmware_version_last_error=%d\n",
		   dev->hd60pro_state.firmware_version.last_error);

	if (!dev->observational_only) {
		seq_puts(s, "mode=active-control\n");
		seq_puts(s, "active_bringup=p1-bootstrap-fw-version-one-shot\n");
		seq_puts(s, "mmio_writes=bootstrap-and-fw-version-only\n");
		seq_puts(s, "mailbox_writes=bootstrap-and-fw-version-only\n");
		seq_puts(s, "i2c_reads=disabled\n");
	} else if (READ_ONCE(hd60pro_experimental_mailbox)) {
		seq_puts(s, "mode=observational-with-manual-mailbox-opt-in\n");
		seq_puts(s, "mmio_writes=experimental-manual-only\n");
		seq_puts(s, "mailbox_writes=experimental-manual-only\n");
		seq_puts(s, "i2c_reads=experimental-manual-only\n");
	} else {
		seq_puts(s, "mode=observational-only\n");
		seq_puts(s, "mmio_writes=disabled\n");
		seq_puts(s, "mailbox_writes=disabled\n");
		seq_puts(s, "i2c_reads=disabled\n");
	}
	seq_puts(s, "i2c_writes=disabled\n");
	seq_puts(s, "mmio_reads=whitelist-only\n");
	seq_puts(s, "mailbox_protocol=reverse-engineered\n");
	seq_printf(s, "experimental_mailbox_opt_in=%u\n",
		   READ_ONCE(hd60pro_experimental_mailbox));

	seq_printf(s, "active_control_opt_in=%u\n",
		   READ_ONCE(hd60pro_active_control));

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

	dev->observational_only = !READ_ONCE(hd60pro_active_control);

	/*
	 * The HD60 Pro may retain DMA state across driver sessions.
	 * Clear bus mastering before any allocation or early-return path.
	 */
	pci_clear_master(pci_dev);

	mutex_init(&dev->hd60pro_state.mailbox_lock);
	sc0710_hd60pro_reset_control_state(&dev->hd60pro_state);
	sc0710_hd60pro_reset_bootstrap_state(&dev->hd60pro_state);
	sc0710_hd60pro_reset_firmware_version_state(&dev->hd60pro_state);
	sc0710_hd60pro_reset_experiment_result(&dev->hd60pro_state);
	sc0710_hd60pro_reset_clear_result(&dev->hd60pro_state);
	sc0710_hd60pro_reset_i2c_result(&dev->hd60pro_state);

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
				"experimental_i2c_read",
				0600,
				dev->hd60pro_debugfs_dir,
				dev,
				&sc0710_hd60pro_experimental_i2c_read_fops);
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

	pr_info("%s: HD60 Pro attached in %s mode\n",
		dev->name,
		dev->observational_only ? "observational-only" : "active-control");
	pr_info("%s: BAR0 size=0x%llx, BAR5 size=0x%llx\n",
		dev->name,
		(unsigned long long)pci_resource_len(pci_dev, 0),
		(unsigned long long)pci_resource_len(pci_dev, 5));
	pr_info("%s: whitelist MMIO reads enabled; "
		"bus mastering, IRQ, DMA, I2C writes and media nodes disabled\n",
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

	pr_info("%s: HD60 Pro %s backend detached\n",
		dev->name,
		dev->observational_only ? "observational" : "active-control");
}

static int
sc0710_hd60pro_active_bringup_p1(struct sc0710_dev *dev)
{
	struct sc0710_hd60pro_state *state;
	int ret;

	if (!dev || !dev->pci)
		return -ENODEV;

	state = &dev->hd60pro_state;

	/*
	 * This is the only runtime admission point for the one-shot P1 sequence.
	 * The orchestrator owns the settling delay, read-only preflight,
	 * one-shot consumption, state transitions, exact Windows-derived MMIO
	 * sequence and bounded completion polling. Persistent diagnostics are
	 * emitted below only after PCI bus mastering has been forced off.
	 *
	 * PCI bus mastering, Linux IRQ installation, DMA, frontend control and
	 * capture remain disabled.
	 */
	mutex_lock(&state->mailbox_lock);
	ret = sc0710_hd60pro_p1_once_locked(dev, state);

	/*
	 * Preserve the fail-closed bus-master invariant on every exit,
	 * including admission failure and committed P1 failure.
	 */
	pci_clear_master(dev->pci);
	sc0710_hd60pro_log_p1_result_locked(dev, state, ret);
	mutex_unlock(&state->mailbox_lock);

	return ret;
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
        .uses_legacy_xdma_pipeline = false,
        .exposes_passive_video     = true,
	.init			= sc0710_hd60pro_probe,
	.fini			= sc0710_hd60pro_remove,
	.active_bringup		= sc0710_hd60pro_active_bringup_p1,
	.capture_prepare	= sc0710_hd60pro_capture_unsupported,
	.capture_start		= sc0710_hd60pro_capture_unsupported,
	.capture_stop		= sc0710_hd60pro_capture_stop,
	.capture_service	= sc0710_hd60pro_capture_service,
};
