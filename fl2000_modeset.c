// SPDX-License-Identifier: GPL-2.0
/*
 * KMS pipeline: one primary plane -> CRTC (FL2000 timing generator + PLL) -> encoder -> one
 * connector. The image goes out on VGA (FL2000 DAC) and, when present, on HDMI (IT66121) at the
 * same time, so a single connector represents both.
 *
 * (C) Copyright 2012, Red Hat
 * (C) Copyright 2018-2020, Artem Mygaiev
 * (C) Copyright 2026, Samuel
 */

#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/slab.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_cache.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_mode_config.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>

#include "fl2000.h"
#include "fl2000_convert.h"

/* IT66121 is a single link TMDS transmitter, 165 MHz max; FL2000DX itself targets 1080p60 */
#define FL2000_MAX_CLOCK_KHZ 165000
#define FL2000_MAX_WIDTH     2560
#define FL2000_MAX_HEIGHT    1600

/* PLL computing precision is 6 digits after comma */
#define FL2000_PLL_PRECISION 1000000ULL

/* Input xtal clock, Hz */
#define FL2000_XTAL 10000000 /* 10 MHz */

/* Internal vco clock min/max, Hz */
#define FL2000_VCOCLOCK_MIN 62500000 /* 62.5 MHz */
#define FL2000_VCOCLOCK_MAX 1000000000 /* 1GHz */

/* Maximum acceptable pixel clock error: VESA DMT tolerance is 0.5%. Timings themselves are never
 * altered, VGA monitors and VGA-to-HDMI converters sample assuming standard htotal
 */
#define FL2000_PPM_ERR_MAX 5000

/* Assume bulk transfers can use only 80% of USB bandwidth, bytes per second */
#define FL2000_BW_HIGH_SPEED  (480000000ULL * 80 / 100 / 8)
#define FL2000_BW_SUPER_SPEED (5000000000ULL * 80 / 100 / 8)

static unsigned int max_bpp = 24;
module_param(max_bpp, uint, 0644);
MODULE_PARM_DESC(max_bpp, "Maximum bits per pixel sent over USB: 24 or 16 (default 24)");

/* VGA monitor DDC (EDID EEPROM) on the FL2000 I2C bus */
#define FL2000_VGA_DDC_ADDR 0x50

static const u32 fl2000_formats[] = {
	DRM_FORMAT_XRGB8888,
};

/* Integer division compute of ppm error */
static u64 fl2000_pll_ppm_err(u64 clock_mil, u32 vco_clk, u32 divisor)
{
	u64 pll_clk_mil = div_u64((u64)vco_clk * FL2000_PLL_PRECISION, divisor);
	u64 pll_clk_err;

	if (pll_clk_mil > clock_mil)
		pll_clk_err = pll_clk_mil - clock_mil;
	else
		pll_clk_err = clock_mil - pll_clk_mil;

	return div64_u64(pll_clk_err, div64_u64(clock_mil, FL2000_PLL_PRECISION));
}

static u32 fl2000_pll_get_divisor(u64 clock_mil, u32 vco_clk, u64 *min_ppm_err)
{
	u32 best_divisor = 0;

	/* Valid divisors are 2, 4, 6..128 */
	for (u32 divisor = 2; divisor <= 128; divisor += (divisor < 6) ? 2 : 1) {
		u64 ppm_err = fl2000_pll_ppm_err(clock_mil, vco_clk, divisor);

		if (ppm_err < *min_ppm_err) {
			*min_ppm_err = ppm_err;
			best_divisor = divisor;
		}
	}

	return best_divisor;
}

/* Try to match pixel clock - find parameters with minimal PLL error */
static u64 fl2000_pll_calc(u64 clock_mil, struct fl2000_pll *pll, u32 *clock_calculated)
{
	u64 min_ppm_err = U64_MAX;

	for (u32 prescaler = 1; prescaler <= 2; prescaler++) {
		for (u32 multiplier = 1; multiplier <= 128; multiplier++) {
			u32 vco_clk = FL2000_XTAL / prescaler * multiplier;
			u32 divisor;

			if (vco_clk < FL2000_VCOCLOCK_MIN || vco_clk > FL2000_VCOCLOCK_MAX)
				continue;

			divisor = fl2000_pll_get_divisor(clock_mil, vco_clk, &min_ppm_err);
			if (divisor == 0)
				continue;

			pll->prescaler = prescaler;
			pll->multiplier = multiplier;
			pll->divisor = divisor;
			if (vco_clk < 125000000)
				pll->function = 0;
			else if (vco_clk < 250000000)
				pll->function = 1;
			else if (vco_clk < 500000000)
				pll->function = 2;
			else
				pll->function = 3;
			*clock_calculated = vco_clk / divisor;
		}
	}

	return min_ppm_err;
}

static int fl2000_mode_calc(const struct drm_display_mode *mode, struct fl2000_pll *pll)
{
	u32 clock_calculated;

	if (mode->clock <= 0 || mode->clock > FL2000_MAX_CLOCK_KHZ)
		return -EINVAL;

	if (fl2000_pll_calc((u64)mode->clock * 1000 * FL2000_PLL_PRECISION, pll,
			    &clock_calculated) >= FL2000_PPM_ERR_MAX)
		return -ERANGE;

	return 0;
}

/* 3 (RGB888) or 2 (RGB565) bytes per pixel depending on USB bandwidth, 0 if mode is too fast */
static unsigned int fl2000_bytes_pix(struct fl2000_device *fl, const struct drm_display_mode *mode)
{
	u64 max_bw, rate;
	u64 bytes_pix;

	switch (fl->udev->speed) {
	case USB_SPEED_HIGH:
		max_bw = FL2000_BW_HIGH_SPEED;
		break;
	case USB_SPEED_SUPER:
	case USB_SPEED_SUPER_PLUS:
		max_bw = FL2000_BW_SUPER_SPEED;
		break;
	default:
		return 0;
	}

	if (!mode->htotal || !mode->vtotal)
		return 0;

	/* Only active pixels are transferred */
	rate = div_u64((u64)mode->clock * 1000 * mode->hdisplay * mode->vdisplay,
		       mode->htotal * mode->vtotal);
	if (!rate)
		return 0;

	bytes_pix = div64_u64(max_bw, rate);
	if (bytes_pix >= 3 && max_bpp >= 24)
		return 3;
	if (bytes_pix >= 2)
		return 2;
	return 0;
}

/* ---- Pixel conversion ---- */

static void fl2000_convert(struct fl2000_device *fl, int idx, const struct iosys_map *map,
			   const struct drm_framebuffer *fb, const struct drm_rect *clip, int src_x,
			   int src_y, u32 *line)
{
	struct fl2000_stream *stream = &fl->stream;
	u8 *dst = stream->bufs[idx].vaddr;
	unsigned int pixels = drm_rect_width(clip);

	for (int y = clip->y1; y < clip->y2; y++) {
		int dx = clip->x1 - src_x;
		int dy = y - src_y;

		if (dx < 0 || dy < 0 || dx + pixels > stream->width || dy >= stream->height)
			continue;

		/* The framebuffer usually is GPU memory mapped write-combined, where plain CPU
		 * reads crawl. drm_memcpy_from_wc() uses streaming loads when source and length
		 * are 16-byte aligned, so copy from a 4-pixel aligned start
		 */
		unsigned int ax1 = clip->x1 & ~3;
		size_t len = min_t(size_t, ALIGN((clip->x2 - ax1) * 4, 16),
				   fb->pitches[0] - ax1 * 4);
		struct iosys_map src = *map;
		struct iosys_map tmp = IOSYS_MAP_INIT_VADDR(line);

		iosys_map_incr(&src, (size_t)y * fb->pitches[0] + ax1 * 4);
		drm_memcpy_from_wc(&tmp, &src, len);

		fl2000_convert_line(dst, (size_t)dy * stream->width + dx, line + (clip->x1 - ax1),
				    pixels, stream->bytes_pix);
	}
}

/* ---- Plane ---- */

static int fl2000_plane_atomic_check(struct drm_plane *plane, struct fl2000_atomic_state *state)
{
	struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *crtc_state = NULL;

	if (new_state->crtc)
		crtc_state = drm_atomic_get_new_crtc_state(state, new_state->crtc);

	return drm_atomic_helper_check_plane_state(new_state, crtc_state, DRM_PLANE_NO_SCALING,
						   DRM_PLANE_NO_SCALING, false, false);
}

static void fl2000_plane_atomic_update(struct drm_plane *plane, struct fl2000_atomic_state *state)
{
	struct fl2000_device *fl = to_fl2000(plane->dev);
	struct drm_plane_state *old_state = drm_atomic_get_old_plane_state(state, plane);
	struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_shadow_plane_state *shadow = to_drm_shadow_plane_state(new_state);
	struct drm_framebuffer *fb = new_state->fb;
	int src_x = new_state->src.x1 >> 16;
	int src_y = new_state->src.y1 >> 16;
	struct drm_rect clip;
	u32 *line;
	ktime_t start;
	bool full;
	int idx, buf;

	if (!fb || !new_state->visible)
		return;

	if (!drm_atomic_helper_damage_merged(old_state, new_state, &clip))
		return;

	if (!drm_dev_enter(plane->dev, &idx))
		return;

	/* Room for the 4-pixel alignment on both ends */
	line = kmalloc_array(drm_rect_width(&clip) + 8, sizeof(*line), GFP_KERNEL);
	if (!line)
		goto out;

	if (drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE))
		goto out_free;

	full = clip.x1 <= src_x && clip.y1 <= src_y &&
	       clip.x2 >= src_x + (int)fl->stream.width && clip.y2 >= src_y + (int)fl->stream.height;

	start = ktime_get();
	buf = fl2000_stream_begin(fl, full);
	if (buf >= 0) {
		fl2000_convert(fl, buf, &shadow->data[0], fb, &clip, src_x, src_y, line);
		fl2000_stream_end(fl, buf);
		dev_dbg_ratelimited(&fl->intf->dev, "update " DRM_RECT_FMT "%s: %lld us\n",
				    DRM_RECT_ARG(&clip), full ? " full" : "",
				    ktime_us_delta(ktime_get(), start));
	}

	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);
out_free:
	kfree(line);
out:
	drm_dev_exit(idx);
}

static const struct drm_plane_helper_funcs fl2000_plane_helper_funcs = {
	DRM_GEM_SHADOW_PLANE_HELPER_FUNCS,
	.atomic_check = fl2000_plane_atomic_check,
	.atomic_update = fl2000_plane_atomic_update,
};

static const struct drm_plane_funcs fl2000_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	DRM_GEM_SHADOW_PLANE_FUNCS,
};

/* ---- CRTC ---- */

static enum drm_mode_status fl2000_crtc_mode_valid(struct drm_crtc *crtc,
						   const struct drm_display_mode *mode)
{
	struct fl2000_device *fl = to_fl2000(crtc->dev);
	struct fl2000_pll pll;

	if (mode->clock > FL2000_MAX_CLOCK_KHZ)
		return MODE_CLOCK_HIGH;

	if (mode->flags & DRM_MODE_FLAG_INTERLACE)
		return MODE_NO_INTERLACE;

	if (fl2000_mode_calc(mode, &pll))
		return MODE_CLOCK_RANGE;

	if (!fl2000_bytes_pix(fl, mode))
		return MODE_CLOCK_HIGH;

	return MODE_OK;
}

static int fl2000_crtc_atomic_check(struct drm_crtc *crtc, struct fl2000_atomic_state *state)
{
	struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state, crtc);

	if (!crtc_state->enable)
		return 0;

	return drm_atomic_helper_check_crtc_primary_plane(crtc_state);
}

static void fl2000_crtc_atomic_enable(struct drm_crtc *crtc, struct fl2000_atomic_state *state)
{
	struct fl2000_device *fl = to_fl2000(crtc->dev);
	struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	const struct drm_display_mode *mode = &crtc_state->adjusted_mode;
	struct fl2000_timings timings;
	struct fl2000_pll pll;
	unsigned int bytes_pix;
	int idx, ret;

	if (!drm_dev_enter(crtc->dev, &idx))
		return;

	bytes_pix = fl2000_bytes_pix(fl, mode);
	if (fl2000_mode_calc(mode, &pll) || !bytes_pix) {
		drm_err(crtc->dev, "Mode " DRM_MODE_FMT " cannot be set\n", DRM_MODE_ARG(mode));
		goto out;
	}

	drm_info(crtc->dev, "Mode " DRM_MODE_FMT ", %u bytes per pixel\n", DRM_MODE_ARG(mode),
		 bytes_pix);

	timings.hactive = mode->hdisplay;
	timings.htotal = mode->htotal;
	timings.hsync_width = mode->hsync_end - mode->hsync_start;
	timings.hstart = mode->htotal - mode->hsync_start + 1;
	timings.vactive = mode->vdisplay;
	timings.vtotal = mode->vtotal;
	timings.vsync_width = mode->vsync_end - mode->vsync_start;
	timings.vstart = mode->vtotal - mode->vsync_start + 1;

	fl->mode_pll = pll;
	fl->mode_timings = timings;
	fl->mode_bytes_pix = bytes_pix;

	ret = fl2000_set_mode(fl, &pll, &timings, bytes_pix);
	if (ret) {
		drm_err(crtc->dev, "Cannot program mode (%d)\n", ret);
		goto out;
	}

	ret = fl2000_stream_start(fl, mode->hdisplay, mode->vdisplay, bytes_pix);
	if (ret)
		drm_err(crtc->dev, "Cannot start streaming (%d)\n", ret);

	fl2000_it66121_enable(fl, mode);
out:
	drm_dev_exit(idx);
}

static void fl2000_crtc_atomic_disable(struct drm_crtc *crtc, struct fl2000_atomic_state *state)
{
	struct fl2000_device *fl = to_fl2000(crtc->dev);
	int idx;

	if (drm_dev_enter(crtc->dev, &idx)) {
		fl2000_it66121_disable(fl);
		drm_dev_exit(idx);
	}

	/* Always, even when unplugged: URBs must be gone before buffers are freed */
	fl2000_stream_stop(fl);
}

static const struct drm_crtc_helper_funcs fl2000_crtc_helper_funcs = {
	.mode_valid = fl2000_crtc_mode_valid,
	.atomic_check = fl2000_crtc_atomic_check,
	.atomic_enable = fl2000_crtc_atomic_enable,
	.atomic_disable = fl2000_crtc_atomic_disable,
};

static const struct drm_crtc_funcs fl2000_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
};

/* ---- Connector ---- */

static int fl2000_vga_edid_block(void *context, u8 *buf, unsigned int block, size_t len)
{
	struct fl2000_device *fl = context;
	unsigned int base = (block & 1) * EDID_LENGTH;

	/* Would need E-DDC segment pointer, not supported by FL2000 I2C master */
	if (block > 1)
		return -EINVAL;

	for (size_t off = 0; off < len; off += 4) {
		u32 data;
		int ret = fl2000_i2c_dword(fl, true, FL2000_VGA_DDC_ADDR, base + off, &data);

		if (ret)
			return ret;

		for (size_t i = 0; i < 4 && off + i < len; i++)
			buf[off + i] = data >> (i * 8);
	}

	return 0;
}

static int fl2000_connector_get_modes(struct drm_connector *connector)
{
	struct fl2000_device *fl = to_fl2000(connector->dev);
	const struct drm_edid *drm_edid = NULL;
	int count;

	if (fl2000_it66121_connected(fl)) {
		drm_edid = fl2000_it66121_read_edid(fl, connector);
		if (drm_edid)
			drm_dbg_kms(connector->dev, "EDID read over HDMI\n");
	}

	if (!drm_edid) {
		drm_edid = drm_edid_read_custom(connector, fl2000_vga_edid_block, fl);
		if (drm_edid)
			drm_dbg_kms(connector->dev, "EDID read over VGA DDC\n");
	}

	drm_edid_connector_update(connector, drm_edid);
	count = drm_edid_connector_add_modes(connector);
	drm_edid_free(drm_edid);

	if (!count) {
		drm_info(connector->dev, "No EDID, using standard modes\n");
		count = drm_add_modes_noedid(connector, 1920, 1200);
		drm_set_preferred_mode(connector, 1024, 768);
	}

	return count;
}

static int fl2000_connector_detect_ctx(struct drm_connector *connector,
				       struct drm_modeset_acquire_ctx *ctx, bool force)
{
	struct fl2000_device *fl = to_fl2000(connector->dev);
	enum drm_connector_status status = connector_status_disconnected;
	union fl2000_vga_status_reg vga = {};
	u32 data;
	int idx;

	if (!drm_dev_enter(connector->dev, &idx))
		return connector_status_disconnected;

	/* VGA load sense works even with monitors/cables that have no DDC lines */
	regmap_read(fl->regmap, FL2000_VGA_STATUS_REG, &vga.val);

	if (vga.monitor_status || fl2000_it66121_connected(fl) ||
	    !fl2000_i2c_dword(fl, true, FL2000_VGA_DDC_ADDR, 0, &data))
		status = connector_status_connected;

	drm_dev_exit(idx);
	return status;
}

static const struct drm_connector_helper_funcs fl2000_connector_helper_funcs = {
	.get_modes = fl2000_connector_get_modes,
	.detect_ctx = fl2000_connector_detect_ctx,
};

static const struct drm_connector_funcs fl2000_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

/* ---- Mode config ---- */

static const struct drm_mode_config_funcs fl2000_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

/* Enable CRTC (allocates stream buffers) before planes are updated */
static const struct drm_mode_config_helper_funcs fl2000_mode_config_helper_funcs = {
	.atomic_commit_tail = drm_atomic_helper_commit_tail_rpm,
};

int fl2000_modeset_init(struct fl2000_device *fl)
{
	struct drm_device *drm = &fl->drm;
	int encoder_type = fl->hdmi ? DRM_MODE_ENCODER_TMDS : DRM_MODE_ENCODER_DAC;
	int connector_type = fl->hdmi ? DRM_MODE_CONNECTOR_HDMIA : DRM_MODE_CONNECTOR_VGA;
	int ret;

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;

	drm->mode_config.min_width = 64;
	drm->mode_config.max_width = FL2000_MAX_WIDTH;
	drm->mode_config.min_height = 64;
	drm->mode_config.max_height = FL2000_MAX_HEIGHT;
	drm->mode_config.preferred_depth = 24;
	drm->mode_config.funcs = &fl2000_mode_config_funcs;
	drm->mode_config.helper_private = &fl2000_mode_config_helper_funcs;

	ret = drm_universal_plane_init(drm, &fl->plane, 0, &fl2000_plane_funcs, fl2000_formats,
				       ARRAY_SIZE(fl2000_formats), NULL, DRM_PLANE_TYPE_PRIMARY,
				       NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&fl->plane, &fl2000_plane_helper_funcs);
	drm_plane_enable_fb_damage_clips(&fl->plane);

	ret = drm_crtc_init_with_planes(drm, &fl->crtc, &fl->plane, NULL, &fl2000_crtc_funcs, NULL);
	if (ret)
		return ret;
	drm_crtc_helper_add(&fl->crtc, &fl2000_crtc_helper_funcs);

	ret = drmm_encoder_init(drm, &fl->encoder, NULL, encoder_type, NULL);
	if (ret)
		return ret;
	fl->encoder.possible_crtcs = drm_crtc_mask(&fl->crtc);

	ret = drm_connector_init(drm, &fl->connector, &fl2000_connector_funcs, connector_type);
	if (ret)
		return ret;
	drm_connector_helper_add(&fl->connector, &fl2000_connector_helper_funcs);
	fl->connector.polled = DRM_CONNECTOR_POLL_CONNECT | DRM_CONNECTOR_POLL_DISCONNECT;

	ret = drm_connector_attach_encoder(&fl->connector, &fl->encoder);
	if (ret)
		return ret;

	drm_mode_config_reset(drm);

	return 0;
}
