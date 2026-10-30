// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dolu <https://github.com/DoluTattoo>

#include "App.h"
#include "Common.h"
#include "FolderModel.h"
#include "KeyboardHook.h"
#include "OverlayWindow.h"
#include "Settings.h"
#include "UpdateChecker.h"

#include <commctrl.h>
#include <shlobj.h>

#define IDM_SETTINGS 1001
#define IDM_EXIT     1002

namespace
{
// Coalesces a burst of file-system change notifications into a single refresh.
constexpr UINT_PTR kFolderRefreshTimer = 1;

LRESULT CALLBACK CtrlProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg)
    {
    case dk::WM_APP_SHOW:
        if (app && app->overlay && !app->PreviewActive())
            app->overlay->Show();
        return 0;

    case dk::WM_APP_HIDE:
        if (app && app->overlay && !app->PreviewActive())
            app->overlay->Hide();
        return 0;

    case dk::WM_APP_TOGGLE:
        if (app && app->overlay && !app->PreviewActive())
        {
            if (app->overlay->IsVisible())
                app->overlay->Hide();
            else
                app->overlay->Show();
        }
        return 0;

    case dk::WM_APP_SHOWSETTINGS:
        if (app)
            app->ShowSettings();
        return 0;

    case dk::WM_APP_ICONS_READY:
        if (app)
            app->OnIconsReady(reinterpret_cast<IconBatch*>(lParam));
        else
            delete reinterpret_cast<IconBatch*>(lParam);
        return 0;

    case dk::WM_APP_UPDATE_READY:
        if (app)
            app->OnUpdateChecked(reinterpret_cast<UpdateResult*>(lParam));
        else
            delete reinterpret_cast<UpdateResult*>(lParam);
        return 0;

    case dk::WM_APP_UPDATE_DOWNLOADED:
        if (app)
            app->OnUpdateDownloaded(reinterpret_cast<DownloadResult*>(lParam));
        else
            delete reinterpret_cast<DownloadResult*>(lParam);
        return 0;

    case dk::WM_APP_FOLDER_CHANGED:
        // Debounce: wait for the change burst to settle, then refresh once.
        SetTimer(hwnd, kFolderRefreshTimer, 200, nullptr);
        return 0;

    case WM_TIMER:
        if (wParam == kFolderRefreshTimer)
        {
            KillTimer(hwnd, kFolderRefreshTimer);
            if (app)
                app->RefreshFolder();
        }
        return 0;

    case dk::WM_TRAY:
        if (LOWORD(lParam) == WM_LBUTTONDBLCLK)
        {
            if (app)
                app->ShowSettings();
            return 0;
        }
        if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU)
        {
            POINT pt{};
            GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"Settings");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Close");
            SetForegroundWindow(hwnd);
            const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0,
                                           hwnd, nullptr);
            DestroyMenu(menu);
            PostMessageW(hwnd, WM_NULL, 0, 0);
            if (cmd == IDM_SETTINGS && app)
                app->ShowSettings();
            else if (cmd == IDM_EXIT && app)
            {
                app->RemoveTray();
                PostQuitMessage(0);
            }
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Resolves the folder to display: the saved one, or the Desktop as a fallback.
std::wstring ResolveStartupFolder(const std::wstring& saved)
{
    if (!saved.empty() && GetFileAttributesW(saved.c_str()) != INVALID_FILE_ATTRIBUTES)
        return saved;

    std::wstring folder;
    PWSTR        desktop = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop)))
    {
        folder = desktop;
        CoTaskMemFree(desktop);
    }
    return folder;
}
} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR lpCmdLine, int)
{
    // A relaunch right after a self-update is started with "--updated". If an
    // instance somehow already runs (the installer and our relauncher can both
    // fire), such a launch just exits quietly instead of popping up settings.
    const bool afterUpdate = lpCmdLine && wcsstr(lpCmdLine, L"--updated") != nullptr;

    // Single-instance guard: a normal second launch asks the running one to open
    // its settings, then exits.
    HANDLE singleton = CreateMutexW(nullptr, TRUE, L"Local\\dock_singleton_4F1C");
    if (singleton && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (!afterUpdate)
            if (HWND existing = FindWindowW(L"DockCtrl", nullptr))
                PostMessageW(existing, dk::WM_APP_SHOWSETTINGS, 0, 0);
        CloseHandle(singleton);
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
        return 1;

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic))))
    {
        CoUninitialize();
        return 1;
    }

    Settings   settings    = LoadSettings();
    const bool firstLaunch = !SettingsExist();
    if (firstLaunch)
        SaveSettings(settings); // create the store now so we greet only once
    settings.folder = ResolveStartupFolder(settings.folder);

    FolderModel model;
    model.SetFolder(settings.folder);
    model.Refresh();

    OverlayWindow overlay;
    if (!overlay.Initialize(hInst, &model, wic.Get()))
    {
        CoUninitialize();
        return 1;
    }

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc   = CtrlProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"DockCtrl";
    RegisterClassExW(&wc);

    KeyboardHook hook;

    App app;
    app.hInst    = hInst;
    app.wic      = wic.Get();
    app.model    = &model;
    app.overlay  = &overlay;
    app.hook     = &hook;
    app.settings = settings;

    HWND ctrl = CreateWindowExW(WS_EX_TOOLWINDOW, L"DockCtrl", L"dock", WS_POPUP, 0, 0, 0,
                                0, nullptr, nullptr, hInst, nullptr);
    app.ctrl = ctrl;
    SetWindowLongPtrW(ctrl, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&app));

    app.AddTray();

    hook.SetCallbacks([ctrl] { PostMessageW(ctrl, dk::WM_APP_SHOW, 0, 0); },
                      [ctrl] { PostMessageW(ctrl, dk::WM_APP_HIDE, 0, 0); },
                      [ctrl] { PostMessageW(ctrl, dk::WM_APP_TOGGLE, 0, 0); });
    hook.Install();
    app.ApplyAll(); // pushes settings to overlay + hook + tray

    // Kick off the (non-blocking) icon load now that the control window exists.
    model.StartIconLoad(ctrl, dk::WM_APP_ICONS_READY, 128);

    // Watch the folder so the overlay reflects files added/removed/renamed.
    app.StartWatching();

    // On the very first launch, open settings so the user can pick a folder;
    // the Desktop is shown as a live preview behind it in the meantime.
    if (firstLaunch)
        PostMessageW(ctrl, dk::WM_APP_SHOWSETTINGS, 0, 0);
    else if (app.settings.checkForUpdates)
        app.CheckForUpdates(false); // silent unless a newer release is available

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        // Route dialog-style keyboard navigation (Tab, arrows, Esc, default
        // button, mnemonics) to the settings window while it is open.
        const HWND settingsHwnd = app.SettingsHwnd();
        if (settingsHwnd && IsDialogMessageW(settingsHwnd, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    app.DestroySettings();
    hook.Uninstall();
    app.RemoveTray();
    overlay.Shutdown();
    CoUninitialize();
    if (singleton)
        CloseHandle(singleton);
    return 0;
}
