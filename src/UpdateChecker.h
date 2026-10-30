// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dolu <https://github.com/DoluTattoo>

#pragma once

#include "Common.h"

// Result of a background update check, posted to the control window as the
// lParam of dk::WM_APP_UPDATE_READY (the receiver takes ownership and deletes).
struct UpdateResult
{
    bool         available     = false; // a newer release exists
    bool         failed        = false; // the check could not complete (network/parse)
    bool         userInitiated = false; // the user pressed "Check now"
    std::wstring latest;                // latest release version, e.g. "0.2.0"
    std::wstring current;               // this build's version
    std::wstring url;                   // official GitHub Releases page
    std::wstring downloadUrl;           // validated installer download URL
    std::wstring checksumUrl;           // matching SHA256SUMS.txt download URL
    std::wstring filename;              // expected dock-<version>-setup.exe
};

// A verified installer's temporary path; the UI thread takes ownership of this result.
struct DownloadResult
{
    bool ok = false;
    std::wstring path;
};

namespace dk
{
// This build's version string (injected by CMake from the project / release tag).
std::wstring CurrentVersionW();

// Compares two dotted version strings ("1.2.3"); a leading 'v' is ignored.
bool IsNewerVersion(const std::wstring& candidate, const std::wstring& baseline);

// Runs the GitHub "latest release" check on a detached thread and posts a
// heap-allocated UpdateResult* to `notify` as `msg`'s lParam when finished.
void CheckForUpdateAsync(HWND notify, UINT msg, bool userInitiated);

// Download and hash-verify a release installer after the user opts in.
void DownloadUpdateAsync(HWND notify, UINT msg, std::wstring installerUrl,
                         std::wstring checksumUrl, std::wstring filename);

} // namespace dk
