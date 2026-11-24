// Uncharted 3 1.02 (20 Nov 2026, ported onto job_trace 23 Nov 2026): a job counter is not handed out again while jobs
// that lower it have yet to finish.
//
// The job system keeps its job counters in a pool (the job manager +0xae0: lock, bitmap of used counters at +0x20,
// counters at +0x120, count at +0x128; 0x80 bytes each, the count at +0x20). Each job names the counter it lowers when
// it finishes (job +0x10). Two functions give a counter back: one at once (image 0xd967c0), one after it waited for the
// count to reach 0 (0xd975b0). Neither looks for jobs still to finish. On PS4 there are none by then. On a PC there can
// be: a job finishes on a counter that was given back before and that the pool already handed to a new owner. The late
// job lowers the new owner's count, its wait ends before its own jobs are done, and so on, until a count skips the value
// a wait looks for and that wait never ends: the game freezes. Syria co-op, stage 6 -> 7 (23 Nov 2026): the collision
// cast cleanup (0x111b390) gives back every per-cast counter once the global cast counter reaches 0, also the counter of
// a cast job still running; Deform Entities takes that slot, the late cast job lowers Deform's count, Deform enqueues a
// compute context still being filled (pComputeContext->Closed() at 0xf0467a) and the job counters go wrong until the
// frame stops.
// job_trace.cpp counts the jobs per counter (+1 at the append queue push 0xda57d0, -1 at the lowering 0xda80bb). Here a
// counter given back while jobs counted on it are unfinished stays out of the pool: its give-back is put off and done
// on a later give-back call once its jobs have finished, or after 2 s (then as before). When the pool is nearly full
// nothing is kept. The wait-then-give-back function becomes: the wait, then the give-back function.
// Tested on 0.10.118 with the same logic and its own job counts: two full Syria co-op runs as host (stage 27/28),
// 205 and 146 counters kept, all of them given back after their jobs, none after 2 s, pool at most 172 of 2048.
#include "u3fix.h"
#include "u3fix_hook.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace u3fix {

namespace {

constexpr unsigned kMaxKept = 1024;
constexpr uint64_t kKeepMs = 2000;
constexpr unsigned kNearlyFull = 128;   // nothing is kept with fewer free counters than this

struct Kept {
    unsigned char *counter;
    uint64_t since_ms;
};

std::mutex g_lock;
Kept g_kept[kMaxKept];
unsigned g_kept_count = 0;
thread_local bool t_releasing = false;

using GiveBack = void(__attribute__((sysv_abi)) *)(unsigned char *);
GiveBack g_give_back = nullptr;   // the original function: the replayed prologue, then the rest of it
unsigned char *const *g_manager = nullptr;

uint64_t g_kept_total = 0, g_done_total = 0, g_timeout_total = 0, g_full_total = 0;   // under g_lock
uint64_t g_last_report_ms = 0;

uint64_t now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

void pool_use(unsigned &used, unsigned &capacity) {
    used = capacity = 0;
    const unsigned char *manager = g_manager ? *g_manager : nullptr;
    if (!manager) return;
    const unsigned char *pool = manager + 0xae0;
    capacity = *reinterpret_cast<const uint32_t *>(pool + 0x128);
    for (unsigned i = 0; i < 32; i++)
        used += static_cast<unsigned>(
            __builtin_popcountll(__atomic_load_n(reinterpret_cast<const uint64_t *>(pool + 0x20) + i, __ATOMIC_RELAXED)));
}

// 1: give the counter back now (the original function goes on); 0: it is kept for later
__attribute__((sysv_abi)) uint64_t on_give_back(unsigned char *counter) {
    if (t_releasing || !counter) return 1;
    const uint64_t now = now_ms();
    unsigned used = 0, capacity = 0;
    pool_use(used, capacity);
    const bool nearly_full = capacity && used + kNearlyFull >= capacity;
    bool keep = pending_jobs(counter) > 0;

    unsigned char *ready[64];
    unsigned ready_count = 0;
    bool report = false;
    unsigned kept_now = 0;
    uint64_t kept_total = 0, done_total = 0, timeout_total = 0, full_total = 0;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        for (unsigned i = 0; i < g_kept_count && ready_count < 64;) {
            const bool done = pending_jobs(g_kept[i].counter) <= 0;
            const bool late = now - g_kept[i].since_ms >= kKeepMs;
            if (!done && !late && !nearly_full) {
                i++;
                continue;
            }
            if (done) {
                g_done_total++;
            } else {
                g_timeout_total++;
                forget_pending_jobs(g_kept[i].counter);   // its jobs are not coming
            }
            ready[ready_count++] = g_kept[i].counter;
            g_kept[i] = g_kept[--g_kept_count];
        }
        if (keep && (nearly_full || g_kept_count >= kMaxKept)) {
            keep = false;
            g_full_total++;
        }
        if (keep) {
            g_kept[g_kept_count++] = {counter, now};
            g_kept_total++;
        }
        kept_now = g_kept_count;
        if (g_kept_total && now - g_last_report_ms >= 30000) {
            g_last_report_ms = now;
            report = true;
            kept_total = g_kept_total;
            done_total = g_done_total;
            timeout_total = g_timeout_total;
            full_total = g_full_total;
        }
    }
    t_releasing = true;
    for (unsigned i = 0; i < ready_count; i++) g_give_back(ready[i]);
    t_releasing = false;

    if (report) {
        std::printf("Uncharted 3: job counters given back with jobs still to finish: %llu kept (%llu after their jobs, "
                    "%llu after 2 s, %llu not kept: pool nearly full), %u kept now; pool %u of %u in use\n",
                    static_cast<unsigned long long>(kept_total), static_cast<unsigned long long>(done_total),
                    static_cast<unsigned long long>(timeout_total), static_cast<unsigned long long>(full_total),
                    kept_now, used, capacity);
        std::fflush(stdout);
    }
    return keep ? 0 : 1;
}

// in both give-back functions: mov r13,[r14]; lea rbx,[r13+0xae0]; mov rax,[r13+0xc00] (the pool, its counters)
constexpr unsigned char kPoolLoads[] = {0x4d, 0x8b, 0x2e, 0x49, 0x8d, 0x9d, 0xe0, 0x0a, 0x00, 0x00,
                                        0x49, 0x8b, 0x85, 0x00, 0x0c, 0x00, 0x00};
constexpr uint64_t kGiveBackLoadsAt = 0xb6;   // from the entry of the give-back function
constexpr uint64_t kWaitFreeLoadsAt = 0xbb;   // from the entry of the wait-then-give-back function
// both entries: push rbp; mov rbp,rsp; push r15; push r14; push r13; push r12; push rbx; sub rsp,0x18; mov r12,rdi
constexpr unsigned char kEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                    0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x18, 0x49, 0x89, 0xfc};
constexpr unsigned kPrologue = 6;   // push rbp; mov rbp,rsp; push r15
// the give-back function then: lea rax,[rip+..]; mov rax,[rax]; mov [rbp-0x30],rax; lea r14,[rip+..] (the manager)
constexpr uint64_t kManagerLeaAt = 0x22;
// the wait-then-give-back function then: lea rax,[rip+..]; mov rax,[rax]; mov [rbp-0x30],rax; call the wait
constexpr uint64_t kWaitCallAt = 0x22;

// entry rsp = 8 mod 16: six pushes and sub rsp,8 make the call aligned
constexpr unsigned char kSave[] = {0x57, 0x56, 0x52, 0x51, 0x41, 0x50, 0x41, 0x51,   // rdi rsi rdx rcx r8 r9
                                   0x48, 0x83, 0xec, 0x08};                          // sub rsp,8
constexpr unsigned char kRestore[] = {0x48, 0x83, 0xc4, 0x08,                        // add rsp,8
                                      0x41, 0x59, 0x41, 0x58, 0x59, 0x5a, 0x5e, 0x5f};

void put(unsigned char *stub, unsigned &n, const void *bytes, unsigned length) {
    std::memcpy(stub + n, bytes, length);
    n += length;
}

void put_mov_rax(unsigned char *stub, unsigned &n, uint64_t value) {
    stub[n++] = 0x48;
    stub[n++] = 0xb8;
    put(stub, n, &value, 8);
}

void put_jmp_back(unsigned char *stub, unsigned &n, uint64_t to) {
    stub[n++] = 0xff;
    stub[n++] = 0x25;
    std::memset(stub + n, 0, 4);   // jmp [rip]
    n += 4;
    put(stub, n, &to, 8);
}

bool hook_give_back(unsigned char *image, uint64_t size, uint64_t at) {
    unsigned char *stub = stub_page_near(image, size);
    if (!stub) return false;
    unsigned n = 0;
    put(stub, n, kSave, sizeof kSave);
    put_mov_rax(stub, n, reinterpret_cast<uint64_t>(&on_give_back));
    stub[n++] = 0xff;
    stub[n++] = 0xd0;   // call rax
    put(stub, n, kRestore, sizeof kRestore);
    stub[n++] = 0x84;
    stub[n++] = 0xc0;   // test al,al
    stub[n++] = 0x75;
    stub[n++] = 0x01;   // jnz the original
    stub[n++] = 0xc3;   // ret: kept
    unsigned char *original = stub + n;
    put(stub, n, image + at, kPrologue);   // the prologue
    put_jmp_back(stub, n, reinterpret_cast<uint64_t>(image + at + kPrologue));
    if (!finish_stub(stub)) return false;
    g_give_back = reinterpret_cast<GiveBack>(original);
    return jump_to(image, at, kPrologue, stub);
}

// the wait-then-give-back function: the wait (its arguments as they came), then the give-back function (hooked)
bool replace_wait_free(unsigned char *image, uint64_t size, uint64_t at, uint64_t give_back) {
    const unsigned char *call = image + at + kWaitCallAt;
    if (call[0] != 0xe8) return false;
    int32_t rel;
    std::memcpy(&rel, call + 1, 4);
    const uint64_t wait = reinterpret_cast<uint64_t>(call + 5 + rel);
    unsigned char *stub = stub_page_near(image, size);
    if (!stub) return false;
    unsigned n = 0;
    stub[n++] = 0x57;   // push rdi: aligned
    put_mov_rax(stub, n, wait);
    stub[n++] = 0xff;
    stub[n++] = 0xd0;   // call rax
    stub[n++] = 0x5f;   // pop rdi
    put_mov_rax(stub, n, reinterpret_cast<uint64_t>(image + give_back));
    stub[n++] = 0xff;
    stub[n++] = 0xe0;   // jmp rax
    if (!finish_stub(stub)) return false;
    return jump_to(image, at, kPrologue, stub);
}

}  // namespace

bool install_counter_keep(unsigned char *image, uint64_t size) {
    uint64_t give_back = kNotFound, wait_free = kNotFound;
    for (uint64_t i = kWaitFreeLoadsAt; i + sizeof kPoolLoads <= size; i++) {
        if (image[i] != kPoolLoads[0] || std::memcmp(image + i, kPoolLoads, sizeof kPoolLoads) != 0) continue;
        if (std::memcmp(image + i - kGiveBackLoadsAt, kEntry, sizeof kEntry) == 0) {
            if (give_back != kNotFound) return false;   // more than one: not the build this was written for
            give_back = i - kGiveBackLoadsAt;
        } else if (std::memcmp(image + i - kWaitFreeLoadsAt, kEntry, sizeof kEntry) == 0) {
            if (wait_free != kNotFound) return false;
            wait_free = i - kWaitFreeLoadsAt;
        }
    }
    if (give_back == kNotFound || wait_free == kNotFound) return false;
    const unsigned char *lea = image + give_back + kManagerLeaAt;
    if (lea[0] != 0x4c || lea[1] != 0x8d || lea[2] != 0x35) return false;   // lea r14,[rip+disp32]
    int32_t disp;
    std::memcpy(&disp, lea + 3, 4);
    g_manager = reinterpret_cast<unsigned char *const *>(lea + 7 + disp);
    if (!hook_give_back(image, size, give_back)) return false;
    if (!replace_wait_free(image, size, wait_free, give_back)) return false;
    std::printf("Uncharted 3: job counters stay out of the pool until their jobs have finished (give back %#llx, wait "
                "and give back %#llx)\n",
                static_cast<unsigned long long>(give_back), static_cast<unsigned long long>(wait_free));
    std::fflush(stdout);
    return true;
}

}  // namespace u3fix
