# C++ vs. Python build — comparison

A low-latency 1:1 screen mirror for Windows: it copies a chosen rectangle of a
**working** monitor (where your drawing software runs) onto a rectangle of a
second **mirror** monitor, exact 1:1 (or cleanly scaled), vsync off, cursor
included, controlled from the system tray.

There are **two implementations**. The **C++ build is the one to use** — it is
lower latency, tiny, and now fully configured from a visual GUI. The **Python
build is kept for archival reference** (it has the fixed right-half behaviour and
is easy to read/hack, but is not maintained for the new GUI workflow).

| | [`../cpp`](../cpp) — **use this** | [`.`](.) — Python (archival) |
|---|---|---|
| Language | C++ (D3D11 / DXGI) | Python (dxcam + pygame) |
| Configuration | **visual GUI** (pick monitors, drag the area, set FPS) | edit constants in `mirror.py` |
| **Latency** | **lowest** | low |
| Pipeline | **all-GPU**, DWM bypassed (independent flip) | capture → **GPU→CPU→GPU** → present via DWM |
| Exe size | **~200 KB** | ~35 MB |
| Startup | **instant** | ~1–2 s (unpacks) |
| CPU / bus load | **minimal** | higher (frame readback each frame) |
| Scaling | 1:1 or ratio-locked scale | 1:1 only |
| Build tool | MinGW g++ | PyInstaller |
| Antivirus flags | rare | occasional (onefile) |

## Why the C++ one is lower latency

The Python build reads each captured frame off the GPU into system RAM, then SDL
uploads it back to the GPU to present — a ~22 MB-down + ~14 MB-up round-trip every
frame — and presents through the Windows compositor (DWM), which adds about one
frame of delay. The C++ build keeps the frame entirely in VRAM (the crop is a GPU
copy) and uses a flip-model swapchain with tearing so the compositor steps out of
the way.

**On a 60 Hz target the perceptible gap is "up to ~a frame"** — real, but bounded
by the panel's own 16.6 ms refresh. The C++ build also uses far less CPU and memory
bandwidth.

## Feature differences

- **C++**: monitors, the working rectangle, the mirror rectangle (ratio-locked),
  and the frame rate are all set in a setup window and saved to `mirror.cfg`
  next to the exe. It can scale (not just 1:1) and prompts you to fix things if a
  monitor is missing or the config is invalid.
- **Python**: mirrors the fixed **right 2560×1440 half** of a 5120×1440 source
  onto a 2560×1440 target, 1:1 only. Change the source/target/crop constants at
  the top of `mirror.py` for a different setup.

Both share the same core idea: DXGI Desktop Duplication, Per-Monitor-V2 DPI
awareness, cursor compositing, and a tray icon that toggles mirroring (lit when
ON, greyed when OFF) — left-click to toggle, right-click for a menu.
