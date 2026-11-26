// host_linux.cpp - the runtime's Linux platform functions (host_linux.h). Windows keeps its own bodies in probe.c and
// runtime_file.c and does not build this file.
//
// Signals: the counterpart of probe.c's Windows vectored exception handler. In order: Naughty Dog's assertion trap
// (int 0x41) and a lone debug break (int3) in guest code are stepped over and logged once per address, an instruction
// this CPU lacks goes to runtime_isa.c, a write to a page the GPU library tracks goes to it, a speculative guest read
// (runtime_fault_recover) resumes at its recovery point; anything else ends the process with a report.
#include "host_linux.h"

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <dirent.h>
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
int runtime_isa_emulate(void *context);                  // runtime_isa.c (inactive on Linux)
int gow3gpu_handle_fault(void *ucontext, void *address);  // gpu/gow3gpu.h
void gow3gpu_dump_guest_writes(void *ucontext);
}

namespace {

const HostLinuxGuest *guest;
uintptr_t exe_base;
int fault_trace;  // GOW3_FAULT_TRACE=1: the first 64 access violations are written to stdout

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
    _exit(128 + sig);
}

void fault(int sig, siginfo_t *info, void *context) {
    ucontext_t *uc = static_cast<ucontext_t *>(context);
    greg_t &rip = uc->uc_mcontext.gregs[REG_RIP];
    // A fault in the handler itself is legitimate a level or two deep (a GPU handler touching another tracked page);
    // deeper than that is a bug: report and stop (SA_NODEFER lets the nested signal in).
    static thread_local int depth;
    if (depth >= 8) {
        put("Fault handler: faulted inside itself 8 levels deep\n");
        _exit(128 + sig);
    }
    struct Level {
        Level() { ++depth; }
        ~Level() { --depth; }
    } level;
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
    // gow3 1 Nov 2026: an instruction this CPU lacks (runtime_isa.c; its Linux body is not active yet).
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
            --depth;
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
