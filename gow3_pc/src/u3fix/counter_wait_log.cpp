// Uncharted 3 1.02, 19 Nov 2026: report a job counter wait that does not end.
//
// The engine's counter wait (image 0xd97480; WaitForCounter in the job system) checks a counter object's value (at +0x20)
// against a target and, on threads that may not run jobs themselves, sleeps 1 ms between checks. In Syria co-op the game
// froze at stage 7 with the main thread and the sound driver thread both sleeping in this loop while all six job workers
// were idle: the job that should bring the counter to its target was never run. To find which job that is, the sleep
// call inside the loop now goes through a handler that sleeps the same 1 ms and, once a thread has waited 5 s on the same
// counter, prints the counter, its value and target, the guest return addresses up the frame chain and the counter
// object's first bytes. Nothing changes for waits that end. Found by its bytes.
#include "u3fix.h"
#include "u3fix_hook.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace u3fix {

namespace {

const unsigned char *g_image = nullptr;
uint64_t g_size = 0;

bool in_image(uint64_t a) {
    const uint64_t base = reinterpret_cast<uint64_t>(g_image);
    return a >= base && a < base + g_size;
}

bool readable(const void *p, size_t n) {
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION info;
    if (!p || !VirtualQuery(p, &info, sizeof info) || info.State != MEM_COMMIT) return false;
    if (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    const uintptr_t end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    return reinterpret_cast<uintptr_t>(p) + n <= end;
#else
    return p != nullptr && n > 0;
#endif
}

// the guest return addresses up the frame chain, as text
void describe(char *chain, size_t size, const uint64_t *f) {
    size_t used = 0;
    chain[0] = 0;
    for (int i = 0; i < 8 && readable(f, 16) && used < size; i++) {
        const uint64_t ret = f[1];
        if (in_image(ret))
            used += std::snprintf(chain + used, size - used, " guest %#llx",
                                  static_cast<unsigned long long>(ret - reinterpret_cast<uint64_t>(g_image)));
        else
            used += std::snprintf(chain + used, size - used, " %#llx", static_cast<unsigned long long>(ret));
        const uint64_t *next = reinterpret_cast<const uint64_t *>(f[0]);
        if (next <= f) break;
        f = next;
    }
}

// the counter, the target, the mode (0: equal, 1: at most) and the wait function's frame pointer
// A counter below 0 has had more jobs finish than were started on it (Syria co-op, 20 Nov 2026: -1 and -36, with the
// jobs' queues empty and every worker idle). A wait for exactly 0 can never end then. Once it has stayed below 0 for
// 100 ms and no job counted on it is unfinished (job_trace.cpp), all its jobs are done: it is set to 0 and its waiting
// jobs released (stuck_counter.cpp), as the right count would have done. While a counted job is still to finish it is
// left alone: releasing the waiters then would let them go on before that job's work is done. (Before 22 Nov 2026 it
// was set to 0 at once, also while jobs on it were still to come, and parked jobs waiting on it were not released.)
// A parked job's wait on such a counter is settled by the checker below (stuck_counter.cpp).
bool settle_negative(const unsigned char *counter, int64_t target, int mode, int64_t waited_ms, const uint64_t *frame) {
    if (target != 0 || mode != 0 || waited_ms < 100 || !readable(counter + 0x20, 8)) return false;
    const int64_t seen = __atomic_load_n(reinterpret_cast<const int64_t *>(counter + 0x20), __ATOMIC_ACQUIRE);
    if (seen >= 0 || pending_jobs(counter) != 0) return false;
    char chain[256];
    describe(chain, sizeof chain, frame);
    return settle_counter_now(const_cast<unsigned char *>(counter), seen, chain);
}

__attribute__((sysv_abi)) void counter_wait_sleep(const unsigned char *counter, int64_t target, int mode,
                                                  const uint64_t *frame) {
    settle_stuck_counters();   // the ones the checker below asked for: this thread holds no job system lock
    using clock = std::chrono::steady_clock;
    thread_local const unsigned char *waiting_on = nullptr;
    thread_local clock::time_point since;
    thread_local bool reported = false;
    thread_local clock::time_point last;
    const auto now = clock::now();
    // a new wait: another counter, or the same (pooled) counter after a gap
    if (counter != waiting_on || now - last > std::chrono::milliseconds(100)) {
        waiting_on = counter;
        since = now;
        reported = false;
    }
    last = now;
    const int64_t waited_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - since).count();
    if (settle_negative(counter, target, mode, waited_ms, frame)) return;   // the loop sees 0 and ends
    if (!reported && now - since >= std::chrono::seconds(5)) {
        reported = true;
        int64_t value = 0;
        if (readable(counter + 0x20, 8)) std::memcpy(&value, counter + 0x20, 8);
        unsigned long thread = 0;
#ifdef _WIN32
        thread = GetCurrentThreadId();
#endif
        char chain[512];
        describe(chain, sizeof chain, frame);
        char bytes[3 * 0x40 + 1];
        bytes[0] = 0;
        if (readable(counter, 0x40))
            for (int i = 0; i < 0x40; i++) std::snprintf(bytes + 3 * i, 4, "%02x ", counter[i]);
        char queues[128], story[1200];
        job_queue_counts(queues, sizeof queues);
        counter_story(counter, story, sizeof story);
        std::printf("Uncharted 3: counter wait over 5 s on thread %lu: counter %p value %lld, waiting for %s %lld; "
                    "callers%s; counter bytes %s; jobs queued per priority: %s; history: %s\n",
                    thread, static_cast<const void *>(counter), static_cast<long long>(value),
                    mode == 1 ? "at most" : "exactly", static_cast<long long>(target), chain, bytes, queues, story);
        std::fflush(stdout);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));   // the 1 ms the loop sleeps (usleep(1000))
}

// Jobs wait the other way: on a job worker the same loop calls the job system's wait (image 0xda84f0), which can park
// the job until its counter is reached, so a parked job has no thread to report from. Each such wait is recorded when
// it starts (counter, target, the job's callers); a checker thread reports, once, a wait whose counter is still short
// of its target after 3 s.
struct JobWait {
    const unsigned char *counter = nullptr;
    int64_t target = 0;
    int mode = 0;
    std::chrono::steady_clock::time_point since;
    bool reported = false;
    char chain[256] = {};
};
JobWait g_job_waits[128];
unsigned g_job_next = 0;
std::atomic_flag g_job_waits_lock = ATOMIC_FLAG_INIT;

void lock_waits() { while (g_job_waits_lock.test_and_set(std::memory_order_acquire)) std::this_thread::yield(); }
void unlock_waits() { g_job_waits_lock.clear(std::memory_order_release); }

bool reached(const unsigned char *counter, int64_t target, int mode, int64_t &value) {
    value = 0;
    if (!readable(counter + 0x20, 8)) return true;
    std::memcpy(&value, counter + 0x20, 8);
    return mode == 1 ? value <= target : value == target;
}

// The wait is recorded; the stub always goes on to the job system's wait (a counter below 0 is settled by the checker,
// stuck_counter.cpp, once no job counted on it is left). Returns 1 only to skip that wait, which nothing asks for now.
__attribute__((sysv_abi)) int job_counter_wait(const unsigned char *counter, int64_t target, int mode,
                                               const uint64_t *frame) {
    int64_t value = 0;
    if (reached(counter, target, mode, value)) return 0;
    lock_waits();
    JobWait *slot = nullptr;
    for (JobWait &w : g_job_waits)
        if (w.counter == counter && w.target == target) { slot = &w; break; }
    if (!slot) {   // a new wait: take the next slot round the ring
        slot = &g_job_waits[g_job_next++ % 128];
        slot->counter = counter;
        slot->target = target;
        slot->mode = mode;
        slot->since = std::chrono::steady_clock::now();
        slot->reported = false;
        describe(slot->chain, sizeof slot->chain, frame);
    }
    unlock_waits();
    return 0;
}

void check_job_waits() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const auto now = std::chrono::steady_clock::now();
        lock_waits();
        for (JobWait &w : g_job_waits) {
            if (!w.counter) continue;
            int64_t value = 0;
            if (reached(w.counter, w.target, w.mode, value)) { w.counter = nullptr; continue; }
            // a wait that can never end is settled (stuck_counter.cpp), after the report below
            consider_stuck_wait(w.counter, w.target, w.mode, value,
                                std::chrono::duration_cast<std::chrono::milliseconds>(now - w.since).count(), w.chain);
            if (w.reported || now - w.since < std::chrono::seconds(3)) continue;
            w.reported = true;
            char queues[128], story[1200];
            job_queue_counts(queues, sizeof queues);
            counter_story(w.counter, story, sizeof story);
            std::printf("Uncharted 3: job waiting over 3 s: counter %p value %lld, waiting for %s %lld; callers%s; "
                        "jobs queued per priority: %s; history: %s\n",
                        static_cast<const void *>(w.counter), static_cast<long long>(value),
                        w.mode == 1 ? "at most" : "exactly", static_cast<long long>(w.target), w.chain, queues, story);
            std::fflush(stdout);
        }
        unlock_waits();
    }
}

// mov rdi,r15; mov rsi,r14; mov edx,ebx; call wait; mov rax,[r15+0x20]; cmp ebx,1; jne +0x0a; cmp ...
constexpr unsigned char kJobSite[] = {0x4c, 0x89, 0xff, 0x4c, 0x89, 0xf6, 0x89, 0xda, 0xe8, 0xb3, 0x0f, 0x01,
                                      0x00, 0x49, 0x8b, 0x47, 0x20, 0x83, 0xfb, 0x01, 0x75, 0x0a, 0x4c, 0x39};
constexpr unsigned kJobCall = 8;

// mov edi,1000; call usleep; mov rax,[r15+0x20]; cmp ebx,1; jne +0x0d; cmp rax,r14
constexpr unsigned char kSite[] = {0xbf, 0xe8, 0x03, 0x00, 0x00, 0xe8, 0x96, 0x54, 0x60, 0x00, 0x49, 0x8b,
                                   0x47, 0x20, 0x83, 0xfb, 0x01, 0x75, 0x0d, 0x4c, 0x39, 0xf0};
constexpr unsigned kLength = 10;   // mov edi + call

}  // namespace

bool install_counter_wait_log(unsigned char *image, uint64_t size) {
    const uint64_t at = find_unique(image, size, kSite, sizeof(kSite));
    if (at == kNotFound) return false;
    unsigned char *stub = stub_page_near(image, size);
    if (!stub) return false;
    g_image = image;
    g_size = size;
    unsigned n = 0;
    const unsigned char code[] = {
        0x4c, 0x89, 0xff,         // mov rdi,r15 (the counter)
        0x4c, 0x89, 0xf6,         // mov rsi,r14 (the target)
        0x89, 0xda,               // mov edx,ebx (the mode)
        0x48, 0x89, 0xe9,         // mov rcx,rbp (the wait function's frame)
        0x48, 0x83, 0xec, 0x08,   // sub rsp,8 (aligned for the call)
        0x48, 0xb8};              // mov rax, handler
    std::memcpy(stub, code, sizeof code);
    n = sizeof code;
    const uint64_t handler = reinterpret_cast<uint64_t>(&counter_wait_sleep);
    std::memcpy(stub + n, &handler, 8);
    n += 8;
    const unsigned char tail[] = {0xff, 0xd0, 0x48, 0x83, 0xc4, 0x08, 0xc3};   // call rax; add rsp,8; ret
    std::memcpy(stub + n, tail, sizeof tail);
    if (!finish_stub(stub)) return false;
    // the loop's "mov edi,1000; call usleep" becomes "call stub" and nops
    const int64_t delta = static_cast<int64_t>(reinterpret_cast<uintptr_t>(stub)) -
                          static_cast<int64_t>(reinterpret_cast<uintptr_t>(image + at + 5));
    if (delta < INT32_MIN || delta > INT32_MAX) return false;
    const int32_t rel = static_cast<int32_t>(delta);
    image[at] = 0xe8;
    std::memcpy(image + at + 1, &rel, 4);
    std::memset(image + at + 5, 0x90, kLength - 5);
    std::printf("Uncharted 3: counter waits over 5 s are reported (%#llx)\n", static_cast<unsigned long long>(at));

    // the jobs' wait: the call goes to a stub that reports, then jumps on to the original wait
    const uint64_t job_at = find_unique(image, size, kJobSite, sizeof(kJobSite));
    if (job_at != kNotFound) {
        unsigned char *call = image + job_at + kJobCall;
        int32_t old_rel = 0;
        std::memcpy(&old_rel, call + 1, 4);
        const uint64_t wait = reinterpret_cast<uint64_t>(call + 5) + static_cast<int64_t>(old_rel);
        unsigned char *js = stub_page_near(image, size);
        if (js) {
            unsigned m = 0;
            const unsigned char head[] = {0x57, 0x56, 0x52,         // push rdi; push rsi; push rdx (aligned now)
                                          0x48, 0x89, 0xe9,         // mov rcx,rbp (the waiting job's frame)
                                          0x48, 0xb8};              // mov rax, handler
            std::memcpy(js, head, sizeof head);
            m = sizeof head;
            const uint64_t h = reinterpret_cast<uint64_t>(&job_counter_wait);
            std::memcpy(js + m, &h, 8);
            m += 8;
            const unsigned char mid[] = {0xff, 0xd0,                // call rax
                                         0x5a, 0x5e, 0x5f,          // pop rdx; pop rsi; pop rdi
                                         0x85, 0xc0, 0x74, 0x01,    // test eax,eax; je: wait as before
                                         0xc3,                      // ret: the counter was settled
                                         0xff, 0x25, 0, 0, 0, 0};   // jmp [rip]: the original wait
            std::memcpy(js + m, mid, sizeof mid);
            m += sizeof mid;
            std::memcpy(js + m, &wait, 8);
            const int64_t d = static_cast<int64_t>(reinterpret_cast<uintptr_t>(js)) -
                              static_cast<int64_t>(reinterpret_cast<uintptr_t>(call + 5));
            if (finish_stub(js) && d >= INT32_MIN && d <= INT32_MAX) {
                const int32_t rel2 = static_cast<int32_t>(d);
                std::memcpy(call + 1, &rel2, 4);
                std::thread(check_job_waits).detach();
                std::printf("Uncharted 3: job waits over 3 s are reported (%#llx)\n",
                            static_cast<unsigned long long>(job_at + kJobCall));
            }
        }
    }
    std::fflush(stdout);
    return true;
}

}  // namespace u3fix
