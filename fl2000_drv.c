// SPDX-License-Identifier: GPL-2.0
/*
 * Fresco Logic FL2000 USB display driver (USB 3.0 to VGA / HDMI adapters)
 *
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2020, Artem Mygaiev
 * (C) Copyright 2026, Samuel
 */

#include <linux/module.h>
#include <linux/slab.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_shmem.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>

#include "fl2000.h"

#define USB_VENDOR_FRESCO_LOGIC 0x1D5C
#define USB_PRODUCT_FL2000	0x2000

#define FL2000_USBIF_INTERRUPT 2

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

/*
 * Interrupt endpoint (EP3 IN of interface 2): FL2000 signals VGA status changes there. As in
 * Fresco's driver every interrupt is acknowledged by reading the status register, which also
 * clears its event bits; left unserviced the interrupt packets pile up in the device.
 */
static void fl2000_intr_work(struct work_struct *work)
{
	struct fl2000_device *fl = container_of(work, struct fl2000_device, intr_work);
	union fl2000_vga_status_reg status = {};
	int idx, ret;

	if (!drm_dev_enter(&fl->drm, &idx))
		return;

	if (!regmap_read(fl->regmap, FL2000_VGA_STATUS_REG, &status.val)) {
		dev_dbg(&fl->intf->dev, "interrupt, status 0x%08x\n", status.val);
		if (status.monitor_event || status.edid_event || status.hdmi_event)
			drm_kms_helper_hotplug_event(&fl->drm);
	}

	ret = usb_submit_urb(fl->intr_urb, GFP_KERNEL);
	if (ret && ret != -EPERM && ret != -ENODEV)
		drm_err(&fl->drm, "Interrupt URB submission failed (%d)\n", ret);

	drm_dev_exit(idx);
}

static void fl2000_intr_complete(struct urb *urb)
{
	struct fl2000_device *fl = urb->context;

	switch (urb->status) {
	case -ENOENT: /* Killed */
	case -ECONNRESET:
	case -ESHUTDOWN: /* Unplugged */
	case -ENODEV:
		return;
	default:
		/* Also errors: the work reads status and resubmits */
		schedule_work(&fl->intr_work);
	}
}

static void fl2000_intr_stop(struct fl2000_device *fl)
{
	if (!fl->intr_urb)
		return;

	/* Poisoned URB cannot be resubmitted by the work */
	usb_poison_urb(fl->intr_urb);
	cancel_work_sync(&fl->intr_work);
}

static void fl2000_intr_start(struct fl2000_device *fl)
{
	int ret;

	if (!fl->intr_urb)
		return;

	ret = usb_submit_urb(fl->intr_urb, GFP_KERNEL);
	if (ret)
		drm_err(&fl->drm, "Interrupt URB submission failed (%d)\n", ret);
}

static void fl2000_intr_release(struct drm_device *drm, void *data)
{
	struct fl2000_device *fl = data;

	fl2000_intr_stop(fl);
	usb_free_urb(fl->intr_urb);
	kfree(fl->intr_buf);
}

static int fl2000_intr_init(struct fl2000_device *fl)
{
	struct usb_interface *iface = usb_ifnum_to_if(fl->udev, FL2000_USBIF_INTERRUPT);
	struct usb_endpoint_descriptor *ep;

	INIT_WORK(&fl->intr_work, fl2000_intr_work);

	if (!iface || usb_find_int_in_endpoint(iface->cur_altsetting, &ep)) {
		drm_warn(&fl->drm, "No interrupt endpoint, monitor changes are polled\n");
		return 0;
	}

	fl->intr_buf = kmalloc(usb_endpoint_maxp(ep), GFP_KERNEL);
	fl->intr_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!fl->intr_buf || !fl->intr_urb) {
		usb_free_urb(fl->intr_urb);
		fl->intr_urb = NULL;
		kfree(fl->intr_buf);
		return -ENOMEM;
	}

	usb_fill_int_urb(fl->intr_urb, fl->udev, usb_rcvintpipe(fl->udev, ep->bEndpointAddress),
			 fl->intr_buf, usb_endpoint_maxp(ep), fl2000_intr_complete, fl,
			 ep->bInterval);

	return drmm_add_action_or_reset(&fl->drm, fl2000_intr_release, fl);
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

	ret = fl2000_intr_init(fl);
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
	fl2000_intr_start(fl);

	return 0;
}

static void fl2000_disconnect(struct usb_interface *intf)
{
	struct fl2000_device *fl = usb_get_intfdata(intf);
	struct drm_device *drm = &fl->drm;

	fl2000_intr_stop(fl);
	drm_kms_helper_poll_fini(drm);
	drm_dev_unplug(drm);
	drm_atomic_helper_shutdown(drm);
}

static int fl2000_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct fl2000_device *fl = usb_get_intfdata(intf);

	fl2000_intr_stop(fl);
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

	if (fl->intr_urb)
		usb_unpoison_urb(fl->intr_urb);
	fl2000_intr_start(fl);
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
