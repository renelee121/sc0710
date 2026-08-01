/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _SC0710_HD60PRO_H
#define _SC0710_HD60PRO_H

struct sc0710_dev;

int sc0710_hd60pro_probe(struct sc0710_dev *dev);
void sc0710_hd60pro_remove(struct sc0710_dev *dev);

enum hd60pro_mailbox_opcode {
    HD60PRO_CMD_SIGNAL_READ        = 0x14,
    HD60PRO_CMD_SIGNAL_WRITE       = 0x15,
    HD60PRO_CMD_SIGNAL_CONFIG      = 0x17,
    HD60PRO_CMD_I2C_READ_REG8      = 0x1a,
    HD60PRO_CMD_I2C_WRITE_REG8     = 0x1b,
    HD60PRO_CMD_MCU_I2C_TRANSFER   = 0x20,
};

enum hd60pro_signal_index {
    HD60PRO_SIGNAL_HDMI_HPD          = 1,
    HD60PRO_SIGNAL_FRONTEND_CTRL_ALT = 2,
    HD60PRO_SIGNAL_FRONTEND_CTRL     = 8,
    HD60PRO_SIGNAL_FRONTEND_RESET_N  = 9,
};

#define HD60PRO_I2C_VIDEO_FRONTEND_ADDR_8BIT  0x90
#define HD60PRO_I2C_VIDEO_FRONTEND_ADDR_7BIT  0x48

#endif /* _SC0710_HD60PRO_H */