// Uncharted 3 1.02, 23 Nov 2026: a job counter on the start-of-frame list is given back once, by the list.
//
// Asynchronous jobs submitted at 0x1090805 make their job counter straight into the next entry of a list (image
// 0x1af89e0, count 0x1afc9e0) and keep the entry's address in their owner (+0x21a0). At the start of the next game
// frame PreProcessUpdate (0x572d70) waits for every counter on the list to reach 0, gives each one back and empties
// the list. The owner's result reader (0x108a4f0, called from 0x109b692) does the same for its own entry when it runs
// first: it waits, gives the counter back (0xd975b0 at 0x108a522) and forgets the entry's address, but the list entry
// still holds the counter. The job counter pool hands out the lowest free counter first, so within the same frame that
// counter is made again for someone else (in co-op Syria: the particle renderer's per-frame counters, made at 0
// before their work is added), and PreProcessUpdate then waits for it (0: no wait) and gives it back a second time,
// while its new owner still uses it. The next owner of the slot gets the old owner's job finishes: counters went
// below 0, waits ended before their work was done and the game stalled or froze (seen on PC, where thread timing
// lets the reader run before PreProcessUpdate far more often than on PS4).
// The reader now only waits (0xd97480, the wait the give-back function starts with, same arguments), also where it is
// inlined for owners embedded in larger objects (0x520f6a, 0xbf27e0); the list keeps
// the counter until PreProcessUpdate gives it back, once. Three unused copies of the reader (0x108a486, 0x1090905,
// 0x10a4751) get the same change. Found by the exact call bytes at each address (the 1.02 image only).
#include "u3fix.h"
#include "u3fix_hook.h"

#include <cstdio>
#include <cstring>

namespace u3fix {

namespace {

constexpr uint64_t kWaitAndGiveBack = 0xd975b0;
constexpr uint64_t kWait = 0xd97480;
// 0x520f6a and 0xbf27e0: the same reader inlined for owners embedded at +0x720 and +0x1c0 (their entry address at
// +0x28c0 = 0x720 + 0x21a0 and +0x2360 = 0x1c0 + 0x21a0)
constexpr uint64_t kCalls[] = {0x108a522, 0x520f6a, 0xbf27e0, 0x108a486, 0x1090905, 0x10a4751};

int32_t rel_to(uint64_t call, uint64_t target) { return static_cast<int32_t>(target - (call + 5)); }

}  // namespace

bool install_async_counter_once(unsigned char *image, uint64_t size) {
    for (uint64_t at : kCalls) {   // each must still be: call 0xd975b0
        int32_t rel;
        if (at + 5 > size || image[at] != 0xe8) return false;
        std::memcpy(&rel, image + at + 1, 4);
        if (rel != rel_to(at, kWaitAndGiveBack)) return false;
    }
    for (uint64_t at : kCalls) {
        const int32_t rel = rel_to(at, kWait);
        std::memcpy(image + at + 1, &rel, 4);
    }
    std::printf("Uncharted 3: async job counters on the start-of-frame list are given back once, by the list "
                "(result readers 0x108a522, 0x520f6a and 0xbf27e0 wait only)\n");
    std::fflush(stdout);
    return true;
}

}  // namespace u3fix
