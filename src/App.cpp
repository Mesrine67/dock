// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dolu <https://github.com/DoluTattoo>

#include "App.h"
#include "FolderModel.h"
#include "FolderWatcher.h"
#include "KeyboardHook.h"
#include "OverlayWindow.h"
#include "SettingsWindow.h"
#include "UpdateChecker.h"
#include "resource.h"

#include <algorithm>
#include <memory>
#include <shlobj.h>

namespace
{
constexpr int kIconPixelSize = 128;

// Opens the modern "pick a folder" dialog; returns false if the user cancels.
bool PickFolder(HWND owner, const std::wstring& initial, std::wstring& out)
{
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dlg))))
        return false;

    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"Choose a folder to display");

    if (!initial.empty())
    {
        ComPtr<IShellItem> item;
        if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&item))))
            dlg->SetFolder(item.Get());
    }

    if (FAILED(dlg->Show(owner)))
        return false;

    ComPtr<IShellItem> result;
    if (FAILED(dlg->GetResult(&result)))
        return false;

    PWSTR path = nullptr;
    if (FAILED(result->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        return false;
    out = path;
    CoTaskMemFree(path);
    return true;
}
} // namespace

App::App()  = default;
App::~App() = default;

void App::ApplyAll()
{
    if (overlay)
    {
        overlay->SetHeader(settings.showFolderName, settings.showItemCount);
        overlay->SetBackdrop(settings.backdrop);
        overlay->SetOpacity(settings.opacity / 100.0f);
        overlay->SetRounded(settings.roundedCorners);
        overlay->SetGrid(settings.gridColumns, settings.gridRows);
        overlay->SetPadding(settings.padding);
        overlay->SetCenterCursor(settings.centerCursor);
        overlay->SetOffset(settings.offsetX, settings.offsetY);
        overlay->SetAnimations(settings.animations);
    }
    ApplyHotkey();
    UpdateTrayTip();
}

void App::ApplyHotkey()
{
    if (hook)
        hook->Configure(settings.hotkey.modifiers, settings.hotkey.vk, settings.toggleMode);
}

void App::SetFolder(const std::wstring& path)
{
    if (path.empty() || !model)
        return;
    settings.folder = path;
    model->SetFolder(path);
    model->Refresh();
    if (overlay)
        overlay->InvalidateIcons();
    model->StartIconLoad(ctrl, dk::WM_APP_ICONS_READY, kIconPixelSize);
    StartWatching(); // watch the new folder for live changes
    UpdateTrayTip();
    SaveSettings(settings);
    if (previewActive_)
        LayoutPreview(); // the item count may change the panel size
}

void App::ChooseFolderInteractive(HWND owner)
{
    std::wstring picked;
    if (model && PickFolder(owner, model->FolderPath(), picked))
        SetFolder(picked);
}

void App::SetHotkey(unsigned modifiers, unsigned vk)
{
    settings.hotkey.modifiers = modifiers;
    settings.hotkey.vk        = vk;
    ApplyHotkey();
    SaveSettings(settings);
}

void App::SetToggleMode(bool toggle)
{
    settings.toggleMode = toggle;
    ApplyHotkey();
    SaveSettings(settings);
}

void App::SetShowFolderName(bool show)
{
    settings.showFolderName = show;
    if (overlay)
        overlay->SetHeader(settings.showFolderName, settings.showItemCount);
    SaveSettings(settings);
    if (previewActive_)
        LayoutPreview(); // the header row changes the panel size
}

void App::SetShowItemCount(bool show)
{
    settings.showItemCount = show;
    if (overlay)
        overlay->SetHeader(settings.showFolderName, settings.showItemCount);
    SaveSettings(settings);
    if (previewActive_)
        LayoutPreview();
}

void App::SetBackdrop(BackdropStyle backdrop)
{
    settings.backdrop = backdrop;
    if (overlay)
        overlay->SetBackdrop(backdrop);
    SaveSettings(settings);
}

void App::SetOpacity(int percent, bool persist)
{
    settings.opacity = percent;
    if (overlay)
        overlay->SetOpacity(percent / 100.0f);
    if (persist)
        SaveSettings(settings);
}

void App::SetRoundedCorners(bool rounded)
{
    settings.roundedCorners = rounded;
    if (overlay)
        overlay->SetRounded(rounded);
    SaveSettings(settings);
}

void App::SetColumns(int columns)
{
    settings.gridColumns = columns;
    if (overlay)
        overlay->SetGrid(settings.gridColumns, settings.gridRows);
    SaveSettings(settings);
    if (previewActive_)
        LayoutPreview(); // the grid size changes the panel size
}

void App::SetRows(int rows)
{
    settings.gridRows = rows;
    if (overlay)
        overlay->SetGrid(settings.gridColumns, settings.gridRows);
    SaveSettings(settings);
    if (previewActive_)
        LayoutPreview();
}

void App::SetPadding(int padding, bool persist)
{
    settings.padding = padding;
    if (overlay)
        overlay->SetPadding(padding);
    if (persist)
        SaveSettings(settings);
    if (previewActive_)
        PlaceOverlayPreview(); // padding changes the panel size; resize live
}

void App::SetCenterCursor(bool center)
{
    settings.centerCursor = center;
    if (overlay)
        overlay->SetCenterCursor(center);
    SaveSettings(settings);
}

void App::SetOffset(int x, int y, bool persist)
{
    settings.offsetX = x;
    settings.offsetY = y;
    if (overlay)
        overlay->SetOffset(x, y);
    if (persist)
        SaveSettings(settings);
    if (previewActive_)
        PlaceOverlayPreview(); // move only the overlay (don't yank the settings window)
}

void App::SetAnimations(bool enabled)
{
    settings.animations = enabled;
    if (overlay)
        overlay->SetAnimations(enabled);
    SaveSettings(settings);
}

void App::SetAutostart(bool enabled)
{
    SetAutostartEnabled(enabled); // writes the per-user Run key (Settings.cpp)
}

void App::SetCheckForUpdates(bool enabled)
{
    settings.checkForUpdates = enabled;
    SaveSettings(settings);
}

void App::CheckForUpdates(bool userInitiated)
{
    if (updateCheckInProgress_)
        return;
    updateCheckInProgress_ = true;
    dk::CheckForUpdateAsync(ctrl, dk::WM_APP_UPDATE_READY, userInitiated);
}

void App::OnUpdateChecked(UpdateResult* result)
{
    std::unique_ptr<UpdateResult> res(result);
    updateCheckInProgress_ = false;
    if (!res)
        return;

    // Reflect the outcome in the settings window if it happens to be open.
    if (settingsWnd)
    {
        std::wstring status;
        if (res->failed)
            status = L"Couldn't check for updates.";
        else if (res->available)
            status = L"Update available: " + res->latest;
        else
            status = L"You're up to date (" + res->current + L").";
        settingsWnd->SetUpdateStatus(status);
    }

    if (res->available)
    {
        const HWND   owner = SettingsHwnd();
        std::wstring text  = L"dock " + res->latest + L" is available.\n\nYou have version " +
                            res->current + L".\n\nOpen the official GitHub Releases page to review and install it?";
        const int choice = MessageBoxW(owner, text.c_str(), L"dock \u2014 Update available",
                                       MB_YESNO | MB_ICONINFORMATION | MB_SETFOREGROUND |
                                           (owner ? 0u : MB_TOPMOST));
        if (choice == IDYES)
            ShellExecuteW(nullptr, L"open", res->url.c_str(), nullptr, nullptr,
                          SW_SHOWNORMAL); // user reviews the release and installer manually
    }
    else if (res->userInitiated && res->failed)
    {
        MessageBoxW(SettingsHwnd(), L"Couldn't check for updates. Please try again later.",
                    L"dock", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
    }
}

void App::StartWatching()
{
    if (!watcher)
        watcher = std::make_unique<FolderWatcher>();
    watcher->Start(model ? model->FolderPath() : std::wstring(), ctrl,
                   dk::WM_APP_FOLDER_CHANGED);
}

void App::RefreshFolder()
{
    if (!model)
        return;
    model->Refresh();
    if (overlay)
        overlay->InvalidateIcons();
    model->StartIconLoad(ctrl, dk::WM_APP_ICONS_READY, kIconPixelSize);
    UpdateTrayTip();
    if (previewActive_)
        LayoutPreview();              // settings preview open: re-place + re-size
    else if (overlay)
        overlay->RelayoutIfVisible(); // live update if the overlay is open
}

void App::OnIconsReady(IconBatch* batch)
{
    std::unique_ptr<IconBatch> owned(batch);
    if (!owned || !model)
        return;
    if (model->ApplyIcons(wic, *owned) && overlay)
    {
        overlay->InvalidateIcons();
        overlay->RerenderIfVisible();
    }
}

void App::OpenCurrentFolder()
{
    if (model)
        ShellExecuteW(nullptr, L"open", model->FolderPath().c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);
}

void App::ShowSettings()
{
    if (!settingsWnd)
        settingsWnd = std::make_unique<SettingsWindow>(*this);
    previewActive_ = true;
    settingsWnd->Prepare();  // create + populate while still hidden
    LayoutPreview();         // position both windows, show the overlay preview
    settingsWnd->Reveal();   // now show the settings window in place
}

void App::OnSettingsClosed()
{
    if (!previewActive_)
        return;
    previewActive_ = false;
    if (overlay)
        overlay->Hide();
}

// Resolves where the overlay opens: centred on the work area, nudged by the
// user offset, clamped so the whole panel stays on screen.
static POINT ResolveOverlayPos(const MONITORINFO& mi, SIZE ov, int offX, int offY)
{
    const int workW = mi.rcWork.right - mi.rcWork.left;
    const int workH = mi.rcWork.bottom - mi.rcWork.top;
    const int ow    = static_cast<int>(ov.cx);
    const int oh    = static_cast<int>(ov.cy);
    int       x     = static_cast<int>(mi.rcWork.left) + (workW - ow) / 2 + offX;
    int       y     = static_cast<int>(mi.rcWork.top) + (workH - oh) / 2 + offY;
    x = std::clamp(x, static_cast<int>(mi.rcWork.left), static_cast<int>(mi.rcWork.right) - ow);
    y = std::clamp(y, static_cast<int>(mi.rcWork.top), static_cast<int>(mi.rcWork.bottom) - oh);
    return POINT{ x, y };
}

// Moves only the overlay to its resolved open position (used for live offset
// dragging so the settings window is never yanked out from under the cursor).
void App::PlaceOverlayPreview()
{
    if (!previewActive_ || !overlay || !settingsWnd)
        return;
    HWND sw = settingsWnd->Hwnd();
    if (!sw)
        return;
    HMONITOR    mon = MonitorFromWindow(sw, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    const SIZE  ov = overlay->MeasureForMonitor(mon);
    const POINT p  = ResolveOverlayPos(mi, ov, settings.offsetX, settings.offsetY);
    overlay->ShowPreviewAt(p.x, p.y);
}

// After a discrete offset change (slider released, arrow key), nudge the
// settings window aside only if it would actually overlap the overlay, so the
// two never cover each other but the window doesn't jump around needlessly.
void App::ReflowPreview()
{
    if (!previewActive_ || !overlay || !settingsWnd)
        return;
    HWND sw = settingsWnd->Hwnd();
    if (!sw)
        return;

    HMONITOR    mon = MonitorFromWindow(sw, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    const SIZE  ov = overlay->MeasureForMonitor(mon);
    const POINT p  = ResolveOverlayPos(mi, ov, settings.offsetX, settings.offsetY);

    RECT ovR{ p.x, p.y, p.x + ov.cx, p.y + ov.cy };
    RECT sr{};
    GetWindowRect(sw, &sr);
    RECT inter{};
    if (IntersectRect(&inter, &ovR, &sr))
        LayoutPreview();        // overlapping -> re-place the settings window aside
    else
        overlay->ShowPreviewAt(p.x, p.y);
}

// Lays the overlay and the settings window out side by side without overlapping:
// the overlay sits at its resolved open position and the settings window is
// placed on whichever side has more room, vertically centred.
void App::LayoutPreview()
{
    if (!previewActive_ || !overlay || !settingsWnd)
        return;
    HWND sw = settingsWnd->Hwnd();
    if (!sw)
        return;

    HMONITOR    mon = MonitorFromWindow(sw, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    const int workH = mi.rcWork.bottom - mi.rcWork.top;

    const SIZE  ov = overlay->MeasureForMonitor(mon);
    const POINT p  = ResolveOverlayPos(mi, ov, settings.offsetX, settings.offsetY);

    RECT sr{};
    GetWindowRect(sw, &sr);
    const int sW  = sr.right - sr.left;
    const int sH  = sr.bottom - sr.top;
    const int gap = MulDiv(24, static_cast<int>(GetDpiForWindow(sw)), 96);

    const int ow        = static_cast<int>(ov.cx);
    const int px        = static_cast<int>(p.x);
    const int leftRoom  = px - static_cast<int>(mi.rcWork.left);
    const int rightRoom = static_cast<int>(mi.rcWork.right) - (px + ow);

    int sX;
    if (rightRoom >= leftRoom)
        sX = std::min(px + ow + gap, static_cast<int>(mi.rcWork.right) - sW);
    else
        sX = std::max(px - gap - sW, static_cast<int>(mi.rcWork.left));
    const int sY = std::clamp(static_cast<int>(mi.rcWork.top) + (workH - sH) / 2,
                              static_cast<int>(mi.rcWork.top),
                              static_cast<int>(mi.rcWork.bottom) - sH);

    MoveWindow(sw, sX, sY, sW, sH, TRUE);
    overlay->ShowPreviewAt(p.x, p.y);
}

void App::DestroySettings()
{
    settingsWnd.reset();
}

HWND App::SettingsHwnd() const
{
    return settingsWnd ? settingsWnd->Hwnd() : nullptr;
}

void App::AddTray()
{
    nid_.cbSize           = sizeof(nid_);
    nid_.hWnd             = ctrl;
    nid_.uID              = 1;
    nid_.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid_.uCallbackMessage = dk::WM_TRAY;
    nid_.hIcon            = static_cast<HICON>(LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON),
                                                          IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                          GetSystemMetrics(SM_CYSMICON),
                                                          LR_DEFAULTCOLOR));
    if (!nid_.hIcon)
        nid_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(nid_.szTip, L"dock");
    Shell_NotifyIconW(NIM_ADD, &nid_);
    UpdateTrayTip();
}

void App::RemoveTray()
{
    Shell_NotifyIconW(NIM_DELETE, &nid_);
}

void App::UpdateTrayTip()
{
    if (!model)
        return;
    const std::wstring shortcut = HotkeyToString(settings.hotkey.modifiers, settings.hotkey.vk);
    const wchar_t*     verb     = settings.toggleMode ? L"press" : L"hold";
    std::wstring       tip      = L"dock \u2014 " + std::wstring(verb) + L" " + shortcut;
    nid_.uFlags = NIF_TIP;
    wcsncpy_s(nid_.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid_);
}
