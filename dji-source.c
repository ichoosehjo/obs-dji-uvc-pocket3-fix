/*
 * dji-source.c — OBS async video source: "DJI UVC Camera (4K60)".
 *
 * Pipeline (per source instance):
 *   libuvc callback thread -> dji-nal AU assembler -> bounded AU queue
 *   decode thread          -> FFmpeg (sw or hw)    -> obs_source_output_video
 *
 * Behaves like the stock Video Capture Device: add source, pick camera,
 * pick mode.  One source per Pocket; multiple sources supported.
 *
 * License: GPL-2.0-or-later
 */
#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/deque.h>

#include <stdlib.h>
#include <string.h>

#include "dji-capture.h"
#ifdef _WIN32
#include "dji-capture-mf.h"
#endif
#include "dji-decode.h"
#include "dji-nal.h"

#define T(s) obs_module_text(s)

#define FOURCC(a, b, c, d) \
	((int)(uint32_t)((a) | ((b) << 8) | ((c) << 16) | ((d) << 24)))
#define FCC_H264 FOURCC('H', '2', '6', '4')
#define FCC_H265 FOURCC('H', '2', '6', '5')

#define MAX_QUEUED_AUS 8 /* backpressure: drop-to-keyframe beyond this */

struct au_item {
	uint8_t *data;
	size_t size;
	bool key;
	int64_t pts_ns;
};

struct dji_src {
	obs_source_t *source;

	/* config */
	struct dji_device_info dev;
	bool dev_valid;
#ifdef _WIN32
	struct dji_mf_device_info mf_dev;
	bool mf_valid;
	struct dji_mf_capture *mf_cap;
#endif
	int fourcc;
	int width, height, fps;
	enum dji_hw hw;

	/* runtime */
	struct dji_capture *cap;
	struct dji_nal_asm nal;
	struct dji_decoder *dec;

	pthread_mutex_t q_lock;
	os_sem_t *q_sem;
	struct deque q;
	size_t q_count;
	bool q_dropping;

	pthread_t decode_thread;
	volatile bool running;
	bool thread_started;

	/* watchdog / auto-reconnect */
	char devkey[640];
	bool want_running;
	volatile long last_payload_s;
	volatile long last_attempt_s;
	volatile bool restarting;
	pthread_t restart_th;
	bool restart_joinable;

	char claimed[640]; /* device-claim registry key we hold, or "" */
};

#define SRC_NAME(s) obs_source_get_name((s)->source)

static long now_s(void)
{
	return (long)(os_gettime_ns() / 1000000000ULL);
}

/* ------------------------------------------------------------------ */
/* Device-claim registry: one physical camera per source.  Prevents    */
/* sources preempting each other (MF error 0xC00D3EA3 ping-pong) and   */
/* lets unconfigured sources auto-pick the first FREE camera so a      */
/* multi-camera scene (e.g. 4x Pocket 3) works without manual setup.   */
/* ------------------------------------------------------------------ */
#define DJI_MAX_CLAIMS 8
static pthread_mutex_t g_claims_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_claims[DJI_MAX_CLAIMS][640];

static bool claim_device(const char *key)
{
	bool ok = false;
	int free_slot = -1;
	pthread_mutex_lock(&g_claims_lock);
	for (int i = 0; i < DJI_MAX_CLAIMS; i++) {
		if (g_claims[i][0] == 0) {
			if (free_slot < 0)
				free_slot = i;
		} else if (strcmp(g_claims[i], key) == 0) {
			goto out; /* already claimed by another source */
		}
	}
	if (free_slot >= 0) {
		snprintf(g_claims[free_slot], sizeof(g_claims[free_slot]),
			 "%s", key);
		ok = true;
	}
out:
	pthread_mutex_unlock(&g_claims_lock);
	return ok;
}

static bool device_is_claimed(const char *key)
{
	bool used = false;
	pthread_mutex_lock(&g_claims_lock);
	for (int i = 0; i < DJI_MAX_CLAIMS; i++) {
		if (g_claims[i][0] && strcmp(g_claims[i], key) == 0) {
			used = true;
			break;
		}
	}
	pthread_mutex_unlock(&g_claims_lock);
	return used;
}

static void release_device(const char *key)
{
	if (!key[0])
		return;
	pthread_mutex_lock(&g_claims_lock);
	for (int i = 0; i < DJI_MAX_CLAIMS; i++) {
		if (strcmp(g_claims[i], key) == 0)
			g_claims[i][0] = 0;
	}
	pthread_mutex_unlock(&g_claims_lock);
}

/* --- capture-thread side ------------------------------------------------ */

static void on_au(void *opaque, const uint8_t *au, size_t size, bool key)
{
	struct dji_src *s = opaque;

	struct au_item item = {
		.data = bmalloc(size),
		.size = size,
		.key = key,
		.pts_ns = (int64_t)os_gettime_ns(),
	};
	memcpy(item.data, au, size);

	pthread_mutex_lock(&s->q_lock);
	if (s->q_count >= MAX_QUEUED_AUS)
		s->q_dropping = true;
	if (s->q_dropping) {
		if (key) {
			/* resync: flush queue, start clean at this keyframe.
			 * Do NOT consume semaphore posts here — blocking on
			 * the semaphore while holding q_lock deadlocks
			 * against the decode thread.  Spurious wakeups are
			 * handled by the decoder's q_count check. */
			while (s->q_count) {
				struct au_item old;
				deque_pop_front(&s->q, &old, sizeof(old));
				bfree(old.data);
				s->q_count--;
			}
			s->q_dropping = false;
			blog(LOG_INFO,
			     "[dji-uvc] '%s': decode lag: dropped to keyframe", SRC_NAME(s));
		} else {
			pthread_mutex_unlock(&s->q_lock);
			bfree(item.data);
			return;
		}
	}
	deque_push_back(&s->q, &item, sizeof(item));
	s->q_count++;
	pthread_mutex_unlock(&s->q_lock);
	os_sem_post(s->q_sem);
}

static void on_payload(void *opaque, const uint8_t *data, size_t size)
{
	struct dji_src *s = opaque;
	os_atomic_set_long(&s->last_payload_s, now_s());
	dji_nal_push(&s->nal, data, size, on_au, s);
}

/* --- decode thread ------------------------------------------------------ */

static void on_frame(void *opaque, const struct dji_decoded *f)
{
	struct dji_src *s = opaque;

	struct obs_source_frame frame = {0};
	frame.width = (uint32_t)f->width;
	frame.height = (uint32_t)f->height;
	frame.timestamp = (uint64_t)f->pts_ns;
	frame.format = (f->format == 0 /*AV_PIX_FMT_YUV420P*/)
			       ? VIDEO_FORMAT_I420
			       : VIDEO_FORMAT_NV12;
	/* AV_PIX_FMT_YUV420P == 0, AV_PIX_FMT_NV12 == 23 in current FFmpeg;
	 * the decode layer only emits these two. */

	frame.data[0] = (uint8_t *)f->data[0];
	frame.data[1] = (uint8_t *)f->data[1];
	frame.data[2] = (uint8_t *)f->data[2];
	frame.linesize[0] = (uint32_t)f->linesize[0];
	frame.linesize[1] = (uint32_t)f->linesize[1];
	frame.linesize[2] = (uint32_t)f->linesize[2];

	video_format_get_parameters(VIDEO_CS_709, VIDEO_RANGE_PARTIAL,
				    frame.color_matrix, frame.color_range_min,
				    frame.color_range_max);
	frame.full_range = false;

	obs_source_output_video(s->source, &frame);
}

static void *decode_thread_fn(void *opaque)
{
	struct dji_src *s = opaque;
	os_set_thread_name("dji-uvc-decode");

	while (s->running) {
		if (os_sem_wait(s->q_sem) != 0)
			continue;
		if (!s->running)
			break;

		struct au_item item;
		pthread_mutex_lock(&s->q_lock);
		if (!s->q_count) {
			pthread_mutex_unlock(&s->q_lock);
			continue;
		}
		deque_pop_front(&s->q, &item, sizeof(item));
		s->q_count--;
		pthread_mutex_unlock(&s->q_lock);

		dji_decoder_push_au(s->dec, item.data, item.size, item.key,
				    item.pts_ns);
		bfree(item.data);
	}
	return NULL;
}

/* --- start / stop -------------------------------------------------------- */

static void stop_stream(struct dji_src *s)
{
	if (s->cap) {
		dji_capture_stop(s->cap);
		s->cap = NULL;
	}
#ifdef _WIN32
	if (s->mf_cap) {
		dji_mf_stop(s->mf_cap);
		s->mf_cap = NULL;
	}
#endif
	release_device(s->claimed);
	s->claimed[0] = 0;
	if (s->thread_started) {
		s->running = false;
		os_sem_post(s->q_sem);
		pthread_join(s->decode_thread, NULL);
		s->thread_started = false;
	}
	if (s->dec) {
		dji_decoder_destroy(s->dec);
		s->dec = NULL;
	}

	pthread_mutex_lock(&s->q_lock);
	while (s->q_count) {
		struct au_item item;
		deque_pop_front(&s->q, &item, sizeof(item));
		bfree(item.data);
		s->q_count--;
	}
	s->q_dropping = false;
	pthread_mutex_unlock(&s->q_lock);

	dji_nal_reset(&s->nal);
	obs_source_output_video(s->source, NULL);
}

static void start_stream(struct dji_src *s)
{
	char err[256] = {0};
	os_atomic_set_long(&s->last_attempt_s, now_s());
	os_atomic_set_long(&s->last_payload_s, now_s());
	int fmt_fourcc = 0, fmt_w = 0, fmt_h = 0, fmt_fps = 0;

#ifdef _WIN32
	if (s->mf_valid) {
		char ckey[700];
		snprintf(ckey, sizeof(ckey), "mf|%s", s->mf_dev.symlink);
		if (!claim_device(ckey)) {
			blog(LOG_WARNING,
			     "[dji-uvc] '%s': camera already in use by another "
			     "dji-uvc source — select a different camera", SRC_NAME(s));
			return;
		}
		snprintf(s->claimed, sizeof(s->claimed), "%s", ckey);
		s->mf_cap = dji_mf_start(&s->mf_dev, s->width, s->height,
					 s->fps, on_payload, s, err,
					 sizeof(err));
		if (!s->mf_cap) {
			blog(LOG_WARNING,
			     "[dji-uvc] '%s': windows-native start failed: %s",
			     SRC_NAME(s), err);
			release_device(s->claimed);
			s->claimed[0] = 0;
			return;
		}
		struct dji_mf_format_info mfmt;
		dji_mf_get_format(s->mf_cap, &mfmt);
		fmt_fourcc = mfmt.fourcc;
		fmt_w = mfmt.width;
		fmt_h = mfmt.height;
		fmt_fps = mfmt.fps;
		blog(LOG_INFO,
		     "[dji-uvc] '%s': windows-native (no driver swap) backend", SRC_NAME(s));
	} else
#endif
	{
		if (!s->dev_valid)
			return;
		char ckey[700];
		snprintf(ckey, sizeof(ckey), "uvc|%s|%s", s->dev.name,
			 s->dev.serial);
		if (!claim_device(ckey)) {
			blog(LOG_WARNING,
			     "[dji-uvc] '%s': camera already in use by another "
			     "dji-uvc source — select a different camera", SRC_NAME(s));
			return;
		}
		snprintf(s->claimed, sizeof(s->claimed), "%s", ckey);
		s->cap = dji_capture_start(&s->dev, s->fourcc, s->width,
					   s->height, s->fps, on_payload, s,
					   err, sizeof(err));
		if (!s->cap) {
			blog(LOG_WARNING, "[dji-uvc] '%s': start failed: %s", SRC_NAME(s), err);
			release_device(s->claimed);
			s->claimed[0] = 0;
			return;
		}
		struct dji_format_info fmt;
		dji_capture_get_format(s->cap, &fmt);
		fmt_fourcc = fmt.fourcc;
		fmt_w = fmt.width;
		fmt_h = fmt.height;
		fmt_fps = fmt.fps;
	}

	blog(LOG_INFO, "[dji-uvc] '%s': streaming %dx%d@%d (%s)", SRC_NAME(s), fmt_w, fmt_h,
	     fmt_fps, fmt_fourcc == FCC_H265 ? "H265" : "H264");

	dji_nal_init(&s->nal, fmt_fourcc == FCC_H265 ? DJI_NAL_H265
						     : DJI_NAL_H264);

	s->dec = dji_decoder_create(fmt_fourcc, s->hw, on_frame, s);
	if (!s->dec) {
		blog(LOG_WARNING, "[dji-uvc] decoder init failed");
		stop_stream(s);
		return;
	}

	s->running = true;
	if (pthread_create(&s->decode_thread, NULL, decode_thread_fn, s) ==
	    0) {
		s->thread_started = true;
	} else {
		s->running = false;
		blog(LOG_WARNING, "[dji-uvc] decode thread create failed");
		stop_stream(s);
	}
}

/* --- OBS plumbing -------------------------------------------------------- */

static const char *dji_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return T("SourceName");
}

static void pick_device(struct dji_src *s);
static void apply_stream_settings(struct dji_src *s, obs_data_t *st);

static void apply_settings(struct dji_src *s, obs_data_t *st)
{
	const char *devkey = obs_data_get_string(st, "device");
	snprintf(s->devkey, sizeof(s->devkey), "%s", devkey);
	s->want_running = true;
	apply_stream_settings(s, st);
	pick_device(s);
}

static void pick_device(struct dji_src *s)
{
	const char *devkey = s->devkey;
	s->dev_valid = false;
#ifdef _WIN32
	s->mf_valid = false;
	bool mf_all_busy = false;

	/* Windows-native devices (stock driver, no Zadig) */
	{
		struct dji_mf_device_info mfd[8];
		int mn = dji_mf_enumerate(mfd, 8);
		for (int i = 0; i < mn; i++) {
			char key[640];
			snprintf(key, sizeof(key), "mf|%s", mfd[i].symlink);
			if (strcmp(key, devkey) == 0) {
				s->mf_dev = mfd[i];
				s->mf_valid = true;
				break;
			}
		}
		if (!s->mf_valid && mn > 0 &&
		    strncmp(devkey, "uvc|", 4) != 0) {
			/* saved key stale or empty — use first FREE camera */
			for (int i = 0; i < mn; i++) {
				char key[700];
				snprintf(key, sizeof(key), "mf|%s",
					 mfd[i].symlink);
				if (device_is_claimed(key))
					continue;
				if (devkey[0])
					blog(LOG_WARNING,
					     "[dji-uvc] saved device id not "
					     "found; using free camera %d",
					     i);
				s->mf_dev = mfd[i];
				s->mf_valid = true;
				break;
			}
			if (!s->mf_valid) {
				mf_all_busy = true;
				blog(LOG_WARNING,
				     "[dji-uvc] '%s': %d camera%s detected "
				     "but all in use by other dji-uvc "
				     "sources — connect another camera",
				     SRC_NAME(s), mn, mn == 1 ? "" : "s");
			}
		}
	}
	if (!s->mf_valid && !mf_all_busy)
#endif
	{
		struct dji_device_info devs[8];
		int n = dji_capture_enumerate(devs, 8);
		for (int i = 0; i < n; i++) {
			char key[192];
			snprintf(key, sizeof(key), "uvc|%s|%s", devs[i].name,
				 devs[i].serial);
			if (strcmp(key, devkey) == 0) {
				s->dev = devs[i];
				s->dev_valid = true;
				break;
			}
		}
		if (!s->dev_valid && n > 0) {
			for (int i = 0; i < n; i++) {
				char key[700];
				snprintf(key, sizeof(key), "uvc|%s|%s",
					 devs[i].name, devs[i].serial);
				if (device_is_claimed(key))
					continue;
				if (devkey[0])
					blog(LOG_WARNING,
					     "[dji-uvc] saved device id not "
					     "found; using free camera %d",
					     i);
				s->dev = devs[i];
				s->dev_valid = true;
				break;
			}
		}
		if (!s->dev_valid
#ifdef _WIN32
		    && !s->mf_valid
#endif
		) {
			blog(LOG_WARNING,
			     "[dji-uvc] '%s': no DJI camera found (Webcam mode? "
			     "connected?)", SRC_NAME(s));
		}
	}

}

static void apply_stream_settings(struct dji_src *s, obs_data_t *st)
{
	const char *codec = obs_data_get_string(st, "codec");
	s->fourcc = 0;
	if (strcmp(codec, "h264") == 0)
		s->fourcc = FCC_H264;
	else if (strcmp(codec, "h265") == 0)
		s->fourcc = FCC_H265;

	const char *mode = obs_data_get_string(st, "mode");
	s->width = s->height = s->fps = 0;
	if (strcmp(mode, "4k60") == 0) {
		s->width = 3840; s->height = 2160; s->fps = 60;
	} else if (strcmp(mode, "4k30") == 0) {
		s->width = 3840; s->height = 2160; s->fps = 30;
	} else if (strcmp(mode, "1080p60") == 0) {
		s->width = 1920; s->height = 1080; s->fps = 60;
	} else if (strcmp(mode, "1080p30") == 0) {
		s->width = 1920; s->height = 1080; s->fps = 30;
	} /* "auto": highest advertised */

	const char *dec = obs_data_get_string(st, "decoder");
	s->hw = DJI_HW_NONE;
	if (strcmp(dec, "cuda") == 0)
		s->hw = DJI_HW_CUDA;
	else if (strcmp(dec, "d3d11va") == 0)
		s->hw = DJI_HW_D3D11VA;
	else if (strcmp(dec, "vaapi") == 0)
		s->hw = DJI_HW_VAAPI;
	else if (strcmp(dec, "videotoolbox") == 0)
		s->hw = DJI_HW_VIDEOTOOLBOX;
}

static void *restart_thread_fn(void *data)
{
	struct dji_src *s = data;
	os_set_thread_name("dji-uvc-restart");
	stop_stream(s);
	pick_device(s); /* re-enumerate: survives unplug/replug */
	start_stream(s);
	os_atomic_set_long(&s->last_attempt_s, now_s());
	os_atomic_set_bool(&s->restarting, false);
	return NULL;
}

static void trigger_restart(struct dji_src *s, const char *why, long stale)
{
	blog(LOG_WARNING, "[dji-uvc] '%s': watchdog: %s (%lds) — restarting capture",
	     SRC_NAME(s), why, stale);
	if (s->restart_joinable) {
		pthread_join(s->restart_th, NULL);
		s->restart_joinable = false;
	}
	os_atomic_set_bool(&s->restarting, true);
	if (pthread_create(&s->restart_th, NULL, restart_thread_fn, s) == 0)
		s->restart_joinable = true;
	else
		os_atomic_set_bool(&s->restarting, false);
}

static void dji_video_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct dji_src *s = data;

	if (!s->want_running || os_atomic_load_bool(&s->restarting))
		return;

	long now = now_s();
	bool active = (s->cap != NULL);
#ifdef _WIN32
	active = active || (s->mf_cap != NULL);
#endif
	if (active) {
		long stale = now - os_atomic_load_long(&s->last_payload_s);
		if (stale >= 3)
			trigger_restart(s, "no frames from camera", stale);
	} else {
		long since = now - os_atomic_load_long(&s->last_attempt_s);
		if (since >= 5)
			trigger_restart(s, "camera not connected, retrying",
					since);
	}
}

static void dji_update(void *data, obs_data_t *settings)
{
	struct dji_src *s = data;
	if (s->restart_joinable) {
		pthread_join(s->restart_th, NULL);
		s->restart_joinable = false;
	}
	stop_stream(s);
	apply_settings(s, settings);
	start_stream(s);
}

static void *dji_create(obs_data_t *settings, obs_source_t *source)
{
	struct dji_src *s = bzalloc(sizeof(*s));
	s->source = source;
	pthread_mutex_init(&s->q_lock, NULL);
	os_sem_init(&s->q_sem, 0);
	deque_init(&s->q);
	dji_nal_init(&s->nal, DJI_NAL_H264);

	apply_settings(s, settings);
	start_stream(s);
	return s;
}

static void dji_destroy(void *data)
{
	struct dji_src *s = data;
	s->want_running = false;
	if (s->restart_joinable) {
		pthread_join(s->restart_th, NULL);
		s->restart_joinable = false;
	}
	stop_stream(s);
	dji_nal_free(&s->nal);
	deque_free(&s->q);
	os_sem_destroy(s->q_sem);
	pthread_mutex_destroy(&s->q_lock);
	bfree(s);
}

static bool refresh_clicked(obs_properties_t *props, obs_property_t *p,
			    void *data)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(p);

	obs_property_t *list = obs_properties_get(props, "device");
	obs_property_list_clear(list);

	int total = 0;
#ifdef _WIN32
	{
		struct dji_mf_device_info mfd[8];
		int mn = dji_mf_enumerate(mfd, 8);
		for (int i = 0; i < mn; i++) {
			char key[640], label[192], serial[64] = {0};
			/* symlink: \\?\usb#vid_xxxx&pid_xxxx#SERIAL#{guid}
			 * — third '#'-separated field is the unit serial */
			const char *a = strchr(mfd[i].symlink, '#');
			const char *b = a ? strchr(a + 1, '#') : NULL;
			const char *c = b ? strchr(b + 1, '#') : NULL;
			if (b && c && (size_t)(c - b - 1) < sizeof(serial)) {
				memcpy(serial, b + 1, (size_t)(c - b - 1));
				serial[c - b - 1] = 0;
			}
			snprintf(key, sizeof(key), "mf|%s", mfd[i].symlink);
			if (serial[0])
				snprintf(label, sizeof(label),
					 "%s [%s] (no driver swap)",
					 mfd[i].name, serial);
			else
				snprintf(label, sizeof(label),
					 "%s #%d (no driver swap)",
					 mfd[i].name, i + 1);
			obs_property_list_add_string(list, label, key);
			total++;
		}
	}
#endif
	{
		struct dji_device_info devs[8];
		int n = dji_capture_enumerate(devs, 8);
		for (int i = 0; i < n; i++) {
			char key[192], label[224];
			snprintf(key, sizeof(key), "uvc|%s|%s", devs[i].name,
				 devs[i].serial);
			if (devs[i].serial[0])
				snprintf(label, sizeof(label),
					 "%s (WinUSB, %s)", devs[i].name,
					 devs[i].serial);
			else
				snprintf(label, sizeof(label), "%s (WinUSB)",
					 devs[i].name);
			obs_property_list_add_string(list, label, key);
			total++;
		}
	}
	if (total == 0)
		obs_property_list_add_string(list, T("NoDevices"), "");
	return true;
}

static obs_properties_t *dji_get_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	obs_property_t *devlist = obs_properties_add_list(
		props, "device", T("Device"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING);
	refresh_clicked(props, devlist, data);

	obs_properties_add_button(props, "refresh", T("Refresh"),
				  refresh_clicked);

	obs_property_t *mode = obs_properties_add_list(
		props, "mode", T("Mode"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(mode, T("AutoHighest"), "auto");
	obs_property_list_add_string(mode, "3840x2160 @ 60", "4k60");
	obs_property_list_add_string(mode, "3840x2160 @ 30", "4k30");
	obs_property_list_add_string(mode, "1920x1080 @ 60", "1080p60");
	obs_property_list_add_string(mode, "1920x1080 @ 30", "1080p30");

	obs_property_t *codec = obs_properties_add_list(
		props, "codec", T("Codec"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(codec, T("Auto"), "auto");
	obs_property_list_add_string(codec, "H.264", "h264");
	obs_property_list_add_string(codec, "H.265 (HEVC)", "h265");

	obs_property_t *dec = obs_properties_add_list(
		props, "decoder", T("Decoder"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(dec, T("Software"), "software");
	obs_property_list_add_string(dec, "Hardware: NVIDIA (cuda)", "cuda");
	obs_property_list_add_string(
		dec, "Hardware: Windows Intel/AMD (d3d11va)", "d3d11va");
	obs_property_list_add_string(dec, "Hardware: Linux (vaapi)", "vaapi");
	obs_property_list_add_string(
		dec, "Hardware: macOS (videotoolbox)", "videotoolbox");

	return props;
}

static void dji_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "mode", "auto");
	obs_data_set_default_string(settings, "codec", "auto");
	obs_data_set_default_string(settings, "decoder", "d3d11va");
}

struct obs_source_info dji_uvc_source = {
	.id = "dji_uvc_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = dji_get_name,
	.create = dji_create,
	.destroy = dji_destroy,
	.update = dji_update,
	.get_defaults = dji_get_defaults,
	.get_properties = dji_get_properties,
	.video_tick = dji_video_tick,
	.icon_type = OBS_ICON_TYPE_CAMERA,
};
