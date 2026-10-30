// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dolu <https://github.com/DoluTattoo>

#pragma once

#include "Common.h"

// Our own modifier bitmask (distinct from Win32 MOD_* because we also need the
// WIN key, which RegisterHotKey-style flags handle differently).
namespace dk
{
constexpr unsigned MOD_WINKEY   = 0x0001;
constexpr unsigned MOD_CTRLKEY  = 0x0002;
constexpr unsigned MOD_ALTKEY   = 0x0004;
constexpr unsigned MOD_SHIFTKEY = 0x0008;

// Shared bounds for the configurable values, used by both the loader (clamping
// persisted values) and the settings UI (slider / combo ranges) so the two can
// never drift apart.
constexpr int kOffsetLimit = 1000; // max |open-position offset|, px (matches the slider)
constexpr int kOpacityMin  = 10;
constexpr int kOpacityMax  = 100;
constexpr int kGridMin     = 0;    // 0 = auto
constexpr int kGridMax     = 10;
constexpr int kPaddingMin  = 0;    // inner panel padding, px
constexpr int kPaddingMax  = 60;
} // namespace dk

// Backdrop applied to the overlay panel.
enum class BackdropStyle
{
    Solid   = 0,
    Acrylic = 1,
};

// The activation shortcut: a set of modifiers plus a main key.
struct Hotkey
{
    unsigned modifiers = dk::MOD_WINKEY;
    unsigned vk        = VK_SPACE;
};

// All user-configurable options, persisted under HKCU\Software\dock.
struct Settings
{
    std::wstring  folder;                               // empty => Desktop fallback
    Hotkey        hotkey;
    bool          toggleMode     = false;               // false = hold, true = toggle
    bool          showFolderName = false;               // header: folder name
    bool          showItemCount  = false;               // header: item count
    BackdropStyle backdrop       = BackdropStyle::Acrylic;
    int           opacity        = 45;                  // panel opacity, percent
    bool          roundedCorners = true;               // rounded panel corners
    bool          animations     = true;                // fade in/out
    int           gridColumns    = 0;                  // 0 = auto
    int           gridRows       = 0;                  // 0 = auto
    int           padding        = 20;                 // inner panel padding, px
    bool          centerCursor   = true;               // place cursor on panel at open
    int           offsetX        = 0;                  // open-position nudge, px (+right)
    int           offsetY        = 0;                  // open-position nudge, px (+down)
    bool          checkForUpdates = true;              // check GitHub for updates at launch
};

Settings     LoadSettings();
void         SaveSettings(const Settings& s);

// True once settings have been persisted (the registry key exists); used to
// detect the very first launch.
bool         SettingsExist();

// "Start with Windows": reads / writes the per-user Run key directly (this is
// OS state, kept out of the Settings blob).
bool         IsAutostartEnabled();
void         SetAutostartEnabled(bool enabled);

// Human-readable shortcut, e.g. "Win + Space" or "Ctrl + Alt + D".
std::wstring HotkeyToString(unsigned modifiers, unsigned vk);
