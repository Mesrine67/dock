// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dolu <https://github.com/DoluTattoo>

#include "SettingsWindow.h"
#include "App.h"
#include "FolderModel.h"
#include "KeyboardHook.h"
#include "Settings.h"
#include "UpdateChecker.h"
#include "resource.h"

#include <dwmapi.h>
#include <uxtheme.h>
#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

namespace
{
const wchar_t kClassName[] = L"DockSettings";

// A flat, uniform dark palette: every control shares the window background so
// there are no visible panels, and because the shared brush is opaque, dynamic
// labels erase their previous text cleanly when updated.
constexpr COLORREF kBackColor   = RGB(32, 32, 32);  // window + control background
constexpr COLORREF kFieldColor  = RGB(45, 45, 48);  // recessed input (hotkey field)
constexpr COLORREF kTextColor   = RGB(235, 235, 235);
constexpr COLORREF kAccentColor = RGB(120, 170, 255); // section headers

// Control ids.
enum : int
{
    IDC_SEC_SHORTCUT = 2001,
    IDC_SEC_FOLDER,
    IDC_SEC_LAYOUT,
    IDC_SEC_POSITION,
    IDC_SEC_HEADER,
    IDC_SEC_APPEARANCE,
    IDC_SEC_STARTUP,
    IDC_SEC_UPDATES,
    IDC_HOTKEY_FIELD = 2010,
    IDC_FOLDER_PATH,
    IDC_BACKDROP_LABEL,
    IDC_OPACITY_LABEL,
    IDC_OPACITY_VAL,
    IDC_COLS_LABEL,
    IDC_ROWS_LABEL,
    IDC_OFFX_LABEL,
    IDC_OFFY_LABEL,
    IDC_OFFX_VAL,
    IDC_OFFY_VAL,
    IDC_PADDING_LABEL,
    IDC_PADDING_VAL,
    IDC_UPDATE_STATUS,
    IDC_TITLE,
    IDC_HOTKEY_BTN = 2100,
    IDC_TOGGLE,
    IDC_FOLDER_BTN,
    IDC_SHOW_NAME,
    IDC_SHOW_COUNT,
    IDC_BACKDROP,
    IDC_OPACITY,
    IDC_COLUMNS,
    IDC_ROWS,
    IDC_ROUNDED,
    IDC_CENTER_CURSOR,
    IDC_OFFSET_X,
    IDC_OFFSET_Y,
    IDC_OFFSET_RESET,
    IDC_PADDING,
    IDC_ANIM,
    IDC_AUTOSTART,
    IDC_UPDATE_CHECK,
    IDC_UPDATE_NOW,
    IDC_CLOSE,
};

// Enables app-wide dark mode for common controls via undocumented uxtheme
// ordinals (the same mechanism File Explorer uses). Best-effort: if the
// ordinals are unavailable the controls simply stay light.
void EnableDarkModeApp()
{
    enum PreferredAppMode { Default, AllowDark, ForceDark, ForceLight, Max };
    using SetPreferredAppModeFn = PreferredAppMode(WINAPI*)(PreferredAppMode);
    using FlushMenuThemesFn     = void(WINAPI*)();

    static bool done = false;
    if (done)
        return;
    done = true;

    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux)
        return;
    if (auto set = reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(ux, MAKEINTRESOURCEA(135))))
        set(ForceDark);
    if (auto flush = reinterpret_cast<FlushMenuThemesFn>(GetProcAddress(ux, MAKEINTRESOURCEA(136))))
        flush();
}

bool RequireModifier(unsigned mods)
{
    return mods != 0;
}

// Updates a "+N px" / "N px" offset value label.
void SetPxLabel(HWND label, int v)
{
    wchar_t buf[24];
    if (v > 0)
        swprintf_s(buf, L"+%d px", v);
    else
        swprintf_s(buf, L"%d px", v);
    SetWindowTextW(label, buf);
}
} // namespace

SettingsWindow::SettingsWindow(App& app) : app_(app)
{
}

SettingsWindow::~SettingsWindow()
{
    if (capturing_ && app_.hook)
        app_.hook->EndCapture();
    if (hwnd_)
        DestroyWindow(hwnd_);
    if (font_)
        DeleteObject(font_);
    if (titleFont_)
        DeleteObject(titleFont_);
    if (sectionFont_)
        DeleteObject(sectionFont_);
    if (fieldBrush_)
        DeleteObject(fieldBrush_);
    if (ctlBrush_)
        DeleteObject(ctlBrush_);
}

void SettingsWindow::Prepare()
{
    EnsureCreated();
    RefreshFromSettings();
}

void SettingsWindow::Reveal()
{
    if (!hwnd_)
        return;
    ShowWindow(hwnd_, SW_SHOW);

    // Reliably pull the window to the foreground even when opened from the tray
    // (the AttachThreadInput trick defeats the foreground-lock).
    const HWND  fg = GetForegroundWindow();
    const DWORD ft = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const DWORD ot = GetCurrentThreadId();
    if (ft && ft != ot)
        AttachThreadInput(ot, ft, TRUE);
    BringWindowToTop(hwnd_);
    SetForegroundWindow(hwnd_);
    SetActiveWindow(hwnd_);
    if (ft && ft != ot)
        AttachThreadInput(ot, ft, FALSE);
}

void SettingsWindow::EnsureCreated()
{
    if (hwnd_)
        return;

    EnableDarkModeApp();

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc   = &SettingsWindow::WndProcStatic;
    wc.hInstance     = app_.hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr; // painted in WM_ERASEBKGND
    wc.hIcon         = LoadIconW(app_.hInst, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm       = LoadIconW(app_.hInst, MAKEINTRESOURCEW(IDI_APPICON));
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    // Created before the window so WM_ERASEBKGND always has a valid brush.
    fieldBrush_ = CreateSolidBrush(kFieldColor);
    ctlBrush_   = CreateSolidBrush(kBackColor);

    // Size the window for a fixed logical layout, scaled to the primary DPI.
    dpi_ = GetDpiForSystem();
    auto S = [this](int v) { return MulDiv(v, static_cast<int>(dpi_), 96); };

    RECT rc{ 0, 0, S(480), S(950) };
    AdjustWindowRectExForDpi(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, 0, dpi_);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;

    POINT cur{};
    GetCursorPos(&cur);
    HMONITOR    mon = MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    const int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2;
    const int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2;

    hwnd_ = CreateWindowExW(0, kClassName, L"dock \u2014 Settings",
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VSCROLL, x, y, w, h,
                            nullptr, nullptr, app_.hInst, this);
    if (!hwnd_)
        return;

    dpi_ = GetDpiForWindow(hwnd_);

    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    auto makeFont = [this](int pt, bool semibold) {
        return CreateFontW(-MulDiv(pt, static_cast<int>(dpi_), 72), 0, 0, 0,
                           semibold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                           semibold ? L"Segoe UI Semibold" : L"Segoe UI");
    };
    font_        = makeFont(10, false);
    titleFont_   = makeFont(19, true);
    sectionFont_ = makeFont(10, true);

    CreateControls();
    FinalizeSizeAndScroll();
}

void SettingsWindow::CreateControls()
{
    auto S = [this](int v) { return MulDiv(v, static_cast<int>(dpi_), 96); };

    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w,
                    int h, int id, HFONT f, const wchar_t* theme) -> HWND {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w),
                                 S(h), hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 app_.hInst, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
        if (theme)
            SetWindowTheme(c, theme, nullptr);
        return c;
    };

    const int M     = 24;
    const int fullW = 480 - 2 * M;
    const int btnX  = 286;
    const int btnW  = 480 - M - btnX;
    int       yc    = 18;

    make(L"STATIC", L"dock settings", SS_LEFT, M, yc, fullW, 32, IDC_TITLE, titleFont_,
         nullptr);
    yc += 52;

    make(L"STATIC", L"Shortcut", SS_LEFT, M, yc, 200, 20, IDC_SEC_SHORTCUT, sectionFont_, nullptr);
    yc += 28;
    hHotkeyField_ = make(L"EDIT", L"", ES_CENTER | ES_READONLY | WS_BORDER, M, yc, 250, 30,
                         IDC_HOTKEY_FIELD, font_, L"DarkMode_CFD");
    hHotkeyBtn_   = make(L"BUTTON", L"Change\u2026", BS_PUSHBUTTON | WS_TABSTOP, btnX, yc, btnW, 30,
                         IDC_HOTKEY_BTN, font_, L"DarkMode_Explorer");
    yc += 42;
    hToggle_ = make(L"BUTTON", L"Toggle mode (press once to show / hide)",
                    BS_AUTOCHECKBOX | WS_TABSTOP, M, yc, fullW, 24, IDC_TOGGLE, font_,
                    L"DarkMode_Explorer");
    yc += 40;

    make(L"STATIC", L"Folder", SS_LEFT, M, yc, 200, 20, IDC_SEC_FOLDER, sectionFont_, nullptr);
    yc += 28;
    hFolderPath_ = make(L"STATIC", L"", SS_LEFT | SS_PATHELLIPSIS | SS_CENTERIMAGE, M, yc, 250, 28,
                        IDC_FOLDER_PATH, font_, nullptr);
    make(L"BUTTON", L"Choose\u2026", BS_PUSHBUTTON | WS_TABSTOP, btnX, yc, btnW, 30, IDC_FOLDER_BTN,
         font_, L"DarkMode_Explorer");
    yc += 42;

    make(L"STATIC", L"Layout", SS_LEFT, M, yc, 200, 20, IDC_SEC_LAYOUT, sectionFont_, nullptr);
    yc += 28;
    make(L"STATIC", L"Columns", SS_LEFT | SS_CENTERIMAGE, M, yc, 64, 28, IDC_COLS_LABEL, font_,
         nullptr);
    hColumns_ = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, M + 70, yc, 80,
                     240, IDC_COLUMNS, font_, L"DarkMode_CFD");
    make(L"STATIC", L"Rows", SS_LEFT | SS_CENTERIMAGE, M + 166, yc, 44, 28, IDC_ROWS_LABEL, font_,
         nullptr);
    hRows_ = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, M + 214, yc, 80,
                  240, IDC_ROWS, font_, L"DarkMode_CFD");
    for (HWND combo : { hColumns_, hRows_ })
    {
        ComboBox_AddString(combo, L"Auto");
        for (int i = 1; i <= 10; ++i)
        {
            wchar_t num[8];
            swprintf_s(num, L"%d", i);
            ComboBox_AddString(combo, num);
        }
    }
    yc += 40;
    hCenterCursor_ = make(L"BUTTON", L"Center cursor on the panel when it opens",
                          BS_AUTOCHECKBOX | WS_TABSTOP, M, yc, fullW, 24, IDC_CENTER_CURSOR, font_,
                          L"DarkMode_Explorer");
    yc += 34;
    make(L"STATIC", L"Inner padding", SS_LEFT | SS_CENTERIMAGE, M, yc, 110, 28, IDC_PADDING_LABEL,
         font_, nullptr);
    hPadding_ = make(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, M + 104, yc, 200,
                     28, IDC_PADDING, font_, nullptr);
    SendMessageW(hPadding_, TBM_SETRANGE, TRUE, MAKELPARAM(dk::kPaddingMin, dk::kPaddingMax));
    SendMessageW(hPadding_, TBM_SETPAGESIZE, 0, 4);
    hPaddingVal_ = make(L"STATIC", L"20 px", SS_LEFT | SS_CENTERIMAGE, M + 104 + 208, yc, 50, 28,
                        IDC_PADDING_VAL, font_, nullptr);
    yc += 40;

    make(L"STATIC", L"Position", SS_LEFT, M, yc, 200, 20, IDC_SEC_POSITION, sectionFont_, nullptr);
    yc += 28;
    make(L"STATIC", L"Horizontal", SS_LEFT | SS_CENTERIMAGE, M, yc, 88, 28, IDC_OFFX_LABEL, font_,
         nullptr);
    hOffsetX_ = make(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, M + 92, yc, 232, 28,
                     IDC_OFFSET_X, font_, nullptr);
    SendMessageW(hOffsetX_, TBM_SETRANGEMIN, FALSE, -dk::kOffsetLimit);
    SendMessageW(hOffsetX_, TBM_SETRANGEMAX, TRUE, dk::kOffsetLimit);
    SendMessageW(hOffsetX_, TBM_SETPAGESIZE, 0, 50);
    hOffXVal_ = make(L"STATIC", L"0 px", SS_LEFT | SS_CENTERIMAGE, M + 332, yc, 60, 28, IDC_OFFX_VAL,
                     font_, nullptr);
    yc += 36;
    make(L"STATIC", L"Vertical", SS_LEFT | SS_CENTERIMAGE, M, yc, 88, 28, IDC_OFFY_LABEL, font_,
         nullptr);
    hOffsetY_ = make(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, M + 92, yc, 232, 28,
                     IDC_OFFSET_Y, font_, nullptr);
    SendMessageW(hOffsetY_, TBM_SETRANGEMIN, FALSE, -dk::kOffsetLimit);
    SendMessageW(hOffsetY_, TBM_SETRANGEMAX, TRUE, dk::kOffsetLimit);
    SendMessageW(hOffsetY_, TBM_SETPAGESIZE, 0, 50);
    hOffYVal_ = make(L"STATIC", L"0 px", SS_LEFT | SS_CENTERIMAGE, M + 332, yc, 60, 28, IDC_OFFY_VAL,
                     font_, nullptr);
    yc += 36;
    make(L"BUTTON", L"Reset position", BS_PUSHBUTTON | WS_TABSTOP, M, yc, 150, 28, IDC_OFFSET_RESET,
         font_, L"DarkMode_Explorer");
    yc += 40;

    make(L"STATIC", L"Header", SS_LEFT, M, yc, 200, 20, IDC_SEC_HEADER, sectionFont_, nullptr);
    yc += 28;
    hShowName_  = make(L"BUTTON", L"Show folder name in header", BS_AUTOCHECKBOX | WS_TABSTOP, M,
                       yc, fullW, 24, IDC_SHOW_NAME, font_, L"DarkMode_Explorer");
    yc += 30;
    hShowCount_ = make(L"BUTTON", L"Show item count in header", BS_AUTOCHECKBOX | WS_TABSTOP, M, yc,
                       fullW, 24, IDC_SHOW_COUNT, font_, L"DarkMode_Explorer");
    yc += 40;

    make(L"STATIC", L"Appearance", SS_LEFT, M, yc, 200, 20, IDC_SEC_APPEARANCE, sectionFont_,
         nullptr);
    yc += 28;
    make(L"STATIC", L"Overlay backdrop", SS_LEFT | SS_CENTERIMAGE, M, yc, 160, 28,
         IDC_BACKDROP_LABEL, font_, nullptr);
    hBackdrop_ = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, btnX - 40, yc,
                      btnW + 40, 200, IDC_BACKDROP, font_, L"DarkMode_CFD");
    ComboBox_AddString(hBackdrop_, L"Solid");
    ComboBox_AddString(hBackdrop_, L"Acrylic blur");
    yc += 40;
    make(L"STATIC", L"Opacity", SS_LEFT | SS_CENTERIMAGE, M, yc, 110, 28, IDC_OPACITY_LABEL, font_,
         nullptr);
    hOpacity_ = make(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, M + 104, yc, 200,
                     28, IDC_OPACITY, font_, nullptr);
    SendMessageW(hOpacity_, TBM_SETRANGE, TRUE, MAKELPARAM(10, 100));
    SendMessageW(hOpacity_, TBM_SETPAGESIZE, 0, 5);
    hOpacityVal_ = make(L"STATIC", L"45%", SS_LEFT | SS_CENTERIMAGE, M + 104 + 208, yc, 50, 28,
                        IDC_OPACITY_VAL, font_, nullptr);
    yc += 40;
    hRounded_ = make(L"BUTTON", L"Rounded corners", BS_AUTOCHECKBOX | WS_TABSTOP, M, yc, 200, 24,
                     IDC_ROUNDED, font_, L"DarkMode_Explorer");
    hAnim_    = make(L"BUTTON", L"Fade animations", BS_AUTOCHECKBOX | WS_TABSTOP, M + 204, yc, 212,
                     24, IDC_ANIM, font_, L"DarkMode_Explorer");
    yc += 44;

    make(L"STATIC", L"Startup", SS_LEFT, M, yc, 200, 20, IDC_SEC_STARTUP, sectionFont_, nullptr);
    yc += 28;
    hAutostart_ = make(L"BUTTON", L"Start dock when I sign in", BS_AUTOCHECKBOX | WS_TABSTOP, M,
                       yc, fullW, 24, IDC_AUTOSTART, font_, L"DarkMode_Explorer");
    yc += 44;

    make(L"STATIC", L"Updates", SS_LEFT, M, yc, 200, 20, IDC_SEC_UPDATES, sectionFont_, nullptr);
    yc += 28;
    hUpdateCheck_ = make(L"BUTTON", L"Check for updates at launch", BS_AUTOCHECKBOX | WS_TABSTOP, M,
                         yc, fullW, 24, IDC_UPDATE_CHECK, font_, L"DarkMode_Explorer");
    yc += 32;
    hUpdateNow_   = make(L"BUTTON", L"Check now", BS_PUSHBUTTON | WS_TABSTOP, M, yc, 130, 30,
                         IDC_UPDATE_NOW, font_, L"DarkMode_Explorer");
    hUpdateStatus_ = make(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE | SS_PATHELLIPSIS, M + 142, yc,
                          fullW - 142, 30, IDC_UPDATE_STATUS, font_, nullptr);
    yc += 44;

    make(L"BUTTON", L"Close", BS_DEFPUSHBUTTON | WS_TABSTOP, btnX, yc, btnW, 32, IDC_CLOSE, font_,
         L"DarkMode_Explorer");
    yc += 32 + 18; // bottom margin
    contentH_ = Scaled(yc);

    // Shrink check boxes to fit their label so their (necessarily opaque) dark
    // fill hugs the text instead of spanning the whole row.
    auto fit = [&](HWND cb) {
        wchar_t text[256] = {};
        GetWindowTextW(cb, text, ARRAYSIZE(text));
        HDC      dc  = GetDC(cb);
        HGDIOBJ  old = SelectObject(dc, font_);
        SIZE     sz{};
        GetTextExtentPoint32W(dc, text, lstrlenW(text), &sz);
        SelectObject(dc, old);
        ReleaseDC(cb, dc);
        RECT r{};
        GetWindowRect(cb, &r);
        POINT p{ r.left, r.top };
        ScreenToClient(hwnd_, &p);
        MoveWindow(cb, p.x, p.y, S(22) + sz.cx + S(8), r.bottom - r.top, TRUE);
    };
    fit(hToggle_);
    fit(hShowName_);
    fit(hShowCount_);
    fit(hCenterCursor_);
    fit(hRounded_);
    fit(hAnim_);
    fit(hAutostart_);
    fit(hUpdateCheck_);
}

// Caps the window to the monitor work area and configures the vertical
// scrollbar so every control stays reachable on short screens / high scaling.
void SettingsWindow::FinalizeSizeAndScroll()
{
    if (!hwnd_)
        return;

    const DWORD frameStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    RECT        rc{ 0, 0, Scaled(480), contentH_ };
    AdjustWindowRectExForDpi(&rc, frameStyle, FALSE, 0, dpi_);
    const int totalW = rc.right - rc.left;
    const int frameH = (rc.bottom - rc.top) - contentH_; // caption + borders

    HMONITOR    mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    const int availH = mi.rcWork.bottom - mi.rcWork.top;

    const int totalH  = std::min(contentH_ + frameH, availH);
    const int clientH = std::max(0, totalH - frameH);

    scrollMax_ = std::max(0, contentH_ - clientH);
    scrollY_   = 0;

    const int px = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - totalW) / 2;
    const int py = mi.rcWork.top + (availH - totalH) / 2;
    SetWindowPos(hwnd_, nullptr, px, py, totalW, totalH, SWP_NOZORDER | SWP_NOACTIVATE);

    SCROLLINFO si{ sizeof(si) };
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin  = 0;
    si.nMax  = (contentH_ > 0) ? contentH_ - 1 : 0;
    si.nPage = static_cast<UINT>(clientH);
    si.nPos  = 0;
    SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
    ShowScrollBar(hwnd_, SB_VERT, scrollMax_ > 0);
}

// Scrolls the content (and all child controls) to the given vertical offset.
void SettingsWindow::ScrollTo(int pos)
{
    pos = std::clamp(pos, 0, scrollMax_);
    const int delta = scrollY_ - pos;
    if (delta == 0)
        return;
    scrollY_ = pos;
    ScrollWindow(hwnd_, 0, delta, nullptr, nullptr);

    SCROLLINFO si{ sizeof(si) };
    si.fMask = SIF_POS;
    si.nPos  = scrollY_;
    SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
    UpdateWindow(hwnd_);
}

void SettingsWindow::RefreshFromSettings()
{
    if (!hwnd_)
        return;
    const Settings& s = app_.settings;
    RefreshHotkeyField();
    Button_SetCheck(hToggle_, s.toggleMode ? BST_CHECKED : BST_UNCHECKED);
    SetWindowTextW(hFolderPath_, app_.model ? app_.model->FolderPath().c_str() : L"");
    Button_SetCheck(hShowName_, s.showFolderName ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(hShowCount_, s.showItemCount ? BST_CHECKED : BST_UNCHECKED);
    ComboBox_SetCurSel(hColumns_, s.gridColumns); // 0 = Auto, 1..10 map to index
    ComboBox_SetCurSel(hRows_, s.gridRows);
    Button_SetCheck(hCenterCursor_, s.centerCursor ? BST_CHECKED : BST_UNCHECKED);
    SendMessageW(hPadding_, TBM_SETPOS, TRUE, s.padding);
    wchar_t padTxt[16];
    swprintf_s(padTxt, L"%d px", s.padding);
    SetWindowTextW(hPaddingVal_, padTxt);
    SendMessageW(hOffsetX_, TBM_SETPOS, TRUE, s.offsetX);
    SendMessageW(hOffsetY_, TBM_SETPOS, TRUE, s.offsetY);
    SetPxLabel(hOffXVal_, s.offsetX);
    SetPxLabel(hOffYVal_, s.offsetY);
    ComboBox_SetCurSel(hBackdrop_, s.backdrop == BackdropStyle::Acrylic ? 1 : 0);
    SendMessageW(hOpacity_, TBM_SETPOS, TRUE, s.opacity);
    wchar_t pct[16];
    swprintf_s(pct, L"%d%%", s.opacity);
    SetWindowTextW(hOpacityVal_, pct);
    Button_SetCheck(hRounded_, s.roundedCorners ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(hAnim_, s.animations ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(hAutostart_, IsAutostartEnabled() ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(hUpdateCheck_, s.checkForUpdates ? BST_CHECKED : BST_UNCHECKED);
    SetWindowTextW(hUpdateStatus_, (L"Version " + dk::CurrentVersionW()).c_str());
}

void SettingsWindow::SetUpdateStatus(const std::wstring& text)
{
    if (hUpdateStatus_)
        SetWindowTextW(hUpdateStatus_, text.c_str());
}

void SettingsWindow::RefreshHotkeyField()
{
    const std::wstring text = HotkeyToString(app_.settings.hotkey.modifiers, app_.settings.hotkey.vk);
    SetWindowTextW(hHotkeyField_, text.c_str());
}

void SettingsWindow::BeginHotkeyCapture()
{
    if (capturing_ || !app_.hook)
        return;
    capturing_ = true;
    SetWindowTextW(hHotkeyField_, L"Press shortcut\u2026  (Esc to cancel)");
    SetWindowTextW(hHotkeyBtn_, L"Cancel");
    app_.hook->BeginCapture(
        [this](unsigned mods, unsigned vk, bool cancelled) { EndHotkeyCapture(cancelled, mods, vk); });
}

void SettingsWindow::EndHotkeyCapture(bool cancelled, unsigned mods, unsigned vk)
{
    capturing_ = false;
    SetWindowTextW(hHotkeyBtn_, L"Change\u2026");
    if (!cancelled && vk != 0)
    {
        if (RequireModifier(mods))
            app_.SetHotkey(mods, vk);
        else
            MessageBeep(MB_ICONWARNING); // require at least one modifier
    }
    RefreshHotkeyField();
}

void SettingsWindow::OnCommand(int id, int code)
{
    switch (id)
    {
    case IDC_HOTKEY_BTN:
        if (capturing_)
        {
            if (app_.hook)
                app_.hook->EndCapture();
            EndHotkeyCapture(true, 0, 0);
        }
        else
        {
            BeginHotkeyCapture();
        }
        break;
    case IDC_TOGGLE:
        app_.SetToggleMode(Button_GetCheck(hToggle_) == BST_CHECKED);
        app_.UpdateTrayTip();
        break;
    case IDC_FOLDER_BTN:
        app_.ChooseFolderInteractive(hwnd_);
        SetWindowTextW(hFolderPath_, app_.model ? app_.model->FolderPath().c_str() : L"");
        break;
    case IDC_SHOW_NAME:
        app_.SetShowFolderName(Button_GetCheck(hShowName_) == BST_CHECKED);
        break;
    case IDC_SHOW_COUNT:
        app_.SetShowItemCount(Button_GetCheck(hShowCount_) == BST_CHECKED);
        break;
    case IDC_COLUMNS:
        if (code == CBN_SELCHANGE)
            app_.SetColumns(ComboBox_GetCurSel(hColumns_));
        break;
    case IDC_ROWS:
        if (code == CBN_SELCHANGE)
            app_.SetRows(ComboBox_GetCurSel(hRows_));
        break;
    case IDC_CENTER_CURSOR:
        app_.SetCenterCursor(Button_GetCheck(hCenterCursor_) == BST_CHECKED);
        break;
    case IDC_OFFSET_RESET:
        SendMessageW(hOffsetX_, TBM_SETPOS, TRUE, 0);
        SendMessageW(hOffsetY_, TBM_SETPOS, TRUE, 0);
        app_.SetOffset(0, 0);
        SetPxLabel(hOffXVal_, 0);
        SetPxLabel(hOffYVal_, 0);
        app_.ReflowPreview();
        break;
    case IDC_BACKDROP:
        if (code == CBN_SELCHANGE)
        {
            const int sel = ComboBox_GetCurSel(hBackdrop_);
            app_.SetBackdrop(sel == 1 ? BackdropStyle::Acrylic : BackdropStyle::Solid);
        }
        break;
    case IDC_ROUNDED:
        app_.SetRoundedCorners(Button_GetCheck(hRounded_) == BST_CHECKED);
        break;
    case IDC_ANIM:
        app_.SetAnimations(Button_GetCheck(hAnim_) == BST_CHECKED);
        break;
    case IDC_AUTOSTART:
        app_.SetAutostart(Button_GetCheck(hAutostart_) == BST_CHECKED);
        break;
    case IDC_UPDATE_CHECK:
        app_.SetCheckForUpdates(Button_GetCheck(hUpdateCheck_) == BST_CHECKED);
        break;
    case IDC_UPDATE_NOW:
        SetWindowTextW(hUpdateStatus_, L"Checking\u2026");
        app_.CheckForUpdates(true);
        break;
    case IDCANCEL:
    case IDC_CLOSE:
        SendMessageW(hwnd_, WM_CLOSE, 0, 0);
        break;
    default:
        break;
    }
}

LRESULT CALLBACK SettingsWindow::WndProcStatic(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_NCCREATE)
    {
        auto* cs   = reinterpret_cast<CREATESTRUCTW*>(l);
        auto* self = static_cast<SettingsWindow*>(cs->lpCreateParams);
        self->hwnd_ = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self)
        return self->WndProc(m, w, l);
    return DefWindowProcW(h, m, w, l);
}

LRESULT SettingsWindow::WndProc(UINT m, WPARAM w, LPARAM l)
{
    switch (m)
    {
    case WM_COMMAND:
        OnCommand(LOWORD(w), HIWORD(w));
        return 0;

    case WM_HSCROLL:
    {
        const bool persist = (LOWORD(w) != TB_THUMBTRACK);
        if (reinterpret_cast<HWND>(l) == hOpacity_)
        {
            const int pos = static_cast<int>(SendMessageW(hOpacity_, TBM_GETPOS, 0, 0));
            app_.SetOpacity(pos, persist);
            wchar_t pct[16];
            swprintf_s(pct, L"%d%%", pos);
            SetWindowTextW(hOpacityVal_, pct);
        }
        else if (reinterpret_cast<HWND>(l) == hOffsetX_ || reinterpret_cast<HWND>(l) == hOffsetY_)
        {
            const int x = static_cast<int>(SendMessageW(hOffsetX_, TBM_GETPOS, 0, 0));
            const int y = static_cast<int>(SendMessageW(hOffsetY_, TBM_GETPOS, 0, 0));
            app_.SetOffset(x, y, persist); // moves only the overlay (live)
            SetPxLabel(hOffXVal_, x);
            SetPxLabel(hOffYVal_, y);
            // On a discrete change / drag release (not a live thumb drag), settle
            // the settings window aside if it would now overlap the overlay.
            if (LOWORD(w) != TB_THUMBTRACK)
                app_.ReflowPreview();
        }
        else if (reinterpret_cast<HWND>(l) == hPadding_)
        {
            const int pos = static_cast<int>(SendMessageW(hPadding_, TBM_GETPOS, 0, 0));
            app_.SetPadding(pos, persist); // resizes the overlay live
            wchar_t buf[16];
            swprintf_s(buf, L"%d px", pos);
            SetWindowTextW(hPaddingVal_, buf);
            // Padding changes the panel size; once settled, dodge the settings
            // window if the now-resized overlay would overlap it.
            if (LOWORD(w) != TB_THUMBTRACK)
                app_.ReflowPreview();
        }
        return 0;
    }

    case WM_VSCROLL:
        if (!l) // the window's own scrollbar (child trackbars send WM_HSCROLL)
        {
            SCROLLINFO si{ sizeof(si) };
            si.fMask = SIF_ALL;
            GetScrollInfo(hwnd_, SB_VERT, &si);
            int pos = si.nPos;
            switch (LOWORD(w))
            {
            case SB_LINEUP:        pos -= Scaled(24);  break;
            case SB_LINEDOWN:      pos += Scaled(24);  break;
            case SB_PAGEUP:        pos -= si.nPage;    break;
            case SB_PAGEDOWN:      pos += si.nPage;    break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION: pos = si.nTrackPos; break;
            case SB_TOP:           pos = 0;            break;
            case SB_BOTTOM:        pos = scrollMax_;   break;
            default:                                  break;
            }
            ScrollTo(pos);
        }
        return 0;

    case WM_MOUSEWHEEL:
        ScrollTo(scrollY_ - (GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA) * Scaled(48));
        return 0;

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    {
        HDC       hdc = reinterpret_cast<HDC>(w);
        const int id  = GetDlgCtrlID(reinterpret_cast<HWND>(l));
        SetBkMode(hdc, OPAQUE);
        if (id == IDC_HOTKEY_FIELD)
        {
            SetTextColor(hdc, RGB(240, 240, 240));
            SetBkColor(hdc, kFieldColor);
            return reinterpret_cast<LRESULT>(fieldBrush_);
        }
        // Labels, check boxes and the slider all share the window background, so
        // they blend in seamlessly and (being opaque) erase old text on change.
        const bool section = (id >= IDC_SEC_SHORTCUT && id <= IDC_SEC_UPDATES);
        SetTextColor(hdc, section ? kAccentColor : kTextColor);
        SetBkColor(hdc, kBackColor);
        return reinterpret_cast<LRESULT>(ctlBrush_);
    }

    case WM_ERASEBKGND:
    {
        HDC  hdc = reinterpret_cast<HDC>(w);
        RECT rc;
        GetClientRect(hwnd_, &rc);
        FillRect(hdc, &rc, ctlBrush_);
        return 1;
    }

    case WM_CLOSE:
        if (capturing_)
        {
            if (app_.hook)
                app_.hook->EndCapture();
            EndHotkeyCapture(true, 0, 0);
        }
        ShowWindow(hwnd_, SW_HIDE);
        app_.OnSettingsClosed();
        return 0;

    case WM_DESTROY:
        hwnd_ = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd_, m, w, l);
}
