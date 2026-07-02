// ===========================================================================
// monitors.h - Shared DXGI monitor enumeration.
//
// Both the configuration GUI (needs the list of monitors, their sizes and
// positions) and the renderer (needs the matching DXGI adapter/output to
// duplicate) use this. Monitors are identified by their device name
// (\\.\DISPLAYx) so a saved config can be matched back to hardware on launch.
// ===========================================================================
#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <string>
#include <vector>

struct MonitorInfo {
    std::wstring device;      // \\.\DISPLAYx  (can renumber on re-plug)
    std::wstring hwid;        // stable hardware id (monitor interface path / EDID-derived)
    RECT         rect{};      // desktop coordinates (physical pixels)
    bool         primary = false;
    UINT         adapterIdx = 0;
    UINT         outputIdx  = 0;

    int  w() const { return rect.right  - rect.left; }
    int  h() const { return rect.bottom - rect.top;  }

    // Human label for combo boxes, e.g. "DISPLAY1  2560x1440  @(0,0)  (primary)".
    std::wstring label() const {
        std::wstring name = device;
        if (name.rfind(L"\\\\.\\", 0) == 0) name = name.substr(4);  // strip the device prefix
        std::wstring s = name;
        s += L"   "; s += std::to_wstring(w()); s += L"x"; s += std::to_wstring(h());
        s += L"   @("; s += std::to_wstring((long)rect.left); s += L",";
        s += std::to_wstring((long)rect.top); s += L")";
        if (primary) s += L"   (primary)";
        return s;
    }
};

// Enumerate all desktop-attached outputs via DXGI. The primary is the one whose
// top-left is the desktop origin (0,0).
inline std::vector<MonitorInfo> EnumMonitors() {
    using Microsoft::WRL::ComPtr;
    std::vector<MonitorInfo> mons;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)factory.GetAddressOf())))
        return mons;

    ComPtr<IDXGIAdapter1> ad;
    for (UINT ai = 0; factory->EnumAdapters1(ai, ad.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++ai) {
        ComPtr<IDXGIOutput> o;
        for (UINT oi = 0; ad->EnumOutputs(oi, o.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++oi) {
            DXGI_OUTPUT_DESC d;
            o->GetDesc(&d);
            if (!d.AttachedToDesktop) continue;
            MonitorInfo mi;
            mi.device     = d.DeviceName;
            mi.rect       = d.DesktopCoordinates;
            mi.primary    = (d.DesktopCoordinates.left == 0 && d.DesktopCoordinates.top == 0);
            mi.adapterIdx = ai;
            mi.outputIdx  = oi;
            // stable hardware id: the monitor interface path (contains the EDID
            // manufacturer/product and a per-connection instance), which survives
            // \\.\DISPLAYx renumbering when displays are re-plugged.
            DISPLAY_DEVICEW dd = {}; dd.cb = sizeof(dd);
            if (EnumDisplayDevicesW(d.DeviceName, 0, &dd, EDD_GET_DEVICE_INTERFACE_NAME) && dd.DeviceID[0])
                mi.hwid = dd.DeviceID;
            mons.push_back(mi);
        }
    }
    return mons;
}

// Find a monitor by device name. Returns index into `mons`, or -1.
inline int FindMonitor(const std::vector<MonitorInfo>& mons, const std::wstring& device) {
    for (size_t i = 0; i < mons.size(); ++i)
        if (mons[i].device == device) return (int)i;
    return -1;
}
