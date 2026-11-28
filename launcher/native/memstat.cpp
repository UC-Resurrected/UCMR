// memstat.cpp - leak hunting aid in uncharted_native.dll. un_memory_monitor spawns a thread that samples the game's
// memory counters, handle / thread / GDI / USER object counts and the system's free memory at a fixed interval and
// appends each sample as a CSV row to memory-<time>.csv, stopping when the game exits. Over a long session a leak
// then appears as a column that only ever grows. Sampling is done from the launcher side, not inside the game.

#include "native.h"
#include "port.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace {

const char *const HEADER =
    "time,seconds,private_mib,working_set_mib,peak_working_set_mib,commit_mib,peak_commit_mib,handles,threads,"
    "gdi_objects,user_objects,pc_free_ram_mib,pc_free_commit_mib,pc_memory_load_pct\n";
const int KEEP_FILES = 10;

// formats the measured columns of one CSV row (the time and seconds columns are added by the caller); returns false
// once the process has exited or its counters are not accessible
bool sample(HANDLE process, DWORD pid, char *row, size_t cap) {
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(process, &exit_code) || exit_code != STILL_ACTIVE) return false;
    PROCESS_MEMORY_COUNTERS_EX mem{};
    mem.cb = sizeof mem;
    if (!K32GetProcessMemoryInfo(process, (PROCESS_MEMORY_COUNTERS *)&mem, sizeof mem)) return false;
    DWORD handles = 0;
    GetProcessHandleCount(process, &handles);
    DWORD gdi = GetGuiResources(process, GR_GDIOBJECTS), user = GetGuiResources(process, GR_USEROBJECTS);
    DWORD threads = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof pe;
    for (BOOL more = snap != INVALID_HANDLE_VALUE && Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe))
        if (pe.th32ProcessID == pid) { threads = pe.cntThreads; break; }
    if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
    MEMORYSTATUSEX sys{};
    sys.dwLength = sizeof sys;
    GlobalMemoryStatusEx(&sys);
    const double MiB = 1024.0 * 1024.0;
    std::snprintf(row, cap, "%.1f,%.1f,%.1f,%.1f,%.1f,%lu,%lu,%lu,%lu,%.1f,%.1f,%lu", mem.PrivateUsage / MiB,
                  mem.WorkingSetSize / MiB, mem.PeakWorkingSetSize / MiB, mem.PagefileUsage / MiB,
                  mem.PeakPagefileUsage / MiB, (unsigned long)handles, (unsigned long)threads, (unsigned long)gdi,
                  (unsigned long)user, sys.ullAvailPhys / MiB, sys.ullAvailPageFile / MiB, (unsigned long)sys.dwMemoryLoad);
    return true;
}

// deletes older memory-*.csv files so that only KEEP_FILES - 1 remain, leaving room for the file about to be created
void trim_old(const std::wstring &folder) {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((folder + L"\\memory-*.csv").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do names.push_back(fd.cFileName);
        while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(names.begin(), names.end());   // timestamped names (memory-YYYYmmdd-HHMMSS.csv) sort chronologically
    for (size_t i = 0; i + (KEEP_FILES - 1) < names.size(); ++i) DeleteFileW((folder + L"\\" + names[i]).c_str());
}

struct Job {
    HANDLE process;
    DWORD pid;
    HANDLE file;
    DWORD interval_ms;
};

DWORD WINAPI run(void *arg) {
    Job *job = (Job *)arg;
    DWORD written;
    WriteFile(job->file, HEADER, (DWORD)std::strlen(HEADER), &written, nullptr);
    ULONGLONG start = GetTickCount64();
    char row[400], line[512];
    for (;;) {
        if (!sample(job->process, job->pid, row, sizeof row)) break;
        std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_s(&local, &now);
        char clock[16];
        std::strftime(clock, sizeof clock, "%H:%M:%S", &local);
        int n = std::snprintf(line, sizeof line, "%s,%llu,%s\n", clock,
                              (unsigned long long)((GetTickCount64() - start) / 1000), row);
        WriteFile(job->file, line, (DWORD)n, &written, nullptr);
        FlushFileBuffers(job->file);
        if (WaitForSingleObject(job->process, job->interval_ms) != WAIT_TIMEOUT) break;   // process handle signalled: exited
    }
    CloseHandle(job->file);
    CloseHandle(job->process);
    delete job;
    return 0;
}

}  // namespace

extern "C" {

// Begins sampling process `pid` every `seconds` into <folder>\memory-<YYYYmmdd-HHMMSS>.csv until it exits; the folder
// is pruned to the 10 most recent such files. On success the CSV path is copied to `out` as UTF-8 and 0 is returned;
// 1 means monitoring could not start, and a negative value is the buffer size `out` would need.
UN_API int un_memory_monitor(unsigned long pid, const un_path_char *folder, double seconds, char *out, int cap) {
    if (seconds <= 0 || !folder) return 1;
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    wchar_t stamp[32];
    wcsftime(stamp, 32, L"%Y%m%d-%H%M%S", &local);
    std::wstring dir = folder, path = dir + L"\\memory-" + stamp + L".csv";
    int n = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n > cap) return -n;
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE, FALSE, pid);
    if (!process) return 1;
    trim_old(dir);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) { CloseHandle(process); return 1; }
    Job *job = new Job{process, pid, file, (DWORD)std::max(250.0, seconds * 1000.0)};
    HANDLE thread = CreateThread(nullptr, 0, run, job, 0, nullptr);
    if (!thread) { CloseHandle(file); CloseHandle(process); delete job; return 1; }
    CloseHandle(thread);
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, out, cap, nullptr, nullptr);
    return 0;
}

}  // extern "C"

#else   // Linux: the same monitor over /proc (docs/WINDOWS-DEPENDENCIES.md §4.1)

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;

// Linux has no commit charge or GDI/USER objects: the columns are the kernel's own counters (kB in /proc, MiB here).
// rss_anon: private memory in RAM (the closest to Windows' private bytes), rss / peak_rss: the working set and its
// peak, vm_data: private data mappings (heap, stacks, anonymous mmap), vm_size / peak_vm_size: the whole address
// space (large for the game: it reserves the PS4's range), fds: open files and sockets (Windows: handles).
const char *const HEADER =
    "time,seconds,rss_anon_mib,rss_mib,peak_rss_mib,vm_data_mib,vm_size_mib,peak_vm_size_mib,fds,threads,"
    "pc_available_ram_mib,pc_free_swap_mib,pc_memory_load_pct\n";
const int KEEP_FILES = 10;

// "Key:   1234 kB" lines of a /proc file, by key
bool proc_values(const std::string &file, std::vector<std::pair<std::string, double>> &values) {
    std::ifstream in(file);
    if (!in) return false;
    for (std::string line; std::getline(in, line);) {
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string key = line.substr(0, colon);
        for (auto &v : values)
            if (v.first == key) v.second = std::strtod(line.c_str() + colon + 1, nullptr);
    }
    return true;
}

// the measured columns of one CSV row; false once the process has exited (or is a zombie waiting for its parent)
bool sample(unsigned long pid, char *row, size_t cap) {
    const std::string proc = "/proc/" + std::to_string(pid);
    std::vector<std::pair<std::string, double>> p = {{"RssAnon", 0}, {"VmRSS", 0}, {"VmHWM", 0}, {"VmData", 0},
                                                     {"VmSize", 0}, {"VmPeak", 0}, {"Threads", 0}};
    std::string state;
    {
        std::ifstream in(proc + "/status");
        if (!in) return false;
        for (std::string line; std::getline(in, line);)
            if (line.rfind("State:", 0) == 0) state = line;
    }
    if (state.find(" Z") != std::string::npos || state.find(" X") != std::string::npos) return false;
    if (!proc_values(proc + "/status", p)) return false;
    std::error_code ec;
    unsigned long fds = 0;
    for (fs::directory_iterator it(proc + "/fd", ec), end; !ec && it != end; it.increment(ec)) ++fds;
    std::vector<std::pair<std::string, double>> m = {{"MemTotal", 0}, {"MemAvailable", 0}, {"SwapFree", 0}};
    proc_values("/proc/meminfo", m);
    const double KiB_per_MiB = 1024.0;
    const unsigned long load = m[0].second > 0 ? (unsigned long)(100.0 - m[1].second * 100.0 / m[0].second + 0.5) : 0;
    std::snprintf(row, cap, "%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%lu,%lu,%.1f,%.1f,%lu", p[0].second / KiB_per_MiB,
                  p[1].second / KiB_per_MiB, p[2].second / KiB_per_MiB, p[3].second / KiB_per_MiB,
                  p[4].second / KiB_per_MiB, p[5].second / KiB_per_MiB, fds, (unsigned long)p[6].second,
                  m[1].second / KiB_per_MiB, m[2].second / KiB_per_MiB, load);
    return true;
}

// deletes older memory-*.csv files so that only KEEP_FILES - 1 remain, leaving room for the file about to be created
void trim_old(const fs::path &folder) {
    std::vector<fs::path> names;
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string n = it->path().filename().string();
        if (n.rfind("memory-", 0) == 0 && n.size() > 11 && n.compare(n.size() - 4, 4, ".csv") == 0) names.push_back(it->path());
    }
    std::sort(names.begin(), names.end());   // timestamped names (memory-YYYYmmdd-HHMMSS.csv) sort chronologically
    for (size_t i = 0; i + (KEEP_FILES - 1) < names.size(); ++i) fs::remove(names[i], ec);
}

void run(unsigned long pid, std::FILE *file, std::chrono::milliseconds interval) {
    std::fputs(HEADER, file);
    const auto start = std::chrono::steady_clock::now();
    char row[400];
    for (;;) {
        if (!sample(pid, row, sizeof row)) break;
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_r(&now, &local);
        char clock[16];
        std::strftime(clock, sizeof clock, "%H:%M:%S", &local);
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start);
        std::fprintf(file, "%s,%llu,%s\n", clock, (unsigned long long)elapsed.count(), row);
        std::fflush(file);
        // wait for the next sample, but end soon after the process does (Windows waits on the process handle)
        const auto next = std::chrono::steady_clock::now() + interval;
        while (std::chrono::steady_clock::now() < next && port::process_alive((long long)pid))
            std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(
                std::chrono::milliseconds(100), next - std::chrono::steady_clock::now()));
        if (!port::process_alive((long long)pid)) break;
    }
    std::fclose(file);
}

}  // namespace

extern "C" {

// Begins sampling process `pid` every `seconds` into <folder>/memory-<YYYYmmdd-HHMMSS>.csv until it exits (see the
// Windows body above); 0 with the CSV path in `out`, 1 when monitoring could not start, negative: the size `out` needs.
UN_API int un_memory_monitor(unsigned long pid, const un_path_char *folder, double seconds, char *out, int cap) {
    if (seconds <= 0 || !folder) return 1;
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &local);
    const fs::path dir = port::path_utf8(folder), path = dir / (std::string("memory-") + stamp + ".csv");
    const std::string text = port::utf8(path);
    if ((int)text.size() + 1 > cap) return -(int)(text.size() + 1);
    if (!port::process_alive((long long)pid)) return 1;   // Windows: OpenProcess failed
    trim_old(dir);
    std::FILE *file = std::fopen(text.c_str(), "wb");
    if (!file) return 1;
    try {
        std::thread(run, pid, file, std::chrono::milliseconds((long long)std::max(250.0, seconds * 1000.0))).detach();
    } catch (...) {
        std::fclose(file);
        return 1;
    }
    std::memcpy(out, text.c_str(), text.size() + 1);
    return 0;
}

}  // extern "C"

#endif
