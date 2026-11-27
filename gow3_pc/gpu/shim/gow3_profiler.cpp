// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: a sampling profiler for one thread (GOW3_PROFILE=<seconds per report>), to find where the
// GPU command thread spends its time and what grows when the frame rate falls over minutes. A
// thread takes a sample of the target about once per millisecond: its instruction pointer and call
// stack (nothing is allocated while it is taken). Windows suspends the target, reads its context and
// resumes it; Linux sends it SIGPROF, and its handler reads the signal frame. Each report lists the
// sampled addresses as module+offset with their counts in GOW3_DUMP_DIR/profile_<n>.txt; offsets into
// the runtime's executable are symbolized offline (llvm-symbolizer, image base 0x140000000 on Windows).
// profile_<n>_stacks.txt holds the call stacks (up to 16 frames, innermost first, counted per
// distinct stack), so a wait shows who waits. Windows unwinds host frames with their unwind tables and
// guest code (no module, "?+address") by its saved frame pointers; Linux follows the saved frame
// pointers throughout (the runtime is built with them).
// GOW3_PROFILE_STALLS=1: no periodic reports; each sampler keeps its last half second of stacks,
// and a frame over 40 ms (NoteStall, from the flip statistics) writes the stacks sampled during
// that frame to GOW3_DUMP_DIR/stall_<n>_<thread>_stacks.txt.
#include "gow3_profiler.h"

#include <atomic>
#include <deque>
#include <mutex>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <cerrno>
#include <csignal>
#include <dlfcn.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>
#endif

namespace Gow3Profiler {

namespace {
std::atomic<unsigned> stall_seq{0};
std::atomic<long long> stall_end_ns{0}, stall_window_ns{0};
std::chrono::steady_clock::time_point stall_epoch = std::chrono::steady_clock::now();

bool StallMode() {
    static const bool on = [] {
        const char* env = std::getenv("GOW3_PROFILE_STALLS");
        return env && env[0] == '1';
    }();
    return on;
}
} // namespace

void NoteStall(double frame_ms) {
    if (!StallMode()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    stall_end_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - stall_epoch).count();
    stall_window_ns = static_cast<long long>(frame_ms * 1e6);
    ++stall_seq;
}

// ---- the system's part: a thread to sample (Target), one sample of it (Sample), an address's name (NameOf) ----
#ifdef _WIN32
namespace {

constexpr int MaxDepth = 16;

struct Stack {
    unsigned long long pc[MaxDepth];
    int depth;
};

bool Read(unsigned long long address, void* out, SIZE_T size) {
    SIZE_T done = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), out, size, &done) &&
           done == size;
}

/// gow3 16 Oct: the loaded modules' unwind tables (.pdata), read while the sampled thread runs.
/// RtlLookupFunctionEntry takes the function table lock: when the suspended thread held it (an
/// exception being dispatched, e.g. a guest page fault), the profiler waited for it forever with
/// the thread suspended and the game froze after the first report.
struct Module {
    unsigned long long base, end;
    const RUNTIME_FUNCTION* table;
    DWORD count;
};

std::vector<Module> LoadModules() {
    std::vector<Module> out;
    HMODULE handles[1024];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), handles, sizeof(handles), &needed)) {
        return out;
    }
    for (DWORD i = 0; i < std::min<DWORD>(needed / sizeof(HMODULE), 1024); ++i) {
        MODULEINFO info{};
        if (!GetModuleInformation(GetCurrentProcess(), handles[i], &info, sizeof(info))) {
            continue;
        }
        const auto base = reinterpret_cast<unsigned long long>(info.lpBaseOfDll);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        out.push_back({base, base + info.SizeOfImage,
                       dir.Size ? reinterpret_cast<const RUNTIME_FUNCTION*>(base + dir.VirtualAddress)
                                : nullptr,
                       DWORD(dir.Size / sizeof(RUNTIME_FUNCTION))});
    }
    std::sort(out.begin(), out.end(), [](const Module& a, const Module& b) { return a.base < b.base; });
    return out;
}

/// The unwind entry for `rip` without locks; image_base is set when rip is in a module.
const RUNTIME_FUNCTION* LookupFunction(const std::vector<Module>& modules, unsigned long long rip,
                                       DWORD64& image_base) {
    image_base = 0;
    auto it = std::upper_bound(modules.begin(), modules.end(), rip,
                               [](unsigned long long value, const Module& m) { return value < m.base; });
    if (it == modules.begin()) {
        return nullptr;
    }
    --it;
    if (rip >= it->end) {
        return nullptr;
    }
    image_base = it->base;
    const DWORD offset = DWORD(rip - it->base);
    DWORD lo = 0, hi = it->count;
    while (lo < hi) {
        const DWORD mid = (lo + hi) / 2;
        const RUNTIME_FUNCTION& f = it->table[mid];
        if (offset < f.BeginAddress) {
            hi = mid;
        } else if (offset >= f.EndAddress) {
            lo = mid + 1;
        } else {
            const RUNTIME_FUNCTION* entry = &f;
            // An indirect entry (low bit set) names the entry that holds the unwind data, as
            // RtlLookupFunctionEntry resolves it (system DLLs have them).
            if (entry->UnwindData & 1) {
                const unsigned long long target = it->base + (entry->UnwindData & ~DWORD(1));
                if (target < it->base || target + sizeof(RUNTIME_FUNCTION) > it->end) {
                    return nullptr;
                }
                entry = reinterpret_cast<const RUNTIME_FUNCTION*>(target);
            }
            // The unwind info must lie in the image and be a known version (1 or 2).
            const unsigned long long info = it->base + entry->UnwindData;
            unsigned char header = 0;
            if (info < it->base || info + 4 > it->end || !Read(info, &header, 1) ||
                (header & 7) == 0 || (header & 7) > 2) {
                return nullptr;
            }
            return entry;
        }
    }
    return nullptr;
}

/// Walks the suspended thread's stack (no allocation and no locks: the thread may hold the heap
/// or the function table lock).
/// One x64 unwind step from the function's unwind codes, every stack read checked (gow3 16 Oct:
/// RtlVirtualUnwind reads the stack directly; a frame register holding garbage in a sampled
/// frame took the game down with a read of address 0xa0). Epilogues are not detected: a sample
/// taken in one may unwind wrongly and ends early. Returns false when the walk has to stop.
bool UnwindStep(const RUNTIME_FUNCTION* function, DWORD64 image_base, CONTEXT& context) {
    // Register numbers of the unwind codes: rax rcx rdx rbx rsp rbp rsi rdi r8-r15.
    unsigned long long* regs[16] = {&context.Rax, &context.Rcx, &context.Rdx, &context.Rbx,
                                    &context.Rsp, &context.Rbp, &context.Rsi, &context.Rdi,
                                    &context.R8,  &context.R9,  &context.R10, &context.R11,
                                    &context.R12, &context.R13, &context.R14, &context.R15};
    const unsigned long long offset = context.Rip - image_base - function->BeginAddress;
    bool primary = true;
    for (int chain = 0; chain < 8; ++chain, primary = false) {
        const unsigned long long info = image_base + function->UnwindData;
        unsigned char header[4];
        if (!Read(info, header, sizeof(header))) {
            return false;
        }
        const unsigned version = header[0] & 7, flags = header[0] >> 3, count = header[2];
        const unsigned frame_reg = header[3] & 15, frame_offset = header[3] >> 4;
        if (version == 0 || version > 2) {
            return false;
        }
        unsigned short codes[256];
        const unsigned slots = (count + 1) & ~1u; // padded to an even count before a chain entry
        if (slots && !Read(info + 4, codes, slots * sizeof(unsigned short))) {
            return false;
        }
        for (unsigned i = 0; i < count;) {
            const unsigned code_offset = codes[i] & 0xff, op = (codes[i] >> 8) & 15, op_info = codes[i] >> 12;
            unsigned used = 1;
            unsigned extra = 0;
            switch (op) {
            case 1: used = op_info == 0 ? 2 : 3; break;
            case 4: case 8: used = 2; break;
            case 5: case 9: used = 3; break;
            case 6: used = version == 2 ? 1 : 2; break; // v2: epilogue descriptor
            default: break;
            }
            if (i + used > count) {
                return false;
            }
            // Prologue operations not yet executed at this offset (primary entry only).
            const bool executed = !primary || op == 6 || offset >= code_offset;
            if (executed) {
                unsigned long long& rsp = context.Rsp;
                switch (op) {
                case 0: // UWOP_PUSH_NONVOL
                    if (!Read(rsp, regs[op_info], 8)) {
                        return false;
                    }
                    rsp += 8;
                    break;
                case 1: // UWOP_ALLOC_LARGE
                    extra = op_info == 0 ? codes[i + 1] * 8u : (codes[i + 1] | (unsigned(codes[i + 2]) << 16));
                    rsp += extra;
                    break;
                case 2: // UWOP_ALLOC_SMALL
                    rsp += op_info * 8 + 8;
                    break;
                case 3: // UWOP_SET_FPREG
                    rsp = *regs[frame_reg] - frame_offset * 16ull;
                    break;
                case 4: // UWOP_SAVE_NONVOL
                    if (!Read(rsp + codes[i + 1] * 8ull, regs[op_info], 8)) {
                        return false;
                    }
                    break;
                case 5: // UWOP_SAVE_NONVOL_FAR
                    if (!Read(rsp + (codes[i + 1] | (unsigned(codes[i + 2]) << 16)), regs[op_info], 8)) {
                        return false;
                    }
                    break;
                case 10: { // UWOP_PUSH_MACHFRAME: the interrupted frame (rip, cs, eflags, rsp, ss)
                    const unsigned long long frame = rsp + (op_info ? 8 : 0);
                    unsigned long long rip = 0, old_rsp = 0;
                    if (!Read(frame, &rip, 8) || !Read(frame + 24, &old_rsp, 8)) {
                        return false;
                    }
                    context.Rip = rip;
                    rsp = old_rsp;
                    return true;
                }
                default: // xmm saves, epilogue descriptors: no general register changes
                    break;
                }
            }
            i += used;
        }
        if (!(flags & UNW_FLAG_CHAININFO)) {
            break;
        }
        // The chained entry follows the (padded) codes.
        static thread_local RUNTIME_FUNCTION chained;
        if (!Read(info + 4 + slots * sizeof(unsigned short), &chained, sizeof(chained))) {
            return false;
        }
        function = &chained;
    }
    unsigned long long ret = 0;
    if (!Read(context.Rsp, &ret, sizeof(ret))) {
        return false;
    }
    context.Rip = ret;
    context.Rsp += 8;
    return true;
}

void Walk(const std::vector<Module>& modules, CONTEXT context, Stack& stack) {
    const unsigned long long low = context.Rsp, high = context.Rsp + (8ull << 20);
    stack.depth = 0;
    while (stack.depth < MaxDepth && context.Rip) {
        stack.pc[stack.depth++] = context.Rip;
        DWORD64 image_base = 0;
        if (const RUNTIME_FUNCTION* function = LookupFunction(modules, context.Rip, image_base)) {
            if (!UnwindStep(function, image_base, context)) {
                break;
            }
        } else if (image_base) { // a leaf function: the return address is on top of the stack
            unsigned long long ret = 0;
            if (!Read(context.Rsp, &ret, sizeof(ret))) {
                break;
            }
            context.Rip = ret;
            context.Rsp += 8;
        } else { // guest code: saved frame pointers, if it keeps them
            unsigned long long frame[2];
            if (context.Rbp < low || context.Rbp >= high || !Read(context.Rbp, frame, sizeof(frame))) {
                break;
            }
            context.Rip = frame[1];
            context.Rsp = context.Rbp + 16;
            context.Rbp = frame[0];
        }
        if (context.Rsp < low || context.Rsp >= high) {
            break;
        }
    }
}

struct Target {
    HANDLE thread = nullptr;
    std::vector<Module> modules;
};

bool Open(unsigned long thread_id, Target& target) {
    target.thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                               thread_id);
    if (!target.thread) {
        return false;
    }
    target.modules = LoadModules();
    return true;
}

/// One sample: false when the thread is gone (the sampler ends); `taken` false when this one failed.
bool Sample(Target& target, unsigned long long& rip, Stack& stack, bool& taken) {
    if (SuspendThread(target.thread) == DWORD(-1)) {
        return false;
    }
    CONTEXT context{};
    context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    taken = GetThreadContext(target.thread, &context) != 0;
    if (taken) {
        rip = context.Rip;
        Walk(target.modules, context, stack);
    }
    ResumeThread(target.thread);
    return true;
}

void Refresh(Target& target) {
    target.modules = LoadModules(); // modules loaded since (the thread runs now)
}

void Close(Target& target) {
    CloseHandle(target.thread);
}

/// module+offset ("?+address" outside every module)
std::string NameOf(unsigned long long rip) {
    HMODULE module = nullptr;
    char module_name[MAX_PATH] = "?";
    unsigned long long offset = rip;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(rip), &module) &&
        module) {
        GetModuleFileNameA(module, module_name, sizeof(module_name));
        offset = rip - reinterpret_cast<unsigned long long>(module);
    }
    const char* base = std::strrchr(module_name, '\\');
    char text[MAX_PATH + 32];
    std::snprintf(text, sizeof(text), "%s+%#llx", base ? base + 1 : module_name, offset);
    return text;
}

unsigned long CurrentThread() {
    return GetCurrentThreadId();
}

} // namespace
#else
namespace {

constexpr int MaxDepth = 16;

struct Stack {
    unsigned long long pc[MaxDepth];
    int depth;
};

/// A read that fails instead of faulting (process_vm_readv on this process; safe in a signal handler).
bool Read(unsigned long long address, void* out, size_t size) {
    iovec local{out, size}, remote{reinterpret_cast<void*>(address), size};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
}

/// The saved frame pointers from the interrupted frame, every stack read checked.
void Walk(unsigned long long rip, unsigned long long rsp, unsigned long long rbp, Stack& stack) {
    const unsigned long long low = rsp, high = rsp + (8ull << 20);
    stack.depth = 0;
    while (stack.depth < MaxDepth && rip) {
        stack.pc[stack.depth++] = rip;
        unsigned long long frame[2];
        if (rbp < low || rbp >= high || !Read(rbp, frame, sizeof(frame))) {
            break;
        }
        rip = frame[1];
        rsp = rbp + 16;
        rbp = frame[0];
        if (rsp < low || rsp >= high) {
            break;
        }
    }
}

/// One sample in flight per sampled thread: the sampler sets `asked` to a new number and sends SIGPROF; the
/// thread's handler fills rip and stack and sets `done` to that number.
struct Slot {
    std::atomic<long> tid{0};
    std::atomic<unsigned> asked{0}, done{0};
    unsigned long long rip;
    Stack stack;
};
constexpr int MaxSlots = 64;
Slot slots[MaxSlots];

void OnSample(int, siginfo_t*, void* context) {
    const int saved_errno = errno;
    const long me = syscall(SYS_gettid);
    for (Slot& slot : slots) {
        if (slot.tid.load(std::memory_order_acquire) != me) {
            continue;
        }
        const unsigned asked = slot.asked.load(std::memory_order_acquire);
        if (asked != slot.done.load(std::memory_order_relaxed)) {
            const auto* uc = static_cast<const ucontext_t*>(context);
            slot.rip = static_cast<unsigned long long>(uc->uc_mcontext.gregs[REG_RIP]);
            Walk(slot.rip, static_cast<unsigned long long>(uc->uc_mcontext.gregs[REG_RSP]),
                 static_cast<unsigned long long>(uc->uc_mcontext.gregs[REG_RBP]), slot.stack);
            slot.done.store(asked, std::memory_order_release);
        }
        break;
    }
    errno = saved_errno;
}

struct Target {
    long tid = 0;
    Slot* slot = nullptr;
};

bool Open(unsigned long thread_id, Target& target) {
    static std::once_flag handler;
    std::call_once(handler, [] {
        struct sigaction sa {};
        sa.sa_sigaction = OnSample;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGPROF, &sa, nullptr);
    });
    for (Slot& slot : slots) {
        long free_slot = 0;
        if (slot.tid.compare_exchange_strong(free_slot, static_cast<long>(thread_id))) {
            target.tid = static_cast<long>(thread_id);
            target.slot = &slot;
            return true;
        }
    }
    return false;
}

/// One sample: false when the thread is gone or no longer answers (the sampler ends).
bool Sample(Target& target, unsigned long long& rip, Stack& stack, bool& taken) {
    Slot& slot = *target.slot;
    const unsigned number = slot.asked.load(std::memory_order_relaxed) + 1;
    slot.asked.store(number, std::memory_order_release);
    if (syscall(SYS_tgkill, getpid(), target.tid, SIGPROF) != 0) {
        return false;
    }
    // the handler runs at the thread's next return to user space; a thread that does not answer in a second has
    // the signal blocked: stop sampling it
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (slot.done.load(std::memory_order_acquire) != number) {
        if (std::chrono::steady_clock::now() > until) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(20));
    }
    rip = slot.rip;
    stack = slot.stack;
    taken = true;
    return true;
}

void Refresh(Target&) {}

void Close(Target& target) {
    // the handler must not fill a slot handed on: the thread id leaves it first
    target.slot->tid.store(0, std::memory_order_release);
}

/// module+offset ("?+address" outside every module)
std::string NameOf(unsigned long long rip) {
    Dl_info where{};
    const char* module_name = "?";
    unsigned long long offset = rip;
    if (dladdr(reinterpret_cast<void*>(rip), &where) && where.dli_fname && where.dli_fbase) {
        const char* base = std::strrchr(where.dli_fname, '/');
        module_name = base ? base + 1 : where.dli_fname;
        offset = rip - reinterpret_cast<unsigned long long>(where.dli_fbase);
    }
    char text[512];
    std::snprintf(text, sizeof(text), "%s+%#llx", module_name, offset);
    return text;
}

unsigned long CurrentThread() {
    return static_cast<unsigned long>(syscall(SYS_gettid));
}

} // namespace
#endif

void Start(unsigned long thread_id, const char* name) {
    const char* env = std::getenv("GOW3_PROFILE");
    if (!env || !*env || std::atoi(env) <= 0) {
        return;
    }
    const int period = std::max(5, std::atoi(env));
    const char* dir_env = std::getenv("GOW3_DUMP_DIR");
    const std::string dir = dir_env && *dir_env ? dir_env : ".";
    std::thread([thread_id, period, dir, label = std::string(name)] {
        Target target;
        if (!Open(thread_id, target)) {
            std::printf("Profiler: cannot open thread %lu\n", thread_id);
            return;
        }
        std::printf("Profiler: sampling %s (thread %lu), a report every %d s\n", label.c_str(),
                    thread_id, period);
        std::fflush(stdout);
        std::vector<unsigned long long> samples;
        samples.reserve(size_t(period) * 2000);
        std::vector<Stack> stacks;
        stacks.reserve(samples.capacity());
        auto last = std::chrono::steady_clock::now();
        // Stall mode: the recent samples (time since stall_epoch, stack).
        std::deque<std::pair<long long, Stack>> recent;
        unsigned seen_stall = stall_seq.load();
        for (int report = 0;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            unsigned long long rip = 0;
            Stack stack{};
            bool taken = false;
            if (!Sample(target, rip, stack, taken)) {
                break;
            }
            if (taken && StallMode()) {
                recent.emplace_back(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now() - stall_epoch)
                                        .count(),
                                    stack);
                while (recent.size() > 600) {
                    recent.pop_front();
                }
            } else if (taken && samples.size() < samples.capacity()) {
                samples.push_back(rip);
                stacks.push_back(stack);
            }
            if (StallMode()) {
                const unsigned seq = stall_seq.load();
                if (seq == seen_stall) {
                    continue;
                }
                seen_stall = seq;
                const long long end = stall_end_ns.load(), begin = end - stall_window_ns.load();
                std::unordered_map<std::string, unsigned> counts;
                unsigned total = 0;
                for (const auto& [when, sampled] : recent) {
                    if (when < begin || when > end) {
                        continue;
                    }
                    std::string key;
                    for (int i = 0; i < sampled.depth; ++i) {
                        key += (i ? ";" : "") + NameOf(sampled.pc[i]);
                    }
                    ++counts[key];
                    ++total;
                }
                std::string tag = label;
                for (char& c : tag) {
                    c = c == ' ' ? '-' : c;
                }
                const std::string path = dir + "/stall_" + std::to_string(seq) + "_" + tag + "_stacks.txt";
                if (FILE* f = std::fopen(path.c_str(), "w")) {
                    std::vector<std::pair<unsigned, std::string>> sorted;
                    for (auto& [key, n] : counts) {
                        sorted.emplace_back(n, key);
                    }
                    std::sort(sorted.rbegin(), sorted.rend());
                    std::fprintf(f, "# %s, %u samples in the %.1f ms frame, stacks innermost first\n",
                                 label.c_str(), total, stall_window_ns.load() / 1e6);
                    for (const auto& [n, key] : sorted) {
                        std::fprintf(f, "%u %s\n", n, key.c_str());
                    }
                    std::fclose(f);
                }
                continue;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - last < std::chrono::seconds(period)) {
                continue;
            }
            last = now;
            std::unordered_map<unsigned long long, unsigned> counts;
            for (const auto sampled : samples) {
                ++counts[sampled];
            }
            std::vector<std::pair<unsigned, unsigned long long>> sorted;
            for (const auto& [sampled, n] : counts) {
                sorted.emplace_back(n, sampled);
            }
            std::sort(sorted.rbegin(), sorted.rend());
            std::string tag = label;
            for (char& c : tag) {
                c = c == ' ' ? '-' : c;
            }
            std::unordered_map<unsigned long long, std::string> names;
            const auto name_of = [&names](unsigned long long address) -> const std::string& {
                auto [it, added] = names.try_emplace(address);
                if (added) {
                    it->second = NameOf(address);
                }
                return it->second;
            };
            const std::string stem = dir + "/profile_" + tag + "_" + std::to_string(report++);
            const std::string path = stem + ".txt";
            if (FILE* f = std::fopen(path.c_str(), "w")) {
                std::fprintf(f, "# %s, %zu samples in %d s\n", label.c_str(), samples.size(), period);
                for (const auto& [n, address] : sorted) {
                    std::fprintf(f, "%u %s\n", n, name_of(address).c_str());
                }
                std::fclose(f);
                std::printf("Profiler: %zu samples written to %s\n", samples.size(), path.c_str());
                std::fflush(stdout);
            }
            std::unordered_map<std::string, unsigned> stack_counts;
            for (const Stack& sampled : stacks) {
                std::string key;
                for (int i = 0; i < sampled.depth; ++i) {
                    key += (i ? ";" : "") + name_of(sampled.pc[i]);
                }
                ++stack_counts[key];
            }
            std::vector<std::pair<unsigned, std::string>> sorted_stacks;
            for (auto& [key, n] : stack_counts) {
                sorted_stacks.emplace_back(n, key);
            }
            std::sort(sorted_stacks.rbegin(), sorted_stacks.rend());
            if (FILE* f = std::fopen((stem + "_stacks.txt").c_str(), "w")) {
                std::fprintf(f, "# %s, %zu samples in %d s, stacks innermost first\n", label.c_str(),
                             stacks.size(), period);
                for (const auto& [n, key] : sorted_stacks) {
                    std::fprintf(f, "%u %s\n", n, key.c_str());
                }
                std::fclose(f);
            }
            samples.clear();
            stacks.clear();
            Refresh(target);
        }
        Close(target);
    }).detach();
}

void StartCurrent(const char* name) {
    Start(CurrentThread(), name);
}

} // namespace Gow3Profiler

/// For the C runtime: GOW3_PROFILE_GUEST=<name prefix> samples guest threads with that name.
extern "C" void gow3_profile_current(const char* name) {
    Gow3Profiler::StartCurrent(name);
}
