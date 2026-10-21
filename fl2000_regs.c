// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2019, Artem Mygaiev
 * (C) Copyright 2026, Samuel
 */

#include <linux/delay.h>
#include <linux/slab.h>

#include "fl2000.h"

#define CONTROL_MSG_READ  64
#define CONTROL_MSG_WRITE 65

#define FL2000_HW_RST_MDELAY 10

/* Timeout in us for I2C read/write operations, HW gives up internally after 256 ms */
#define I2C_RDWR_INTERVAL (200)
#define I2C_RDWR_TIMEOUT  (300 * 1000)

static bool fl2000_reg_precious(struct device *dev, unsigned int reg)
{
	return FL2000_REG_PRECIOUS(reg);
}

static bool fl2000_reg_volatile(struct device *dev, unsigned int reg)
{
	return FL2000_REG_VOLATILE(reg);
}

/* Protected by internal regmap mutex */
static int fl2000_reg_read(void *context, unsigned int reg, unsigned int *val)
{
	struct fl2000_device *fl = context;
	__le32 *data;
	int ret;

	data = kmalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	ret = usb_control_msg(fl->udev, usb_rcvctrlpipe(fl->udev, 0), CONTROL_MSG_READ,
			      USB_DIR_IN | USB_TYPE_VENDOR, 0, (u16)reg, data, sizeof(*data),
			      USB_CTRL_GET_TIMEOUT);
	if (ret == sizeof(*data)) {
		*val = le32_to_cpu(*data);
		ret = 0;
	} else {
		dev_dbg(&fl->intf->dev, "reg read 0x%04x failed: %d\n", reg, ret);
		if (ret >= 0)
			ret = -EIO;
	}

	kfree(data);
	return ret;
}

/* Protected by internal regmap mutex */
static int fl2000_reg_write(void *context, unsigned int reg, unsigned int val)
{
	struct fl2000_device *fl = context;
	__le32 *data;
	int ret;

	data = kmalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	*data = cpu_to_le32(val);

	ret = usb_control_msg(fl->udev, usb_sndctrlpipe(fl->udev, 0), CONTROL_MSG_WRITE,
			      USB_DIR_OUT | USB_TYPE_VENDOR, 0, (u16)reg, data, sizeof(*data),
			      USB_CTRL_SET_TIMEOUT);
	if (ret == sizeof(*data))
		ret = 0;
	else if (ret >= 0)
		ret = -EIO;

	kfree(data);
	return ret;
}

/* No register cache: SW reset clears HW registers behind the cache's back, and every access is
 * cheap enough compared to a frame transfer
 */
static const struct regmap_config fl2000_regmap_config = {
	.val_bits = 32,
	.reg_bits = 16,
	.reg_stride = 4,
	.max_register = 0xFFFF,

	.cache_type = REGCACHE_NONE,

	.precious_reg = fl2000_reg_precious,
	.volatile_reg = fl2000_reg_volatile,

	.reg_read = fl2000_reg_read,
	.reg_write = fl2000_reg_write,
};

int fl2000_regmap_init(struct fl2000_device *fl)
{
	fl->regmap = devm_regmap_init(&fl->intf->dev, NULL, fl, &fl2000_regmap_config);
	if (IS_ERR(fl->regmap))
		return PTR_ERR(fl->regmap);

	return 0;
}

int fl2000_set_pll(struct fl2000_device *fl, const struct fl2000_pll *pll)
{
	struct regmap *regmap = fl->regmap;
	union fl2000_vga_pll_reg pll_reg = { .val = 0 };
	union fl2000_vga_ctrl_reg_aclk aclk = { .val = 0 };
	u32 mask = 0;

	pll_reg.prescaler = pll->prescaler;
	pll_reg.multiplier = pll->multiplier;
	pll_reg.divisor = pll->divisor;
	pll_reg.function = pll->function;
	regmap_write(regmap, FL2000_VGA_PLL_REG, pll_reg.val);

	aclk.force_pll_up = true;
	fl2000_add_bitmask(mask, union fl2000_vga_ctrl_reg_aclk, force_pll_up);
	aclk.force_vga_connect = true;
	fl2000_add_bitmask(mask, union fl2000_vga_ctrl_reg_aclk, force_vga_connect);
	regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_ACLK, mask, aclk.val);

	return 0;
}

int fl2000_set_timings(struct fl2000_device *fl, const struct fl2000_timings *timings)
{
	struct regmap *regmap = fl->regmap;
	union fl2000_vga_hsync_reg1 hsync1 = { .val = 0 };
	union fl2000_vga_hsync_reg2 hsync2 = { .val = 0 };
	union fl2000_vga_vsync_reg1 vsync1 = { .val = 0 };
	union fl2000_vga_vsync_reg2 vsync2 = { .val = 0 };

	hsync1.hactive = timings->hactive;
	hsync1.htotal = timings->htotal;
	regmap_write(regmap, FL2000_VGA_HSYNC_REG1, hsync1.val);

	hsync2.hsync_width = timings->hsync_width;
	hsync2.hstart = timings->hstart;
	regmap_write(regmap, FL2000_VGA_HSYNC_REG2, hsync2.val);

	vsync1.vactive = timings->vactive;
	vsync1.vtotal = timings->vtotal;
	regmap_write(regmap, FL2000_VGA_VSYNC_REG1, vsync1.val);

	vsync2.vsync_width = timings->vsync_width;
	vsync2.vstart = timings->vstart;
	vsync2.start_latency = timings->vstart;
	regmap_write(regmap, FL2000_VGA_VSYNC_REG2, vsync2.val);

	return 0;
}

int fl2000_set_pixfmt(struct fl2000_device *fl, u32 bytes_pix)
{
	struct regmap *regmap = fl->regmap;
	union fl2000_vga_cntrl_reg_pxclk pxclk = { .val = 0 };
	u32 mask = 0;

	pxclk.dac_output_en = false;
	fl2000_add_bitmask(mask, union fl2000_vga_cntrl_reg_pxclk, dac_output_en);
	pxclk.drop_cnt = false;
	fl2000_add_bitmask(mask, union fl2000_vga_cntrl_reg_pxclk, drop_cnt);
	pxclk.vga565_mode = (bytes_pix == 2);
	fl2000_add_bitmask(mask, union fl2000_vga_cntrl_reg_pxclk, vga565_mode);
	pxclk.vga332_mode = false;
	fl2000_add_bitmask(mask, union fl2000_vga_cntrl_reg_pxclk, vga332_mode);
	pxclk.vga555_mode = false;
	fl2000_add_bitmask(mask, union fl2000_vga_cntrl_reg_pxclk, vga555_mode);
	pxclk.vga_compress = false;
	fl2000_add_bitmask(mask, union fl2000_vga_cntrl_reg_pxclk, vga_compress);
	pxclk.dac_output_en = true;
	fl2000_add_bitmask(mask, union fl2000_vga_cntrl_reg_pxclk, dac_output_en);
	pxclk.clear_watermark = true;
	fl2000_add_bitmask(mask, union fl2000_vga_cntrl_reg_pxclk, clear_watermark);
	regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_PXCLK, mask, pxclk.val);

	return 0;
}

int fl2000_set_transfers(struct fl2000_device *fl)
{
	struct regmap *regmap = fl->regmap;
	union fl2000_vga_ctrl_reg_aclk aclk = { .val = 0 };
	union fl2000_vga_isoch_reg isoch = { .val = 0 };
	u32 mask;

	mask = 0;
	aclk.use_pkt_pending = false;
	fl2000_add_bitmask(mask, union fl2000_vga_ctrl_reg_aclk, use_pkt_pending);
	aclk.use_zero_td = false;
	fl2000_add_bitmask(mask, union fl2000_vga_ctrl_reg_aclk, use_zero_td);
	aclk.use_zero_pkt_len = true;
	fl2000_add_bitmask(mask, union fl2000_vga_ctrl_reg_aclk, use_zero_pkt_len);
	aclk.vga_err_int_en = true;
	regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_ACLK, mask, aclk.val);

	mask = 0;
	isoch.mframe_cnt = 0;
	fl2000_add_bitmask(mask, union fl2000_vga_isoch_reg, mframe_cnt);
	regmap_write_bits(regmap, FL2000_VGA_ISOCH_REG, mask, isoch.val);

	return 0;
}

int fl2000_reset(struct fl2000_device *fl)
{
	struct regmap *regmap = fl->regmap;
	union fl2000_rst_cntrl_reg rst_cntrl_reg = { .val = 0 };
	u32 mask = 0;

	rst_cntrl_reg.sw_reset = true;
	fl2000_add_bitmask(mask, union fl2000_rst_cntrl_reg, sw_reset);
	regmap_write_bits(regmap, FL2000_RST_CTRL_REG, mask, rst_cntrl_reg.val);

	msleep(FL2000_HW_RST_MDELAY);

	return 0;
}

int fl2000_afe_magic(struct fl2000_device *fl)
{
	struct regmap *regmap = fl->regmap;
	union fl2000_usb_lpm_reg usb_lpm_reg = { .val = 0 };
	u32 mask = 0;

	usb_lpm_reg.magic = true;
	fl2000_add_bitmask(mask, union fl2000_usb_lpm_reg, magic);
	regmap_write_bits(regmap, FL2000_USB_LPM_REG, mask, usb_lpm_reg.val);

	return 0;
}

int fl2000_usb_magic(struct fl2000_device *fl)
{
	struct regmap *regmap = fl->regmap;
	union fl2000_vga_i2c_sc_reg vga_i2c_sc_reg = { .val = 0 };
	union fl2000_vga_ctrl_reg_3 vga_ctrl_reg_3 = { .val = 0 };
	union fl2000_usb_lpm_reg usb_lpm_reg = { .val = 0 };
	union fl2000_usb_ctrl_reg usb_ctrl_reg = { .val = 0 };
	u32 mask;

	mask = 0;
	vga_i2c_sc_reg.monitor_detect = true;
	fl2000_add_bitmask(mask, union fl2000_vga_i2c_sc_reg, monitor_detect);
	vga_i2c_sc_reg.edid_detect = true;
	fl2000_add_bitmask(mask, union fl2000_vga_i2c_sc_reg, edid_detect);
	regmap_write_bits(regmap, FL2000_VGA_I2C_SC_REG, mask, vga_i2c_sc_reg.val);

	mask = 0;
	vga_ctrl_reg_3.wakeup_clr_en = false;
	fl2000_add_bitmask(mask, union fl2000_vga_ctrl_reg_3, wakeup_clr_en);
	regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_3, mask, vga_ctrl_reg_3.val);

	mask = 0;
	usb_lpm_reg.u1_reject = true;
	fl2000_add_bitmask(mask, union fl2000_usb_lpm_reg, u1_reject);
	usb_lpm_reg.u2_reject = true;
	fl2000_add_bitmask(mask, union fl2000_usb_lpm_reg, u2_reject);
	regmap_write_bits(regmap, FL2000_USB_LPM_REG, mask, usb_lpm_reg.val);

	mask = 0;
	usb_ctrl_reg.wake_nrdy = false;
	fl2000_add_bitmask(mask, union fl2000_usb_ctrl_reg, wake_nrdy);
	regmap_write_bits(regmap, FL2000_USB_CTRL_REG, mask, usb_ctrl_reg.val);

	return 0;
}

static int __fl2000_i2c_dword(struct fl2000_device *fl, bool read, u16 addr, u8 offset, u32 *data)
{
	int ret;
	union fl2000_vga_i2c_sc_reg reg = { .val = 0 };
	u32 mask = 0;
	struct regmap *regmap = fl->regmap;

	if (!read) {
		ret = regmap_write(regmap, FL2000_VGA_I2C_WR_REG, *data);
		if (ret)
			return -EIO;
	}

	/* This bit always reads back as 0, so we need to restore it back. Though not quite
	 * sure if we need to enable monitor detection circuit for HDMI use case
	 */
	reg.monitor_detect = true;
	fl2000_add_bitmask(mask, union fl2000_vga_i2c_sc_reg, monitor_detect);
	reg.edid_detect = true;
	fl2000_add_bitmask(mask, union fl2000_vga_i2c_sc_reg, edid_detect);

	reg.i2c_status = 0;
	fl2000_add_bitmask(mask, union fl2000_vga_i2c_sc_reg, i2c_status);
	reg.i2c_addr = addr;
	fl2000_add_bitmask(mask, union fl2000_vga_i2c_sc_reg, i2c_addr);
	reg.i2c_cmd = read;
	fl2000_add_bitmask(mask, union fl2000_vga_i2c_sc_reg, i2c_cmd);
	reg.i2c_offset = offset;
	fl2000_add_bitmask(mask, union fl2000_vga_i2c_sc_reg, i2c_offset);
	reg.i2c_done = false;
	fl2000_add_bitmask(mask, union fl2000_vga_i2c_sc_reg, i2c_done);

	ret = regmap_write_bits(regmap, FL2000_VGA_I2C_SC_REG, mask, reg.val);
	if (ret)
		return -EIO;

	ret = regmap_read_poll_timeout(regmap, FL2000_VGA_I2C_SC_REG, reg.val, reg.i2c_done,
				       I2C_RDWR_INTERVAL, I2C_RDWR_TIMEOUT);
	/* This shouldn't normally happen: there's internal 256ms HW timeout on I2C operations and
	 * USB must be always available so no I/O errors. But if it happens we are probably in
	 * irreversible HW issue
	 */
	if (ret || reg.i2c_status != 0) {
		u32 rd = 0;

		regmap_read(regmap, FL2000_VGA_I2C_RD_REG, &rd);
		dev_dbg(&fl->intf->dev, "i2c 0x%02x[0x%02x] poll %d, sc 0x%08x, rd 0x%08x\n", addr,
			offset, ret, reg.val, rd);
		return -EIO;
	}

	if (read) {
		ret = regmap_read(regmap, FL2000_VGA_I2C_RD_REG, data);
		if (ret)
			return -EIO;
	}

	return 0;
}

/* Read or write 4 consecutive registers of an I2C device; offset must be 4-byte aligned */
int fl2000_i2c_dword(struct fl2000_device *fl, bool read, u16 addr, u8 offset, u32 *data)
{
	int ret;

	mutex_lock(&fl->i2c_lock);
	ret = __fl2000_i2c_dword(fl, read, addr, offset, data);
	mutex_unlock(&fl->i2c_lock);

	dev_dbg(&fl->intf->dev, "i2c %s 0x%02x[0x%02x] = 0x%08x: %d\n", read ? "rd" : "wr", addr,
		offset, *data, ret);

	return ret;
}
