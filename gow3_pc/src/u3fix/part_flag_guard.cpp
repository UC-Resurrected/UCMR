// Uncharted 3 1.02, 23 Nov 2026: a character's part list may hold an empty entry; showing or hiding the parts skips it.
//
// One game function (image 0x91ca70..0x91cbc0) sets or clears a flag (bit 0x20 of word +0x1b8) on every part of an
// object and of the object it is linked to: object +0x130 points to a list {+0x10 groups of 0x68 bytes, +0x18 count},
// each group holds as many 0x18-byte entries as its byte +0x60 says, and an entry's first word is the part. The four
// loops (set / clear, the object's own parts / the linked object's) write through that word without testing it. In a
// co-op match on a guest (Syria stage 15, 23 Nov 2026) an entry was empty and the clear loop wrote to 0x1b8: the game
// crashed. An empty entry is now skipped; every part that is there gets exactly the write it got before. Found by the
// bytes of each loop (each 18-byte loop is unique in the 1.02 image).
#include "u3fix.h"
#include "u3fix_hook.h"

#include <cstdio>
#include <cstring>

namespace u3fix {

namespace {

// one loop: mov reg,[rsi]; or/and dword [reg+0x1b8],imm8; add rsi,0x18; dec r32; jne back
struct Loop {
    unsigned char site[18];
    unsigned char load[3];    // mov reg,[rsi]
    unsigned char test[3];    // test reg,reg
    unsigned char write[7];   // or / and dword [reg+0x1b8],imm8
};

constexpr Loop kLoops[] = {
    // set, the object's own parts (rax, ebp)
    {{0x48, 0x8b, 0x06, 0x83, 0x88, 0xb8, 0x01, 0x00, 0x00, 0x20, 0x48, 0x83, 0xc6, 0x18, 0xff, 0xcd, 0x75, 0xee},
     {0x48, 0x8b, 0x06}, {0x48, 0x85, 0xc0}, {0x83, 0x88, 0xb8, 0x01, 0x00, 0x00, 0x20}},
    // set, the linked object's parts (rbx, edi)
    {{0x48, 0x8b, 0x1e, 0x83, 0x8b, 0xb8, 0x01, 0x00, 0x00, 0x20, 0x48, 0x83, 0xc6, 0x18, 0xff, 0xcf, 0x75, 0xee},
     {0x48, 0x8b, 0x1e}, {0x48, 0x85, 0xdb}, {0x83, 0x8b, 0xb8, 0x01, 0x00, 0x00, 0x20}},
    // clear, the object's own parts (rax, ebp)
    {{0x48, 0x8b, 0x06, 0x83, 0xa0, 0xb8, 0x01, 0x00, 0x00, 0xdf, 0x48, 0x83, 0xc6, 0x18, 0xff, 0xcd, 0x75, 0xee},
     {0x48, 0x8b, 0x06}, {0x48, 0x85, 0xc0}, {0x83, 0xa0, 0xb8, 0x01, 0x00, 0x00, 0xdf}},
    // clear, the linked object's parts (rbx, edi)
    {{0x48, 0x8b, 0x1e, 0x83, 0xa3, 0xb8, 0x01, 0x00, 0x00, 0xdf, 0x48, 0x83, 0xc6, 0x18, 0xff, 0xcf, 0x75, 0xee},
     {0x48, 0x8b, 0x1e}, {0x48, 0x85, 0xdb}, {0x83, 0xa3, 0xb8, 0x01, 0x00, 0x00, 0xdf}},
};
constexpr unsigned kReplaced = 10;     // the load and the write (the jump goes there)
constexpr unsigned kStubSpace = 0x40;  // bytes per stub in the shared page

}  // namespace

bool install_part_flag_guard(unsigned char *image, uint64_t size) {
    uint64_t at[4];
    for (int i = 0; i < 4; i++) {
        at[i] = find_unique(image, size, kLoops[i].site, sizeof kLoops[i].site);
        if (at[i] == kNotFound) return false;   // another image: leave all four loops as they are
    }
    unsigned char *page = stub_page_near(image, size);
    if (!page) return false;
    for (int i = 0; i < 4; i++) {
        // load; test; jz skip; write; skip: jmp [rip] -> the loop's add rsi
        unsigned char *s = page + kStubSpace * i, *p = s;
        std::memcpy(p, kLoops[i].load, 3), p += 3;
        std::memcpy(p, kLoops[i].test, 3), p += 3;
        *p++ = 0x74;
        *p++ = 7;   // jz over the write
        std::memcpy(p, kLoops[i].write, 7), p += 7;
        const unsigned char jmp[] = {0xff, 0x25, 0, 0, 0, 0};
        std::memcpy(p, jmp, sizeof jmp), p += sizeof jmp;
        const uint64_t back = reinterpret_cast<uint64_t>(image + at[i] + kReplaced);
        std::memcpy(p, &back, 8);
    }
    if (!finish_stub(page)) return false;
    for (int i = 0; i < 4; i++)
        if (!jump_to(image, at[i], kReplaced, page + kStubSpace * i)) return false;
    std::printf("Uncharted 3: showing or hiding a character's parts skips an empty part entry (%#llx)\n",
                static_cast<unsigned long long>(at[0]));
    std::fflush(stdout);
    return true;
}

}  // namespace u3fix
