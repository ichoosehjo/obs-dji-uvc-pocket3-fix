/*
 * dji-capture.h — libuvc-based encoded-UVC capture for DJI Osmo cameras.
 *
 * Opens the camera via libusb/WinUSB (Windows: bind with Zadig), negotiates
 * the frame-based H.264 descriptor (this is where 4K60 lives — DJI's spec
 * sheet only lists the YUV/MJPEG descriptors), and delivers raw Annex-B
 * payload chunks to a callback.
 *
 * License: GPL-2.0-or-later
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DJI_USB_VID 0x2ca3

struct dji_device_info {
	char name[128];
	char serial[64];
	uint16_t vid;
	uint16_t pid;
	uint8_t bus;
	uint8_t addr;
};

struct dji_format_info {
	int fourcc;   /* 'H264' / 'H265' */
	int width;
	int height;
	int fps;
};

/* Enumerate DJI (VID 0x2CA3) cameras. Returns count, fills up to max. */
int dji_capture_enumerate(struct dji_device_info *out, int max);

/* List encoded (frame-based H264/H265) formats a device advertises. */
int dji_capture_list_formats(const struct dji_device_info *dev,
			     struct dji_format_info *out, int max);

typedef void (*dji_payload_cb)(void *opaque, const uint8_t *data, size_t size);

struct dji_capture; /* opaque */

/*
 * Open + start streaming.
 * fourcc: 'H264' (0x34363248) or 'H265'; width/height/fps: 0 = highest
 * advertised.  cb fires on the libuvc callback thread.
 */
struct dji_capture *dji_capture_start(const struct dji_device_info *dev,
				      int fourcc, int width, int height,
				      int fps, dji_payload_cb cb,
				      void *opaque, char *errbuf,
				      size_t errbuf_size);

/* Actual negotiated format. */
void dji_capture_get_format(struct dji_capture *c, struct dji_format_info *f);

void dji_capture_stop(struct dji_capture *c);

#ifdef __cplusplus
}
#endif
