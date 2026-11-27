// hang.cpp - diagnostics for a game that stopped responding (built into uncharted_native.dll). The watchdog that the
// game starter runs (start.cpp) relies on it, and so do the debugging tools. All inspection happens from the
// launching process, never inside the game, which keeps it usable even when every game thread is blocked.
//
// un_thread_report: dumps all game threads, merging threads whose stacks are identical. Each group lists thread ids
//   and names, the host frames as module+offset (unwound by StackWalk64 using the PE unwind data, so no PDBs are
//   needed) and, when present, the guest frames (guest code preserves rbp, so they are found by chasing the rbp
//   chain; printed as offsets from the guest image base).
// un_minidump: writes a Windows minidump holding the stacks and registers of all threads.

#include "native.h"
#include "port.h"

#include <cstdlib>
#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <mutex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

const int MAX_FRAMES = 28;

struct Module {
    uint64_t base, size;
    std::string name;
};

std::string utf8(const std::wstring &w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::string hex(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "0x%llx", (unsigned long long)v);
    return b;
}

std::vector<Module> modules_of(HANDLE process) {
    std::vector<Module> out;
    std::vector<HMODULE> list(1024);
    DWORD needed = 0;
    if (!K32EnumProcessModulesEx(process, list.data(), (DWORD)(list.size() * sizeof(HMODULE)), &needed, LIST_MODULES_ALL))
        return out;
    list.resize(std::min<size_t>(list.size(), needed / sizeof(HMODULE)));
    for (HMODULE m : list) {
        MODULEINFO info{};
        wchar_t name[MAX_PATH];
        if (!K32GetModuleInformation(process, m, &info, sizeof info)) continue;
        DWORD n = K32GetModuleBaseNameW(process, m, name, MAX_PATH);
        out.push_back({(uint64_t)info.lpBaseOfDll, info.SizeOfImage, n ? utf8(std::wstring(name, n)) : "?"});
    }
    return out;
}

std::string place(uint64_t address, const std::vector<Module> &modules, uint64_t guest, uint64_t guest_size) {
    if (guest && address >= guest && address < guest + guest_size) return "guest " + hex(address - guest);
    for (const Module &m : modules)
        if (address >= m.base && address < m.base + m.size) return m.name + "+" + hex(address - m.base);
    return hex(address);
}

std::string stub_target(HANDLE process, uint64_t address, const std::vector<Module> &modules, uint64_t guest,
                        uint64_t guest_size) {
    unsigned char bytes[40] = {};
    const uint64_t start = address >= 16 ? address - 16 : 0;
    SIZE_T got = 0;
    if (!ReadProcessMemory(process, (LPCVOID)start, bytes, sizeof bytes, &got)) return std::string();
    for (int i = 0; i <= 16; ++i) {
        if (bytes[i] != 0x48 || bytes[i + 1] != 0xb8 || bytes[i + 12] != 0xff || bytes[i + 13] != 0xe0) continue;
        uint64_t target = 0;
        std::memcpy(&target, bytes + i + 2, sizeof target);
        return " [near stub " + hex(start + i) + " -> " + place(target, modules, guest, guest_size) + "]";
    }
    return std::string();
}

std::string code_bytes(HANDLE process, uint64_t address) {
    unsigned char bytes[64] = {};
    SIZE_T got = 0;
    if (!ReadProcessMemory(process, (LPCVOID)address, bytes, sizeof bytes, &got) || !got) return std::string();
    std::string out = " bytes";
    char part[4];
    for (SIZE_T i = 0; i < got && i < sizeof bytes; ++i) {
        std::snprintf(part, sizeof part, " %02x", bytes[i]);
        out += part;
    }
    return out;
}

std::string thread_name(DWORD id) {
    typedef HRESULT(WINAPI * Describe)(HANDLE, PWSTR *);
    static Describe describe = (Describe)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");
    if (!describe) return std::string();
    HANDLE t = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id);
    if (!t) return std::string();
    PWSTR text = nullptr;
    std::string out;
    if (SUCCEEDED(describe(t, &text)) && text) {
        out = utf8(text);
        LocalFree(text);
    }
    CloseHandle(t);
    return out;
}

struct Stack {
    std::vector<std::string> native, game, regs;
    bool operator<(const Stack &o) const {
        if (native != o.native) return native < o.native;
        if (game != o.game) return game < o.game;
        return regs < o.regs;
    }
};

std::string reg(uint64_t v, const std::vector<Module> &modules, uint64_t guest, uint64_t guest_size) {
    return place(v, modules, guest, guest_size);
}

void read_bytes(HANDLE process, uint64_t address, void *out, size_t size) {
    SIZE_T got = 0;
    if (!ReadProcessMemory(process, (LPCVOID)address, out, size, &got) || got != size) std::memset(out, 0, size);
}

std::string loop_state(HANDLE process, const CONTEXT &ctx, const std::vector<Module> &modules, uint64_t guest,
                       uint64_t guest_size) {
    const uint64_t off = ctx.Rip - guest;
    if (!guest || off < 0x16dcd0 || off > 0x16dce1) return std::string();
    uint32_t node[2] = {}, block[4] = {}, entry_word = 0, field48 = 0;
    uint16_t flags = 0;
    uint64_t entry = 0;
    read_bytes(process, ctx.Rax, node, sizeof node);
    read_bytes(process, ctx.R12, block, sizeof block);
    read_bytes(process, ctx.R13 + 8, &entry, sizeof entry);
    if (entry) read_bytes(process, entry, &entry_word, sizeof entry_word);
    read_bytes(process, ctx.R14 + ctx.Rbx + 0x12, &flags, sizeof flags);
    read_bytes(process, ctx.R14 + ctx.Rbx + 0x48, &field48, sizeof field48);
    char b[640];
    std::snprintf(b, sizeof b,
                  "loop: rax=%s [rax]=%#x [rax+4]=%#x ecx=%#x edx=%#x r12=%s [r12]=%#x r14=%s rbx=%#llx "
                  "entry=%s [entry]=%#x type=%#x flags=%#x field48=%#x block=%08x %08x %08x %08x",
                  reg(ctx.Rax, modules, guest, guest_size).c_str(), node[0], node[1], (unsigned)ctx.Rcx,
                  (unsigned)ctx.Rdx, reg(ctx.R12, modules, guest, guest_size).c_str(), block[0],
                  reg(ctx.R14, modules, guest, guest_size).c_str(), (unsigned long long)ctx.Rbx,
                  reg(entry, modules, guest, guest_size).c_str(), entry_word, (unsigned)(entry_word >> 24), flags, field48,
                  block[0], block[1], block[2], block[3]);
    return b;
}

Stack walk(HANDLE process, HANDLE thread, const std::vector<Module> &modules, uint64_t guest, uint64_t guest_size) {
    Stack s;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &ctx)) return s;
    // guest frames: each rbp points at {caller's rbp, return address}; stop when the chain stops going up the stack
    uint64_t rbp = ctx.Rbp;
    if (guest && ctx.Rip >= guest && ctx.Rip < guest + guest_size) {
        s.game.push_back(hex(ctx.Rip - guest));
        const std::string loop = loop_state(process, ctx, modules, guest, guest_size);
        if (!loop.empty()) {
            char b[512];
            std::snprintf(b, sizeof b,
                          "regs: rip=%s rsp=%s rbp=%s rax=%s rbx=%s rcx=%s rdx=%s rsi=%s rdi=%s "
                          "r8=%s r9=%s r10=%s r11=%s r12=%s r13=%s r14=%s r15=%s",
                          reg(ctx.Rip, modules, guest, guest_size).c_str(),
                          reg(ctx.Rsp, modules, guest, guest_size).c_str(), reg(ctx.Rbp, modules, guest, guest_size).c_str(),
                          reg(ctx.Rax, modules, guest, guest_size).c_str(), reg(ctx.Rbx, modules, guest, guest_size).c_str(),
                          reg(ctx.Rcx, modules, guest, guest_size).c_str(), reg(ctx.Rdx, modules, guest, guest_size).c_str(),
                          reg(ctx.Rsi, modules, guest, guest_size).c_str(), reg(ctx.Rdi, modules, guest, guest_size).c_str(),
                          reg(ctx.R8, modules, guest, guest_size).c_str(), reg(ctx.R9, modules, guest, guest_size).c_str(),
                          reg(ctx.R10, modules, guest, guest_size).c_str(), reg(ctx.R11, modules, guest, guest_size).c_str(),
                          reg(ctx.R12, modules, guest, guest_size).c_str(), reg(ctx.R13, modules, guest, guest_size).c_str(),
                          reg(ctx.R14, modules, guest, guest_size).c_str(), reg(ctx.R15, modules, guest, guest_size).c_str());
            s.regs.push_back(b);
            s.regs.push_back(loop);
        }
    }
    if (!s.game.empty() || (guest && ctx.Rip < guest) || !guest) {
        uint64_t words[128] = {};
        SIZE_T got = 0;
        if (ReadProcessMemory(process, (LPCVOID)ctx.Rsp, words, sizeof words, &got) && got) {
            int found = 0;
            for (size_t i = 0; i < got / sizeof words[0] && found < 8; ++i) {
                if (guest && words[i] >= guest && words[i] < guest + guest_size) {
                    s.game.push_back("stack " + hex(words[i] - guest));
                    ++found;
                }
            }
        }
    }
    for (int i = 0; i < MAX_FRAMES && rbp; ++i) {
        uint64_t pair[2];
        SIZE_T got = 0;
        if (!ReadProcessMemory(process, (LPCVOID)rbp, pair, sizeof pair, &got) || got != sizeof pair) break;
        if (guest && pair[1] >= guest && pair[1] < guest + guest_size) s.game.push_back(hex(pair[1] - guest));
        if (pair[0] <= rbp) break;
        rbp = pair[0];
    }
    s.native.push_back("current " + place(ctx.Rip, modules, guest, guest_size));
    // host frames via dbghelp's unwinder
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    for (int i = 0; i < MAX_FRAMES; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &ctx, nullptr, SymFunctionTableAccess64,
                         SymGetModuleBase64, nullptr) || !frame.AddrPC.Offset)
            break;
        std::string label = place(frame.AddrPC.Offset, modules, guest, guest_size);
        if (!label.empty() && label[0] == '0') {
            label += stub_target(process, frame.AddrPC.Offset, modules, guest, guest_size);
            label += code_bytes(process, frame.AddrPC.Offset);
        }
        s.native.push_back(label);
    }
    return s;
}

std::string joined(const std::vector<std::string> &parts, const char *sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) out += (i ? sep : "") + parts[i];
    return out;
}

int give(const std::string &text, char *out, int cap) {
    if ((int)text.size() + 1 > cap) return -(int)(text.size() + 1);
    std::memcpy(out, text.c_str(), text.size() + 1);
    return 0;
}

}  // namespace

extern "C" {

UN_API int un_thread_report(unsigned long pid, unsigned long long guest_base, unsigned long long guest_size, char *out,
                            int cap) {
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process) return give("  (could not open the game process)", out, cap);
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    bool symbols = SymInitializeW(process, nullptr, TRUE) != FALSE;
    std::vector<Module> modules = modules_of(process);
    std::map<Stack, std::vector<std::string>> groups;
    std::vector<Stack> order;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    te.dwSize = sizeof te;
    for (BOOL more = snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te); more; more = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!t) continue;
        Stack s;
        if (SuspendThread(t) != (DWORD)-1) {
            s = walk(process, t, modules, guest_base, guest_size);
            ResumeThread(t);
        }
        CloseHandle(t);
        std::string name = thread_name(te.th32ThreadID);
        std::string who = std::to_string(te.th32ThreadID) + (name.empty() ? "" : " " + name);
        if (!groups.count(s)) order.push_back(s);
        groups[s].push_back(who);
    }
    if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
    if (symbols) SymCleanup(process);
    CloseHandle(process);
    std::string text;
    for (const Stack &s : order) {
        const std::vector<std::string> &who = groups[s];
        std::vector<std::string> shown(who.begin(), who.begin() + std::min<size_t>(who.size(), 6));
        text += "  threads " + joined(shown, ", ") +
                (who.size() > 6 ? " (+" + std::to_string(who.size() - 6) + " more)" : std::string()) + ":\n";
        text += "    host: " + (s.native.empty() ? std::string("(stack unavailable)") : joined(s.native, " <- ")) + "\n";
        if (!s.game.empty()) text += "    guest: " + joined(s.game, " <- ") + "\n";
        for (const std::string &r : s.regs) text += "    " + r + "\n";
    }
    return give(text, out, cap);
}

}  // extern "C"

namespace {

// ------------------------------------------------------------------------------------------------ freeze watchdog

struct Watchdog {
    HANDLE process;
    std::wstring log, dumps, root;
    int seconds;
    std::mutex lock;
    std::vector<std::wstring> reports;
};
std::mutex watchdogs_lock;
std::vector<Watchdog *> watchdogs;   // never freed; the game starter may query any watchdog's reports until exit

std::wstring parent_of(const std::wstring &p) {
    size_t cut = p.find_last_of(L"\\/");
    return cut == std::wstring::npos ? L"." : p.substr(0, cut);
}

std::wstring env_text(const wchar_t *name) {
    wchar_t buf[256];
    DWORD n = GetEnvironmentVariableW(name, buf, 256);
    return (n && n < 256) ? std::wstring(buf, n) : std::wstring();
}

bool exists(const std::wstring &p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// full paths of the plain files matching folder\pattern, in case-insensitive name order
std::vector<std::wstring> sorted_files(const std::wstring &folder, const wchar_t *pattern) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((folder + L"\\" + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(folder + L"\\" + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const std::wstring &a, const std::wstring &b) {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    return out;
}

// writes one line straight to the starter's stdout (which is its log), bypassing any buffering
void say(const std::string &line) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) return;
    std::string text = line + "\n";
    DWORD put = 0;
    WriteFile(out, text.data(), (DWORD)text.size(), &put, nullptr);
}

std::string report_text(DWORD pid, uint64_t guest_base) {
    std::vector<char> buf(1 << 18);
    for (;;) {
        int status = un_thread_report(pid, guest_base, guest_base ? 0x20000000ULL : 0, buf.data(), (int)buf.size());
        if (status >= 0) return buf.data();
        buf.resize((size_t)-status);
    }
}

void freeze_report(Watchdog *w, uint64_t guest_base) {
    char stamp[32];
    time_t now = time(nullptr);
    struct tm local;
    localtime_s(&local, &now);
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &local);
    std::vector<std::string> lines = {"Hang watchdog: freeze detected at " + std::string(stamp) + ", nothing rendered for " +
                                      std::to_string(w->seconds) + " s. Per-thread stacks:"};
    ULONGLONG started = GetTickCount64();
    std::string text = report_text(GetProcessId(w->process), guest_base);
    for (size_t at = 0; at < text.size();) {   // split at '\n'; a trailing newline does not add an empty line
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        lines.push_back(text.substr(at, end - at));
        at = end + 1;
    }
    char took[64];
    std::snprintf(took, sizeof took, "(collecting the stacks took %.1f s)", (GetTickCount64() - started) / 1000.0);
    lines.push_back(took);
    // Minidumps run to hundreds of MB, so one is only written in a source checkout (no VERSION file) or when
    // UNCHARTED_HANG_DUMP=1 asks for it
    std::wstring dump_env = env_text(L"UNCHARTED_HANG_DUMP");
    SetLastError(0);
    if (!GetEnvironmentVariableW(L"UNCHARTED_HANG_DUMP", nullptr, 0) && GetLastError() == ERROR_ENVVAR_NOT_FOUND) dump_env = exists(w->root + L"\\VERSION") ? L"0" : L"1";
    if (dump_env == L"1") {
        CreateDirectoryW(parent_of(w->dumps).c_str(), nullptr);
        CreateDirectoryW(w->dumps.c_str(), nullptr);
        std::vector<std::wstring> old = sorted_files(w->dumps, L"hang-*.dmp");
        for (size_t i = 0; i + 1 < old.size(); ++i) DeleteFileW(old[i].c_str());   // keep 1 old dump + the new one = 2
        std::wstring dump = w->dumps + L"\\hang-" + std::wstring(stamp, stamp + std::strlen(stamp)) + L".dmp";
        lines.push_back(un_minidump(GetProcessId(w->process), dump.c_str()) ? "Hang watchdog: minidump written to " + utf8(dump)
                                                                            : "Hang watchdog: could not write a minidump");
    }
    std::wstring folder = parent_of(w->log);
    std::vector<std::wstring> old = sorted_files(folder, L"hang-*.txt");
    for (size_t i = 0; i + 9 < old.size(); ++i) DeleteFileW(old[i].c_str());   // keep 9 old reports + the new one = 10
    std::wstring report = folder + L"\\hang-" + std::wstring(stamp, stamp + std::strlen(stamp)) + L".txt";
    std::string body;
    for (auto &l : lines) body += l + "\n";
    HANDLE f = CreateFileW(report.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD put = 0;
        WriteFile(f, body.data(), (DWORD)body.size(), &put, nullptr);
        CloseHandle(f);
    }
    {
        std::lock_guard<std::mutex> hold(w->lock);
        w->reports.push_back(report);
    }
    say("Hang watchdog: the game froze; report " + utf8(report));
}

// Polls the game log. While rendering, the runtime prints a "Frame stats" line every 5 s, so if the process is still
// alive and no such line has appeared for `seconds`, the game is treated as frozen. One report is written per
// freeze; the watchdog arms itself again once frame lines resume.
DWORD WINAPI watch(void *arg) {
    Watchdog *w = (Watchdog *)arg;
    uint64_t position = 0, guest_base = 0;
    bool armed = true, seen = false, have_base = false;
    ULONGLONG last_frame = GetTickCount64();
    while (WaitForSingleObject(w->process, 2000) == WAIT_TIMEOUT) {
        HANDLE f = CreateFileW(w->log.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;
        std::string chunk;
        LARGE_INTEGER size{}, to{};
        to.QuadPart = (LONGLONG)position;
        if (GetFileSizeEx(f, &size) && (uint64_t)size.QuadPart > position && SetFilePointerEx(f, to, nullptr, FILE_BEGIN)) {
            chunk.resize((size_t)((uint64_t)size.QuadPart - position));
            DWORD got = 0;
            size_t done = 0;
            while (done < chunk.size() &&
                   ReadFile(f, &chunk[done], (DWORD)std::min<size_t>(chunk.size() - done, 1 << 24), &got, nullptr) && got)
                done += got;
            chunk.resize(done);
        }
        CloseHandle(f);
        position += chunk.size();
        if (!have_base) {
            size_t at = chunk.find("Guest image base: ");
            if (at != std::string::npos) {
                size_t s = at + 18, e = s;
                while (e < chunk.size() && std::isxdigit((unsigned char)chunk[e])) ++e;
                if (e > s) { guest_base = std::strtoull(chunk.substr(s, e - s).c_str(), nullptr, 16); have_base = true; }
            }
        }
        if (chunk.find("Frame stats") != std::string::npos) {
            last_frame = GetTickCount64();
            armed = seen = true;
            continue;
        }
        if (!(seen && armed) || GetTickCount64() - last_frame < (ULONGLONG)w->seconds * 1000 ||
            WaitForSingleObject(w->process, 0) != WAIT_TIMEOUT)
            continue;
        armed = false;
        freeze_report(w, have_base ? guest_base : 0);
    }
    return 0;
}

}  // namespace

extern "C" {

// Launches a watchdog thread for process `pid`. Arguments: the game log to poll, the folder for minidumps, how many
// seconds without frames mean frozen, and the launcher root (a release package has a VERSION file there, which turns
// minidumps off unless UNCHARTED_HANG_DUMP=1). Returns a watchdog id > 0, or 0 on failure.
UN_API int un_hang_watchdog(unsigned long pid, const un_path_char *log, const un_path_char *dumps, int seconds, const un_path_char *root) {
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return 0;
    Watchdog *w = new Watchdog{process, port::wtext(log, L""), port::wtext(dumps, L""), port::wtext(root, L"."), seconds, {}, {}};
    int id;
    {
        std::lock_guard<std::mutex> hold(watchdogs_lock);
        watchdogs.push_back(w);
        id = (int)watchdogs.size();
    }
    HANDLE thread = CreateThread(nullptr, 0, watch, w, 0, nullptr);
    if (!thread) return 0;
    CloseHandle(thread);
    return id;
}

// paths of every report this watchdog has produced so far, newline-separated
UN_API int un_hang_reports(int id, char *out, int cap) {
    Watchdog *w = nullptr;
    {
        std::lock_guard<std::mutex> hold(watchdogs_lock);
        if (id >= 1 && id <= (int)watchdogs.size()) w = watchdogs[id - 1];
    }
    std::string text;
    if (w) {
        std::lock_guard<std::mutex> hold(w->lock);
        for (auto &r : w->reports) text += utf8(r) + "\n";
    }
    return give(text, out, cap);
}

UN_API int un_minidump(unsigned long pid, const un_path_char *path) {
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE, FALSE, pid);
    if (!process) return 0;
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    BOOL ok = FALSE;
    if (file != INVALID_HANDLE_VALUE) {
        ok = MiniDumpWriteDump(process, pid, file,
                               (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithProcessThreadData |
                                               MiniDumpWithHandleData),
                               nullptr, nullptr, nullptr);
        CloseHandle(file);
        if (!ok) DeleteFileW(path);
    }
    CloseHandle(process);
    return ok ? 1 : 0;
}

}  // extern "C"

#else   // Linux: the same report and watchdog over /proc and ptrace (docs/WINDOWS-DEPENDENCIES.md §4.1)

// The thread report attaches to each game thread in turn (PTRACE_SEIZE + PTRACE_INTERRUPT: the thread stops where it
// is, as SuspendThread stops it on Windows), reads its registers, detaches, and reads the game's memory with
// process_vm_readv. The game starter is the game's parent, which Linux's default ptrace rule (Yama scope 1) allows.
// Host frames come from the rbp chain like the guest frames (the runtime is built with frame pointers; Windows uses
// the PE unwind data instead). There is no minidump on Linux: a core dump of a running game needs gdb (gcore).

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <csignal>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;

const int MAX_FRAMES = 28;

struct Module {
    uint64_t base, size;
    std::string name;
};

std::string hex(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "0x%llx", (unsigned long long)v);
    return b;
}

// the game's mapped files (from /proc/<pid>/maps): each file from its lowest to its highest mapped address, named by
// its file name as Windows names a module
std::vector<Module> modules_of(pid_t pid) {
    std::map<std::string, std::pair<uint64_t, uint64_t>> spans;
    std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
    for (std::string line; std::getline(maps, line);) {
        unsigned long long start = 0, end = 0;
        if (std::sscanf(line.c_str(), "%llx-%llx", &start, &end) != 2) continue;
        const size_t slash = line.find('/');
        if (slash == std::string::npos) continue;
        const std::string path = line.substr(slash);
        auto it = spans.find(path);
        if (it == spans.end()) spans[path] = {start, end};
        else it->second = {std::min<uint64_t>(it->second.first, start), std::max<uint64_t>(it->second.second, end)};
    }
    std::vector<Module> out;
    for (auto &s : spans) out.push_back({s.second.first, s.second.second - s.second.first, fs::path(s.first).filename().string()});
    return out;
}

std::string place(uint64_t address, const std::vector<Module> &modules, uint64_t guest, uint64_t guest_size) {
    if (guest && address >= guest && address < guest + guest_size) return "guest " + hex(address - guest);
    for (const Module &m : modules)
        if (address >= m.base && address < m.base + m.size) return m.name + "+" + hex(address - m.base);
    return hex(address);
}

// bytes of the game's memory; how many could be read (0 when none)
size_t read_some(pid_t pid, uint64_t address, void *out, size_t size) {
    struct iovec local = {out, size}, remote = {(void *)(uintptr_t)address, size};
    const ssize_t got = process_vm_readv(pid, &local, 1, &remote, 1, 0);
    return got > 0 ? (size_t)got : 0;
}

void read_bytes(pid_t pid, uint64_t address, void *out, size_t size) {
    if (read_some(pid, address, out, size) != size) std::memset(out, 0, size);
}

std::string stub_target(pid_t pid, uint64_t address, const std::vector<Module> &modules, uint64_t guest, uint64_t guest_size) {
    unsigned char bytes[40] = {};
    const uint64_t start = address >= 16 ? address - 16 : 0;
    if (!read_some(pid, start, bytes, sizeof bytes)) return std::string();
    for (int i = 0; i <= 16; ++i) {
        if (bytes[i] != 0x48 || bytes[i + 1] != 0xb8 || bytes[i + 12] != 0xff || bytes[i + 13] != 0xe0) continue;
        uint64_t target = 0;
        std::memcpy(&target, bytes + i + 2, sizeof target);
        return " [near stub " + hex(start + i) + " -> " + place(target, modules, guest, guest_size) + "]";
    }
    return std::string();
}

std::string code_bytes(pid_t pid, uint64_t address) {
    unsigned char bytes[64] = {};
    const size_t got = read_some(pid, address, bytes, sizeof bytes);
    if (!got) return std::string();
    std::string out = " bytes";
    char part[4];
    for (size_t i = 0; i < got; ++i) {
        std::snprintf(part, sizeof part, " %02x", bytes[i]);
        out += part;
    }
    return out;
}

std::string thread_name(pid_t pid, pid_t tid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/task/" + std::to_string(tid) + "/comm");
    std::string name;
    std::getline(f, name);
    return name;
}

struct Stack {
    std::vector<std::string> native, game, regs;
    bool operator<(const Stack &o) const {
        if (native != o.native) return native < o.native;
        if (game != o.game) return game < o.game;
        return regs < o.regs;
    }
};

std::string reg(uint64_t v, const std::vector<Module> &modules, uint64_t guest, uint64_t guest_size) {
    return place(v, modules, guest, guest_size);
}

std::string loop_state(pid_t pid, const user_regs_struct &ctx, const std::vector<Module> &modules, uint64_t guest,
                       uint64_t guest_size) {
    const uint64_t off = ctx.rip - guest;
    if (!guest || off < 0x16dcd0 || off > 0x16dce1) return std::string();
    uint32_t node[2] = {}, block[4] = {}, entry_word = 0, field48 = 0;
    uint16_t flags = 0;
    uint64_t entry = 0;
    read_bytes(pid, ctx.rax, node, sizeof node);
    read_bytes(pid, ctx.r12, block, sizeof block);
    read_bytes(pid, ctx.r13 + 8, &entry, sizeof entry);
    if (entry) read_bytes(pid, entry, &entry_word, sizeof entry_word);
    read_bytes(pid, ctx.r14 + ctx.rbx + 0x12, &flags, sizeof flags);
    read_bytes(pid, ctx.r14 + ctx.rbx + 0x48, &field48, sizeof field48);
    char b[640];
    std::snprintf(b, sizeof b,
                  "loop: rax=%s [rax]=%#x [rax+4]=%#x ecx=%#x edx=%#x r12=%s [r12]=%#x r14=%s rbx=%#llx "
                  "entry=%s [entry]=%#x type=%#x flags=%#x field48=%#x block=%08x %08x %08x %08x",
                  reg(ctx.rax, modules, guest, guest_size).c_str(), node[0], node[1], (unsigned)ctx.rcx,
                  (unsigned)ctx.rdx, reg(ctx.r12, modules, guest, guest_size).c_str(), block[0],
                  reg(ctx.r14, modules, guest, guest_size).c_str(), (unsigned long long)ctx.rbx,
                  reg(entry, modules, guest, guest_size).c_str(), entry_word, (unsigned)(entry_word >> 24), flags, field48,
                  block[0], block[1], block[2], block[3]);
    return b;
}

// one stopped thread's stack, from its registers (the Windows walk() with the rbp chain for the host frames)
Stack walk(pid_t pid, const user_regs_struct &ctx, const std::vector<Module> &modules, uint64_t guest, uint64_t guest_size) {
    Stack s;
    uint64_t rbp = ctx.rbp;
    if (guest && ctx.rip >= guest && ctx.rip < guest + guest_size) {
        s.game.push_back(hex(ctx.rip - guest));
        const std::string loop = loop_state(pid, ctx, modules, guest, guest_size);
        if (!loop.empty()) {
            char b[512];
            std::snprintf(b, sizeof b,
                          "regs: rip=%s rsp=%s rbp=%s rax=%s rbx=%s rcx=%s rdx=%s rsi=%s rdi=%s "
                          "r8=%s r9=%s r10=%s r11=%s r12=%s r13=%s r14=%s r15=%s",
                          reg(ctx.rip, modules, guest, guest_size).c_str(),
                          reg(ctx.rsp, modules, guest, guest_size).c_str(), reg(ctx.rbp, modules, guest, guest_size).c_str(),
                          reg(ctx.rax, modules, guest, guest_size).c_str(), reg(ctx.rbx, modules, guest, guest_size).c_str(),
                          reg(ctx.rcx, modules, guest, guest_size).c_str(), reg(ctx.rdx, modules, guest, guest_size).c_str(),
                          reg(ctx.rsi, modules, guest, guest_size).c_str(), reg(ctx.rdi, modules, guest, guest_size).c_str(),
                          reg(ctx.r8, modules, guest, guest_size).c_str(), reg(ctx.r9, modules, guest, guest_size).c_str(),
                          reg(ctx.r10, modules, guest, guest_size).c_str(), reg(ctx.r11, modules, guest, guest_size).c_str(),
                          reg(ctx.r12, modules, guest, guest_size).c_str(), reg(ctx.r13, modules, guest, guest_size).c_str(),
                          reg(ctx.r14, modules, guest, guest_size).c_str(), reg(ctx.r15, modules, guest, guest_size).c_str());
            s.regs.push_back(b);
            s.regs.push_back(loop);
        }
    }
    if (!s.game.empty() || (guest && ctx.rip < guest) || !guest) {
        uint64_t words[128] = {};
        const size_t got = read_some(pid, ctx.rsp, words, sizeof words);
        int found = 0;
        for (size_t i = 0; i < got / sizeof words[0] && found < 8; ++i)
            if (guest && words[i] >= guest && words[i] < guest + guest_size) {
                s.game.push_back("stack " + hex(words[i] - guest));
                ++found;
            }
    }
    std::vector<uint64_t> returns;   // the rbp chain's return addresses, for the guest and the host frames
    for (int i = 0; i < MAX_FRAMES && rbp; ++i) {
        uint64_t pair[2];
        if (read_some(pid, rbp, pair, sizeof pair) != sizeof pair) break;
        if (guest && pair[1] >= guest && pair[1] < guest + guest_size) s.game.push_back(hex(pair[1] - guest));
        returns.push_back(pair[1]);
        if (pair[0] <= rbp) break;
        rbp = pair[0];
    }
    s.native.push_back("current " + place(ctx.rip, modules, guest, guest_size));
    returns.insert(returns.begin(), ctx.rip);
    for (size_t i = 0; i < returns.size() && (int)i < MAX_FRAMES && returns[i]; ++i) {
        std::string label = place(returns[i], modules, guest, guest_size);
        if (!label.empty() && label[0] == '0') {
            label += stub_target(pid, returns[i], modules, guest, guest_size);
            label += code_bytes(pid, returns[i]);
        }
        s.native.push_back(label);
    }
    return s;
}

// stops one thread, reads its registers, lets it go on; false when it could not be stopped (gone, or not allowed). A
// signal that reached the thread while it was being stopped is reported instead of the interrupt: it is handed back
// to the thread when it is let go, so nothing the game expects is lost.
bool stopped_registers(pid_t tid, user_regs_struct &regs, int &error) {
    if (ptrace(PTRACE_SEIZE, tid, nullptr, nullptr) != 0) { error = errno; return false; }
    bool ok = ptrace(PTRACE_INTERRUPT, tid, nullptr, nullptr) == 0;
    int status = 0, signal = 0;
    if (ok) ok = waitpid(tid, &status, __WALL) == tid && WIFSTOPPED(status);
    if (ok && (status >> 16) == 0 && WSTOPSIG(status) != SIGTRAP) signal = WSTOPSIG(status);   // a signal-delivery stop
    if (ok) ok = ptrace(PTRACE_GETREGS, tid, nullptr, &regs) == 0;
    if (!ok) error = errno;
    ptrace(PTRACE_DETACH, tid, nullptr, (void *)(intptr_t)signal);
    return ok;
}

std::string joined(const std::vector<std::string> &parts, const char *sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) out += (i ? sep : "") + parts[i];
    return out;
}

int give(const std::string &text, char *out, int cap) {
    if ((int)text.size() + 1 > cap) return -(int)(text.size() + 1);
    std::memcpy(out, text.c_str(), text.size() + 1);
    return 0;
}

}  // namespace

extern "C" {

UN_API int un_thread_report(unsigned long pid, unsigned long long guest_base, unsigned long long guest_size, char *out,
                            int cap) {
    const std::string task = "/proc/" + std::to_string(pid) + "/task";
    std::vector<pid_t> tids;
    std::error_code ec;
    for (fs::directory_iterator it(task, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string n = it->path().filename().string();
        if (!n.empty() && std::all_of(n.begin(), n.end(), ::isdigit)) tids.push_back((pid_t)std::stol(n));
    }
    if (tids.empty()) return give("  (could not open the game process)", out, cap);
    std::sort(tids.begin(), tids.end());
    const std::vector<Module> modules = modules_of((pid_t)pid);
    std::map<Stack, std::vector<std::string>> groups;
    std::vector<Stack> order;
    int refused = 0, error = 0;
    for (pid_t tid : tids) {
        Stack s;
        user_regs_struct regs{};
        if (stopped_registers(tid, regs, error)) s = walk((pid_t)pid, regs, modules, guest_base, guest_size);
        else ++refused;
        const std::string name = thread_name((pid_t)pid, tid);
        const std::string who = std::to_string(tid) + (name.empty() ? "" : " " + name);
        if (!groups.count(s)) order.push_back(s);
        groups[s].push_back(who);
    }
    std::string text;
    if (refused == (int)tids.size())
        text = "  (the game's threads could not be stopped: " + std::string(std::strerror(error)) +
               "; Linux allows it to the process that started the game unless /proc/sys/kernel/yama/ptrace_scope is 2 or 3)\n";
    for (const Stack &s : order) {
        const std::vector<std::string> &who = groups[s];
        std::vector<std::string> shown(who.begin(), who.begin() + std::min<size_t>(who.size(), 6));
        text += "  threads " + joined(shown, ", ") +
                (who.size() > 6 ? " (+" + std::to_string(who.size() - 6) + " more)" : std::string()) + ":\n";
        text += "    host: " + (s.native.empty() ? std::string("(stack unavailable)") : joined(s.native, " <- ")) + "\n";
        if (!s.game.empty()) text += "    guest: " + joined(s.game, " <- ") + "\n";
        for (const std::string &r : s.regs) text += "    " + r + "\n";
    }
    return give(text, out, cap);
}

}  // extern "C"

namespace {

// ------------------------------------------------------------------------------------------------ freeze watchdog

struct Watchdog {
    pid_t pid;
    fs::path log, dumps, root;
    int seconds;
    std::mutex lock;
    std::vector<std::string> reports;
};
std::mutex watchdogs_lock;
std::vector<Watchdog *> watchdogs;   // never freed; the game starter may query any watchdog's reports until exit

// the game is still running: alive and not a zombie waiting for the starter to collect it
bool running(pid_t pid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/stat");
    std::string stat;
    if (!std::getline(f, stat)) return false;
    const size_t close = stat.rfind(')');
    return close != std::string::npos && close + 2 < stat.size() && stat[close + 2] != 'Z' && stat[close + 2] != 'X';
}

// full paths of the plain files in `folder` whose names begin with `prefix` and end with `suffix`, in
// case-insensitive name order (as the Windows body sorts them)
std::vector<fs::path> sorted_files(const fs::path &folder, const std::string &prefix, const std::string &suffix) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string n = it->path().filename().string();
        std::error_code e2;
        if (it->is_regular_file(e2) && n.size() >= prefix.size() + suffix.size() && n.compare(0, prefix.size(), prefix) == 0 &&
            n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0)
            out.push_back(it->path());
    }
    auto lower = [](std::string s) { for (char &c : s) c = (char)std::tolower((unsigned char)c); return s; };
    std::sort(out.begin(), out.end(), [&](const fs::path &a, const fs::path &b) { return lower(a.string()) < lower(b.string()); });
    return out;
}

// writes one line straight to the starter's stdout (which is its log), bypassing any buffering
void say(const std::string &line) {
    const std::string text = line + "\n";
    if (write(1, text.data(), text.size()) < 0) {}
}

std::string report_text(pid_t pid, uint64_t guest_base) {
    std::vector<char> buf(1 << 18);
    for (;;) {
        int status = un_thread_report((unsigned long)pid, guest_base, guest_base ? 0x20000000ULL : 0, buf.data(), (int)buf.size());
        if (status >= 0) return buf.data();
        buf.resize((size_t)-status);
    }
}

void freeze_report(Watchdog *w, uint64_t guest_base) {
    char stamp[32];
    time_t now = time(nullptr);
    struct tm local;
    localtime_r(&now, &local);
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &local);
    std::vector<std::string> lines = {"Hang watchdog: freeze detected at " + std::string(stamp) + ", nothing rendered for " +
                                      std::to_string(w->seconds) + " s. Per-thread stacks:"};
    const auto started = std::chrono::steady_clock::now();
    std::string text = report_text(w->pid, guest_base);
    for (size_t at = 0; at < text.size();) {   // split at '\n'; a trailing newline does not add an empty line
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        lines.push_back(text.substr(at, end - at));
        at = end + 1;
    }
    char took[64];
    std::snprintf(took, sizeof took, "(collecting the stacks took %.1f s)",
                  std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    lines.push_back(took);
    // Windows writes a minidump in a source checkout (no VERSION file) or when UNCHARTED_HANG_DUMP=1 asks for it; Linux
    // has no such dump of a running process without gdb, so the report says how to make a core dump instead
    std::string dump_env;
    std::error_code ec;
    if (!port::env("UNCHARTED_HANG_DUMP", dump_env)) dump_env = fs::exists(w->root / "VERSION", ec) ? "0" : "1";
    if (dump_env == "1")
        lines.push_back("Hang watchdog: no minidump on Linux (a core dump of the frozen game: gcore " + std::to_string(w->pid) +
                        ", from gdb)");
    const fs::path folder = w->log.parent_path();
    std::vector<fs::path> old = sorted_files(folder, "hang-", ".txt");
    for (size_t i = 0; i + 9 < old.size(); ++i) fs::remove(old[i], ec);   // keep 9 old reports + the new one = 10
    const fs::path report = folder / ("hang-" + std::string(stamp) + ".txt");
    std::string body;
    for (auto &l : lines) body += l + "\n";
    std::ofstream(report, std::ios::binary | std::ios::trunc) << body;
    {
        std::lock_guard<std::mutex> hold(w->lock);
        w->reports.push_back(report.string());
    }
    say("Hang watchdog: the game froze; report " + report.string());
}

// Polls the game log every 2 s (the Windows body's rules): while rendering, the runtime prints a "Frame stats" line
// every 5 s, so if the game still runs and no such line has appeared for `seconds`, it is treated as frozen. One
// report per freeze; the watchdog arms itself again once frame lines resume.
void watch(Watchdog *w) {
    uint64_t position = 0, guest_base = 0;
    bool armed = true, seen = false, have_base = false;
    auto last_frame = std::chrono::steady_clock::now();
    for (;;) {
        // wait up to 2 s, but end soon after the game does (Windows waits on the process handle)
        for (int i = 0; i < 20 && running(w->pid); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!running(w->pid)) return;
        std::ifstream f(w->log, std::ios::binary);
        if (!f) continue;
        f.seekg(0, std::ios::end);
        const uint64_t size = (uint64_t)f.tellg();
        std::string chunk;
        if (size > position) {
            f.seekg((std::streamoff)position);
            chunk.resize((size_t)(size - position));
            f.read(&chunk[0], (std::streamsize)chunk.size());
            chunk.resize((size_t)f.gcount());
        }
        position += chunk.size();
        if (!have_base) {
            size_t at = chunk.find("Guest image base: ");
            if (at != std::string::npos) {
                size_t s = at + 18, e = s;
                while (e < chunk.size() && std::isxdigit((unsigned char)chunk[e])) ++e;
                if (e > s) { guest_base = std::strtoull(chunk.substr(s, e - s).c_str(), nullptr, 16); have_base = true; }
            }
        }
        if (chunk.find("Frame stats") != std::string::npos) {
            last_frame = std::chrono::steady_clock::now();
            armed = seen = true;
            continue;
        }
        if (!(seen && armed) || std::chrono::steady_clock::now() - last_frame < std::chrono::seconds(w->seconds) ||
            !running(w->pid))
            continue;
        armed = false;
        freeze_report(w, have_base ? guest_base : 0);
    }
}

}  // namespace

extern "C" {

// Launches a watchdog thread for process `pid` (see the Windows body). Returns a watchdog id > 0, or 0 on failure.
UN_API int un_hang_watchdog(unsigned long pid, const un_path_char *log, const un_path_char *dumps, int seconds,
                            const un_path_char *root) {
    if (!running((pid_t)pid)) return 0;
    Watchdog *w = new Watchdog{(pid_t)pid, port::path_utf8(log ? log : ""), port::path_utf8(dumps ? dumps : ""),
                               port::path_utf8(root && *root ? root : "."), seconds, {}, {}};
    int id;
    {
        std::lock_guard<std::mutex> hold(watchdogs_lock);
        watchdogs.push_back(w);
        id = (int)watchdogs.size();
    }
    try {
        std::thread(watch, w).detach();
    } catch (...) {
        return 0;
    }
    return id;
}

// paths of every report this watchdog has produced so far, newline-separated
UN_API int un_hang_reports(int id, char *out, int cap) {
    Watchdog *w = nullptr;
    {
        std::lock_guard<std::mutex> hold(watchdogs_lock);
        if (id >= 1 && id <= (int)watchdogs.size()) w = watchdogs[id - 1];
    }
    std::string text;
    if (w) {
        std::lock_guard<std::mutex> hold(w->lock);
        for (auto &r : w->reports) text += r + "\n";
    }
    return give(text, out, cap);
}

// no minidump format on Linux (see above): 0, "could not write a minidump"
UN_API int un_minidump(unsigned long pid, const un_path_char *path) { return 0; }

}  // extern "C"

#endif
