// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dolu <https://github.com/DoluTattoo>

#include "Settings.h"

#include <algorithm>

namespace
{
constexpr wchar_t kKey[] = L"Software\\dock";

DWORD ReadDword(const wchar_t* name, DWORD fallback)
{
    DWORD value = 0;
    DWORD cb    = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_DWORD, nullptr, &value, &cb) ==
        ERROR_SUCCESS)
        return value;
    return fallback;
}

std::wstring ReadString(const wchar_t* name)
{
    wchar_t buf[1024] = {};
    DWORD   cb        = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_SZ, nullptr, buf, &cb) ==
        ERROR_SUCCESS)
        return buf;
    return L"";
}

void WriteDword(HKEY key, const wchar_t* name, DWORD value)
{
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

void WriteString(HKEY key, const wchar_t* name, const std::wstring& value)
{
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

// Returns true for the virtual-key codes that act as modifiers (and so cannot
// be the main key of a shortcut).
bool IsModifierVk(unsigned vk)
{
    switch (vk)
    {
    case VK_LWIN: case VK_RWIN:
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU: case VK_LMENU: case VK_RMENU:
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
        return true;
    default:
        return false;
    }
}

std::wstring KeyName(unsigned vk)
{
    // Prefer stable English names for common keys (GetKeyNameText is localized
    // and upper-cased, which would clash with the otherwise-English UI).
    switch (vk)
    {
    case VK_SPACE:  return L"Space";
    case VK_RETURN: return L"Enter";
    case VK_TAB:    return L"Tab";
    case VK_ESCAPE: return L"Esc";
    case VK_BACK:   return L"Backspace";
    case VK_DELETE: return L"Delete";
    case VK_INSERT: return L"Insert";
    case VK_HOME:   return L"Home";
    case VK_END:    return L"End";
    case VK_PRIOR:  return L"Page Up";
    case VK_NEXT:   return L"Page Down";
    case VK_LEFT:   return L"Left";
    case VK_RIGHT:  return L"Right";
    case VK_UP:     return L"Up";
    case VK_DOWN:   return L"Down";
    default:        break;
    }

    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);

    bool extended = false;
    switch (vk)
    {
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
    case VK_PRIOR: case VK_NEXT: case VK_HOME: case VK_END:
    case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
        extended = true;
        break;
    default:
        break;
    }

    LONG lParam = static_cast<LONG>(sc << 16);
    if (extended)
        lParam |= (1 << 24);

    wchar_t name[64] = {};
    if (sc != 0 && GetKeyNameTextW(lParam, name, ARRAYSIZE(name)) > 0)
        return name;

    wchar_t fallback[16] = {};
    swprintf_s(fallback, L"0x%02X", vk);
    return fallback;
}
} // namespace

Settings LoadSettings()
{
    Settings s;
    s.folder              = ReadString(L"Folder");
    s.hotkey.modifiers    = ReadDword(L"HotkeyMods", dk::MOD_WINKEY);
    s.hotkey.vk           = ReadDword(L"HotkeyVk", VK_SPACE);
    s.toggleMode          = ReadDword(L"ToggleMode", 0) != 0;
    s.showFolderName      = ReadDword(L"ShowFolderName", 0) != 0;
    s.showItemCount       = ReadDword(L"ShowItemCount", 0) != 0;
    s.backdrop            = static_cast<BackdropStyle>(ReadDword(L"Backdrop",
                                static_cast<DWORD>(BackdropStyle::Acrylic)));
    s.opacity             = static_cast<int>(ReadDword(L"Opacity", 45));
    s.roundedCorners      = ReadDword(L"RoundedCorners", 1) != 0;
    s.animations          = ReadDword(L"Animations", 1) != 0;
    s.gridColumns         = static_cast<int>(ReadDword(L"GridColumns", 0));
    s.gridRows            = static_cast<int>(ReadDword(L"GridRows", 0));
    s.padding             = static_cast<int>(ReadDword(L"Padding", 20));
    s.centerCursor        = ReadDword(L"CenterCursor", 1) != 0;
    s.offsetX             = static_cast<int>(ReadDword(L"OffsetX", 0));
    s.offsetY             = static_cast<int>(ReadDword(L"OffsetY", 0));
    s.checkForUpdates     = ReadDword(L"CheckForUpdates", 1) != 0;

    if (s.hotkey.vk == 0 || IsModifierVk(s.hotkey.vk))
        s.hotkey.vk = VK_SPACE;
    s.opacity     = std::clamp(s.opacity, dk::kOpacityMin, dk::kOpacityMax);
    s.gridColumns = std::clamp(s.gridColumns, dk::kGridMin, dk::kGridMax);
    s.gridRows    = std::clamp(s.gridRows, dk::kGridMin, dk::kGridMax);
    s.padding     = std::clamp(s.padding, dk::kPaddingMin, dk::kPaddingMax);
    s.offsetX     = std::clamp(s.offsetX, -dk::kOffsetLimit, dk::kOffsetLimit);
    s.offsetY     = std::clamp(s.offsetY, -dk::kOffsetLimit, dk::kOffsetLimit);
    return s;
}

void SaveSettings(const Settings& s)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS)
        return;

    WriteString(key, L"Folder", s.folder);
    WriteDword(key, L"HotkeyMods", s.hotkey.modifiers);
    WriteDword(key, L"HotkeyVk", s.hotkey.vk);
    WriteDword(key, L"ToggleMode", s.toggleMode ? 1 : 0);
    WriteDword(key, L"ShowFolderName", s.showFolderName ? 1 : 0);
    WriteDword(key, L"ShowItemCount", s.showItemCount ? 1 : 0);
    WriteDword(key, L"Backdrop", static_cast<DWORD>(s.backdrop));
    WriteDword(key, L"Opacity", static_cast<DWORD>(s.opacity));
    WriteDword(key, L"RoundedCorners", s.roundedCorners ? 1 : 0);
    WriteDword(key, L"Animations", s.animations ? 1 : 0);
    WriteDword(key, L"GridColumns", static_cast<DWORD>(s.gridColumns));
    WriteDword(key, L"GridRows", static_cast<DWORD>(s.gridRows));
    WriteDword(key, L"Padding", static_cast<DWORD>(s.padding));
    WriteDword(key, L"CenterCursor", s.centerCursor ? 1 : 0);
    WriteDword(key, L"OffsetX", static_cast<DWORD>(s.offsetX));
    WriteDword(key, L"OffsetY", static_cast<DWORD>(s.offsetY));
    WriteDword(key, L"CheckForUpdates", s.checkForUpdates ? 1 : 0);
    RegCloseKey(key);
}

bool SettingsExist()
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_READ, &key) == ERROR_SUCCESS)
    {
        RegCloseKey(key);
        return true;
    }
    return false;
}

namespace
{
constexpr wchar_t kRunKey[]   = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"dock";
} // namespace

bool IsAutostartEnabled()
{
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, nullptr,
                        nullptr) == ERROR_SUCCESS;
}

void SetAutostartEnabled(bool enabled)
{
    if (enabled)
    {
        wchar_t path[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, path, ARRAYSIZE(path)) == 0)
            return;
        const std::wstring cmd = L"\"" + std::wstring(path) + L"\"";
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                            nullptr) != ERROR_SUCCESS)
            return;
        RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                       static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
    else
    {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue);
    }
}

std::wstring HotkeyToString(unsigned modifiers, unsigned vk)
{
    std::wstring out;
    auto add = [&out](const wchar_t* part) {
        if (!out.empty())
            out += L" + ";
        out += part;
    };

    if (modifiers & dk::MOD_WINKEY)
        add(L"Win");
    if (modifiers & dk::MOD_CTRLKEY)
        add(L"Ctrl");
    if (modifiers & dk::MOD_ALTKEY)
        add(L"Alt");
    if (modifiers & dk::MOD_SHIFTKEY)
        add(L"Shift");

    add(KeyName(vk).c_str());
    return out;
}
