/*
 * dji-capture-mf.h — Windows-native capture backend via Media Foundation.
 *
 * Uses the stock Windows UVC driver (no Zadig/WinUSB): selects the camera's
 * native compressed H.264 media type and delivers raw elementary-stream
 * payloads.  The camera keeps working as a normal Windows webcam for other
 * apps when not in use.
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

struct dji_mf_device_info {
	char name[128];
	char symlink[512]; /* MF symbolic link (contains vid_2ca3) */
};

struct dji_mf_format_info {
	int fourcc; /* 'H264' only for now */
	int width;
	int height;
	int fps;
};

typedef void (*dji_mf_payload_cb)(void *opaque, const uint8_t *data,
				  size_t size);

struct dji_mf_capture; /* opaque */

/* Enumerate DJI cameras visible through Media Foundation. */
int dji_mf_enumerate(struct dji_mf_device_info *out, int max);

/* Open + start streaming compressed H.264. width/height/fps: 0 = highest. */
struct dji_mf_capture *dji_mf_start(const struct dji_mf_device_info *dev,
				    int width, int height, int fps,
				    dji_mf_payload_cb cb, void *opaque,
				    char *errbuf, size_t errbuf_size);

void dji_mf_get_format(struct dji_mf_capture *c,
		       struct dji_mf_format_info *f);

void dji_mf_stop(struct dji_mf_capture *c);

#ifdef __cplusplus
}
#endif
