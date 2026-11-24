// Uncharted 3 1.02: the general malloc pool ("Wrapped Malloc", pool 0x15) is too small for multiplayer.
//
// The game creates its memory pools from a table in its data (image 0x16f8d90, read by the memory setup at 0x5ac2f1):
// 32-byte entries {u32 pool id, u32 type, u64 size, u64 alignment, u64 0}. The ids are pool numbers, not allocation
// contexts: pool 0x14 is ALLOCATION_SCOPED_TEMP_ALLOCS (16 MiB), pool 0x15 is ALLOCATION_WRAPPED_MALLOC_POOL, the general
// malloc pool (343,040 bytes on PS4), shared by every malloc user. On PS3 the network allocation context had its own
// 2.5 MiB "Network Pool", lent from level pages while a network game runs; the PS4 build has no network pool
// (ALLOCATION_NETWORK_HEAP has size 0), so every network allocation (net events, their queue nodes, packet records) also
// lands in Wrapped Malloc. With the restored multiplayer it fills: "Allocation failed: 576 bytes ... Wrapped Malloc
// [F:336 T:343040]", then a write through the null result (0x6ab7fb / 0x6ae308), and the game closes (12 Nov 2026).
//
// 21 Nov 2026: not 16 MiB. The pool is part of the game's fixed CPU memory budget ("Context(kAllocAppCpu) Size Does Not
// Add Up!"): 16 MiB left the CPU heap about 16 MB short and the 8 MiB start-up allocation (FgData) failed (0.10.119).
// 23 Nov 2026: 2 MiB still ran out in a long co-op adventure (F:124592 T:2097152, crash at 0x6ae308). Now 3 MiB: PS3's
// 2.5 MiB network pool plus PS4's own 335 KB for the ordinary malloc users, rounded up. It takes 1 MiB more of the CPU
// budget (ALLOCATION_APP_CPU_FREE about 3,320 KB -> 2,296 KB). A null result at the four network allocation sites that
// wrote through it now skips that one message instead (net_alloc_guard.cpp).
//
// Every 30 s the runtime log gets one line with the pool's use: total, free, the number of free blocks and the largest
// one, read from the game's Wrapped Malloc allocator object, and the null allocations the guard skipped.
#include "u3fix.h"
#include "u3fix_hook.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace u3fix {

namespace {

struct PoolEntry {   // the game's pool table entry
    uint32_t id, type;
    uint64_t size, align, zero;
};
static_assert(sizeof(PoolEntry) == 32);

constexpr uint64_t kOldSize = 0x53c00;    // 343,040 bytes
constexpr uint64_t kNewSize = 0x300000;   // 3 MiB

// The Wrapped Malloc allocator object (image addresses of the 1.02 build): its vtable, its total size (+0xc8) and its
// free list (+0x60 the first free block; a block's size is the u32 at +4, the next block at +0x10).
constexpr uint64_t kAllocator = 0x1bf6b40;
constexpr uint64_t kAllocatorVtable = 0x16c0930;
constexpr uint64_t kTotal = 0xc8, kFreeList = 0x60, kBlockSize = 4, kBlockNext = 0x10;
constexpr unsigned kMaxBlocks = 1000000;   // a list longer than this is being changed under the walk

unsigned char *g_image = nullptr;

// true when [p, p + n) is committed readable memory (the walk takes no lock: a block taken while it reads may hold
// anything where the next pointer was)
bool readable(const void *p, size_t n) {
#ifdef _WIN32
    static uintptr_t lo = 0, hi = 0;   // the last region found readable (only the report thread calls this)
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);
    if (a >= lo && a + n <= hi) return true;
    MEMORY_BASIC_INFORMATION info;
    if (!p || !VirtualQuery(p, &info, sizeof info) || info.State != MEM_COMMIT) return false;
    if (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    lo = reinterpret_cast<uintptr_t>(info.BaseAddress);
    hi = lo + info.RegionSize;
    return a + n <= hi;
#else
    (void)p, (void)n;
    return false;
#endif
}

// the pool's line; nothing while the allocator object is not made yet
void report_pool() {
    const unsigned char *a = g_image + kAllocator;
    uint64_t vtable = 0, total = 0, head = 0;
    std::memcpy(&vtable, a, 8);
    std::memcpy(&total, a + kTotal, 8);
    if (vtable != reinterpret_cast<uint64_t>(g_image + kAllocatorVtable) || total != kNewSize) return;
    std::memcpy(&head, a + kFreeList, 8);
    uint64_t free = 0, blocks = 0, largest = 0;
    bool whole = true;
    for (uint64_t p = head; p; blocks++) {
        const unsigned char *b = reinterpret_cast<const unsigned char *>(p);
        if (blocks == kMaxBlocks || !readable(b, kBlockNext + 8)) {
            whole = false;
            break;
        }
        uint32_t n = 0;
        std::memcpy(&n, b + kBlockSize, 4);
        free += n;
        if (n > largest) largest = n;
        std::memcpy(&p, b + kBlockNext, 8);
    }
    if (free > total) whole = false;
    uint64_t skipped[kNetAllocSites];
    for (unsigned i = 0; i < kNetAllocSites; i++) skipped[i] = net_alloc_skips(i);
    if (whole)
        std::printf("Uncharted 3: Wrapped Malloc pool used %llu of %llu (free %llu in %llu blocks, largest %llu)",
                    static_cast<unsigned long long>(total - free), static_cast<unsigned long long>(total),
                    static_cast<unsigned long long>(free), static_cast<unsigned long long>(blocks),
                    static_cast<unsigned long long>(largest));
    else
        std::printf("Uncharted 3: Wrapped Malloc pool of %llu: free list changed while it was read",
                    static_cast<unsigned long long>(total));
    std::printf("; null network allocations skipped: state %llu, mask node %llu, queue node %llu, retarget node %llu\n",
                static_cast<unsigned long long>(skipped[0]), static_cast<unsigned long long>(skipped[1]),
                static_cast<unsigned long long>(skipped[2]), static_cast<unsigned long long>(skipped[3]));
    std::fflush(stdout);
}

// the 30 s report; a first skipped allocation is reported at once (within a second)
void report_loop() {
    uint64_t seen = 0;
    for (unsigned tick = 1;; tick++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        uint64_t all = 0;
        for (unsigned i = 0; i < kNetAllocSites; i++) all += net_alloc_skips(i);
        if ((seen == 0 && all != 0) || tick % 30 == 0) report_pool();
        if (all) seen = all;
    }
}

}  // namespace

bool raise_net_process_pool(unsigned char *image, uint64_t size) {
    // pool 0x14 (type 5, 16 MiB, align 0x80) followed by pool 0x15 (type 5, 343,040 bytes, align 0x80). Only the first
    // 24 bytes of each entry are compared: the last word is zero in the file but set when the image's relocations are
    // applied, which happens before this runs (21 Nov 2026: compared with it, the pool was never found and never raised,
    // and four-player matches ran out of it again)
    auto is = [&](uint64_t at, uint32_t id, uint64_t bytes) {
        PoolEntry e;
        std::memcpy(&e, image + at, sizeof(e));
        return e.id == id && e.type == 5 && e.size == bytes && e.align == 0x80;
    };
    for (uint64_t at = 0; at + 64 <= size; at += 8) {
        if (!is(at, 0x14, 0x1000000) || !is(at + 32, 0x15, kOldSize)) continue;
        PoolEntry e;
        std::memcpy(&e, image + at + 32, sizeof(e));
        e.size = kNewSize;
        std::memcpy(image + at + 32, &e, sizeof(e));
        std::printf("Uncharted 3: general malloc pool (0x15, Wrapped Malloc) raised from %llu bytes to %llu (pool table "
                    "at %#llx)\n",
                    static_cast<unsigned long long>(kOldSize), static_cast<unsigned long long>(kNewSize),
                    static_cast<unsigned long long>(at));
        std::fflush(stdout);
        if (kAllocator + kTotal + 8 <= size) {
            g_image = image;
            std::thread(report_loop).detach();
        }
        return true;
    }
    return false;
}

}  // namespace u3fix
