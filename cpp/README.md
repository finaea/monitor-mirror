# mirror (C++ / D3D11)

Ultra-low-latency 1:1 monitor mirror for Windows. Mirrors the **right 2560×1440 half**
of a 32:9 (5120×1440) source display onto a 2560×1440 target display, exact 1:1,
vsync off, with the cursor composited in. Runs from the **system tray**.

This is the all-GPU rewrite of the Python `mirror.py`: the frame never leaves the
GPU (no CPU readback round-trip), and the flip-model swapchain bypasses the DWM
compositor — the two latency costs the Python version couldn't avoid.

## Build

Requires MinGW-w64 g++ (already installed at `C:\ProgramData\mingw64`).

```
cd cpp
build.bat
```

Produces `mirror.exe` (~130 KB, no console window, instant startup).

## Run

Double-click `mirror.exe`. It sits in the notification area (system tray):

- **Left-click** the tray icon → toggle mirroring on/off.
- **Right-click** → menu (Enabled checkbox / Quit).
- **Esc** (when the mirror window is focused) → quit.

When disabled it hides the window and idles (no GPU/CPU use) until re-enabled.

## How it works

| Stage | Mechanism |
|-------|-----------|
| Capture | DXGI Desktop Duplication → desktop as a GPU texture |
| Crop | `CopySubresourceRegion` of the right-half box → GPU texture (GPU→GPU) |
| Present | Flip-model swapchain, `ALLOW_TEARING` (vsync off), waitable + max latency 1, independent flip (DWM bypassed) |
| Cursor | Duplication pointer position/shape → composited as an alpha-blended GPU quad |
| Scaling | none — crop size == target size, point sampling = exact 1:1 |
| DPI | Per-Monitor-V2, so DXGI/coords are physical pixels |

Displays are **auto-detected** by resolution (source = 5120×1440 or widest;
target = the other 2560×1440 output), so it survives re-plugging without edits.

## Tuning

Constants at the top of `mirror.cpp`:

- `SRC_W/SRC_H`, `TGT_W/TGT_H` — expected resolutions (auto-detect overrides).
- `CROP_X/CROP_Y` — region origin; defaults to the right half. Left half = `0,0`.
- `VSYNC` — `true` to trade latency for zero tearing.
- `SHOW_CURSOR` — composite the cursor.

## Notes / limitations

- Both monitors must be on the **same GPU** (they are here) so the crop copy is a
  pure in-VRAM op. Cross-adapter (e.g. hybrid laptop) would need a shared texture.
- Color cursors render perfectly. Monochrome/inverted cursors (rare; some text
  I-beams) are approximated since true XOR-with-screen isn't available in a single pass.
- On a 60 Hz target, perceived latency is bounded by the 60 Hz scanout; this design
  removes the *added* latency (compositor frame + CPU round-trip), not the panel's own.

## Tray icon

The tray icon is **generated at runtime** by `makeTrayIcon()` — a wide monitor
with the right half lit cyan (the mirrored region). It greys out when mirroring
is disabled, so the tray reflects state at a glance. No external `.ico` file is
needed. Tweak the colors/shape in `makeTrayIcon()` in `mirror.cpp`.

To use your own `.ico` instead:

```
echo IDI_ICON1 ICON "mirror.ico" > app.rc
windres app.rc -O coff -o app.res
```
add `app.res` to the `g++` line in `build.bat`, and replace the
`makeTrayIcon(...)` calls with
`LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_ICON1))` (value `101`).
