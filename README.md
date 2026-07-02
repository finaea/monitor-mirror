# Monitor mirror — two builds

A low-latency 1:1 screen mirror for Windows: it takes the **right 2560×1440 half**
of a 32:9 (5120×1440) source display and shows it, pixel-for-pixel, borderless on a
2560×1440 target display — vsync off, cursor included, controlled from the system tray.

There are **two implementations** with identical features but different trade-offs:

| | [`python/`](python) | [`cpp/`](cpp) |
|---|---|---|
| Language | Python (dxcam + pygame) | C++ (D3D11 / DXGI) |
| **Latency** | low | **lowest** |
| Pipeline | capture → **GPU→CPU→GPU** → present via DWM | **all-GPU**, DWM bypassed (independent flip) |
| Exe size | ~35 MB | **~130 KB** |
| Startup | ~1–2 s (unpacks) | **instant** |
| CPU / bus load | higher (frame readback each frame) | **minimal** |
| Edit & run | **easy** — just edit `.py` | recompile with g++ |
| Build tool | PyInstaller | MinGW g++ |
| Antivirus flags | occasional (onefile) | rare |

Both have the **same behavior**: auto-detect displays by resolution, Per-Monitor-V2
DPI awareness, exact 1:1 (no scaling), cursor compositing, and a tray icon that
toggles mirroring (lit when ON, greyed when OFF) — left-click to toggle, right-click
for a menu.

## Why the C++ one is lower latency

The Python build reads each captured frame off the GPU into system RAM, then SDL
uploads it back to the GPU to present — a ~22 MB-down + ~14 MB-up round-trip every
frame — and presents through the Windows compositor (DWM), which adds about one
frame of delay. The C++ build keeps the frame entirely in VRAM (crop is a GPU copy)
and uses a flip-model swapchain with tearing so the compositor steps out of the way.

**On your 60 Hz target the perceptible gap is "up to ~a frame"** — real, but bounded
by the panel's own 16.6 ms refresh. The C++ build also uses far less CPU and memory
bandwidth.

## Which should I use?

- **Just want it running / want to tweak it** → `python/` (run `mirror.py`, or
  `build.bat` for a standalone exe).
- **Want the leanest, lowest-latency background utility** → `cpp/` (run `build.bat`,
  get a 130 KB `mirror.exe`).

See each folder's `README.md` for build/run details.

## Adjusting for a different setup

Both default to mirroring the **right half** of a 5120×1440 source onto a 2560×1440
target. Change the source/target/crop constants at the top of `python/mirror.py` or
`cpp/mirror.cpp` if your monitors differ (resolutions auto-detect; the crop origin
and vsync are config constants).
