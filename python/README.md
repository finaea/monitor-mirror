# mirror (Python) — archival

> **This is the older, archived version.** The maintained app is the C++ build in
> [`../cpp`](../cpp), which is faster, smaller, and configured with a visual setup
> window. Use that unless you specifically want the readable Python source.

Low-latency 1:1 monitor mirror. Mirrors the **right 2560×1440 half** of a 32:9
(5120×1440) source display onto a 2560×1440 target display, exact 1:1, vsync off,
with the cursor composited in. Runs from the **system tray**. Settings are edited
as constants in `mirror.py` (there is no setup window in this version).

A full side-by-side comparison of the two builds is in
[`build_compare.md`](build_compare.md).

## Run from source

```
pip install dxcam pygame pystray Pillow      (numpy comes with dxcam)
python mirror.py
```

## Build a standalone exe

```
pip install pyinstaller
build.bat
```

Produces `dist\mirror.exe` (~35 MB, no console, bundles Python + all deps).
Double-click it — no Python install needed on the target machine.

## Controls (tray)

- **Left-click** the tray icon → toggle mirroring on/off.
- **Right-click** → menu (Enabled checkbox / Quit).
- **Esc** (mirror window focused) or **Ctrl+C** (from source) → quit.

The tray icon is a wide monitor with the right half lit cyan when ON, greyed when
OFF. While disabled it stops capture, hides the window, and idles.

## How it works

| Stage | Mechanism |
|-------|-----------|
| Capture | `dxcam` (DXGI Desktop Duplication), native BGRA, no color convert |
| Crop | full frame captured; right-half blitted via a pygame sub-rect (this dxcam build crashes on hardware-region capture) |
| Present | pygame/SDL window, vsync off, present-on-new-frame |
| Cursor | live cursor read via Win32 GDI and composited (Duplication omits it) |
| DPI | Per-Monitor-V2, so coordinates/window are physical pixels (1:1) |

Displays are auto-detected by resolution. Tuning constants are at the top of
`mirror.py` (`CROP_X/Y`, `VSYNC`, `CAPTURE_FPS`, `SHOW_CURSOR`, manual overrides).

## Notes

- The frame takes a GPU→CPU→GPU trip (dxcam reads to system RAM, SDL uploads back)
  and presents through the DWM compositor. Both add latency the C++ build avoids.
  On a 60 Hz target the difference is small but real.
- `dist\`, `build\`, and `mirror.spec` are PyInstaller outputs; safe to delete.
