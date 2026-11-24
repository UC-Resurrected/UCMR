// Uncharted 3 1.02, 22 Nov 2026: every job counter's recent history, and the moment its count goes wrong.
//
// A job counter (0x80 bytes from the job manager's pool, the value at +0x20) is made with a count (image 0xd96620, the
// xchg at 0xd96716; RunJobs 0xd96f60 makes one inline at 0xd97109), lowered by one by every job that finishes on it
// (the worker loop's lock xadd at 0xda80bb), changed by game code itself (about 160 places add to or take from it and
// then call the counter update 0xd969c0 with the new value) and given back to the pool (two places clear its bit in
// the pool's map). Nothing in the pool tells two lives of one counter apart: a job still holding a counter that was
// given back lowers whatever the counter is used for next.
//
// The trace notes, per pool slot, who made the counter and with what count, the last eight things that happened to it
// (a job that lowered it: its entry and argument; game code that changed it; the give-back; a settle by the runtime) and
// how many jobs were submitted on it and have not finished yet (+1 per job in a list the game appends to a queue, the
// queue push 0xda57d0; -1 when one finishes). That count is what the stuck-wait safety net (stuck_counter.cpp) and the
// below-zero settle (counter_wait_log.cpp) go by. It reports at once, with the history:
//   - a counter made again while jobs counted on its last life have not finished (those jobs will lower the new life),
//   - a counter given back while jobs counted on it have not finished,
//   - a job taking a counter from 0 to -1 (more jobs finished than it was made for), once per life.
// The waits that do not end print the same history. Nothing in the game changes. Found by their bytes.
#include "u3fix.h"
#include "u3fix_hook.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace u3fix {

namespace {

constexpr unsigned kSlots = 4096;   // the counter pool's counters (2048 in this build)
constexpr unsigned kEvents = 8;

enum Kind : uint32_t { kNone, kMade, kLowered, kUncountedLowered, kUpdated, kFreed, kSettled };

struct Event {
    uint64_t who;    // guest code (maker, caller, freer) or the job's entry
    uint64_t arg;    // the job's argument; for an update the value the counter held; for a free the unfinished jobs
    int64_t value;   // the value after the change (made: the count; update: the value passed)
    uint32_t ms;     // when, ms since the trace began
    uint32_t kind;
};

struct Slot {
    uint32_t life;      // how many times the counter was made
    uint32_t trouble;   // CounterTrouble bits of this life
    uint32_t next;      // the next event's place in the ring
    uint64_t maker;
    int64_t count;
    uint32_t made_ms;
    Event events[kEvents];
};

Slot g_slots[kSlots];
std::atomic<uint32_t> g_busy[kSlots];   // a lock per slot
std::atomic<int32_t> g_pending[kSlots];   // jobs submitted on the counter and not finished yet
std::atomic<unsigned> g_reports{0};
std::atomic<unsigned> g_uncounted{0};
const unsigned char *g_image = nullptr;
uint64_t g_size = 0;
unsigned char *const *g_manager = nullptr;
CounterUpdate g_update = nullptr;
const std::chrono::steady_clock::time_point g_start = std::chrono::steady_clock::now();

uint64_t guest(uint64_t a) {
    const uint64_t base = reinterpret_cast<uint64_t>(g_image);
    return a >= base && a < base + g_size ? a - base : a;
}

uint32_t now_ms() {
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_start).count());
}

// the counter's place in the pool (the job manager +0xae0: counters at +0x120, count at +0x128, 0x80 bytes each)
int slot_of(const void *counter) {
    const unsigned char *manager = g_manager ? *g_manager : nullptr;
    if (!manager || !counter) return -1;
    const unsigned char *pool = manager + 0xae0;
    const uintptr_t base = *reinterpret_cast<const uintptr_t *>(pool + 0x120);
    const uint32_t capacity = *reinterpret_cast<const uint32_t *>(pool + 0x128);
    const uintptr_t c = reinterpret_cast<uintptr_t>(counter);
    if (c < base || (c - base) % 0x80) return -1;
    const uintptr_t i = (c - base) / 0x80;
    return i < capacity && i < kSlots ? static_cast<int>(i) : -1;
}

void lock(int i) {
    while (g_busy[i].exchange(1, std::memory_order_acquire))
        while (g_busy[i].load(std::memory_order_relaxed)) {}
}
void unlock(int i) { g_busy[i].store(0, std::memory_order_release); }

void note(Slot &s, uint32_t kind, uint64_t who, uint64_t arg, int64_t value) {
    s.events[s.next % kEvents] = Event{who, arg, value, now_ms(), kind};
    s.next++;
}

bool may_report() {
    const unsigned n = ++g_reports;
    return n <= 40 || n % 100 == 0;
}

// the history as text, newest event first (the slot is locked)
void describe(const Slot &s, char *out, size_t size) {
    if (!size) return;
    out[0] = 0;
    const uint32_t now = now_ms();
    size_t used = 0;
    auto add = [&](const char *format, auto... values) {
        if (used >= size) return;
        const int n = std::snprintf(out + used, size - used, format, values...);
        if (n > 0) used += static_cast<size_t>(n);
    };
    if (!s.life) add("%s", "made before the trace");
    else add("life %u made %u ms ago by guest %#llx for %lld", s.life, now - s.made_ms,
             static_cast<unsigned long long>(s.maker), static_cast<long long>(s.count));
    if (s.trouble & kCounterReused) add("%s", "; made while jobs of its last life were unfinished");
    if (s.trouble & kCounterBelowZero) add("%s", "; a job took it below 0");
    if (s.trouble & kCounterFreedBusy) add("%s", "; given back with jobs unfinished");
    if (s.trouble & kCounterSettled) add("%s", "; settled by the runtime");
    add("%s", "; events (newest first):");
    for (uint32_t i = 0; i < kEvents && i < s.next; i++) {
        const Event &e = s.events[(s.next - 1 - i) % kEvents];
        const unsigned long long who = e.who, arg = e.arg;
        const long long value = e.value;
        const unsigned ago = now - e.ms;
        switch (e.kind) {
        case kMade: add(" [-%u ms made by guest %#llx for %lld]", ago, who, value); break;
        case kLowered: add(" [-%u ms job %#llx arg %#llx lowered it to %lld]", ago, who, arg, value); break;
        case kUncountedLowered:
            add(" [-%u ms job %#llx arg %#llx (not counted) lowered it to %lld]", ago, who, arg, value);
            break;
        case kUpdated: add(" [-%u ms guest %#llx set %lld (value %lld)]", ago, who, value, static_cast<long long>(arg)); break;
        case kFreed: add(" [-%u ms given back by guest %#llx at %lld, %llu jobs unfinished]", ago, who, value, arg); break;
        case kSettled: add(" [-%u ms settled from %lld]", ago, value); break;
        default: break;
        }
    }
}

void report(const char *what, const void *counter, const Slot &s) {
    char story[900];
    describe(s, story, sizeof story);
    std::printf("Uncharted 3: job counter %p %s; %s (report %u)\n", counter, what, story, g_reports.load());
    std::fflush(stdout);
}

// the queue push (rdi = the list, sil = 1 when appended): a list the game appends holds jobs it submits; a list pushed
// to the front holds jobs released from their waits, counted when their list was submitted.
// The list: +0x4 write index, +0x8 job count, +0x10 the jobs (0x40 bytes each, the counter at +0x10), +0x18 capacity.
__attribute__((sysv_abi)) void on_push(const unsigned char *list, uint64_t append) {
    if (!list || !(append & 0xff)) return;
    const int32_t write = *reinterpret_cast<const int32_t *>(list + 0x4);
    const int32_t count = *reinterpret_cast<const int32_t *>(list + 0x8);
    const unsigned char *jobs = *reinterpret_cast<unsigned char *const *>(list + 0x10);
    const int32_t capacity = *reinterpret_cast<const int32_t *>(list + 0x18);
    if (!jobs || count <= 0 || capacity <= 0 || count > capacity) return;
    const int32_t first = ((write - count) % capacity + capacity) % capacity;
    for (int32_t i = 0; i < count; i++) {
        const unsigned char *job = jobs + 0x40 * static_cast<uint64_t>((first + i) % capacity);
        const int slot = slot_of(*reinterpret_cast<void *const *>(job + 0x10));
        if (slot >= 0) g_pending[slot].fetch_add(1, std::memory_order_acq_rel);
    }
}

__attribute__((sysv_abi)) void on_made(const void *counter, int64_t count, uint64_t ret) {
    const int i = slot_of(counter);
    if (i < 0) return;
    const int32_t pending = g_pending[i].load(std::memory_order_acquire);
    Slot &s = g_slots[i];
    lock(i);
    s.life++;
    s.maker = guest(ret);
    s.count = count;
    s.made_ms = now_ms();
    s.trouble = pending > 0 ? kCounterReused : 0;
    note(s, kMade, s.maker, 0, count);
    if (pending <= 0) {
        unlock(i);
        return;
    }
    const Slot copy = s;
    unlock(i);
    if (!may_report()) return;
    char what[160];
    std::snprintf(what, sizeof what, "made again while %d jobs counted on its last life have not finished (they will "
                  "lower this life)", pending);
    report(what, counter, copy);
}

// after the lowering's lock xadd: before = the value it had
__attribute__((sysv_abi)) void on_lowered(const void *counter, int64_t before, uint64_t entry, uint64_t arg) {
    const int i = slot_of(counter);
    if (i < 0) return;
    int32_t p = g_pending[i].load(std::memory_order_acquire);
    while (p > 0 && !g_pending[i].compare_exchange_weak(p, p - 1, std::memory_order_acq_rel)) {}
    const bool counted = p > 0;
    if (!counted) ++g_uncounted;
    Slot &s = g_slots[i];
    lock(i);
    note(s, counted ? kLowered : kUncountedLowered, guest(entry), arg, before - 1);
    if (before != 0 || (s.trouble & kCounterBelowZero)) {
        unlock(i);
        return;
    }
    s.trouble |= kCounterBelowZero;
    const Slot copy = s;
    unlock(i);
    if (may_report()) report("taken from 0 to -1 by a job (more jobs finished on it than it was made for)", counter, copy);
}

// the counter update's entry: game code changed the counter and passes its new value
__attribute__((sysv_abi)) void on_updated(const void *counter, int64_t value, uint64_t ret) {
    const int i = slot_of(counter);
    if (i < 0) return;
    const int64_t held = __atomic_load_n(reinterpret_cast<const int64_t *>(static_cast<const unsigned char *>(counter) + 0x20),
                                         __ATOMIC_RELAXED);
    Slot &s = g_slots[i];
    lock(i);
    note(s, kUpdated, guest(ret), static_cast<uint64_t>(held), value);
    unlock(i);
}

// before its bit in the pool's map is cleared: another thread can take the counter only after that
__attribute__((sysv_abi)) void on_freed(const void *counter, uint64_t ret) {
    const int i = slot_of(counter);
    if (i < 0) return;
    const int32_t pending = g_pending[i].load(std::memory_order_acquire);
    const int64_t held = __atomic_load_n(reinterpret_cast<const int64_t *>(static_cast<const unsigned char *>(counter) + 0x20),
                                         __ATOMIC_RELAXED);
    Slot &s = g_slots[i];
    lock(i);
    note(s, kFreed, guest(ret), static_cast<uint64_t>(pending > 0 ? pending : 0), held);
    if (pending <= 0 || (s.trouble & kCounterFreedBusy)) {
        unlock(i);
        return;
    }
    s.trouble |= kCounterFreedBusy;
    const Slot copy = s;
    unlock(i);
    if (!may_report()) return;
    char what[120];
    std::snprintf(what, sizeof what, "given back while %d jobs counted on it have not finished", pending);
    report(what, counter, copy);
}

// the queue push and the counter update start the same way: push rbp; mov rbp,rsp; push r15 (6 bytes)
constexpr unsigned kPrologue = 6;
// the queue push: push rbp; mov rbp,rsp; push r15; push r14; push rbx; push rax; movsxd rax,[rdi+0x4c]; lea r14,[rip+..]
constexpr unsigned char kPush[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53,
                                   0x50, 0x48, 0x63, 0x47, 0x4c, 0x4c, 0x8d, 0x35};
// the counter update: push rbp; mov rbp,rsp; push r15; push r14; push r12; push rbx; mov r14,rsi; mov r12,rdi;
// lea r15,[rip+..] (the job manager)
constexpr unsigned char kUpdate[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54,
                                     0x53, 0x49, 0x89, 0xf6, 0x49, 0x89, 0xfc, 0x4c, 0x8d, 0x3d};
// made (0xd96620): rdtsc; shl rdx,32; or rdx,rax; mov [rbx+0x18],rdx; xchg r13,[rbx+0x20]; mov [rbp-0x38],r13; ...
constexpr unsigned char kMake[] = {0x0f, 0x31, 0x48, 0xc1, 0xe2, 0x20, 0x48, 0x09, 0xc2, 0x48, 0x89,
                                   0x53, 0x18, 0x4c, 0x87, 0x6b, 0x20, 0x4c, 0x89, 0x6d, 0xc8, 0x48,
                                   0x8b, 0x45, 0xc8, 0x49, 0x8b, 0x3e};
constexpr unsigned kMakeAt = 13, kMakeLength = 8;   // xchg r13,[rbx+0x20]; mov [rbp-0x38],r13
// made inline in RunJobs: rdtsc; shl rdx,32; or rdx,rax; mov [r13+0x18],rdx; mov rax,r14; xchg [r13+0x20],rax;
// mov [rbp-0x38],rax; mov rax,[rbp-0x38]; mov rdi,[r12]
constexpr unsigned char kMakeInline[] = {0x0f, 0x31, 0x48, 0xc1, 0xe2, 0x20, 0x48, 0x09, 0xc2, 0x49, 0x89,
                                         0x55, 0x18, 0x4c, 0x89, 0xf0, 0x49, 0x87, 0x45, 0x20, 0x48, 0x89,
                                         0x45, 0xc8, 0x48, 0x8b, 0x45, 0xc8, 0x49, 0x8b, 0x3c, 0x24};
constexpr unsigned kMakeInlineAt = 16, kMakeInlineLength = 8;   // xchg [r13+0x20],rax; mov [rbp-0x38],rax
// the lowering: mov rax,-1; mov rdi,[rbp-0x78]; lock xadd [rdi+0x20],rax; mov [rbp-0x40],rax; mov rsi,[rbp-0x40];
// dec rsi; call (the waiters' release)
constexpr unsigned char kLower[] = {0x48, 0xc7, 0xc0, 0xff, 0xff, 0xff, 0xff, 0x48, 0x8b, 0x7d, 0x88, 0xf0,
                                    0x48, 0x0f, 0xc1, 0x47, 0x20, 0x48, 0x89, 0x45, 0xc0, 0x48, 0x8b, 0x75,
                                    0xc0, 0x48, 0xff, 0xce, 0xe8};
constexpr unsigned kLowerAt = 11, kLowerLength = 10;   // lock xadd [rdi+0x20],rax; mov [rbp-0x40],rax
// given back (both places, the counter in r12): shr rcx,6; and [r13+rcx*8+0xb00],rax (its bit in the pool's map); sfence
constexpr unsigned char kFree[] = {0x48, 0xc1, 0xe9, 0x06, 0x49, 0x21, 0x84, 0xcd,
                                   0x00, 0x0b, 0x00, 0x00, 0x0f, 0xae, 0xf8};
constexpr unsigned kFreeAt = 4, kFreeLength = 8;

struct Writer {
    unsigned char *p;
    unsigned n = 0;
    void put(std::initializer_list<unsigned char> b) { for (unsigned char c : b) p[n++] = c; }
    void bytes(const unsigned char *b, unsigned length) { std::memcpy(p + n, b, length); n += length; }
    void imm64(uint64_t v) { std::memcpy(p + n, &v, 8); n += 8; }
    // rax rcx rdx rsi rdi r8..r11 (72 bytes)
    void save() { put({0x50, 0x51, 0x52, 0x56, 0x57, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53}); }
    void restore() { put({0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5f, 0x5e, 0x5a, 0x59, 0x58}); }
    // push rbp; mov rbp,rsp; and rsp,-16; mov rax,h; call rax; mov rsp,rbp; pop rbp
    void call(uint64_t h) {
        put({0x55, 0x48, 0x89, 0xe5, 0x48, 0x83, 0xe4, 0xf0, 0x48, 0xb8});
        imm64(h);
        put({0xff, 0xd0, 0x48, 0x89, 0xec, 0x5d});
    }
    void jump(uint64_t to) { put({0xff, 0x25, 0, 0, 0, 0}); imm64(to); }
    unsigned char *align() {   // the next stub on a 16-byte boundary
        n = (n + 15) & ~15u;
        return p + n;
    }
};

}  // namespace

int64_t pending_jobs(const void *counter) {
    const int i = slot_of(counter);
    return i < 0 ? -1 : g_pending[i].load(std::memory_order_acquire);
}

void forget_pending_jobs(const void *counter) {
    const int i = slot_of(counter);
    if (i >= 0) g_pending[i].store(0, std::memory_order_release);
}

uint32_t counter_trouble(const void *counter, uint32_t *life) {
    const int i = slot_of(counter);
    if (i < 0) return 0;
    Slot &s = g_slots[i];
    lock(i);
    const uint32_t trouble = s.trouble;
    if (life) *life = s.life;
    unlock(i);
    return trouble;
}

void note_settled(const void *counter, int64_t from) {
    const int i = slot_of(counter);
    if (i < 0) return;
    Slot &s = g_slots[i];
    lock(i);
    note(s, kSettled, 0, 0, from);
    s.trouble |= kCounterSettled;
    unlock(i);
}

void counter_story(const void *counter, char *out, size_t size) {
    if (!size) return;
    out[0] = 0;
    const int i = slot_of(counter);
    if (i < 0) return;
    Slot &s = g_slots[i];
    lock(i);
    const Slot copy = s;
    unlock(i);
    const int pending = g_pending[i].load(std::memory_order_acquire);
    const int used = std::snprintf(out, size, "%d jobs counted on it unfinished (%u lowerings not counted in all); ",
                                   pending, g_uncounted.load());
    if (used > 0 && static_cast<size_t>(used) < size) describe(copy, out + used, size - used);
}

const unsigned char *job_manager() { return g_manager ? *g_manager : nullptr; }

CounterUpdate counter_update_function() { return g_update; }

bool install_job_trace(unsigned char *image, uint64_t size) {
    const uint64_t push = find_unique(image, size, kPush, sizeof kPush);
    const uint64_t update = find_unique(image, size, kUpdate, sizeof kUpdate);
    const uint64_t make = find_unique(image, size, kMake, sizeof kMake);
    const uint64_t inline_make = find_unique(image, size, kMakeInline, sizeof kMakeInline);
    const uint64_t lower = find_unique(image, size, kLower, sizeof kLower);
    uint64_t frees[2];
    unsigned nfrees = 0;
    for (uint64_t at = 0; at + sizeof kFree <= size; at++)
        if (image[at] == kFree[0] && std::memcmp(image + at, kFree, sizeof kFree) == 0) {
            if (nfrees == 2) return false;   // not the build this was written for
            frees[nfrees++] = at;
        }
    if (push == kNotFound || update == kNotFound || make == kNotFound || inline_make == kNotFound ||
        lower == kNotFound || nfrees != 2)
        return false;
    unsigned char *page = stub_page_near(image, size);
    if (!page) return false;
    g_image = image;
    g_size = size;
    const unsigned char *lea = image + update + sizeof kUpdate - 3;   // lea r15,[rip+disp32]: the job manager
    int32_t disp;
    std::memcpy(&disp, lea + 3, 4);
    g_manager = reinterpret_cast<unsigned char *const *>(lea + 7 + disp);

    Writer w{page};
    // the queue push and the counter update, at their entry: the arguments go to the handler (the update's caller is
    // at [rsp+72] after the saves), then the prologue is replayed
    unsigned char *push_stub = w.align();
    w.save();
    w.call(reinterpret_cast<uint64_t>(&on_push));
    w.restore();
    w.bytes(image + push, kPrologue);
    w.jump(reinterpret_cast<uint64_t>(image + push + kPrologue));

    unsigned char *update_stub = w.align();
    w.save();
    w.put({0x48, 0x8b, 0x54, 0x24, 0x48});   // mov rdx,[rsp+0x48] (who called the update)
    w.call(reinterpret_cast<uint64_t>(&on_updated));
    w.restore();
    w.bytes(image + update, kPrologue);
    w.jump(reinterpret_cast<uint64_t>(image + update + kPrologue));

    // made: the counter rbx, the count r13, the maker's return address [rbp+8]; then the replaced code
    unsigned char *make_stub = w.align();
    w.save();
    w.put({0x48, 0x89, 0xdf});         // mov rdi,rbx
    w.put({0x4c, 0x89, 0xee});         // mov rsi,r13
    w.put({0x48, 0x8b, 0x55, 0x08});   // mov rdx,[rbp+8]
    w.call(reinterpret_cast<uint64_t>(&on_made));
    w.restore();
    w.bytes(image + make + kMakeAt, kMakeLength);
    w.jump(reinterpret_cast<uint64_t>(image + make + kMakeAt + kMakeLength));

    // made inline: the counter r13, the count rax, RunJobs' caller [rbp+8]
    unsigned char *inline_stub = w.align();
    w.save();
    w.put({0x4c, 0x89, 0xef});         // mov rdi,r13
    w.put({0x48, 0x89, 0xc6});         // mov rsi,rax
    w.put({0x48, 0x8b, 0x55, 0x08});   // mov rdx,[rbp+8]
    w.call(reinterpret_cast<uint64_t>(&on_made));
    w.restore();
    w.bytes(image + inline_make + kMakeInlineAt, kMakeInlineLength);
    w.jump(reinterpret_cast<uint64_t>(image + inline_make + kMakeInlineAt + kMakeInlineLength));

    // lowered: the replaced code first (the xadd leaves the value before in rax), then the counter rdi, that value,
    // the job's entry rbx (the worker loop keeps it there after the job returns) and its argument [rbp-0x88]
    unsigned char *lower_stub = w.align();
    w.bytes(image + lower + kLowerAt, kLowerLength);
    w.save();
    w.put({0x48, 0x89, 0xc6});                           // mov rsi,rax
    w.put({0x48, 0x89, 0xda});                           // mov rdx,rbx
    w.put({0x48, 0x8b, 0x8d, 0x78, 0xff, 0xff, 0xff});   // mov rcx,[rbp-0x88]
    w.call(reinterpret_cast<uint64_t>(&on_lowered));
    w.restore();
    w.jump(reinterpret_cast<uint64_t>(image + lower + kLowerAt + kLowerLength));

    // given back: noted first, then the bit is cleared
    unsigned char *free_stub[2];
    for (unsigned i = 0; i < 2; i++) {
        free_stub[i] = w.align();
        w.save();
        w.put({0x4c, 0x89, 0xe7});         // mov rdi,r12
        w.put({0x48, 0x8b, 0x75, 0x08});   // mov rsi,[rbp+8]
        w.call(reinterpret_cast<uint64_t>(&on_freed));
        w.restore();
        w.bytes(image + frees[i] + kFreeAt, kFreeLength);
        w.jump(reinterpret_cast<uint64_t>(image + frees[i] + kFreeAt + kFreeLength));
    }
    if (w.n > 0x1000 || !finish_stub(page)) return false;

    // the counting starts before anything is made or lowered through the trace: the push first
    if (!jump_to(image, push, kPrologue, push_stub) || !jump_to(image, update, kPrologue, update_stub) ||
        !jump_to(image, make + kMakeAt, kMakeLength, make_stub) ||
        !jump_to(image, inline_make + kMakeInlineAt, kMakeInlineLength, inline_stub) ||
        !jump_to(image, lower + kLowerAt, kLowerLength, lower_stub) ||
        !jump_to(image, frees[0] + kFreeAt, kFreeLength, free_stub[0]) ||
        !jump_to(image, frees[1] + kFreeAt, kFreeLength, free_stub[1]))
        return false;
    g_update = reinterpret_cast<CounterUpdate>(image + update);
    std::printf("Uncharted 3: job counter history kept (push %#llx, update %#llx, made %#llx and %#llx, lowered %#llx, "
                "given back %#llx and %#llx)\n",
                static_cast<unsigned long long>(push), static_cast<unsigned long long>(update),
                static_cast<unsigned long long>(make + kMakeAt),
                static_cast<unsigned long long>(inline_make + kMakeInlineAt),
                static_cast<unsigned long long>(lower + kLowerAt), static_cast<unsigned long long>(frees[0] + kFreeAt),
                static_cast<unsigned long long>(frees[1] + kFreeAt));
    std::fflush(stdout);
    return true;
}

}  // namespace u3fix
