// u2mp menu draw: the per-frame 2D command arena the PS4 menus draw into is too small for the
// multiplayer pages. Notes: u2mp/docs/PORT-MENUDRAW.md.
//
// Every menu primitive is a deferred render command allocated from one linear arena, the global
// 0x124a000 (allocator object at +0x10: +0x10 top, +0x18 end, +0x20 base, +0x28 size << 2 | flags,
// +0x30 high-water mark, +0x40 spin lock, +0x70 command list):
//   * textured quad 0xbdce90 with a NULL draw list: material 0x3039 (0x50 bytes, 0xbdcf48),
//     vertices 0x303a (0x140, 0xbdd2c1), end 0x303c (0x48, 0xbdd518): 0x1d0 bytes per quad,
//   * text 0xbda9c0: 0xe0 + strlen bytes (the renderer's text at 0x5be615),
//   * the renderer 0x5bcd70 itself (0x5c1b25, 0x5c2941) and the item tail 0x5b4e90 (0x5b501d,
//     0x5b50d8), the frames / rows 0x5b3eb0..0x5b5de0, plus HUD and other 2D users.
// HeapArena::Allocate (0xb32170) returns NULL when the arena is full; with flag bit 0 set (Draw
// sets it, 0x3fc434) it does not even print "HeapArena::Allocate: out of memory". Callers skip the
// primitive on NULL. Draw (0x3fc0b0, Bluepoint's bp_ucctweaks.cpp, called each frame from
// 0x3fb596) gives the arena 0x30000 bytes (192 KiB) of memory context 0x33 every frame
// (0x3fc38c mov edi,0x30000; 0x3fc391 call 0xb33160; 0x3fc3f3 or rax,0xc0000; 0x3fc403 add
// r14,0x30000). That fits the single-player menus; the Profile page over the page behind it does
// not, and the arena is shared by worker threads (spin lock), so which primitives are lost changes
// frame to frame. PS3 1.10 has no such arena (its menus draw through GCM with the PS3 frame
// allocator, renderer 0x41a790): the limit is the port's.
//
// Fix: the arena gets kArenaSize bytes from a ring of host buffers (one per frame, kRing deep so a
// buffer is not reused while the render side may still read it). The size constants of Draw are
// patched to match, and the call 0x3fc391 goes to arena_memory below. A diagnostic wrapper on the
// call at 0x3fae9c (just before the frame reset of the arena, 0x3faea1) logs the previous frame's
// use and the peak.
//
// Second limit (run 32): with the commands surviving, the quads overflow the per-frame dynamic
// vertex / index streams of the 2D context (256 KiB vertex = 1,024 quads, 8 KiB index = 682
// quads) and the writer 0xbdc190 asserts at 0xbdc5c2 / 0xbdc65a. install_streams raises the stream
// sizes at their setup 0xc79d40 (guest frame-heap memory, as before) and guards 0xbdc190 so that a
// quad that still does not fit is dropped instead of written past the end.
//
// Also (install_dropdown_rows): hidden lobby option rows take no space and the Profile option rows span their
// column, as PS3 (control kind 13, below).
//
// Also (install_carousel_hint): the picture carousels (control kind 8) draw no "<>" beside a focused, closed
// row while a multiplayer session is up, as PS3 (u2mp/docs/MENUS.md 3.2).
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iterator>

#include "u2mp_internal.h"

namespace u2mp {
namespace {

constexpr uint64_t kArena = 0x124a000;          // the 2D command arena (lea at 0x3fc39e)
constexpr uint64_t fFrameAlloc = 0xb33160;      // Allocate(size, &memory context)
constexpr uint64_t fFrameBegin = 0x4b4f30;      // called at 0x3fae9c, right before the reset
constexpr uint64_t kMovSize = 0x3fc38c;         // mov edi,0x30000
constexpr uint64_t kCallAlloc = 0x3fc391;       // call 0xb33160
constexpr uint64_t kOrFlags = 0x3fc3f3;         // or rax,0xc0000   (size << 2)
constexpr uint64_t kAddEnd = 0x3fc403;          // add r14,0x30000
constexpr uint64_t kCallFrameBegin = 0x3fae9c;  // call 0x4b4f30
constexpr uint32_t kOldSize = 0x30000;
constexpr uint32_t kArenaSize = 0x200000;       // 2 MiB, about 4,500 quads
constexpr unsigned kRing = 4;
static_assert(kArenaSize % 0x40 == 0 && (uint64_t(kArenaSize) << 2) < 0x80000000u, "imm32 of or rax");

alignas(64) unsigned char ring[kRing][kArenaSize];
std::atomic<unsigned> ring_next{0};

template <typename T> T &G(uint64_t offset) { return *image.at<T>(offset); }
template <typename F> F guest(uint64_t offset) { return reinterpret_cast<F>(image.base + offset); }

// Replaces the call at 0x3fc391: the arena's memory for this frame.
U2MP_GUEST void *arena_memory(uint64_t size, void *context) {
    if (uint32_t(size) == kArenaSize) {
        const unsigned i = ring_next.fetch_add(1, std::memory_order_relaxed) % kRing;
        return ring[i];
    }
    return guest<void *(U2MP_GUEST *)(uint64_t, void *)>(fFrameAlloc)(size, context);
}

uint64_t peak = 0;
unsigned frames = 0;

// Wraps the call at 0x3fae9c: the arena still holds the previous frame's commands here.
U2MP_GUEST uint64_t frame_begin(void *arg) {
    const uint64_t top = G<uint64_t>(kArena + 0x10), base = G<uint64_t>(kArena + 0x20);
    const uint64_t high = G<uint64_t>(kArena + 0x30);
    const uint64_t used = (base && top >= base) ? top - base : 0;
    const uint64_t most = used > high ? used : high;
    ++frames;
    if (most > peak) {
        // Log each new peak once it grew by 1/16 of the arena (or crossed the old 192 KiB limit).
        const bool crossed = peak < kOldSize && most >= kOldSize;
        if (crossed || most >= peak + kArenaSize / 16 || peak == 0)
            log("menu draw: 2D arena peak %llu of %u bytes (old limit %u) at frame %u%s",
                (unsigned long long)most, kArenaSize, kOldSize, frames,
                most >= kOldSize ? " - the PS4 arena would have dropped primitives" : "");
        peak = most;
    }
    return guest<uint64_t(U2MP_GUEST *)(void *)>(fFrameBegin)(arg);
}

// ---- the dynamic vertex / index streams the quads are written to --------------------------------
// Render-side execution of the 0x303a command, 0xbdc190(ctx, vertices, alpha in xmm0), writes 4
// vertices of 0x40 bytes at the 64-aligned current pointer of the vertex stream [ctx+0x10] and 6
// u16 indices (vertex number = offset / 0x40) to the index stream [ctx+0x18]. Only afterwards does
// it assert (dynamic-stream-mgr.h SetPointer, line 0x9a: 0xbdc5c2 / 0xbdc65a) that the stream did
// not pass its size. Stream object (0x28 bytes): +0 buffer object, +8 per-frame view, +0x10 start,
// +0x18 current, +0x20 u32 size. The flush 0xbdc670 (0x303c) draws (index count - batch start)
// indices and skips the draw when that is 0, so a dropped quad is safe.
constexpr uint64_t fQuadWrite = 0xbdc190;
constexpr uint64_t kQuadCall = 0xbdad3c;   // call 0xbdc190 (0x303a in the command list executor 0xbdaca0)
constexpr uint64_t kQuadJmp = 0xbdd16c;    // jmp 0xbdc190 (immediate path of 0xbdd130)
constexpr uint32_t kVertexOld = 0x40000, kVertexNew = 0x180000;   // 1,024 -> 6,144 quads
constexpr uint32_t kIndexOld = 0x2000, kIndexNew = 0x12000;       // 682 -> 6,144 quads
static_assert(kVertexNew / 0x40 <= 0x10000, "u16 indices address the whole vertex stream");

std::atomic<uint64_t> quads_dropped{0};

uint64_t rd64(const void *p, ptrdiff_t off) {
    uint64_t v;
    std::memcpy(&v, static_cast<const uint8_t *>(p) + off, sizeof v);
    return v;
}
uint32_t rd32(const void *p, ptrdiff_t off) {
    uint32_t v;
    std::memcpy(&v, static_cast<const uint8_t *>(p) + off, sizeof v);
    return v;
}

// True when `bytes` more fit in stream `s` at its current pointer aligned to `align` (0xbdc203).
bool stream_room(const uint8_t *s, uint64_t align, uint64_t bytes, uint64_t *offset) {
    const uint64_t start = rd64(s, 0x10), cur = rd64(s, 0x18), size = rd32(s, 0x20);
    const uint64_t at = (cur - start + align - 1) & ~(align - 1);
    if (offset) *offset = at;
    return at + bytes < size;
}

U2MP_GUEST void quad_write(uint8_t *ctx, void *vertices, float alpha) {
    const uint8_t *vs = ctx ? reinterpret_cast<const uint8_t *>(rd64(ctx, 0x10)) : nullptr;
    const uint8_t *is = ctx ? reinterpret_cast<const uint8_t *>(rd64(ctx, 0x18)) : nullptr;
    // Streams missing or unmapped: the original asserts as before.
    if (vs && is && rd64(vs, 0x10) && rd64(vs, 0x18) && rd64(is, 0x10) && rd64(is, 0x18)) {
        uint64_t voff = 0;
        const bool ok = stream_room(vs, 0x40, 0x100, &voff) && stream_room(is, 1, 0xc, nullptr) &&
                        voff / 0x40 + 3 <= 0xffff;   // u16 indices
        if (!ok) {
            const uint64_t n = quads_dropped.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 4 || (n & (n - 1)) == 0)
                log("menu draw: vertex/index stream full, quad dropped (%llu so far; streams %u / %u bytes)",
                    (unsigned long long)n, rd32(vs, 0x20), rd32(is, 0x20));
            return;
        }
    }
    guest<void(U2MP_GUEST *)(uint8_t *, void *, float)>(fQuadWrite)(ctx, vertices, alpha);
}

// Stream sizes. 0xc79d40 (render init, from 0xc8edb0) sets up 4 frame slots of 0x370 bytes at
// 0x13d2ee0; each frame 0xc7a3e0 (from 0xb6315b, frame params +0x98) takes the memory of every
// stream from the frame heap (0xc12150 -> 0xb333f0, context 0x46 / 0x35) with the size at +0x20,
// so the streams stay guest, GPU-visible memory. Each stream also has a buffer object created
// from {u32 bytes, u32 kind} (0xc16820) whose byte count must agree with +0x20. The 2D context
// (0xccf830 type 0, set up at 0x4bab4c for the frame's 2D pass that runs the command list at
// 0x4bad05) uses stream +0x00 (vertex) and +0xf0 (index); types 3 / 4 use +0x28 / +0x118. The
// immediates are shared between neighbouring streams, so those grow too (all small).
struct StreamImm { uint64_t at; unsigned char op[2]; uint64_t old_value, new_value; };
constexpr uint64_t kVSize = 0x800000000ull, kVBuf = 0x100000000ull, kIBuf = 0x200000000ull;
constexpr StreamImm kStreamImms[] = {
    {0xc79d70, {0x48, 0xb8}, kVSize | kVertexOld, kVSize | kVertexNew},  // +0x20, +0x48 sizes
    {0xc79d8b, {0x48, 0xb8}, kVBuf | kVertexOld, kVBuf | kVertexNew},    // +0x00, +0x28 buffers
    {0xc79ed3, {0x48, 0xb8}, kVSize | kIndexOld, kVSize | kIndexNew},    // +0xc0 (vertex +0xa0), +0x110 sizes
    {0xc79ee8, {0x48, 0xb8}, kVBuf | kIndexOld, kVBuf | kIndexNew},      // +0xa0 buffer
    {0xc79f33, {0x49, 0xbc}, kIBuf | kIndexOld, kIBuf | kIndexNew},      // +0xf0, +0x118 buffers (r12)
    {0xc79f68, {0x48, 0xb8}, kVSize | kIndexOld, kVSize | kIndexNew},    // +0x138 size
};

void install_streams(int &installed) {
    bool all = true;
    for (const StreamImm &s : kStreamImms) {
        unsigned char want[10];
        std::memcpy(want, s.op, 2);
        std::memcpy(want + 2, &s.old_value, 8);
        if (!image.matches(s.at, want, sizeof want)) all = false;
    }
    if (all) {
        for (const StreamImm &s : kStreamImms) std::memcpy(image.base + s.at + 2, &s.new_value, 8);
        log("menu draw: dynamic streams per frame: vertex 0x%x -> 0x%x, index 0x%x -> 0x%x bytes", kVertexOld,
            kVertexNew, kIndexOld, kIndexNew);
        ++installed;
    } else {
        log("menu draw: stream setup 0xc79d40 differs, stream sizes left alone");
    }

    // The overflow guard, also when the sizes could not be raised.
    static const unsigned char quad_head[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                              0x41, 0x55, 0x41, 0x54, 0x53, 0x50};
    if (!image.matches(fQuadWrite, quad_head, sizeof quad_head)) {
        log("menu draw: quad writer 0xbdc190 differs, no stream overflow guard");
        return;
    }
    if (retarget_call(kQuadCall, fQuadWrite, reinterpret_cast<void *>(&quad_write), "menu draw: quad stream guard"))
        ++installed;
    int32_t rel = 0;
    std::memcpy(&rel, image.base + kQuadJmp + 1, 4);
    unsigned char *stub = nullptr;
    if (image.base[kQuadJmp] == 0xe9 && kQuadJmp + 5 + int64_t(rel) == int64_t(fQuadWrite) && (stub = near_stub(16))) {
        // mov rax,quad_write; jmp rax   (a tail jump: rdi, rsi and xmm0 pass through)
        const uint64_t target = uint64_t(uintptr_t(&quad_write));
        stub[0] = 0x48;
        stub[1] = 0xb8;
        std::memcpy(stub + 2, &target, 8);
        stub[10] = 0xff;
        stub[11] = 0xe0;
        const int64_t delta = int64_t(uintptr_t(stub)) - int64_t(uintptr_t(image.base + kQuadJmp + 5));
        if (delta >= INT32_MIN && delta <= INT32_MAX) {
            const int32_t r = int32_t(delta);
            std::memcpy(image.base + kQuadJmp + 1, &r, 4);
            log("menu draw: quad stream guard: jmp at 0x%llx -> C++", (unsigned long long)kQuadJmp);
        }
    } else {
        log("menu draw: jmp at 0x%llx differs, immediate quad path left unguarded", (unsigned long long)kQuadJmp);
    }
}

// ---- picture carousels (control kind 8): no "<>" beside a closed, focused row in multiplayer ----------
// Draw 0x588ed0 of the kind-8 control (Game Type, Map, Playlist, Boosters, Hero / Villain) draws two arrow
// images (type 6) beside the value whenever the row has the focus (+0x8) and its box is closed (open
// fraction 0): the block 0x58a0d3..0x58a68d, entered past `cmpb [r14+8],0` / `je 0x58a68d` at 0x58a0ab /
// 0x58a0bf, at the value rect from 0x57f060 + (11 / 31, -100). PS3 shows no arrows there: they belong to
// the open box only (drawn by the same function before 0x589d35). The je goes to a stub that also skips the
// block while a multiplayer session is up (NET_SM, s32 0x14ce97c, > 1), so the campaign's chapter carousel
// (PAGE_CHAPTERS, also kind 8) keeps it. Flags are not live at 0x58a68d (rewritten by `test` / `vucomiss`).
constexpr uint64_t kHintJe = 0x58a0bf;     // je 0x58a68d
constexpr uint64_t kHintNext = 0x58a0c5;   // the hint block (vxorps; vucomiss: drawn only when closed)
constexpr uint64_t kHintSkip = 0x58a68d;
constexpr uint64_t kNetSm = 0x14ce97c;

// rel32 of a jump / rip-relative operand ending at `from_end` to `to`; false when out of range.
bool rel32(const unsigned char *from_end, const unsigned char *to, int32_t *out) {
    const int64_t d = int64_t(uintptr_t(to)) - int64_t(uintptr_t(from_end));
    if (d < INT32_MIN || d > INT32_MAX) return false;
    *out = int32_t(d);
    return true;
}

void install_carousel_hint(int &installed) {
    // cmpb [r14+8],0; mov r15d,[rbp-0x7c8]; vmovss xmm1,[rbp-0x7bc]; je 0x58a68d; vxorps xmm0,xmm0,xmm0
    static const unsigned char want[] = {0x41, 0x80, 0x7e, 0x08, 0x00, 0x44, 0x8b, 0xbd, 0x38, 0xf8, 0xff, 0xff,
                                         0xc5, 0xfa, 0x10, 0x8d, 0x44, 0xf8, 0xff, 0xff, 0x0f, 0x84, 0xc8, 0x05,
                                         0x00, 0x00, 0xc5, 0xf8, 0x57, 0xc0};
    static_assert(kHintJe + 6 == kHintNext && kHintNext + 0x5c8 == kHintSkip, "je 0x58a68d at 0x58a0bf");
    unsigned char *stub = nullptr;
    if (!image.matches(0x58a0ab, want, sizeof want) || !(stub = near_stub(24))) {
        log("menu draw: carousel draw 0x588ed0 differs, focus arrows left as on PS4");
        return;
    }
    // je skip; cmp dword [rip+NET_SM],1; jg skip; jmp next
    unsigned char code[24] = {0x0f, 0x84, 0, 0, 0, 0, 0x83, 0x3d, 0, 0, 0, 0, 0x01,
                              0x0f, 0x8f, 0, 0, 0, 0, 0xe9, 0, 0, 0, 0};
    int32_t r[4] = {};
    unsigned char *const b = image.base;
    if (!rel32(stub + 6, b + kHintSkip, &r[0]) || !rel32(stub + 13, b + kNetSm, &r[1]) ||
        !rel32(stub + 19, b + kHintSkip, &r[2]) || !rel32(stub + 24, b + kHintNext, &r[3])) {
        log("menu draw: no stub in range for the carousel focus arrows");
        return;
    }
    std::memcpy(code + 2, &r[0], 4);
    std::memcpy(code + 8, &r[1], 4);
    std::memcpy(code + 15, &r[2], 4);
    std::memcpy(code + 20, &r[3], 4);
    std::memcpy(stub, code, sizeof code);
    int32_t to_stub = 0;
    if (!rel32(b + kHintJe + 5, stub, &to_stub)) return;
    unsigned char *site = b + kHintJe;
    site[0] = 0xe9;
    std::memcpy(site + 1, &to_stub, 4);
    site[5] = 0x90;
    log("menu draw: picture carousels draw no focus arrows in multiplayer (0x%llx -> stub)", (unsigned long long)kHintJe);
    ++installed;
}

// ---- drop-down rows (control kind 13) as PS3 lays them out ---------------------------------------------------------
// The page lays out each column from the rows' sizes (virtual +0x28 of the control: {width, height}): every row gets
// the column's x and width (the widest row) and the height its size gives, and a row of height 0 takes no space
// (0x5a2110). Two PS4 differences from PS3 for kind 13 (class 0x586b80, MENUS.md 3.2), fixed for the multiplayer rows
// named by their query condition (record +0x34):
//  - Size 0x587df0: a row with no label (hidden by its condition) still measures one empty text line, where the
//    list row it replaces (kind 6, size 0x57f370) measures 0 x 0. The overlay made the lobby's Variation, Score
//    Limit, Rounds, Time Limit and Co-op Difficulty rows kind 13 (they were kind 6 on PS3 and PS4), so each hidden one
//    left a blank line: the gap between Score Limit and Time Limit, and a taller panel (higher on the screen) for
//    Find Match, where all five are hidden. For these rows the size is 0 x 0 again when there is no label.
//  - Draw 0x587300: the row is drawn at its menu record's width (+0x5c) instead of the width the layout gave it. On
//    PS3 the Profile page's Co-op Difficulty, Taunt, Matchmaking and Golden Guns rows span their column (as wide as
//    the Hero / Villain rows), so these rows keep the laid-out width.
constexpr uint64_t kSizeLabelTest = 0x587e27;   // test rax,rax; je 0x587e74; cmp byte [rax],0; je 0x587e74
constexpr uint64_t kSizeLabelled = 0x587e31;    // a label: measure the row
constexpr uint64_t kSizeEmpty = 0x587e74;       // no label: one empty text line
constexpr uint64_t kSizeReturn = 0x588195;      // stack check and return of the size (rax = the result r15)
constexpr uint64_t kDrawWidth = 0x587359;       // mov rax,[r14+0x80]: the record, for its width
constexpr uint64_t kDrawRecordWidth = 0x587360; // test rax,rax: the record's width replaces the laid-out one
constexpr uint64_t kDrawWidthDone = 0x58738e;   // rect at [rbp-0x1c0], width in xmm0
constexpr uint32_t kHiddenRowConds[] = {59, 60, 61, 63, 65};   // Variation, Score Limit, Time Limit, Rounds, Co-op Diff.
constexpr uint32_t kColumnRowConds[] = {80, 82, 108, 135};     // Co-op Difficulty, Taunt, Golden Guns, Matchmaking

// A little x86-64 writer for the stubs below: bytes, short conditional jumps to labels, rel32 jumps into the image.
struct Code {
    unsigned char *at;
    size_t n = 0;
    bool ok = true;
    void bytes(std::initializer_list<unsigned char> b) {
        for (unsigned char c : b) at[n++] = c;
    }
    void u32(uint32_t v) {
        std::memcpy(at + n, &v, 4);
        n += 4;
    }
    size_t jcc8(unsigned char op) {   // returns the place of the rel8, filled by land()
        bytes({op, 0});
        return n - 1;
    }
    void land(size_t rel8_at) {
        const ptrdiff_t d = ptrdiff_t(n) - ptrdiff_t(rel8_at + 1);
        if (d < 0 || d > 127) ok = false;
        at[rel8_at] = static_cast<unsigned char>(d);
    }
    void jmp(uint64_t image_offset) {
        int32_t r = 0;
        bytes({0xe9});
        ok = ok && rel32(at + n + 4, image.base + image_offset, &r);
        u32(uint32_t(r));
    }
};

// cmp ecx,imm32 / je <label> for each condition; returns the rel8 places
template <size_t N> void jump_if_cond(Code &c, const uint32_t (&conds)[N], size_t (&to)[N]) {
    for (size_t i = 0; i < N; ++i) {
        c.bytes({0x81, 0xf9});
        c.u32(conds[i]);
        to[i] = c.jcc8(0x74);
    }
}

void install_dropdown_rows(int &installed) {
    // mov rdi,[r12+0x18]; mov rax,[rdi]; call [rax+0x28] (the label); test rax,rax; je +0x48; cmp byte [rax],0; je +0x43
    static const unsigned char size_want[] = {0x49, 0x8b, 0x7c, 0x24, 0x18, 0x48, 0x8b, 0x07, 0xff, 0x50, 0x28,
                                              0x48, 0x85, 0xc0, 0x74, 0x48, 0x80, 0x38, 0x00, 0x74, 0x43};
    // vmovss xmm0,[rbp-0x104]; vmovss [r15+4],xmm0; mov rax,[r13]; cmp rax,[rbp-0x30]
    static const unsigned char return_want[] = {0xc5, 0xfa, 0x10, 0x85, 0xfc, 0xfe, 0xff, 0xff, 0xc4, 0xc1, 0x7a,
                                                0x11, 0x47, 0x04, 0x49, 0x8b, 0x45, 0x00, 0x48, 0x3b, 0x45, 0xd0};
    // vmovups xmm1,[r14+0x90]; mov rax,[r14+0x80]; test rax,rax; je +0x1b; mov eax,[rax+0x5c]
    static const unsigned char draw_want[] = {0xc4, 0xc1, 0x78, 0x10, 0x8e, 0x90, 0x00, 0x00, 0x00, 0x49, 0x8b,
                                              0x86, 0x80, 0x00, 0x00, 0x00, 0x48, 0x85, 0xc0, 0x74, 0x1b, 0x8b,
                                              0x40, 0x5c};
    // vmovaps [rbp-0x1c0],xmm1; vmovss xmm0,[r14+0xc]; mov eax,[r14+0xb0]
    static const unsigned char done_want[] = {0xc5, 0xf8, 0x29, 0x8d, 0x40, 0xfe, 0xff, 0xff, 0xc4, 0xc1, 0x7a,
                                              0x10, 0x46, 0x0c, 0x41, 0x8b, 0x86, 0xb0, 0x00, 0x00, 0x00};
    static_assert(kSizeLabelTest == 0x587e1c + 11 && kDrawWidth == 0x587350 + 9, "patch sites inside the patterns");
    if (!image.matches(0x587e1c, size_want, sizeof size_want) ||
        !image.matches(kSizeReturn - 14, return_want, sizeof return_want) ||
        !image.matches(0x587350, draw_want, sizeof draw_want) ||
        !image.matches(kDrawWidthDone - 14, done_want, sizeof done_want)) {
        log("menu draw: drop-down size 0x587df0 / draw 0x587300 differ, hidden rows and row widths left as on PS4");
        return;
    }
    unsigned char *size_stub = near_stub(0x80), *draw_stub = near_stub(0x60);
    if (!size_stub || !draw_stub) {
        log("menu draw: no stub memory for the drop-down rows");
        return;
    }

    // Size: a label goes on as before; no label: 0 x 0 for the hidden-row conditions, else the empty line.
    Code s{size_stub};
    s.bytes({0x48, 0x85, 0xc0});                           // test rax,rax
    const size_t no_label = s.jcc8(0x74);                  // je empty
    s.bytes({0x80, 0x38, 0x00});                           // cmp byte [rax],0
    const size_t empty_label = s.jcc8(0x74);               // je empty
    s.jmp(kSizeLabelled);
    s.land(no_label);
    s.land(empty_label);
    s.bytes({0x49, 0x8b, 0x84, 0x24, 0x80, 0x00, 0x00, 0x00});   // mov rax,[r12+0x80]   the menu record
    s.bytes({0x48, 0x85, 0xc0});                                 // test rax,rax
    const size_t no_record = s.jcc8(0x74);                       // je as_shipped
    s.bytes({0x8b, 0x48, 0x34});                                 // mov ecx,[rax+0x34]   its condition
    size_t hidden[std::size(kHiddenRowConds)];
    jump_if_cond(s, kHiddenRowConds, hidden);
    s.land(no_record);
    s.jmp(kSizeEmpty);                                           // as_shipped
    for (size_t h : hidden) s.land(h);
    s.bytes({0x49, 0xc7, 0x07, 0x00, 0x00, 0x00, 0x00});         // mov qword [r15],0    width and height 0
    s.jmp(kSizeReturn);

    // Draw: for the column rows keep the laid-out rect and use its width; else the record's width as before.
    Code d{draw_stub};
    d.bytes({0x49, 0x8b, 0x86, 0x80, 0x00, 0x00, 0x00});         // mov rax,[r14+0x80]   (the instruction replaced)
    d.bytes({0x48, 0x85, 0xc0});                                 // test rax,rax
    const size_t none = d.jcc8(0x74);                            // je as_shipped
    d.bytes({0x8b, 0x48, 0x34});                                 // mov ecx,[rax+0x34]
    size_t column[std::size(kColumnRowConds)];
    jump_if_cond(d, kColumnRowConds, column);
    d.land(none);
    d.jmp(kDrawRecordWidth);                                     // as_shipped (rax = the record)
    for (size_t c : column) d.land(c);
    d.bytes({0xc5, 0xf8, 0x29, 0x8d, 0x40, 0xfe, 0xff, 0xff});   // vmovaps [rbp-0x1c0],xmm1   the laid-out rect
    d.bytes({0xc5, 0xf0, 0x12, 0xc1});                           // vmovhlps xmm0,xmm1,xmm1    xmm0 = its width
    d.jmp(kDrawWidthDone);

    int32_t to_size = 0, to_draw = 0;
    if (!s.ok || !d.ok || !rel32(image.base + kSizeLabelTest + 5, size_stub, &to_size) ||
        !rel32(image.base + kDrawWidth + 5, draw_stub, &to_draw)) {
        log("menu draw: no stub in range for the drop-down rows");
        return;
    }
    unsigned char *site = image.base + kSizeLabelTest;   // 10 bytes: jmp stub, 5 nops
    site[0] = 0xe9;
    std::memcpy(site + 1, &to_size, 4);
    std::memset(site + 5, 0x90, 5);
    site = image.base + kDrawWidth;   // 7 bytes: jmp stub, 2 nops
    site[0] = 0xe9;
    std::memcpy(site + 1, &to_draw, 4);
    std::memset(site + 5, 0x90, 2);
    log("menu draw: drop-down rows: hidden lobby option rows take no space, Profile option rows span their column");
    ++installed;
}

}  // namespace

void install_menudraw(int &installed) {
    install_streams(installed);   // first: more surviving 2D commands need room in the streams
    install_carousel_hint(installed);
    install_dropdown_rows(installed);

    static const unsigned char mov_size[] = {0xbf, 0x00, 0x00, 0x03, 0x00, 0xe8};
    static const unsigned char or_flags[] = {0x48, 0x0d, 0x00, 0x00, 0x0c, 0x00};
    static const unsigned char add_end[] = {0x49, 0x81, 0xc6, 0x00, 0x00, 0x03, 0x00};
    // lea r12,[rip+..] = 0x124a000 right after the call (0x3fc39e), mov [r12+0x20],r14 (0x3fc3e5)
    static const unsigned char lea_arena[] = {0x4c, 0x8d, 0x25};
    static const unsigned char set_base[] = {0x4d, 0x89, 0x74, 0x24, 0x20};
    int32_t rel = 0;
    std::memcpy(&rel, image.base + 0x3fc3a1, 4);
    if (!image.matches(kMovSize, mov_size, sizeof mov_size) || !image.matches(kOrFlags, or_flags, sizeof or_flags) ||
        !image.matches(kAddEnd, add_end, sizeof add_end) || !image.matches(0x3fc39e, lea_arena, sizeof lea_arena) ||
        0x3fc3a5 + int64_t(rel) != int64_t(kArena) || !image.matches(0x3fc3e5, set_base, sizeof set_base)) {
        log("menu draw: Draw 0x3fc0b0 differs, 2D arena left at 0x%x bytes", kOldSize);
        return;
    }
    if (!retarget_call(kCallAlloc, fFrameAlloc, reinterpret_cast<void *>(&arena_memory), "menu draw: 2D arena memory"))
        return;
    std::memcpy(image.base + kMovSize + 1, &kArenaSize, 4);
    const uint32_t flags = kArenaSize << 2;
    std::memcpy(image.base + kOrFlags + 2, &flags, 4);
    std::memcpy(image.base + kAddEnd + 3, &kArenaSize, 4);
    log("menu draw: 2D command arena 0x124a000 0x%x -> 0x%x bytes (%u host buffers)", kOldSize, kArenaSize, kRing);
    ++installed;

    // Diagnostics only: the previous frame's use, read just before the reset at 0x3faea1.
    static const unsigned char frame_call[] = {0x48, 0x8d, 0x3d};   // lea rdi,[rip+..] at 0x3fae95
    if (image.matches(0x3fae95, frame_call, sizeof frame_call) &&
        retarget_call(kCallFrameBegin, fFrameBegin, reinterpret_cast<void *>(&frame_begin), "menu draw: arena use log"))
        ++installed;
}

}  // namespace u2mp
