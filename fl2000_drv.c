// SPDX-License-Identifier: GPL-2.0
/*
 * Fresco Logic FL2000 USB display driver (USB 3.0 to VGA / HDMI adapters)
 *
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2020, Artem Mygaiev
 * (C) Copyright 2026, Samuel
 */

#include <linux/module.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_shmem.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>

#include "fl2000.h"

#define USB_VENDOR_FRESCO_LOGIC 0x1D5C
#define USB_PRODUCT_FL2000	0x2000

/* Interface 0 altsetting 1 provides bulk OUT endpoint 1 used for frame streaming */
#define FL2000_USBIF_AVCONTROL	  0
#define FL2000_STREAMING_ALTSETTING 1

DEFINE_DRM_GEM_FOPS(fl2000_fops);

static const struct drm_driver fl2000_drm_driver = {
	.driver_features = DRIVER_ATOMIC | DRIVER_GEM | DRIVER_MODESET,
	.fops = &fl2000_fops,
	DRM_GEM_SHMEM_DRIVER_OPS,
	DRM_FBDEV_SHMEM_DRIVER_OPS,

	.name = DRIVER_NAME,
	.desc = "Fresco Logic FL2000 USB display",
	.major = 1,
	.minor = 0,
};

/* Bring FL2000 (and IT66121) to a known state; used at probe and after resume */
static int fl2000_hw_init(struct fl2000_device *fl)
{
	int ret;

	ret = usb_set_interface(fl->udev, FL2000_USBIF_AVCONTROL, FL2000_STREAMING_ALTSETTING);
	if (ret)
		return ret;

	ret = usb_pipe_type_check(fl->udev, usb_sndbulkpipe(fl->udev, FL2000_EP_STREAM));
	if (ret) {
		drm_err(&fl->drm, "No bulk OUT endpoint %d for streaming\n", FL2000_EP_STREAM);
		return ret;
	}

	ret = fl2000_reset(fl);
	if (ret)
		return ret;

	return fl2000_usb_magic(fl);
}

static int fl2000_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct fl2000_device *fl;
	struct drm_device *drm;
	struct device *dma_dev;
	int ret;

	if (udev->speed < USB_SPEED_HIGH) {
		dev_err(&intf->dev, "USB 1.1 is not supported\n");
		return -ENODEV;
	}
	if (udev->speed < USB_SPEED_SUPER)
		dev_warn(&intf->dev, "Connected over USB 2.0, only small resolutions will work\n");

	if (!udev->bus->sg_tablesize) {
		dev_err(&intf->dev, "USB host controller does not support scatter-gather\n");
		return -ENODEV;
	}

	fl = devm_drm_dev_alloc(&intf->dev, &fl2000_drm_driver, struct fl2000_device, drm);
	if (IS_ERR(fl))
		return PTR_ERR(fl);
	drm = &fl->drm;

	fl->udev = udev;
	fl->intf = intf;
	mutex_init(&fl->i2c_lock);
	usb_set_intfdata(intf, fl);

	/* For PRIME import buffers are mapped by the USB host controller */
	dma_dev = usb_intf_get_dma_device(intf);
	if (dma_dev) {
		drm_dev_set_dma_dev(drm, dma_dev);
		put_device(dma_dev);
	} else {
		drm_warn(drm, "No DMA device, buffer sharing with GPUs is unavailable\n");
	}

	ret = fl2000_regmap_init(fl);
	if (ret)
		return ret;

	ret = fl2000_hw_init(fl);
	if (ret) {
		drm_err(drm, "Hardware initialization failed (%d)\n", ret);
		return ret;
	}

	{
		union fl2000_vga_status_reg status = {};

		regmap_read(fl->regmap, FL2000_VGA_STATUS_REG, &status.val);
		dev_dbg(&intf->dev, "status 0x%08x: monitor %d edid %d hdmi %d\n", status.val,
			status.monitor_status, status.edid_status, status.hdmi_status);
	}

	/* Optional HDMI transmitter; without it this is a VGA-only adapter */
	ret = fl2000_it66121_probe(fl);
	if (ret == -ENODEV)
		drm_info(drm, "No IT66121 found, VGA output only\n");
	else if (ret)
		return ret;

	ret = fl2000_it66121_init(fl);
	if (ret)
		drm_warn(drm, "IT66121 initialization failed (%d)\n", ret);

	ret = fl2000_stream_init(fl);
	if (ret)
		return ret;

	ret = fl2000_modeset_init(fl);
	if (ret)
		return ret;

	drmm_kms_helper_poll_init(drm);

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	drm_client_setup(drm, NULL);

	return 0;
}

static void fl2000_disconnect(struct usb_interface *intf)
{
	struct fl2000_device *fl = usb_get_intfdata(intf);
	struct drm_device *drm = &fl->drm;

	drm_kms_helper_poll_fini(drm);
	drm_dev_unplug(drm);
	drm_atomic_helper_shutdown(drm);
}

static int fl2000_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct fl2000_device *fl = usb_get_intfdata(intf);

	return drm_mode_config_helper_suspend(&fl->drm);
}

static int fl2000_resume(struct usb_interface *intf)
{
	struct fl2000_device *fl = usb_get_intfdata(intf);
	int ret;

	ret = fl2000_hw_init(fl);
	if (!ret)
		ret = fl2000_it66121_init(fl);
	if (ret)
		drm_err(&fl->drm, "Hardware re-initialization failed (%d)\n", ret);

	return drm_mode_config_helper_resume(&fl->drm);
}

static const struct usb_device_id fl2000_id_table[] = {
	{ USB_DEVICE_INTERFACE_NUMBER(USB_VENDOR_FRESCO_LOGIC, USB_PRODUCT_FL2000,
				      FL2000_USBIF_AVCONTROL) },
	{},
};
MODULE_DEVICE_TABLE(usb, fl2000_id_table);

static struct usb_driver fl2000_driver = {
	.name = DRIVER_NAME,
	.probe = fl2000_probe,
	.disconnect = fl2000_disconnect,
	.suspend = fl2000_suspend,
	.resume = fl2000_resume,
	.reset_resume = fl2000_resume,
	.id_table = fl2000_id_table,
	.supports_autosuspend = false,
	.disable_hub_initiated_lpm = true,
};

module_usb_driver(fl2000_driver);

MODULE_AUTHOR("Artem Mygaiev");
MODULE_AUTHOR("Samuel");
MODULE_DESCRIPTION("Fresco Logic FL2000 USB display driver");
MODULE_LICENSE("GPL");
