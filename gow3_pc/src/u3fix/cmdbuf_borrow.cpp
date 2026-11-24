// Uncharted 3 1.02, 20 Nov 2026: a graphics context that starts while its thread's command buffer is still in use
// takes a free one instead of sharing it.
//
// The renderer keeps two command buffers per job worker (and two for all other threads together): a table of 7 rows
// of 2 entries at 0x1cf5380, 0x40 bytes each, "in use" at +0x38. Starting a context (image 0xe8308c) takes the entry
// of the current thread's row and the context's kind, asserts that it is free (cmdBuf.m_bInUse == false) and marks
// it used; the context keeps the row (+0xe810) and finishing it frees that entry again. A job that fills a context
// can wait in the middle of it and go on later on another worker. On PS4 that hardly ever happens; on a PC it does,
// and in Syria co-op the worker then started a second context in the same buffer: the assertion fired, both contexts
// wrote over each other, a compute context was queued unfinished and job counters went wrong until the game froze
// (20 Nov 2026). Now the start claims the entry atomically; when its own row's entry is taken it claims the same kind
// of entry in another row and records that row in the context, so finishing it frees the right one. Only when all
// seven are taken does it go on as before. Found by its bytes.
#include "u3fix.h"
#include "u3fix_hook.h"

#include <atomic>
#include <climits>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace u3fix {

namespace {

unsigned char *g_table = nullptr;   // 7 rows of 2 entries of 0x40 bytes
constexpr unsigned kRows = 7;

bool claim(unsigned row, unsigned slot) {
    unsigned char *in_use = g_table + row * 0x80 + slot * 0x40 + 0x38;
    unsigned char expected = 0;
    return __atomic_compare_exchange_n(in_use, &expected, static_cast<unsigned char>(1), false, __ATOMIC_ACQ_REL,
                                       __ATOMIC_ACQUIRE);
}

// returns the row to use, with bit 32 set when its entry is now claimed
__attribute__((sysv_abi)) uint64_t choose_cmdbuf(uint64_t row, uint32_t slot) {
    if (row >= kRows || slot >= 2) return row;   // the game's own assertions handle these
    if (claim(static_cast<unsigned>(row), slot)) return row | (1ull << 32);
    for (unsigned other = 0; other < kRows; other++) {
        if (other == row || !claim(other, slot)) continue;
        static std::atomic<unsigned> borrowed{0};
        const unsigned n = ++borrowed;
        if (n <= 20 || n % 500 == 0) {
            std::printf("Uncharted 3: command buffer %u of thread row %llu still in use; the context takes row %u's "
                        "(time %u)\n",
                        slot, static_cast<unsigned long long>(row), other, n);
            std::fflush(stdout);
        }
        return other | (1ull << 32);
    }
    return row;   // all taken: as before (the game asserts and shares)
}

// mov r14d,r14d; shl r12,7; lea rbx,[table]; shl r14,6; add rbx,r12; cmp byte [r14+rbx+0x38],0; je +0x35
constexpr unsigned char kSite[] = {0x45, 0x89, 0xf6, 0x49, 0xc1, 0xe4, 0x07, 0x48, 0x8d, 0x1d, 0xe6, 0x22, 0xe7, 0x00, 0x49,
                                   0xc1, 0xe6, 0x06, 0x4c, 0x01, 0xe3, 0x41, 0x80, 0x7c, 0x1e, 0x38, 0x00, 0x74, 0x35};
constexpr unsigned kCheck = 21;                    // the cmp
constexpr unsigned kFree = sizeof(kSite) + 0x35;   // the je's target: the entry is marked used and filled in

}  // namespace

bool install_cmdbuf_borrow(unsigned char *image, uint64_t size) {
    const uint64_t at = find_unique(image, size, kSite, sizeof(kSite));
    if (at == kNotFound) return false;
    int32_t disp = 0;
    std::memcpy(&disp, image + at + 10, 4);
    g_table = image + at + 14 + disp;
    unsigned char *stub = stub_page_near(image, size);
    if (!stub) return false;
    unsigned n = 0;
    auto put = [&](std::initializer_list<unsigned char> b) { for (unsigned char c : b) stub[n++] = c; };
    auto imm64 = [&](uint64_t v) { std::memcpy(stub + n, &v, 8); n += 8; };
    put({0x50, 0x51, 0x52, 0x56, 0x57, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53});   // save rax..r11
    put({0x4c, 0x89, 0xe7});                         // mov rdi,r12 (the thread's row)
    put({0x44, 0x89, 0xf6});                         // mov esi,r14d (the kind of buffer)
    put({0x55, 0x48, 0x89, 0xe5, 0x48, 0x83, 0xe4, 0xf0, 0x48, 0xb8});   // push rbp; mov rbp,rsp; and rsp,-16; mov rax,
    imm64(reinterpret_cast<uint64_t>(&choose_cmdbuf));
    put({0xff, 0xd0, 0x48, 0x89, 0xec, 0x5d});       // call rax; mov rsp,rbp; pop rbp
    put({0x49, 0x89, 0xc4});                         // mov r12,rax (row | claimed << 32)
    put({0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5f, 0x5e, 0x5a, 0x59, 0x58});   // restore r11..rax
    put({0x4c, 0x89, 0xe0});                         // mov rax,r12
    put({0x45, 0x89, 0xe4});                         // mov r12d,r12d (the row)
    put({0x4d, 0x89, 0xa7, 0x10, 0xe8, 0x00, 0x00}); // mov [r15+0xe810],r12: finishing frees this row's entry
    put({0x48, 0xc1, 0xe8, 0x20});                   // shr rax,32 (claimed)
    put({0x45, 0x89, 0xf6, 0x49, 0xc1, 0xe6, 0x06}); // mov r14d,r14d; shl r14,6
    put({0x48, 0xbb});                               // mov rbx, table
    imm64(reinterpret_cast<uint64_t>(g_table));
    put({0x4c, 0x89, 0xe1, 0x48, 0xc1, 0xe1, 0x07, 0x48, 0x01, 0xcb});   // mov rcx,r12; shl rcx,7; add rbx,rcx
    put({0x49, 0xc1, 0xe4, 0x07});                   // shl r12,7 (as the game left it)
    put({0x85, 0xc0, 0x74, 0x0e});                   // test eax,eax; jz: not claimed
    put({0xff, 0x25, 0, 0, 0, 0});                   // claimed: on where the game marks and fills it
    imm64(reinterpret_cast<uint64_t>(image + at + kFree));
    put({0xff, 0x25, 0, 0, 0, 0});                   // not claimed: the game's own check (it asserts)
    imm64(reinterpret_cast<uint64_t>(image + at + kCheck));
    if (!finish_stub(stub) || !jump_to(image, at, kCheck, stub)) return false;
    std::printf("Uncharted 3: graphics contexts take a free command buffer when their thread's is in use (%#llx)\n",
                static_cast<unsigned long long>(at));
    std::fflush(stdout);
    return true;
}

}  // namespace u3fix
