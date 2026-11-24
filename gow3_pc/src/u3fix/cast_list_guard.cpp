// Uncharted 3 1.02, 23 Nov 2026: the physics casts' cleanup does not run in the middle of a cast kick.
//
// The world casts (havok-wws-cast.cpp) keep a list of per-cast job counters (image 0x1d85498, owners at 0x1d85490,
// its length the count of the job counter at 0x1d854a0). A kick (0x114ccf0 and 0x114d310) takes a place in the list
// (lock xadd on the length), makes the cast's counter and writes it there, and only then raises the casts' master
// counter (0x1b24e70). Once a frame the cleanup (0x111b390) waits for the master counter to reach 0, gives back every
// counter in the list and empties it. When the cleanup ran between a kick's list write and its master raise, the
// master was 0, so it gave back the kick's new counter; the kick went on using it while the pool handed it to someone
// else (in co-op Syria: the particle renderer's per-frame counters), whose next owner got the cast's job finishes:
// counters went below 0, waits ended early and the game stalled, froze or crashed (0xf88f74). On PS4 a kick is never
// interrupted inside those few instructions; on a PC the thread can be.
// Now a kick counts as in progress from its list place to its master raise, and the cleanup, after its master wait,
// lets the kicks in progress finish and checks the master again (waiting again when it is no longer 0) before it gives
// anything back; no kick starts while the cleanup empties the list. The cleanup waits for kicks, never the other way
// round, and a kick holds nothing while it runs, so nothing can wait in a circle. Found by the exact bytes at each
// address (the 1.02 image only).
#include "u3fix.h"
#include "u3fix_hook.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <thread>

namespace u3fix {

namespace {

constexpr uint64_t kWait = 0xd97480;     // wait for a counter's value (counter, value, mode)
constexpr uint64_t kUpdate = 0xd969c0;   // release the waiters for a counter's value (counter, value)
constexpr uint64_t kMasterCounter = 0x1b24e70;

using Wait = __attribute__((sysv_abi)) void (*)(void *, int64_t, int64_t);
using Update = __attribute__((sysv_abi)) void (*)(void *, int64_t);
Wait g_wait = nullptr;
Update g_update = nullptr;

std::atomic<int> g_kicks{0};
std::atomic<bool> g_cleaning{false};
std::atomic<unsigned> g_waited_for_kicks{0}, g_master_again{0};

__attribute__((sysv_abi)) void kick_start() {
    for (;;) {
        while (g_cleaning.load(std::memory_order_seq_cst)) std::this_thread::yield();
        g_kicks.fetch_add(1, std::memory_order_seq_cst);
        if (!g_cleaning.load(std::memory_order_seq_cst)) return;
        g_kicks.fetch_sub(1, std::memory_order_seq_cst);
    }
}

__attribute__((sysv_abi)) void kick_end() { g_kicks.fetch_sub(1, std::memory_order_seq_cst); }

void note(const char *what, std::atomic<unsigned> &count) {
    const unsigned n = ++count;
    if (n <= 10 || n % 100 == 0) {
        std::printf("Uncharted 3: the physics casts' cleanup %s (time %u)\n", what, n);
        std::fflush(stdout);
    }
}

// replaces the cleanup's master wait (the call at 0x111b3c6: rdi = the master counter, esi = edx = 0)
__attribute__((sysv_abi)) void cleanup_begin(void *master, int64_t value, int64_t mode) {
    for (;;) {
        g_wait(master, value, mode);
        g_cleaning.store(true, std::memory_order_seq_cst);
        if (g_kicks.load(std::memory_order_seq_cst) > 0) {
            note("waited for a cast kick in progress", g_waited_for_kicks);
            while (g_kicks.load(std::memory_order_seq_cst) > 0) std::this_thread::yield();
        }
        const int64_t now = __atomic_load_n(reinterpret_cast<const int64_t *>(
                                                static_cast<const unsigned char *>(master) + 0x20),
                                            __ATOMIC_SEQ_CST);
        if (now == value) return;   // the list is the cleanup's until cleanup_end
        g_cleaning.store(false, std::memory_order_seq_cst);
        note("found new casts after its wait and waited again", g_master_again);
    }
}

// replaces the cleanup's last call (0x111b45f: the length counter's waiters released after it was set to 0)
__attribute__((sysv_abi)) void cleanup_end(void *counter, int64_t value) {
    g_update(counter, value);
    g_cleaning.store(false, std::memory_order_seq_cst);
}

void put_bytes(unsigned char *stub, unsigned &n, std::initializer_list<unsigned char> bytes) {
    for (unsigned char b : bytes) stub[n++] = b;
}

// at `at`: save every register and the flags, call handler(), restore, run the `length` bytes replaced (or, when
// `call_target` is set, the bytes before a 5-byte call and then that call), go on after them
bool hook_around(unsigned char *image, uint64_t size, uint64_t at, unsigned length, uint64_t handler,
                 uint64_t call_target = 0) {
    unsigned char *stub = stub_page_near(image, size);
    if (!stub) return false;
    unsigned n = 0;
    put_bytes(stub, n, {0x9c, 0x55, 0x48, 0x89, 0xe5, 0x48, 0x83, 0xe4, 0xf0, 0x50, 0x51, 0x52, 0x56, 0x57, 0x41, 0x50,
                        0x41, 0x51, 0x41, 0x52, 0x41, 0x53, 0x48, 0x83, 0xec, 0x08, 0x48, 0xb8});
    std::memcpy(stub + n, &handler, 8);
    n += 8;
    put_bytes(stub, n, {0xff, 0xd0, 0x48, 0x83, 0xc4, 0x08, 0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5f, 0x5e,
                        0x5a, 0x59, 0x58, 0x48, 0x89, 0xec, 0x5d, 0x9d});
    if (call_target) {
        std::memcpy(stub + n, image + at, length - 5);   // the instructions before the call
        n += length - 5;
        const uint64_t target = reinterpret_cast<uint64_t>(image + call_target);
        put_bytes(stub, n, {0x48, 0xb8});   // mov rax, target; call rax
        std::memcpy(stub + n, &target, 8);
        n += 8;
        put_bytes(stub, n, {0xff, 0xd0});
    } else {
        std::memcpy(stub + n, image + at, length);
        n += length;
    }
    const uint64_t back = reinterpret_cast<uint64_t>(image + at + length);
    put_bytes(stub, n, {0xff, 0x25, 0x00, 0x00, 0x00, 0x00});
    std::memcpy(stub + n, &back, 8);
    n += 8;
    return finish_stub(stub) && jump_to(image, at, length, stub);
}

// the call at `at` (call rel32) goes to `handler` instead, through a stub near the image
bool retarget_call(unsigned char *image, uint64_t size, uint64_t at, uint64_t handler) {
    unsigned char *stub = stub_page_near(image, size);
    if (!stub) return false;
    const unsigned char jump[] = {0xff, 0x25, 0x00, 0x00, 0x00, 0x00};
    std::memcpy(stub, jump, sizeof jump);
    std::memcpy(stub + sizeof jump, &handler, 8);
    if (!finish_stub(stub)) return false;
    const int64_t rel = reinterpret_cast<int64_t>(stub) - reinterpret_cast<int64_t>(image + at + 5);
    if (rel < INT32_MIN || rel > INT32_MAX) return false;
    const int32_t rel32 = static_cast<int32_t>(rel);
    std::memcpy(image + at + 1, &rel32, 4);
    return true;
}

struct Expect {
    uint64_t at;
    unsigned length;
    unsigned char bytes[8];
};
// the kicks' start (mov eax,1 before the length xadd), their ends (after the master raise: the casts at once and the
// jobs path of each kick) and the cleanup's two calls
const Expect kStartA{0x114cd55, 5, {0xb8, 0x01, 0x00, 0x00, 0x00}};
const Expect kStartB{0x114d375, 5, {0xb8, 0x01, 0x00, 0x00, 0x00}};
const Expect kEndA1{0x114ce22, 6, {0x41, 0x80, 0x4c, 0x24, 0x1f, 0x02}};   // or byte [r12+0x1f],2
const Expect kEndA2{0x114cef4, 6, {0x49, 0x89, 0xd9, 0x45, 0x85, 0xc9}};   // mov r9,rbx; test r9d,r9d
const Expect kEndB1{0x114d446, 7, {0x80, 0x4b, 0x1f, 0x02, 0x8b, 0x43, 0x30}};   // or byte [rbx+0x1f],2; mov eax,..
const Expect kEndB2{0x114d518, 8, {0x4c, 0x89, 0xfe, 0xe8, 0xa0, 0x94, 0xc4, 0xff}};   // mov rsi,r15; call update
const Expect kCleanupWait{0x111b3c6, 5, {0xe8, 0xb5, 0xc0, 0xc7, 0xff}};   // call 0xd97480
const Expect kCleanupLast{0x111b45f, 5, {0xe8, 0x5c, 0xb5, 0xc7, 0xff}};   // call 0xd969c0

bool matches(const unsigned char *image, uint64_t size, const Expect &e) {
    return e.at + e.length <= size && std::memcmp(image + e.at, e.bytes, e.length) == 0;
}

}  // namespace

bool install_cast_list_guard(unsigned char *image, uint64_t size) {
    for (const Expect *e : {&kStartA, &kStartB, &kEndA1, &kEndA2, &kEndB1, &kEndB2, &kCleanupWait, &kCleanupLast})
        if (!matches(image, size, *e)) return false;
    g_wait = reinterpret_cast<Wait>(image + kWait);
    g_update = reinterpret_cast<Update>(image + kUpdate);
    const uint64_t start = reinterpret_cast<uint64_t>(&kick_start), end = reinterpret_cast<uint64_t>(&kick_end);
    // the cleanup first: until its calls are retargeted, the kicks' counts are not looked at
    bool ok = retarget_call(image, size, kCleanupWait.at, reinterpret_cast<uint64_t>(&cleanup_begin)) &&
              retarget_call(image, size, kCleanupLast.at, reinterpret_cast<uint64_t>(&cleanup_end));
    ok = ok && hook_around(image, size, kEndA1.at, kEndA1.length, end) &&
         hook_around(image, size, kEndA2.at, kEndA2.length, end) &&
         hook_around(image, size, kEndB1.at, kEndB1.length, end) &&
         hook_around(image, size, kEndB2.at, kEndB2.length, end, kUpdate) &&
         hook_around(image, size, kStartA.at, kStartA.length, start) &&
         hook_around(image, size, kStartB.at, kStartB.length, start);
    std::printf("Uncharted 3: the physics casts' cleanup %s a cast kick in progress (kicks 0x114ccf0 / 0x114d310, "
                "cleanup 0x111b390)\n",
                ok ? "waits for" : "could NOT be made to wait for");
    std::fflush(stdout);
    return ok;
}

}  // namespace u3fix
