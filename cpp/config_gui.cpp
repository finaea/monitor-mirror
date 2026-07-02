// ===========================================================================
// config_gui.cpp - Implementation of the visual setup dialog (raw Win32).
//
// Two "canvas" child windows draw a monitor to scale with a draggable /
// resizable rectangle on it. The left canvas is the free-form working area on
// the main monitor; the right canvas is the mirror area, whose box is locked to
// the working area's aspect ratio. Numeric fields mirror each box two-way.
// ===========================================================================
#ifndef UNICODE
#define UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include "config_gui.h"
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <commctrl.h>
#include <cmath>
#include <cwchar>
#include <algorithm>

// ---- control ids -----------------------------------------------------------
enum {
    ID_SRC_COMBO = 100, ID_TGT_COMBO,
    ID_SRC_X = 110, ID_SRC_Y, ID_SRC_W, ID_SRC_H,
    ID_TGT_X = 120, ID_TGT_Y, ID_TGT_W, ID_TGT_H,
    ID_FPS = 130, ID_VSYNC, ID_CURSOR, ID_COVER,
    ID_ROT, ID_FLIPH, ID_FLIPV, ID_WINDOWED, ID_LOG,
    ID_SAVE = 140, ID_IMPORT, ID_EXPORT, ID_QUIT
};
static const UINT WM_CANVAS_CHANGED = WM_APP + 20;   // wParam: 0=source, 1=target

// shared fonts / brushes (created in RunConfigDialog)
static HFONT  g_font = nullptr, g_hdrFont = nullptr, g_titleFont = nullptr, g_subFont = nullptr, g_btnFont = nullptr;
static HBRUSH g_bgBrush = nullptr, g_cardBrush = nullptr;

// ---- a monitor-with-a-box canvas -------------------------------------------
struct Canvas {
    HWND hwnd = nullptr, parent = nullptr;
    const MonitorInfo* mon = nullptr;
    RECT region{};                 // monitor-local pixels (left,top,right,bottom)
    bool  ratioLocked = false;
    double lockAspect = 1.0;       // w/h to preserve when ratioLocked
    int   which = 0;               // 0 source, 1 target
    bool  enabled = true;          // target stays disabled until a source is chosen
    // view mapping (monitor px -> canvas px)
    double scale = 1.0; int offX = 0, offY = 0;
    // drag
    int   dragMode = -1;           // -1 none, 8 = move, 0..7 = handles
    POINT dragMouse0{}; RECT dragRegion0{};
};

static double clampd(double v, double lo, double hi){ return v < lo ? lo : (v > hi ? hi : v); }
static int    clampi(int v, int lo, int hi){ return v < lo ? lo : (v > hi ? hi : v); }
static void   fillRound(HDC dc, RECT r, int rad, COLORREF fill);     // defined below
static void   strokeRound(HDC dc, RECT r, int rad, COLORREF line);

static void computeView(Canvas* c) {
    RECT cr; GetClientRect(c->hwnd, &cr);
    int cw = cr.right, ch = cr.bottom, pad = 8;
    if (!c->mon) { c->scale = 1; c->offX = c->offY = 0; return; }
    double sx = (double)(cw - 2*pad) / c->mon->w();
    double sy = (double)(ch - 2*pad) / c->mon->h();
    c->scale = std::min(sx, sy);
    if (c->scale <= 0) c->scale = 0.0001;
    c->offX = (int)((cw - c->mon->w() * c->scale) / 2);
    c->offY = (int)((ch - c->mon->h() * c->scale) / 2);
}
static POINT m2c(Canvas* c, int mx, int my) {
    return { (LONG)(c->offX + mx * c->scale), (LONG)(c->offY + my * c->scale) };
}
static POINT c2m(Canvas* c, int cx, int cy) {
    return { (LONG)((cx - c->offX) / c->scale), (LONG)((cy - c->offY) / c->scale) };
}

// The 8 handle centres in monitor-local coords, order: TL,T,TR,R,BR,B,BL,L
static void handlePoints(const RECT& r, POINT out[8]) {
    int mx = (r.left + r.right)/2, my = (r.top + r.bottom)/2;
    out[0] = { r.left,  r.top };    out[1] = { mx, r.top };    out[2] = { r.right, r.top };
    out[3] = { r.right, my };       out[4] = { r.right, r.bottom };
    out[5] = { mx, r.bottom };      out[6] = { r.left, r.bottom };  out[7] = { r.left, my };
}
static bool isCorner(int h){ return h==0||h==2||h==4||h==6; }

static void normalize(RECT& r){
    if (r.right  < r.left) std::swap(r.left, r.right);
    if (r.bottom < r.top ) std::swap(r.top,  r.bottom);
}
static void clampInside(RECT& r, int mw, int mh){
    if (r.left < 0) r.left = 0;   if (r.top < 0) r.top = 0;
    if (r.right  > mw) r.right  = mw;   if (r.bottom > mh) r.bottom = mh;
    if (r.right - r.left < 8) r.right  = std::min<LONG>(mw, r.left + 8);
    if (r.bottom - r.top < 8) r.bottom = std::min<LONG>(mh, r.top  + 8);
}

// A box centred on the monitor, `frac` of its size (e.g. 0.5 = half).
static RECT centeredBox(const MonitorInfo& m, double frac){
    int w = (int)(m.w() * frac), h = (int)(m.h() * frac);
    int x = (m.w() - w) / 2, y = (m.h() - h) / 2;
    return { x, y, x + w, y + h };
}
// A centred box that fits aspect `asp` (w/h) inside `frac` of the monitor.
static RECT centeredRatioBox(const MonitorInfo& m, double frac, double asp){
    if (asp <= 0) asp = 1;
    double bw = m.w() * frac, bh = m.h() * frac;
    double w = bw, h = w / asp;
    if (h > bh){ h = bh; w = h * asp; }
    int iw = (int)w, ih = (int)h;
    int x = (m.w() - iw) / 2, y = (m.h() - ih) / 2;
    return { x, y, x + iw, y + ih };
}

// snap region edges to the monitor edges / centre when within a few pixels.
static void snapRect(Canvas* c, RECT& r, bool moving){
    int mw = c->mon->w(), mh = c->mon->h();
    int thr = (int)(7.0 / (c->scale > 0 ? c->scale : 1.0)); if (thr < 2) thr = 2;
    auto nr = [&](int a, int b){ return abs(a - b) <= thr; };
    if (moving){
        int w = r.right - r.left, h = r.bottom - r.top;
        if      (nr(r.left, 0))                 { r.left = 0;      r.right = w; }
        else if (nr(r.right, mw))               { r.right = mw;    r.left = mw - w; }
        else if (nr((r.left+r.right)/2, mw/2))  { r.left = mw/2 - w/2; r.right = r.left + w; }
        if      (nr(r.top, 0))                  { r.top = 0;       r.bottom = h; }
        else if (nr(r.bottom, mh))              { r.bottom = mh;   r.top = mh - h; }
        else if (nr((r.top+r.bottom)/2, mh/2))  { r.top = mh/2 - h/2; r.bottom = r.top + h; }
    } else {
        if (nr(r.left, 0))   r.left = 0;
        if (nr(r.right, mw)) r.right = mw;
        if (nr(r.top, 0))    r.top = 0;
        if (nr(r.bottom, mh))r.bottom = mh;
    }
}

// resize with the aspect ratio preserved, anchored at the corner opposite `h`.
static void resizeRatio(Canvas* c, int h, int mx, int my, double asp){
    const RECT r0 = c->dragRegion0;
    int mw = c->mon->w(), mh = c->mon->h();
    if (asp <= 0) asp = 1;
    int anchorX, anchorY;   // fixed corner
    int signX, signY;       // +1 grows right/down from anchor, -1 grows left/up
    switch (h){
        case 0: anchorX=r0.right; anchorY=r0.bottom; signX=-1; signY=-1; break; // TL
        case 2: anchorX=r0.left;  anchorY=r0.bottom; signX=+1; signY=-1; break; // TR
        case 4: anchorX=r0.left;  anchorY=r0.top;    signX=+1; signY=+1; break; // BR
        default:anchorX=r0.right; anchorY=r0.top;    signX=-1; signY=+1; break; // BL(6)
    }
    mx = clampi(mx, 0, mw); my = clampi(my, 0, mh);
    double availW = (signX > 0) ? (mw - anchorX) : anchorX;
    double availH = (signY > 0) ? (mh - anchorY) : anchorY;
    double w = std::fabs((double)mx - anchorX);
    w = clampd(w, 8, std::min(availW, availH * asp));
    double hh = w / asp;
    RECT r;
    if (signX > 0){ r.left = anchorX; r.right = anchorX + (int)w; }
    else          { r.right = anchorX; r.left = anchorX - (int)w; }
    if (signY > 0){ r.top = anchorY; r.bottom = anchorY + (int)hh; }
    else          { r.bottom = anchorY; r.top = anchorY - (int)hh; }
    clampInside(r, mw, mh);
    c->region = r;
}

static void resizeFree(Canvas* c, int h, int mx, int my){
    RECT r = c->dragRegion0;
    int mw = c->mon->w(), mh = c->mon->h();
    mx = clampi(mx, 0, mw); my = clampi(my, 0, mh);
    if (h==0||h==6||h==7) r.left  = mx;    // left edge
    if (h==2||h==3||h==4) r.right = mx;    // right edge
    if (h==0||h==1||h==2) r.top    = my;   // top edge
    if (h==4||h==5||h==6) r.bottom = my;   // bottom edge
    normalize(r);
    snapRect(c, r, false);
    clampInside(r, mw, mh);
    c->region = r;
}

static void moveRegion(Canvas* c, int mx, int my){
    RECT r = c->dragRegion0;
    int mw = c->mon->w(), mh = c->mon->h();
    int dx = mx - c->dragMouse0.x, dy = my - c->dragMouse0.y;
    r.left += dx; r.right += dx; r.top += dy; r.bottom += dy;
    if (r.left < 0){ r.right -= r.left; r.left = 0; }
    if (r.top  < 0){ r.bottom -= r.top; r.top = 0; }
    if (r.right  > mw){ int o = r.right - mw; r.left -= o; r.right -= o; }
    if (r.bottom > mh){ int o = r.bottom - mh; r.top -= o; r.bottom -= o; }
    c->region = r;
}

// nudge / clamp-shift the whole region by (dx,dy) monitor px (arrow keys).
static void nudgeRegion(Canvas* c, int dx, int dy){
    int mw = c->mon->w(), mh = c->mon->h();
    RECT r = c->region;
    r.left += dx; r.right += dx; r.top += dy; r.bottom += dy;
    if (r.left < 0){ r.right -= r.left; r.left = 0; }
    if (r.top  < 0){ r.bottom -= r.top; r.top = 0; }
    if (r.right  > mw){ int o = r.right - mw; r.left -= o; r.right -= o; }
    if (r.bottom > mh){ int o = r.bottom - mh; r.top -= o; r.bottom -= o; }
    c->region = r;
}

static int hitTest(Canvas* c, int cx, int cy){
    if (!c->mon) return -1;
    POINT hp[8]; handlePoints(c->region, hp);
    for (int i = 0; i < 8; ++i){
        if (c->ratioLocked && !isCorner(i)) continue;  // locked: corners only
        POINT p = m2c(c, hp[i].x, hp[i].y);
        if (abs(cx - p.x) <= 6 && abs(cy - p.y) <= 6) return i;
    }
    POINT a = m2c(c, c->region.left, c->region.top);
    POINT b = m2c(c, c->region.right, c->region.bottom);
    if (cx > a.x && cx < b.x && cy > a.y && cy < b.y) return 8;  // move
    return -1;
}

static LPCWSTR cursorFor(int h){
    switch (h){
        case 0: case 4: return IDC_SIZENWSE;
        case 2: case 6: return IDC_SIZENESW;
        case 1: case 5: return IDC_SIZENS;
        case 3: case 7: return IDC_SIZEWE;
        case 8:         return IDC_SIZEALL;
        default:        return IDC_ARROW;
    }
}

static void paintCanvas(Canvas* c){
    PAINTSTRUCT ps; HDC dc = BeginPaint(c->hwnd, &ps);
    RECT cr; GetClientRect(c->hwnd, &cr);
    // double buffer
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, cr.right, cr.bottom);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);

    HBRUSH bg = CreateSolidBrush(RGB(255,255,255)); FillRect(mem, &cr, bg); DeleteObject(bg);

    if (c->mon){
        computeView(c);
        SetBkMode(mem, TRANSPARENT);

        // monitor body (rounded dark panel)
        POINT a = m2c(c, 0, 0), b = m2c(c, c->mon->w(), c->mon->h());
        RECT mr = { a.x, a.y, b.x, b.y };
        fillRound(mem, mr, 8, RGB(37, 42, 53));
        strokeRound(mem, mr, 8, RGB(22, 26, 34));

        const COLORREF regFill = c->ratioLocked ? RGB(36, 168, 124) : RGB(70, 130, 232);
        const COLORREF regEdge = c->ratioLocked ? RGB(120, 226, 186) : RGB(150, 190, 255);

        // region rectangle
        POINT ra = m2c(c, c->region.left, c->region.top);
        POINT rb = m2c(c, c->region.right, c->region.bottom);
        RECT rr = { ra.x, ra.y, rb.x, rb.y };
        fillRound(mem, rr, 4, regFill);
        HPEN pen = CreatePen(PS_SOLID, 2, regEdge);
        HGDIOBJ op = SelectObject(mem, pen), ob = SelectObject(mem, GetStockObject(NULL_BRUSH));
        RoundRect(mem, rr.left, rr.top, rr.right, rr.bottom, 4, 4);
        SelectObject(mem, ob); SelectObject(mem, op); DeleteObject(pen);

        // dimension text centred in the region
        wchar_t txt[64];
        swprintf(txt, 64, L"%d × %d", c->region.right - c->region.left, c->region.bottom - c->region.top);
        SetTextColor(mem, RGB(255, 255, 255));
        SelectObject(mem, g_subFont ? g_subFont : (HFONT)GetStockObject(DEFAULT_GUI_FONT));
        DrawTextW(mem, txt, -1, &rr, DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOCLIP);

        // handles as circles (white fill, coloured ring)
        POINT hp[8]; handlePoints(c->region, hp);
        HPEN hpen = CreatePen(PS_SOLID, 2, regEdge);
        HBRUSH hbr = CreateSolidBrush(RGB(255, 255, 255));
        HGDIOBJ op2 = SelectObject(mem, hpen), ob2 = SelectObject(mem, hbr);
        for (int i = 0; i < 8; ++i){
            if (c->ratioLocked && !isCorner(i)) continue;
            POINT p = m2c(c, hp[i].x, hp[i].y);
            Ellipse(mem, p.x - 5, p.y - 5, p.x + 5, p.y + 5);
        }
        SelectObject(mem, ob2); SelectObject(mem, op2); DeleteObject(hpen); DeleteObject(hbr);
    } else {
        SetBkMode(mem, TRANSPARENT); SetTextColor(mem, RGB(150,155,164));
        SelectObject(mem, g_font ? g_font : (HFONT)GetStockObject(DEFAULT_GUI_FONT));
        LPCWSTR hint = c->enabled ? L"Select a monitor above."
                                  : L"Select the main working monitor first.";
        DrawTextW(mem, hint, -1, &cr, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    }

    BitBlt(dc, 0, 0, cr.right, cr.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
    EndPaint(c->hwnd, &ps);
}

static LRESULT CALLBACK CanvasProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp){
    Canvas* c = (Canvas*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg){
    case WM_PAINT: if (c) paintCanvas(c); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_GETDLGCODE: return DLGC_WANTARROWS;   // let arrow keys reach us for nudging
    case WM_LBUTTONDOWN: {
        if (!c || !c->mon) return 0;
        SetFocus(hwnd);                            // so arrow-key nudging works after a click
        int h = hitTest(c, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (h < 0) return 0;
        c->dragMode = h;
        c->dragRegion0 = c->region;
        c->dragMouse0 = c2m(c, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        SetCapture(hwnd);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!c || !c->mon) return 0;
        POINT mm = c2m(c, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (c->dragMode < 0){
            int h = hitTest(c, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            SetCursor(LoadCursorW(nullptr, cursorFor(h)));
            return 0;
        }
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (c->dragMode == 8)          moveRegion(c, mm.x, mm.y);
        else if (c->ratioLocked)       resizeRatio(c, c->dragMode, mm.x, mm.y, c->lockAspect);
        else if (shift && isCorner(c->dragMode)){   // hold Shift = keep the working area's shape
            const RECT& r0 = c->dragRegion0;
            double a = (r0.bottom > r0.top) ? (double)(r0.right - r0.left) / (r0.bottom - r0.top) : 1.0;
            resizeRatio(c, c->dragMode, mm.x, mm.y, a);
        } else                         resizeFree(c, c->dragMode, mm.x, mm.y);
        InvalidateRect(hwnd, nullptr, FALSE);
        SendMessageW(c->parent, WM_CANVAS_CHANGED, (WPARAM)c->which, 0);
        return 0;
    }
    case WM_KEYDOWN: {
        if (!c || !c->mon) return 0;
        int step = (GetKeyState(VK_SHIFT) & 0x8000) ? 10 : 1;
        int dx = 0, dy = 0;
        if      (wp == VK_LEFT)  dx = -step;
        else if (wp == VK_RIGHT) dx =  step;
        else if (wp == VK_UP)    dy = -step;
        else if (wp == VK_DOWN)  dy =  step;
        else return 0;
        nudgeRegion(c, dx, dy);
        InvalidateRect(hwnd, nullptr, FALSE);
        SendMessageW(c->parent, WM_CANVAS_CHANGED, (WPARAM)c->which, 0);
        return 0;
    }
    case WM_LBUTTONUP:
        if (c && c->dragMode >= 0){ c->dragMode = -1; ReleaseCapture();
            SendMessageW(c->parent, WM_CANVAS_CHANGED, (WPARAM)c->which, 0); }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) return TRUE;  // we set it in WM_MOUSEMOVE
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- dialog state ----------------------------------------------------------
struct DlgState {
    const std::vector<MonitorInfo>* mons = nullptr;
    MirrorConfig* out = nullptr;
    std::wstring  defaultPath;
    ConfigResult  result = ConfigResult::Quit;
    bool          done = false;
    bool          syncing = false;       // guard against edit<->canvas feedback
    Canvas src, tgt;
    HWND hSrcCombo=0, hTgtCombo=0;
    HWND hSrcX=0,hSrcY=0,hSrcW=0,hSrcH=0;
    HWND hTgtX=0,hTgtY=0,hTgtW=0,hTgtH=0;
    HWND hFps=0, hVsync=0, hCursor=0, hCover=0;
    HWND hRot=0, hFlipH=0, hFlipV=0, hWindowed=0, hLog=0;
    HWND hover=nullptr;          // button currently hovered (for owner-draw)
    std::wstring banner;         // info/error strip text (painted, not a control)
};

static void setInt(HWND e, int v){ wchar_t b[16]; swprintf(b,16,L"%d",v); SetWindowTextW(e,b); }
static int  getInt(HWND e){ wchar_t b[16]={0}; GetWindowTextW(e,b,16); return _wtoi(b); }
// Like setInt, but never rewrites the control the user is typing in — SetWindowText
// resets the caret to the start, which would reverse what's being typed.
static void setIntF(HWND e, int v){ if (e == GetFocus()) return; setInt(e, v); }

// rotation currently selected in the dialog (degrees), and the resulting
// aspect the target box must have (rotation by 90/270 swaps w/h).
static int dlgRotation(DlgState* s){
    int i = (int)SendMessageW(s->hRot, CB_GETCURSEL, 0, 0);
    return (i <= 0) ? 0 : i * 90;
}
static double dlgEffAspect(DlgState* s){
    int w = s->src.region.right - s->src.region.left;
    int h = s->src.region.bottom - s->src.region.top;
    if (w <= 0 || h <= 0) return 1.0;
    double a = (double)w / h;
    int rot = dlgRotation(s);
    return (rot == 90 || rot == 270) ? 1.0 / a : a;
}

static void writeSrcFields(DlgState* s){
    s->syncing = true;
    setIntF(s->hSrcX, s->src.region.left);
    setIntF(s->hSrcY, s->src.region.top);
    setIntF(s->hSrcW, s->src.region.right - s->src.region.left);
    setIntF(s->hSrcH, s->src.region.bottom - s->src.region.top);
    s->syncing = false;
}
static void writeTgtFields(DlgState* s){
    s->syncing = true;
    setIntF(s->hTgtX, s->tgt.region.left);
    setIntF(s->hTgtY, s->tgt.region.top);
    setIntF(s->hTgtW, s->tgt.region.right - s->tgt.region.left);
    setIntF(s->hTgtH, s->tgt.region.bottom - s->tgt.region.top);
    s->syncing = false;
}

// re-shape the target box to the (rotation-adjusted) source aspect
static void refitTarget(DlgState* s){
    if (!s->tgt.mon) return;
    double asp = dlgEffAspect(s);
    s->tgt.lockAspect = asp;
    int mw = s->tgt.mon->w(), mh = s->tgt.mon->h();
    RECT& r = s->tgt.region;
    int w = r.right - r.left; if (w < 8) w = 8;
    int h = (int)std::lround(w / asp); if (h < 8){ h = 8; w = (int)std::lround(h*asp); }
    if (w > mw){ w = mw; h = (int)std::lround(w/asp); }
    if (h > mh){ h = mh; w = (int)std::lround(h*asp); }
    r.right = r.left + w; r.bottom = r.top + h;
    if (r.right  > mw){ int o = r.right - mw; r.left -= o; r.right -= o; if (r.left<0) r.left=0; }
    if (r.bottom > mh){ int o = r.bottom - mh; r.top -= o; r.bottom -= o; if (r.top<0) r.top=0; }
}

static void selectMonitorIdx(HWND combo, int idx){
    SendMessageW(combo, CB_SETCURSEL, idx < 0 ? (WPARAM)-1 : (WPARAM)idx, 0);
}

// The mirror monitor can only be chosen once a working monitor is picked (so its
// box can be initialised at the working area's aspect ratio).
static void setTargetEnabled(DlgState* s, bool on){
    s->tgt.enabled = on;
    EnableWindow(s->hTgtCombo, on);
    EnableWindow(s->tgt.hwnd, on);
    EnableWindow(s->hTgtX, on); EnableWindow(s->hTgtY, on); EnableWindow(s->hTgtW, on);
    if (!on){ SendMessageW(s->hTgtCombo, CB_SETCURSEL, (WPARAM)-1, 0); s->tgt.mon = nullptr; }
    InvalidateRect(s->tgt.hwnd, nullptr, FALSE);
}

// load all controls from a MirrorConfig
static void controlsFromConfig(DlgState* s, const MirrorConfig& c){
    const auto& mons = *s->mons;
    int si = ResolveMonitor(mons, c.source);   // stable hwid first, device name fallback
    int ti = ResolveMonitor(mons, c.target);
    selectMonitorIdx(s->hSrcCombo, si);
    selectMonitorIdx(s->hTgtCombo, ti);
    s->src.mon = si >= 0 ? &mons[si] : nullptr;
    s->tgt.mon = ti >= 0 ? &mons[ti] : nullptr;

    if (s->src.mon){
        if (c.source.w <= 0) s->src.region = centeredBox(*s->src.mon, 0.5);  // fresh: centred 50%
        else {
            RECT r = { c.source.x, c.source.y, c.source.x + c.source.w, c.source.y + c.source.h };
            clampInside(r, s->src.mon->w(), s->src.mon->h());
            s->src.region = r;
        }
    }
    // options (rotation set before computing the target's default aspect)
    setInt(s->hFps, c.fps);
    SendMessageW(s->hVsync,  BM_SETCHECK, c.vsync  ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(s->hCursor, BM_SETCHECK, c.cursor ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(s->hCover,  BM_SETCHECK, c.cover  ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(s->hRot, CB_SETCURSEL, (WPARAM)(c.rotation / 90), 0);
    SendMessageW(s->hFlipH, BM_SETCHECK, c.flipH ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(s->hFlipV, BM_SETCHECK, c.flipV ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(s->hWindowed, BM_SETCHECK, c.windowed ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(s->hLog, BM_SETCHECK, c.log ? BST_CHECKED : BST_UNCHECKED, 0);

    if (s->tgt.mon){
        if (c.target.w <= 0) s->tgt.region = centeredRatioBox(*s->tgt.mon, 0.5, dlgEffAspect(s));
        else {
            RECT r = { c.target.x, c.target.y, c.target.x + c.target.w, c.target.y + c.target.h };
            clampInside(r, s->tgt.mon->w(), s->tgt.mon->h());
            s->tgt.region = r;
        }
    }
    refitTarget(s);
    setTargetEnabled(s, s->src.mon != nullptr);
    if (s->src.mon) selectMonitorIdx(s->hTgtCombo, ti);   // re-apply after enable
    writeSrcFields(s); writeTgtFields(s);
    InvalidateRect(s->src.hwnd, nullptr, FALSE);
    InvalidateRect(s->tgt.hwnd, nullptr, FALSE);
}

static bool configFromControls(DlgState* s, MirrorConfig& c){
    int si = (int)SendMessageW(s->hSrcCombo, CB_GETCURSEL, 0, 0);
    int ti = (int)SendMessageW(s->hTgtCombo, CB_GETCURSEL, 0, 0);
    if (si < 0 || ti < 0) return false;
    const auto& mons = *s->mons;
    c.source.device = mons[si].device; c.source.hwid = mons[si].hwid;
    c.target.device = mons[ti].device; c.target.hwid = mons[ti].hwid;
    c.source.x = s->src.region.left; c.source.y = s->src.region.top;
    c.source.w = s->src.region.right - s->src.region.left;
    c.source.h = s->src.region.bottom - s->src.region.top;
    c.target.x = s->tgt.region.left; c.target.y = s->tgt.region.top;
    c.target.w = s->tgt.region.right - s->tgt.region.left;
    c.target.h = s->tgt.region.bottom - s->tgt.region.top;
    c.fps = clampi(getInt(s->hFps), 0, 1000);
    c.vsync    = SendMessageW(s->hVsync,    BM_GETCHECK, 0, 0) == BST_CHECKED;
    c.cursor   = SendMessageW(s->hCursor,   BM_GETCHECK, 0, 0) == BST_CHECKED;
    c.cover    = SendMessageW(s->hCover,    BM_GETCHECK, 0, 0) == BST_CHECKED;
    c.windowed = SendMessageW(s->hWindowed, BM_GETCHECK, 0, 0) == BST_CHECKED;
    c.flipH    = SendMessageW(s->hFlipH,    BM_GETCHECK, 0, 0) == BST_CHECKED;
    c.flipV    = SendMessageW(s->hFlipV,    BM_GETCHECK, 0, 0) == BST_CHECKED;
    c.log      = SendMessageW(s->hLog,      BM_GETCHECK, 0, 0) == BST_CHECKED;
    c.rotation = dlgRotation(s);
    return true;
}

// user typed in the source X/Y/W/H fields
static void srcFieldsToCanvas(DlgState* s){
    if (!s->src.mon) return;
    int x = getInt(s->hSrcX), y = getInt(s->hSrcY), w = getInt(s->hSrcW), h = getInt(s->hSrcH);
    RECT r = { x, y, x + std::max(8,w), y + std::max(8,h) };
    clampInside(r, s->src.mon->w(), s->src.mon->h());
    s->src.region = r;
    refitTarget(s);
    InvalidateRect(s->src.hwnd, nullptr, FALSE);
    InvalidateRect(s->tgt.hwnd, nullptr, FALSE);
    writeTgtFields(s);
}
// user typed in the target X/Y/W fields (H is derived from the locked ratio)
static void tgtFieldsToCanvas(DlgState* s){
    if (!s->tgt.mon) return;
    int x = getInt(s->hTgtX), y = getInt(s->hTgtY), w = getInt(s->hTgtW);
    double asp = s->tgt.lockAspect > 0 ? s->tgt.lockAspect : 1.0;
    int h = (int)std::lround(std::max(8,w) / asp);
    RECT r = { x, y, x + std::max(8,w), y + std::max(8,h) };
    clampInside(r, s->tgt.mon->w(), s->tgt.mon->h());
    s->tgt.region = r;
    refitTarget(s);   // re-clamp exactly to ratio & bounds
    InvalidateRect(s->tgt.hwnd, nullptr, FALSE);
    writeTgtFields(s);
}

// ---- import / export -------------------------------------------------------
static bool browseFile(HWND owner, const std::wstring& startDir, bool save, std::wstring& out){
    wchar_t file[MAX_PATH] = L"mirror.cfg";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Mirror config (*.cfg)\0*.cfg\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = startDir.c_str();
    ofn.lpstrDefExt = L"cfg";
    ofn.Flags = OFN_NOCHANGEDIR | (save ? (OFN_OVERWRITEPROMPT) : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST));
    BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    if (!ok) return false;
    out = file; return true;
}

static const COLORREF CLR_BG        = RGB(242, 244, 248);
static const COLORREF CLR_CARD      = RGB(255, 255, 255);
static const COLORREF CLR_CARD_LINE = RGB(220, 224, 231);
static const COLORREF CLR_SHADOW    = RGB(228, 231, 237);
static const COLORREF CLR_TEXT      = RGB(30, 34, 42);
static const COLORREF CLR_SUB       = RGB(112, 118, 130);
static const COLORREF CLR_ACCENT    = RGB(43, 104, 218);
static const COLORREF CLR_ACCENT_HI = RGB(58, 122, 240);
static const COLORREF CLR_ACCENT_LO = RGB(32, 84, 186);
static const COLORREF CLR_HDRTX     = RGB(255, 255, 255);
static const COLORREF CLR_HDRSUB    = RGB(200, 216, 248);
static const COLORREF CLR_BANNER_BG = RGB(255, 246, 209);   // soft amber info strip
static const COLORREF CLR_BANNER_LN = RGB(240, 214, 140);
static const COLORREF CLR_BANNER_TX = RGB(122, 82, 8);
static const COLORREF CLR_DANGER    = RGB(197, 58, 58);
static const COLORREF CLR_DANGER_HI = RGB(253, 240, 240);

// ---- layout constants (shared by createControls and WM_PAINT) --------------
enum {
    LO_HDR_H = 60, LO_MARGIN = 16, LO_CARD_W = 448, LO_GAP = 12,
    LO_CARD_AX = LO_MARGIN,
    LO_CARD_BX = LO_MARGIN + LO_CARD_W + LO_GAP,
    LO_BANNER_Y = LO_HDR_H + 12, LO_BANNER_H = 44,
    LO_CARD_Y = LO_BANNER_Y + LO_BANNER_H + 12,
    LO_CARD_H = 312,
    LO_PAD = 16,
    LO_OPT_Y = LO_CARD_Y + LO_CARD_H + 16,
    LO_OPT_H = 84,                       // options card holds two rows
    LO_BTN_Y = LO_OPT_Y + LO_OPT_H + 14
};

static HWND makeLabel(HWND p, HINSTANCE hi, LPCWSTR t, int x, int y, int w, int h, HFONT f = nullptr){
    // SS_CENTERIMAGE centres text vertically so descenders (g, p, y) are never clipped.
    HWND c = CreateWindowExW(0, L"STATIC", t, WS_CHILD|WS_VISIBLE|SS_LEFT|SS_CENTERIMAGE,
                             x,y,w,h, p, nullptr, hi, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)(f ? f : g_font), TRUE); return c;
}
static HWND makeEdit(HWND p, HINSTANCE hi, int id, int x, int y, int w, int h){
    HWND c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD|WS_VISIBLE|ES_NUMBER|ES_RIGHT,
                             x,y,w,h, p, (HMENU)(INT_PTR)id, hi, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE); return c;
}
static HWND makeButton(HWND p, HINSTANCE hi, int id, LPCWSTR t, int x, int y, int w, int h){
    HWND c = CreateWindowExW(0, L"BUTTON", t, WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,
                             x,y,w,h, p, (HMENU)(INT_PTR)id, hi, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)(g_btnFont ? g_btnFont : g_font), TRUE); return c;
}
static HWND makeCheck(HWND p, HINSTANCE hi, int id, LPCWSTR t, int x, int y, int w, int h){
    HWND c = CreateWindowExW(0, L"BUTTON", t, WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
                             x,y,w,h, p, (HMENU)(INT_PTR)id, hi, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE); return c;
}

// ---- GDI paint helpers -----------------------------------------------------
static void fillRound(HDC dc, RECT r, int rad, COLORREF fill){
    HBRUSH b = CreateSolidBrush(fill); HPEN pn = CreatePen(PS_SOLID, 1, fill);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, pn);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b); DeleteObject(pn);
}
static void strokeRound(HDC dc, RECT r, int rad, COLORREF line){
    HPEN pn = CreatePen(PS_SOLID, 1, line); HGDIOBJ op = SelectObject(dc, pn);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(pn);
}
static void drawCard(HDC dc, int x, int y, int w, int h){
    RECT sh = { x, y + 2, x + w + 2, y + h + 2 }; fillRound(dc, sh, 14, CLR_SHADOW);  // soft shadow
    RECT c  = { x, y, x + w, y + h };            fillRound(dc, c, 14, CLR_CARD);
    strokeRound(dc, c, 14, CLR_CARD_LINE);
}

// hover tracking for owner-drawn buttons
static LRESULT CALLBACK BtnSub(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref){
    DlgState* s = (DlgState*)ref;
    if (m == WM_MOUSEMOVE){
        if (s && s->hover != h){
            s->hover = h; InvalidateRect(h, nullptr, TRUE);
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t);
        }
    } else if (m == WM_MOUSELEAVE){
        if (s && s->hover == h){ s->hover = nullptr; InvalidateRect(h, nullptr, TRUE); }
    }
    return DefSubclassProc(h, m, w, l);
}
static void drawButton(DRAWITEMSTRUCT* di, DlgState* s){
    HDC dc = di->hDC; RECT r = di->rcItem;
    bool down = (di->itemState & ODS_SELECTED) != 0;
    bool hot  = s && s->hover == di->hwndItem;
    bool primary = (int)di->CtlID == ID_SAVE, danger = (int)di->CtlID == ID_QUIT;
    COLORREF fill, line, tx;
    if (primary){ fill = down ? CLR_ACCENT_LO : (hot ? CLR_ACCENT_HI : CLR_ACCENT); line = fill; tx = RGB(255,255,255); }
    else if (danger){ fill = down ? RGB(246,224,224) : (hot ? CLR_DANGER_HI : CLR_CARD); line = CLR_DANGER; tx = CLR_DANGER; }
    else { fill = down ? RGB(230,235,242) : (hot ? RGB(244,247,251) : CLR_CARD); line = CLR_CARD_LINE; tx = CLR_TEXT; }

    HBRUSH bg = CreateSolidBrush(CLR_BG); FillRect(dc, &r, bg); DeleteObject(bg);   // clear rounded corners
    fillRound(dc, r, 9, fill); strokeRound(dc, r, 9, line);

    wchar_t txt[80] = {0}; GetWindowTextW(di->hwndItem, txt, 80);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, tx);
    HGDIOBJ of = SelectObject(dc, g_btnFont ? g_btnFont : g_font);
    DrawTextW(dc, txt, -1, &r, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    SelectObject(dc, of);
}

static void createControls(HWND hwnd, DlgState* s, HINSTANCE hi, const std::wstring& banner){
    const auto& mons = *s->mons;
    s->banner = banner;

    // Card A (source) / Card B (target) inner geometry
    const int ax = LO_CARD_AX + LO_PAD, bx = LO_CARD_BX + LO_PAD;
    const int innerW = LO_CARD_W - LO_PAD * 2;                 // usable width inside a card
    const int canvasW = 200, gap = 32;                         // wide gap so fields never crowd the box
    const int fw = innerW - canvasW - gap;                     // numeric-field column width
    const int afx = ax + canvasW + gap, bfx = bx + canvasW + gap;
    const int y0 = LO_CARD_Y;                                  // card top

    auto column = [&](int cardX, int cx, int fx, int hdrNum, LPCWSTR title, LPCWSTR hint,
                      HWND& combo, int comboId, Canvas& cv,
                      HWND& eX, HWND& eY, HWND& eW, HWND& eH, int idX, int idY, int idW, int idH,
                      bool tgt){
        makeLabel(hwnd, hi, title, cx, y0 + 14, LO_CARD_W - LO_PAD*2, 24, g_hdrFont);
        makeLabel(hwnd, hi, hint,  cx, y0 + 40, LO_CARD_W - LO_PAD*2, 20, g_subFont);
        combo = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL, cx, y0 + 64, innerW, 300,
            hwnd, (HMENU)(INT_PTR)comboId, hi, nullptr);
        SendMessageW(combo, WM_SETFONT, (WPARAM)g_font, TRUE);

        int cvY = y0 + 100, cvH = 196;
        cv.hwnd = CreateWindowExW(0, L"MirrorCfgCanvas", L"", WS_CHILD|WS_VISIBLE,
                                  cx, cvY, canvasW, cvH, hwnd, nullptr, hi, nullptr);
        cv.parent = hwnd; cv.which = tgt ? 1 : 0; cv.ratioLocked = tgt;
        SetWindowLongPtrW(cv.hwnd, GWLP_USERDATA, (LONG_PTR)&cv);

        // each row: label, then its edit 21px lower (clear gap), rows 49px apart
        int r0 = cvY, rh = 49, lo = 22;
        makeLabel(hwnd, hi, L"X  (px)", fx, r0,        fw, 20); eX = makeEdit(hwnd, hi, idX, fx, r0+lo,        fw, 24);
        makeLabel(hwnd, hi, L"Y  (px)", fx, r0+rh,     fw, 20); eY = makeEdit(hwnd, hi, idY, fx, r0+rh+lo,     fw, 24);
        makeLabel(hwnd, hi, L"Width",   fx, r0+rh*2,   fw, 20); eW = makeEdit(hwnd, hi, idW, fx, r0+rh*2+lo,   fw, 24);
        makeLabel(hwnd, hi, tgt ? L"Height (auto)" : L"Height",
                  fx, r0+rh*3,   fw, 20); eH = makeEdit(hwnd, hi, idH, fx, r0+rh*3+lo,   fw, 24);
    };

    column(LO_CARD_AX, ax, afx, 1,
           L"1  ·  Main working monitor",
           L"Where your drawing app runs — drag the box or type the area.",
           s->hSrcCombo, ID_SRC_COMBO, s->src,
           s->hSrcX, s->hSrcY, s->hSrcW, s->hSrcH, ID_SRC_X, ID_SRC_Y, ID_SRC_W, ID_SRC_H, false);
    column(LO_CARD_BX, bx, bfx, 2,
           L"2  ·  Mirror onto monitor",
           L"Locked to the same shape — drag to place, drag a corner to resize.",
           s->hTgtCombo, ID_TGT_COMBO, s->tgt,
           s->hTgtX, s->hTgtY, s->hTgtW, s->hTgtH, ID_TGT_X, ID_TGT_Y, ID_TGT_W, ID_TGT_H, true);

    EnableWindow(s->hTgtH, FALSE);   // derived from the locked ratio

    for (const auto& m : mons){
        SendMessageW(s->hSrcCombo, CB_ADDSTRING, 0, (LPARAM)m.label().c_str());
        SendMessageW(s->hTgtCombo, CB_ADDSTRING, 0, (LPARAM)m.label().c_str());
    }

    // ---- options card: two rows ----
    int ox = LO_CARD_AX + LO_PAD, oy = LO_OPT_Y + 10, oy2 = LO_OPT_Y + 46;
    // row 1
    makeLabel(hwnd, hi, L"Frame rate (FPS)", ox, oy + 2, 118, 22);
    s->hFps    = makeEdit (hwnd, hi, ID_FPS, ox + 120, oy, 56, 24);
    makeLabel(hwnd, hi, L"0 = uncapped", ox + 182, oy + 2, 96, 22);
    s->hVsync  = makeCheck(hwnd, hi, ID_VSYNC,  L"V-Sync",           ox + 300, oy + 2, 88,  22);
    s->hCursor = makeCheck(hwnd, hi, ID_CURSOR, L"Show cursor",      ox + 396, oy + 2, 128, 22);
    s->hCover  = makeCheck(hwnd, hi, ID_COVER,  L"Black background", ox + 540, oy + 2, 180, 22);
    // row 2
    makeLabel(hwnd, hi, L"Rotation", ox, oy2 + 2, 66, 22);
    s->hRot = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST, ox + 68, oy2, 84, 160, hwnd, (HMENU)ID_ROT, hi, nullptr);
    SendMessageW(s->hRot, WM_SETFONT, (WPARAM)g_font, TRUE);
    for (LPCWSTR r : { L"0°", L"90°", L"180°", L"270°" }) SendMessageW(s->hRot, CB_ADDSTRING, 0, (LPARAM)r);
    SendMessageW(s->hRot, CB_SETCURSEL, 0, 0);
    s->hFlipH    = makeCheck(hwnd, hi, ID_FLIPH,    L"Flip H",          ox + 168, oy2 + 2, 78,  22);
    s->hFlipV    = makeCheck(hwnd, hi, ID_FLIPV,    L"Flip V",          ox + 250, oy2 + 2, 78,  22);
    s->hWindowed = makeCheck(hwnd, hi, ID_WINDOWED, L"Floating window", ox + 336, oy2 + 2, 168, 22);
    s->hLog      = makeCheck(hwnd, hi, ID_LOG,      L"Log to file",     ox + 512, oy2 + 2, 130, 22);

    // ---- buttons ----
    int by = LO_BTN_Y, right = LO_CARD_BX + LO_CARD_W;
    makeButton(hwnd, hi, ID_SAVE,   L"Save && Start",  LO_CARD_AX,       by, 168, 40);
    makeButton(hwnd, hi, ID_IMPORT, L"Import…",        LO_CARD_AX + 180, by, 112, 40);
    makeButton(hwnd, hi, ID_EXPORT, L"Export…",        LO_CARD_AX + 300, by, 112, 40);
    makeButton(hwnd, hi, ID_QUIT,   L"Cancel && Quit", right - 150,      by, 150, 40);

    // subclass the four owner-drawn buttons for hover feedback
    for (int id : { ID_SAVE, ID_IMPORT, ID_EXPORT, ID_QUIT })
        SetWindowSubclass(GetDlgItem(hwnd, id), BtnSub, 1, (DWORD_PTR)s);
}

static LRESULT CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp){
    DlgState* s = (DlgState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg){
    case WM_ERASEBKGND: return 1;   // painted fully in WM_PAINT (no flicker)
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
        RECT cr; GetClientRect(hwnd, &cr);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, cr.right, cr.bottom);
        HGDIOBJ ob = SelectObject(mem, bmp);

        HBRUSH bg = CreateSolidBrush(CLR_BG); FillRect(mem, &cr, bg); DeleteObject(bg);

        // header band
        RECT hb = { 0, 0, cr.right, LO_HDR_H };
        HBRUSH ab = CreateSolidBrush(CLR_ACCENT); FillRect(mem, &hb, ab); DeleteObject(ab);
        SetBkMode(mem, TRANSPARENT);
        SelectObject(mem, g_titleFont); SetTextColor(mem, CLR_HDRTX);
        RECT tr = { LO_MARGIN + 6, 9, cr.right - 16, 41 };
        DrawTextW(mem, L"Display Mirror", -1, &tr, DT_LEFT|DT_SINGLELINE);
        SelectObject(mem, g_subFont); SetTextColor(mem, CLR_HDRSUB);
        RECT sr = { LO_MARGIN + 6, 37, cr.right - 16, 56 };
        DrawTextW(mem, L"Choose what to mirror and where it appears.", -1, &sr, DT_LEFT|DT_SINGLELINE);

        // info / error banner
        if (s && !s->banner.empty()){
            RECT br = { LO_MARGIN, LO_BANNER_Y, cr.right - LO_MARGIN, LO_BANNER_Y + LO_BANNER_H };
            fillRound(mem, br, 10, CLR_BANNER_BG); strokeRound(mem, br, 10, CLR_BANNER_LN);
            SelectObject(mem, g_font); SetTextColor(mem, CLR_BANNER_TX);
            RECT bt = br; bt.left += 14; bt.right -= 14; bt.top += 5;
            DrawTextW(mem, s->banner.c_str(), -1, &bt, DT_LEFT|DT_WORDBREAK);
        }

        // cards
        drawCard(mem, LO_CARD_AX, LO_CARD_Y, LO_CARD_W, LO_CARD_H);
        drawCard(mem, LO_CARD_BX, LO_CARD_Y, LO_CARD_W, LO_CARD_H);
        drawCard(mem, LO_CARD_AX, LO_OPT_Y, LO_CARD_BX + LO_CARD_W - LO_CARD_AX, LO_OPT_H);

        BitBlt(dc, 0, 0, cr.right, cr.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* di = (DRAWITEMSTRUCT*)lp;
        if (di->CtlType == ODT_BUTTON){ drawButton(di, s); return TRUE; }
        break;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, OPAQUE); SetTextColor(dc, CLR_TEXT); SetBkColor(dc, CLR_CARD);
        return (LRESULT)g_cardBrush;   // controls sit on white cards
    }
    case WM_CANVAS_CHANGED:
        if (s){
            if (wp == 0){ writeSrcFields(s); refitTarget(s); writeTgtFields(s);
                          InvalidateRect(s->tgt.hwnd, nullptr, FALSE); }
            else        { writeTgtFields(s); }
        }
        return 0;
    case WM_COMMAND: {
        if (!s) return 0;
        int id = LOWORD(wp), code = HIWORD(wp);
        if (code == CBN_SELCHANGE && (id == ID_SRC_COMBO || id == ID_TGT_COMBO)){
            const auto& mons = *s->mons;
            int sel = (int)SendMessageW((HWND)lp, CB_GETCURSEL, 0, 0);
            if (id == ID_SRC_COMBO){
                s->src.mon = (sel >= 0) ? &mons[sel] : nullptr;
                if (s->src.mon)
                    s->src.region = centeredBox(*s->src.mon, 0.5);   // start centred, 50%
                // the mirror monitor becomes selectable now that we know the ratio
                setTargetEnabled(s, s->src.mon != nullptr);
                refitTarget(s);
                writeSrcFields(s); writeTgtFields(s);
                InvalidateRect(s->src.hwnd, nullptr, FALSE);
                InvalidateRect(s->tgt.hwnd, nullptr, FALSE);
            } else {                                                 // target
                s->tgt.mon = (sel >= 0) ? &mons[sel] : nullptr;
                if (s->tgt.mon){
                    double asp = dlgEffAspect(s);
                    s->tgt.lockAspect = asp;
                    s->tgt.region = centeredRatioBox(*s->tgt.mon, 0.5, asp);  // centred, 50%, locked ratio
                }
                writeTgtFields(s);
                InvalidateRect(s->tgt.hwnd, nullptr, FALSE);
            }
            return 0;
        }
        if (code == CBN_SELCHANGE && id == ID_ROT){   // rotation changes the target's shape
            refitTarget(s); writeTgtFields(s);
            InvalidateRect(s->tgt.hwnd, nullptr, FALSE);
            return 0;
        }
        if (code == EN_CHANGE && !s->syncing){
            if (id==ID_SRC_X||id==ID_SRC_Y||id==ID_SRC_W||id==ID_SRC_H){ srcFieldsToCanvas(s); writeSrcFields(s); }
            else if (id==ID_TGT_X||id==ID_TGT_Y||id==ID_TGT_W)         { tgtFieldsToCanvas(s); }
            return 0;
        }
        if (code == EN_KILLFOCUS){   // field lost focus: show the actual (clamped) value
            if (id==ID_SRC_X||id==ID_SRC_Y||id==ID_SRC_W||id==ID_SRC_H) writeSrcFields(s);
            else if (id==ID_TGT_X||id==ID_TGT_Y||id==ID_TGT_W)          writeTgtFields(s);
            return 0;
        }
        if (code == BN_CLICKED){
            if (id == ID_QUIT){ s->result = ConfigResult::Quit; s->done = true; DestroyWindow(hwnd); return 0; }
            if (id == ID_SAVE){
                MirrorConfig c;
                if (!configFromControls(s, c)){
                    MessageBoxW(hwnd, L"Please choose both a working monitor and a mirror monitor.",
                                L"Setup", MB_ICONWARNING); return 0; }
                std::wstring err = ValidateConfig(c, *s->mons);
                if (!err.empty()){ MessageBoxW(hwnd, err.c_str(), L"Setup", MB_ICONWARNING); return 0; }
                if (!SaveConfig(c, s->defaultPath)){
                    MessageBoxW(hwnd, L"Could not write the configuration file.", L"Setup", MB_ICONERROR); return 0; }
                *s->out = c; s->result = ConfigResult::Save; s->done = true; DestroyWindow(hwnd); return 0;
            }
            if (id == ID_IMPORT){
                std::wstring path;
                if (browseFile(hwnd, ExeDir(), false, path)){
                    MirrorConfig c;
                    if (LoadConfig(c, path)){ controlsFromConfig(s, c);
                        MessageBoxW(hwnd, L"Configuration imported. Review it, then click Save && Start.",
                                    L"Import", MB_ICONINFORMATION); }
                    else MessageBoxW(hwnd, L"That file is not a valid mirror configuration.",
                                     L"Import", MB_ICONERROR);
                }
                return 0;
            }
            if (id == ID_EXPORT){
                MirrorConfig c;
                if (!configFromControls(s, c)){
                    MessageBoxW(hwnd, L"Choose both monitors before exporting.", L"Export", MB_ICONWARNING); return 0; }
                std::wstring path;
                if (browseFile(hwnd, ExeDir(), true, path)){
                    if (SaveConfig(c, path)) MessageBoxW(hwnd, L"Configuration exported.", L"Export", MB_ICONINFORMATION);
                    else MessageBoxW(hwnd, L"Could not write that file.", L"Export", MB_ICONERROR);
                }
                return 0;
            }
        }
        return 0;
    }
    case WM_CLOSE:
        if (s){ s->result = ConfigResult::Quit; s->done = true; }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

ConfigResult RunConfigDialog(MirrorConfig& cfg,
                             const std::vector<MonitorInfo>& mons,
                             HINSTANCE hInst,
                             const std::wstring& banner,
                             const std::wstring& defaultPath){
    {
        INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES };
        InitCommonControlsEx(&icc);   // opt into themed (modern) common controls
    }
    auto mkFont = [](int px, int weight){
        return CreateFontW(-px, 0,0,0, weight, 0,0,0, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH|FF_DONTCARE, L"Segoe UI");
    };
    if (!g_font)      g_font      = mkFont(15, FW_NORMAL);
    if (!g_subFont)   g_subFont   = mkFont(13, FW_NORMAL);
    if (!g_hdrFont)   g_hdrFont   = mkFont(17, FW_SEMIBOLD);
    if (!g_btnFont)   g_btnFont   = mkFont(15, FW_SEMIBOLD);
    if (!g_titleFont) g_titleFont = mkFont(22, FW_SEMIBOLD);
    if (!g_bgBrush)   g_bgBrush   = CreateSolidBrush(CLR_BG);
    if (!g_cardBrush) g_cardBrush = CreateSolidBrush(CLR_CARD);

    static bool canvasReg = false;
    if (!canvasReg){
        WNDCLASSW wc = {};
        wc.lpfnWndProc = CanvasProc; wc.hInstance = hInst;
        wc.lpszClassName = L"MirrorCfgCanvas";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        RegisterClassW(&wc);
        canvasReg = true;
    }
    static bool dlgReg = false;
    if (!dlgReg){
        WNDCLASSW wc = {};
        wc.lpfnWndProc = DlgProc; wc.hInstance = hInst;
        wc.lpszClassName = L"MirrorCfgDialog";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = g_bgBrush;
        wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));  // app icon (IDI_ICON1)
        RegisterClassW(&wc);
        dlgReg = true;
    }

    DlgState st;
    st.mons = &mons; st.out = &cfg; st.defaultPath = defaultPath;

    // client must fit: header + banner + cards + options + buttons + margin
    int clientW = LO_CARD_BX + LO_CARD_W + LO_MARGIN;
    int clientH = LO_BTN_Y + 40 + LO_MARGIN;
    RECT wr = { 0, 0, clientW, clientH };
    AdjustWindowRectEx(&wr, WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU, FALSE, WS_EX_APPWINDOW);
    int W = wr.right - wr.left, H = wr.bottom - wr.top;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - W) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - H) / 2;
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, L"MirrorCfgDialog", L"Mirror - Setup",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, sx, sy, W, H, nullptr, nullptr, hInst, nullptr);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)&st);

    createControls(hwnd, &st, hInst, banner);
    // seed from the incoming cfg (may be blank on first launch)
    controlsFromConfig(&st, cfg);

    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);

    MSG msg;
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0)){
        if (!IsDialogMessageW(hwnd, &msg)){
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
    }
    return st.result;
}
