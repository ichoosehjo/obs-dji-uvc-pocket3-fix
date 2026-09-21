# obs-dji-uvc

Native OBS Studio source for DJI Osmo Pocket 3 / 4 / 4P over **encoded UVC** — H.264 up to **4K60**, multiple cameras, Windows-first.

## Why this exists

- DJI's spec sheet "UVC" modes = the YUV/MJPEG descriptors only (Pocket 3: 1080p, 4K25/30 after fw v01.04.08.02)
- 4K60 lives on a separate **frame-based H.264 UVC descriptor** — the one BELABOX's `gstlibuvch264src` pulls via libuvc
- Windows' `usbvideo.sys` / DirectShow never exposes that descriptor → OBS's stock Video Capture Device can't reach it
- This plugin claims the camera via libusb/WinUSB and does the probe/commit itself, BELABOX-style, then decodes on the GPU

## Pipeline

```
Pocket (H.264 elementary stream, up to 4K60)
  └─ libuvc (WinUSB) ──► NAL/AU assembler ──► bounded queue
                                                └─ decode thread (FFmpeg, d3d11va/cuda/sw)
                                                     └─ obs_source_output_video (NV12)
```

- Drop-to-keyframe backpressure: if decode falls behind, queue flushes and resyncs at the next IDR
- One source per camera; USB bandwidth is trivial (encoded 4K60 ≈ 50–150 Mbps/cam), the wall is decode → keep it on the GPU

## Build (Windows)

1. Visual Studio 2022 (Desktop C++), CMake ≥ 3.22, git
2. Download `windows-deps-*-x64.zip` from [obs-deps releases](https://github.com/obsproject/obs-deps/releases), extract to `C:\obs-deps`
3. `powershell -ExecutionPolicy Bypass -File build-windows.ps1 -ObsDeps C:\obs-deps`
4. Output: `build\RelWithDebInfo\obs-dji-uvc.dll`
5. Copy DLL → `C:\Program Files\obs-studio\obs-plugins\64bit\`; copy `data\` → `C:\Program Files\obs-studio\data\obs-plugins\obs-dji-uvc\`

Or push to GitHub — `.github/workflows/build.yml` produces the DLL + Inno Setup installer with zero local toolchain.

## USB binding (required, one-time per camera)

libusb must be able to *claim* the camera; by default Windows' UVC driver owns it.

1. Camera → Webcam mode → plug in
2. [Zadig](https://zadig.akeo.ie) → Options → List All Devices
3. Select the DJI **video** interface (VID `2CA3`)
4. Replace driver with **WinUSB**

Trade-offs:
- Camera stops appearing as a normal Windows webcam (Mimo/Teams/etc. won't see it) — reversible via Device Manager → Uninstall device → rescan
- Composite-device note: bind the *interface*, not the parent device, or audio disappears too

## Use

1. Sources → + → **DJI UVC Camera (4K60)**
2. Pick camera (Refresh re-enumerates), Mode (`Auto` = highest advertised), Decoder
3. Repeat per Pocket

Decoder guidance:
- 1× 1080p: Software fine
- Anything 4K60 or multi-cam: `d3d11va` (default) or `cuda` on your 4090
- HW init failure logs a warning and falls back to software automatically

Audio: this is a video-only source. One dedicated audio source + single Sync Offset (Advanced Audio Properties) at 48 kHz; capture→decode latency is near-constant so one offset holds.

## Charging while streaming (hardware, not software)

Data + charge flow over the same USB-C cable — no splitter, no plugin work. The variable is **port wattage**:

| Port type | Power | 4K webcam draw ~5–8 W | Result |
|---|---|---|---|
| USB 3.x A (default 900 mA) | 4.5 W | > supply | slow drain |
| USB A with BC1.2 CDP | 7.5 W | ≈ supply | holds / trickle |
| Motherboard USB-C (5V/3A) | 15 W | < supply | charges |
| Powered hub, CDP data ports | 7.5–15 W/port | < supply | charges |

- Use rear-panel motherboard USB-C or a powered USB 3.x hub whose **data** ports do CDP/1.5A+ (many hubs' "charging" ports are power-only — verify data+charge on the same port)
- Validate per camera: note battery % → 60 min 4K60 stream → % must be flat or rising

## Known limits / TODO

- PID table is labels-only; VID filter catches any DJI camera (Pocket 4P with unknown PID shows as "DJI camera" — add its PID from Device Manager for a nicer label)
- Hotplug = manual Refresh button (libusb hotplug callbacks are the obvious upgrade)
- H.265 descriptor support is wired but the Pockets emit H.264 on this path in practice
- Only `src/dji-nal.c` is compile-verified in the build sandbox (unit tests in `src/test-nal.c`, all passing); the libobs/FFmpeg/libuvc-linked code needs a Windows build + hardware bring-up pass
- Device selection keys on serial; pin by serial if hot-swapping cameras mid-session

## License

GPL-2.0-or-later (matches OBS Studio). Vendored libuvc: BSD.
