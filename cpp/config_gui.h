// ===========================================================================
// config_gui.h - The visual setup dialog.
//
// RunConfigDialog() shows a window where the user:
//   1. picks the main working monitor and drags/sizes a pixel-exact working box,
//   2. picks the mirror monitor and places a box that stays locked to the
//      working area's aspect ratio,
//   3. sets the mirror frame rate (and vsync / cursor),
//   and then Saves, Imports, Exports, or Cancels & Quits.
//
// It is fully self-contained (no owner window needed) so it can run at first
// launch before anything else exists, and again from the tray "Edit..." menu.
// ===========================================================================
#pragma once
#include <windows.h>
#include <vector>
#include "config.h"
#include "monitors.h"

enum class ConfigResult {
    Save,   // user saved; `cfg` holds the new configuration (already written to defaultPath)
    Quit    // user chose Cancel & Quit
};

// `banner` is an optional message shown at the top (e.g. a fallback error, or
// "No configuration found - set up your working area, or Import one.").
ConfigResult RunConfigDialog(MirrorConfig& cfg,
                             const std::vector<MonitorInfo>& mons,
                             HINSTANCE hInst,
                             const std::wstring& banner,
                             const std::wstring& defaultPath);
