// Uncharted 3 1.02, 23 Nov 2026: a failed network allocation skips that one message instead of crashing.
//
// Four places on the network path take memory from the general malloc pool (net_pool.cpp) and write through the result
// without testing it. When the pool is out of a fitting block the allocator reports "Allocation failed" and returns
// null, and the next write crashes the game (co-op host, 23 Nov 2026, at 0x6ae308). Each place now tests the result:
// - the NPC character state message (576 bytes, sent unreliably to the remote players; image 0x6ae300): on null the
//   message is not built or sent, and the function leaves through its normal end, which also leaves the network
//   allocation context it entered. The next state message replaces the skipped one, as for a lost packet;
// - the queue node made for each target connection when an event is sent to a set of players (0x733aaf), the queue node
//   made when an event is queued on one connection (0x6a8344), and the queue node made when queued events are moved to
//   another connection (0x6aa6d3): on null that event is not queued for that connection and its reference count is not
//   raised; the other connections get it as before.
// Every non-null result runs exactly the instructions it ran before. The skips are counted; net_pool.cpp's 30 s pool
// line prints the counts (the first skip at once). This is a safety net only: the pool size is what must keep it from
// firing. Each place is found by its bytes; the code each null path jumps to is checked by its bytes too.
#include "u3fix.h"
#include "u3fix_hook.h"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace u3fix {

namespace {

std::atomic<uint64_t> g_skips[kNetAllocSites];
static_assert(sizeof(std::atomic<uint64_t>) == 8 && std::atomic<uint64_t>::is_always_lock_free);

struct Site {
    const char *name;
    unsigned char bytes[56];   // the allocation call and what follows it (unique in the 1.02 image)
    unsigned length;
    unsigned at;               // the first replaced instruction (right after the call returns)
    unsigned replaced;         // bytes replaced by the jump (whole instructions)
    unsigned char before[3];   // replaced instructions that run before the test (rax untouched)
    unsigned before_length;
    unsigned char after[8];    // replaced instructions that run after a non-null test
    unsigned after_length;
    unsigned char restore[10]; // the registers the normal path restores before reaching the null path's target
    unsigned restore_length;
    int64_t target;            // where the null path goes, from at
    unsigned char check[9];    // the bytes there
};

constexpr unsigned char kZeroNext[8] = {0x48, 0xc7, 0x40, 0x10, 0x00, 0x00, 0x00, 0x00};   // mov qword [rax+0x10],0

const Site kSites[kNetAllocSites] = {
    // the state message: mov edi,0x240; mov edx,0x10; call alloc; mov rbx,rax; mov byte [rbx+9],2; mov byte [rbx+8],0x10
    // -> null: the function's end that leaves the allocation context (mov rax,fs:[0] ...)
    {"state message",
     {0xbf, 0x40, 0x02, 0x00, 0x00, 0xba, 0x10, 0x00, 0x00, 0x00, 0xe8, 0x8b, 0x06, 0x6f, 0x00, 0x48, 0x89, 0xc3, 0xc6,
      0x43, 0x09, 0x02, 0xc6, 0x43, 0x08, 0x10},
     26, 15, 7, {0x48, 0x89, 0xc3}, 3, {0xc6, 0x43, 0x09, 0x02}, 4, {}, 0, 0x6ae6ad - 0x6ae305,
     {0x64, 0x48, 0x8b, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00}},
    // send to a set of players: mov dword [rbp-0x60],0x14; mov edi,0x28; mov edx,8; lea rsi,[rbp-0x60]; call alloc;
    // mov qword [rax+0x10],0; mov rcx,[rbp-0x68]; mov [rax],rcx; inc byte [rcx+0xa]
    // -> null: the next connection (inc rbx; cmp rbx,10; jne)
    {"mask node",
     {0xc7, 0x45, 0xa0, 0x14, 0x00, 0x00, 0x00, 0xbf, 0x28, 0x00, 0x00, 0x00, 0xba, 0x08, 0x00, 0x00, 0x00, 0x48, 0x8d,
      0x75, 0xa0, 0xe8, 0xdc, 0xae, 0x66, 0x00, 0x48, 0xc7, 0x40, 0x10, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x4d, 0x98,
      0x48, 0x89, 0x08, 0xfe, 0x41, 0x0a},
     44, 26, 8, {}, 0, {}, 0, {}, 0, 0x733b50 - 0x733ab4, {0x48, 0xff, 0xc3, 0x48, 0x83, 0xfb, 0x0a, 0x0f, 0x85}},
    // queue on one connection: mov dword [rbp-0x38],0x14; lea rsi,[rbp-0x38]; mov edi,0x28; mov edx,8; call alloc;
    // mov rbx,rax; mov qword [rbx+0x10],0; mov [rbx],r15; inc byte [r15+0xa]
    // -> null: the function's end (the stack check and return)
    {"queue node",
     {0xc7, 0x45, 0xc8, 0x14, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x75, 0xc8, 0xbf, 0x28, 0x00, 0x00, 0x00, 0xba, 0x08, 0x00,
      0x00, 0x00, 0xe8, 0x47, 0x66, 0x6f, 0x00, 0x48, 0x89, 0xc3, 0x48, 0xc7, 0x43, 0x10, 0x00, 0x00, 0x00, 0x00, 0x4c,
      0x89, 0x3b, 0x41, 0xfe, 0x47, 0x0a},
     44, 26, 11, {0x48, 0x89, 0xc3}, 3, {0x48, 0xc7, 0x43, 0x10, 0x00, 0x00, 0x00, 0x00}, 8, {}, 0, 0x6a841d - 0x6a8349,
     {0x48, 0x8d, 0x05, 0x04, 0xed, 0x03, 0x01, 0x48, 0x8b}},
    // move to another connection: mov [rbp-0x58],rsi; mov [rbp-0x4c],edx; mov rbx,rcx; mov dword [rbp-0x38],0x14;
    // mov edi,0x28; mov edx,8; lea rsi,[rbp-0x38]; call alloc; mov qword [rax+0x10],0; mov rcx,[rbp-0x48]; mov [rax],rcx;
    // inc byte [rcx+0xa] -> null: rcx, edx, rsi back as the normal path sets them, then the next event (inc r12; cmp)
    {"retarget node",
     {0x48, 0x89, 0x75, 0xa8, 0x89, 0x55, 0xb4, 0x48, 0x89, 0xcb, 0xc7, 0x45, 0xc8, 0x14, 0x00, 0x00, 0x00, 0xbf, 0x28,
      0x00, 0x00, 0x00, 0xba, 0x08, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x75, 0xc8, 0xe8, 0xb8, 0x42, 0x6f, 0x00, 0x48, 0xc7,
      0x40, 0x10, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x4d, 0xb8, 0x48, 0x89, 0x08, 0xfe, 0x41, 0x0a},
     54, 36, 8, {}, 0, {}, 0, {0x48, 0x89, 0xd9, 0x8b, 0x55, 0xb4, 0x48, 0x8b, 0x75, 0xa8}, 10, 0x6aa760 - 0x6aa6d8,
     {0x49, 0xff, 0xc4, 0x41, 0x39, 0xd4, 0x0f, 0x85, 0xf4}},
};

constexpr unsigned kStubSpace = 0x80;   // bytes per stub in the shared page

}  // namespace

uint64_t net_alloc_skips(unsigned site) { return site < kNetAllocSites ? g_skips[site].load(std::memory_order_relaxed) : 0; }

bool install_net_alloc_guard(unsigned char *image, uint64_t size) {
    uint64_t at[kNetAllocSites];
    for (unsigned i = 0; i < kNetAllocSites; i++) {
        const Site &s = kSites[i];
        const uint64_t found = find_unique(image, size, s.bytes, s.length);
        if (found == kNotFound) return false;   // another image: leave all four as they are
        at[i] = found + s.at;
        const int64_t target = static_cast<int64_t>(at[i]) + s.target;
        if (target < 0 || static_cast<uint64_t>(target) + sizeof s.check > size ||
            std::memcmp(image + target, s.check, sizeof s.check) != 0)
            return false;
    }
    unsigned char *page = stub_page_near(image, size);
    if (!page) return false;
    for (unsigned i = 0; i < kNetAllocSites; i++) {
        const Site &s = kSites[i];
        const bool zero_next = s.after_length == 0 && s.before_length == 0;   // the node sites that only clear +0x10
        const unsigned char *after = zero_next ? kZeroNext : s.after;
        const unsigned after_length = zero_next ? sizeof kZeroNext : s.after_length;
        unsigned char *p = page + kStubSpace * i;
        const unsigned char jmp[] = {0xff, 0x25, 0, 0, 0, 0};   // jmp [rip], the address follows
        std::memcpy(p, s.before, s.before_length), p += s.before_length;
        const unsigned char test[] = {0x48, 0x85, 0xc0, 0x74, static_cast<unsigned char>(after_length + 14)};
        std::memcpy(p, test, sizeof test), p += sizeof test;   // test rax,rax; jz null
        std::memcpy(p, after, after_length), p += after_length;
        std::memcpy(p, jmp, sizeof jmp), p += sizeof jmp;
        const uint64_t back = reinterpret_cast<uint64_t>(image + at[i] + s.replaced);
        std::memcpy(p, &back, 8), p += 8;
        // null: the restores, then lock inc qword [counter] through rax (null here, set again by the code at the target)
        std::memcpy(p, s.restore, s.restore_length), p += s.restore_length;
        *p++ = 0x48, *p++ = 0xb8;   // mov rax, counter
        const uint64_t counter = reinterpret_cast<uint64_t>(&g_skips[i]);
        std::memcpy(p, &counter, 8), p += 8;
        const unsigned char inc[] = {0xf0, 0x48, 0xff, 0x00};   // lock inc qword [rax]
        std::memcpy(p, inc, sizeof inc), p += sizeof inc;
        std::memcpy(p, jmp, sizeof jmp), p += sizeof jmp;
        const uint64_t target = reinterpret_cast<uint64_t>(image + at[i] + s.target);
        std::memcpy(p, &target, 8);
    }
    if (!finish_stub(page)) return false;
    for (unsigned i = 0; i < kNetAllocSites; i++)
        if (!jump_to(image, at[i], kSites[i].replaced, page + kStubSpace * i)) return false;
    std::printf("Uncharted 3: a failed network allocation skips that message instead of crashing (%#llx %#llx %#llx "
                "%#llx)\n",
                static_cast<unsigned long long>(at[0]), static_cast<unsigned long long>(at[1]),
                static_cast<unsigned long long>(at[2]), static_cast<unsigned long long>(at[3]));
    std::fflush(stdout);
    return true;
}

}  // namespace u3fix
