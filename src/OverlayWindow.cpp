// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dolu <https://github.com/DoluTattoo>

#include "OverlayWindow.h"
#include "FolderModel.h"

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wincodec.h>
#include <shellscalingapi.h>
#include <shellapi.h>
#include <windowsx.h>
#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <string>

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
// DWM_WINDOW_CORNER_PREFERENCE values (avoid depending on the SDK enum).
constexpr int kCornerDoNotRound = 1;
constexpr int kCornerRound      = 2;

namespace
{
// Layout metrics in logical units (DIPs). Scaled by the monitor DPI at render.
constexpr float kTitleH   = 26.0f;
constexpr float kCellW    = 96.0f;
constexpr float kCellH    = 98.0f;
constexpr float kIcon     = 48.0f;
constexpr float kIconTop  = 12.0f;
constexpr float kTextGap  = 8.0f;
constexpr float kCorner   = 16.0f;
constexpr float kMinWidth = 280.0f;
constexpr int   kMaxCols  = 8;

UINT GetMonitorDpi(HMONITOR mon)
{
    UINT dpiX = 96, dpiY = 96;
    if (mon && SUCCEEDED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)))
        return dpiX;
    return 96;
}

// The Start menu and taskbar live in elevated z-order "bands" that a normal
// HWND_TOPMOST window cannot rise above. The shell creates such windows with
// the undocumented user32 export CreateWindowInBand. ZBID_SYSTEM_TOOLS (16) is
// a band that sits above the Start menu and does not require uiAccess.
constexpr DWORD ZBID_SYSTEM_TOOLS = 16;

using CreateWindowInBandFn = HWND(WINAPI*)(DWORD dwExStyle, ATOM atom, LPCWSTR lpWindowName,
                                           DWORD dwStyle, int x, int y, int w, int h,
                                           HWND parent, HMENU menu, HINSTANCE inst,
                                           LPVOID param, DWORD band);

// Undocumented acrylic blur-behind for layered windows, via user32's
// SetWindowCompositionAttribute. This is what gives the overlay the Windows 11
// "free blur" look while keeping our per-pixel-alpha rounded panel on top.
enum AccentState
{
    ACCENT_DISABLED                 = 0,
    ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
};
struct ACCENT_POLICY
{
    DWORD AccentState;
    DWORD AccentFlags;
    DWORD GradientColor; // 0xAABBGGRR
    DWORD AnimationId;
};
struct WINDOWCOMPOSITIONATTRIBDATA
{
    DWORD  Attrib;
    PVOID  pvData;
    SIZE_T cbData;
};
constexpr DWORD WCA_ACCENT_POLICY = 19;
using SetWindowCompositionAttributeFn = BOOL(WINAPI*)(HWND, WINDOWCOMPOSITIONATTRIBDATA*);

constexpr UINT  kAnimTimer = 1;
constexpr float kFadeStep  = 0.16f; // opacity change per ~8 ms tick (~80 ms fade)
} // namespace

bool OverlayWindow::Initialize(HINSTANCE hInst, FolderModel* model, IWICImagingFactory* wic)
{
    hInst_ = hInst;
    model_ = model;
    wic_   = wic;

    CreateDeviceIndependentResources();

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc   = &OverlayWindow::WndProcStatic;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DockOverlay";
    const ATOM atom = RegisterClassExW(&wc);

    // NOTE: deliberately NO WS_EX_NOACTIVATE — the overlay must be able to take
    // the foreground (see ForceForeground) so games release their cursor clip.
    const DWORD exStyle = WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST;

    // Prefer creating the window in the shell's system-tools z-band so it draws
    // above the Start menu; fall back to a normal top-most window otherwise.
    if (HMODULE u32 = GetModuleHandleW(L"user32.dll"))
    {
        if (auto createInBand =
                reinterpret_cast<CreateWindowInBandFn>(GetProcAddress(u32, "CreateWindowInBand")))
        {
            if (atom)
                hwnd_ = createInBand(exStyle, atom, L"dock", WS_POPUP, 0, 0, 16, 16,
                                     nullptr, nullptr, hInst, this, ZBID_SYSTEM_TOOLS);
        }
    }

    if (!hwnd_)
        hwnd_ = CreateWindowExW(exStyle, L"DockOverlay", L"dock", WS_POPUP,
                                0, 0, 16, 16, nullptr, nullptr, hInst, this);

    return hwnd_ != nullptr;
}

void OverlayWindow::Shutdown()
{
    if (hwnd_)
    {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    ReleaseDeviceResources();
    if (memDC_)
    {
        if (origBitmap_)
            SelectObject(memDC_, origBitmap_);
        DeleteDC(memDC_);
        memDC_ = nullptr;
    }
    if (dib_)
    {
        DeleteObject(dib_);
        dib_ = nullptr;
    }
}

void OverlayWindow::CreateDeviceIndependentResources()
{
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory_.GetAddressOf());
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()));

    dwrite_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                              12.5f, L"", &textFormat_);
    textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    textFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    {
        DWRITE_TRIMMING            tr{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        ComPtr<IDWriteInlineObject> sign;
        dwrite_->CreateEllipsisTrimmingSign(textFormat_.Get(), &sign);
        textFormat_->SetTrimming(&tr, sign.Get());
    }

    dwrite_->CreateTextFormat(L"Segoe UI Semibold", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                              15.0f, L"", &titleFormat_);
    titleFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    titleFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    titleFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    {
        DWRITE_TRIMMING            tr{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        ComPtr<IDWriteInlineObject> sign;
        dwrite_->CreateEllipsisTrimmingSign(titleFormat_.Get(), &sign);
        titleFormat_->SetTrimming(&tr, sign.Get());
    }
}

void OverlayWindow::CreateDeviceResources()
{
    if (dcRT_)
        return;

    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        0.0f, 0.0f, D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE);

    if (FAILED(d2dFactory_->CreateDCRenderTarget(&props, &dcRT_)))
    {
        dcRT_.Reset();
        return;
    }

    // The panel body opacity is user-controlled; the title, text and icons keep
    // their own (opaque) brushes so they stay readable at low opacity.
    dcRT_->CreateSolidColorBrush(D2D1::ColorF(0.11f, 0.11f, 0.12f, bgOpacity_), &brushBg_);
    dcRT_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f), &brushBorder_);
    dcRT_->CreateSolidColorBrush(D2D1::ColorF(0.92f, 0.92f, 0.94f, 1.0f), &brushText_);
    dcRT_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f), &brushTitle_);
    dcRT_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f), &brushHover_);
    iconCache_.clear();
}

void OverlayWindow::ReleaseDeviceResources()
{
    iconCache_.clear();
    brushBg_.Reset();
    brushBorder_.Reset();
    brushText_.Reset();
    brushTitle_.Reset();
    brushHover_.Reset();
    dcRT_.Reset();
}

void OverlayWindow::EnsureDIB(int w, int h)
{
    if (w == dibSize_.cx && h == dibSize_.cy && dib_)
        return;
    if (w <= 0 || h <= 0)
        return;

    if (!memDC_)
        memDC_ = CreateCompatibleDC(nullptr);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -h; // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void*   bits   = nullptr;
    HBITMAP newDib = CreateDIBSection(memDC_, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!newDib)
        return;

    HGDIOBJ prev = SelectObject(memDC_, newDib);
    if (!origBitmap_)
        origBitmap_ = prev; // first selection returns the default 1x1 bitmap
    if (dib_)
        DeleteObject(dib_);

    dib_     = newDib;
    dibSize_ = { w, h };
}

void OverlayWindow::ComputeLayout(float scale)
{
    const auto&   items   = model_->Items();
    const int     n       = static_cast<int>(items.size());
    constexpr int kMaxDim = 12;

    // Columns: explicit, or derived from explicit rows, or the auto square-ish
    // grid. Rows: explicit (capped to what the items need) or derived.
    int cols;
    if (userCols_ > 0)
        cols = userCols_;
    else if (userRows_ > 0)
        cols = (n > 0) ? (n + userRows_ - 1) / userRows_ : 1;
    else
        cols = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(std::max(1, n)))));

    const int maxCols = (userCols_ > 0 || userRows_ > 0) ? kMaxDim : kMaxCols;
    cols = std::clamp(cols, 1, maxCols);
    if (userCols_ == 0 && userRows_ == 0 && n > 0 && cols > n)
        cols = n;

    const int neededRows = (n > 0) ? (n + cols - 1) / cols : 1;
    int       rows = (userRows_ > 0) ? std::min(userRows_, neededRows) : neededRows;
    rows = std::clamp(rows, 1, kMaxDim);

    cols_         = cols;
    rows_         = rows;
    visibleCount_ = (n > 0) ? std::min(n, cols * rows) : 0;

    const float gridW = cols * kCellW;
    const bool  hasHeader = showName_ || showCount_;
    gridTopDip_       = hasHeader ? (padding_ + kTitleH + 8.0f) : padding_;
    panelWDip_        = std::max(gridW + 2.0f * padding_, kMinWidth);
    panelHDip_        = gridTopDip_ + ((n > 0) ? rows * kCellH : 60.0f) + padding_;
    gridLeftDip_      = (panelWDip_ - gridW) / 2.0f;

    panelPxW_ = static_cast<int>(std::lround(panelWDip_ * scale));
    panelPxH_ = static_cast<int>(std::lround(panelHDip_ * scale));

    itemRects_.clear();
    itemRects_.reserve(visibleCount_);
    for (int i = 0; i < visibleCount_; ++i)
    {
        const int   c = i % cols_;
        const int   r = i / cols_;
        const float x = gridLeftDip_ + c * kCellW;
        const float y = gridTopDip_ + r * kCellH;
        RECT        rc{
            static_cast<LONG>(std::lround(x * scale)),
            static_cast<LONG>(std::lround(y * scale)),
            static_cast<LONG>(std::lround((x + kCellW) * scale)),
            static_cast<LONG>(std::lround((y + kCellH) * scale)) };
        itemRects_.push_back(rc);
    }
}

void OverlayWindow::Render(float scale)
{
    if (!model_)
        return;

    CreateDeviceResources();
    if (!dcRT_)
        return;

    EnsureDIB(panelPxW_, panelPxH_);
    if (!dib_)
        return;

    RECT bind{ 0, 0, panelPxW_, panelPxH_ };
    if (FAILED(dcRT_->BindDC(memDC_, &bind)))
        return;
    dcRT_->SetDpi(96.0f * scale, 96.0f * scale);

    const auto& items = model_->Items();

    if (iconCache_.size() != items.size())
        iconCache_.assign(items.size(), nullptr);
    for (size_t i = 0; i < items.size(); ++i)
    {
        if (!iconCache_[i] && items[i].icon)
            dcRT_->CreateBitmapFromWicBitmap(items[i].icon.Get(), &iconCache_[i]);
    }

    dcRT_->BeginDraw();
    dcRT_->Clear(D2D1::ColorF(0, 0, 0, 0));

    if (backdrop_ == BackdropStyle::Acrylic)
    {
        // Fill full-bleed: DWM rounds the window (and the blur behind it) so the
        // corners stay smooth. A per-pixel-alpha region would leave the blur
        // square, which is the artefact we are avoiding here.
        dcRT_->FillRectangle(D2D1::RectF(0.0f, 0.0f, panelWDip_, panelHDip_), brushBg_.Get());
        dcRT_->DrawRectangle(D2D1::RectF(0.5f, 0.5f, panelWDip_ - 0.5f, panelHDip_ - 0.5f),
                             brushBorder_.Get(), 1.0f);
    }
    else
    {
        const D2D1_RECT_F body = D2D1::RectF(0.5f, 0.5f, panelWDip_ - 0.5f, panelHDip_ - 0.5f);
        if (rounded_)
        {
            const D2D1_ROUNDED_RECT bg = D2D1::RoundedRect(body, kCorner, kCorner);
            dcRT_->FillRoundedRectangle(bg, brushBg_.Get());
            dcRT_->DrawRoundedRectangle(bg, brushBorder_.Get(), 1.0f);
        }
        else
        {
            dcRT_->FillRectangle(body, brushBg_.Get());
            dcRT_->DrawRectangle(body, brushBorder_.Get(), 1.0f);
        }
    }

    const size_t count = items.size();
    if (showName_ || showCount_)
    {
        std::wstring title;
        if (showName_)
            title = model_->FolderName();
        if (showCount_)
        {
            std::wstring c = std::to_wstring(count) + (count == 1 ? L" item" : L" items");
            title = title.empty() ? c : title + L"   \u00B7   " + c;
        }
        const D2D1_RECT_F titleRect =
            D2D1::RectF(padding_, padding_ - 2.0f, panelWDip_ - padding_, padding_ + kTitleH);
        dcRT_->DrawText(title.c_str(), static_cast<UINT32>(title.size()), titleFormat_.Get(),
                        titleRect, brushTitle_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    if (items.empty())
    {
        const D2D1_RECT_F r =
            D2D1::RectF(padding_, gridTopDip_, panelWDip_ - padding_, panelHDip_ - padding_);
        const wchar_t* msg = L"Empty folder";
        dcRT_->DrawText(msg, static_cast<UINT32>(wcslen(msg)), textFormat_.Get(), r,
                        brushText_.Get());
    }
    else
    {
        const size_t shown = std::min(items.size(), static_cast<size_t>(visibleCount_));
        for (size_t i = 0; i < shown; ++i)
        {
            const int   c  = static_cast<int>(i) % cols_;
            const int   r  = static_cast<int>(i) / cols_;
            const float cx = gridLeftDip_ + c * kCellW;
            const float cy = gridTopDip_ + r * kCellH;

            if (static_cast<int>(i) == hoverIndex_)
            {
                const D2D1_RECT_F hlRect = D2D1::RectF(cx + 4.0f, cy + 4.0f, cx + kCellW - 4.0f,
                                                       cy + kCellH - 4.0f);
                if (rounded_)
                    dcRT_->FillRoundedRectangle(D2D1::RoundedRect(hlRect, 10.0f, 10.0f),
                                                brushHover_.Get());
                else
                    dcRT_->FillRectangle(hlRect, brushHover_.Get());
            }

            if (iconCache_[i])
            {
                // Thumbnails are rarely square, so fit the bitmap inside the
                // icon box while preserving its aspect ratio and centre it — the
                // same letterboxing Explorer uses for previews in its cells.
                const D2D1_SIZE_F bs = iconCache_[i]->GetSize();
                float             dw = kIcon, dh = kIcon;
                if (bs.width > 0.0f && bs.height > 0.0f)
                {
                    const float fit = std::min(kIcon / bs.width, kIcon / bs.height);
                    dw              = bs.width * fit;
                    dh              = bs.height * fit;
                }
                const float       ix  = cx + (kCellW - dw) / 2.0f;
                const float       iy  = cy + kIconTop + (kIcon - dh) / 2.0f;
                const D2D1_RECT_F dst = D2D1::RectF(ix, iy, ix + dw, iy + dh);
                dcRT_->DrawBitmap(iconCache_[i].Get(), dst, 1.0f,
                                  D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            }

            const D2D1_RECT_F tr = D2D1::RectF(cx + 2.0f, cy + kIconTop + kIcon + kTextGap,
                                               cx + kCellW - 2.0f, cy + kCellH - 2.0f);
            dcRT_->DrawText(items[i].name.c_str(), static_cast<UINT32>(items[i].name.size()),
                            textFormat_.Get(), tr, brushText_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }

    const HRESULT hr = dcRT_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET)
    {
        ReleaseDeviceResources();
        return;
    }

    Present();
}

// Pushes the rendered DIB to the layered window at the current fade opacity.
void OverlayWindow::Present()
{
    if (!dib_)
        return;
    POINT         ptSrc{ 0, 0 };
    SIZE          sz{ panelPxW_, panelPxH_ };
    POINT         ptDst = panelPos_;
    const BYTE    a     = static_cast<BYTE>(std::lround(std::clamp(curAlpha_, 0.0f, 1.0f) * 255.0f));
    BLENDFUNCTION blend{ AC_SRC_OVER, 0, a, AC_SRC_ALPHA };
    HDC           screen = GetDC(nullptr);
    UpdateLayeredWindow(hwnd_, screen, &ptDst, &sz, memDC_, &ptSrc, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

void OverlayWindow::Show()
{
    if (!model_)
        return;

    POINT cursor{};
    GetCursorPos(&cursor);
    HMONITOR    mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);

    preview_       = false;
    const SIZE sz  = MeasureForMonitor(mon);

    const int workW = mi.rcWork.right - mi.rcWork.left;
    const int workH = mi.rcWork.bottom - mi.rcWork.top;
    const int sw    = static_cast<int>(sz.cx);
    const int sh    = static_cast<int>(sz.cy);
    int       px    = static_cast<int>(mi.rcWork.left) + (workW - sw) / 2 + offsetX_;
    int       py    = static_cast<int>(mi.rcWork.top) + (workH - sh) / 2 + offsetY_;
    px = std::clamp(px, static_cast<int>(mi.rcWork.left), static_cast<int>(mi.rcWork.right) - sw);
    py = std::clamp(py, static_cast<int>(mi.rcWork.top), static_cast<int>(mi.rcWork.bottom) - sh);
    panelPos_.x = px;
    panelPos_.y = py;

    ApplyBackdrop();
    visible_       = true;
    targetVisible_ = true;
    curAlpha_      = animations_ ? 0.0f : 1.0f; // fade in from transparent

    Render(scale_); // paints the DIB and presents it at curAlpha_

    // Remember who had focus (e.g. a game) so we can hand it back on Hide().
    prevForeground_ = GetForegroundWindow();

    ShowWindow(hwnd_, SW_SHOWNA);
    ForceForeground();

    // A game (e.g. FiveM) may have hidden/confined/recentred the cursor. Now
    // that we own the foreground, optionally drop the cursor onto the panel so
    // it is visible and the hover works immediately.
    if (centerCursor_)
        SetCursorPos(panelPos_.x + panelPxW_ / 2, panelPos_.y + panelPxH_ / 2);

    if (animations_)
        SetTimer(hwnd_, kAnimTimer, 8, nullptr);
}

// Computes the panel layout for a monitor without showing it, and returns its
// pixel size so the caller can position it.
SIZE OverlayWindow::MeasureForMonitor(HMONITOR mon)
{
    scale_      = GetMonitorDpi(mon) / 96.0f;
    hoverIndex_ = -1;
    tracking_   = false;
    ComputeLayout(scale_);
    return SIZE{ panelPxW_, panelPxH_ };
}

// Shows the panel at an explicit position for live preview while the settings
// window is open. Unlike Show(), it does not move the cursor or steal the
// foreground, so the settings window stays interactive. Call MeasureForMonitor
// first to lay the panel out.
void OverlayWindow::ShowPreviewAt(int x, int y)
{
    if (!model_)
        return;

    const bool wasVisible = visible_;
    preview_     = true;
    panelPos_.x  = x;
    panelPos_.y  = y;

    ApplyBackdrop();
    visible_       = true;
    targetVisible_ = true;
    if (!wasVisible)
        curAlpha_ = animations_ ? 0.0f : 1.0f;

    Render(scale_);

    ShowWindow(hwnd_, SW_SHOWNA);
    // Stay above normal windows without activating (the system-tools band keeps
    // it above the settings window, which retains keyboard focus).
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);

    if (animations_ && !wasVisible)
        SetTimer(hwnd_, kAnimTimer, 8, nullptr);
}

// Robustly pulls the foreground to our overlay, even from a background process
// and even when a full-screen/windowed game currently owns input. This makes
// the game deactivate, which releases its cursor clip and restores the cursor.
void OverlayWindow::ForceForeground()
{
    ClipCursor(nullptr); // release any cursor confinement the game set

    const HWND  fg        = GetForegroundWindow();
    const DWORD fgThread  = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const DWORD ourThread = GetCurrentThreadId();

    // Sharing the foreground thread's input queue lets SetForegroundWindow
    // succeed from a background app (the well-known AttachThreadInput trick).
    if (fgThread && fgThread != ourThread)
        AttachThreadInput(ourThread, fgThread, TRUE);

    AllowSetForegroundWindow(ASFW_ANY);
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    BringWindowToTop(hwnd_);
    SetForegroundWindow(hwnd_);
    SetActiveWindow(hwnd_);

    if (fgThread && fgThread != ourThread)
        AttachThreadInput(ourThread, fgThread, FALSE);
}

void OverlayWindow::Hide()
{
    if (!visible_)
        return;
    visible_       = false;
    targetVisible_ = false;

    if (animations_ && IsWindowVisible(hwnd_))
        SetTimer(hwnd_, kAnimTimer, 8, nullptr); // fade out, finalize when done
    else
        FinalizeHide();
}

void OverlayWindow::FinalizeHide()
{
    KillTimer(hwnd_, kAnimTimer);
    curAlpha_ = 0.0f;
    ShowWindow(hwnd_, SW_HIDE);

    // Hand focus back to the app/game that had it, so the mouse-look and cursor
    // capture resume exactly as before. In preview mode the settings window owns
    // the foreground, so leave it untouched.
    if (!preview_ && prevForeground_ && IsWindow(prevForeground_))
        SetForegroundWindow(prevForeground_);
    prevForeground_ = nullptr;
    preview_        = false;
}

// Applies rounded corners and (for Acrylic) the blur-behind composition effect.
void OverlayWindow::ApplyBackdrop()
{
    // Round corners through DWM: unlike a GDI window region, the DWM corner clip
    // also rounds the acrylic blur. For Solid we keep our own antialiased
    // Direct2D corners and ask DWM not to round.
    int pref = (backdrop_ == BackdropStyle::Acrylic && rounded_) ? kCornerRound : kCornerDoNotRound;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
    SetWindowRgn(hwnd_, nullptr, FALSE);

    static auto setAttr = reinterpret_cast<SetWindowCompositionAttributeFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute"));
    if (!setAttr)
        return;

    ACCENT_POLICY accent{};
    if (backdrop_ == BackdropStyle::Acrylic)
    {
        accent.AccentState   = ACCENT_ENABLE_ACRYLICBLURBEHIND;
        accent.GradientColor = 0xB0181820; // 0xAABBGGRR dark tint
    }
    else
    {
        accent.AccentState = ACCENT_DISABLED;
    }
    WINDOWCOMPOSITIONATTRIBDATA data{ WCA_ACCENT_POLICY, &accent, sizeof(accent) };
    setAttr(hwnd_, &data);
}

void OverlayWindow::SetHeader(bool showName, bool showCount)
{
    showName_  = showName;
    showCount_ = showCount;
}

void OverlayWindow::SetRounded(bool rounded)
{
    if (rounded_ == rounded)
        return;
    rounded_ = rounded;
    if (visible_)
    {
        ApplyBackdrop();
        Render(scale_);
    }
}

void OverlayWindow::SetGrid(int columns, int rows)
{
    userCols_ = columns;
    userRows_ = rows;
    // The panel size may change; the caller re-lays-out (App::LayoutPreview) or
    // the next Show() recomputes from scratch.
}

void OverlayWindow::SetPadding(int padding)
{
    padding_ = static_cast<float>(padding);
    // The panel size changes; the caller re-lays-out (App::PlaceOverlayPreview)
    // or the next Show() recomputes from scratch.
}

void OverlayWindow::SetCenterCursor(bool center)
{
    centerCursor_ = center;
}

void OverlayWindow::SetOffset(int offsetX, int offsetY)
{
    offsetX_ = offsetX;
    offsetY_ = offsetY;
}

void OverlayWindow::SetBackdrop(BackdropStyle backdrop)
{
    if (backdrop_ == backdrop)
        return;
    backdrop_ = backdrop;
    ReleaseDeviceResources(); // rebuild brushes with the new background alpha
    if (visible_)
    {
        ApplyBackdrop();
        Render(scale_);
    }
}

void OverlayWindow::SetOpacity(float alpha)
{
    alpha = std::clamp(alpha, 0.10f, 1.0f);
    if (std::fabs(alpha - bgOpacity_) < 0.001f)
        return;
    bgOpacity_ = alpha;
    ReleaseDeviceResources(); // rebuild the background brush at the new alpha
    if (visible_)
        Render(scale_);
}

void OverlayWindow::SetAnimations(bool enabled)
{
    animations_ = enabled;
}

void OverlayWindow::InvalidateIcons()
{
    // Drop cached device bitmaps so the next render rebuilds them from the
    // (possibly changed) folder model.
    iconCache_.clear();
    hoverIndex_ = -1;
}

void OverlayWindow::RerenderIfVisible()
{
    // Re-paint in place (e.g. when async icons finish loading) without changing
    // the panel position or size.
    if (visible_)
        Render(scale_);
}

void OverlayWindow::RelayoutIfVisible()
{
    // Re-measure and re-position for the current monitor when the folder
    // contents changed while the overlay is open, so the panel grows or shrinks
    // to fit. Preview mode is repositioned by the App (LayoutPreview) instead.
    if (!visible_ || preview_)
        return;

    HMONITOR    mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);

    const SIZE sz    = MeasureForMonitor(mon);
    const int  workW = mi.rcWork.right - mi.rcWork.left;
    const int  workH = mi.rcWork.bottom - mi.rcWork.top;
    const int  sw    = static_cast<int>(sz.cx);
    const int  sh    = static_cast<int>(sz.cy);
    int        px    = static_cast<int>(mi.rcWork.left) + (workW - sw) / 2 + offsetX_;
    int        py    = static_cast<int>(mi.rcWork.top) + (workH - sh) / 2 + offsetY_;
    px = std::clamp(px, static_cast<int>(mi.rcWork.left), static_cast<int>(mi.rcWork.right) - sw);
    py = std::clamp(py, static_cast<int>(mi.rcWork.top), static_cast<int>(mi.rcWork.bottom) - sh);
    panelPos_.x = px;
    panelPos_.y = py;

    Render(scale_);
}

int OverlayWindow::HitTest(POINT pt) const
{
    for (size_t i = 0; i < itemRects_.size(); ++i)
    {
        if (PtInRect(&itemRects_[i], pt))
            return static_cast<int>(i);
    }
    return -1;
}

void OverlayWindow::OpenItem(int index)
{
    if (!model_ || index < 0 || index >= static_cast<int>(model_->Items().size()))
        return;
    ShellExecuteW(nullptr, L"open", model_->Items()[index].path.c_str(), nullptr, nullptr,
                  SW_SHOWNORMAL);
}

LRESULT CALLBACK OverlayWindow::WndProcStatic(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_NCCREATE)
    {
        auto* cs   = reinterpret_cast<CREATESTRUCTW*>(l);
        auto* self = static_cast<OverlayWindow*>(cs->lpCreateParams);
        self->hwnd_ = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }

    auto* self = reinterpret_cast<OverlayWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self)
        return self->WndProc(m, w, l);
    return DefWindowProcW(h, m, w, l);
}

LRESULT OverlayWindow::WndProc(UINT m, WPARAM w, LPARAM l)
{
    switch (m)
    {
    case WM_TIMER:
        if (w == kAnimTimer)
        {
            const float target = targetVisible_ ? 1.0f : 0.0f;
            if (curAlpha_ < target)
                curAlpha_ = std::min(target, curAlpha_ + kFadeStep);
            else
                curAlpha_ = std::max(target, curAlpha_ - kFadeStep);
            Present();
            if (curAlpha_ == target)
            {
                if (targetVisible_)
                    KillTimer(hwnd_, kAnimTimer);
                else
                    FinalizeHide();
            }
        }
        return 0;
    case WM_MOUSEMOVE:
    {
        if (!tracking_)
        {
            TRACKMOUSEEVENT tme{ sizeof(tme) };
            tme.dwFlags   = TME_LEAVE;
            tme.hwndTrack = hwnd_;
            TrackMouseEvent(&tme);
            tracking_ = true;
        }
        POINT     pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        const int idx = HitTest(pt);
        if (idx != hoverIndex_)
        {
            hoverIndex_ = idx;
            Render(scale_);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_ = false;
        if (hoverIndex_ != -1)
        {
            hoverIndex_ = -1;
            Render(scale_);
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT)
        {
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(hwnd_, &pt);
            SetCursor(LoadCursorW(nullptr, HitTest(pt) >= 0 ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONUP:
    {
        POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        const int idx = HitTest(pt);
        if (idx >= 0)
            OpenItem(idx); // keep the panel open so several items can be opened
                           // during one hold; it dismisses on release / toggle /
                           // right-click
        return 0;
    }
    case WM_RBUTTONUP:
        if (!preview_)
            Hide();
        return 0;
    }
    return DefWindowProcW(hwnd_, m, w, l);
}
