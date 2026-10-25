// Build-time compatibility shim for compiling the FidelityFX SDK with GCC/Clang.
//
// This header is force-included (-include) into FidelityFX SDK translation units
// only; the vendored submodule is never modified. It supplies the small set of
// MSVC-only CRT helpers that sdk 3.0.4 assumes (see cmake/FidelityFX.cmake).
#pragma once

#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif

// sdk/src/shared/ffx_message.cpp uses FFX_UNUSED without including ffx_util.h; on Windows
// it arrives through <windows.h>. Same expansion as the SDK's own definition, so a later
// include of ffx_util.h is not a conflicting redefinition.
#ifndef FFX_UNUSED
#define FFX_UNUSED(x) ((void)(x))
#endif

// MSVC secure-CRT string formatting, used by sdk/src/shared/ffx_breadcrumbs_list.h.
// Mirrors the Windows contract: truncation is an error, and the buffer is emptied
// rather than left partially written.
inline int sprintf_s(char* buffer, std::size_t sizeOfBuffer, const char* format, ...)
{
    if (buffer == nullptr || sizeOfBuffer == 0 || format == nullptr) {
        return -1;
    }
    va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(buffer, sizeOfBuffer, format, args);
    va_end(args);
    if (written < 0 || static_cast<std::size_t>(written) >= sizeOfBuffer) {
        buffer[0] = '\0';
        return -1;
    }
    return written;
}

template <std::size_t N>
inline int strcpy_s(char (&dst)[N], const char* src)
{
    if (src == nullptr) {
        return EINVAL;
    }
    std::strncpy(dst, src, N);
    dst[N - 1] = '\0';
    return 0;
}

// Pointer + length overloads: ffx_vk.cpp's debug-marker path passes a raw buffer with an
// explicit element count rather than an array reference.
inline int wcscpy_s(wchar_t* dst, std::size_t size, const wchar_t* src)
{
    if (dst == nullptr || size == 0) {
        return EINVAL;
    }
    if (src == nullptr) {
        dst[0] = L'\0';
        return EINVAL;
    }
    std::wcsncpy(dst, src, size);
    dst[size - 1] = L'\0';
    return 0;
}

inline int strcpy_s(char* dst, std::size_t size, const char* src)
{
    if (dst == nullptr || size == 0) {
        return EINVAL;
    }
    if (src == nullptr) {
        dst[0] = '\0';
        return EINVAL;
    }
    std::strncpy(dst, src, size);
    dst[size - 1] = '\0';
    return 0;
}

// MSVC secure-CRT array overloads used by the SDK (e.g. wcscpy_s(dst, L"name")).
template <std::size_t N>
inline int wcscpy_s(wchar_t (&dst)[N], const wchar_t* src)
{
    if (src == nullptr) {
        return EINVAL;
    }
    std::wcsncpy(dst, src, N);
    dst[N - 1] = L'\0';
    return 0;
}

template <std::size_t N>
inline int wcscat_s(wchar_t (&dst)[N], const wchar_t* src)
{
    if (src == nullptr) {
        return EINVAL;
    }
    const std::size_t len = std::wcslen(dst);
    if (len >= N) {
        return EINVAL;
    }
    std::wcsncat(dst, src, N - len - 1);
    dst[N - 1] = L'\0';
    return 0;
}

// libstdc++ 16 removed std::wstring_convert; the SDK only uses it to widen
// shader binding names, so decode UTF-8 into wchar_t directly.
struct RhineFfxWideConverter
{
    std::wstring from_bytes(const char* text) const
    {
        std::wstring out;
        if (text == nullptr) {
            return out;
        }
        const auto* cursor = reinterpret_cast<const unsigned char*>(text);
        while (*cursor != 0) {
            std::uint32_t codePoint = 0;
            int trailing = 0;
            if (*cursor < 0x80) {
                codePoint = *cursor++;
            } else if ((*cursor & 0xE0) == 0xC0) {
                codePoint = *cursor++ & 0x1Fu;
                trailing = 1;
            } else if ((*cursor & 0xF0) == 0xE0) {
                codePoint = *cursor++ & 0x0Fu;
                trailing = 2;
            } else if ((*cursor & 0xF8) == 0xF0) {
                codePoint = *cursor++ & 0x07u;
                trailing = 3;
            } else {
                ++cursor;
                continue;
            }
            for (int index = 0; index < trailing; ++index) {
                if ((*cursor & 0xC0) != 0x80) {
                    codePoint = 0xFFFDu;
                    break;
                }
                codePoint = (codePoint << 6) | (*cursor++ & 0x3Fu);
            }
            out.push_back(static_cast<wchar_t>(codePoint));
        }
        return out;
    }

    std::string to_bytes(const std::wstring& text) const
    {
        std::string out;
        out.reserve(text.size());
        for (wchar_t wc : text) {
            auto codePoint = static_cast<std::uint32_t>(wc);
            if (codePoint < 0x80) {
                out.push_back(static_cast<char>(codePoint));
            } else if (codePoint < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
                out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
            } else if (codePoint < 0x10000) {
                out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
                out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
            } else {
                out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
                out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
            }
        }
        return out;
    }

    std::string to_bytes(const wchar_t* text) const
    {
        return to_bytes(text == nullptr ? std::wstring() : std::wstring(text));
    }
};
