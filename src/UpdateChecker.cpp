// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dolu <https://github.com/DoluTattoo>

#include "UpdateChecker.h"

#include <winhttp.h>

#include <algorithm>
#include <cstring>
#include <thread>

// Injected by CMake from the project version / release tag; falls back to a
// sentinel so the app still builds (and reports "an update is available") if it
// is ever compiled without the definition.
#ifndef DOCK_VERSION
#define DOCK_VERSION "0.0.0"
#endif

namespace
{
constexpr wchar_t kApiHost[] = L"api.github.com";
constexpr wchar_t kApiPath[] = L"/repos/Mesrine67/dock/releases/latest";
// Stable page that always redirects to the newest release.
constexpr wchar_t kReleasePage[] = L"https://github.com/Mesrine67/dock/releases/latest";

std::wstring Widen(const std::string& s)
{
    if (s.empty())
        return std::wstring();
    const int n =
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Reads the JSON string literal whose opening quote is at `openQuote`, undoing
// backslash escapes. Enough for GitHub's well-formed release payload.
std::string ReadJsonStringAt(const std::string& json, size_t openQuote)
{
    std::string out;
    for (size_t i = openQuote + 1; i < json.size(); ++i)
    {
        const char c = json[i];
        if (c == '\\' && i + 1 < json.size())
        {
            out.push_back(json[++i]); // keep the escaped character verbatim
            continue;
        }
        if (c == '"')
            break;
        out.push_back(c);
    }
    return out;
}

// Returns the value of the first string field named `key`.
std::string ExtractJsonString(const std::string& json, const char* key)
{
    const std::string needle = std::string("\"") + key + "\"";
    const size_t      k      = json.find(needle);
    if (k == std::string::npos)
        return std::string();
    const size_t colon = json.find(':', k + needle.size());
    if (colon == std::string::npos)
        return std::string();
    const size_t open = json.find('"', colon + 1);
    if (open == std::string::npos)
        return std::string();
    return ReadJsonStringAt(json, open);
}

// Returns the release's installer asset URL: the "browser_download_url" whose
// value ends in "setup.exe" (case-insensitive). Empty if none is present.
std::string ExtractSetupAssetUrl(const std::string& json)
{
    const std::string key = "\"browser_download_url\"";
    auto endsWith = [](const std::string& s, const char* suffix) {
        const size_t n = std::strlen(suffix);
        if (s.size() < n)
            return false;
        return _stricmp(s.c_str() + (s.size() - n), suffix) == 0;
    };

    size_t pos = 0;
    while ((pos = json.find(key, pos)) != std::string::npos)
    {
        const size_t colon = json.find(':', pos + key.size());
        const size_t open  = (colon == std::string::npos) ? std::string::npos
                                                           : json.find('"', colon + 1);
        if (open == std::string::npos)
            break;
        const std::string value = ReadJsonStringAt(json, open);
        if (endsWith(value, "setup.exe"))
            return value;
        pos = open + 1;
    }
    return std::string();
}

// Owns a WinHTTP handle and closes it on scope exit (no manual cleanup paths).
class WinHttpHandle
{
public:
    explicit WinHttpHandle(HINTERNET h = nullptr) : h_(h) {}
    ~WinHttpHandle()
    {
        if (h_)
            WinHttpCloseHandle(h_);
    }
    WinHttpHandle(const WinHttpHandle&)            = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    operator HINTERNET() const { return h_; }
    explicit operator bool() const { return h_ != nullptr; }

private:
    HINTERNET h_;
};

// Performs one HTTPS GET. On a 200 response, `sink(data, len)` is called for each
// received chunk (returning false from it aborts). Returns false on any error or
// non-200 status; follows GitHub's default https->https redirects.
template <class Sink>
bool HttpsGet(const wchar_t* host, INTERNET_PORT port, const wchar_t* path,
              const wchar_t* extraHeaders, DWORD receiveTimeoutMs, Sink&& sink)
{
    WinHttpHandle session(WinHttpOpen(L"dock-update-check",
                                      WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session)
        return false;
    // Don't let a slow or unreachable server hang the worker thread for long.
    WinHttpSetTimeouts(session, 8000, 8000, 8000, static_cast<int>(receiveTimeoutMs));

    WinHttpHandle connect(WinHttpConnect(session, host, port, 0));
    if (!connect)
        return false;
    WinHttpHandle request(WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request)
        return false;
    if (extraHeaders)
        WinHttpAddRequestHeaders(request, extraHeaders, static_cast<DWORD>(-1),
                                 WINHTTP_ADDREQ_FLAG_ADD);

    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0,
                            0, 0) ||
        !WinHttpReceiveResponse(request, nullptr))
        return false;

    DWORD status = 0;
    DWORD len    = sizeof(status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                             WINHTTP_NO_HEADER_INDEX) ||
        status != 200)
        return false;

    for (;;)
    {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request, &avail))
            return false;
        if (avail == 0)
            return true; // clean end of stream
        std::vector<char> buf(avail);
        DWORD             read = 0;
        if (!WinHttpReadData(request, buf.data(), avail, &read) || read == 0)
            return false;
        if (!sink(buf.data(), read))
            return false;
    }
}

// Downloads an arbitrary HTTPS URL to `dest`, removing any partial file on error.
bool DownloadToFile(const std::wstring& url, const std::wstring& dest)
{
    URL_COMPONENTS uc{};
    uc.dwStructSize      = sizeof(uc);
    wchar_t host[256]    = {};
    wchar_t path[3072]   = {};
    wchar_t extra[2048]  = {};
    uc.lpszHostName      = host;
    uc.dwHostNameLength  = ARRAYSIZE(host);
    uc.lpszUrlPath       = path;
    uc.dwUrlPathLength   = ARRAYSIZE(path);
    uc.lpszExtraInfo     = extra;
    uc.dwExtraInfoLength = ARRAYSIZE(extra);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc))
        return false;

    HANDLE file = CreateFileW(dest.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    const std::wstring fullPath = std::wstring(path) + extra;
    const bool         ok =
        HttpsGet(host, uc.nPort, fullPath.c_str(), nullptr, 30000, // generous receive for the binary
                 [file](const char* data, DWORD n) {
                     DWORD written = 0;
                     return WriteFile(file, data, n, &written, nullptr) && written == n;
                 });
    CloseHandle(file);
    if (!ok)
        DeleteFileW(dest.c_str());
    return ok;
}
} // namespace

namespace dk
{
std::wstring CurrentVersionW()
{
    return Widen(DOCK_VERSION);
}

bool IsNewerVersion(const std::wstring& candidate, const std::wstring& baseline)
{
    auto parse = [](const std::wstring& v) {
        std::vector<int> parts;
        int              cur = 0;
        for (wchar_t ch : v)
        {
            if (ch >= L'0' && ch <= L'9')
                cur = cur * 10 + (ch - L'0');
            else if (ch == L'.')
            {
                parts.push_back(cur);
                cur = 0;
            }
            // Anything else (a leading 'v', a pre-release suffix, ...) is ignored.
        }
        parts.push_back(cur);
        return parts;
    };

    const std::vector<int> a = parse(candidate);
    const std::vector<int> b = parse(baseline);
    const size_t           n = std::max(a.size(), b.size());
    for (size_t i = 0; i < n; ++i)
    {
        const int ai = i < a.size() ? a[i] : 0;
        const int bi = i < b.size() ? b[i] : 0;
        if (ai != bi)
            return ai > bi;
    }
    return false; // equal
}

void CheckForUpdateAsync(HWND notify, UINT msg, bool userInitiated)
{
    std::thread([notify, msg, userInitiated] {
        // GitHub's API needs COM-free WinHTTP only; no apartment init required.
        auto* result          = new UpdateResult();
        result->userInitiated = userInitiated;
        result->current       = CurrentVersionW();
        result->url           = kReleasePage;

        std::string body;
        const bool  fetched = HttpsGet(kApiHost, INTERNET_DEFAULT_HTTPS_PORT, kApiPath,
                                      L"Accept: application/vnd.github+json\r\n", 5000,
                                      [&body](const char* data, DWORD n) {
                                          body.append(data, n);
                                          return true;
                                      });
        if (fetched)
        {
            const std::string tag = ExtractJsonString(body, "tag_name");
            if (!tag.empty())
            {
                result->latest      = Widen(tag);
                result->available   = IsNewerVersion(result->latest, result->current);
                result->downloadUrl = Widen(ExtractSetupAssetUrl(body));
            }
            else
            {
                result->failed = true; // unexpected payload
            }
        }
        else
        {
            result->failed = true; // network error / non-200
        }

        if (!PostMessageW(notify, msg, 0, reinterpret_cast<LPARAM>(result)))
            delete result; // the window is gone; don't leak
    }).detach();
}

void DownloadUpdateAsync(HWND notify, UINT msg, std::wstring url)
{
    std::thread([notify, msg, url] {
        auto* res = new DownloadResult();

        wchar_t tmp[MAX_PATH] = {};
        if (GetTempPathW(MAX_PATH, tmp) != 0)
        {
            const std::wstring dest = std::wstring(tmp) + L"dock-update-setup.exe";
            if (!url.empty() && DownloadToFile(url, dest))
            {
                res->ok   = true;
                res->path = dest;
            }
        }

        if (!PostMessageW(notify, msg, 0, reinterpret_cast<LPARAM>(res)))
            delete res; // the window is gone; don't leak
    }).detach();
}
} // namespace dk
