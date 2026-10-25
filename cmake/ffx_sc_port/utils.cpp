// Rhine port shim — replaces the SDK's utils.cpp for non-Windows builds.
//
// The upstream implementation is a thin wrapper over WideCharToMultiByte and
// MultiByteToWideChar. Windows.h already provides portable equivalents, so this file
// just forwards to them and keeps the same two entry points.

#include "utils.h"

std::string WCharToUTF8(const std::wstring& wstr)
{
    if (wstr.empty())
        return std::string();

    const int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), int(wstr.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return std::string();

    std::string str(size_t(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), int(wstr.size()), str.data(), size, nullptr, nullptr);
    return str;
}

std::wstring UTF8ToWChar(const std::string& str)
{
    if (str.empty())
        return std::wstring();

    const int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), int(str.size()), nullptr, 0);
    if (size <= 0)
        return std::wstring();

    std::wstring wstr(size_t(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), int(str.size()), wstr.data(), size);
    return wstr;
}
