// Rhine port shim — minimal Windows API surface for FidelityFX's ffx_shader_compiler.
//
// Upstream ffx_shader_compiler is Windows-only: it pulls in <Windows.h>, <pathcch.h>,
// the DXC COM interfaces and d3dcompiler. Only the GLSL/glslang path is reachable from
// the Vulkan backend, so this header implements exactly the handful of helpers that the
// GLSL code path actually calls (path canonicalisation, directory creation, wide fopen)
// and nothing else. The HLSL compiler is stubbed out separately (hlsl_compiler.h).
//
// Paths are normalised to absolute forward-slash form: the tool hands them straight to
// std::filesystem (glsl_compiler.cpp appends "/<name>_temp"), which on POSIX treats a
// backslash as an ordinary filename character. Directory creation therefore happens in
// PathAllocCanonicalize rather than in ffx_sc.cpp's Win32 component walk.

#pragma once
#ifndef RHINE_FFX_WIN_SHIM_H
#define RHINE_FFX_WIN_SHIM_H

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

using HRESULT = long;
using ULONG   = unsigned long;
using PWSTR   = wchar_t*;
using PCWSTR  = const wchar_t*;
using BOOL    = int;
using HMODULE = void*;

#define S_OK         ((HRESULT)0)
#define E_FAIL       ((HRESULT)0x80004005L)
#define E_INVALIDARG ((HRESULT)0x80070057L)

#define CP_UTF8 65001

// ffx_sc.cpp defines `int wmain(int, wchar_t**)`. Rename it here so entry.cpp can own the
// real process entry point and forward a converted argv.
#define wmain rhine_ffx_sc_wmain

// pathcch.h flags — the shim ignores them, but ffx_sc.cpp passes them by name.
#define PATHCCH_ALLOW_LONG_PATHS 0x00000001UL
#define PATHCCH_ENSURE_TRAILING_SLASH 0x00000004UL

namespace rhine_ffx_shim {

inline std::string toUtf8(const std::wstring& w)
{
    std::string out;
    out.reserve(w.size());
    for (wchar_t wc : w)
    {
        const unsigned int cp = static_cast<unsigned int>(wc);
        if (cp < 0x80)
        {
            out.push_back(static_cast<char>(cp));
        }
        else if (cp < 0x800)
        {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else if (cp < 0x10000)
        {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else
        {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

inline std::wstring fromUtf8(const std::string& s)
{
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();)
    {
        const unsigned char lead = static_cast<unsigned char>(s[i]);
        unsigned int        cp   = lead;
        int                 len  = 1;
        if (lead < 0x80)
        {
            cp = lead;
        }
        else if ((lead & 0xE0) == 0xC0)
        {
            cp  = lead & 0x1F;
            len = 2;
        }
        else if ((lead & 0xF0) == 0xE0)
        {
            cp  = lead & 0x0F;
            len = 3;
        }
        else
        {
            cp  = lead & 0x07;
            len = 4;
        }
        for (int k = 1; k < len && i + static_cast<size_t>(k) < s.size(); ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        i += static_cast<size_t>(len);
        out.push_back(static_cast<wchar_t>(cp));
    }
    return out;
}

/// Backslash form -> real filesystem path.
inline std::string toNativePath(const std::wstring& w)
{
    std::string s = toUtf8(w);
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

inline wchar_t* allocWide(const std::wstring& s)
{
    wchar_t* p = static_cast<wchar_t*>(std::malloc((s.size() + 1) * sizeof(wchar_t)));
    if (!p)
        return nullptr;
    std::memcpy(p, s.c_str(), (s.size() + 1) * sizeof(wchar_t));
    return p;
}

/// Absolute, forward-slash separated, `.`/`..` resolved, optionally with a trailing
/// separator, and created on disk.
///
/// Upstream keeps everything backslash separated because the Win32 component walk in
/// LaunchParameters::EnsureOutputPathExistsAndMakeCanonical splits on '\\'. On POSIX the
/// tool also hands these strings straight to std::filesystem (glsl_compiler.cpp appends
/// "/<name>_temp"), so backslashes would become literal filename characters. Forward
/// slashes satisfy std::filesystem, and the directory tree is created here instead of by
/// the (now unreachable) component walk.
inline std::wstring canonicalize(const std::wstring& in, bool trailingSeparator)
{
    std::string narrow = toUtf8(in);
    std::replace(narrow.begin(), narrow.end(), '\\', '/');
    std::filesystem::path p(narrow);
    if (!p.is_absolute())
        p = std::filesystem::current_path() / p;
    std::string normalised = p.lexically_normal().string();
    if (normalised.empty() || normalised.back() != '/')
    {
        if (trailingSeparator)
            normalised.push_back('/');
    }
    std::error_code ec;
    std::filesystem::create_directories(normalised, ec);
    return fromUtf8(normalised);
}

}  // namespace rhine_ffx_shim

inline HRESULT PathAllocCanonicalize(PCWSTR path, ULONG /*flags*/, PWSTR* out)
{
    if (!path || !out)
        return E_INVALIDARG;
    // ffx_sc.cpp always passes ENSURE_TRAILING_SLASH; honour it unconditionally.
    const std::wstring canonical = rhine_ffx_shim::canonicalize(std::wstring(path), true);
    *out                         = rhine_ffx_shim::allocWide(canonical);
    return *out ? S_OK : E_FAIL;
}

inline HRESULT PathCchSkipRoot(PCWSTR path, PWSTR* rootEnd)
{
    if (!path || !rootEnd)
        return E_INVALIDARG;
    const wchar_t* p = path;
    while (*p == L'/' || *p == L'\\')
        ++p;
    *rootEnd = const_cast<wchar_t*>(p);
    return S_OK;
}

inline HRESULT PathAllocCombine(PCWSTR base, PCWSTR more, ULONG /*flags*/, PWSTR* out)
{
    if (!out)
        return E_INVALIDARG;
    std::wstring lhs = base ? base : L"";
    std::wstring rhs = more ? more : L"";
    std::replace(lhs.begin(), lhs.end(), L'\\', L'/');
    std::replace(rhs.begin(), rhs.end(), L'\\', L'/');
    if (!lhs.empty() && lhs.back() != L'/')
        lhs.push_back(L'/');
    while (!rhs.empty() && rhs.front() == L'/')
        rhs.erase(rhs.begin());
    *out = rhine_ffx_shim::allocWide(lhs + rhs);
    return *out ? S_OK : E_FAIL;
}

inline void LocalFree(void* p)
{
    std::free(p);
}

inline BOOL CreateDirectoryW(PCWSTR path, void* /*security*/)
{
    if (!path)
        return 0;
    std::error_code ec;
    std::filesystem::create_directories(rhine_ffx_shim::toNativePath(path), ec);
    return ec ? 0 : 1;
}

inline int _wfopen_s(FILE** file, PCWSTR path, PCWSTR mode)
{
    if (!file || !path || !mode)
        return -1;
    const std::string native = rhine_ffx_shim::toNativePath(path);
    const std::string modeN  = rhine_ffx_shim::toUtf8(mode);
    *file                    = std::fopen(native.c_str(), modeN.c_str());
    return *file ? 0 : -1;
}

inline int WideCharToMultiByte(unsigned int /*codepage*/, unsigned long /*flags*/, PCWSTR in, int inLen,
                               char* out, int outLen, const char* /*defaultChar*/, BOOL* /*usedDefault*/)
{
    const std::string utf8 = rhine_ffx_shim::toUtf8(in ? std::wstring(in, inLen < 0 ? wcslen(in) : size_t(inLen)) : std::wstring());
    if (!out || outLen <= 0)
        return static_cast<int>(utf8.size());
    const int n = std::min<int>(outLen, static_cast<int>(utf8.size()));
    std::memcpy(out, utf8.data(), size_t(n));
    return n;
}

inline int MultiByteToWideChar(unsigned int /*codepage*/, unsigned long /*flags*/, const char* in, int inLen,
                               wchar_t* out, int outLen)
{
    const std::wstring wide =
        rhine_ffx_shim::fromUtf8(in ? std::string(in, inLen < 0 ? std::strlen(in) : size_t(inLen)) : std::string());
    if (!out || outLen <= 0)
        return static_cast<int>(wide.size());
    const int n = std::min<int>(outLen, static_cast<int>(wide.size()));
    std::memcpy(out, wide.data(), size_t(n) * sizeof(wchar_t));
    return n;
}

#endif  // RHINE_FFX_WIN_SHIM_H
