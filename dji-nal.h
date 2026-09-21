/*
 * dji-nal.h — Annex-B NAL scanner / access-unit assembler.
 *
 * The DJI encoded-UVC stream delivers H.264 (Annex-B byte stream) in UVC
 * payload frames.  Frame boundaries from libuvc *usually* align with access
 * units, but not always (SPS/PPS may arrive split, and some firmware splits
 * large IDR frames across UVC frames).  This module reassembles complete
 * access units and reports keyframe status.
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

enum dji_nal_codec {
	DJI_NAL_H264 = 0,
	DJI_NAL_H265 = 1,
};

struct dji_nal_asm {
	enum dji_nal_codec codec;

	uint8_t *buf;      /* accumulation buffer (Annex-B)          */
	size_t len;        /* bytes currently buffered               */
	size_t cap;        /* buffer capacity                        */

	bool have_sps;     /* parameter sets seen at least once      */
	bool have_pps;
	bool cur_is_key;   /* pending AU contains an IDR/IRAP slice  */
	bool cur_has_slice;/* pending AU contains at least one slice */
	size_t scan_floor; /* resume offset for incremental scanning   */
};

void dji_nal_init(struct dji_nal_asm *a, enum dji_nal_codec codec);
void dji_nal_free(struct dji_nal_asm *a);
void dji_nal_reset(struct dji_nal_asm *a);

/*
 * Feed one UVC payload frame worth of Annex-B bytes.
 *
 * on_au() is invoked zero or more times with a complete access unit
 * (Annex-B, starts with a start code).  The pointer is only valid for the
 * duration of the callback.
 */
typedef void (*dji_nal_au_cb)(void *opaque, const uint8_t *au, size_t size,
			      bool keyframe);

void dji_nal_push(struct dji_nal_asm *a, const uint8_t *data, size_t size,
		  dji_nal_au_cb on_au, void *opaque);

/* Flush whatever is pending as a final AU (end of stream). */
void dji_nal_flush(struct dji_nal_asm *a, dji_nal_au_cb on_au, void *opaque);

#ifdef __cplusplus
}
#endif
