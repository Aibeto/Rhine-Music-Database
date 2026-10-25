// Rhine port shim — replaces the SDK's pch.hpp for non-Windows builds.
//
// The upstream header pulls in <Windows.h>, the DXC COM interfaces, d3dcompiler and
// d3d12shader. Here <Windows.h> resolves to our shim (same directory, quoted-include
// search order), the COM/D3D headers resolve to empty stubs, and <process.hpp> comes
// from the SDK's bundled tiny-process-library, which already ships a Unix backend.

#pragma once

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <atlcomcli.h>
#include <dxcapi.h>
#include <d3dcompiler.h>
#include <assert.h>
#include <stdio.h>
// MSVC's <Windows.h> drags in the C math header transitively; ffx_sc.cpp relies on
// ceilf/log2f/pow being visible without including <cmath> itself.
#include <cmath>
#include <cstdint>
#include <exception>
#include <string>
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <iomanip>
#include <bitset>
#include <iostream>
#include <algorithm>
#include <fstream>
#include <d3d12shader.h>

#include <filesystem>
namespace fs = std::filesystem;

#include <process.hpp>
namespace tpl = TinyProcessLib;
