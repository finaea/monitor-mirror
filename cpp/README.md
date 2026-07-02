# Monitor Mirror — how to use it

You set it up once in a simple window; after that it runs quietly in
the system tray (the little icons near the clock).

No installation. It's a single file: **`mirror.exe`**.

> **Works on Windows 10 and Windows 11 only** (64-bit). It uses a display feature
> that older Windows versions don't have. It is not available for macOS or Linux.

---

## 1. Get it & start it

**[Download the latest `mirror.exe`](https://github.com/finaea/monitor-mirror/releases/latest/download/mirror.exe)**
from the [Releases page](https://github.com/finaea/monitor-mirror/releases) — no
installer, just the one file. (Windows SmartScreen may warn about an unknown
publisher since the exe isn't code-signed; choose *More info → Run anyway*.)

Then double-click **`mirror.exe`**.

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

**Rotation / Flip H / Flip V** — rotate the mirrored image by 90°, 180°, or 270°,
and/or flip it horizontally or vertically. Handy for tablets mounted sideways or
upside-down. When you rotate 90°/270°, the mirror box automatically switches to the
matching (portrait) shape so nothing is stretched.

**Floating window** — instead of taking over a second monitor, show the mirror in
an ordinary movable, resizable window. It can sit on the **same** screen as your
drawing area, so you don't need a second monitor — ideal for testing or for
capturing in OBS (Window Capture). Drag it anywhere; resize it freely (the picture
keeps its shape and letterboxes). Closing the window just hides it to the tray.

**Log to file** — write a small `mirror.log` next to the exe (startup, the
detected monitors, and any capture errors). Turn this on if you need to diagnose a
problem on another machine.

Working-area tips: drag the box or type exact pixels; **arrow keys nudge** the
selected box by 1px (**Shift+arrow** = 10px); dragging an **edge snaps** to the
monitor edge; hold **Shift while dragging a corner** of the working box to keep its
shape.

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

## How fast is it, and how does it work?

Follow one frame from the source monitor to the mirror. It's a straight,
all-on-the-GPU pipeline; the latency each stage *adds* is called out at every step
(figures are typical on a mid-range desktop GPU).

**1. Wait for a fresh frame — DXGI Desktop Duplication.**
`AcquireNextFrame` returns the moment the source produces a new frame, handing it
over as a GPU texture already in VRAM (no screen-scrape, no copy to system RAM).
This also paces the whole loop, so it burns no CPU spinning.
→ *sampling wait: 0 up to one source refresh (≤16.7 ms @60 Hz, ≤6.9 ms @144 Hz);
processing added: **~0.1–0.3 ms**.*

**2. Crop — `CopySubresourceRegion`.**
The working rectangle is blitted GPU→GPU into a shader-readable texture; the pixels
never leave VRAM.
→ *added: **<0.1 ms**.*

**3. Transform — one textured quad.**
A single draw call maps the crop onto the mirror rectangle, with rotation/flip
baked into per-vertex UVs and a scissor clip; point sampling at 1:1, bilinear only
when scaled.
→ *added: **<0.1 ms**.*

**4. Present — flip-model swapchain, tearing on.**
`DXGI_SWAP_EFFECT_FLIP_DISCARD` + `ALLOW_TEARING` + vsync off + a frame-latency
**waitable object** at `SetMaximumFrameLatency(1)`: the buffer is submitted and
picked up immediately, exactly one frame is ever in flight, and in the
borderless-overlay path this hits **independent flip** so DWM is bypassed
entirely — no compositor queue.
→ *added: **<0.5 ms** (≈0 wait; V-Sync would add up to one refresh).*

**5. Scanout — the mirror panel.**
The monitor physically draws the pixels. This is hardware, not the app.
→ *added: 0 up to one mirror refresh (≤16.7 ms @60 Hz, ≤6.9 ms @144 Hz), ~half that
on average.*

### Total

| | Contribution |
|---|---|
| GPU work the app controls (steps 2–4) | **< ~1 ms** |
| Source sampling (step 1) | 0 – one source refresh |
| Panel scanout (step 5) | 0 – one mirror refresh |

**So the *added* latency is under ~1 ms; end to end it's essentially one
source-frame sample + <1 ms + up to one mirror scanout** — i.e. "up to about a
frame" on 60 Hz hardware, and less on faster panels. The two costs a naive mirror
pays are simply not in the path: **no GPU→CPU→GPU round trip** (the frame stays in
VRAM) and **no DWM compositor frame** (independent flip). An optional **FPS cap**
(`QueryPerformanceCounter` + `Sleep`, `timeBeginPeriod(1)` for 1 ms granularity) can
throttle step 4 when you'd rather save power than have the freshest frame.

### Trade-offs & limits

- **Same GPU.** Both the source and mirror outputs must be on one adapter so the
  crop is an in-VRAM copy; the D3D11 device is created on the source's adapter.
  Cross-adapter (e.g. hybrid laptops driving the two displays from different GPUs)
  would need a shared/keyed-mutex texture and isn't supported.
- **Floating-window mode** is a normal DWM-composited window (so it can be moved,
  resized, and captured by OBS). It gives up the independent-flip path, so it adds
  roughly one compositor frame versus the borderless overlay — still no CPU round
  trip.
- **Cursor** is delivered separately by Duplication and composited as an
  alpha-blended quad; its position/size is transformed through the same
  rotation/flip so it tracks correctly.

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
| `mirror.cpp` | App entry, tray, window lifecycle, and the all-GPU capture→crop→transform→present renderer |
| `config.h` | Settings model, load/save (`mirror.cfg`), stable-id resolution, and validation |
| `config_gui.h` / `config_gui.cpp` | The visual Setup window (draggable boxes, monitor pickers, options, import/export) |
| `monitors.h` | DXGI monitor enumeration + EDID-derived stable hardware ids |
| `app.rc` / `app.manifest` | Exe icon + Common-Controls-6 / Per-Monitor-V2 manifest |

The rendering pipeline is described in **[How fast is it, and how does it
work?](#how-fast-is-it-and-how-does-it-work)** above. Additional implementation
notes:

- **Clipping / letterbox** — a rasterizer scissor keeps the picture inside the
  mirror rectangle; the surrounding area is cleared black (overlay `cover=1`) or
  simply not covered (`cover=0`, window == mirror rect), or letterboxed
  (floating-window mode).
- **DPI** — Per-Monitor-V2 (manifest), so all DXGI/window coordinates are physical
  pixels; rectangles in `mirror.cfg` are monitor-local physical pixels.
- **Reconfigure** — display changes (`WM_DISPLAYCHANGE`), tray *Edit setup…*, and
  capture-loss (`DXGI_ERROR_ACCESS_LOST`) all funnel through one validate → build
  path that re-shows the Setup window with a specific reason on failure.

### Config file format

`mirror.cfg` is a small INI. Rectangles are stored relative to each monitor's
top-left, so the file stays meaningful if the desktop is rearranged.

```ini
[source]
device=\\.\DISPLAY1
hwid=\\?\DISPLAY#GSM5B09#...#{...}
x=2560
y=0
w=2560
h=1440
[target]
device=\\.\DISPLAY2
hwid=\\?\DISPLAY#...
x=0
y=0
w=2560
h=1440
[options]
fps=0
vsync=0
cursor=1
cover=1
windowed=0
rotation=0
fliph=0
flipv=0
log=0
```

- `hwid` is the stable monitor id (EDID-derived interface path). Monitors are
  matched by `hwid` first, then by `device` (`\\.\DISPLAYx`) — so a config keeps
  working when Windows renumbers displays after a re-plug.
- `cover=1` fills the rest of the mirror monitor with black; `cover=0` shrinks the
  window to just the mirror rectangle so the desktop around it shows through.
- `windowed=1` shows the mirror in a movable/resizable window (may share the
  source screen; `cover` is ignored).
- `rotation` ∈ {0,90,180,270}; `fliph`/`flipv` mirror the image. Rotation by
  90/270 swaps the target rectangle's aspect so nothing is stretched.

### Tray icon

The tray icon is generated at runtime by `makeTrayIcon()` (a wide monitor with
the right half lit cyan; greyed when disabled) — no external `.ico` needed for
the tray. `app.rc` supplies the exe's file icon.
