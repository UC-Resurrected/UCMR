// Uncharted 3 1.02, 20 Nov 2026 (made stricter 22 Nov 2026): a job wait that can never end is ended, but only when the
// work it waits for is done.
//
// In Syria and Yemen co-op the game froze now and then (a kill, a roll and a weapon switch at once in the busy maps):
// the frame stopped with jobs parked on job counters that were short of their target while no job submitted on them
// was left to finish, every priority queue was empty and every worker idle. Nothing would ever lower those counters.
// The jobs submitted on each counter are counted by the counter history (job_trace.cpp: +1 at the queue push, -1 when a
// job finishes). A parked job's wait is settled -- the counter set to 0 and the job system's counter update (0xd969c0:
// under the job lock, the jobs waiting for that value go back to the queues) called, as a job finishing on it would
// have -- in two cases:
//   - the counter is above 0, the wait is for 0 (or for at most 0 or more) and has lasted 4 s, no counted job is left
//     on it, every queue is empty, the frame has stopped for 3 s (the particle renderer's game-jobs check, 0xeb9ca0, runs
//     once a frame) AND nothing went wrong in the counter's current life. A counter made again while jobs of its last
//     life were unfinished, given back with jobs unfinished, taken below 0 by a job or settled before is left as is
//     and reported: its count does not say whether its work was done, and releasing its waiters made the game read
//     data that was never produced (22 Nov 2026: a custom match crashed at 0xf88f74 after ten such settles);
//   - the counter is below 0 (more jobs finished on it than it was made for), the wait is for exactly 0, has lasted
//     1 s and no counted job is left on it: all its jobs are done.
// The update runs on a game thread that sleeps in the thread wait loop (it holds no job system lock); the
// counter_wait_log checker only asks for it. Tested in a full Syria co-op run (0.10.118): one freeze began, the counter
// was settled, the game paused about 2 s and went on to the end. Found by their bytes.
#include "u3fix.h"
#include "u3fix_hook.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace u3fix {

namespace {

std::atomic<int64_t> g_last_frame_ms{-1};

struct SettleRequest {
    unsigned char *counter;
    int64_t seen;
    char chain[160];
};
std::mutex g_settle_lock;
SettleRequest g_settle[32];
unsigned g_settle_count = 0;
std::atomic<unsigned> g_settled{0};

struct LeftAsIs {
    const void *counter;
    uint32_t life;
};
LeftAsIs g_left[64];   // the counter lives already reported as left as is (under g_settle_lock)
unsigned g_left_next = 0;

int64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool queues_empty() {
    const unsigned char *manager = job_manager();
    if (!manager) return false;
    const unsigned char *queues = *reinterpret_cast<const unsigned char *const *>(manager + 0x4f10);
    if (!queues) return false;
    for (unsigned p = 0; p < 8; p++)   // 8 priorities, 0x2078 bytes each, the count first
        if (__atomic_load_n(reinterpret_cast<const uint64_t *>(queues + p * 0x2078), __ATOMIC_RELAXED)) return false;
    return true;
}

__attribute__((sysv_abi)) void on_frame() { g_last_frame_ms.store(steady_ms(), std::memory_order_relaxed); }

// the game-jobs check: push rbp; mov rbp,rsp; push r15..r12; push rbx; push rax; the first counter loads
constexpr unsigned char kGameCheck[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53,
                                        0x50, 0x48, 0x8b, 0x07, 0x48, 0x8b, 0x70, 0x20, 0x48, 0x8b, 0x47, 0x08, 0x4c,
                                        0x8b, 0x68, 0x20};
constexpr unsigned kPrologue = 6;   // push rbp; mov rbp,rsp; push r15

void put(unsigned char *stub, unsigned &n, const void *bytes, unsigned length) {
    std::memcpy(stub + n, bytes, length);
    n += length;
}

// at a function entry: keep the argument registers, call the handler, replay the prologue, go on
bool hook_entry(unsigned char *image, uint64_t size, uint64_t at, uint64_t handler) {
    unsigned char *stub = stub_page_near(image, size);
    if (!stub) return false;
    // entry rsp = 8 mod 16: six pushes and sub rsp,8 make the call aligned
    static constexpr unsigned char kSave[] = {0x57, 0x56, 0x52, 0x51, 0x41, 0x50, 0x41, 0x51, 0x48, 0x83, 0xec, 0x08};
    static constexpr unsigned char kRestore[] = {0x48, 0x83, 0xc4, 0x08, 0x41, 0x59, 0x41, 0x58, 0x59, 0x5a, 0x5e, 0x5f};
    unsigned n = 0;
    put(stub, n, kSave, sizeof kSave);
    stub[n++] = 0x48; stub[n++] = 0xb8; put(stub, n, &handler, 8);   // mov rax, handler
    stub[n++] = 0xff; stub[n++] = 0xd0;                              // call rax
    put(stub, n, kRestore, sizeof kRestore);
    put(stub, n, image + at, kPrologue);
    const uint64_t back = reinterpret_cast<uint64_t>(image + at + kPrologue);
    stub[n++] = 0xff; stub[n++] = 0x25; std::memset(stub + n, 0, 4); n += 4;   // jmp [rip]
    put(stub, n, &back, 8);
    return finish_stub(stub) && jump_to(image, at, kPrologue, stub);
}

const char *trouble_text(uint32_t trouble) {
    if (trouble & kCounterReused) return "it was made again while jobs of its last life were unfinished";
    if (trouble & kCounterFreedBusy) return "it was given back while jobs counted on it were unfinished";
    if (trouble & kCounterBelowZero) return "a job took it below 0 in this life";
    if (trouble & kCounterSettled) return "it was settled before in this life";
    return "";
}

}  // namespace

bool settle_counter_now(unsigned char *counter, int64_t seen, const char *chain) {
    const CounterUpdate update = counter_update_function();
    if (!update || !counter || !seen) return false;
    int64_t *value = reinterpret_cast<int64_t *>(counter + 0x20);
    if (!__atomic_compare_exchange_n(value, &seen, int64_t{0}, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return false;
    note_settled(counter, seen);
    update(counter, 0);
    const unsigned t = ++g_settled;
    if (t <= 50 || t % 100 == 0) {
        char story[1200];
        counter_story(counter, story, sizeof story);
        if (seen > 0)
            std::printf("Uncharted 3: job counter %p stuck at %lld with no job left to lower it, every queue empty and "
                        "the frame stopped: set to 0 and its waiting jobs released so the game goes on (time %u); "
                        "waiting code%s; history: %s\n",
                        static_cast<void *>(counter), static_cast<long long>(seen), t, chain ? chain : "", story);
        else
            std::printf("Uncharted 3: job counter %p was %lld (more jobs finished than started) with no job counted on "
                        "it unfinished: set to 0 and its waiting jobs released (time %u); waiting code%s; history: %s\n",
                        static_cast<void *>(counter), static_cast<long long>(seen), t, chain ? chain : "", story);
        std::fflush(stdout);
    }
    return true;
}

bool consider_stuck_wait(const unsigned char *counter, int64_t target, int mode, int64_t value, int64_t waited_ms,
                         const char *chain) {
    if (!counter_update_function() || !counter) return false;
    if (mode == 0 && target == 0 && value < 0) {   // below 0: settled once every counted job on it has finished
        if (waited_ms < 1000 || pending_jobs(counter) != 0) return false;
    } else {
        const bool for_zero = (mode == 0 && target == 0) || (mode == 1 && target >= 0);
        const int64_t last_frame = g_last_frame_ms.load(std::memory_order_relaxed);
        if (!for_zero || value <= 0 || waited_ms < 4000 || last_frame < 0 || steady_ms() - last_frame < 3000) return false;
        if (pending_jobs(counter) != 0 || !queues_empty()) return false;
        uint32_t life = 0;
        const uint32_t trouble = counter_trouble(counter, &life);
        if (trouble) {   // its count does not tell whether its work was done: left as is, reported once per life
            {
                std::lock_guard<std::mutex> guard(g_settle_lock);
                for (const LeftAsIs &l : g_left)
                    if (l.counter == counter && l.life == life) return false;
                g_left[g_left_next++ % 64] = LeftAsIs{counter, life};
            }
            char story[1200];
            counter_story(counter, story, sizeof story);
            std::printf("Uncharted 3: job counter %p stuck at %lld is left as is: %s, so the work its waiting jobs need "
                        "may not be done; waiting code%s; history: %s\n",
                        static_cast<const void *>(counter), static_cast<long long>(value), trouble_text(trouble),
                        chain ? chain : "", story);
            std::fflush(stdout);
            return false;
        }
    }
    std::lock_guard<std::mutex> guard(g_settle_lock);
    for (unsigned i = 0; i < g_settle_count; i++)
        if (g_settle[i].counter == counter) return true;
    if (g_settle_count >= 32) return false;
    SettleRequest &r = g_settle[g_settle_count++];
    r.counter = const_cast<unsigned char *>(counter);
    r.seen = value;
    std::snprintf(r.chain, sizeof r.chain, "%s", chain ? chain : "");
    return true;
}

void settle_stuck_counters() {
    if (!counter_update_function()) return;
    SettleRequest todo[32];
    unsigned n = 0;
    {
        std::lock_guard<std::mutex> guard(g_settle_lock);
        if (!g_settle_count) return;
        n = g_settle_count;
        std::memcpy(todo, g_settle, sizeof(SettleRequest) * n);
        g_settle_count = 0;
    }
    for (unsigned i = 0; i < n; i++) {
        unsigned char *counter = todo[i].counter;
        const int64_t seen = __atomic_load_n(reinterpret_cast<const int64_t *>(counter + 0x20), __ATOMIC_ACQUIRE);
        // it moved on after all: another sign, or a job was counted on it since
        if (!seen || (seen > 0) != (todo[i].seen > 0) || pending_jobs(counter) != 0) continue;
        settle_counter_now(counter, seen, todo[i].chain);
    }
}

bool install_stuck_counter(unsigned char *image, uint64_t size) {
    if (!counter_update_function()) return false;   // the job counts come from the counter history
    const uint64_t check = find_unique(image, size, kGameCheck, sizeof kGameCheck);
    if (check == kNotFound) return false;
    if (!hook_entry(image, size, check, reinterpret_cast<uint64_t>(&on_frame))) return false;
    std::printf("Uncharted 3: a job wait that can't end is settled when its work is done (stuck above 0: no job left "
                "to lower it, queues empty, frame stopped 4 s, nothing wrong in its life; below 0: no job left on it "
                "after 1 s) (frame check %#llx)\n",
                static_cast<unsigned long long>(check));
    std::fflush(stdout);
    return true;
}

}  // namespace u3fix
