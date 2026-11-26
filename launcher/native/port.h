// port.h - the few operating-system calls the launcher makes that the C++ library does not give directly. Windows and
// Linux bodies sit side by side here, behind the same name, so the code that uses them has no #ifdef. Everything else
// uses <filesystem>, <fstream>, <chrono> and <thread> directly.
//
// Text is UTF-8 everywhere inside the launcher. Wide strings exist only at the exported C API (native.h) and at the
// Windows API boundary; port::utf8 / port::wide are the only conversions.
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace port {

namespace fs = std::filesystem;

// ------------------------------------------------------------------------------------------------ text

#ifdef _WIN32
inline std::string utf8(const std::wstring &w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

inline std::wstring wide(const std::string &s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
#else
inline std::string utf8(const std::wstring &w) {   // wchar_t is UTF-32 here
    std::string out;
    out.reserve(w.size() * 2);
    for (size_t i = 0; i < w.size(); ++i) {
        uint32_t c = (uint32_t)w[i];
        if (c >= 0xd800 && c <= 0xdbff && i + 1 < w.size()) {   // a UTF-16 surrogate pair that reached a wstring
            const uint32_t lo = (uint32_t)w[i + 1];
            if (lo >= 0xdc00 && lo <= 0xdfff) {
                c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
                ++i;
            }
        }
        if (c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) c = 0xfffd;
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) {
            out += (char)(0xc0 | (c >> 6));
            out += (char)(0x80 | (c & 0x3f));
        } else if (c < 0x10000) {
            out += (char)(0xe0 | (c >> 12));
            out += (char)(0x80 | ((c >> 6) & 0x3f));
            out += (char)(0x80 | (c & 0x3f));
        } else {
            out += (char)(0xf0 | (c >> 18));
            out += (char)(0x80 | ((c >> 12) & 0x3f));
            out += (char)(0x80 | ((c >> 6) & 0x3f));
            out += (char)(0x80 | (c & 0x3f));
        }
    }
    return out;
}

inline std::wstring wide(const std::string &s) {   // UTF-8 to UTF-32
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        uint32_t cp = 0xfffd;
        size_t n = 1;
        if (c < 0x80) cp = c;
        else if ((c & 0xe0) == 0xc0 && i + 1 < s.size()) { cp = (uint32_t)(c & 0x1f) << 6 | ((unsigned char)s[i + 1] & 0x3f); n = 2; }
        else if ((c & 0xf0) == 0xe0 && i + 2 < s.size()) {
            cp = (uint32_t)(c & 0x0f) << 12 | ((unsigned char)s[i + 1] & 0x3f) << 6 | ((unsigned char)s[i + 2] & 0x3f);
            n = 3;
        } else if ((c & 0xf8) == 0xf0 && i + 3 < s.size()) {
            cp = (uint32_t)(c & 0x07) << 18 | ((unsigned char)s[i + 1] & 0x3f) << 12 | ((unsigned char)s[i + 2] & 0x3f) << 6 |
                 ((unsigned char)s[i + 3] & 0x3f);
            n = 4;
        }
        out += (wchar_t)cp;
        i += n;
    }
    return out;
}
#endif

// the launcher's wide path text as a path. Linux: UTF-8, and a backslash (the Windows separator some joins and
// constant names still use) becomes "/"; no file name of ours or of the game contains one.
inline fs::path path(const std::wstring &w) {
#ifdef _WIN32
    return fs::path(w);
#else
    std::string s = utf8(w);
    for (char &c : s)
        if (c == '\\') c = '/';
    return fs::path(s);
#endif
}

#ifndef _WIN32
// an exported function's UTF-8 path argument (native.h un_path_char) as a path
inline fs::path path(const char *s) { return fs::path(s ? s : ""); }
#endif

// an exported function's path argument (native.h un_path_char: wchar_t on Windows, UTF-8 char on Linux) as the
// launcher's wide text; `fallback` when it is null
inline std::wstring wtext(const wchar_t *p, const wchar_t *fallback = L"") { return p ? std::wstring(p) : std::wstring(fallback); }
inline std::wstring wtext(const char *p, const wchar_t *fallback = L"") { return p ? wide(p) : std::wstring(fallback); }

// the other way: UTF-8 text as an exported function's path argument (wide on Windows, UTF-8 on Linux)
#ifdef _WIN32
using arg_string = std::wstring;
inline std::wstring arg(const std::string &s) { return wide(s); }
inline std::wstring arg(const std::wstring &w) { return w; }
#else
using arg_string = std::string;
inline std::string arg(const std::string &s) { return s; }
inline std::string arg(const std::wstring &w) { return utf8(w); }
#endif

// the native separator for wide joins
#ifdef _WIN32
inline constexpr wchar_t sep = L'\\';
#else
inline constexpr wchar_t sep = L'/';
#endif

// UTF-8 path text as a path (Linux: a backslash becomes "/", as in path() above)
inline fs::path path_utf8(const std::string &s) {
#ifdef _WIN32
    return fs::u8path(s);
#else
    std::string t = s;
    for (char &c : t)
        if (c == '\\') c = '/';
    return fs::path(t);
#endif
}

// opens a folder, file or web address with the desktop's default program and does not wait (Windows: ShellExecuteW
// "open"; Linux: xdg-open)
inline void open_path(const fs::path &p) {
#ifdef _WIN32
    ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    const pid_t pid = ::fork();
    if (pid == 0) {   // a grandchild runs xdg-open, so nobody has to wait for it
        if (::fork() == 0) {
            ::execlp("xdg-open", "xdg-open", p.c_str(), (char *)nullptr);
            ::_exit(127);
        }
        ::_exit(0);
    }
    if (pid > 0) ::waitpid(pid, nullptr, 0);
#endif
}

// the same file path (Windows folds case, Linux does not)
inline bool same_path(const fs::path &a, const fs::path &b) {
#ifdef _WIN32
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
#else
    return a == b;
#endif
}

inline std::string utf8(const fs::path &p) {
#ifdef _WIN32
    return utf8(p.wstring());
#else
    return p.string();
#endif
}

// ------------------------------------------------------------------------------------------------ files

// false when the file is missing, not a regular file, or larger than `max`; the file is read whole otherwise
inline bool read(const fs::path &p, std::string &out, uint64_t max) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return false;
    const uint64_t size = fs::file_size(p, ec);
    if (ec || size > max) return false;
    out.assign((size_t)size, '\0');
    if (!size) return true;
    std::FILE *f = nullptr;
#ifdef _WIN32
    f = _wfopen(p.c_str(), L"rb");
#else
    f = std::fopen(p.c_str(), "rb");
#endif
    if (!f) return false;
    const size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    out.resize(got);
    return got == size;
}

// up to `count` bytes from `at` (a negative `at` counts from the end of the file). `out` holds the bytes read, also
// when the file ended early; false when the file is missing, can't be opened or can't be seeked.
inline bool read_part(const fs::path &p, long long at, long long count, std::vector<uint8_t> &out) {
    out.clear();
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return false;
    const long long size = (long long)fs::file_size(p, ec);
    if (ec) return false;
    if (at < 0) at = size + at < 0 ? 0 : size + at;
    if (at >= size || count <= 0) return true;
    if (count > size - at) count = size - at;
    std::FILE *f = nullptr;
#ifdef _WIN32
    f = _wfopen(p.c_str(), L"rb");
#else
    f = std::fopen(p.c_str(), "rb");
#endif
    if (!f) return false;
    bool ok = false;
#ifdef _WIN32
    const int moved = _fseeki64(f, at, SEEK_SET);
#else
    const int moved = fseeko(f, (off_t)at, SEEK_SET);
#endif
    if (moved == 0) {
        out.resize((size_t)count);
        const size_t got = std::fread(out.data(), 1, out.size(), f);
        out.resize(got);
        ok = got == (size_t)count;
    }
    std::fclose(f);
    return ok;
}

// Windows: MoveFileExW(MOVEFILE_REPLACE_EXISTING); Linux: rename(), which replaces atomically
inline bool replace(const fs::path &from, const fs::path &to) {
#ifdef _WIN32
    return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return ::rename(from.c_str(), to.c_str()) == 0;
#endif
}

// writes the whole file, truncating it (creates it when missing); same sharing as before on Windows
inline bool write(const fs::path &p, const void *data, size_t n) {
    std::FILE *f = nullptr;
#ifdef _WIN32
    f = _wfopen(p.c_str(), L"wb");
#else
    f = std::fopen(p.c_str(), "wb");
#endif
    if (!f) return false;
    const bool written = n == 0 || std::fwrite(data, 1, n, f) == n;
    const bool closed = std::fclose(f) == 0;
    return written && closed;
}

// a temporary file next to the target, then replace it
inline bool write_atomic(const fs::path &p, const void *data, size_t n) {
    if (p.empty()) return false;
    fs::path tmp = p;
    tmp += ".tmp";
#ifdef _WIN32
    std::FILE *f = _wfopen(tmp.c_str(), L"wb");
#else
    std::FILE *f = std::fopen(tmp.c_str(), "wb");
#endif
    if (!f) return false;
    const bool written = n == 0 || std::fwrite(data, 1, n, f) == n;
    const bool closed = std::fclose(f) == 0;
    if (!written || !closed || !replace(tmp, p)) {
        std::error_code ec;
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

inline bool ensure_dir(const fs::path &p) {
    std::error_code ec;
    if (fs::is_directory(p, ec)) return true;
    fs::create_directories(p, ec);
    return fs::is_directory(p, ec);
}

// secrets: a compiler may not elide a write through a volatile pointer
inline void zero(void *p, size_t n) {
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--) *v++ = 0;
}

// ------------------------------------------------------------------------------------------------ environment

inline bool env(const char *name, std::string &out) {
#ifdef _WIN32
    const std::wstring wname = wide(name);   // names are ASCII, values may not be
    wchar_t buf[32768];
    const DWORD n = GetEnvironmentVariableW(wname.c_str(), buf, (DWORD)(sizeof buf / sizeof *buf));
    if (!n || n >= sizeof buf / sizeof *buf) return false;
    out = utf8(std::wstring(buf, n));
    return true;
#else
    const char *v = std::getenv(name);
    if (!v) return false;
    out = v;
    return true;
#endif
}

// ------------------------------------------------------------------------------------------------ the program and its libraries

inline fs::path executable_path() {
#ifdef _WIN32
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
    buf.resize(n);
    return fs::path(buf);
#else
    std::vector<char> buf(4096);
    for (;;) {
        const ssize_t n = ::readlink("/proc/self/exe", buf.data(), buf.size());
        if (n < 0) return fs::path();
        if ((size_t)n < buf.size()) return fs::path(std::string(buf.data(), (size_t)n));
        buf.resize(buf.size() * 2);
    }
#endif
}

inline void *load_library(const fs::path &p) {
#ifdef _WIN32
    return (void *)LoadLibraryExW(p.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
#else
    return ::dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

// a library the system's loader finds by name (SDL3 on Linux: "libSDL3.so.0")
inline void *load_library_name(const std::string &name) {
#ifdef _WIN32
    return (void *)LoadLibraryW(wide(name).c_str());
#else
    return ::dlopen(name.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

inline void *symbol(void *library, const char *name) {
#ifdef _WIN32
    return (void *)GetProcAddress((HMODULE)library, name);
#else
    return ::dlsym(library, name);
#endif
}

// ------------------------------------------------------------------------------------------------ the machine

// this process's id, used in temporary file names
inline unsigned long process_id() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return (unsigned long)::getpid();
#endif
}

// logical CPUs this process may run on, like GetProcessAffinityMask / sched_getaffinity; 8 when unknown
inline int hardware_threads() {
#ifdef _WIN32
    DWORD_PTR process = 0, system = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system) && process) {
        int n = 0;
        for (DWORD_PTR m = process; m; m &= m - 1) ++n;
        return n;
    }
    const DWORD all = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return all ? (int)all : 8;
#else
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof set, &set) == 0) {
        int n = 0;
        for (int i = 0; i < CPU_SETSIZE; ++i)
            if (CPU_ISSET(i, &set)) ++n;
        if (n) return n;
    }
    const unsigned n = std::thread::hardware_concurrency();
    return n ? (int)n : 8;
#endif
}

// the OS's own description of an error code (Windows: FormatMessage; Linux: strerror), without a trailing newline
inline std::string error_text(unsigned long code) {
#ifdef _WIN32
    wchar_t *text = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                       FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, (DWORD)code, 0, (LPWSTR)&text, 0, nullptr);
    std::string out;
    if (n && text) out = utf8(std::wstring(text, n));
    if (text) LocalFree(text);
    while (!out.empty() && (out.back() == '\r' || out.back() == '\n' || out.back() == ' ' || out.back() == '\t'))
        out.pop_back();
    return out.empty() ? "error" : out;
#else
    const char *text = std::strerror((int)code);
    return text ? text : "error";
#endif
}

// ------------------------------------------------------------------------------------------------ time

// milliseconds on a monotonic clock; the epoch differs from GetTickCount64's, only differences are used
inline long long now_ms() {
    using namespace std::chrono;
    return (long long)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// ------------------------------------------------------------------------------------------------ program names

// a program file name: "u3fix" -> "u3fix.exe" on Windows, "u3fix" on Linux
inline std::string exe_name(const std::string &base) {
#ifdef _WIN32
    return base + ".exe";
#else
    return base;
#endif
}

// a name the Windows build and the recipe write with ".exe" ("u3fix.exe") as this system's program name: unchanged on
// Windows, without the ".exe" on Linux (any case)
inline std::string program_name(const std::string &name) {
#ifdef _WIN32
    return name;
#else
    const size_t n = name.size();
    if (n > 4 && name[n - 4] == '.' && (name[n - 3] | 0x20) == 'e' && (name[n - 2] | 0x20) == 'x' &&
        (name[n - 1] | 0x20) == 'e')
        return name.substr(0, n - 4);
    return name;
#endif
}

// ------------------------------------------------------------------------------------------------ processes (Linux)
//
// Windows: every caller keeps its own CreateProcessW body (each with its own flags, handle list and window rules) next
// to the Linux body that uses these. Linux: fork + execve, so the child can take its working folder, priority and
// death signal before the program starts; the child calls only async-signal-safe functions. A failed execve reaches
// spawn() through a close-on-exec pipe, so spawn() fails like CreateProcessW does when the program is missing.
#ifndef _WIN32

struct Spawn {
    std::vector<std::string> argv;   // argv[0]: a path, or a name looked up on PATH
    std::string cwd;                 // "" = this process's
    bool set_env = false;            // true: `env` is the child's whole environment ("NAME=value"), else it inherits
    std::vector<std::string> env;
    enum Out { inherit, pipe, file, null } out = inherit;   // stdout
    enum Err { same_as_out, inherit_err, err_pipe } err = same_as_out;   // stderr: where stdout goes, ours, or its own pipe (Child::err)
    std::string out_file;            // Out::file: this file, truncated or (append) added to
    bool append = false;
    bool in_pipe = false;            // a pipe to its stdin (Child::in); else stdin is /dev/null (or inherited)
    bool null_in = true;
    bool low_priority = false;       // nice 10: BELOW_NORMAL_PRIORITY_CLASS's counterpart
    bool die_with_parent = false;    // kill_with_parent: SIGKILL when the thread that started it ends (PR_SET_PDEATHSIG)
    bool own_group = false;          // its own process group (and session): it outlives the launcher's terminal
};

struct Child {
    pid_t pid = -1;
    int in = -1;    // Spawn::in_pipe: write end of its stdin
    int out = -1;   // Out::pipe: read end of its stdout (+ stderr unless Spawn::err says otherwise)
    int err = -1;   // Spawn::err_pipe: read end of its stderr
    bool done = false;
    int code = -1;  // exit code once done (128 + signal when killed)
};

inline void close_fd(int &fd) {
    if (fd >= 0) ::close(fd);
    fd = -1;
}

// the program file execve gets: a name without '/' is looked up on PATH (of the child's environment, else ours)
inline std::string find_program(const std::string &name, const std::vector<std::string> *env = nullptr) {
    if (name.empty() || name.find('/') != std::string::npos) return name;
    std::string path_var;
    bool found = false;
    if (env)
        for (const std::string &e : *env)
            if (e.compare(0, 5, "PATH=") == 0) { path_var = e.substr(5); found = true; }
    if (!found) {
        const char *p = std::getenv("PATH");
        path_var = p ? p : "/usr/local/bin:/usr/bin:/bin";
    }
    size_t at = 0;
    for (;;) {
        const size_t end = path_var.find(':', at);
        std::string dir = path_var.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (dir.empty()) dir = ".";
        const std::string full = dir + "/" + name;
        if (::access(full.c_str(), X_OK) == 0) {
            struct stat st;
            if (::stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode)) return full;
        }
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return name;   // execve fails with ENOENT, reported by spawn()
}

// starts the program; false with the reason in `error` when it could not start
inline bool spawn(const Spawn &s, Child &child, std::string &error) {
    child = Child();
    if (s.argv.empty()) { error = "no program"; return false; }
    const std::string program = find_program(s.argv[0], s.set_env ? &s.env : nullptr);
    std::vector<char *> argv, envp;
    for (const std::string &a : s.argv) argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);
    if (s.set_env) {
        for (const std::string &e : s.env) envp.push_back(const_cast<char *>(e.c_str()));
        envp.push_back(nullptr);
    }
    int in_fd[2] = {-1, -1}, out_fd[2] = {-1, -1}, err_fd[2] = {-1, -1}, report[2] = {-1, -1}, out_file = -1, null_fd = -1;
    auto fail = [&](const std::string &why) {
        for (int *fd : {&in_fd[0], &in_fd[1], &out_fd[0], &out_fd[1], &err_fd[0], &err_fd[1], &report[0], &report[1], &out_file, &null_fd})
            close_fd(*fd);
        error = why;
        return false;
    };
    if (s.in_pipe && ::pipe2(in_fd, O_CLOEXEC) != 0) return fail(std::string("pipe: ") + std::strerror(errno));
    if (s.out == Spawn::pipe && ::pipe2(out_fd, O_CLOEXEC) != 0) return fail(std::string("pipe: ") + std::strerror(errno));
    if (s.err == Spawn::err_pipe && ::pipe2(err_fd, O_CLOEXEC) != 0) return fail(std::string("pipe: ") + std::strerror(errno));
    if (s.out == Spawn::file) {
        out_file = ::open(s.out_file.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | (s.append ? O_APPEND : O_TRUNC), 0644);
        if (out_file < 0) return fail("could not open " + s.out_file + ": " + std::strerror(errno));
    }
    if ((!s.in_pipe && s.null_in) || s.out == Spawn::null) {
        null_fd = ::open("/dev/null", O_RDWR | O_CLOEXEC);
        if (null_fd < 0) return fail(std::string("/dev/null: ") + std::strerror(errno));
    }
    if (::pipe2(report, O_CLOEXEC) != 0) return fail(std::string("pipe: ") + std::strerror(errno));
    const int stdin_fd = s.in_pipe ? in_fd[0] : s.null_in ? null_fd : -1;
    const int stdout_fd = s.out == Spawn::pipe ? out_fd[1] : s.out == Spawn::file ? out_file : s.out == Spawn::null ? null_fd : -1;
    const pid_t parent = ::getpid();
    const char *cwd = s.cwd.empty() ? nullptr : s.cwd.c_str();
    const pid_t pid = ::fork();
    if (pid < 0) return fail(std::string("fork: ") + std::strerror(errno));
    if (pid == 0) {   // the child: async-signal-safe calls only, up to execve
        int why = 0;
        if (s.die_with_parent) {
            ::prctl(PR_SET_PDEATHSIG, SIGKILL);
            if (::getppid() != parent) ::_exit(127);   // the parent is already gone
        }
        if (s.own_group) ::setsid();
        if (stdin_fd >= 0) ::dup2(stdin_fd, 0);   // dup2 clears close-on-exec on the copy
        if (err_fd[1] >= 0) ::dup2(err_fd[1], 2);
        if (stdout_fd >= 0) {
            ::dup2(stdout_fd, 1);
            if (s.err == Spawn::same_as_out) ::dup2(stdout_fd, 2);
        }
        struct sigaction dfl;
        std::memset(&dfl, 0, sizeof dfl);
        dfl.sa_handler = SIG_DFL;
        ::sigaction(SIGPIPE, &dfl, nullptr);
        sigset_t none;
        sigemptyset(&none);
        ::sigprocmask(SIG_SETMASK, &none, nullptr);
        if (s.low_priority) ::setpriority(PRIO_PROCESS, 0, 10);
        if (cwd && ::chdir(cwd) != 0) why = errno;
        if (!why) {
            ::execve(program.c_str(), argv.data(), s.set_env ? envp.data() : environ);
            why = errno;
        }
        const ssize_t put = ::write(report[1], &why, sizeof why);
        (void)put;
        ::_exit(127);
    }
    close_fd(report[1]);
    int why = 0;
    ssize_t got;
    do got = ::read(report[0], &why, sizeof why); while (got < 0 && errno == EINTR);
    close_fd(report[0]);
    close_fd(in_fd[0]);
    close_fd(out_fd[1]);
    close_fd(err_fd[1]);
    close_fd(out_file);
    close_fd(null_fd);
    if (got == (ssize_t)sizeof why) {   // execve (or chdir) failed: the child has exited
        ::waitpid(pid, nullptr, 0);
        close_fd(in_fd[1]);
        close_fd(out_fd[0]);
        close_fd(err_fd[0]);
        error = std::string(cwd && why == ENOENT && ::access(cwd, F_OK) != 0 ? "no folder " + s.cwd
                                                                               : "could not start " + s.argv[0]) +
                ": " + std::strerror(why);
        return false;
    }
    child.pid = pid;
    child.in = in_fd[1];
    child.out = out_fd[0];
    child.err = err_fd[0];
    return true;
}

inline int exit_code_of_status(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

// true while it runs; once it has ended, child.done and child.code are set (the process is reaped here)
inline bool running(Child &child) {
    if (child.pid <= 0 || child.done) return false;
    int status = 0;
    const pid_t r = ::waitpid(child.pid, &status, WNOHANG);
    if (r == 0) return true;
    child.done = true;
    child.code = r == child.pid ? exit_code_of_status(status) : -1;
    return false;
}

// waits up to timeout_ms (negative: no limit); its exit code, or -1 when it still runs
inline int wait(Child &child, int timeout_ms = -1) {
    if (child.pid <= 0) return -1;
    if (child.done) return child.code;
    if (timeout_ms < 0) {
        int status = 0;
        pid_t r;
        do r = ::waitpid(child.pid, &status, 0); while (r < 0 && errno == EINTR);
        child.done = true;
        child.code = r == child.pid ? exit_code_of_status(status) : -1;
        return child.code;
    }
    const long long until = now_ms() + timeout_ms;
    for (;;) {
        if (!running(child)) return child.code;
        if (now_ms() >= until) return -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// TerminateProcess's counterpart: SIGKILL, then reaped
inline void kill(Child &child) {
    if (child.pid <= 0 || child.done) return;
    ::kill(child.pid, SIGKILL);
    wait(child);
}

// closes our pipe ends (the process itself is left alone)
inline void close(Child &child) {
    close_fd(child.in);
    close_fd(child.out);
    close_fd(child.err);
}

// up to `n` bytes from a pipe within timeout_ms (0: only what is there now, negative: until some arrives or it ends):
// the count, 0 when nothing came in time, -1 when the other side has closed it
inline long read_some(int fd, char *buf, size_t n, int timeout_ms) {
    if (fd < 0) return -1;
    pollfd p{fd, POLLIN, 0};
    int r;
    do r = ::poll(&p, 1, timeout_ms); while (r < 0 && errno == EINTR);
    if (r < 0) return -1;
    if (r == 0) return 0;
    ssize_t got;
    do got = ::read(fd, buf, n); while (got < 0 && errno == EINTR);
    return got > 0 ? (long)got : -1;
}

// the whole buffer into a pipe (or file); false when it could not be written
inline bool write_all(int fd, const void *data, size_t n) {
    const char *p = (const char *)data;
    while (n) {
        ssize_t put = ::write(fd, p, n);
        if (put < 0 && errno == EINTR) continue;
        if (put <= 0) return false;
        p += put;
        n -= (size_t)put;
    }
    return true;
}

// the process exists (OpenProcess(SYNCHRONIZE)'s counterpart: kill(pid, 0); EPERM = it exists, owned by someone else)
inline bool process_alive(long long pid) {
    if (pid <= 0) return false;
    return ::kill((pid_t)pid, 0) == 0 || errno == EPERM;
}

struct ProcessInfo {
    long long pid = 0, parent = 0;
    std::string name;   // the program's file name ("steam", "gow3-probe"); /proc/<pid>/comm when the link is unreadable
};

// every process (the toolhelp snapshot's counterpart), from /proc
inline std::vector<ProcessInfo> list_processes() {
    std::vector<ProcessInfo> out;
    DIR *d = ::opendir("/proc");
    if (!d) return out;
    while (dirent *e = ::readdir(d)) {
        char *end = nullptr;
        const long long pid = std::strtoll(e->d_name, &end, 10);
        if (pid <= 0 || !end || *end) continue;
        const std::string base = std::string("/proc/") + e->d_name;
        std::FILE *f = std::fopen((base + "/stat").c_str(), "rb");   // /proc files report size 0: read them as a stream
        if (!f) continue;                                             // it has ended meanwhile
        char buf[1024];
        const size_t got = std::fread(buf, 1, sizeof buf, f);
        std::fclose(f);
        const std::string stat(buf, got);
        ProcessInfo p;
        p.pid = pid;
        const size_t open = stat.find('('), close_at = stat.rfind(')');
        if (open == std::string::npos || close_at == std::string::npos || close_at < open) continue;
        p.name = stat.substr(open + 1, close_at - open - 1);   // comm: at most 15 characters
        // after ") ": state, then the parent's id
        const size_t ppid_at = stat.find(' ', close_at + 2);
        if (ppid_at != std::string::npos) p.parent = std::strtoll(stat.c_str() + ppid_at + 1, nullptr, 10);
        char link[4096];
        const ssize_t n = ::readlink((base + "/exe").c_str(), link, sizeof link - 1);
        if (n > 0) {
            std::string exe(link, (size_t)n);
            const std::string deleted = " (deleted)";
            if (exe.size() > deleted.size() && exe.compare(exe.size() - deleted.size(), deleted.size(), deleted) == 0)
                exe.resize(exe.size() - deleted.size());
            const size_t slash = exe.rfind('/');
            p.name = slash == std::string::npos ? exe : exe.substr(slash + 1);
        }
        out.push_back(p);
    }
    ::closedir(d);
    return out;
}

// `taskkill /PID <pid> /T /F`'s counterpart: SIGKILL to the process and everything it started (children first
// stopped, so none can start more meanwhile)
inline void kill_tree(long long pid) {
    if (pid <= 0) return;
    const std::vector<ProcessInfo> all = list_processes();
    std::vector<long long> tree{pid};
    for (size_t i = 0; i < tree.size(); ++i)
        for (const ProcessInfo &p : all)
            if (p.parent == tree[i] && p.pid != pid) tree.push_back(p.pid);
    for (long long p : tree) ::kill((pid_t)p, SIGSTOP);
    for (long long p : tree) ::kill((pid_t)p, SIGKILL);
}

#endif   // !_WIN32

// ------------------------------------------------------------------------------------------------ later phases

// a feature whose Linux body comes in a later phase (docs/WINDOWS-DEPENDENCIES.md §13): one log line on stderr, and
// the text the caller hands back as its reason
inline const char *not_yet(const char *what) {
    std::fprintf(stderr, "uncharted_native: %s: not on Linux yet\n", what);
    return "not on Linux yet";
}

// the core's text answer convention (native.h): 0 when `s` fits in `out`, else -(the size needed)
inline int give(const std::string &s, char *out, int cap) {
    if ((int)s.size() + 1 > cap) return -(int)(s.size() + 1);
    std::memcpy(out, s.c_str(), s.size() + 1);
    return 0;
}

// a stub's answer: "<what>: not on Linux yet" in `out`, then `status` (or the size needed)
inline int not_yet_answer(const char *what, int status, char *out, int cap) {
    const int r = give(std::string(what) + ": " + not_yet(what), out, cap);
    return r < 0 ? r : status;
}

}   // namespace port
