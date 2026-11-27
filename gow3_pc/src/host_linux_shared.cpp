// host_linux_shared.cpp - the Linux platform functions (host_linux.h) that the runtime and its unit tests share: the
// tests link this file as they link compat_win.c on Windows, without the signal handlers of host_linux.cpp. Windows
// keeps its own bodies (runtime_kernel.c, compat_win.c) and does not build this file.
#include "host_linux.h"

#include <ctime>
#include <filesystem>
#include <system_error>

#include <x86intrin.h>

// The CPU's time stamp counter frequency for GOW3_NATIVE_TSC: TSC ticks across a 100 ms sleep, divided by the time the
// monotonic clock saw pass (Windows measures the same 100 ms against QueryPerformanceCounter).
extern "C" uint64_t host_linux_tsc_hz(void) {
    timespec begin{}, end{};
    clock_gettime(CLOCK_MONOTONIC, &begin);
    const uint64_t start = __rdtsc();
    timespec pause{0, 100000000}, rest{};
    while (nanosleep(&pause, &rest) != 0) pause = rest;
    const uint64_t stop = __rdtsc();
    clock_gettime(CLOCK_MONOTONIC, &end);
    const long double seconds =
        static_cast<long double>(end.tv_sec - begin.tv_sec) + static_cast<long double>(end.tv_nsec - begin.tv_nsec) / 1e9L;
    return static_cast<uint64_t>(static_cast<long double>(stop - start) / seconds);
}

// A folder and everything in it, or a single file (save data folders, test folders).
extern "C" int compat_remove_tree(const char *path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    return ec ? -1 : 0;
}
