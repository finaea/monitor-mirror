// ===========================================================================
// config.h - Mirror configuration: model, load/save, and validation.
//
// The config describes WHAT to mirror and WHERE:
//   source : a monitor + a pixel rectangle on it (the drawing / working area)
//   target : a monitor + a rectangle on it (where the mirror is shown), whose
//            aspect ratio is locked to the source rectangle's.
//   fps    : present rate cap for the mirror (0 = uncapped, follow source).
//
// Rectangles are stored MONITOR-LOCAL (0,0 = that monitor's top-left) so a
// config stays meaningful if the desktop is rearranged. Monitors are keyed by
// device name (\\.\DISPLAYx).
//
// The file is a tiny INI. It is auto-loaded/auto-saved next to the exe so the
// app remembers between launches; Import/Export just let the user move it
// around (the file dialogs start in the exe's directory).
// ===========================================================================
#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cwchar>
#include "monitors.h"

struct RegionCfg {
    std::wstring device;              // which monitor
    int x = 0, y = 0, w = 0, h = 0;   // rectangle, monitor-local pixels
};

struct MirrorConfig {
    RegionCfg source;
    RegionCfg target;
    int  fps    = 0;        // 0 = uncapped
    bool vsync  = false;
    bool cursor = true;
    bool cover  = true;     // true = fill the rest of the mirror monitor black;
                            // false = only cover the mirror rectangle (desktop shows through)

    double srcAspect() const { return source.h ? (double)source.w / source.h : 1.0; }
};

// ---- exe-directory helpers -------------------------------------------------
inline std::wstring ExeDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path);
    size_t slash = s.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? L"." : s.substr(0, slash);
}
inline std::wstring DefaultConfigPath() { return ExeDir() + L"\\mirror.cfg"; }

// ---- tiny INI writer/reader ------------------------------------------------
inline bool SaveConfig(const MirrorConfig& c, const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"w, ccs=UTF-8");
    if (!f) return false;
    // NOTE: %ls (not %s) - in this CRT's wide fwprintf, %s treats the argument
    // as a narrow string and truncates the wchar_t* at its first zero byte.
    fwprintf(f, L"# mirror configuration\n");
    fwprintf(f, L"[source]\ndevice=%ls\nx=%d\ny=%d\nw=%d\nh=%d\n",
             c.source.device.c_str(), c.source.x, c.source.y, c.source.w, c.source.h);
    fwprintf(f, L"[target]\ndevice=%ls\nx=%d\ny=%d\nw=%d\nh=%d\n",
             c.target.device.c_str(), c.target.x, c.target.y, c.target.w, c.target.h);
    fwprintf(f, L"[options]\nfps=%d\nvsync=%d\ncursor=%d\ncover=%d\n",
             c.fps, c.vsync ? 1 : 0, c.cursor ? 1 : 0, c.cover ? 1 : 0);
    fclose(f);
    return true;
}

inline bool LoadConfig(MirrorConfig& c, const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"r, ccs=UTF-8");
    if (!f) return false;
    wchar_t line[512];
    std::wstring section;
    bool sawSource = false, sawTarget = false;
    while (fgetws(line, 512, f)) {
        // trim leading/trailing whitespace
        std::wstring s(line);
        size_t a = s.find_first_not_of(L" \t\r\n");
        if (a == std::wstring::npos) continue;
        size_t b = s.find_last_not_of(L" \t\r\n");
        s = s.substr(a, b - a + 1);
        if (s.empty() || s[0] == L'#' || s[0] == L';') continue;
        if (s[0] == L'[') { section = s.substr(1, s.find(L']') - 1); continue; }

        size_t eq = s.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = s.substr(0, eq), val = s.substr(eq + 1);

        RegionCfg* r = nullptr;
        if (section == L"source") { r = &c.source; sawSource = true; }
        else if (section == L"target") { r = &c.target; sawTarget = true; }

        if (r) {
            if      (key == L"device") r->device = val;
            else if (key == L"x") r->x = _wtoi(val.c_str());
            else if (key == L"y") r->y = _wtoi(val.c_str());
            else if (key == L"w") r->w = _wtoi(val.c_str());
            else if (key == L"h") r->h = _wtoi(val.c_str());
        } else if (section == L"options") {
            if      (key == L"fps")    c.fps    = _wtoi(val.c_str());
            else if (key == L"vsync")  c.vsync  = _wtoi(val.c_str()) != 0;
            else if (key == L"cursor") c.cursor = _wtoi(val.c_str()) != 0;
            else if (key == L"cover")  c.cover  = _wtoi(val.c_str()) != 0;
        }
    }
    fclose(f);
    return sawSource && sawTarget && !c.source.device.empty() && !c.target.device.empty();
}

// ---- validation against the CURRENT monitors -------------------------------
// Returns an empty string if the config is usable right now, otherwise a
// human-readable reason (shown as the banner in the setup dialog).
inline std::wstring ValidateConfig(const MirrorConfig& c, const std::vector<MonitorInfo>& mons) {
    if (mons.empty())
        return L"No monitors were detected.";

    int si = FindMonitor(mons, c.source.device);
    if (si < 0)
        return L"The main working monitor from your configuration (" + c.source.device +
               L") is not connected.";
    int ti = FindMonitor(mons, c.target.device);
    if (ti < 0)
        return L"The mirror monitor from your configuration (" + c.target.device +
               L") is not connected.";
    if (si == ti)
        return L"The working monitor and the mirror monitor are the same. Pick two different monitors.";

    const MonitorInfo& sm = mons[si];
    const MonitorInfo& tm = mons[ti];

    if (c.source.w < 8 || c.source.h < 8)
        return L"The working area is too small.";
    if (c.source.x < 0 || c.source.y < 0 ||
        c.source.x + c.source.w > sm.w() || c.source.y + c.source.h > sm.h())
        return L"The working area no longer fits on the main monitor "
               L"(its resolution may have changed).";

    if (c.target.w < 8 || c.target.h < 8)
        return L"The mirror area is too small.";
    if (c.target.x < 0 || c.target.y < 0 ||
        c.target.x + c.target.w > tm.w() || c.target.y + c.target.h > tm.h())
        return L"The mirror area no longer fits on the mirror monitor "
               L"(its resolution may have changed).";

    // aspect ratio must match (target is ratio-locked to source); allow ~1% drift.
    double sa = c.srcAspect();
    double ta = c.target.h ? (double)c.target.w / c.target.h : 0.0;
    if (sa > 0 && ta > 0) {
        double diff = (sa > ta) ? (sa - ta) / sa : (ta - sa) / ta;
        if (diff > 0.02)
            return L"The mirror area no longer matches the working area's shape (aspect ratio).";
    }

    if (c.fps < 0 || c.fps > 1000)
        return L"The frame rate is out of range (0-1000).";

    return L"";  // OK
}
