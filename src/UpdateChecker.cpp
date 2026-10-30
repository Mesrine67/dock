// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dolu <https://github.com/DoluTattoo>

#include "UpdateChecker.h"

#include <winhttp.h>

#include <algorithm>
#include <bcrypt.h>
#include <array>
#include <cwctype>
#include <thread>
#include <vector>
#include <cctype>
#include <utility>

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

// Extract a named release asset from GitHub's release metadata.
std::wstring ReleaseAsset(const std::string& json, const std::string& assetName)
{
    // GitHub's JSON usually has whitespace after colons.
    size_t p = json.find("\"name\"");
    while (p != std::string::npos)
    {
        const size_t colon = json.find(':', p + 6);
        if (colon == std::string::npos) break;
        const size_t quote = json.find('"', colon + 1);
        if (quote == std::string::npos) break;
        if (ReadJsonStringAt(json, quote) == assetName)
        {
            const size_t after = json.find("\"browser_download_url\"", quote + 1);
            const size_t next = json.find("\"name\"", quote + 1);
            if (after == std::string::npos || (next != std::string::npos && next < after))
                return L"";
            const size_t assetColon = json.find(':', after);
            const size_t assetQuote = json.find('"', assetColon + 1);
            return assetQuote == std::string::npos ? L"" :
                Widen(ReadJsonStringAt(json, assetQuote));
        }
        p = json.find("\"name\"", quote + 1);
    }
    return L"";
}

bool TrustedReleaseAsset(const std::wstring& url, const std::wstring& expectedName)
{
    // The release's origin and path are fixed, not derived from arbitrary API URLs.
    const std::wstring prefix = L"https://github.com/Mesrine67/dock/releases/download/";
    if (url.compare(0, prefix.size(), prefix) != 0)
        return false;
    const size_t slash = url.find(L'/', prefix.size());
    if (slash == std::wstring::npos)
        return false;
    const auto tag = url.substr(prefix.size(), slash - prefix.size());
    if (tag.size() < 2 || tag[0] != L'v')
        return false;
    for (const auto c : tag.substr(1))
        if (!iswdigit(c) && c != L'.')
            return false;
    return url.substr(slash + 1) == expectedName;
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

// GET an explicitly constrained GitHub release asset over HTTPS.
template<class Sink>
bool GetAsset(const std::wstring& url, Sink&& sink)
{
    URL_COMPONENTSW u{};
    u.dwStructSize = sizeof(u);
    wchar_t host[256]{};
    wchar_t path[2048]{};
    wchar_t extra[1024]{};
    u.lpszHostName = host; u.dwHostNameLength = ARRAYSIZE(host);
    u.lpszUrlPath = path; u.dwUrlPathLength = ARRAYSIZE(path);
    u.lpszExtraInfo = extra; u.dwExtraInfoLength = ARRAYSIZE(extra);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &u) || u.nScheme != INTERNET_SCHEME_HTTPS ||
        _wcsicmp(host, L"github.com") != 0 || u.nPort != INTERNET_DEFAULT_HTTPS_PORT)
        return false;
    const std::wstring target = std::wstring(path) + extra;
    return HttpsGet(host, u.nPort, target.c_str(), nullptr, 30000,
                    std::forward<Sink>(sink));
}

bool HexDigestEquals(const std::vector<unsigned char>& bytes, const std::string& expected)
{
    if (expected.size() != 64)
        return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::array<unsigned char, 32> digest{};
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0;
    if (ok) ok = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0;
    if (ok) ok = BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()),
                               static_cast<ULONG>(bytes.size()), 0) == 0;
    if (ok) ok = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return false;
    static constexpr char hex[] = "0123456789abcdef";
    for (size_t i=0;i<digest.size();++i)
        if (tolower(static_cast<unsigned char>(expected[2*i])) != hex[digest[i] >> 4] ||
            tolower(static_cast<unsigned char>(expected[2*i+1])) != hex[digest[i] & 15])
            return false;
    return true;
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
                result->filename = L"dock-" + Widen(tag.substr(tag[0] == 'v' ? 1 : 0)) + L"-setup.exe";
                result->downloadUrl = ReleaseAsset(body, std::string(result->filename.begin(), result->filename.end()));
                result->checksumUrl = ReleaseAsset(body, "SHA256SUMS.txt");
                if (!TrustedReleaseAsset(result->downloadUrl, result->filename) ||
                    !TrustedReleaseAsset(result->checksumUrl, L"SHA256SUMS.txt"))
                {
                    result->downloadUrl.clear();
                    result->checksumUrl.clear();
                }
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

void DownloadUpdateAsync(HWND notify, UINT msg, std::wstring installerUrl,
                         std::wstring checksumUrl, std::wstring filename)
{
    std::thread([notify, msg, installerUrl, checksumUrl, filename] {
        auto* result = new DownloadResult();
        if (TrustedReleaseAsset(installerUrl, filename) &&
            TrustedReleaseAsset(checksumUrl, L"SHA256SUMS.txt"))
        {
            std::string checksums;
            const bool gotChecksums = GetAsset(checksumUrl, [&checksums](const char* p, DWORD n) {
                if (checksums.size() + n > 32768) return false;
                checksums.append(p,n);
                return true;
            });
            std::string expected;
            if (gotChecksums)
            {
                auto offset = checksums.find(std::string(filename.begin(),filename.end()));
                if (offset != std::string::npos)
                {
                    size_t line = checksums.rfind('\n', offset);
                    line = (line == std::string::npos) ? 0 : line+1;
                    if (offset >= line+66)
                        expected = checksums.substr(line,64);
                }
            }
            std::vector<unsigned char> binary;
            const bool downloaded = expected.size() == 64 &&
                GetAsset(installerUrl, [&binary](const char* p, DWORD n) {
                    if (binary.size() + n > 128 * 1024 * 1024) return false;
                    binary.insert(binary.end(), p, p+n);
                    return true;
                });
            if (downloaded && HexDigestEquals(binary, expected))
            {
                wchar_t temp[MAX_PATH]{};
                if (GetTempPathW(MAX_PATH, temp))
                {
                    wchar_t dir[MAX_PATH]{};
                    if (GetTempFileNameW(temp, L"dku", 0, dir))
                    {
                        DeleteFileW(dir);
                        if (CreateDirectoryW(dir, nullptr))
                        {
                            result->path = std::wstring(dir) + L"\\dock-verified-update.exe";
                            HANDLE file = CreateFileW(result->path.c_str(), GENERIC_WRITE, 0,
                                nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
                            if (file != INVALID_HANDLE_VALUE)
                            {
                                DWORD written = 0;
                                result->ok = WriteFile(file, binary.data(),
                                    static_cast<DWORD>(binary.size()), &written, nullptr) &&
                                    written == binary.size();
                                CloseHandle(file);
                                if (!result->ok) DeleteFileW(result->path.c_str());
                            }
                        }
                    }
                }
            }
        }
        if (!PostMessageW(notify, msg, 0, reinterpret_cast<LPARAM>(result)))
        {
            if (result->ok) DeleteFileW(result->path.c_str());
            delete result;
        }
    }).detach();
}

} // namespace dk
