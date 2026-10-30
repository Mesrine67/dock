// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dolu <https://github.com/DoluTattoo>

#pragma once

#include "Common.h"
#include "Settings.h"
#include <shellapi.h>

#include <memory>

class OverlayWindow;
class KeyboardHook;
class FolderModel;
class SettingsWindow;
class FolderWatcher;
struct IWICImagingFactory;
struct IconBatch;
struct UpdateResult;
struct DownloadResult;

namespace dk
{
// Messages posted to the hidden control window (handled in main.cpp CtrlProc).
constexpr UINT WM_TRAY             = WM_APP + 1;
constexpr UINT WM_APP_SHOW         = WM_APP + 2;
constexpr UINT WM_APP_HIDE         = WM_APP + 3;
constexpr UINT WM_APP_TOGGLE       = WM_APP + 4;
constexpr UINT WM_APP_SHOWSETTINGS = WM_APP + 5;
constexpr UINT WM_APP_ICONS_READY  = WM_APP + 6;
constexpr UINT WM_APP_FOLDER_CHANGED = WM_APP + 7;
constexpr UINT WM_APP_UPDATE_READY = WM_APP + 8;
constexpr UINT WM_APP_UPDATE_DOWNLOADED = WM_APP + 9;
} // namespace dk

// Shared application context: owns the live settings and wires every settings
// change through to the overlay, the keyboard hook, the tray, and persistence.
struct App
{
    HINSTANCE           hInst   = nullptr;
    HWND                ctrl    = nullptr; // hidden message window
    IWICImagingFactory* wic     = nullptr;
    FolderModel*        model   = nullptr;
    OverlayWindow*      overlay = nullptr;
    KeyboardHook*       hook    = nullptr;
    std::unique_ptr<SettingsWindow> settingsWnd;
    std::unique_ptr<FolderWatcher>  watcher;

    Settings settings;

    App();
    ~App();

    // Pushes the whole settings state to the overlay, hook and tray.
    void ApplyAll();

    // Individual mutations: update settings, apply live, and persist.
    void SetFolder(const std::wstring& path);
    void ChooseFolderInteractive(HWND owner);
    void SetHotkey(unsigned modifiers, unsigned vk);
    void SetToggleMode(bool toggle);
    void SetShowFolderName(bool show);
    void SetShowItemCount(bool show);
    void SetBackdrop(BackdropStyle backdrop);
    void SetOpacity(int percent, bool persist = true);
    void SetRoundedCorners(bool rounded);
    void SetColumns(int columns);
    void SetRows(int rows);
    void SetPadding(int padding, bool persist = true);
    void SetCenterCursor(bool center);
    void SetOffset(int x, int y, bool persist = true);
    void SetAnimations(bool enabled);
    void SetAutostart(bool enabled);
    void SetCheckForUpdates(bool enabled);

    // Update check: kicks off an async GitHub query; userInitiated=true reports
    // "you're up to date" / failures, otherwise the check is silent unless a
    // newer version is found. OnUpdateChecked handles the posted result.
    void CheckForUpdates(bool userInitiated);
    void OnUpdateChecked(UpdateResult* result);

    // Self-update: downloads the installer in the background, then launches it
    // (silent, elevated) and quits so it can replace dock and relaunch.
    void BeginUpdateDownload(const std::wstring& url);
    void OnUpdateDownloaded(DownloadResult* result);

    // Re-targets the folder watcher at the current folder.
    void StartWatching();
    // Re-enumerates the folder and reloads icons (folder contents changed).
    void RefreshFolder();

    void OpenCurrentFolder();
    void OnIconsReady(IconBatch* batch);
    void ShowSettings();
    void DestroySettings();
    void OnSettingsClosed();
    void LayoutPreview();
    void PlaceOverlayPreview();
    void ReflowPreview();
    bool PreviewActive() const { return previewActive_; }
    HWND SettingsHwnd() const;

    // Tray icon.
    void AddTray();
    void RemoveTray();
    void UpdateTrayTip();

private:
    void ApplyHotkey();
    NOTIFYICONDATAW nid_{};
    bool            previewActive_ = false;
    bool            updateCheckInProgress_ = false;
    bool            updateDownloadInProgress_ = false;
};
