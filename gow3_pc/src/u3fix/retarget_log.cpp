// Uncharted 3 1.02, 20 Nov 2026: which skeleton-to-skeleton animation mappings the game asks for, and which it lacks.
//
// An animation made for one skeleton plays on another through a retarget entry (dc1 skel-retargets). The function that
// finds the entry for a pair (image 0xdc4240: the source skeleton object, whose name id is at +0x40, and the target
// skeleton's name id) answers 0 when there is none; the animation then does not play and the character stands in its
// bind pose. The function now runs as before; each distinct pair is printed once with whether an entry was found
// (ids as "#%08x", names from the level data), so a character in a T-pose shows which mapping it is missing. Found by
// its bytes.
#include "u3fix.h"
#include "u3fix_hook.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace u3fix {

namespace {

std::atomic<uint64_t> seen[1024];   // (source << 32 | target) << 1 | found, plus 1 so 0 means empty
std::atomic<int> printed{0};
const unsigned char *g_image = nullptr;

// the state the lookup depends on: the two global switches (image 0x16e7930 +0xe5 on, +0xc0 off), the number of
// retarget entries (0x1c0a480) and the entry of this pair in the table (0x1c09d00, 0x28 bytes each: +8 / +0xc the
// skeleton ids, +0x18 the built mapping, +0x20 / +0x22 two flag words)
void describe(uint32_t src, uint32_t target, char *out, size_t size) {
    if (!g_image) { out[0] = 0; return; }
    const unsigned char *settings = g_image + 0x16e7930;
    uint32_t count = 0;
    std::memcpy(&count, g_image + 0x1c0a480, 4);
    int n = std::snprintf(out, size, "switches e5=%u c0=%u, %u entries", settings[0xe5], settings[0xc0], count);
    for (uint32_t i = 0; i < count && i < 64 && n > 0 && size_t(n) < size; i++) {
        const unsigned char *e = g_image + 0x1c09d00 + 0x28 * i;
        uint32_t a = 0, b = 0;
        uint64_t built = 0;
        uint16_t f20 = 0, f22 = 0;
        std::memcpy(&a, e + 8, 4);
        std::memcpy(&b, e + 0xc, 4);
        std::memcpy(&built, e + 0x18, 8);
        std::memcpy(&f20, e + 0x20, 2);
        std::memcpy(&f22, e + 0x22, 2);
        const unsigned char *list = nullptr;   // the list entry: +0 active, +4 single player, +8 multiplayer
        std::memcpy(&list, e, sizeof list);
        uint32_t sw[3] = {9, 9, 9};
        if (list) std::memcpy(sw, list, sizeof sw);
        if (a == src || b == src || a == target || b == target)   // this pair, and any entry naming either skeleton
            n += std::snprintf(out + n, size - n,
                               "; entry %u: #%08x -> #%08x built %#llx flags %#x %#x switches %u/%u/%u", i, a, b,
                               static_cast<unsigned long long>(built), f20, f22, sw[0], sw[1], sw[2]);
    }
}

__attribute__((sysv_abi)) void note(const unsigned char *source, uint32_t target, uint64_t result) {
    if (!source) return;
    uint32_t src = 0;
    std::memcpy(&src, source + 0x40, 4);
    const uint64_t key = (((uint64_t{src} << 32) | target) << 1 | (result != 0)) + 1;
    uint64_t slot = (key * 0x9e3779b97f4a7c15ull) >> 54;
    for (int probe = 0; probe < 1024; probe++, slot = (slot + 1) & 1023) {
        uint64_t cur = seen[slot].load(std::memory_order_relaxed);
        if (cur == key) return;
        if (cur == 0) {
            if (!seen[slot].compare_exchange_strong(cur, key)) {
                if (cur == key) return;
                continue;
            }
            if (printed.fetch_add(1) >= 400) return;
            char state[512];
            describe(src, target, state, sizeof state);
            std::printf("Uncharted 3: skeleton retarget #%08x -> #%08x %s (%s)\n", src, target,
                        result ? "found" : "MISSING", state);
            std::fflush(stdout);
            return;
        }
    }
}

// push rbp; mov rbp,rsp; push r15; push r14; push r13; push r12; push rbx; sub rsp,0x28; mov rbx,rcx; mov r15,rdx;
// mov r13d,esi
constexpr unsigned char kSite[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53,
                                   0x48, 0x83, 0xec, 0x28, 0x48, 0x89, 0xcb, 0x49, 0x89, 0xd7, 0x41, 0x89, 0xf5};
constexpr unsigned kPrologue = 6;   // push rbp; mov rbp,rsp; push r15

}  // namespace

bool install_retarget_log(unsigned char *image, uint64_t size) {
    const uint64_t at = find_unique(image, size, kSite, sizeof(kSite));
    if (at == kNotFound) return false;
    if (size > 0x1c0a484) g_image = image;
    unsigned char *stub = stub_page_near(image, size);
    if (!stub) return false;
    unsigned n = 0;
    auto put = [&](std::initializer_list<unsigned char> b) { for (unsigned char ch : b) stub[n++] = ch; };
    // the entry: keep the source object and target id, run the original function (prologue here, then on in the
    // image; xmm0 and the other arguments untouched), then note its answer and return it
    put({0x57, 0x56, 0x48, 0x83, 0xec, 0x08});   // push rdi; push rsi; sub rsp,8 (aligned for the call)
    put({0xe8, 0, 0, 0, 0});                     // call trampoline (rel32 filled in below)
    const unsigned call_end = n;
    put({0x48, 0x83, 0xc4, 0x08, 0x5e, 0x5f});   // add rsp,8; pop rsi; pop rdi
    put({0x50, 0x48, 0x89, 0xc2});               // push rax (the answer; also aligns); mov rdx,rax; rdi, esi kept
    put({0x48, 0xb8});                           // mov rax, note
    const uint64_t fn = reinterpret_cast<uint64_t>(&note);
    std::memcpy(stub + n, &fn, 8);
    n += 8;
    put({0xff, 0xd0, 0x58, 0xc3});               // call rax; pop rax; ret
    const unsigned trampoline = n;
    std::memcpy(stub + n, image + at, kPrologue);   // the original prologue
    n += kPrologue;
    put({0xff, 0x25, 0, 0, 0, 0});                  // jmp [rip]: on in the image
    const uint64_t back = reinterpret_cast<uint64_t>(image + at + kPrologue);
    std::memcpy(stub + n, &back, 8);
    n += 8;
    const int32_t rel = static_cast<int32_t>(trampoline - call_end);
    std::memcpy(stub + call_end - 4, &rel, 4);
    if (!finish_stub(stub) || !jump_to(image, at, kPrologue, stub)) return false;
    std::printf("Uncharted 3: skeleton retarget lookups are logged (%#llx)\n", static_cast<unsigned long long>(at));
    std::fflush(stdout);
    return true;
}

}  // namespace u3fix
