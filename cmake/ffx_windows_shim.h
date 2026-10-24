// POSIX stand-in for <Windows.h>, covering the surface that FidelityFX's Vulkan
// frame-interpolation swapchain uses. It is installed as Windows.h in the build tree's
// override include directory, which sits ahead of the SDK headers, so only the SDK
// translation units that unconditionally include <Windows.h> see it.
//
// Implemented: auto-reset and manual-reset events, threads, the performance counter, and
// a handful of no-op thread-tuning calls. Semaphores are not needed: the swapchain only
// creates Vulkan semaphores.
//
// Semantics that matter and are reproduced deliberately:
//   * CreateEvent(manualReset = FALSE) is an auto-reset event: one waiter consumes the
//     signal. SetEvent therefore notifies one waiter, not all.
//   * WaitForSingleObject on a thread handle joins it.
//   * QueryPerformanceFrequency/QueryPerformanceCounter both use nanoseconds, so any
//     ratio of the two is unit-consistent.

#pragma once
#ifndef RHINE_FFX_WINDOWS_SHIM_H
#define RHINE_FFX_WINDOWS_SHIM_H

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

using BOOL = int;
using DWORD = std::uint32_t;
using LPVOID = void*;
using HANDLE = void*;

#define WINAPI
#define TEXT(literal) literal

#define INFINITE 0xFFFFFFFFu
#define WAIT_OBJECT_0 0x00000000u
#define WAIT_TIMEOUT 0x00000102u
#define WAIT_FAILED 0xFFFFFFFFu
#define THREAD_PRIORITY_HIGHEST 2

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

struct LARGE_INTEGER
{
    long long QuadPart;
};

namespace rhine_ffx_win {

enum class HandleKind
{
    Event,
    Thread
};

struct Handle
{
    HandleKind kind = HandleKind::Event;
    std::mutex mutex;
    std::condition_variable condition;
    bool signaled = false;
    bool manualReset = false;
    std::thread thread;
};

inline Handle* cast(HANDLE handle)
{
    return static_cast<Handle*>(handle);
}

}  // namespace rhine_ffx_win

inline HANDLE CreateEvent(void* /*attributes*/, BOOL manualReset, BOOL initialState, const char* /*name*/)
{
    auto* handle = new rhine_ffx_win::Handle();
    handle->kind = rhine_ffx_win::HandleKind::Event;
    handle->manualReset = manualReset != FALSE;
    handle->signaled = initialState != FALSE;
    return handle;
}

inline BOOL SetEvent(HANDLE handle)
{
    auto* entry = rhine_ffx_win::cast(handle);
    if (entry == nullptr)
        return FALSE;
    {
        std::lock_guard<std::mutex> lock(entry->mutex);
        entry->signaled = true;
    }
    // An auto-reset event releases exactly one waiter.
    if (entry->manualReset)
        entry->condition.notify_all();
    else
        entry->condition.notify_one();
    return TRUE;
}

inline BOOL ResetEvent(HANDLE handle)
{
    auto* entry = rhine_ffx_win::cast(handle);
    if (entry == nullptr)
        return FALSE;
    std::lock_guard<std::mutex> lock(entry->mutex);
    entry->signaled = false;
    return TRUE;
}

inline DWORD WaitForSingleObject(HANDLE handle, DWORD milliseconds)
{
    auto* entry = rhine_ffx_win::cast(handle);
    if (entry == nullptr)
        return WAIT_FAILED;

    if (entry->kind == rhine_ffx_win::HandleKind::Thread)
    {
        if (entry->thread.joinable())
            entry->thread.join();
        return WAIT_OBJECT_0;
    }

    std::unique_lock<std::mutex> lock(entry->mutex);
    const auto ready = [entry] { return entry->signaled; };
    if (milliseconds == INFINITE)
    {
        entry->condition.wait(lock, ready);
    }
    else if (!entry->condition.wait_for(lock, std::chrono::milliseconds(milliseconds), ready))
    {
        return WAIT_TIMEOUT;
    }
    if (!entry->manualReset)
        entry->signaled = false;
    return WAIT_OBJECT_0;
}

inline BOOL CloseHandle(HANDLE handle)
{
    auto* entry = rhine_ffx_win::cast(handle);
    if (entry == nullptr)
        return FALSE;
    if (entry->thread.joinable())
        entry->thread.join();
    delete entry;
    return TRUE;
}

using LPTHREAD_START_ROUTINE = DWORD (*)(LPVOID);

inline HANDLE CreateThread(void* /*attributes*/, std::size_t /*stackSize*/, LPTHREAD_START_ROUTINE start,
                           LPVOID parameter, DWORD /*flags*/, DWORD* /*threadId*/)
{
    if (start == nullptr)
        return nullptr;
    auto* handle = new rhine_ffx_win::Handle();
    handle->kind = rhine_ffx_win::HandleKind::Thread;
    handle->thread = std::thread([start, parameter] { start(parameter); });
    return handle;
}

inline BOOL SetThreadPriority(HANDLE /*handle*/, int /*priority*/)
{
    return TRUE;
}

inline BOOL SetThreadDescription(HANDLE /*handle*/, const wchar_t* /*description*/)
{
    return TRUE;
}

inline void Sleep(DWORD milliseconds)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

// Windows critical sections are recursive, so std::recursive_mutex is the right match.
// The struct is always a default-constructed member, so the mutex needs no explicit
// construction in InitializeCriticalSection.
struct CRITICAL_SECTION
{
    std::recursive_mutex mutex;
};

inline void InitializeCriticalSection(CRITICAL_SECTION* /*section*/) {}

inline void EnterCriticalSection(CRITICAL_SECTION* section)
{
    if (section != nullptr)
        section->mutex.lock();
}

inline void LeaveCriticalSection(CRITICAL_SECTION* section)
{
    if (section != nullptr)
        section->mutex.unlock();
}

inline void DeleteCriticalSection(CRITICAL_SECTION* /*section*/) {}

// Both use nanoseconds so any counter/frequency ratio stays unit-consistent.
inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* frequency)
{
    if (frequency == nullptr)
        return FALSE;
    frequency->QuadPart = 1000000000LL;
    return TRUE;
}

inline BOOL QueryPerformanceCounter(LARGE_INTEGER* counter)
{
    if (counter == nullptr)
        return FALSE;
    counter->QuadPart = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    return TRUE;
}

#endif  // RHINE_FFX_WINDOWS_SHIM_H
