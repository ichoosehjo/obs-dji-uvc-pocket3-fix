/*
 * dji-decode.h — FFmpeg decode layer (H.264/H.265 AU -> NV12 frames).
 * License: GPL-2.0-or-later
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum dji_hw {
	DJI_HW_NONE = 0,   /* software                    */
	DJI_HW_CUDA,       /* NVIDIA NVDEC                */
	DJI_HW_D3D11VA,    /* Windows Intel/AMD/NVIDIA    */
	DJI_HW_VAAPI,      /* Linux                       */
	DJI_HW_VIDEOTOOLBOX,
};

struct dji_decoded {
	const uint8_t *data[3];
	int linesize[3];
	int width;
	int height;
	int format; /* AV_PIX_FMT_* — NV12 after transfer/convert */
	int64_t pts_ns;
};

typedef void (*dji_frame_cb)(void *opaque, const struct dji_decoded *frame);

struct dji_decoder; /* opaque */

/* codec_fourcc: 'H264' or 'H265'. Falls back to software if hw init fails
 * (logged via blog). */
struct dji_decoder *dji_decoder_create(int codec_fourcc, enum dji_hw hw,
				       dji_frame_cb cb, void *opaque);

/* Feed one complete Annex-B access unit. pts_ns: capture timestamp. */
void dji_decoder_push_au(struct dji_decoder *d, const uint8_t *au,
			 size_t size, bool keyframe, int64_t pts_ns);

void dji_decoder_destroy(struct dji_decoder *d);

#ifdef __cplusplus
}
#endif
