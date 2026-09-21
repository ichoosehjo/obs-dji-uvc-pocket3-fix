/*
 * dji-nal.c — Annex-B NAL scanner / access-unit assembler.
 *
 * Strategy: scan for start codes incrementally.  As soon as a NAL's header
 * byte(s) (+1 payload byte for slices) are buffered, we can decide whether it
 * begins a new access unit — the NAL does not need to be complete.  Emission
 * happens at that boundary; the tail stays buffered.
 *
 * License: GPL-2.0-or-later
 */
#include "dji-nal.h"

#include <stdlib.h>
#include <string.h>

#define DJI_NAL_INITIAL_CAP (1 << 20) /* 1 MiB */

struct priv_state {
	size_t next_scan; /* stored in a->buf-independent offset */
};

void dji_nal_init(struct dji_nal_asm *a, enum dji_nal_codec codec)
{
	memset(a, 0, sizeof(*a));
	a->codec = codec;
	a->buf = malloc(DJI_NAL_INITIAL_CAP);
	a->cap = a->buf ? DJI_NAL_INITIAL_CAP : 0;
}

void dji_nal_free(struct dji_nal_asm *a)
{
	free(a->buf);
	memset(a, 0, sizeof(*a));
}

void dji_nal_reset(struct dji_nal_asm *a)
{
	a->len = 0;
	a->cur_is_key = false;
	a->cur_has_slice = false;
}

static bool ensure_cap(struct dji_nal_asm *a, size_t need)
{
	if (need <= a->cap)
		return true;
	size_t ncap = a->cap ? a->cap : DJI_NAL_INITIAL_CAP;
	while (ncap < need)
		ncap *= 2;
	uint8_t *nb = realloc(a->buf, ncap);
	if (!nb)
		return false;
	a->buf = nb;
	a->cap = ncap;
	return true;
}

/* --- NAL classification ------------------------------------------------ */

enum nal_kind {
	NAL_OTHER = 0,
	NAL_SLICE,
	NAL_SLICE_IDR,
	NAL_SPS,
	NAL_PPS,
	NAL_AUD,
	NAL_SEI,
};

static enum nal_kind classify_h264(uint8_t b)
{
	switch (b & 0x1f) {
	case 1:  return NAL_SLICE;
	case 5:  return NAL_SLICE_IDR;
	case 6:  return NAL_SEI;
	case 7:  return NAL_SPS;
	case 8:  return NAL_PPS;
	case 9:  return NAL_AUD;
	default: return NAL_OTHER;
	}
}

static enum nal_kind classify_h265(uint8_t b)
{
	uint8_t t = (b >> 1) & 0x3f;
	if (t <= 9)
		return NAL_SLICE;
	if (t >= 16 && t <= 23)
		return NAL_SLICE_IDR; /* IRAP */
	switch (t) {
	case 32:
	case 33: return NAL_SPS; /* VPS/SPS */
	case 34: return NAL_PPS;
	case 35: return NAL_AUD;
	case 39:
	case 40: return NAL_SEI;
	default: return NAL_OTHER;
	}
}

/*
 * H.264: first_mb_in_slice is ue(v); value 0 <=> first RBSP bit is 1.
 * H.265: first_slice_segment_in_pic_flag is the first RBSP bit.
 * Both: MSB of first payload byte set => this slice starts a new picture.
 */
static bool slice_new_picture(const uint8_t *payload)
{
	return (payload[0] & 0x80) != 0;
}

/*
 * Find next 00 00 01 pattern at/after `from`.  Returns the offset of the
 * start code (extended to 4 bytes if a leading zero precedes it), or `len`.
 */
static size_t find_start_code(const uint8_t *p, size_t len, size_t from,
			      size_t *sc_len)
{
	if (len < 3) {
		*sc_len = 0;
		return len;
	}
	for (size_t i = from; i + 3 <= len; i++) {
		if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
			if (i > 0 && p[i - 1] == 0) {
				*sc_len = 4;
				return i - 1;
			}
			*sc_len = 3;
			return i;
		}
	}
	*sc_len = 0;
	return len;
}

static void emit_au(struct dji_nal_asm *a, size_t upto, dji_nal_au_cb on_au,
		    void *opaque)
{
	if (upto == 0)
		return;
	if (on_au && a->cur_has_slice)
		on_au(opaque, a->buf, upto, a->cur_is_key);
	memmove(a->buf, a->buf + upto, a->len - upto);
	a->len -= upto;
	a->cur_is_key = false;
	a->cur_has_slice = false;
}

void dji_nal_push(struct dji_nal_asm *a, const uint8_t *data, size_t size,
		  dji_nal_au_cb on_au, void *opaque)
{
	if (!size || !ensure_cap(a, a->len + size))
		return;

	memcpy(a->buf + a->len, data, size);
	a->len += size;

	const size_t hdr_len = (a->codec == DJI_NAL_H264) ? 1 : 2;

	/*
	 * Rescan from slightly before the appended data so start codes that
	 * straddle push boundaries are found.  Reclassifying an already-seen
	 * NAL is harmless *except* that a boundary would double-emit — we
	 * prevent that by tracking a per-buffer scan floor.
	 */
	size_t scan = a->scan_floor;

	for (;;) {
		size_t sc_len = 0;
		size_t off = find_start_code(a->buf, a->len, scan, &sc_len);
		if (off >= a->len)
			break;

		size_t hdr = off + sc_len;
		size_t need = hdr + hdr_len; /* bytes to classify */
		if (need > a->len) {
			/* header not yet buffered — retry on next push */
			a->scan_floor = off;
			return;
		}

		enum nal_kind kind =
			(a->codec == DJI_NAL_H264)
				? classify_h264(a->buf[hdr])
				: classify_h265(a->buf[hdr]);

		if ((kind == NAL_SLICE || kind == NAL_SLICE_IDR) &&
		    need + 1 > a->len) {
			/* need first payload byte for new-picture test */
			a->scan_floor = off;
			return;
		}

		bool boundary = false;
		switch (kind) {
		case NAL_AUD:
		case NAL_SPS:
		case NAL_PPS:
			if (kind == NAL_SPS)
				a->have_sps = true;
			if (kind == NAL_PPS)
				a->have_pps = true;
			boundary = a->cur_has_slice;
			break;
		case NAL_SLICE:
		case NAL_SLICE_IDR:
			boundary = a->cur_has_slice &&
				   slice_new_picture(a->buf + hdr + hdr_len);
			break;
		default:
			break;
		}

		if (boundary) {
			size_t shift = off;
			emit_au(a, off, on_au, opaque);
			off -= shift;
			hdr -= shift;
		}

		if (kind == NAL_SLICE || kind == NAL_SLICE_IDR) {
			a->cur_has_slice = true;
			if (kind == NAL_SLICE_IDR)
				a->cur_is_key = true;
		}

		scan = hdr + hdr_len; /* resume after this NAL's header */
		a->scan_floor = scan;
	}

	a->scan_floor = (a->len >= 3) ? a->len - 3 : 0;
}

void dji_nal_flush(struct dji_nal_asm *a, dji_nal_au_cb on_au, void *opaque)
{
	if (a->len && a->cur_has_slice)
		emit_au(a, a->len, on_au, opaque);
	dji_nal_reset(a);
	a->scan_floor = 0;
}
