// SPDX-License-Identifier: GPL-2.0
/*
 * Frame streaming. FL2000 has no frame memory: it must be fed whole frames over bulk endpoint 1
 * continuously, so the URBs keep re-sending the latest complete frame until a newer one is ready.
 *
 * Buffer life: writer takes a free buffer, fills it and publishes it as "latest"; each completing
 * URB picks "latest" for its next transfer. With N URBs at most N buffers are in flight, so with
 * N + 2 buffers the writer always finds one that is neither in flight nor latest.
 *
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2020, Artem Mygaiev
 * (C) Copyright 2026, Samuel
 */

#include <linux/slab.h>
#include <linux/vmalloc.h>

#include <drm/drm_managed.h>
#include <drm/drm_print.h>

#include "fl2000.h"

#define FL2000_STREAM_MAX_RETRIES 10

static void fl2000_buf_free(struct fl2000_buf *buf)
{
	if (buf->vaddr)
		vunmap(buf->vaddr);

	sg_free_table(&buf->sgt);

	if (buf->pages) {
		for (unsigned int i = 0; i < buf->nr_pages; i++)
			if (buf->pages[i])
				__free_page(buf->pages[i]);
		kfree(buf->pages);
	}

	memset(buf, 0, sizeof(*buf));
}

static int fl2000_buf_alloc(struct fl2000_buf *buf, size_t size)
{
	unsigned int nr_pages = PAGE_ALIGN(size) >> PAGE_SHIFT;
	int ret;

	buf->pages = kcalloc(nr_pages, sizeof(*buf->pages), GFP_KERNEL);
	if (!buf->pages)
		return -ENOMEM;
	buf->nr_pages = nr_pages;

	for (unsigned int i = 0; i < nr_pages; i++) {
		buf->pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!buf->pages[i])
			goto err_nomem;
	}

	ret = sg_alloc_table_from_pages(&buf->sgt, buf->pages, nr_pages, 0, size, GFP_KERNEL);
	if (ret)
		goto err;

	buf->vaddr = vmap(buf->pages, nr_pages, VM_MAP, PAGE_KERNEL);
	if (!buf->vaddr)
		goto err_nomem;

	return 0;

err_nomem:
	ret = -ENOMEM;
err:
	fl2000_buf_free(buf);
	return ret;
}

static void fl2000_stream_free_bufs(struct fl2000_stream *stream)
{
	for (int i = 0; i < FL2000_STREAM_BUFS; i++)
		fl2000_buf_free(&stream->bufs[i]);
	stream->size = 0;
}

/* Called with stream->lock held */
static void fl2000_stream_submit(struct fl2000_device *fl, int i)
{
	struct fl2000_stream *stream = &fl->stream;
	struct fl2000_buf *buf = &stream->bufs[stream->latest];
	struct urb *urb = stream->urbs[i];
	int ret;

	urb->sg = buf->sgt.sgl;
	urb->num_sgs = buf->sgt.orig_nents;
	urb->transfer_buffer_length = stream->size;

	buf->users++;
	stream->urb_buf[i] = stream->latest;

	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret) {
		buf->users--;
		stream->urb_buf[i] = -1;
		stream->running = false;
		if (ret != -ENODEV && ret != -EPERM)
			drm_err_ratelimited(&fl->drm, "Frame URB submission failed (%d)\n", ret);
	}
}

static void fl2000_stream_complete(struct urb *urb)
{
	struct fl2000_device *fl = urb->context;
	struct fl2000_stream *stream = &fl->stream;
	unsigned long flags;
	int i;

	for (i = 0; i < FL2000_STREAM_URBS; i++)
		if (stream->urbs[i] == urb)
			break;
	if (WARN_ON_ONCE(i == FL2000_STREAM_URBS))
		return;

	spin_lock_irqsave(&stream->lock, flags);

	if (stream->urb_buf[i] >= 0)
		stream->bufs[stream->urb_buf[i]].users--;
	stream->urb_buf[i] = -1;

	switch (urb->status) {
	case 0:
		stream->retries = 0;
		break;
	case -ENOENT: /* Killed by fl2000_stream_stop() or recover work */
	case -ECONNRESET:
	case -ESHUTDOWN: /* Device unplugged */
	case -ENODEV:
		stream->running = false;
		break;
	default:
		/* Transient error or stalled endpoint: restart from process context */
		if (stream->running) {
			stream->running = false;
			stream->error = urb->status;
			if (stream->enabled)
				schedule_work(&stream->recover);
		}
		break;
	}

	if (stream->running)
		fl2000_stream_submit(fl, i);

	spin_unlock_irqrestore(&stream->lock, flags);
}

static void fl2000_stream_kill_urbs(struct fl2000_stream *stream)
{
	/* Waits for completion handlers, which will not resubmit anymore */
	for (int i = 0; i < FL2000_STREAM_URBS; i++)
		usb_kill_urb(stream->urbs[i]);
}

static void fl2000_stream_recover(struct work_struct *work)
{
	struct fl2000_stream *stream = container_of(work, struct fl2000_stream, recover);
	struct fl2000_device *fl = container_of(stream, struct fl2000_device, stream);
	unsigned long flags;
	int error;

	fl2000_stream_kill_urbs(stream);

	spin_lock_irqsave(&stream->lock, flags);
	error = stream->error;
	if (++stream->retries > FL2000_STREAM_MAX_RETRIES) {
		drm_err(&fl->drm, "Frame transfers keep failing (%d), giving up\n", error);
		stream->enabled = false;
	}
	spin_unlock_irqrestore(&stream->lock, flags);

	if (!stream->enabled)
		return;

	drm_dbg(&fl->drm, "Frame transfer failed (%d), restarting\n", error);

	if (error == -EPIPE)
		usb_clear_halt(fl->udev, usb_sndbulkpipe(fl->udev, FL2000_EP_STREAM));

	spin_lock_irqsave(&stream->lock, flags);
	if (stream->enabled && !stream->running) {
		stream->running = true;
		for (int i = 0; i < FL2000_STREAM_URBS && stream->running; i++)
			fl2000_stream_submit(fl, i);
	}
	spin_unlock_irqrestore(&stream->lock, flags);
}

void fl2000_stream_stop(struct fl2000_device *fl)
{
	struct fl2000_stream *stream = &fl->stream;
	unsigned long flags;

	spin_lock_irqsave(&stream->lock, flags);
	stream->enabled = false;
	stream->running = false;
	spin_unlock_irqrestore(&stream->lock, flags);

	cancel_work_sync(&stream->recover);
	fl2000_stream_kill_urbs(stream);
}

int fl2000_stream_start(struct fl2000_device *fl, unsigned int width, unsigned int height,
			unsigned int bytes_pix)
{
	struct fl2000_stream *stream = &fl->stream;
	/* Round buffer size up to multiple of 8 to meet HW expectations */
	size_t size = ALIGN((size_t)width * height * bytes_pix, 8);
	unsigned long flags;
	int ret;

	fl2000_stream_stop(fl);

	if (stream->size != size) {
		fl2000_stream_free_bufs(stream);
		for (int i = 0; i < FL2000_STREAM_BUFS; i++) {
			ret = fl2000_buf_alloc(&stream->bufs[i], size);
			if (ret) {
				fl2000_stream_free_bufs(stream);
				return ret;
			}
		}
		stream->size = size;
	} else {
		/* Same size, but old content is in another pixel format: start from black */
		for (int i = 0; i < FL2000_STREAM_BUFS; i++)
			memset(stream->bufs[i].vaddr, 0, size);
	}

	spin_lock_irqsave(&stream->lock, flags);
	stream->width = width;
	stream->height = height;
	stream->bytes_pix = bytes_pix;
	stream->latest = 0;
	for (int i = 0; i < FL2000_STREAM_BUFS; i++)
		stream->bufs[i].users = 0;
	stream->retries = 0;
	stream->enabled = true;
	stream->running = true;
	for (int i = 0; i < FL2000_STREAM_URBS && stream->running; i++)
		fl2000_stream_submit(fl, i);
	ret = stream->running ? 0 : -EIO;
	spin_unlock_irqrestore(&stream->lock, flags);

	return ret;
}

/**
 * fl2000_stream_begin - get a buffer to draw the next frame into
 * @fl: device
 * @full: caller will redraw the whole frame, no need to copy the latest one
 *
 * Return: buffer index for fl2000_stream_end(), or negative error when not streaming
 */
int fl2000_stream_begin(struct fl2000_device *fl, bool full)
{
	struct fl2000_stream *stream = &fl->stream;
	unsigned long flags;
	int idx = -ENODEV;

	spin_lock_irqsave(&stream->lock, flags);
	if (stream->size) {
		for (int i = 0; i < FL2000_STREAM_BUFS; i++) {
			if (i != stream->latest && !stream->bufs[i].users) {
				stream->bufs[i].users = 1;
				idx = i;
				break;
			}
		}
		WARN_ON_ONCE(idx < 0);
	}
	spin_unlock_irqrestore(&stream->lock, flags);

	/* Only the writer modifies buffers and atomic commits are serialized, so "latest" content
	 * is stable here even without the lock
	 */
	if (idx >= 0 && !full)
		memcpy(stream->bufs[idx].vaddr, stream->bufs[stream->latest].vaddr, stream->size);

	return idx;
}

void fl2000_stream_end(struct fl2000_device *fl, int idx)
{
	struct fl2000_stream *stream = &fl->stream;
	unsigned long flags;

	spin_lock_irqsave(&stream->lock, flags);
	stream->bufs[idx].users--;
	stream->latest = idx;
	spin_unlock_irqrestore(&stream->lock, flags);
}

static void fl2000_stream_release(struct drm_device *drm, void *data)
{
	struct fl2000_device *fl = data;
	struct fl2000_stream *stream = &fl->stream;

	fl2000_stream_stop(fl);
	for (int i = 0; i < FL2000_STREAM_URBS; i++)
		usb_free_urb(stream->urbs[i]);
	fl2000_stream_free_bufs(stream);
}

int fl2000_stream_init(struct fl2000_device *fl)
{
	struct fl2000_stream *stream = &fl->stream;
	int ret;

	spin_lock_init(&stream->lock);
	INIT_WORK(&stream->recover, fl2000_stream_recover);

	for (int i = 0; i < FL2000_STREAM_URBS; i++) {
		struct urb *urb = usb_alloc_urb(0, GFP_KERNEL);

		if (!urb)
			break;

		usb_fill_bulk_urb(urb, fl->udev, usb_sndbulkpipe(fl->udev, FL2000_EP_STREAM), NULL, 0,
				  fl2000_stream_complete, fl);
		/* Short/zero-length packet marks the end of frame */
		urb->transfer_flags |= URB_ZERO_PACKET;
		stream->urbs[i] = urb;
		stream->urb_buf[i] = -1;
	}

	/* Release action also frees whatever URBs were allocated before a failure */
	ret = drmm_add_action_or_reset(&fl->drm, fl2000_stream_release, fl);
	if (ret)
		return ret;

	return stream->urbs[FL2000_STREAM_URBS - 1] ? 0 : -ENOMEM;
}
