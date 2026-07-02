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
    ID_SAVE = 140, ID_IMPORT, ID_EXPORT, ID_QUIT
};
static const UINT WM_CANVAS_CHANGED = WM_APP + 20;   // wParam: 0=source, 1=target

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

// resize with the aspect ratio preserved, anchored at the corner opposite `h`.
static void resizeRatio(Canvas* c, int h, int mx, int my){
    const RECT r0 = c->dragRegion0;
    int mw = c->mon->w(), mh = c->mon->h();
    double asp = c->lockAspect; if (asp <= 0) asp = 1;
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

    HBRUSH bg = CreateSolidBrush(RGB(252,252,253)); FillRect(mem, &cr, bg); DeleteObject(bg);
    HBRUSH border = CreateSolidBrush(RGB(206,211,219)); FrameRect(mem, &cr, border); DeleteObject(border);

    if (c->mon){
        computeView(c);
        // monitor body
        POINT a = m2c(c, 0, 0), b = m2c(c, c->mon->w(), c->mon->h());
        RECT mr = { a.x, a.y, b.x, b.y };
        HBRUSH mb = CreateSolidBrush(RGB(40,44,52)); FillRect(mem, &mr, mb); DeleteObject(mb);
        FrameRect(mem, &mr, (HBRUSH)GetStockObject(BLACK_BRUSH));

        // region
        POINT ra = m2c(c, c->region.left, c->region.top);
        POINT rb = m2c(c, c->region.right, c->region.bottom);
        RECT rr = { ra.x, ra.y, rb.x, rb.y };
        COLORREF fill = c->ratioLocked ? RGB(46,120,90) : RGB(53,110,214);
        HBRUSH rb2 = CreateSolidBrush(fill); FillRect(mem, &rr, rb2); DeleteObject(rb2);
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(120,200,255));
        HPEN op = (HPEN)SelectObject(mem, pen);
        HBRUSH ob = (HBRUSH)SelectObject(mem, GetStockObject(NULL_BRUSH));
        Rectangle(mem, rr.left, rr.top, rr.right, rr.bottom);
        SelectObject(mem, ob); SelectObject(mem, op); DeleteObject(pen);

        // handles
        POINT hp[8]; handlePoints(c->region, hp);
        HBRUSH hb = CreateSolidBrush(RGB(255,255,255));
        for (int i = 0; i < 8; ++i){
            if (c->ratioLocked && !isCorner(i)) continue;
            POINT p = m2c(c, hp[i].x, hp[i].y);
            RECT h = { p.x-4, p.y-4, p.x+4, p.y+4 };
            FillRect(mem, &h, hb); FrameRect(mem, &h, (HBRUSH)GetStockObject(BLACK_BRUSH));
        }
        DeleteObject(hb);

        // size label
        wchar_t txt[64];
        swprintf(txt, 64, L"%d x %d px", c->region.right-c->region.left, c->region.bottom-c->region.top);
        SetBkMode(mem, TRANSPARENT); SetTextColor(mem, RGB(230,230,230));
        RECT tr = { mr.left+4, mr.top+3, mr.right, mr.top+22 };
        DrawTextW(mem, txt, -1, &tr, DT_LEFT|DT_TOP|DT_SINGLELINE);
    } else {
        SetBkMode(mem, TRANSPARENT); SetTextColor(mem, RGB(140,145,154));
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
    case WM_LBUTTONDOWN: {
        if (!c || !c->mon) return 0;
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
        if (c->dragMode == 8)                    moveRegion(c, mm.x, mm.y);
        else if (c->ratioLocked)                 resizeRatio(c, c->dragMode, mm.x, mm.y);
        else                                     resizeFree(c, c->dragMode, mm.x, mm.y);
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
    HWND hFps=0, hVsync=0, hCursor=0, hCover=0, hBanner=0;
};

static void setInt(HWND e, int v){ wchar_t b[16]; swprintf(b,16,L"%d",v); SetWindowTextW(e,b); }
static int  getInt(HWND e){ wchar_t b[16]={0}; GetWindowTextW(e,b,16); return _wtoi(b); }

static void writeSrcFields(DlgState* s){
    s->syncing = true;
    setInt(s->hSrcX, s->src.region.left);
    setInt(s->hSrcY, s->src.region.top);
    setInt(s->hSrcW, s->src.region.right - s->src.region.left);
    setInt(s->hSrcH, s->src.region.bottom - s->src.region.top);
    s->syncing = false;
}
static void writeTgtFields(DlgState* s){
    s->syncing = true;
    setInt(s->hTgtX, s->tgt.region.left);
    setInt(s->hTgtY, s->tgt.region.top);
    setInt(s->hTgtW, s->tgt.region.right - s->tgt.region.left);
    setInt(s->hTgtH, s->tgt.region.bottom - s->tgt.region.top);
    s->syncing = false;
}

// re-shape the target box to the source aspect (keep its top-left & width, clamp)
static void refitTarget(DlgState* s){
    if (!s->tgt.mon) return;
    double asp = s->src.region.bottom > s->src.region.top
        ? (double)(s->src.region.right - s->src.region.left) /
          (s->src.region.bottom - s->src.region.top) : 1.0;
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

static void selectMonitorInCombo(HWND combo, const std::vector<MonitorInfo>& mons, const std::wstring& dev){
    int idx = FindMonitor(mons, dev);
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
    selectMonitorInCombo(s->hSrcCombo, mons, c.source.device);
    selectMonitorInCombo(s->hTgtCombo, mons, c.target.device);
    int si = FindMonitor(mons, c.source.device);
    int ti = FindMonitor(mons, c.target.device);
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
    double srcAsp = (s->src.region.bottom > s->src.region.top)
        ? (double)(s->src.region.right - s->src.region.left) /
          (s->src.region.bottom - s->src.region.top) : 1.0;
    if (s->tgt.mon){
        if (c.target.w <= 0) s->tgt.region = centeredRatioBox(*s->tgt.mon, 0.5, srcAsp);
        else {
            RECT r = { c.target.x, c.target.y, c.target.x + c.target.w, c.target.y + c.target.h };
            clampInside(r, s->tgt.mon->w(), s->tgt.mon->h());
            s->tgt.region = r;
        }
    }
    setInt(s->hFps, c.fps);
    SendMessageW(s->hVsync,  BM_SETCHECK, c.vsync  ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(s->hCursor, BM_SETCHECK, c.cursor ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(s->hCover,  BM_SETCHECK, c.cover  ? BST_CHECKED : BST_UNCHECKED, 0);
    refitTarget(s);
    setTargetEnabled(s, s->src.mon != nullptr);
    if (s->src.mon) selectMonitorInCombo(s->hTgtCombo, mons, c.target.device);  // re-apply after enable
    writeSrcFields(s); writeTgtFields(s);
    InvalidateRect(s->src.hwnd, nullptr, FALSE);
    InvalidateRect(s->tgt.hwnd, nullptr, FALSE);
}

static bool configFromControls(DlgState* s, MirrorConfig& c){
    int si = (int)SendMessageW(s->hSrcCombo, CB_GETCURSEL, 0, 0);
    int ti = (int)SendMessageW(s->hTgtCombo, CB_GETCURSEL, 0, 0);
    if (si < 0 || ti < 0) return false;
    const auto& mons = *s->mons;
    c.source.device = mons[si].device;
    c.target.device = mons[ti].device;
    c.source.x = s->src.region.left; c.source.y = s->src.region.top;
    c.source.w = s->src.region.right - s->src.region.left;
    c.source.h = s->src.region.bottom - s->src.region.top;
    c.target.x = s->tgt.region.left; c.target.y = s->tgt.region.top;
    c.target.w = s->tgt.region.right - s->tgt.region.left;
    c.target.h = s->tgt.region.bottom - s->tgt.region.top;
    c.fps = clampi(getInt(s->hFps), 0, 1000);
    c.vsync  = SendMessageW(s->hVsync,  BM_GETCHECK, 0, 0) == BST_CHECKED;
    c.cursor = SendMessageW(s->hCursor, BM_GETCHECK, 0, 0) == BST_CHECKED;
    c.cover  = SendMessageW(s->hCover,  BM_GETCHECK, 0, 0) == BST_CHECKED;
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

static HFONT  g_font = nullptr, g_hdrFont = nullptr, g_titleFont = nullptr;
static HBRUSH g_bgBrush = nullptr, g_bannerBrush = nullptr;
static const COLORREF CLR_BG        = RGB(247, 248, 250);
static const COLORREF CLR_TEXT      = RGB(32, 36, 44);
static const COLORREF CLR_SUB       = RGB(110, 116, 126);
static const COLORREF CLR_BANNER_BG = RGB(255, 246, 209);   // soft amber info strip
static const COLORREF CLR_BANNER_TX = RGB(122, 82, 8);

static HWND makeLabel(HWND p, HINSTANCE hi, LPCWSTR t, int x, int y, int w, int h, HFONT f = nullptr){
    HWND c = CreateWindowExW(0, L"STATIC", t, WS_CHILD|WS_VISIBLE, x,y,w,h, p, nullptr, hi, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)(f ? f : g_font), TRUE); return c;
}
static HWND makeEdit(HWND p, HINSTANCE hi, int id, int x, int y, int w, int h){
    HWND c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD|WS_VISIBLE|ES_NUMBER|ES_RIGHT,
                             x,y,w,h, p, (HMENU)(INT_PTR)id, hi, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE); return c;
}
static HWND makeButton(HWND p, HINSTANCE hi, int id, LPCWSTR t, int x, int y, int w, int h, bool def = false){
    HWND c = CreateWindowExW(0, L"BUTTON", t, WS_CHILD|WS_VISIBLE|(def ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON),
                             x,y,w,h, p, (HMENU)(INT_PTR)id, hi, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE); return c;
}
static HWND makeCheck(HWND p, HINSTANCE hi, int id, LPCWSTR t, int x, int y, int w, int h){
    HWND c = CreateWindowExW(0, L"BUTTON", t, WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
                             x,y,w,h, p, (HMENU)(INT_PTR)id, hi, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE); return c;
}

static void createControls(HWND hwnd, DlgState* s, HINSTANCE hi, const std::wstring& banner){
    const auto& mons = *s->mons;
    const int L = 18, R = 470;                 // left / right column x
    const int COLW = 434;                      // column width
    const int fx = L + 312, tx = R + 312, fw = 110;   // numeric-field x + width

    // ---- title ----
    makeLabel(hwnd, hi, L"Display Mirror — Setup", L, 12, 500, 32, g_titleFont);

    // ---- banner / info strip ----
    s->hBanner = makeLabel(hwnd, hi, banner.c_str(), L, 50, COLW*2 + (R-L-COLW), 40);

    // ---- section 1 : source ----
    makeLabel(hwnd, hi, L"1  ·  Main working monitor", L, 100, COLW, 22, g_hdrFont);
    makeLabel(hwnd, hi, L"Where your drawing app runs. Drag the box, or type the exact area.",
              L, 124, COLW, 16);
    s->hSrcCombo = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL, L, 146, COLW, 300,
        hwnd, (HMENU)ID_SRC_COMBO, hi, nullptr);
    SendMessageW(s->hSrcCombo, WM_SETFONT, (WPARAM)g_font, TRUE);

    // ---- section 2 : target ----
    makeLabel(hwnd, hi, L"2  ·  Mirror onto monitor", R, 100, COLW, 22, g_hdrFont);
    makeLabel(hwnd, hi, L"Kept locked to the same shape. Drag to place, drag a corner to resize.",
              R, 124, COLW, 16);
    s->hTgtCombo = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL, R, 146, COLW, 300,
        hwnd, (HMENU)ID_TGT_COMBO, hi, nullptr);
    SendMessageW(s->hTgtCombo, WM_SETFONT, (WPARAM)g_font, TRUE);

    for (const auto& m : mons){
        SendMessageW(s->hSrcCombo, CB_ADDSTRING, 0, (LPARAM)m.label().c_str());
        SendMessageW(s->hTgtCombo, CB_ADDSTRING, 0, (LPARAM)m.label().c_str());
    }

    // canvases (borderless; each paints its own card + border)
    s->src.hwnd = CreateWindowExW(0, L"MirrorCfgCanvas", L"",
        WS_CHILD|WS_VISIBLE, L, 182, 300, 196, hwnd, nullptr, hi, nullptr);
    s->tgt.hwnd = CreateWindowExW(0, L"MirrorCfgCanvas", L"",
        WS_CHILD|WS_VISIBLE, R, 182, 300, 196, hwnd, nullptr, hi, nullptr);
    s->src.parent = s->tgt.parent = hwnd;
    s->src.which = 0; s->tgt.which = 1;
    s->tgt.ratioLocked = true;
    SetWindowLongPtrW(s->src.hwnd, GWLP_USERDATA, (LONG_PTR)&s->src);
    SetWindowLongPtrW(s->tgt.hwnd, GWLP_USERDATA, (LONG_PTR)&s->tgt);

    // numeric fields (right of each canvas)
    makeLabel(hwnd, hi, L"X  (px)", fx, 182, fw, 16); s->hSrcX = makeEdit(hwnd, hi, ID_SRC_X, fx, 200, fw, 24);
    makeLabel(hwnd, hi, L"Y  (px)", fx, 230, fw, 16); s->hSrcY = makeEdit(hwnd, hi, ID_SRC_Y, fx, 248, fw, 24);
    makeLabel(hwnd, hi, L"Width",  fx, 278, fw, 16);  s->hSrcW = makeEdit(hwnd, hi, ID_SRC_W, fx, 296, fw, 24);
    makeLabel(hwnd, hi, L"Height", fx, 326, fw, 16);  s->hSrcH = makeEdit(hwnd, hi, ID_SRC_H, fx, 344, fw, 24);

    makeLabel(hwnd, hi, L"X  (px)", tx, 182, fw, 16); s->hTgtX = makeEdit(hwnd, hi, ID_TGT_X, tx, 200, fw, 24);
    makeLabel(hwnd, hi, L"Y  (px)", tx, 230, fw, 16); s->hTgtY = makeEdit(hwnd, hi, ID_TGT_Y, tx, 248, fw, 24);
    makeLabel(hwnd, hi, L"Width",  tx, 278, fw, 16);  s->hTgtW = makeEdit(hwnd, hi, ID_TGT_W, tx, 296, fw, 24);
    makeLabel(hwnd, hi, L"Height (auto)", tx, 326, fw, 16);
    s->hTgtH = makeEdit(hwnd, hi, ID_TGT_H, tx, 344, fw, 24);
    EnableWindow(s->hTgtH, FALSE);   // derived from the locked ratio

    // ---- options row ----
    int oy = 392;
    makeLabel(hwnd, hi, L"Frame rate (FPS)", L, oy + 4, 120, 18);
    s->hFps    = makeEdit (hwnd, hi, ID_FPS, L + 122, oy, 56, 24);
    makeLabel(hwnd, hi, L"0 = uncapped", L + 184, oy + 4, 96, 18);
    s->hVsync  = makeCheck(hwnd, hi, ID_VSYNC,  L"V-Sync",           L + 292, oy + 2, 90,  22);
    s->hCursor = makeCheck(hwnd, hi, ID_CURSOR, L"Show cursor",      L + 388, oy + 2, 130, 22);
    s->hCover  = makeCheck(hwnd, hi, ID_COVER,  L"Black background", L + 524, oy + 2, 170, 22);

    // ---- buttons ----
    int by = 432;
    makeButton(hwnd, hi, ID_SAVE,   L"Save && Start",  L,       by, 160, 36, true);
    makeButton(hwnd, hi, ID_IMPORT, L"Import…",   L + 170, by, 110, 36);
    makeButton(hwnd, hi, ID_EXPORT, L"Export…",   L + 288, by, 110, 36);
    makeButton(hwnd, hi, ID_QUIT,   L"Cancel && Quit", R + 320, by, 132, 36);
}

static LRESULT CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp){
    DlgState* s = (DlgState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg){
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, OPAQUE);
        if (s && (HWND)lp == s->hBanner){
            SetTextColor(dc, CLR_BANNER_TX); SetBkColor(dc, CLR_BANNER_BG);
            return (LRESULT)g_bannerBrush;
        }
        SetTextColor(dc, CLR_TEXT); SetBkColor(dc, CLR_BG);
        return (LRESULT)g_bgBrush;
    }
    case WM_CTLCOLORBTN: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, OPAQUE); SetTextColor(dc, CLR_TEXT); SetBkColor(dc, CLR_BG);
        return (LRESULT)g_bgBrush;
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
                    double asp = (s->src.region.bottom > s->src.region.top)
                        ? (double)(s->src.region.right - s->src.region.left) /
                          (s->src.region.bottom - s->src.region.top) : 1.0;
                    s->tgt.lockAspect = asp;
                    s->tgt.region = centeredRatioBox(*s->tgt.mon, 0.5, asp);  // centred, 50%, locked ratio
                }
                writeTgtFields(s);
                InvalidateRect(s->tgt.hwnd, nullptr, FALSE);
            }
            return 0;
        }
        if (code == EN_CHANGE && !s->syncing){
            if (id==ID_SRC_X||id==ID_SRC_Y||id==ID_SRC_W||id==ID_SRC_H){ srcFieldsToCanvas(s); writeSrcFields(s); }
            else if (id==ID_TGT_X||id==ID_TGT_Y||id==ID_TGT_W)         { tgtFieldsToCanvas(s); }
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
    if (!g_hdrFont)   g_hdrFont   = mkFont(17, FW_SEMIBOLD);
    if (!g_titleFont) g_titleFont = mkFont(23, FW_SEMIBOLD);
    if (!g_bgBrush)     g_bgBrush     = CreateSolidBrush(CLR_BG);
    if (!g_bannerBrush) g_bannerBrush = CreateSolidBrush(CLR_BANNER_BG);

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

    int W = 940, H = 524;
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
