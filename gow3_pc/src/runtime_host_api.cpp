// runtime_host_api.cpp - the host function table of uncharted_host.h, which the Uncharted 3 multiplayer helper's PC
// platform layer calls on Windows and on Linux alike (docs/WINDOWS-DEPENDENCIES.md §4.10, phase 5).
//
// Until helper v7.267 the helper found these functions itself on Windows: it walked the process environment block
// to ws2_32 / kernel32 / kernelbase / advapi32 / ucrtbase / user32 and called them through ms_abi pointers, which
// only works on Windows. The Windows bodies below make the same calls with the same arguments, so the helper sees
// the same results; the Linux bodies give the same meaning with BSD sockets and the C library: Winsock error
// numbers, Winsock (= FreeBSD) socket option numbers, millisecond timeouts, no SIGPIPE.
#include "uncharted_host.h"

#include "u3voice/u3voice.h"

#include <cstdint>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
#else
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <asm/prctl.h>

extern char **environ;
extern "C" void *runtime_low_map(size_t size, int prot);   // runtime_memory.c: host memory below 1 TiB
#endif

#define HOST UNCHARTED_HOST_ABI

namespace {

// ------------------------------------------------------------------------------------------------ Windows
#ifdef _WIN32

HOST int h_startup() {
    WSADATA data;
    return WSAStartup(0x0202, &data);
}
HOST uint64_t h_socket(int family, int type, int protocol) { return (uint64_t)::socket(family, type, protocol); }
HOST int h_close(uint64_t s) { return closesocket((SOCKET)s); }
HOST int h_bind(uint64_t s, const void *a, int n) { return ::bind((SOCKET)s, (const sockaddr *)a, n); }
HOST int h_connect(uint64_t s, const void *a, int n) { return ::connect((SOCKET)s, (const sockaddr *)a, n); }
HOST uint64_t h_accept(uint64_t s, void *a, int *n) { return (uint64_t)::accept((SOCKET)s, (sockaddr *)a, n); }
HOST int h_listen(uint64_t s, int backlog) { return ::listen((SOCKET)s, backlog); }
HOST int h_send(uint64_t s, const void *d, int n, int flags) { return ::send((SOCKET)s, (const char *)d, n, flags); }
HOST int h_recv(uint64_t s, void *d, int n, int flags) { return ::recv((SOCKET)s, (char *)d, n, flags); }
HOST int h_sendto(uint64_t s, const void *d, int n, int flags, const void *to, int tn) {
    return ::sendto((SOCKET)s, (const char *)d, n, flags, (const sockaddr *)to, tn);
}
HOST int h_recvfrom(uint64_t s, void *d, int n, int flags, void *from, int *fn) {
    return ::recvfrom((SOCKET)s, (char *)d, n, flags, (sockaddr *)from, fn);
}
HOST int h_setsockopt(uint64_t s, int level, int option, const void *v, int n) {
    return ::setsockopt((SOCKET)s, level, option, (const char *)v, n);
}
HOST int h_nonblocking(uint64_t s, uint32_t *on) { return ioctlsocket((SOCKET)s, (long)0x8004667eu, (u_long *)on); }
HOST int h_getsockname(uint64_t s, void *a, int *n) { return ::getsockname((SOCKET)s, (sockaddr *)a, n); }
HOST int h_last_error() { return WSAGetLastError(); }
HOST int h_resolve(const char *host, uint32_t *address) {
    const hostent *he = gethostbyname(host);
    if (!he || !he->h_addr_list || !he->h_addr_list[0]) return -1;
    std::memcpy(address, he->h_addr_list[0], 4);
    return 0;
}
HOST void *h_alloc(uint64_t bytes) { return VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); }

struct ThreadStart {
    UnchartedHostThread fn;
    void *arg;
};
DWORD WINAPI thread_start(void *p) {
    const ThreadStart start = *(ThreadStart *)p;
    delete (ThreadStart *)p;
    start.fn(start.arg);
    return 0;
}
HOST int h_thread(UnchartedHostThread fn, void *arg, uint64_t stack) {
    ThreadStart *start = new ThreadStart{fn, arg};
    DWORD id = 0;
    HANDLE t = CreateThread(nullptr, (SIZE_T)stack, thread_start, start, 0, &id);
    if (!t) {
        delete start;
        return -1;
    }
    CloseHandle(t);
    return 0;
}
HOST void h_sleep_ms(uint32_t ms) { Sleep(ms); }
HOST void h_utc(uint16_t time[8]) {
    SYSTEMTIME t;
    GetSystemTime(&t);
    std::memcpy(time, &t, 16);
}
HOST int h_random(void *out, uint32_t n) {
    typedef BOOLEAN(WINAPI * Gen)(PVOID, ULONG);
    static const Gen gen = (Gen)(void *)GetProcAddress(LoadLibraryA("advapi32.dll"), "SystemFunction036");   // RtlGenRandom
    return gen && gen(out, n) ? 0 : -1;
}
HOST uint32_t h_env_take(const char *name, char *out, uint32_t cap) {
    if (!out || cap < 2) return 0;
    std::memset(out, 0, cap);
    const DWORD n = GetEnvironmentVariableA(name, out, cap);
    if (!n) return 0;                    // not set: nothing to remove
    _putenv_s(name, "");                 // the C runtime's copy ("" removes it)
    SetEnvironmentVariableA(name, nullptr);   // the process environment block
    if (n >= cap) {                      // too long: not ours
        std::memset(out, 0, cap);
        return 0;
    }
    return n;
}
HOST int h_diag_append(const char *path, const char *text, uint32_t n) {
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE || !h) return 0;
    DWORD done = 0;
    const BOOL ok = WriteFile(h, text, n, &done, nullptr);
    CloseHandle(h);
    return ok && done == n;
}

// the one-line box (the helper's x035 box, the same window)
struct Box {
    const char *title, *label, *initial;
    int password;
    char *out;
    uint32_t cap;
    volatile uint8_t *result;
    volatile int32_t *busy;
};
Box g_box;
volatile LONG g_box_done, g_box_ok;
LRESULT CALLBACK box_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_COMMAND && ((w & 0xffff) == 1 || (w & 0xffff) == 2)) {
        g_box_ok = (w & 0xffff) == 1;
        g_box_done = 1;
        return 0;
    }
    if (m == WM_CLOSE) {
        g_box_ok = 0;
        g_box_done = 1;
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}
DWORD WINAPI box_thread(void *) {
    static bool registered;
    HINSTANCE inst = GetModuleHandleA(nullptr);
    if (!registered) {
        WNDCLASSA wc{};
        wc.lpfnWndProc = box_proc;
        wc.hInstance = inst;
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = "U3TextBox";
        if (!RegisterClassA(&wc)) {
            *g_box.busy = 0;
            return 0;
        }
        registered = true;
    }
    g_box_done = 0;
    g_box_ok = 0;
    HWND w = CreateWindowExA(WS_EX_TOPMOST, "U3TextBox", g_box.title, WS_CAPTION | WS_SYSMENU | WS_VISIBLE, 200, 160, 470, 130,
                             nullptr, nullptr, inst, nullptr);
    if (!w) {
        *g_box.busy = 0;
        return 0;
    }
    HGDIOBJ font = GetStockObject(DEFAULT_GUI_FONT);
    HWND edit = nullptr;
    if (HWND st = CreateWindowExA(0, "STATIC", g_box.label, WS_CHILD | WS_VISIBLE, 14, 16, 180, 20, w, nullptr, inst, nullptr))
        SendMessageA(st, WM_SETFONT, (WPARAM)font, 0);
    edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", g_box.password ? "" : g_box.initial,
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | (g_box.password ? ES_PASSWORD : 0), 200, 12,
                           240, 26, w, nullptr, inst, nullptr);
    if (edit) {
        SendMessageA(edit, WM_SETFONT, (WPARAM)font, 0);
        SendMessageA(edit, EM_SETSEL, 0, -1);
        SetFocus(edit);
    }
    HWND ok = CreateWindowExA(0, "BUTTON", "OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 250, 50, 90, 30, w,
                              (HMENU)1, inst, nullptr);
    HWND cancel = CreateWindowExA(0, "BUTTON", "Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 350, 50, 90, 30, w, (HMENU)2, inst,
                                  nullptr);
    if (ok) SendMessageA(ok, WM_SETFONT, (WPARAM)font, 0);
    if (cancel) SendMessageA(cancel, WM_SETFONT, (WPARAM)font, 0);
    SetForegroundWindow(w);
    MSG msg;
    while (!g_box_done && GetMessageA(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageA(w, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
    if (g_box_ok) {
        std::memset(g_box.out, 0, g_box.cap);
        if (edit) GetWindowTextA(edit, g_box.out, (int)g_box.cap - 1);
        *g_box.result = 1;
    }
    DestroyWindow(w);
    *g_box.busy = 0;
    return 0;
}
HOST int h_textbox(const char *title, const char *label, const char *initial, int password, char *out, uint32_t cap,
                   volatile uint8_t *result, volatile int32_t *busy) {
    g_box = Box{title, label, initial, password, out, cap, result, busy};
    *busy = 1;
    HANDLE t = CreateThread(nullptr, 0x40000, box_thread, nullptr, 0, nullptr);
    if (!t) {
        *busy = 0;
        return 0;
    }
    CloseHandle(t);
    return 1;
}

// ------------------------------------------------------------------------------------------------ Linux
#else

// the Winsock number of the last failing call on this thread (WSAGetLastError's counterpart)
thread_local int t_error;
int wsa(int e) {
    switch (e) {
    case EINTR: return 10004;
    case EBADF: case ENOTSOCK: return 10038;
    case EACCES: case EPERM: return 10013;
    case EFAULT: return 10014;
    case EINVAL: return 10022;
    case EMFILE: case ENFILE: return 10024;
    case EAGAIN: case EINPROGRESS: return 10035;   // Winsock: a non-blocking connect is WSAEWOULDBLOCK too
    case EALREADY: return 10037;
    case EDESTADDRREQ: return 10039;
    case EMSGSIZE: return 10040;
    case EPROTOTYPE: return 10041;
    case ENOPROTOOPT: return 10042;
    case EPROTONOSUPPORT: return 10043;
    case EOPNOTSUPP: return 10045;
    case EAFNOSUPPORT: return 10047;
    case EADDRINUSE: return 10048;
    case EADDRNOTAVAIL: return 10049;
    case ENETDOWN: return 10050;
    case ENETUNREACH: return 10051;
    case ENETRESET: return 10052;
    case ECONNABORTED: return 10053;
    case ECONNRESET: case EPIPE: return 10054;
    case ENOBUFS: case ENOMEM: return 10055;
    case EISCONN: return 10056;
    case ENOTCONN: return 10057;
    case ESHUTDOWN: return 10058;
    case ETIMEDOUT: return 10060;
    case ECONNREFUSED: return 10061;
    case EHOSTDOWN: return 10064;
    case EHOSTUNREACH: return 10065;
    default: return 10000 + (e & 0xff);
    }
}
int failed(int r) {   // -1 with the Winsock number noted
    t_error = wsa(errno);
    return r;
}
// the same for a call on socket s that may have waited: on a blocking socket, Linux reports an expired
// SO_RCVTIMEO / SO_SNDTIMEO as EAGAIN (EINPROGRESS for connect), where Winsock reports WSAETIMEDOUT
int failed_on(int s, int r) {
    const int e = errno;
    const int flags = fcntl(s, F_GETFL);
    t_error = (e == EAGAIN || e == EWOULDBLOCK || e == EINPROGRESS) && flags >= 0 && !(flags & O_NONBLOCK) ? 10060 : wsa(e);
    return r;
}

HOST int h_startup() { return 0; }
HOST uint64_t h_socket(int family, int type, int protocol) {
    const int s = ::socket(family, type | SOCK_CLOEXEC, protocol);
    if (s < 0) {
        failed(-1);
        return ~(uint64_t)0;
    }
    return (uint64_t)s;
}
HOST int h_close(uint64_t s) { return ::close((int)s) == 0 ? 0 : failed(-1); }
HOST int h_bind(uint64_t s, const void *a, int n) { return ::bind((int)s, (const sockaddr *)a, (socklen_t)n) == 0 ? 0 : failed(-1); }
HOST int h_connect(uint64_t s, const void *a, int n) {
    return ::connect((int)s, (const sockaddr *)a, (socklen_t)n) == 0 ? 0 : failed_on((int)s, -1);
}
HOST uint64_t h_accept(uint64_t s, void *a, int *n) {
    socklen_t length = n ? (socklen_t)*n : 0;
    const int r = ::accept4((int)s, (sockaddr *)a, a ? &length : nullptr, SOCK_CLOEXEC);
    if (r < 0) {
        failed_on((int)s, -1);
        return ~(uint64_t)0;
    }
    if (n) *n = (int)length;
    return (uint64_t)r;
}
HOST int h_listen(uint64_t s, int backlog) { return ::listen((int)s, backlog) == 0 ? 0 : failed(-1); }
HOST int h_send(uint64_t s, const void *d, int n, int flags) {
    const ssize_t r = ::send((int)s, d, (size_t)n, flags | MSG_NOSIGNAL);
    return r < 0 ? failed_on((int)s, -1) : (int)r;
}
HOST int h_recv(uint64_t s, void *d, int n, int flags) {
    const ssize_t r = ::recv((int)s, d, (size_t)n, flags);
    return r < 0 ? failed_on((int)s, -1) : (int)r;
}
HOST int h_sendto(uint64_t s, const void *d, int n, int flags, const void *to, int tn) {
    const ssize_t r = ::sendto((int)s, d, (size_t)n, flags | MSG_NOSIGNAL, (const sockaddr *)to, (socklen_t)tn);
    return r < 0 ? failed_on((int)s, -1) : (int)r;
}
HOST int h_recvfrom(uint64_t s, void *d, int n, int flags, void *from, int *fn) {
    socklen_t length = fn ? (socklen_t)*fn : 0;
    const ssize_t r = ::recvfrom((int)s, d, (size_t)n, flags, (sockaddr *)from, from ? &length : nullptr);
    if (r < 0) return failed_on((int)s, -1);
    if (fn) *fn = (int)length;
    return (int)r;
}
// Winsock's (= FreeBSD's) option numbers to Linux's; the timeouts take milliseconds as on Winsock
HOST int h_setsockopt(uint64_t s, int level, int option, const void *v, int n) {
    int lv = level, opt = option;
    if (level == 0xffff) {
        lv = SOL_SOCKET;
        switch (option) {
        case 0x0004: opt = SO_REUSEADDR; break;
        case 0x0008: opt = SO_KEEPALIVE; break;
        case 0x0020: opt = SO_BROADCAST; break;
        case 0x1001: opt = SO_SNDBUF; break;
        case 0x1002: opt = SO_RCVBUF; break;
        case 0x0080: {   // SO_LINGER: Winsock's {u16 on, u16 seconds}
            if (!v || n < 4) { t_error = 10014; return -1; }
            uint16_t w[2];
            std::memcpy(w, v, 4);
            const linger l{w[0], w[1]};
            return ::setsockopt((int)s, SOL_SOCKET, SO_LINGER, &l, sizeof l) == 0 ? 0 : failed(-1);
        }
        case 0x1005:
        case 0x1006: {   // SO_SNDTIMEO / SO_RCVTIMEO in milliseconds
            if (!v || n < 4) { t_error = 10014; return -1; }
            uint32_t ms;
            std::memcpy(&ms, v, 4);
            const timeval t{(time_t)(ms / 1000), (suseconds_t)(ms % 1000) * 1000};
            return ::setsockopt((int)s, SOL_SOCKET, option == 0x1005 ? SO_SNDTIMEO : SO_RCVTIMEO, &t, sizeof t) == 0 ? 0 : failed(-1);
        }
        default: t_error = 10022; return -1;   // as Winsock: an option it does not know is WSAEINVAL here
        }
    } else if (level == 0) {   // IPPROTO_IP
        switch (option) {
        case 3: opt = IP_TOS; break;
        case 4: opt = IP_TTL; break;
        case 9: opt = IP_MULTICAST_IF; break;
        case 10: opt = IP_MULTICAST_TTL; break;
        case 11: opt = IP_MULTICAST_LOOP; break;
        case 12: opt = IP_ADD_MEMBERSHIP; break;
        case 13: opt = IP_DROP_MEMBERSHIP; break;
        default: t_error = 10042; return -1;   // as Winsock: WSAENOPROTOOPT for an unknown IP option
        }
    } else if (level == 6) {   // IPPROTO_TCP
        if (option != 1) { t_error = 10022; return -1; }   // as Winsock
        opt = TCP_NODELAY;
    } else {
        t_error = 10022;   // as Winsock: a level it does not know
        return -1;
    }
    return ::setsockopt((int)s, lv, opt, v, (socklen_t)n) == 0 ? 0 : failed(-1);
}
HOST int h_nonblocking(uint64_t s, uint32_t *on) {
    const int flags = fcntl((int)s, F_GETFL);
    if (flags < 0) return failed(-1);
    return fcntl((int)s, F_SETFL, (on && *on) ? flags | O_NONBLOCK : flags & ~O_NONBLOCK) == 0 ? 0 : failed(-1);
}
HOST int h_getsockname(uint64_t s, void *a, int *n) {
    socklen_t length = n ? (socklen_t)*n : 0;
    if (::getsockname((int)s, (sockaddr *)a, &length) != 0) return failed(-1);
    if (n) *n = (int)length;
    return 0;
}
HOST int h_last_error() { return t_error; }
HOST int h_resolve(const char *host, uint32_t *address) {
    addrinfo hints{}, *found = nullptr;
    hints.ai_family = AF_INET;
    if (getaddrinfo(host, nullptr, &hints, &found) != 0 || !found) return -1;
    std::memcpy(address, &((const sockaddr_in *)found->ai_addr)->sin_addr, 4);
    freeaddrinfo(found);
    return 0;
}
HOST void *h_alloc(uint64_t bytes) { return runtime_low_map((size_t)bytes, PROT_READ | PROT_WRITE); }

struct ThreadStart {
    UnchartedHostThread fn;
    void *arg;
};
void *thread_start(void *p) {
    const ThreadStart start = *(ThreadStart *)p;
    delete (ThreadStart *)p;
    // A helper thread is a host thread, as on Windows: no guest TCB. Windows reads its TLS slot as 0 there; here GS
    // points at a zeroed page, so a gs-relative read gives 0 too instead of faulting.
    if (void *none = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); none != MAP_FAILED)
        syscall(SYS_arch_prctl, ARCH_SET_GS, (unsigned long)none);
    start.fn(start.arg);
    return nullptr;
}
HOST int h_thread(UnchartedHostThread fn, void *arg, uint64_t stack) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (stack) pthread_attr_setstacksize(&attr, (stack < (uint64_t)PTHREAD_STACK_MIN ? (size_t)PTHREAD_STACK_MIN : (size_t)stack));
    ThreadStart *start = new ThreadStart{fn, arg};
    pthread_t t;
    const int r = pthread_create(&t, &attr, thread_start, start);
    pthread_attr_destroy(&attr);
    if (r != 0) {
        delete start;
        return -1;
    }
    return 0;
}
HOST void h_sleep_ms(uint32_t ms) {
    timespec t{(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    while (nanosleep(&t, &t) != 0 && errno == EINTR) {}
}
HOST void h_utc(uint16_t time[8]) {   // SYSTEMTIME's fields
    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    std::tm t{};
    gmtime_r(&now.tv_sec, &t);
    time[0] = (uint16_t)(t.tm_year + 1900);
    time[1] = (uint16_t)(t.tm_mon + 1);
    time[2] = (uint16_t)t.tm_wday;
    time[3] = (uint16_t)t.tm_mday;
    time[4] = (uint16_t)t.tm_hour;
    time[5] = (uint16_t)t.tm_min;
    time[6] = (uint16_t)t.tm_sec;
    time[7] = (uint16_t)(now.tv_nsec / 1000000);
}
HOST int h_random(void *out, uint32_t n) {
    for (uint32_t done = 0; done < n;) {
        const ssize_t got = getrandom((uint8_t *)out + done, n - done, 0);
        if (got < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        done += (uint32_t)got;
    }
    return 0;
}
HOST uint32_t h_env_take(const char *name, char *out, uint32_t cap) {
    if (!out || cap < 2) return 0;
    std::memset(out, 0, cap);
    const char *v = getenv(name);
    if (!v) return 0;   // not set: nothing to remove
    const size_t n = std::strlen(v);
    if (n < cap) std::memcpy(out, v, n);
    unsetenv(name);
    return n < cap ? (uint32_t)n : 0;   // too long: not ours
}
HOST int h_diag_append(const char *path, const char *text, uint32_t n) {
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return 0;
    const ssize_t put = ::write(fd, text, n);
    ::close(fd);
    return put == (ssize_t)n;
}

// The box: zenity or kdialog (whichever the desktop has) on its own thread; neither: no box (0), as the consoles.
struct Box {
    std::string title, label, initial;
    int password;
    char *out;
    uint32_t cap;
    volatile uint8_t *result;
    volatile int32_t *busy;
    std::vector<std::string> argv;
};
bool on_path(const char *name) {
    const char *path = getenv("PATH");
    std::string p = path ? path : "/usr/bin:/bin";
    for (size_t at = 0; at <= p.size();) {
        size_t end = p.find(':', at);
        if (end == std::string::npos) end = p.size();
        const std::string file = (end > at ? p.substr(at, end - at) : std::string(".")) + "/" + name;
        if (access(file.c_str(), X_OK) == 0) return true;
        at = end + 1;
    }
    return false;
}
void *box_thread(void *p) {
    Box *box = (Box *)p;
    int out[2];
    bool ok = false;
    std::string text;
    if (pipe2(out, O_CLOEXEC) == 0) {
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, out[1], 1);
        std::vector<char *> args;
        for (std::string &a : box->argv) args.push_back(&a[0]);
        args.push_back(nullptr);
        pid_t pid = -1;
        const bool started = posix_spawnp(&pid, args[0], &actions, nullptr, args.data(), environ) == 0;
        posix_spawn_file_actions_destroy(&actions);
        ::close(out[1]);
        if (started) {
            char buf[512];
            for (ssize_t got; (got = ::read(out[0], buf, sizeof buf)) > 0;) text.append(buf, (size_t)got);
            int status = 0;
            waitpid(pid, &status, 0);
            ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;   // 1: Cancel
        }
        ::close(out[0]);
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    if (ok) {
        std::memset(box->out, 0, box->cap);
        std::memcpy(box->out, text.data(), std::min<size_t>(text.size(), box->cap - 1));
        *box->result = 1;
    }
    *box->busy = 0;
    delete box;
    return nullptr;
}
HOST int h_textbox(const char *title, const char *label, const char *initial, int password, char *out, uint32_t cap,
                   volatile uint8_t *result, volatile int32_t *busy) {
    if (!out || cap < 2) return 0;
    Box *box = new Box{title ? title : "", label ? label : "", initial ? initial : "", password, out, cap, result, busy, {}};
    if (on_path("zenity")) {
        box->argv = {"zenity", "--entry", "--title=" + box->title, "--text=" + box->label};
        if (password) box->argv.push_back("--hide-text");
        else box->argv.push_back("--entry-text=" + box->initial);
    } else if (on_path("kdialog")) {
        box->argv = {"kdialog", "--title", box->title};
        if (password) box->argv.insert(box->argv.end(), {"--password", box->label});
        else box->argv.insert(box->argv.end(), {"--inputbox", box->label, box->initial});
    } else {
        delete box;
        return 0;
    }
    *busy = 1;
    pthread_t t;
    if (pthread_create(&t, nullptr, box_thread, box) != 0) {
        *busy = 0;
        delete box;
        return 0;
    }
    pthread_detach(t);
    return 1;
}
#endif

HOST const void *h_voice() { return uncharted_u3_voice(); }

const UnchartedHost g_table = {
    UNCHARTED_HOST_VERSION, sizeof(UnchartedHost),
    h_startup, h_socket, h_close, h_bind, h_connect, h_accept, h_listen, h_send, h_recv, h_sendto, h_recvfrom,
    h_setsockopt, h_nonblocking, h_getsockname, h_last_error, h_resolve,
    h_alloc, h_thread, h_sleep_ms, h_utc, h_random, h_env_take, h_diag_append, h_textbox, h_voice,
};

}  // namespace

extern "C" int runtime_host_api_fd(void) {
    const uint64_t address = (uint64_t)(uintptr_t)&g_table;
    int fd[2];
#ifdef _WIN32
    if (_pipe(fd, 64, _O_BINARY) != 0) return -1;
    const bool ok = _write(fd[1], &address, sizeof address) == (int)sizeof address;
    _close(fd[1]);
    if (!ok) {
        _close(fd[0]);
        return -1;
    }
#else
    if (pipe2(fd, O_CLOEXEC) != 0) return -1;
    const bool ok = ::write(fd[1], &address, sizeof address) == (ssize_t)sizeof address;
    ::close(fd[1]);
    if (!ok) {
        ::close(fd[0]);
        return -1;
    }
#endif
    return fd[0];
}
