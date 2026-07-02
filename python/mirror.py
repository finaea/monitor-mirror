"""
mirror.py - Low-latency 1:1 mirror of the right 16:9 half of a 32:9 display.

Setup this is tuned for:
  SOURCE : DISPLAY1, primary 32:9, NATIVE 5120x1440 @ 144Hz (shown at 125% scaling
           by Windows, which is why it reports as 4096x1152 - we capture native px).
  TARGET : DISPLAY2, 2560x1440 @ 60Hz, to the left.
  We capture the right 2560x1440 half of the source and present it 1:1
  (no scaling, no letterbox) borderless-fullscreen on the target. vsync OFF.

Why this is near the latency floor on Windows:
  - dxcam = GPU Desktop Duplication (no CPU screen-scrape).
  - native BGRA capture -> zero per-frame color conversion (no OpenCV).
  - crop size == window size  -> zero resampling.
  - vsync off + present-on-new-frame -> the 60Hz panel always scans the freshest frame.

Install:  pip install dxcam pygame pystray Pillow   (numpy comes with dxcam)
Run:      python mirror.py
Control:  system-tray icon - left-click to toggle mirroring on/off, right-click
          for a menu; Esc (window focused) or Ctrl+C also quit.

Notes:
  - This dxcam build segfaults on hardware-region capture, so we grab the FULL
    source frame and crop to the wanted region with a zero-copy pygame blit sub-rect.
  - Desktop Duplication does NOT capture the mouse cursor (it's a separate hardware
    overlay), so we read the live cursor via Win32 and composite it ourselves.
"""

import os
import sys
import time
import threading
import ctypes
from ctypes import wintypes

# Under a windowed (no-console) build (PyInstaller --noconsole) stdout/stderr are
# None and any print() would crash. Route them to the void so logging stays safe.
if sys.stdout is None:
    sys.stdout = open(os.devnull, "w")
if sys.stderr is None:
    sys.stderr = open(os.devnull, "w")

# ----------------------------------------------------------------------------
# CONFIG  (edit here)
# ----------------------------------------------------------------------------
SRC_W, SRC_H = 5120, 1440      # native resolution of the 32:9 source
TGT_W, TGT_H = 2560, 1440      # resolution of the target window / monitor

# Top-left corner of the region to mirror, in source px. The region size always
# equals the target (TGT_W x TGT_H) so the result is exact 1:1, no scaling.
# Default = RIGHT half. For the left half use (0, 0).
CROP_X, CROP_Y = SRC_W - TGT_W, 0          # -> (2560, 0); region = right half

VSYNC        = 0       # 0 = off (lowest latency, may tear) | 1 = on
CAPTURE_FPS  = 144     # cap capture at the source's refresh rate
ALWAYS_ON_TOP = True
SHOW_FPS     = True    # print achieved fps to the console every ~2s
SHOW_CURSOR  = True    # composite the live mouse cursor (Duplication omits it)

# Manual overrides (leave as None to auto-detect):
SRC_DEVICE_IDX = None  # GPU index for the source
SRC_OUTPUT_IDX = None  # output index of the source monitor on that GPU
TARGET_POS     = None  # (left, top) of the target monitor in virtual px
# ----------------------------------------------------------------------------


# --- Per-Monitor-DPI-V2: required so coordinates & the window are PHYSICAL px.
#     Without this, the 125% scaling re-virtualizes everything and 1:1 breaks.
def set_dpi_aware():
    try:
        ctypes.windll.user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
        return "per-monitor-v2"
    except Exception:
        pass
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
        return "per-monitor"
    except Exception:
        pass
    try:
        ctypes.windll.user32.SetProcessDPIAware()
        return "system"
    except Exception:
        return "none"


def enumerate_monitors():
    """Return list of {left,top,right,bottom,width,height,primary} in physical px."""
    monitors = []
    MonitorEnumProc = ctypes.WINFUNCTYPE(
        ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p,
        ctypes.POINTER(wintypes.RECT), wintypes.LPARAM)

    class MONITORINFO(ctypes.Structure):
        _fields_ = [("cbSize", wintypes.DWORD),
                    ("rcMonitor", wintypes.RECT),
                    ("rcWork", wintypes.RECT),
                    ("dwFlags", wintypes.DWORD)]

    def _cb(hMonitor, hdc, lprc, lparam):
        mi = MONITORINFO()
        mi.cbSize = ctypes.sizeof(MONITORINFO)
        ctypes.windll.user32.GetMonitorInfoW(hMonitor, ctypes.byref(mi))
        r = mi.rcMonitor
        monitors.append({
            "left": r.left, "top": r.top, "right": r.right, "bottom": r.bottom,
            "width": r.right - r.left, "height": r.bottom - r.top,
            "primary": bool(mi.dwFlags & 1),
        })
        return 1

    cb = MonitorEnumProc(_cb)            # keep ref alive during the call
    ctypes.windll.user32.EnumDisplayMonitors(0, 0, cb, 0)
    return monitors


def find_target_pos(monitors):
    if TARGET_POS is not None:
        return TARGET_POS
    # the target is the non-primary monitor matching TGT_W x TGT_H
    for m in monitors:
        if m["width"] == TGT_W and m["height"] == TGT_H and not m["primary"]:
            return (m["left"], m["top"])
    # fallback: any monitor of that size
    for m in monitors:
        if m["width"] == TGT_W and m["height"] == TGT_H:
            return (m["left"], m["top"])
    raise RuntimeError(f"No {TGT_W}x{TGT_H} target monitor found. "
                       f"Set TARGET_POS manually. Monitors: {monitors}")


# ----------------------------------------------------------------------------
# Cursor overlay - Desktop Duplication omits the pointer, so we grab it via GDI
# and composite it onto the mirror at the mapped position.
# ----------------------------------------------------------------------------
_user32 = ctypes.windll.user32
_gdi32 = ctypes.windll.gdi32
_c_void_p = ctypes.c_void_p

# 64-bit safety: handle-returning calls MUST have c_void_p restype or they truncate.
for _fn, _ret, _args in [
    (_user32.GetDC, _c_void_p, [_c_void_p]),
    (_user32.ReleaseDC, ctypes.c_int, [_c_void_p, _c_void_p]),
    (_user32.FillRect, ctypes.c_int, [_c_void_p, _c_void_p, _c_void_p]),
    (_user32.DrawIconEx, wintypes.BOOL,
        [_c_void_p, ctypes.c_int, ctypes.c_int, _c_void_p, ctypes.c_int,
         ctypes.c_int, ctypes.c_uint, _c_void_p, ctypes.c_uint]),
    (_user32.GetIconInfo, wintypes.BOOL, [_c_void_p, _c_void_p]),
    (_user32.GetCursorInfo, wintypes.BOOL, [_c_void_p]),
    (_gdi32.CreateCompatibleDC, _c_void_p, [_c_void_p]),
    (_gdi32.CreateDIBSection, _c_void_p,
        [_c_void_p, _c_void_p, ctypes.c_uint, ctypes.POINTER(_c_void_p),
         _c_void_p, wintypes.DWORD]),
    (_gdi32.SelectObject, _c_void_p, [_c_void_p, _c_void_p]),
    (_gdi32.CreateSolidBrush, _c_void_p, [wintypes.DWORD]),
    (_gdi32.DeleteObject, wintypes.BOOL, [_c_void_p]),
    (_gdi32.DeleteDC, wintypes.BOOL, [_c_void_p]),
    (_gdi32.GetObjectW, ctypes.c_int, [_c_void_p, ctypes.c_int, _c_void_p]),
]:
    _fn.restype, _fn.argtypes = _ret, _args

CURSOR_SHOWING = 0x0001
DI_NORMAL = 0x0003


class _POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


class _CURSORINFO(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("flags", wintypes.DWORD),
                ("hCursor", _c_void_p), ("ptScreenPos", _POINT)]


class _ICONINFO(ctypes.Structure):
    _fields_ = [("fIcon", wintypes.BOOL), ("xHotspot", wintypes.DWORD),
                ("yHotspot", wintypes.DWORD), ("hbmMask", _c_void_p),
                ("hbmColor", _c_void_p)]


class _BITMAP(ctypes.Structure):
    _fields_ = [("bmType", ctypes.c_long), ("bmWidth", ctypes.c_long),
                ("bmHeight", ctypes.c_long), ("bmWidthBytes", ctypes.c_long),
                ("bmPlanes", ctypes.c_ushort), ("bmBitsPixel", ctypes.c_ushort),
                ("bmBits", _c_void_p)]


class _BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wintypes.DWORD), ("biWidth", ctypes.c_long),
                ("biHeight", ctypes.c_long), ("biPlanes", ctypes.c_ushort),
                ("biBitCount", ctypes.c_ushort), ("biCompression", wintypes.DWORD),
                ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", ctypes.c_long),
                ("biYPelsPerMeter", ctypes.c_long), ("biClrUsed", wintypes.DWORD),
                ("biClrImportant", wintypes.DWORD)]


def _draw_cursor_over(np, hcursor, w, h, bg):
    """Render hcursor over a solid bg into a top-down 32bpp DIB -> (h,w,4) uint8 BGRA."""
    hdc_screen = _user32.GetDC(None)
    hdc_mem = _gdi32.CreateCompatibleDC(hdc_screen)
    bmi = _BITMAPINFOHEADER()
    bmi.biSize = ctypes.sizeof(_BITMAPINFOHEADER)
    bmi.biWidth, bmi.biHeight = w, -h          # negative -> top-down
    bmi.biPlanes, bmi.biBitCount = 1, 32
    bmi.biCompression = 0                       # BI_RGB
    ppv = _c_void_p()
    hbmp = _gdi32.CreateDIBSection(hdc_mem, ctypes.byref(bmi), 0,
                                   ctypes.byref(ppv), None, 0)
    old = _gdi32.SelectObject(hdc_mem, hbmp)
    brush = _gdi32.CreateSolidBrush(bg)
    _user32.FillRect(hdc_mem, ctypes.byref(wintypes.RECT(0, 0, w, h)), brush)
    _gdi32.DeleteObject(brush)
    _user32.DrawIconEx(hdc_mem, 0, 0, hcursor, w, h, 0, None, DI_NORMAL)
    raw = ctypes.string_at(ppv, w * h * 4)
    arr = np.frombuffer(raw, np.uint8).reshape(h, w, 4).copy()
    _gdi32.SelectObject(hdc_mem, old)
    _gdi32.DeleteObject(hbmp)
    _gdi32.DeleteDC(hdc_mem)
    _user32.ReleaseDC(None, hdc_screen)
    return arr


def _build_cursor_surface(pygame, np, hcursor):
    """Return (surface, hotspot_x, hotspot_y) for an HCURSOR, or (None, 0, 0)."""
    info = _ICONINFO()
    if not _user32.GetIconInfo(hcursor, ctypes.byref(info)):
        return None, 0, 0
    hotx, hoty = info.xHotspot, info.yHotspot
    bm = _BITMAP()
    src_bmp = info.hbmColor if info.hbmColor else info.hbmMask
    _gdi32.GetObjectW(src_bmp, ctypes.sizeof(_BITMAP), ctypes.byref(bm))
    w = bm.bmWidth
    h = bm.bmHeight if info.hbmColor else bm.bmHeight // 2  # mono mask is 2x tall
    if info.hbmMask:
        _gdi32.DeleteObject(info.hbmMask)
    if info.hbmColor:
        _gdi32.DeleteObject(info.hbmColor)
    if w <= 0 or h <= 0:
        return None, 0, 0

    # Two-pass alpha: render over black and over white; the per-channel difference
    # gives transparency, which works for both 32bpp-alpha and legacy mask cursors.
    over_black = _draw_cursor_over(np, hcursor, w, h, 0x000000)[:, :, :3].astype(np.int16)
    over_white = _draw_cursor_over(np, hcursor, w, h, 0xFFFFFF)[:, :, :3].astype(np.int16)
    transp = np.clip((over_white - over_black).mean(axis=2), 0, 255)
    alpha = 255 - transp
    a = np.clip(alpha, 1, 255)[:, :, None] / 255.0
    color = np.clip(over_black / a, 0, 255)     # un-premultiply (over_black = a*C)
    out = np.empty((h, w, 4), np.uint8)
    out[:, :, :3] = color.astype(np.uint8)      # B, G, R
    out[:, :, 3] = alpha.astype(np.uint8)
    surf = pygame.image.frombuffer(out.tobytes(), (w, h), "BGRA").convert_alpha()
    return surf, hotx, hoty


class CursorOverlay:
    """Draws the live system cursor onto the mirror, mapped into the crop region."""

    def __init__(self, pygame, np, src_left, src_top):
        self.pygame, self.np = pygame, np
        self.src_left, self.src_top = src_left, src_top
        self._handle = None
        self._surf = None
        self._hot = (0, 0)

    def draw(self, screen):
        ci = _CURSORINFO()
        ci.cbSize = ctypes.sizeof(_CURSORINFO)
        if not _user32.GetCursorInfo(ctypes.byref(ci)):
            return
        if not (ci.flags & CURSOR_SHOWING) or not ci.hCursor:
            return
        sx = ci.ptScreenPos.x - self.src_left   # cursor in source pixels
        sy = ci.ptScreenPos.y - self.src_top
        if not (CROP_X <= sx < CROP_X + TGT_W and CROP_Y <= sy < CROP_Y + TGT_H):
            return                              # pointer isn't over the mirrored area
        if ci.hCursor != self._handle:          # shape changed -> rebuild (cached)
            self._surf, hx, hy = _build_cursor_surface(self.pygame, self.np, ci.hCursor)
            self._hot = (hx, hy)
            self._handle = ci.hCursor
        if self._surf is not None:
            screen.blit(self._surf,
                        (sx - CROP_X - self._hot[0], sy - CROP_Y - self._hot[1]))


def open_source_camera(dxcam):
    """Open the dxcam output whose native size matches SRC_W x SRC_H."""
    if SRC_DEVICE_IDX is not None and SRC_OUTPUT_IDX is not None:
        return dxcam.create(device_idx=SRC_DEVICE_IDX,
                            output_idx=SRC_OUTPUT_IDX, output_color="BGRA")
    # primary (device 0 / output 0) is almost always the source here
    try:
        cam = dxcam.create(device_idx=0, output_idx=0, output_color="BGRA")
        if cam and cam.width == SRC_W and cam.height == SRC_H:
            return cam
        if cam:
            del cam
    except Exception:
        pass
    # otherwise scan
    for dev in range(2):
        for out in range(4):
            try:
                cam = dxcam.create(device_idx=dev, output_idx=out,
                                   output_color="BGRA")
            except Exception:
                continue
            if cam and cam.width == SRC_W and cam.height == SRC_H:
                return cam
            if cam:
                del cam
    raise RuntimeError(f"No {SRC_W}x{SRC_H} source output found via dxcam. "
                       f"Set SRC_DEVICE_IDX / SRC_OUTPUT_IDX manually.")


# ----------------------------------------------------------------------------
# System tray: enable/disable toggle + quit (matches the C++ build).
# ----------------------------------------------------------------------------
class _State:
    def __init__(self):
        self.want_enabled = True
        self.running = True

STATE = _State()


def _make_icon(on):
    """A wide monitor with the right half lit (cyan when ON, grey when OFF)."""
    from PIL import Image, ImageDraw
    img = Image.new("RGBA", (32, 32), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    frame = (32, 40, 51, 255)
    dim = (20, 50, 58, 255) if on else (42, 42, 42, 255)
    lit = (53, 214, 240, 255) if on else (85, 85, 85, 255)
    d.rectangle([2, 8, 29, 23], fill=frame)        # wide screen bezel
    d.rectangle([4, 10, 15, 21], fill=dim)         # left half (source)
    d.rectangle([16, 10, 27, 21], fill=lit)        # right half (mirrored)
    d.rectangle([14, 24, 17, 26], fill=frame)      # stand
    return img


def start_tray():
    """Run a tray icon in a background thread; returns the pystray Icon or None."""
    try:
        import pystray
    except ImportError:
        print("[mirror] pystray not installed; running without tray")
        return None

    def on_toggle(icon, item):
        STATE.want_enabled = not STATE.want_enabled
        icon.icon = _make_icon(STATE.want_enabled)
        icon.title = "Mirror: ON" if STATE.want_enabled else "Mirror: OFF"

    def on_quit(icon, item):
        STATE.running = False
        icon.stop()

    menu = pystray.Menu(
        pystray.MenuItem("Enabled", on_toggle,
                         checked=lambda i: STATE.want_enabled, default=True),
        pystray.MenuItem("Quit", on_quit),
    )
    icon = pystray.Icon("mirror", _make_icon(True), "Mirror: ON", menu)
    threading.Thread(target=icon.run, daemon=True).start()
    return icon


def main():
    mode = set_dpi_aware()

    # Make SDL respect our DPI awareness and our requested window position.
    os.environ.setdefault("SDL_WINDOWS_DPI_AWARENESS", "permonitorv2")

    try:
        import dxcam
        import numpy as np
    except ImportError:
        sys.exit("Missing dependency: pip install dxcam")
    try:
        import pygame
    except ImportError:
        sys.exit("Missing dependency: pip install pygame")

    monitors = enumerate_monitors()
    tx, ty = find_target_pos(monitors)
    os.environ["SDL_VIDEO_WINDOW_POS"] = f"{tx},{ty}"

    # origin of the source monitor (cursor coords are relative to it)
    src_left, src_top = 0, 0
    for m in monitors:
        if m["width"] == SRC_W and m["height"] == SRC_H:
            src_left, src_top = m["left"], m["top"]
            break

    print(f"[mirror] dpi={mode}  target@({tx},{ty}) {TGT_W}x{TGT_H}  "
          f"vsync={VSYNC} cap_fps={CAPTURE_FPS}")

    print(f"[mirror] crop region = ({CROP_X},{CROP_Y}) {TGT_W}x{TGT_H} of source")

    camera = open_source_camera(dxcam)
    src_w, src_h = camera.width, camera.height
    crop_rect = pygame.Rect(CROP_X, CROP_Y, TGT_W, TGT_H)
    cursor = CursorOverlay(pygame, np, src_left, src_top) if SHOW_CURSOR else None

    pygame.init()
    screen = pygame.display.set_mode((TGT_W, TGT_H), pygame.NOFRAME, vsync=VSYNC)
    pygame.display.set_caption("mirror")
    pygame.mouse.set_visible(False)

    # Hard-pin position/size and (optionally) topmost via Win32 - more reliable
    # across monitors than the SDL position hint alone.
    hwnd = None
    try:
        hwnd = pygame.display.get_wm_info()["window"]
        HWND_TOPMOST, HWND_NOTOPMOST = -1, -2
        SWP_SHOWWINDOW = 0x0040
        z = HWND_TOPMOST if ALWAYS_ON_TOP else HWND_NOTOPMOST
        ctypes.windll.user32.SetWindowPos(hwnd, z, tx, ty, TGT_W, TGT_H,
                                          SWP_SHOWWINDOW)
    except Exception as e:
        print(f"[mirror] SetWindowPos skipped: {e}")

    SW_HIDE, SW_SHOWNA = 0, 8
    def show_window(show):
        if hwnd:
            ctypes.windll.user32.ShowWindow(hwnd, SW_SHOWNA if show else SW_HIDE)

    icon = start_tray()
    camera.start(target_fps=CAPTURE_FPS, video_mode=True)
    applied_enabled = True

    max_frames = int(os.environ.get("MIRROR_MAX_FRAMES", "0"))  # 0 = run forever
    frames = 0
    t_last = 0.0
    rendered = 0

    try:
        while STATE.running:
            # Apply enable/disable transitions on this (main) thread - never touch
            # dxcam/SDL from the tray thread.
            if STATE.want_enabled != applied_enabled:
                if STATE.want_enabled:
                    show_window(True)
                    try:
                        camera.start(target_fps=CAPTURE_FPS, video_mode=True)
                    except Exception:
                        camera = open_source_camera(dxcam)
                        camera.start(target_fps=CAPTURE_FPS, video_mode=True)
                else:
                    try:
                        camera.stop()
                    except Exception:
                        pass
                    show_window(False)
                applied_enabled = STATE.want_enabled

            for e in pygame.event.get():
                if e.type == pygame.QUIT:
                    STATE.running = False
                elif e.type == pygame.KEYDOWN and e.key == pygame.K_ESCAPE:
                    STATE.running = False

            if not applied_enabled:
                time.sleep(0.05)           # idle while disabled (no capture/present)
                continue

            frame = camera.get_latest_frame()   # blocks until a NEW frame (<=144/s)
            if frame is not None:
                # zero-copy view of the full source frame, then blit only the
                # wanted region to the window origin -> exact 1:1, no scaling.
                surf = pygame.image.frombuffer(frame, (src_w, src_h), "BGRA")
                screen.blit(surf, (0, 0), crop_rect)
                if cursor is not None:
                    cursor.draw(screen)
                pygame.display.flip()           # vsync off -> present immediately
                rendered += 1
                if max_frames and rendered >= max_frames:
                    STATE.running = False

            if SHOW_FPS:
                frames += 1
                t = pygame.time.get_ticks() / 1000.0
                if t - t_last >= 2.0:
                    print(f"[mirror] {frames / (t - t_last):.0f} fps")
                    frames = 0
                    t_last = t
    except KeyboardInterrupt:
        pass
    finally:
        try:
            camera.stop()
        except Exception:
            pass
        if icon is not None:
            try:
                icon.stop()
            except Exception:
                pass
        pygame.quit()


if __name__ == "__main__":
    main()
