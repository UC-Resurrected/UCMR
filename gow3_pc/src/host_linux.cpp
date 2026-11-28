// host_linux.cpp - the runtime's Linux platform functions (host_linux.h). Windows keeps its own bodies in probe.c and
// runtime_file.c and does not build this file.
//
// Signals: the counterpart of probe.c's Windows vectored exception handler. In order: Naughty Dog's assertion trap
// (int 0x41) and a lone debug break (int3) in guest code are stepped over and logged once per address, an instruction
// this CPU lacks goes to runtime_isa.cpp, a write to a page the GPU library tracks goes to it, a speculative guest read
// (runtime_fault_recover) resumes at its recovery point; anything else ends the process with a report.
#include "host_linux.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <dirent.h>
#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>
#ifndef TRAP_PERF
#define TRAP_PERF 6   // si_code of a perf event's SIGTRAP (the kernel's asm-generic/siginfo.h; C libraries may lack it)
#endif
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>

extern "C" {
extern __thread sigjmp_buf *runtime_fault_recover;      // runtime.h
int runtime_isa_emulate(void *context);                  // runtime_isa.cpp (on the ucontext_t)
int gow3gpu_handle_fault(void *ucontext, void *address);  // gpu/gow3gpu.h
void gow3gpu_dump_guest_writes(void *ucontext);
void gow3gpu_note_fatal_fault(void);                      // vk_pipeline_serialization.cpp: crash during the cache warm-up
}

namespace {

const HostLinuxGuest *guest;
uintptr_t exe_base;
int fault_trace;  // GOW3_FAULT_TRACE=1: the first 64 access violations are written to stdout
void watch_hit(const siginfo_t *info, const ucontext_t *uc);   // the exec watch's breakpoints (below)

uintptr_t image_base() { return guest && guest->image ? reinterpret_cast<uintptr_t>(*guest->image) : 0; }
bool in_image(uintptr_t address) {
    const uintptr_t image = image_base();
    return image && address - image < 0x20000000;
}
bool gpu_enabled() { return guest && guest->gpu_enabled && *guest->gpu_enabled; }

void put(const char *text, size_t size) {
    if (write(2, text, size) < 0) {}
}
void put(const char *text) { put(text, std::strlen(text)); }

// once per address and kind, as on Windows (at most 64 remembered)
bool first_time(uintptr_t (&seen)[64], int &count, uintptr_t at) {
    for (int k = 0; k < count; ++k)
        if (seen[k] == at) return false;
    if (count < 64) seen[count++] = at;
    return true;
}

void report(int sig, siginfo_t *info, ucontext_t *uc) {
    const uintptr_t rip = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
    char line[512];
    Dl_info where;
    if (in_image(rip))
        std::snprintf(line, sizeof line, "Guest fault (signal %d) at guest offset 0x%lx, address %p\n", sig,
                      static_cast<unsigned long>(rip - image_base()), info->si_addr);
    else if (dladdr(reinterpret_cast<void *>(rip), &where) && where.dli_fname)
        std::snprintf(line, sizeof line, "Host fault (signal %d) in %s+0x%lx (%s), address %p\n", sig, where.dli_fname,
                      static_cast<unsigned long>(rip - reinterpret_cast<uintptr_t>(where.dli_fbase)),
                      where.dli_sname ? where.dli_sname : "?", info->si_addr);
    else
        std::snprintf(line, sizeof line, "Fault (signal %d) at RIP %p, address %p\n", sig,
                      reinterpret_cast<void *>(rip), info->si_addr);
    put(line);
    if (gpu_enabled()) gow3gpu_dump_guest_writes(uc);
    // host call chain (frames with unwind info; guest frames end it)
    void *frames[32];
    const int depth = backtrace(frames, 32);
    for (int i = 2; i < depth; ++i) {
        if (dladdr(frames[i], &where) && where.dli_fname)
            std::snprintf(line, sizeof line, "  #%d %s+0x%lx (%s)\n", i, where.dli_fname,
                          static_cast<unsigned long>(reinterpret_cast<uintptr_t>(frames[i]) -
                                                     reinterpret_cast<uintptr_t>(where.dli_fbase)),
                          where.dli_sname ? where.dli_sname : "?");
        else
            std::snprintf(line, sizeof line, "  #%d %p\n", i, frames[i]);
        put(line);
    }
    // a crash on a pipeline cache warm-up thread: the next start clears the cache (as the Windows handler does)
    if (gpu_enabled()) gow3gpu_note_fatal_fault();
    _exit(128 + sig);
}

// How deep the fault handler runs inside itself on this thread (fault below).
thread_local int fault_depth;

void fault_level(int sig, siginfo_t *info, void *context);

void fault(int sig, siginfo_t *info, void *context) {
    // A fault in the handler itself is legitimate a level or two deep (a GPU handler touching another tracked page);
    // deeper than that is a bug: report and stop (SA_NODEFER lets the nested signal in). The count is kept by hand,
    // not by a destructor, because the recovery path leaves through siglongjmp.
    if (fault_depth >= 8) {
        put("Fault handler: faulted inside itself 8 levels deep\n");
        _exit(128 + sig);
    }
    ++fault_depth;
    fault_level(sig, info, context);
    --fault_depth;
}

void fault_level(int sig, siginfo_t *info, void *context) {
    ucontext_t *uc = static_cast<ucontext_t *>(context);
    greg_t &rip = uc->uc_mcontext.gregs[REG_RIP];
    // a hardware breakpoint of the exec watch (GOW3_EXEC_WATCH, a perf event): logged; the program goes on (the kernel
    // resumes an instruction breakpoint past itself)
    if (sig == SIGTRAP && info->si_code == TRAP_PERF) {
        watch_hit(info, uc);
        return;
    }
    // Uncharted 3, 10 Nov 2026: Naughty Dog's assertion trap, int 0x41 (cd 41), after the assertion's message. The
    // console returns from it and the game goes on; Linux reports it as a general protection fault (SIGSEGV from
    // the kernel). Stepped over and logged once per address.
    if (sig == SIGSEGV && info->si_code == SI_KERNEL && in_image(static_cast<uintptr_t>(rip))) {
        const uint8_t *p = reinterpret_cast<const uint8_t *>(rip);
        if (p[0] == 0xCD && p[1] == 0x41) {
            static uintptr_t seen[64];
            static int seen_count;
            const uintptr_t at = static_cast<uintptr_t>(rip) - image_base();
            if (first_time(seen, seen_count, at)) {
                std::fprintf(stderr, "Runtime: guest assertion trap (int 0x41) at guest offset 0x%" PRIxPTR
                             " (thread %ld), stepped over\n", at, static_cast<long>(gettid()));
                std::fflush(stderr);
            }
            rip += 2;
            return;
        }
    }
    // gow3 7 Nov 2026: a debug break (int3) in guest code that PS3's retail build compiled out. Linux reports it
    // after the instruction (rip is past the 0xcc). A lone int3 is stepped over and logged once per address; one
    // inside padding (code that jumped to a wrong place) ends the process with the report.
    if (sig == SIGTRAP && in_image(static_cast<uintptr_t>(rip) - 1)) {
        const uint8_t *p = reinterpret_cast<const uint8_t *>(rip) - 1;
        if (p[0] == 0xCC && p[1] != 0xCC && p[-1] != 0xCC) {
            static uintptr_t seen[64];
            static int seen_count;
            const uintptr_t at = reinterpret_cast<uintptr_t>(p) - image_base();
            if (first_time(seen, seen_count, at)) {
                const uint64_t *sp = reinterpret_cast<const uint64_t *>(uc->uc_mcontext.gregs[REG_RSP]);
                std::fprintf(stderr, "Runtime: guest debug break at guest offset 0x%" PRIxPTR " (thread %ld, return 0x%"
                             PRIxPTR "), stepped over\n", at, static_cast<long>(gettid()),
                             static_cast<uintptr_t>(sp[0]) - image_base());
                std::fflush(stderr);
            }
            return;
        }
        report(sig, info, uc);
    }
    // gow3 1 Nov 2026: an instruction this CPU lacks (runtime_isa.cpp, on the signal frame).
    if (sig == SIGILL && runtime_isa_emulate(context)) return;
    if (sig == SIGSEGV || sig == SIGBUS) {
        if (fault_trace > 0 && sig == SIGSEGV) {
            --fault_trace;
            const unsigned long error = static_cast<unsigned long>(uc->uc_mcontext.gregs[REG_ERR]);
            Dl_info where;
            const bool known = dladdr(reinterpret_cast<void *>(rip), &where) && where.dli_fname;
            const char *name = known ? std::strrchr(where.dli_fname, '/') : nullptr;
            char line[256];
            const int n = std::snprintf(
                line, sizeof line, "AV %s %p rip %p (%s+%#llx, guest %#llx) thread %ld\n",
                (error & 16) ? "exec" : (error & 2) ? "write" : "read", info->si_addr, reinterpret_cast<void *>(rip),
                name ? name + 1 : known ? where.dli_fname : "?",
                known ? static_cast<unsigned long long>(static_cast<uintptr_t>(rip) -
                                                         reinterpret_cast<uintptr_t>(where.dli_fbase)) : 0ULL,
                in_image(static_cast<uintptr_t>(rip)) ? static_cast<unsigned long long>(rip - image_base()) : 0ULL,
                static_cast<long>(gettid()));
            if (n > 0 && write(1, line, static_cast<size_t>(n) < sizeof line ? n : sizeof line - 1) < 0) {}
        }
        // GPU page tracking (write-protected guest pages) first, except during a speculative read (draw preparation
        // workers): resolving a GPU-modified page there waits for the GPU thread, which may wait for a lock that
        // worker holds. Such a fault takes the recovery path; the GPU thread does that draw.
        if (gpu_enabled() && sig == SIGSEGV && !runtime_fault_recover && gow3gpu_handle_fault(context, info->si_addr))
            return;
        // a speculative guest memory read (runtime_memory.c) failed: resume at its recovery point
        if (runtime_fault_recover) {
            sigjmp_buf *recover = runtime_fault_recover;
            runtime_fault_recover = nullptr;
            sigset_t unblock;
            sigemptyset(&unblock);
            sigaddset(&unblock, sig);
            pthread_sigmask(SIG_UNBLOCK, &unblock, nullptr);
            --fault_depth;
            siglongjmp(*recover, 1);
        }
    }
    report(sig, info, uc);
}

// Watchdog: RIP and the rbp frame chain of every thread (guest offsets when inside the image). Reads use
// process_vm_readv so bad frames cannot fault.
void write_hex(char *out, uint64_t v) {
    const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) out[i] = digits[(v >> (60 - i * 4)) & 15];
}
void dump_frames(ucontext_t *uc) {
    char line[] = "  tid=0000000000000000 rip=0000000000000000 image-relative=0000000000000000 host-relative=0000000000000000\n";
    uintptr_t rip = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]),
              rbp = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RBP]);
    const uint64_t tid = static_cast<uint64_t>(gettid());
    // first argument register: the lock address when a thread waits on a futex
    char arg[] = "  tid=0000000000000000 rdi=0000000000000000\n";
    write_hex(arg + 6, tid);
    write_hex(arg + 27, static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RDI]));
    put(arg, sizeof arg - 1);
    for (int level = 0; level < 24; ++level) {
        write_hex(line + 6, tid);
        write_hex(line + 27, rip);
        write_hex(line + 59, rip - image_base());
        write_hex(line + 90, rip - exe_base);
        put(line, sizeof line - 1);
        uintptr_t frame[2];
        struct iovec local = {frame, sizeof frame}, remote = {reinterpret_cast<void *>(rbp), sizeof frame};
        if (!rbp || process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != static_cast<ssize_t>(sizeof frame)) break;
        if (frame[0] <= rbp) break;
        rbp = frame[0];
        rip = frame[1];
    }
}
void thread_dump(int, siginfo_t *, void *context) { dump_frames(static_cast<ucontext_t *>(context)); }
void watchdog(int sig, siginfo_t *, void *context) {
    put("STOP: watchdog timeout; thread stacks:\n");
    dump_frames(static_cast<ucontext_t *>(context));
    const int dir = open("/proc/self/task", O_RDONLY | O_DIRECTORY);
    alignas(8) char buffer[4096];
    long n;
    const pid_t self = gettid();
    while (dir >= 0 && (n = syscall(SYS_getdents64, dir, buffer, sizeof buffer)) > 0)
        for (long at = 0; at < n;) {
            struct Entry {
                uint64_t ino;
                int64_t off;
                unsigned short reclen;
                unsigned char type;
                char name[1];
            };
            const Entry *d = reinterpret_cast<const Entry *>(buffer + at);
            const pid_t tid = static_cast<pid_t>(std::strtol(d->name, nullptr, 10));
            if (tid > 0 && tid != self) {
                syscall(SYS_tgkill, getpid(), tid, SIGUSR2);
                usleep(20000);
            }
            at += d->reclen;
        }
    usleep(100000);
    _exit(128 + sig);
}

void handle(int sig, void (*handler)(int, siginfo_t *, void *), int flags) {
    struct sigaction action = {};
    action.sa_sigaction = handler;
    action.sa_flags = SA_SIGINFO | flags;
    sigemptyset(&action.sa_mask);
    sigaction(sig, &action, nullptr);
}

}  // namespace

extern "C" void host_linux_install_signals(const HostLinuxGuest *g, unsigned timeout_seconds) {
    guest = g;
    const char *trace = std::getenv("GOW3_FAULT_TRACE");
    fault_trace = trace && trace[0] == '1' ? 64 : 0;
    Dl_info self;
    if (dladdr(reinterpret_cast<void *>(&host_linux_install_signals), &self)) exe_base = reinterpret_cast<uintptr_t>(self.dli_fbase);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP}) handle(sig, fault, SA_NODEFER);
    handle(SIGUSR2, thread_dump, SA_RESTART);
    handle(SIGALRM, watchdog, 0);
    alarm(timeout_seconds);  // 0: no watchdog
}

extern "C" void host_linux_restart(void) {
    // GOW3_RESTART_COMMAND (set by whoever started the game): the launch command line, run again (new patches).
    // Descriptors past stdout/stderr are closed first so the old GPU device and its memory go before the new process
    // opens its own; the output descriptors stay, so a launcher reading the log keeps reading the new launch.
    std::fflush(nullptr);
    const char *command = std::getenv("GOW3_RESTART_COMMAND");
    if (!command || !*command) {
        std::fputs("runtime_restart: GOW3_RESTART_COMMAND is not set\n", stderr);
        _exit(1);
    }
    std::puts("Runtime: restarting through GOW3_RESTART_COMMAND");
    syscall(SYS_close_range, 3u, ~0u, 0u);
    execl("/bin/sh", "sh", "-c", command, static_cast<char *>(nullptr));
    std::perror("runtime_restart: exec");
    _exit(1);
}

// ------------------------------------------------------------------------------------------------------ exec watch
// The Linux body of probe.c's exec watch (diagnostics): GOW3_EXEC_WATCH=<off>[,<off>...] (hex guest image offsets;
// w:<address> an 8-byte write watch, r:<address> a 4-byte read/write watch; up to 4), GOW3_EXEC_WATCH_FILE=<path> (the
// list, read every second while the game runs; a changed list arms every thread again), GOW3_EXEC_WATCH_SECONDS (how
// long new threads are armed, default 600). Windows sets the debug registers through each thread's context; here each
// thread gets one hardware breakpoint per watch through perf_event_open, with a synchronous SIGTRAP (TRAP_PERF) per
// hit, armed by a helper thread as threads appear (the guest main thread before guest code starts). A hit in guest
// code is logged as on Windows; "dr6" names the watch that hit (1 << its number).
namespace {

std::atomic<uint64_t> watch_list[4];       // as probe.c: bit 63 a write watch, bit 62 a read/write watch, else an offset
std::atomic<uintptr_t> watch_address[4];   // what each watch breaks on (the handler finds the watch by si_addr)
std::atomic<int> watch_count{0}, watch_hits{0};
std::atomic<unsigned> watch_generation{0};  // a new list: every thread is armed again
const char *watch_file;
char watch_file_text[256];

int parse_watch(const char *text, uint64_t out[4]) {
    int count = 0;
    for (const char *p = text; *p && count < 4;) {
        while (*p == ' ' || *p == '\n' || *p == '\r') ++p;
        bool data = false, read = false;
        if ((p[0] == 'w' || p[0] == 'W') && p[1] == ':') data = true, p += 2;
        else if ((p[0] == 'r' || p[0] == 'R') && p[1] == ':') read = true, p += 2;
        char *end = nullptr;
        const uint64_t v = std::strtoull(p, &end, 16);
        if (end == p) break;
        out[count++] = data ? v | (1ull << 63) : read ? v | (1ull << 62) : v;
        p = *end == ',' ? end + 1 : end;
    }
    return count;
}

// a new list: the addresses, and every thread to be armed again
void set_watches(const uint64_t list[4], int count) {
    for (int i = 0; i < 4; ++i) {
        const uint64_t w = i < count ? list[i] : 0;
        watch_list[i] = w;
        watch_address[i] = (w >> 62) == 1 ? (w & ~(3ull << 62) & ~3ull)
                           : (w >> 63)    ? (w & ~(1ull << 63) & ~7ull)
                                          : image_base() + w;
    }
    watch_count = count;
    ++watch_generation;
}

int open_watch(pid_t tid, int i) {
    const uint64_t w = watch_list[i];
    perf_event_attr a{};
    a.type = PERF_TYPE_BREAKPOINT;
    a.size = sizeof a;
    if ((w >> 62) == 1) a.bp_type = HW_BREAKPOINT_RW, a.bp_len = HW_BREAKPOINT_LEN_4;
    else if (w >> 63) a.bp_type = HW_BREAKPOINT_W, a.bp_len = HW_BREAKPOINT_LEN_8;
    else a.bp_type = HW_BREAKPOINT_X, a.bp_len = sizeof(long);
    a.bp_addr = watch_address[i];
    a.sample_period = 1;
    a.exclude_kernel = 1;
    a.exclude_hv = 1;
    a.sigtrap = 1;          // the hit as a synchronous SIGTRAP on the thread that made it
    a.remove_on_exec = 1;   // (sigtrap needs it)
    return static_cast<int>(syscall(SYS_perf_event_open, &a, tid, -1, -1, PERF_FLAG_FD_CLOEXEC));
}

void poll_watch_file() {
    if (!watch_file) return;
    FILE *f = std::fopen(watch_file, "rb");
    if (!f) return;
    char text[256] = {};
    const size_t n = std::fread(text, 1, sizeof text - 1, f);
    std::fclose(f);
    text[n] = 0;
    if (!std::strcmp(text, watch_file_text)) return;
    std::strcpy(watch_file_text, text);
    uint64_t list[4] = {};
    const int count = parse_watch(text, list);
    set_watches(list, count);
    watch_hits = 0;
    std::printf("Exec watch: %d watches from %s: %s\n", count, watch_file, text);
    std::fflush(stdout);
}

void watch_thread(std::promise<void> ready) {
    struct Armed {
        pid_t tid;
        unsigned generation;
        int fds[4];
        bool seen;
    };
    std::vector<Armed> armed;
    const pid_t self = static_cast<pid_t>(gettid());
    const auto start = std::chrono::steady_clock::now();
    const char *secs = std::getenv("GOW3_EXEC_WATCH_SECONDS");
    const auto limit = std::chrono::seconds(secs && std::atoi(secs) > 0 ? std::atoi(secs) : 600);
    bool warned = false;
    for (int round = 0; watch_file || std::chrono::steady_clock::now() - start < limit; ++round) {
        if (round % 50 == 0) poll_watch_file();
        const unsigned generation = watch_generation;
        for (Armed &a : armed) a.seen = false;
        if (DIR *dir = opendir("/proc/self/task")) {
            while (dirent *e = readdir(dir)) {
                const pid_t tid = static_cast<pid_t>(std::atoi(e->d_name));
                if (tid <= 0 || tid == self) continue;
                Armed *a = nullptr;
                for (Armed &known : armed)
                    if (known.tid == tid) a = &known;
                if (a && a->generation == generation) {
                    a->seen = true;
                    continue;
                }
                if (!a) {
                    if (armed.size() >= 8192) continue;
                    armed.push_back({tid, 0, {-1, -1, -1, -1}, false});
                    a = &armed.back();
                }
                for (int &fd : a->fds)
                    if (fd >= 0) ::close(fd), fd = -1;
                const int count = watch_count;
                for (int i = 0; i < count; ++i) {
                    a->fds[i] = open_watch(tid, i);
                    if (a->fds[i] < 0 && !warned) {
                        warned = true;
                        std::printf("Exec watch: no hardware breakpoint for thread %d (perf_event_open: %s; "
                                    "/proc/sys/kernel/perf_event_paranoid must be 2 or less)\n",
                                    static_cast<int>(tid), std::strerror(errno));
                        std::fflush(stdout);
                    }
                }
                a->generation = generation;
                a->seen = true;
            }
            closedir(dir);
        }
        // threads that ended: their events went with them
        for (size_t i = 0; i < armed.size();) {
            if (armed[i].seen) {
                ++i;
                continue;
            }
            for (int fd : armed[i].fds)
                if (fd >= 0) ::close(fd);
            armed[i] = armed.back();
            armed.pop_back();
        }
        if (round == 0) ready.set_value();
        std::this_thread::sleep_for(std::chrono::milliseconds(round < 200 ? 1 : 20));
    }
    std::printf("Exec watch: done (%zu threads armed)\n", armed.size());
    std::fflush(stdout);
}

// printable bytes at address (a checked read), for the log
void watch_string(char *out, size_t size, uint64_t address) {
    unsigned char buf[64] = {};
    out[0] = 0;
    iovec local{buf, sizeof buf - 1}, remote{reinterpret_cast<void *>(address), sizeof buf - 1};
    if (address < 0x10000 || process_vm_readv(getpid(), &local, 1, &remote, 1, 0) <= 0) return;
    size_t n = 0;
    while (n + 1 < size && n < sizeof buf - 1 && buf[n] >= 0x20 && buf[n] < 0x7f) out[n] = static_cast<char>(buf[n]), ++n;
    out[n] = 0;
}

void watch_hit(const siginfo_t *info, const ucontext_t *uc) {
    const uintptr_t at = reinterpret_cast<uintptr_t>(info->si_addr);
    uint64_t dr6 = 0;
    for (int i = 0; i < watch_count; ++i)
        if (watch_address[i] == at) dr6 |= 1ull << i;
    const greg_t *r = uc->uc_mcontext.gregs;
    const uintptr_t image = image_base(), rip = static_cast<uintptr_t>(r[REG_RIP]);
    // only guest code is logged: runtime copies of the watched memory would fill the 400 lines
    if (!image || rip - image >= 0x2000000 || watch_hits.fetch_add(1) + 1 > 400) return;
    char line[1024], s1[64], s2[64];
    watch_string(s1, sizeof s1, static_cast<uint64_t>(r[REG_RSI]));
    watch_string(s2, sizeof s2, static_cast<uint64_t>(r[REG_RDX]));
    int n = std::snprintf(line, sizeof line,
                          "Watch: dr6 %#llx rip guest %#llx rax %#llx rcx %#llx rdx %#llx rsi %#llx rdi %#llx r8 %#llx "
                          "r9 %#llx [rsi '%s'] [rdx '%s'] callers",
                          static_cast<unsigned long long>(dr6), static_cast<unsigned long long>(rip - image),
                          static_cast<unsigned long long>(r[REG_RAX]), static_cast<unsigned long long>(r[REG_RCX]),
                          static_cast<unsigned long long>(r[REG_RDX]), static_cast<unsigned long long>(r[REG_RSI]),
                          static_cast<unsigned long long>(r[REG_RDI]), static_cast<unsigned long long>(r[REG_R8]),
                          static_cast<unsigned long long>(r[REG_R9]), s1, s2);
    uint64_t stack[64] = {};
    iovec local{stack, sizeof stack}, remote{reinterpret_cast<void *>(r[REG_RSP]), sizeof stack};
    const ssize_t got = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    for (int i = 0, found = 0; got > 0 && i < static_cast<int>(got / 8) && found < 6 && n < static_cast<int>(sizeof line) - 24; ++i)
        if (stack[i] - image < 0x2000000) {
            n += std::snprintf(line + n, sizeof line - static_cast<size_t>(n), " %#llx", static_cast<unsigned long long>(stack[i] - image));
            ++found;
        }
    line[n++] = '\n';
    if (write(1, line, static_cast<size_t>(n)) < 0) {}
}

}  // namespace

extern "C" void host_linux_start_exec_watch(void) {
    const char *env = std::getenv("GOW3_EXEC_WATCH");
    watch_file = std::getenv("GOW3_EXEC_WATCH_FILE");
    if ((!env || !*env) && !watch_file) return;
    if (env) {
        uint64_t list[4] = {};
        const int count = parse_watch(env, list);
        set_watches(list, count);
    }
    std::promise<void> ready;
    std::future<void> armed = ready.get_future();
    std::thread(watch_thread, std::move(ready)).detach();
    armed.wait_for(std::chrono::seconds(5));
    std::printf("Exec watch: %d breakpoints armed from %s\n", watch_count.load(), env ? env : "(file)");
    std::fflush(stdout);   // before the hits, which the handler writes straight to stdout
}

// Diagnostics: points data watch 0 at `address` (8-byte writes) and arms every thread again (needs GOW3_EXEC_WATCH set
// at start, e.g. w:0). Called by the GPU library's checks (GOW3_SRT_WATCH).
extern "C" void runtime_exec_watch_retarget(uint64_t address) {
    uint64_t list[4];
    for (int i = 0; i < 4; ++i) list[i] = watch_list[i];
    list[0] = address | (1ull << 63);
    set_watches(list, std::max(1, watch_count.load()));
    std::printf("Exec watch: data watch moved to %#llx\n", static_cast<unsigned long long>(address));
    std::fflush(stdout);
}

extern "C" unsigned char *host_linux_alloc_between(uintptr_t lo, uintptr_t hi, size_t bytes, int prot) {
    constexpr uintptr_t granule = 0x10000;
    // the mapped ranges, in address order (/proc/self/maps lists them sorted)
    std::vector<std::pair<uintptr_t, uintptr_t>> used;
    std::ifstream maps("/proc/self/maps");
    for (std::string line; std::getline(maps, line);) {
        unsigned long long start = 0, end = 0;
        if (std::sscanf(line.c_str(), "%llx-%llx", &start, &end) == 2) used.emplace_back(start, end);
    }
    uintptr_t free_from = lo;
    used.emplace_back(UINTPTR_MAX, UINTPTR_MAX);
    for (const auto &[start, end] : used) {
        if (end <= free_from) continue;
        if (start > free_from) {
            const uintptr_t at = (free_from + granule - 1) & ~(granule - 1);
            if (at >= hi) break;
            if (at + bytes <= start && at + bytes <= hi) {
                void *p = mmap(reinterpret_cast<void *>(at), bytes, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                               -1, 0);
                if (p != MAP_FAILED) return static_cast<unsigned char *>(p);
            }
        }
        if (end > free_from) free_from = end;
        if (free_from >= hi) break;
    }
    return nullptr;
}

extern "C" int host_linux_fold_case(char *path, size_t fixed_length) {
    struct stat st;
    if (!path || lstat(path, &st) == 0 || errno != ENOENT) return 0;
    const size_t length = std::strlen(path);
    if (fixed_length >= length) return 0;
    std::string out(path, fixed_length);
    size_t at = fixed_length;
    bool changed = false;
    while (at < length) {
        if (path[at] == '/') {
            out += '/';
            ++at;
            continue;
        }
        size_t end = at;
        while (end < length && path[end] != '/') ++end;
        const std::string name(path + at, end - at);
        std::string candidate = out + name;
        if (lstat(candidate.c_str(), &st) != 0) {
            // the folder's entry with this name in another case, if there is one
            DIR *d = opendir(out.empty() ? "/" : out.c_str());
            std::string found;
            if (d) {
                while (const dirent *e = readdir(d))
                    if (strcasecmp(e->d_name, name.c_str()) == 0 && std::strlen(e->d_name) == name.size()) {
                        found = e->d_name;
                        break;
                    }
                closedir(d);
            }
            if (found.empty()) {  // not there in any case: the rest stays as written
                out.append(path + at, length - at);
                break;
            }
            candidate = out + found;
            changed = true;
        }
        out = std::move(candidate);
        at = end;
    }
    if (!changed) return 0;
    std::memcpy(path, out.data(), out.size());  // same length: only the case of ASCII letters differs
    return 1;
}
