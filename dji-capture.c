/*
 * dji-capture.c — libuvc-based encoded-UVC capture for DJI Osmo cameras.
 *
 * Mirrors what BELABOX's gstlibuvch264src does: probe/commit the frame-based
 * H.264 format descriptor via libuvc and stream the elementary bitstream.
 * The OS webcam stack (usbvideo.sys / DirectShow) never exposes this
 * descriptor, which is why OBS's stock Video Capture Device tops out at the
 * YUV/MJPEG modes (4K30 on the Pocket 3).
 *
 * License: GPL-2.0-or-later
 */
#include "dji-capture.h"

#include <libuvc/libuvc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FOURCC(a, b, c, d) \
	((int)(uint32_t)((a) | ((b) << 8) | ((c) << 16) | ((d) << 24)))
#define FCC_H264 FOURCC('H', '2', '6', '4')
#define FCC_H265 FOURCC('H', '2', '6', '5')

/* GUID prefixes: frame-based formats carry the FourCC in the first 4 bytes
 * of guidFormat, followed by 00 00 10 00 80 00 00 AA 00 38 9B 71. */
static int guid_fourcc(const uint8_t *guid)
{
	return FOURCC(guid[0], guid[1], guid[2], guid[3]);
}

struct dji_capture {
	uvc_context_t *ctx;
	uvc_device_t *dev;
	uvc_device_handle_t *devh;
	uvc_stream_ctrl_t ctrl;
	struct dji_format_info fmt;
	dji_payload_cb cb;
	void *opaque;
	bool streaming;
};

/* ---------------------------------------------------------------------- */

static void fill_info(uvc_device_t *dev, struct dji_device_info *out)
{
	memset(out, 0, sizeof(*out));

	uvc_device_descriptor_t *desc = NULL;
	if (uvc_get_device_descriptor(dev, &desc) == UVC_SUCCESS && desc) {
		out->vid = desc->idVendor;
		out->pid = desc->idProduct;
		if (desc->product)
			snprintf(out->name, sizeof(out->name), "%s",
				 desc->product);
		if (desc->serialNumber)
			snprintf(out->serial, sizeof(out->serial), "%s",
				 desc->serialNumber);
		uvc_free_device_descriptor(desc);
	}
	if (!out->name[0]) {
		/* Known PIDs are labels only — VID filter catches everything. */
		const char *n = "DJI camera";
		switch (out->pid) {
		case 0x0054: n = "DJI Osmo Pocket 3"; break;
		default: break;
		}
		snprintf(out->name, sizeof(out->name), "%s", n);
	}
	out->bus = uvc_get_bus_number(dev);
	out->addr = uvc_get_device_address(dev);
}

int dji_capture_enumerate(struct dji_device_info *out, int max)
{
	uvc_context_t *ctx = NULL;
	if (uvc_init(&ctx, NULL) != UVC_SUCCESS)
		return 0;

	uvc_device_t **list = NULL;
	int n = 0;
	if (uvc_get_device_list(ctx, &list) == UVC_SUCCESS && list) {
		for (int i = 0; list[i] && n < max; i++) {
			uvc_device_descriptor_t *d = NULL;
			if (uvc_get_device_descriptor(list[i], &d) !=
				    UVC_SUCCESS ||
			    !d)
				continue;
			bool is_dji = (d->idVendor == DJI_USB_VID);
			uvc_free_device_descriptor(d);
			if (is_dji)
				fill_info(list[i], &out[n++]);
		}
		uvc_free_device_list(list, 1);
	}
	uvc_exit(ctx);
	return n;
}

/* ---------------------------------------------------------------------- */

static uvc_device_t *find_device(uvc_context_t *ctx,
				 const struct dji_device_info *want)
{
	uvc_device_t **list = NULL;
	uvc_device_t *found = NULL;

	if (uvc_get_device_list(ctx, &list) != UVC_SUCCESS || !list)
		return NULL;

	uvc_device_t *fallback = NULL;
	for (int i = 0; list[i]; i++) {
		uvc_device_descriptor_t *d = NULL;
		if (uvc_get_device_descriptor(list[i], &d) != UVC_SUCCESS ||
		    !d)
			continue;
		bool is_dji = (d->idVendor == DJI_USB_VID);
		bool serial_match =
			want->serial[0] && d->serialNumber &&
			strcmp(want->serial, d->serialNumber) == 0;
		uvc_free_device_descriptor(d);
		if (!is_dji)
			continue;

		if (serial_match) {
			found = list[i];
			uvc_ref_device(found);
			break;
		}
		if (!fallback &&
		    uvc_get_bus_number(list[i]) == want->bus &&
		    uvc_get_device_address(list[i]) == want->addr) {
			fallback = list[i];
			uvc_ref_device(fallback);
		}
	}
	if (!found)
		found = fallback;
	else if (fallback)
		uvc_unref_device(fallback);

	uvc_free_device_list(list, 1);
	return found;
}

/*
 * Walk the format descriptors on an *open* handle and collect frame-based
 * H264/H265 modes.  libuvc exposes this via uvc_get_format_descs().
 */
static int list_formats_on_handle(uvc_device_handle_t *devh,
				  struct dji_format_info *out, int max)
{
	int n = 0;
	const uvc_format_desc_t *fd = uvc_get_format_descs(devh);
	for (; fd && n < max; fd = fd->next) {
		if (fd->bDescriptorSubtype != UVC_VS_FORMAT_FRAME_BASED)
			continue;
		int fcc = guid_fourcc(fd->guidFormat);
		if (fcc != FCC_H264 && fcc != FCC_H265)
			continue;

		const uvc_frame_desc_t *fr = fd->frame_descs;
		for (; fr && n < max; fr = fr->next) {
			/* Default interval first; enumerate the discrete
			 * interval table if present. */
			if (fr->intervals) {
				for (uint32_t *iv = fr->intervals;
				     *iv && n < max; iv++) {
					out[n].fourcc = fcc;
					out[n].width = fr->wWidth;
					out[n].height = fr->wHeight;
					out[n].fps =
						(int)(10000000.0 / *iv + 0.5);
					n++;
				}
			} else {
				out[n].fourcc = fcc;
				out[n].width = fr->wWidth;
				out[n].height = fr->wHeight;
				out[n].fps = (int)(10000000.0 /
							   fr->dwDefaultFrameInterval +
						   0.5);
				n++;
			}
		}
	}
	return n;
}

int dji_capture_list_formats(const struct dji_device_info *dev,
			     struct dji_format_info *out, int max)
{
	uvc_context_t *ctx = NULL;
	if (uvc_init(&ctx, NULL) != UVC_SUCCESS)
		return 0;

	int n = 0;
	uvc_device_t *d = find_device(ctx, dev);
	if (d) {
		uvc_device_handle_t *h = NULL;
		if (uvc_open(d, &h) == UVC_SUCCESS) {
			n = list_formats_on_handle(h, out, max);
			uvc_close(h);
		}
		uvc_unref_device(d);
	}
	uvc_exit(ctx);
	return n;
}

/* ---------------------------------------------------------------------- */

static void stream_cb(uvc_frame_t *frame, void *opaque)
{
	struct dji_capture *c = opaque;
	if (!frame || !frame->data || !frame->data_bytes)
		return;
	c->cb(c->opaque, (const uint8_t *)frame->data, frame->data_bytes);
}

struct dji_capture *dji_capture_start(const struct dji_device_info *dev,
				      int fourcc, int width, int height,
				      int fps, dji_payload_cb cb,
				      void *opaque, char *errbuf,
				      size_t errbuf_size)
{
#define FAIL(...)                                              \
	do {                                                   \
		if (errbuf)                                    \
			snprintf(errbuf, errbuf_size,          \
				 __VA_ARGS__);                 \
		goto fail;                                     \
	} while (0)

	struct dji_capture *c = calloc(1, sizeof(*c));
	if (!c)
		return NULL;
	c->cb = cb;
	c->opaque = opaque;

	if (uvc_init(&c->ctx, NULL) != UVC_SUCCESS)
		FAIL("libuvc init failed (libusb backend unavailable?)");

	c->dev = find_device(c->ctx, dev);
	if (!c->dev)
		FAIL("camera not found (unplugged, or WinUSB not bound "
		     "via Zadig?)");

	uvc_error_t rc = uvc_open(c->dev, &c->devh);
	if (rc != UVC_SUCCESS)
		FAIL("uvc_open failed: %s (driver conflict — Windows needs "
		     "the camera interface bound to WinUSB with Zadig)",
		     uvc_strerror(rc));

	/* Pick the target format among advertised encoded modes. */
	struct dji_format_info fmts[64];
	int nf = list_formats_on_handle(c->devh, fmts, 64);
	if (nf == 0)
		FAIL("no frame-based H.264/H.265 descriptor advertised — "
		     "is the camera in Webcam mode?");

	int best = -1;
	long best_score = -1;
	for (int i = 0; i < nf; i++) {
		if (fourcc && fmts[i].fourcc != fourcc)
			continue;
		if (width && fmts[i].width != width)
			continue;
		if (height && fmts[i].height != height)
			continue;
		if (fps && fmts[i].fps != fps)
			continue;
		long score = (long)fmts[i].width * fmts[i].height +
			     fmts[i].fps;
		if (score > best_score) {
			best_score = score;
			best = i;
		}
	}
	if (best < 0)
		FAIL("requested mode not advertised by the camera");
	c->fmt = fmts[best];

	enum uvc_frame_format ff = UVC_FRAME_FORMAT_H264;
#ifdef UVC_FRAME_FORMAT_H265
	if (c->fmt.fourcc == FCC_H265)
		ff = UVC_FRAME_FORMAT_H265;
#endif

	rc = uvc_get_stream_ctrl_format_size(c->devh, &c->ctrl, ff,
					     c->fmt.width, c->fmt.height,
					     c->fmt.fps);
	if (rc != UVC_SUCCESS)
		FAIL("probe/commit failed for %dx%d@%d: %s", c->fmt.width,
		     c->fmt.height, c->fmt.fps, uvc_strerror(rc));

	rc = uvc_start_streaming(c->devh, &c->ctrl, stream_cb, c, 0);
	if (rc != UVC_SUCCESS)
		FAIL("uvc_start_streaming failed: %s", uvc_strerror(rc));

	c->streaming = true;
	return c;

fail:
	dji_capture_stop(c);
	return NULL;
#undef FAIL
}

void dji_capture_get_format(struct dji_capture *c, struct dji_format_info *f)
{
	*f = c->fmt;
}

void dji_capture_stop(struct dji_capture *c)
{
	if (!c)
		return;
	if (c->streaming)
		uvc_stop_streaming(c->devh);
	if (c->devh)
		uvc_close(c->devh);
	if (c->dev)
		uvc_unref_device(c->dev);
	if (c->ctx)
		uvc_exit(c->ctx);
	free(c);
}
