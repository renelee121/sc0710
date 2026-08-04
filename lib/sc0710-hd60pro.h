/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _SC0710_HD60PRO_H
#define _SC0710_HD60PRO_H

struct sc0710_dev;
struct sc0710_hw_ops;

/*
 * BAR0 mailbox and interrupt register map reconstructed from
 * e60MZ0380.X64.SYS.
 *
 * These definitions document the Windows driver protocol.
 * Their presence does not mean MMIO writes are enabled.
 */
enum hd60pro_bar0_register {
	HD60PRO_BAR0_MAILBOX_TRIGGER	= 0x0000,
	HD60PRO_BAR0_MAILBOX_OPCODE	= 0x0004,
	HD60PRO_BAR0_MAILBOX_WORD2	= 0x0008,
	HD60PRO_BAR0_MAILBOX_RESPONSE0	= 0x000c,
	HD60PRO_BAR0_MAILBOX_RESPONSE1	= 0x0010,
	HD60PRO_BAR0_MAILBOX_STATUS	= 0x002c,
	HD60PRO_BAR0_IRQ_STATUS		= 0x0030,
	HD60PRO_BAR0_IRQ_TAG		= 0x0040,
	HD60PRO_BAR0_IRQ_ACK_BYTE	= 0x0050,
};

/*
 * The second Windows MMIO resource strongly correlates with PCI BAR5
 * on the HD60 Pro 12ab:0380 / 1cfa:0006.
 */
enum hd60pro_bar5_register {
	HD60PRO_BAR5_IRQ_ACK		= 0x00dc,
};

enum hd60pro_mailbox_opcode {
	HD60PRO_CMD_SIGNAL_READ		= 0x14,
	HD60PRO_CMD_SIGNAL_WRITE	= 0x15,
	HD60PRO_CMD_SIGNAL_CONFIG	= 0x17,
	HD60PRO_CMD_I2C_READ_REG8	= 0x1a,
	HD60PRO_CMD_I2C_WRITE_REG8	= 0x1b,
	HD60PRO_CMD_MCU_I2C_TRANSFER	= 0x20,
};

enum hd60pro_signal_index {
	HD60PRO_SIGNAL_HDMI_HPD			= 1,
	HD60PRO_SIGNAL_FRONTEND_CTRL_ALT	= 2,
	HD60PRO_SIGNAL_FRONTEND_CTRL		= 8,
	HD60PRO_SIGNAL_FRONTEND_RESET_N		= 9,
};

enum hd60pro_mcu_i2c_direction {
	HD60PRO_MCU_I2C_WRITE = 0,
	HD60PRO_MCU_I2C_READ  = 1,
};

/*
 * Values observed in the Windows driver.
 * They remain unused while the Linux backend is observational-only.
 */
#define HD60PRO_MAILBOX_TRIGGER_VALUE		0x00000800U
#define HD60PRO_IRQ_ACK_TRIGGER_VALUE		0x00000400U

#define HD60PRO_MAILBOX_STATUS_COMPLETE		0x00000001U
#define HD60PRO_IRQ_STATUS_MAILBOX_COMPLETE	0x00000800U
#define HD60PRO_IRQ_TAG_INDEX_MASK		0x00000007U

#define HD60PRO_BAR5_IRQ_ACK_VALUE		0x00000002U

#define HD60PRO_I2C_VIDEO_FRONTEND_ADDR_8BIT	0x90
#define HD60PRO_I2C_VIDEO_FRONTEND_ADDR_7BIT	0x48

int sc0710_hd60pro_probe(struct sc0710_dev *dev);
void sc0710_hd60pro_remove(struct sc0710_dev *dev);

extern const struct sc0710_hw_ops sc0710_hd60pro_ops;

#define HD60PRO_MCU_I2C_MAX_LENGTH		32U
#define HD60PRO_MCU_I2C_DIRECTION_SHIFT	8U
#define HD60PRO_MCU_I2C_LENGTH_SHIFT		16U

#endif /* _SC0710_HD60PRO_H */