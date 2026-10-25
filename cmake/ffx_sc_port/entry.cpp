// Rhine port shim — process entry point for non-Windows builds.
//
// ffx_sc.cpp keeps its upstream `int wmain(int argc, wchar_t** argv)` signature; the
// Windows.h shim renames that symbol to rhine_ffx_sc_wmain. This translation unit
// provides the real main(), converts the narrow argv the C runtime hands us into the
// wide form the tool expects, and forwards.

#include "pch.hpp"

#include <string>
#include <vector>

int rhine_ffx_sc_wmain(int argc, wchar_t** argv);

int main(int argc, char** argv)
{
    std::vector<std::wstring> storage;
    storage.reserve(size_t(argc > 0 ? argc : 0));
    for (int i = 0; i < argc; ++i)
        storage.push_back(rhine_ffx_shim::fromUtf8(argv[i] ? argv[i] : ""));

    std::vector<wchar_t*> wideArgv;
    wideArgv.reserve(storage.size() + 1);
    for (std::wstring& argument : storage)
        wideArgv.push_back(const_cast<wchar_t*>(argument.c_str()));
    wideArgv.push_back(nullptr);

    return rhine_ffx_sc_wmain(argc, wideArgv.data());
}
