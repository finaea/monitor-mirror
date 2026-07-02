# Monitor Mirror — how to use it

This app shows a live copy of part of one screen on another screen, with almost
no delay. You set it up once in a simple window; after that it runs quietly in
the system tray (the little icons near the clock).

No installation. It's a single file: **`mirror.exe`**.

> **Works on Windows 10 and Windows 11 only** (64-bit). It uses a display feature
> that older Windows versions don't have. It is not available for macOS or Linux.

---

## 1. Start it

Double-click **`mirror.exe`**.

The **first time** (or any time it can't find its settings), a **Setup window**
opens. You must set it up once before it will run — or you can load settings
someone gave you with **Import…**.

## 2. Set it up

The Setup window has two halves:

**① Main working monitor** — the screen where your drawing app is.
- Pick the monitor from the dropdown.
- A picture of that monitor appears with a coloured **box** on it. That box is
  the area that gets mirrored.
- **Drag the box** to move it, or **drag its corners/edges** to resize it.
- Prefer exact numbers? Type the **X, Y, Width, Height** (in pixels) in the
  boxes next to it. (X/Y = distance from the monitor's top-left corner.)

**② Mirror to monitor** — the screen where the copy should appear.
- Pick the monitor (must be a different one).
- Drag its box to place the copy, and drag a **corner** to resize it.
- This box always keeps the **same shape** as your working area, so the picture
  is never stretched or squished. (Its height is set for you.)

**Frame rate (FPS)** — how many times per second the copy refreshes.
- Leave it **0** for "as fast as possible / lowest lag" (recommended).
- Or set a number (e.g. 60) to cap it and save power.

**V-Sync / Show cursor** — optional. V-Sync removes any slight tearing at the
cost of a touch more lag; Show cursor includes your mouse pointer in the copy.

**Black background** — controls what surrounds the mirror on the second screen:
- **On** (default): the whole mirror monitor is covered; everything around the
  mirrored area is black.
- **Off**: only the mirrored rectangle is drawn — the rest of that monitor keeps
  showing your normal desktop and windows, and stays clickable.

Then click one of:
- **Save & Start** — saves your settings and starts mirroring.
- **Import…** — load settings from a `.cfg` file.
- **Export…** — save your current settings to a `.cfg` file to back up or share.
- **Cancel & Quit** — close the app without running.

Your settings are saved automatically as **`mirror.cfg`** next to `mirror.exe`,
so next time it just starts.

## 3. While it's running (the tray icon)

Find the little monitor icon in the system tray (near the clock). It's **lit
when mirroring is ON**, greyed when OFF.

- **Left-click** it → turn mirroring on/off.
- **Right-click** it → menu:
  - **Enabled** — toggle mirroring.
  - **Edit setup…** — reopen the Setup window to change anything.
  - **Quit** — close the app.

When mirroring is off it hides and uses no resources until you turn it back on.

## 4. If something goes wrong

If a monitor gets unplugged, a resolution changes, or the settings no longer
fit, the app **won't guess** — it reopens the Setup window and tells you what's
wrong at the top. From there you can:

- fix it and **Save & Start**,
- **Import…** a known-good `.cfg`, or
- **Cancel & Quit** to close the app.

## Moving or sharing your settings

- **Import… / Export…** both start in the folder where `mirror.exe` lives.
- To move the app to another PC, copy `mirror.exe` (and, if you like,
  `mirror.cfg`). If the other PC's monitors differ, just run Setup again.

## How fast is it, and how does it work? (plain-English)

**The delay is tiny — on a normal 60 Hz screen you'd have a hard time noticing
it.** In the worst case the copy is about one screen-refresh behind (roughly
1/60th of a second, ~16 milliseconds), and usually less. It's built to add as
little lag as physically possible.

Here's the idea without the jargon:

- **It copies the picture on the graphics card, not through your processor.**
  Windows can hand the app the screen image while it's still on the GPU (the
  graphics chip). The app crops and resizes it *right there* and sends it
  straight to the second screen. The image never takes a slow detour through
  main memory and back, which is what usually adds lag.
- **It doesn't wait for the screen's polite "ready" signal.** Normally Windows
  holds each frame until the monitor's next refresh (this avoids a faint tear
  line but adds delay). This app skips that wait so the newest image is shown the
  instant it's ready. You may occasionally see a tiny tear — turn on **V-Sync**
  in Setup if you'd rather trade a hair of lag for none at all.
- **It stays out of the way.** When mirroring is off it stops completely and uses
  no graphics power until you turn it back on.

The trade-off for this speed: both screens must be driven by the **same graphics
card** (true for a typical desktop). On some laptops with two graphics chips this
particular shortcut isn't available.

---

## For developers — building from source

Runs on **Windows 10 / 11 (x64)**; uses DXGI Desktop Duplication + Direct3D 11.

Requires MinGW-w64 g++ (installed at `C:\ProgramData\mingw64`).

```
cd cpp
build.bat
```

Produces `mirror.exe` (no console window, instant startup).

Source files:

| File | Purpose |
|------|---------|
| `mirror.cpp` | App entry, tray, and the all-GPU capture→crop→scale→present renderer |
| `config.h` | Settings model, load/save (`mirror.cfg`), and validation |
| `config_gui.h` / `config_gui.cpp` | The visual Setup window (draggable boxes, monitor pickers, import/export) |
| `monitors.h` | Shared monitor enumeration (DXGI) |

### How the mirror works (technical)

| Stage | Mechanism |
|-------|-----------|
| Capture | DXGI Desktop Duplication → the working monitor as a GPU texture |
| Crop | `CopySubresourceRegion` of the working rectangle → GPU texture (GPU→GPU) |
| Scale | drawn into the mirror rectangle; point sampling at 1:1, linear when scaled |
| Present | Flip-model swapchain, `ALLOW_TEARING` (vsync off), waitable + max latency 1, independent flip (DWM bypassed) |
| Cursor | Duplication pointer position/shape → composited as an alpha-blended quad, scaled to match |
| Clipping | rasterizer scissor keeps the picture inside the mirror rectangle. `cover=1`: window fills the monitor, margins cleared black. `cover=0`: window is only the mirror rectangle, so the desktop around it stays visible |
| DPI | Per-Monitor-V2, so DXGI/coords are physical pixels |

Both monitors must be on the **same GPU** (so the crop copy is a pure in-VRAM
op). Cross-adapter setups (e.g. some hybrid laptops) would need a shared texture.

### Config file format

`mirror.cfg` is a small INI. Rectangles are stored relative to each monitor's
top-left, so the file stays meaningful if the desktop is rearranged.

```ini
[source]
device=\\.\DISPLAY1
x=2560
y=0
w=2560
h=1440
[target]
device=\\.\DISPLAY2
x=0
y=0
w=2560
h=1440
[options]
fps=0
vsync=0
cursor=1
cover=1
```

`cover=1` fills the rest of the mirror monitor with black; `cover=0` shrinks the
window to just the mirror rectangle so the surrounding desktop shows through.

### Tray icon

The tray icon is generated at runtime by `makeTrayIcon()` (a wide monitor with
the right half lit cyan; greyed when disabled) — no external `.ico` needed for
the tray. `app.rc` supplies the exe's file icon.
