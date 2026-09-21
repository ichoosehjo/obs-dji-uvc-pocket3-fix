/*
 * dji-decode.c — FFmpeg decode layer.
 *
 * Software decode by default; optional hwaccel (cuda / d3d11va / vaapi /
 * videotoolbox) with automatic software fallback.  Output is normalized to
 * NV12 (hw frames are transferred to system memory; odd sw formats go
 * through swscale).
 *
 * License: GPL-2.0-or-later
 */
#include "dji-decode.h"

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DJI_LOG
#ifdef DJI_HAVE_OBS
#include <util/base.h>
#define DJI_LOG(fmt, ...) blog(LOG_INFO, "[dji-uvc] " fmt, ##__VA_ARGS__)
#define DJI_WARN(fmt, ...) blog(LOG_WARNING, "[dji-uvc] " fmt, ##__VA_ARGS__)
#else
#define DJI_LOG(fmt, ...) fprintf(stderr, "[dji-uvc] " fmt "\n", ##__VA_ARGS__)
#define DJI_WARN DJI_LOG
#endif
#endif

#define FOURCC(a, b, c, d) \
	((int)(uint32_t)((a) | ((b) << 8) | ((c) << 16) | ((d) << 24)))
#define FCC_H264 FOURCC('H', '2', '6', '4')
#define FCC_H265 FOURCC('H', '2', '6', '5')

struct dji_decoder {
	AVCodecContext *cctx;
	AVBufferRef *hw_device;
	enum AVPixelFormat hw_pix_fmt;
	AVPacket *pkt;
	AVFrame *frame;    /* decode output (maybe hw)      */
	AVFrame *sw_frame; /* hw transfer target            */
	AVFrame *nv12;     /* swscale target when needed    */
	struct SwsContext *sws;
	dji_frame_cb cb;
	void *opaque;
	bool got_key; /* only expose decoded frames after first keyframe */
};

static enum AVHWDeviceType hw_to_av(enum dji_hw hw)
{
	switch (hw) {
	case DJI_HW_CUDA:         return AV_HWDEVICE_TYPE_CUDA;
	case DJI_HW_D3D11VA:      return AV_HWDEVICE_TYPE_D3D11VA;
	case DJI_HW_VAAPI:        return AV_HWDEVICE_TYPE_VAAPI;
	case DJI_HW_VIDEOTOOLBOX: return AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
	default:                  return AV_HWDEVICE_TYPE_NONE;
	}
}

static enum AVPixelFormat get_hw_format(AVCodecContext *ctx,
					const enum AVPixelFormat *fmts)
{
	struct dji_decoder *d = ctx->opaque;
	for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++) {
		if (*p == d->hw_pix_fmt)
			return *p;
	}
	DJI_WARN("hw pixel format unavailable, falling back to software");
	return fmts[0];
}

struct dji_decoder *dji_decoder_create(int codec_fourcc, enum dji_hw hw,
				       dji_frame_cb cb, void *opaque)
{
	struct dji_decoder *d = calloc(1, sizeof(*d));
	if (!d)
		return NULL;
	d->cb = cb;
	d->opaque = opaque;

	enum AVCodecID cid = (codec_fourcc == FCC_H265)
				     ? AV_CODEC_ID_HEVC
				     : AV_CODEC_ID_H264;
	const AVCodec *codec = avcodec_find_decoder(cid);
	if (!codec)
		goto fail;

	d->cctx = avcodec_alloc_context3(codec);
	if (!d->cctx)
		goto fail;
	d->cctx->opaque = d;
	/* Low-latency knobs: no frame-threads reordering delay. */
	d->cctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
	d->cctx->thread_type = FF_THREAD_SLICE;
	d->cctx->thread_count = 0;

	if (hw != DJI_HW_NONE) {
		enum AVHWDeviceType t = hw_to_av(hw);
		const AVCodecHWConfig *cfg = NULL;
		for (int i = 0;; i++) {
			cfg = avcodec_get_hw_config(codec, i);
			if (!cfg)
				break;
			if ((cfg->methods &
			     AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
			    cfg->device_type == t) {
				d->hw_pix_fmt = cfg->pix_fmt;
				break;
			}
		}
		if (cfg &&
		    av_hwdevice_ctx_create(&d->hw_device, t, NULL, NULL, 0) ==
			    0) {
			d->cctx->hw_device_ctx =
				av_buffer_ref(d->hw_device);
			d->cctx->get_format = get_hw_format;
			DJI_LOG("hardware decode: %s",
				av_hwdevice_get_type_name(t));
		} else {
			DJI_WARN("hwaccel init failed — software decode");
		}
	}

	if (avcodec_open2(d->cctx, codec, NULL) < 0)
		goto fail;

	d->pkt = av_packet_alloc();
	d->frame = av_frame_alloc();
	d->sw_frame = av_frame_alloc();
	d->nv12 = av_frame_alloc();
	if (!d->pkt || !d->frame || !d->sw_frame || !d->nv12)
		goto fail;

	return d;
fail:
	dji_decoder_destroy(d);
	return NULL;
}

static void deliver(struct dji_decoder *d, AVFrame *f, int64_t pts_ns)
{
	AVFrame *out = f;

	if (f->format != AV_PIX_FMT_NV12 && f->format != AV_PIX_FMT_YUV420P) {
		/* Normalize odd formats (e.g. P010) through swscale. */
		d->sws = sws_getCachedContext(d->sws, f->width, f->height,
					      f->format, f->width, f->height,
					      AV_PIX_FMT_NV12,
					      SWS_FAST_BILINEAR, NULL, NULL,
					      NULL);
		if (!d->sws)
			return;
		d->nv12->format = AV_PIX_FMT_NV12;
		d->nv12->width = f->width;
		d->nv12->height = f->height;
		if (av_frame_get_buffer(d->nv12, 32) < 0)
			return;
		sws_scale(d->sws, (const uint8_t *const *)f->data,
			  f->linesize, 0, f->height, d->nv12->data,
			  d->nv12->linesize);
		out = d->nv12;
	}

	struct dji_decoded dec = {
		.data = {out->data[0], out->data[1], out->data[2]},
		.linesize = {out->linesize[0], out->linesize[1],
			     out->linesize[2]},
		.width = out->width,
		.height = out->height,
		.format = out->format,
		.pts_ns = pts_ns,
	};
	d->cb(d->opaque, &dec);

	if (out == d->nv12)
		av_frame_unref(d->nv12);
}

void dji_decoder_push_au(struct dji_decoder *d, const uint8_t *au,
			 size_t size, bool keyframe, int64_t pts_ns)
{
	if (!d || !d->cctx)
		return;

	/* IMPORTANT: feed pre-key AUs to FFmpeg as well. DJI may send SPS/PPS
	 * in an AU before the first IDR after capture starts. Dropping those AUs
	 * here loses the parameter sets, causing "non-existing PPS 0" forever
	 * when the IDR arrives. FFmpeg can safely parse/retain parameter sets
	 * from pre-key packets; we simply suppress output until a keyframe. */
	if (keyframe && !d->got_key) {
		d->got_key = true;
		DJI_LOG("decoder synchronized at first keyframe");
	}

	av_packet_unref(d->pkt);
	if (av_new_packet(d->pkt, (int)size) < 0)
		return;
	memcpy(d->pkt->data, au, size);
	d->pkt->pts = pts_ns;
	d->pkt->dts = pts_ns;
	if (keyframe)
		d->pkt->flags |= AV_PKT_FLAG_KEY;

	if (avcodec_send_packet(d->cctx, d->pkt) < 0)
		return;

	while (avcodec_receive_frame(d->cctx, d->frame) == 0) {
		/* Do not expose potentially incomplete pre-IDR pictures to OBS. */
		if (!d->got_key) {
			av_frame_unref(d->frame);
			continue;
		}

		AVFrame *f = d->frame;
		if (d->hw_device && f->format == d->hw_pix_fmt) {
			av_frame_unref(d->sw_frame);
			if (av_hwframe_transfer_data(d->sw_frame, f, 0) < 0) {
				av_frame_unref(f);
				continue;
			}
			d->sw_frame->width = f->width;
			d->sw_frame->height = f->height;
			deliver(d, d->sw_frame,
				f->pts != AV_NOPTS_VALUE ? f->pts : pts_ns);
		} else {
			deliver(d, f,
				f->pts != AV_NOPTS_VALUE ? f->pts : pts_ns);
		}
		av_frame_unref(d->frame);
	}
}

void dji_decoder_destroy(struct dji_decoder *d)
{
	if (!d)
		return;
	if (d->sws)
		sws_freeContext(d->sws);
	av_frame_free(&d->nv12);
	av_frame_free(&d->sw_frame);
	av_frame_free(&d->frame);
	av_packet_free(&d->pkt);
	avcodec_free_context(&d->cctx);
	av_buffer_unref(&d->hw_device);
	free(d);
}
