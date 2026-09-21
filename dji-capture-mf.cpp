/*
 * dji-capture-mf.cpp — Windows-native capture via Media Foundation.
 * License: GPL-2.0-or-later
 */
#ifdef _WIN32

#include "dji-capture-mf.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

#include <util/base.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

#define FOURCC(a, b, c, d) \
	((int)(uint32_t)((a) | ((b) << 8) | ((c) << 16) | ((d) << 24)))
#define FCC_H264 FOURCC('H', '2', '6', '4')

template<typename T> struct com_ptr {
	T *p = nullptr;
	~com_ptr() { reset(); }
	void reset()
	{
		if (p) {
			p->Release();
			p = nullptr;
		}
	}
	T **out()
	{
		reset();
		return &p;
	}
	T *operator->() const { return p; }
	explicit operator bool() const { return p != nullptr; }
};

struct mf_runtime {
	bool started = false;
	bool com = false;
	bool init()
	{
		HRESULT ch = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		com = SUCCEEDED(ch) || ch == RPC_E_CHANGED_MODE;
		if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE)))
			return false;
		started = true;
		return true;
	}
	~mf_runtime()
	{
		if (started)
			MFShutdown();
		/* CoUninitialize intentionally skipped when mode differs */
	}
};

static void narrow(const WCHAR *w, char *out, size_t out_size)
{
	if (!w) {
		out[0] = 0;
		return;
	}
	WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)out_size, nullptr,
			    nullptr);
	out[out_size - 1] = 0;
}

static bool symlink_is_dji(const char *s)
{
	/* symbolic link embeds the hardware id, e.g. ...vid_2ca3&pid_0023... */
	char lower[512];
	size_t n = strlen(s);
	if (n >= sizeof(lower))
		n = sizeof(lower) - 1;
	for (size_t i = 0; i < n; i++)
		lower[i] = (char)tolower((unsigned char)s[i]);
	lower[n] = 0;
	return strstr(lower, "vid_2ca3") != nullptr;
}

/* ---------------------------------------------------------------------- */

int dji_mf_enumerate(struct dji_mf_device_info *out, int max)
{
	mf_runtime rt;
	if (!rt.init())
		return 0;

	com_ptr<IMFAttributes> attrs;
	if (FAILED(MFCreateAttributes(attrs.out(), 1)))
		return 0;
	attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
		       MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

	IMFActivate **devices = nullptr;
	UINT32 count = 0;
	if (FAILED(MFEnumDeviceSources(attrs.p, &devices, &count)))
		return 0;

	int n = 0;
	for (UINT32 i = 0; i < count; i++) {
		WCHAR *wlink = nullptr, *wname = nullptr;
		UINT32 len = 0;
		devices[i]->GetAllocatedString(
			MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
			&wlink, &len);
		devices[i]->GetAllocatedString(
			MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &wname, &len);

		char slink[512], sname[128];
		narrow(wlink, slink, sizeof(slink));
		narrow(wname, sname, sizeof(sname));
		if (wlink)
			CoTaskMemFree(wlink);
		if (wname)
			CoTaskMemFree(wname);

		if (n < max && symlink_is_dji(slink)) {
			snprintf(out[n].name, sizeof(out[n].name), "%s",
				 sname[0] ? sname : "DJI camera");
			snprintf(out[n].symlink, sizeof(out[n].symlink), "%s",
				 slink);
			n++;
		}
		devices[i]->Release();
	}
	CoTaskMemFree(devices);
	return n;
}

/* ---------------------------------------------------------------------- */

struct dji_mf_capture {
	mf_runtime rt;
	com_ptr<IMFMediaSource> source;
	com_ptr<IMFSourceReader> reader;
	struct dji_mf_format_info fmt = {};
	dji_mf_payload_cb cb = nullptr;
	void *opaque = nullptr;
	std::thread worker;
	std::atomic<bool> running{false};
};

static HRESULT create_source_by_symlink(const char *symlink,
					IMFMediaSource **out)
{
	com_ptr<IMFAttributes> attrs;
	HRESULT hr = MFCreateAttributes(attrs.out(), 2);
	if (FAILED(hr))
		return hr;
	attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
		       MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

	WCHAR wlink[512];
	MultiByteToWideChar(CP_UTF8, 0, symlink, -1, wlink, 512);
	attrs->SetString(
		MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
		wlink);
	return MFCreateDeviceSource(attrs.p, out);
}

struct dji_mf_capture *dji_mf_start(const struct dji_mf_device_info *dev,
				    int width, int height, int fps,
				    dji_mf_payload_cb cb, void *opaque,
				    char *errbuf, size_t errbuf_size)
{
#define FAIL(...)                                             \
	do {                                                  \
		if (errbuf)                                   \
			snprintf(errbuf, errbuf_size,         \
				 __VA_ARGS__);                \
		delete c;                                     \
		return nullptr;                               \
	} while (0)

	auto *c = new dji_mf_capture();
	c->cb = cb;
	c->opaque = opaque;

	if (!c->rt.init())
		FAIL("Media Foundation startup failed");

	HRESULT hr = create_source_by_symlink(dev->symlink, c->source.out());
	if (FAILED(hr))
		FAIL("MFCreateDeviceSource failed (0x%08lx) — camera in use "
		     "by another app?", (unsigned long)hr);

	com_ptr<IMFAttributes> rattrs;
	MFCreateAttributes(rattrs.out(), 1);
	/* No video processing — we want the untouched compressed stream. */
	hr = MFCreateSourceReaderFromMediaSource(c->source.p, rattrs.p,
						 c->reader.out());
	if (FAILED(hr))
		FAIL("source reader creation failed (0x%08lx)",
		     (unsigned long)hr);

	/* Walk native media types; pick H264 matching the request (or the
	 * highest advertised). */
	int best_index = -1;
	long best_score = -1;
	int best_w = 0, best_h = 0, best_fps = 0;

	for (DWORD i = 0;; i++) {
		com_ptr<IMFMediaType> mt;
		hr = c->reader->GetNativeMediaType(
			(DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, i,
			mt.out());
		if (hr == MF_E_NO_MORE_TYPES)
			break;
		if (FAILED(hr))
			break;

		GUID sub = {};
		if (FAILED(mt->GetGUID(MF_MT_SUBTYPE, &sub)))
			continue;
		if (sub != MFVideoFormat_H264)
			continue;

		UINT32 w = 0, h = 0, num = 0, den = 1;
		MFGetAttributeSize(mt.p, MF_MT_FRAME_SIZE, &w, &h);
		MFGetAttributeRatio(mt.p, MF_MT_FRAME_RATE, &num, &den);
		int rate = den ? (int)((num + den / 2) / den) : 0;

		if (width && (int)w != width)
			continue;
		if (height && (int)h != height)
			continue;
		if (fps && rate != fps)
			continue;

		long score = (long)w * (long)h + rate;
		if (score > best_score) {
			best_score = score;
			best_index = (int)i;
			best_w = (int)w;
			best_h = (int)h;
			best_fps = rate;
		}
	}

	if (best_index < 0)
		FAIL("camera does not advertise a matching H264 mode via "
		     "Media Foundation (is it in Webcam mode?)");

	com_ptr<IMFMediaType> chosen;
	hr = c->reader->GetNativeMediaType(
		(DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
		(DWORD)best_index, chosen.out());
	if (FAILED(hr))
		FAIL("GetNativeMediaType failed (0x%08lx)",
		     (unsigned long)hr);
	hr = c->reader->SetCurrentMediaType(
		(DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr,
		chosen.p);
	if (FAILED(hr))
		FAIL("SetCurrentMediaType H264 %dx%d@%d failed (0x%08lx)",
		     best_w, best_h, best_fps, (unsigned long)hr);

	c->fmt.fourcc = FCC_H264;
	c->fmt.width = best_w;
	c->fmt.height = best_h;
	c->fmt.fps = best_fps;

	c->running = true;
	c->worker = std::thread([c]() {
		while (c->running) {
			DWORD stream = 0, flags = 0;
			LONGLONG ts = 0;
			IMFSample *sample = nullptr;
			HRESULT hr2 = c->reader->ReadSample(
				(DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
				0, &stream, &flags, &ts, &sample);
			if (FAILED(hr2)) {
				blog(LOG_WARNING,
				     "[dji-uvc] MF ReadSample failed 0x%08lx "
				     "— capture stopped (device dropped?)",
				     (unsigned long)hr2);
				break;
			}
			if (flags & (MF_SOURCE_READERF_ENDOFSTREAM |
				     MF_SOURCE_READERF_ERROR)) {
				blog(LOG_WARNING,
				     "[dji-uvc] MF stream ended (flags "
				     "0x%08lx)",
				     (unsigned long)flags);
				if (sample)
					sample->Release();
				break;
			}
			if (!sample)
				continue;

			IMFMediaBuffer *buf = nullptr;
			if (SUCCEEDED(sample->ConvertToContiguousBuffer(
				    &buf)) &&
			    buf) {
				BYTE *data = nullptr;
				DWORD len = 0;
				if (SUCCEEDED(buf->Lock(&data, nullptr,
							&len)) &&
				    data && len) {
					c->cb(c->opaque, data, len);
					buf->Unlock();
				}
				buf->Release();
			}
			sample->Release();
		}
	});

	return c;
#undef FAIL
}

void dji_mf_get_format(struct dji_mf_capture *c,
		       struct dji_mf_format_info *f)
{
	*f = c->fmt;
}

void dji_mf_stop(struct dji_mf_capture *c)
{
	if (!c)
		return;
	c->running = false;
	/* Releasing the reader unblocks a pending ReadSample. */
	if (c->reader)
		c->reader->Flush((DWORD)MF_SOURCE_READER_ALL_STREAMS);
	if (c->worker.joinable())
		c->worker.join();
	if (c->source)
		c->source->Shutdown();
	delete c;
}

#endif /* _WIN32 */
