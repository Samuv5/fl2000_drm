// SPDX-License-Identifier: GPL-2.0
/*
 * ITE IT66121 HDMI transmitter found behind FL2000 in USB-HDMI (and dual VGA+HDMI) adapters. It is
 * accessed through the FL2000 I2C master, so there is no i2c_client and no drm_bridge here: the
 * FL2000 driver calls these helpers directly.
 *
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2019, Artem Mygaiev
 * (C) Copyright 2026, Samuel
 */

#include <linux/delay.h>
#include <linux/hdmi.h>
#include <linux/slab.h>

#include <drm/drm_edid.h>
#include <drm/drm_modes.h>
#include <drm/drm_print.h>

#include "fl2000.h"
#include "it66121_registers.h"

#define VENDOR_ID      0x4954
#define DEVICE_ID      0x0612
#define REVISION_MASK  0xF000
#define REVISION_SHIFT 12

#define EDID_SLEEP     1000
#define EDID_TIMEOUT   200000
#define EDID_DDC_ADDR  0xA0
#define EDID_FIFO_SIZE 32

/* HW defect leading to losing 3 first bytes during EDID read operation */
#define EDID_LOSS_LEN 3

enum it66121_ddc_cmd {
	DDC_CMD_BURST_READ = 0x0,
	DDC_CMD_EDID_READ = 0x3,
	DDC_CMD_FIFO_CLEAR = 0x9,
	DDC_CMD_ABORT = 0xF,
};

struct fl2000_it66121 {
	struct fl2000_device *fl;
	u16 addr;
	struct regmap *regmap;
	struct regmap_field *hpd;
	struct regmap_field *ddc_done;
	struct regmap_field *ddc_error;
	struct mutex ddc_lock; /* EDID read is a multi-register sequence */
	bool dvi_mode;
};

/* FL2000 I2C master always transfers 4 registers at once, aligned to 4 */
static int it66121_reg_read(void *context, unsigned int reg, unsigned int *val)
{
	struct fl2000_it66121 *it = context;
	u32 data;
	int ret;

	ret = fl2000_i2c_dword(it->fl, true, it->addr, reg & ~3u, &data);
	if (ret)
		return ret;

	*val = (data >> ((reg & 3) * 8)) & 0xFF;
	return 0;
}

static int it66121_reg_write(void *context, unsigned int reg, unsigned int val)
{
	struct fl2000_it66121 *it = context;
	unsigned int shift = (reg & 3) * 8;
	u32 data;
	int ret;

	/* Read before write in order not to corrupt the 3 neighbour registers */
	ret = fl2000_i2c_dword(it->fl, true, it->addr, reg & ~3u, &data);
	if (ret)
		return ret;

	data &= ~(0xFFu << shift);
	data |= (val & 0xFF) << shift;

	return fl2000_i2c_dword(it->fl, false, it->addr, reg & ~3u, &data);
}

static const struct regmap_range_cfg it66121_regmap_banks[] = {
	/* Do not put common registers to any range, this will lead to skipping "bank" configuration
	 * when accessing those at addresses 0x00-0x2F
	 */
	{
		.name = "Banks",
		.range_min = IT66121_BANK_START,
		.range_max = IT66121_BANK_END,
		.selector_reg = IT66121_SYS_CONTROL,
		.selector_mask = IT66121_SYS_BANK_MASK,
		.selector_shift = 0,
		.window_start = IT66121_BANK_START,
		.window_len = IT66121_BANK_SIZE,
	},
};

static const struct regmap_config it66121_regmap_config = {
	.name = "it66121",
	.val_bits = 8,
	.reg_bits = 8,
	.reg_stride = 1,
	.max_register = 2 * IT66121_BANK_SIZE - 1,

	/* Read-modify-write of whole dwords makes caching unreliable */
	.cache_type = REGCACHE_NONE,

	.ranges = it66121_regmap_banks,
	.num_ranges = ARRAY_SIZE(it66121_regmap_banks),

	.reg_read = it66121_reg_read,
	.reg_write = it66121_reg_write,
};

static int it66121_wait_ddc_ready(struct fl2000_it66121 *it)
{
	unsigned int val;
	int ret;

	ret = regmap_field_read_poll_timeout(it->ddc_done, val, val, EDID_SLEEP, EDID_TIMEOUT);
	if (ret)
		return ret;

	ret = regmap_field_read(it->ddc_error, &val);
	if (ret)
		return ret;

	return val ? -EIO : 0;
}

static int it66121_abort_ddc_ops(struct fl2000_it66121 *it)
{
	int ret;

	/* From original driver: According to 2009/01/15 modification by Jau-Chih.Tseng@ite.com.tw
	 * do abort DDC twice
	 */
	for (int i = 0; i < 2; i++) {
		ret = regmap_write(it->regmap, IT66121_DDC_COMMAND, DDC_CMD_ABORT);
		if (ret)
			return ret;

		ret = it66121_wait_ddc_ready(it);
		if (ret)
			return ret;
	}

	return 0;
}

static int it66121_get_edid_block(void *context, u8 *buf, unsigned int block, size_t len)
{
	static const u8 header[EDID_LOSS_LEN] = { 0x00, 0xFF, 0xFF };
	struct fl2000_it66121 *it = context;
	unsigned int segment = block >> 1;
	unsigned int offset = block & 1 ? 128 : 0;
	size_t remain = len;
	unsigned int val;
	int ret;

	ret = it66121_abort_ddc_ops(it);
	if (ret)
		return ret;

	/* Statically fill first 3 bytes (due to EDID reading HW bug) */
	while (offset < EDID_LOSS_LEN && remain > 0) {
		*(buf++) = header[offset];
		remain--;
		offset++;
	}

	while (remain > 0) {
		/* Add bytes that will be lost during EDID read and check size fits FIFO */
		size_t size = min_t(size_t, remain + EDID_LOSS_LEN, EDID_FIFO_SIZE);

		ret = regmap_write(it->regmap, IT66121_DDC_COMMAND, DDC_CMD_FIFO_CLEAR);
		if (ret)
			return ret;
		ret = regmap_write(it->regmap, IT66121_DDC_ADDRESS, EDID_DDC_ADDR);
		if (ret)
			return ret;
		ret = regmap_write(it->regmap, IT66121_DDC_OFFSET, offset - EDID_LOSS_LEN);
		if (ret)
			return ret;
		ret = regmap_write(it->regmap, IT66121_DDC_SIZE, size);
		if (ret)
			return ret;
		ret = regmap_write(it->regmap, IT66121_DDC_SEGMENT, segment);
		if (ret)
			return ret;
		ret = regmap_write(it->regmap, IT66121_DDC_COMMAND, DDC_CMD_EDID_READ);
		if (ret)
			return ret;

		/* DDC runs at ~100 kHz, a full FIFO takes ~3 ms. Status register cannot be polled
		 * here: it shares a dword with the FIFO and reading it would pop a byte
		 */
		usleep_range(5000, 6000);

		size -= EDID_LOSS_LEN;
		for (size_t i = 0; i < size; i++) {
			ret = regmap_read(it->regmap, IT66121_DDC_RD_FIFO, &val);
			if (ret)
				return ret;
			*(buf++) = val & 0xFF;
		}

		remain -= size;
		offset += size;
	}

	return 0;
}

const struct drm_edid *fl2000_it66121_read_edid(struct fl2000_device *fl,
						struct drm_connector *connector)
{
	struct fl2000_it66121 *it = fl->hdmi;
	const struct drm_edid *drm_edid;

	if (!it)
		return NULL;

	mutex_lock(&it->ddc_lock);
	drm_edid = drm_edid_read_custom(connector, it66121_get_edid_block, it);
	mutex_unlock(&it->ddc_lock);

	return drm_edid;
}

bool fl2000_it66121_connected(struct fl2000_device *fl)
{
	unsigned int val;

	if (!fl->hdmi || regmap_field_read(fl->hdmi->hpd, &val))
		return false;

	return val;
}

static int it66121_check_id(struct fl2000_device *fl, u16 addr)
{
	u32 data;
	u16 vendor, device;
	int ret;

	/* Registers 0x00-0x03: vendor ID (LE), device ID (LE) with revision in upper nibble */
	ret = fl2000_i2c_dword(fl, true, addr, 0, &data);
	if (ret)
		return ret;

	vendor = data & 0xFFFF;
	device = data >> 16;
	if (vendor != VENDOR_ID || (device & ~REVISION_MASK) != DEVICE_ID) {
		drm_dbg(&fl->drm, "No IT66121 at 0x%02X (0x%04X-0x%04X)\n", addr, vendor, device);
		return -ENODEV;
	}

	drm_info(&fl->drm, "IT66121 HDMI transmitter at 0x%02X, revision %d\n", addr,
		 (device & REVISION_MASK) >> REVISION_SHIFT);
	return 0;
}

int fl2000_it66121_probe(struct fl2000_device *fl)
{
	/* According to datasheet IT66121 addresses are 0x98 or 0x9A including R/W bit */
	static const u16 addrs[] = { 0x98 >> 1, 0x9A >> 1 };
	struct device *dev = &fl->intf->dev;
	struct fl2000_it66121 *it;
	int i;

	for (i = 0; i < ARRAY_SIZE(addrs); i++)
		if (!it66121_check_id(fl, addrs[i]))
			break;
	if (i == ARRAY_SIZE(addrs))
		return -ENODEV;

	it = devm_kzalloc(dev, sizeof(*it), GFP_KERNEL);
	if (!it)
		return -ENOMEM;

	it->fl = fl;
	it->addr = addrs[i];
	mutex_init(&it->ddc_lock);

	it->regmap = devm_regmap_init(dev, NULL, it, &it66121_regmap_config);
	if (IS_ERR(it->regmap))
		return PTR_ERR(it->regmap);

	it->hpd = devm_regmap_field_alloc(dev, it->regmap, IT66121_SYS_STATUS_hpd);
	it->ddc_done = devm_regmap_field_alloc(dev, it->regmap, IT66121_DDC_STATUS_ddc_done);
	it->ddc_error = devm_regmap_field_alloc(dev, it->regmap, IT66121_DDC_STATUS_ddc_error);
	if (IS_ERR(it->hpd) || IS_ERR(it->ddc_done) || IS_ERR(it->ddc_error))
		return -ENOMEM;

	fl->hdmi = it;
	return 0;
}

/* Power-up sequence, done once after probe and after resume */
int fl2000_it66121_init(struct fl2000_device *fl)
{
	struct fl2000_it66121 *it = fl->hdmi;
	struct regmap *map;
	int ret;

	if (!it)
		return 0;
	map = it->regmap;

	/* Reset according to IT66121 manual */
	ret = regmap_write_bits(map, IT66121_SW_RST, IT66121_SW_REF_RST_HDMITX,
				IT66121_SW_REF_RST_HDMITX);
	if (ret)
		return ret;
	msleep(50);

	/* Power up GRCLK & power down IACLK, TxCLK, CRCLK */
	regmap_write_bits(map, IT66121_SYS_CONTROL, (0xf << 3), (7 << 3));

	/* Continue according to IT66121 manual */
	regmap_write_bits(map, IT66121_INT_CONTROL, BIT(0), 0);
	regmap_write_bits(map, IT66121_AFE_DRV_CONTROL, BIT(5), 0);
	regmap_write_bits(map, IT66121_AFE_XP_CONTROL, BIT(2) | BIT(6), 0);
	regmap_write_bits(map, IT66121_AFE_IP_CONTROL_1, BIT(6), 0);
	regmap_write_bits(map, IT66121_AFE_DRV_CONTROL, BIT(4), 0);
	regmap_write_bits(map, IT66121_AFE_XP_CONTROL, BIT(3), BIT(3));
	regmap_write_bits(map, IT66121_AFE_IP_CONTROL_1, BIT(2), BIT(2));

	/* Extra steps for AFE from original driver, values are undisclosed */
	regmap_write_bits(map, IT66121_AFE_XP_TEST, 0xFF, 0x70);
	regmap_write_bits(map, IT66121_AFE_DRV_HS, 0xFF, 0x1F);
	/* DRV_ISW[5:3] = '111': maximum output current level swing */
	regmap_write_bits(map, IT66121_AFE_IP_CONTROL_2, 0xFF, 0x38);

	/* Power up IACLK, TxCLK */
	regmap_write_bits(map, IT66121_SYS_CONTROL, (3 << 4), 0);

	/* Reset according to IT66121 manual */
	regmap_write_bits(map, IT66121_SW_RST, IT66121_SW_REF_RST_HDMITX,
			  IT66121_SW_REF_RST_HDMITX);
	msleep(50);

	/* We do not support HDCP so its ok to statically set host controls */
	return regmap_write(map, IT66121_DDC_CONTROL,
			    IT66121_DDC_MASTER_DDC | IT66121_DDC_MASTER_HOST);
}

static int it66121_configure_afe(struct regmap *map, int clock_khz)
{
	int ret;

	ret = regmap_write(map, IT66121_AFE_DRV_CONTROL, IT66121_AFE_RST);
	if (ret)
		return ret;

	if (clock_khz > 80000) {
		regmap_write_bits(map, IT66121_AFE_XP_CONTROL, 0x90, 0x80);
		regmap_write_bits(map, IT66121_AFE_IP_CONTROL_1, 0x89, 0x80);
		regmap_write_bits(map, IT66121_AFE_IP_CONTROL_3, 0x10, 0x80);
	} else {
		regmap_write_bits(map, IT66121_AFE_XP_CONTROL, 0x90, 0x10);
		regmap_write_bits(map, IT66121_AFE_IP_CONTROL_1, 0x89, 0x09);
		regmap_write_bits(map, IT66121_AFE_IP_CONTROL_3, 0x10, 0x10);
	}

	/* Fire AFE */
	return regmap_write(map, IT66121_AFE_DRV_CONTROL, 0);
}

static int it66121_write_avi_infoframe(struct fl2000_device *fl, const struct drm_display_mode *mode)
{
	static const u16 aviinfo_reg[HDMI_AVI_INFOFRAME_SIZE] = {
		IT66121_HDMI_AVIINFO_DB1,  IT66121_HDMI_AVIINFO_DB2,  IT66121_HDMI_AVIINFO_DB3,
		IT66121_HDMI_AVIINFO_DB4,  IT66121_HDMI_AVIINFO_DB5,  IT66121_HDMI_AVIINFO_DB6,
		IT66121_HDMI_AVIINFO_DB7,  IT66121_HDMI_AVIINFO_DB8,  IT66121_HDMI_AVIINFO_DB9,
		IT66121_HDMI_AVIINFO_DB10, IT66121_HDMI_AVIINFO_DB11, IT66121_HDMI_AVIINFO_DB12,
		IT66121_HDMI_AVIINFO_DB13
	};
	struct regmap *map = fl->hdmi->regmap;
	struct hdmi_avi_infoframe frame;
	u8 buf[HDMI_INFOFRAME_SIZE(AVI)];
	ssize_t size;
	int ret;

	ret = drm_hdmi_avi_infoframe_from_display_mode(&frame, &fl->connector, mode);
	if (ret)
		return ret;

	size = hdmi_avi_infoframe_pack(&frame, buf, sizeof(buf));
	if (size < 0)
		return size;

	for (int i = 0; i < HDMI_AVI_INFOFRAME_SIZE; i++) {
		ret = regmap_write(map, aviinfo_reg[i], buf[i + HDMI_INFOFRAME_HEADER_SIZE]);
		if (ret)
			return ret;
	}
	ret = regmap_write(map, IT66121_HDMI_AVIINFO_CSUM, buf[3]);
	if (ret)
		return ret;

	return regmap_write(map, IT66121_HDMI_AVI_INFO_PKT,
			    IT66121_HDMI_AVI_INFO_PKT_ON | IT66121_HDMI_AVI_INFO_RPT);
}

void fl2000_it66121_enable(struct fl2000_device *fl, const struct drm_display_mode *mode)
{
	struct fl2000_it66121 *it = fl->hdmi;
	struct regmap *map;
	int ret;

	if (!it)
		return;
	map = it->regmap;

	it->dvi_mode = !fl->connector.display_info.is_hdmi;

	ret = regmap_write(map, IT66121_HDMI_MODE,
			   it->dvi_mode ? IT66121_HDMI_MODE_DVI : IT66121_HDMI_MODE_HDMI);
	if (ret)
		goto err;

	/* Mute AV while reconfiguring */
	regmap_write(map, IT66121_HDMI_AV_MUTE, IT66121_HDMI_AV_MUTE_ON | IT66121_HDMI_AV_MUTE_BLUE);

	if (!it->dvi_mode) {
		/* Enable HDMI packets, repeat for every data frame as recommended */
		regmap_write(map, IT66121_HDMI_GEN_CTRL_PKT,
			     IT66121_HDMI_GEN_CTRL_PKT_ON | IT66121_HDMI_GEN_CTRL_PKT_RPT);

		ret = it66121_write_avi_infoframe(fl, mode);
		if (ret)
			drm_warn(&fl->drm, "Cannot write AVI infoframe (%d)\n", ret);
	}

	/* Set reset flags and disable TXCLK prior to configuration */
	regmap_write_bits(map, IT66121_SW_RST, IT66121_SW_REF_RST_HDMITX | IT66121_SW_HDMI_VID_RST,
			  IT66121_SW_REF_RST_HDMITX | IT66121_SW_HDMI_VID_RST);
	regmap_write_bits(map, IT66121_SYS_CONTROL, IT66121_SYS_TXCLK_OFF, IT66121_SYS_TXCLK_OFF);

	/* Input from FL2000: RGB, 1 cycle PCLK delay, no color conversion */
	regmap_write(map, IT66121_INPUT_MODE, IT66121_INPUT_MODE_RGB | IT66121_INPUT_PCLKDELAY1);
	regmap_write(map, IT66121_INPUT_COLOR_CONV, IT66121_INPUT_NO_CONV);

	ret = it66121_configure_afe(map, mode->clock);
	if (ret)
		goto err;

	/* Clear reset flags, enable TXCLK and unmute */
	regmap_write_bits(map, IT66121_SW_RST, IT66121_SW_REF_RST_HDMITX | IT66121_SW_HDMI_VID_RST,
			  0);
	regmap_write_bits(map, IT66121_SYS_CONTROL, IT66121_SYS_TXCLK_OFF, 0);
	ret = regmap_write(map, IT66121_HDMI_AV_MUTE, 0);
	if (ret)
		goto err;

	return;
err:
	drm_err(&fl->drm, "IT66121 configuration failed (%d)\n", ret);
}

void fl2000_it66121_disable(struct fl2000_device *fl)
{
	if (!fl->hdmi)
		return;

	regmap_write_bits(fl->hdmi->regmap, IT66121_HDMI_AV_MUTE, IT66121_HDMI_AV_MUTE_ON,
			  IT66121_HDMI_AV_MUTE_ON);
}
