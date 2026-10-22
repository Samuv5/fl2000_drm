/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Fresco Logic FL2000 USB display driver
 *
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2020, Artem Mygaiev
 * (C) Copyright 2026, Samuel
 */

#ifndef __FL2000_H__
#define __FL2000_H__

#include <linux/bits.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/scatterlist.h>
#include <linux/spinlock.h>
#include <linux/usb.h>
#include <linux/workqueue.h>

#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_encoder.h>
#include <drm/drm_plane.h>

#include "fl2000_registers.h"

/* Linux 7.2 renamed struct drm_atomic_state to struct drm_atomic_commit (detected in Kbuild) */
#ifdef FL2000_HAVE_DRM_ATOMIC_COMMIT
#define fl2000_atomic_state drm_atomic_commit
#else
#define fl2000_atomic_state drm_atomic_state
#endif

#define DRIVER_NAME "fl2000"

/* Interface 0 altsetting 1 provides bulk OUT endpoint 1 used for frame streaming */
#define FL2000_USBIF_AVCONTROL	    0
#define FL2000_STREAMING_ALTSETTING 1
#define FL2000_EP_STREAM	    1

/* Streaming: buffers in flight never exceed URB count, plus the latest frame, plus one to draw */
#define FL2000_STREAM_URBS 2
#define FL2000_STREAM_BUFS (FL2000_STREAM_URBS + 2)

struct fl2000_buf {
	struct sg_table sgt;
	struct page **pages;
	unsigned int nr_pages;
	void *vaddr;
	int users; /* URBs transmitting it, or the writer filling it */
};

struct fl2000_stream {
	struct fl2000_buf bufs[FL2000_STREAM_BUFS];
	struct urb *urbs[FL2000_STREAM_URBS];
	int urb_buf[FL2000_STREAM_URBS];
	spinlock_t lock; /* Protects everything below, taken from URB completion */
	size_t size;
	unsigned int width;
	unsigned int height;
	unsigned int bytes_pix;
	int latest;
	bool enabled; /* Between fl2000_stream_start() and fl2000_stream_stop() */
	bool running; /* URBs are being resubmitted */
	int error; /* Last transfer error, handled by recover work */
	unsigned int retries;
	struct work_struct recover;
	struct delayed_work watchdog;
};

struct fl2000_pll {
	u32 prescaler;
	u32 multiplier;
	u32 divisor;
	u32 function;
};

struct fl2000_timings {
	u32 hactive;
	u32 htotal;
	u32 hsync_width;
	u32 hstart;
	u32 vactive;
	u32 vtotal;
	u32 vsync_width;
	u32 vstart;
};

struct fl2000_it66121;

struct fl2000_device {
	struct drm_device drm;
	struct usb_device *udev;
	struct usb_interface *intf;
	struct regmap *regmap;
	struct mutex i2c_lock; /* FL2000 I2C master is a multi-register sequence */

	struct drm_plane plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;

	struct fl2000_it66121 *hdmi; /* NULL when there is no IT66121 (VGA only) */

	/* Current mode, to resynchronize the device after a failed transfer */
	struct fl2000_pll mode_pll;
	struct fl2000_timings mode_timings;
	u32 mode_bytes_pix;
	struct fl2000_stream stream;

	struct urb *intr_urb;
	u8 *intr_buf;
	struct work_struct intr_work;
};

static inline struct fl2000_device *to_fl2000(struct drm_device *drm)
{
	return container_of(drm, struct fl2000_device, drm);
}

/**
 * fl2000_add_bitmask - Set bitmask for structure field
 *
 * @__mask: Variable to set mask to (assumed u32)
 * @__type: Structure type to use with bitfield (assumed size equal to u32)
 * @__field: Field to set mask for in the '__type' structure
 */
#define fl2000_add_bitmask(__mask, __type, __field) \
	({                                          \
		union {                             \
			__type __umask;             \
			typeof(__mask) __val;       \
		} __aligned(4) __data = {};         \
		__data.__umask.__field = ~0;        \
		(__mask) |= __data.__val;           \
	})

/* fl2000_regs.c */
int fl2000_regmap_init(struct fl2000_device *fl);
int fl2000_reset(struct fl2000_device *fl);
int fl2000_usb_magic(struct fl2000_device *fl);
int fl2000_set_mode(struct fl2000_device *fl, const struct fl2000_pll *pll,
		    const struct fl2000_timings *t, u32 bytes_pix);
int fl2000_i2c_dword(struct fl2000_device *fl, bool read, u16 addr, u8 offset, u32 *data);

/* fl2000_modeset.c */
int fl2000_modeset_init(struct fl2000_device *fl);

/* fl2000_stream.c */
int fl2000_stream_init(struct fl2000_device *fl);
int fl2000_stream_start(struct fl2000_device *fl, unsigned int width, unsigned int height,
			unsigned int bytes_pix);
void fl2000_stream_stop(struct fl2000_device *fl);
int fl2000_stream_begin(struct fl2000_device *fl, bool full);
void fl2000_stream_end(struct fl2000_device *fl, int idx);

/* fl2000_it66121.c */
int fl2000_it66121_probe(struct fl2000_device *fl);
int fl2000_it66121_init(struct fl2000_device *fl);
bool fl2000_it66121_connected(struct fl2000_device *fl);
const struct drm_edid *fl2000_it66121_read_edid(struct fl2000_device *fl,
						struct drm_connector *connector);
void fl2000_it66121_enable(struct fl2000_device *fl, const struct drm_display_mode *mode);
void fl2000_it66121_disable(struct fl2000_device *fl);

#endif /* __FL2000_H__ */
