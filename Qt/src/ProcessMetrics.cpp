#include "ProcessMetrics.h"
#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#else
#include <mach/mach.h>
#include <sys/resource.h>
#endif

ProcessMetrics readProcessMetrics() {
    ProcessMetrics result;
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS memory{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory))) {
        result.residentBytes = qint64(memory.WorkingSetSize);
    }
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        const auto nanoseconds = [](FILETIME value) {
            return qint64((quint64(value.dwHighDateTime) << 32) | value.dwLowDateTime) * 100;
        };
        result.cpuNanoseconds = nanoseconds(kernel) + nanoseconds(user);
    }
#else
    mach_task_basic_info memory{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&memory), &count) ==
        KERN_SUCCESS) {
        result.residentBytes = qint64(memory.resident_size);
    }
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        result.cpuNanoseconds = (qint64(usage.ru_utime.tv_sec) + usage.ru_stime.tv_sec) * 1000000000 +
                                (qint64(usage.ru_utime.tv_usec) + usage.ru_stime.tv_usec) * 1000;
    }
#endif
    return result;
}
