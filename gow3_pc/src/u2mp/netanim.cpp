// u2mp netanim: remote characters' animation in multiplayer.
//
// Reimplemented from the behaviour description u2mp/docs/spec/netanim.md.
//
// PS4 1.02 lost two pieces of the network animation path:
//  1. The snapshot stream no longer carries a clip's phase, so the visitor that builds a remote
//     character's clip command is never reached. Two call-outs put the phase back: one after the
//     writer stores a clip's anim index (appends the phase bits), one in the reader's anim case
//     (reads them and hands them to visitor slot 3). This changes the wire format, so both PCs
//     need this build.
//  2. NetAnimCmdGenerator slot 4 is an empty function. It should fire the frame-keyed events
//     (sounds, effects) of what a remote character played between two snapshots. The host
//     version keeps a small animation list per generator and calls the game's event firing
//     function, which PS4 still has but never calls.
// UNCHARTED_U2MP_NETANIM=0 turns all of it off.

// Every float result must be rounded on its own, exactly as the description orders the steps.
#pragma clang fp contract(off)

#include "u2mp_internal.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace u2mp {
namespace {

// ---- image offsets (Uncharted 2 PS4 1.02) -------------------------------------------------------
constexpr uint64_t kWriterSite = 0x6364b5, kWriterResume = 0x638000;
constexpr uint64_t kReaderSite = 0x66c035, kReaderResume = 0x66c250;
constexpr uint64_t kIndexSite = 0x66b637, kIndexResume = 0x66b6a5;
constexpr uint64_t kPrefillSite = 0x5f3d63, kPrefillResume = 0x5f3d6a;
constexpr uint64_t kDepthSite = 0x5f3dd1, kDepthResume = 0x5f3dd8;
constexpr uint64_t kPoseGenerator = 0x100ac80;
constexpr uint64_t kPadPlayerSite = 0x60e296, kPadPlayerResume = 0x60e29e;
constexpr uint64_t kPadOtherSite = 0x60e9f3, kPadOtherResume = 0x60e9f9;
constexpr uint64_t kSnapshotReader = 0x66b430;
constexpr uint64_t kSnapshotEpilogue = 0x66c380;
constexpr uint64_t kAssertSig = 0x156df78;
constexpr uint64_t kVisitorReset = 0x66b270;
constexpr uint64_t kFireEvents = 0x4bcfd0;
constexpr uint64_t kGeneratorSlot4 = 0x5f48d0;
constexpr uint64_t kSlot4Caller = 0x391590;
constexpr unsigned kWriterSiteLength = 11, kReaderSiteLength = 28;

// ---- guest memory access (unaligned, little endian) ---------------------------------------------
template <typename T> T peek(const void *p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
template <typename T> void poke(void *p, T v) { std::memcpy(p, &v, sizeof v); }
const uint8_t *peek_ptr(const void *p) { return peek<const uint8_t *>(p); }

// Guest animation records can carry debug fill (0xaaaaaaaa...) in fields the game did not set.
bool guest_pointer(const void *p) {
    const uintptr_t v = uintptr_t(p);
    if (v < 0x10000 || v >= 0x0000800000000000ull) return false;
    MEMORY_BASIC_INFORMATION m{};
    if (VirtualQuery(p, &m, sizeof m) != sizeof m) return false;
    return m.State == MEM_COMMIT && !(m.Protect & (PAGE_NOACCESS | PAGE_GUARD));
}

// At most 8 log lines from the two stream call-outs together; the rest is silent.
std::atomic<int> g_overflow_lines{0};
bool overflow_line_allowed() {
    if (g_overflow_lines.load() >= 8) return false;
    return g_overflow_lines.fetch_add(1) < 8;
}

// ---- phase encoding -----------------------------------------------------------------------------
// How many bits a clip's phase takes, and the integer that stands for phase 1.0. Short clips need
// fewer steps; long ones get 16 bits.
struct PhaseFormat {
    unsigned bits;
    float scale;
    float step;
};
constexpr PhaseFormat kPhase{10, 1023.0f, 1.0f / 1023.0f};

// Float to int32 the way the x86 conversion saturates: NaN and anything too low give INT32_MIN,
// anything too high INT32_MAX, the rest truncates toward zero.
int32_t to_int32_saturated(float v) {
    if (v != v) return INT32_MIN;
    if (v >= 2147483648.0f) return INT32_MAX;
    if (v <= -2147483648.0f) return INT32_MIN;
    return int32_t(v);
}

// Bit stream: +0 word array, +8 position in bits, +0xc limit. Bits are numbered from the top of
// each 32-bit word down.
uint32_t *stream_words(uint8_t *stream) { return peek<uint32_t *>(stream); }
uint32_t stream_position(const uint8_t *stream) { return peek<uint32_t>(stream + 8); }
uint32_t stream_limit(const uint8_t *stream) { return peek<uint32_t>(stream + 0xc); }

bool stream_has_room(const uint8_t *stream, unsigned bits) {
    return uint64_t(stream_position(stream)) + bits <= uint64_t(stream_limit(stream));
}

// Writes `bits` low bits of `value` at the position. A word the field starts in the middle of
// loses its tail first; a word the field starts at the top of is only ORed into (the game's
// writer behaves this way and the reader on the other PC does not care). A word the field spills
// into is replaced as a whole.
void stream_put(uint8_t *stream, uint32_t value, unsigned bits) {
    const uint32_t pos = stream_position(stream);
    uint32_t *word = stream_words(stream) + pos / 32;
    const unsigned used = pos % 32;
    const unsigned free_bits = 32 - used;
    const uint32_t field = value & ((1u << bits) - 1);
    uint32_t w = peek<uint32_t>(word);
    if (used != 0) w &= ~(0xffffffffu >> used);
    if (bits <= free_bits) {
        w |= field << (free_bits - bits);
        poke<uint32_t>(word, w);
    } else {
        const unsigned spill = bits - free_bits;
        w |= field >> spill;
        poke<uint32_t>(word, w);
        poke<uint32_t>(word + 1, field << (32 - spill));
    }
    poke<uint32_t>(stream + 8, pos + bits);
}

uint32_t stream_get(uint8_t *stream, unsigned bits) {
    const uint32_t pos = stream_position(stream);
    const uint32_t *word = stream_words(stream) + pos / 32;
    const unsigned used = pos % 32;
    const unsigned free_bits = 32 - used;
    const uint32_t first = peek<uint32_t>(word);
    uint32_t value;
    if (bits <= free_bits) {
        value = (first >> (free_bits - bits)) & ((1u << bits) - 1);
    } else {
        // Only now is the following word touched.
        const unsigned spill = bits - free_bits;
        const uint32_t head = first & ((1u << free_bits) - 1);  // free_bits < 32 here
        value = (head << spill) | (peek<uint32_t>(word + 1) >> (32 - spill));
    }
    poke<uint32_t>(stream + 8, pos + bits);
    return value;
}

// Writer call-out: r12 = the clip command just written (anim pointer at +4, frame at +0x10),
// r15 = the bit stream. Appends the clip's phase.
U2MP_GUEST void write_clip_phase(const uint8_t *cmd, uint8_t *stream) {
    static int calls = 0;
    const uint8_t *anim = peek_ptr(cmd + 4);
    const uint8_t *info = anim ? peek_ptr(anim + 0x28) : nullptr;
    const PhaseFormat fmt = kPhase;
    if (calls < 12) {
        ++calls;
        log("netanim: clip cmd %p size %u anim %p stream %p pos %u limit %u", static_cast<const void *>(cmd),
            unsigned(peek<uint16_t>(cmd + 2)), static_cast<const void *>(anim), static_cast<void *>(stream),
            stream_position(stream), stream_limit(stream));
    }
    int32_t quantised = 0;
    if (info) {
        const float frame = peek<float>(cmd + 0x10);
        const uint16_t frames = peek<uint16_t>(info + 0x22);
        if (frames > 1 && frame > 0.0f) {
            const float scaled = frame * fmt.scale;
            quantised = to_int32_saturated(scaled / (float(frames) - 1.0f));
            if (quantised > 1023) quantised = 1023;
        }
    }
    if (!stream_has_room(stream, fmt.bits)) {
        if (overflow_line_allowed()) log("netanim: snapshot stream full, clip phase not written");
        return;
    }
    stream_put(stream, uint32_t(quantised), fmt.bits);
}

// Per-thread snapshot parse state: the 11-bit anim indices and the list mark.
thread_local uint16_t g_indices[32];
thread_local uint32_t g_index_count;
thread_local struct { uint8_t *list; uint32_t count, top; } g_mark;
thread_local struct { const uint8_t *group; uint32_t idx; const uint8_t *anim; } g_cache[8];
thread_local unsigned g_cache_next;
std::atomic<int> g_leaves{0}, g_clips{0}, g_poses{0}, g_misses{0}, g_deep{0}, g_indices_read{0};

uint32_t read_bits(uint8_t *stream, unsigned bits) { return stream_get(stream, bits); }

U2MP_GUEST void read_indices(uint8_t *rbp, uint32_t count) {
    const int seen = g_indices_read.fetch_add(1) + 1;
    if (seen <= 4) log("netanim: %u anim indices", count);
    uint8_t *reader = rbp - 0x150;
    const uint32_t pos = stream_position(reader);
    const uint32_t max = stream_limit(reader);
    g_index_count = count < 32 ? count : 32;
    uint32_t p = pos;
    for (uint32_t i = 0; i < count; ++i, p += 11) {
        uint8_t tmp[0x20];
        poke<uint32_t>(tmp, p);
        poke<uint32_t>(tmp + 8, p);
        poke<uint32_t>(tmp + 0xc, max);
        poke<uint32_t *>(tmp, stream_words(reader));
        g_indices[i < 32 ? i : 31] = p + 11 <= max ? uint16_t(stream_get(tmp, 11)) : 0xffff;
    }
    poke<uint32_t>(reader + 8, p);
}

bool name_at(const uint8_t *p, char *out, size_t size) {
    for (size_t at = 0; p && at + 16 <= size; at += 16) {
        std::memcpy(out + at, p + at, 16);
        if (std::memchr(out + at, 0, 16)) return true;
    }
    return false;
}

const uint8_t *anim_for(const uint8_t *group, uint32_t idx) {
    if (!group || !guest_pointer(group) || idx >= 0x800) return nullptr;
    for (unsigned i = 0; i < 8; ++i)
        if (g_cache[i].group == group && g_cache[i].idx == idx && g_cache[i].anim) return g_cache[i].anim;
    const uint8_t *tab = peek_ptr(peek_ptr(group + 0x30) + 0x10);
    if (!tab || !guest_pointer(tab) || idx >= peek<uint32_t>(tab)) return nullptr;
    const uint8_t *entry = peek_ptr(tab + 8) + uint64_t(idx) * 16;
    if (!guest_pointer(entry)) return nullptr;
    const uint32_t want = peek<uint32_t>(entry);
    const uint8_t *x = peek_ptr(entry + 8);
    const uint8_t *found = nullptr;
    for (int deref = 0; deref < 2 && !found && x; ++deref) {
        const uint8_t *a = deref ? peek_ptr(x) : x;
        char name[128];
        const uint8_t *text = a ? peek_ptr(a) : nullptr;
        const uint8_t *clip = a ? peek_ptr(a + 0x28) : nullptr;
        if (a && guest_pointer(a) && text && guest_pointer(text) && name_at(text, name, sizeof name) &&
            sid(name) == want && clip && guest_pointer(clip) && peek<uint16_t>(clip + 0x22))
            found = a;
    }
    if (found) {
        const unsigned k = g_cache_next++ & 7;
        g_cache[k] = {group, idx, found};
    } else {
        g_misses.fetch_add(1);
    }
    return found;
}

U2MP_GUEST void read_leaf(uint8_t *rbp, const uint8_t *group, uint32_t next) {
    const int leaf = g_leaves.fetch_add(1) + 1;
    uint8_t *reader = rbp - 0x150;
    uint32_t q = 0;
    if (stream_has_room(reader, kPhase.bits)) q = stream_get(reader, kPhase.bits);
    const uint8_t *gen = peek_ptr(rbp - 0x190);
    if (!gen || !guest_pointer(gen)) return;
    const uint8_t *vt = peek_ptr(gen);
    if (vt != image.base + kPoseGenerator) return;
    const uint32_t slot = peek<uint32_t>(rbp - 0x180);
    const uint32_t sub = peek<uint32_t>(gen + 0xc);
    if (sub + slot >= 7) return;
    const uint8_t *anim = next >= 1 && next <= g_index_count ? anim_for(group, g_indices[next - 1]) : nullptr;
    if (anim) {
        using ClipFn = void(U2MP_GUEST *)(void *, const void *, uint32_t, float);
        reinterpret_cast<ClipFn>(peek<uint64_t>(vt + 0x18))(const_cast<uint8_t *>(gen), anim, slot, float(q) * kPhase.step);
        g_clips.fetch_add(1);
    } else {
        using PoseFn = void(U2MP_GUEST *)(void *, uint32_t);
        reinterpret_cast<PoseFn>(peek<uint64_t>(vt + 0x28))(const_cast<uint8_t *>(gen), slot);
        g_poses.fetch_add(1);
    }
    if (leaf % 64 == 0)
        log("netanim: leaves %d, clips %d, poses %d, misses %d, deep drops %d, index reads %d", leaf,
            g_clips.load(), g_poses.load(), g_misses.load(), g_deep.load(), g_indices_read.load());
}

U2MP_GUEST void prefill(uint8_t *list, uint32_t base) {
    uint32_t *words = peek<uint32_t *>(list);
    uint32_t count = peek<uint32_t>(list + 0xc);
    uint32_t top = peek<uint32_t>(list + 0x10);
    for (uint32_t s = base; s <= base + 1; ++s) {
        words[count] = 0x0002000du;
        words[count + 1] = s;
        count += 2;
        if (top < s) top = s;
    }
    poke<uint32_t>(list + 0xc, count);
    poke<uint32_t>(list + 0x10, top);
    g_mark = {list, count, top};
}

U2MP_GUEST void depth_check(uint8_t *list, uint32_t base) {
    const uint32_t top = peek<uint32_t>(list + 0x10);
    const uint32_t depth = top >= base ? top - base : 0;
    if (depth <= 5 || g_mark.list != list) return;
    poke<uint32_t>(list + 0xc, g_mark.count);
    poke<uint32_t>(list + 0x10, g_mark.top);
    if (g_deep.fetch_add(1) < 8) log("netanim: deep tree dropped (%u slots, limit 5)", depth);
}

// ---- call-out stubs -----------------------------------------------------------------------------
// Machine code placed next to the image. The game jumps here from the patched site; the stub keeps
// every register the game may still need, calls the C++ function with an aligned stack, then
// performs the instructions the jump replaced and continues at the original target.
struct Code {
    uint8_t *p;
    void bytes(std::initializer_list<uint8_t> list) {
        for (uint8_t b : list) *p++ = b;
    }
    void u32(uint32_t v) { std::memcpy(p, &v, 4), p += 4; }
    void u64(uint64_t v) { std::memcpy(p, &v, 8), p += 8; }
    // movups [rsp+offset], xmmN (store) or movups xmmN, [rsp+offset] (load)
    void xmm(bool load, unsigned n, uint32_t offset) {
        if (n >= 8) *p++ = 0x44;
        bytes({0x0f, uint8_t(load ? 0x10 : 0x11)});
        if (offset < 0x80) {
            bytes({uint8_t(0x44 | ((n & 7) << 3)), 0x24, uint8_t(offset)});
        } else {
            bytes({uint8_t(0x84 | ((n & 7) << 3)), 0x24});
            u32(offset);
        }
    }
};

// Scratch registers saved by push, in this order (encoding of push; pop is +8).
constexpr struct { bool high; uint8_t low; } kSaved[] = {
    {false, 0}, {false, 1}, {false, 2}, {false, 6}, {false, 7},  // rax rcx rdx rsi rdi
    {true, 0},  {true, 1},  {true, 2},  {true, 3},               // r8 r9 r10 r11
    {false, 3},                                                  // rbx
};
constexpr unsigned kSavedCount = sizeof kSaved / sizeof kSaved[0];

void emit_save(Code &c) {
    c.bytes({0x48, 0x8d, 0x64, 0x24, 0x80});  // lea rsp, [rsp-0x80]: step over the red zone
    c.bytes({0x55});                          // push rbp
    c.bytes({0x48, 0x89, 0xe5});              // mov rbp, rsp
    for (const auto &r : kSaved) {
        if (r.high) c.bytes({0x41});
        c.bytes({uint8_t(0x50 + r.low)});
    }
    c.bytes({0x48, 0x83, 0xe4, 0xf0});              // and rsp, -16
    c.bytes({0x48, 0x81, 0xec, 0x00, 0x01, 0x00, 0x00});  // sub rsp, 0x100
    for (unsigned n = 0; n < 16; ++n) c.xmm(false, n, n * 16);
}

void emit_call(Code &c, void *host) {
    c.bytes({0x48, 0xb8});  // mov rax, host
    c.u64(uint64_t(uintptr_t(host)));
    c.bytes({0xff, 0xd0});  // call rax
}

void emit_restore(Code &c) {
    for (unsigned n = 0; n < 16; ++n) c.xmm(true, n, n * 16);
    c.bytes({0x48, 0x8d, 0x65, uint8_t(-int(kSavedCount * 8))});  // lea rsp, [rbp-0x50]
    for (unsigned i = kSavedCount; i-- > 0;) {
        if (kSaved[i].high) c.bytes({0x41});
        c.bytes({uint8_t(0x58 + kSaved[i].low)});
    }
    c.bytes({0x5d});                                            // pop rbp
    c.bytes({0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00});  // lea rsp, [rsp+0x80]
}

void emit_jump(Code &c, uint64_t offset) {
    c.bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00});  // jmp [rip+0]
    c.u64(uint64_t(uintptr_t(image.base + offset)));
}

void build_writer_stub(uint8_t *stub) {
    Code c{stub};
    emit_save(c);
    c.bytes({0x4c, 0x89, 0xe7});  // mov rdi, r12 (clip command)
    c.bytes({0x4c, 0x89, 0xfe});  // mov rsi, r15 (bit stream)
    emit_call(c, reinterpret_cast<void *>(&write_clip_phase));
    emit_restore(c);
    c.bytes({0x41, 0xff, 0xc2});  // inc r10d
    c.bytes({0x41, 0xff, 0xc6});  // inc r14d
    emit_jump(c, kWriterResume);
}

void build_reader_stub(uint8_t *stub) {
    Code c{stub};
    c.bytes({0x4c, 0x89, 0xb5, 0x78, 0xfe, 0xff, 0xff});  // mov [rbp-0x188], r14 (replaced store)
    emit_save(c);
    c.bytes({0x48, 0x8b, 0x7d, 0x00});  // mov rdi, [rbp]: the reader's own rbp
    c.bytes({0x48, 0x89, 0xc6});        // mov rsi, rax (the art group)
    c.bytes({0x4c, 0x89, 0xf2});        // mov rdx, r14 (the index + 1)
    emit_call(c, reinterpret_cast<void *>(&read_leaf));
    emit_restore(c);
    c.bytes({0x48, 0x8b, 0x85, 0x80, 0xfe, 0xff, 0xff});  // mov rax, [rbp-0x180]
    c.bytes({0xff, 0xc0});                                // inc eax
    c.bytes({0x48, 0x89, 0x85, 0x80, 0xfe, 0xff, 0xff});  // mov [rbp-0x180], rax
    emit_jump(c, kReaderResume);
}

void build_index_stub(uint8_t *stub) {
    Code c{stub};
    emit_save(c);
    c.bytes({0x48, 0x8b, 0x7d, 0x00});  // mov rdi, [rbp]
    c.bytes({0x44, 0x89, 0xf6});        // mov esi, r14d
    emit_call(c, reinterpret_cast<void *>(&read_indices));
    emit_restore(c);
    emit_jump(c, kIndexResume);
}

void build_prefill_stub(uint8_t *stub) {
    Code c{stub};
    emit_save(c);
    c.bytes({0x48, 0x89, 0xdf});  // mov rdi, rbx (the list)
    c.bytes({0x44, 0x89, 0xe6});  // mov esi, r12d (the base slot)
    emit_call(c, reinterpret_cast<void *>(&prefill));
    emit_restore(c);
    c.bytes({0x48, 0xb9});  // mov rcx, image + 0x100ac80
    c.u64(uint64_t(uintptr_t(image.base + kPoseGenerator)));
    emit_jump(c, kPrefillResume);
}

void build_depth_stub(uint8_t *stub) {
    Code c{stub};
    emit_save(c);
    c.bytes({0x4c, 0x89, 0xff});  // mov rdi, r15 (the list)
    c.bytes({0x89, 0xde});        // mov esi, ebx (the base slot)
    emit_call(c, reinterpret_cast<void *>(&depth_check));
    emit_restore(c);
    c.bytes({0x41, 0x8b, 0x47, 0x0c});  // mov eax, [r15+0xc]
    c.bytes({0x49, 0x8b, 0x0f});        // mov rcx, [r15]
    emit_jump(c, kDepthResume);
}

// A snapshot's bit count decides how many bytes are copied: the stream is 32-bit words filled
// from the top, but the copy is ceil(bits/8) bytes. On little-endian the last partial word's high
// bits never reach the wire, leaving whatever an earlier snapshot left there. Round the count up
// to whole words (the writer's rest of the word is zero after the end marker); the history holds
// 0xaa bytes, so a count above 0x550 keeps its exact value.
U2MP_GUEST void snapshot_pad(uint8_t *snapshot, uint32_t bits) {
    uint32_t n = (bits + 31u) & ~31u;
    if (n > 0x550) n = bits;
    std::memcpy(snapshot + 0xbc, &n, 4);
}

void build_pad_stub(uint8_t *stub, bool player) {
    Code c{stub};
    emit_save(c);
    if (player) c.bytes({0x4c, 0x89, 0xe7});  // mov rdi, r12 (the player snapshot)
    else c.bytes({0x48, 0x89, 0xdf});         // mov rdi, rbx (the other writer's snapshot)
    c.bytes({0x89, 0xc6});                    // mov esi, eax (the bit count)
    emit_call(c, reinterpret_cast<void *>(&snapshot_pad));
    emit_restore(c);
    emit_jump(c, player ? kPadPlayerResume : kPadOtherResume);
}

// Writes `jmp stub` at the site and fills the rest of the replaced bytes with int3.
bool jump_to_stub(uint64_t site, unsigned length, const uint8_t *stub) {
    const int64_t delta = int64_t(uintptr_t(stub)) - int64_t(uintptr_t(image.base + site + 5));
    if (delta < INT32_MIN || delta > INT32_MAX) {
        log("netanim: stub out of jump range");
        return false;
    }
    uint8_t *code = image.base + site;
    code[0] = 0xe9;
    poke<int32_t>(code + 1, int32_t(delta));
    std::memset(code + 5, 0xcc, length - 5);
    return true;
}

// ---- per-generator animation lists --------------------------------------------------------------
constexpr unsigned kListCapacity = 16;

struct AnimEntry {
    uint32_t key;
    const uint8_t *anim;
    float phase;
    uint8_t dir;  // 1 = playing forward, 0 = backward
    uint8_t mark;
};

struct AnimList {
    AnimEntry entry[kListCapacity];
    uint32_t count;

    void clear() { *this = AnimList{}; }
    void append(const AnimEntry &e) {
        if (count < kListCapacity) entry[count++] = e;
    }
};

// Lists live as long as the process; the anim layer may call from several job threads.
std::mutex g_lists_lock;
std::unordered_map<const uint8_t *, std::unique_ptr<AnimList>> g_lists;

AnimList &list_of(const uint8_t *generator) {
    std::lock_guard<std::mutex> hold(g_lists_lock);
    std::unique_ptr<AnimList> &slot = g_lists[generator];
    if (!slot) slot = std::make_unique<AnimList>();
    return *slot;
}

bool same_keys(const AnimList &x, const AnimList &y, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i)
        if (x.entry[i].key != y.entry[i].key) return false;
    return true;
}

// Keeps the list while one of its animations has not reached its end in its own direction.
void keep_or_clear(AnimList &list) {
    for (uint32_t i = 0; i < list.count; ++i) {
        const AnimEntry &e = list.entry[i];
        if (e.dir != 0 ? e.phase < 1.0f : e.phase > 0.0f) return;
    }
    list.clear();
}

// ---- host visitor (the PS4 layout the snapshot reader expects) ----------------------------------
constexpr uint64_t kVisitorList = 0x38, kVisitorLastKey = 0x40;

AnimList *visitor_list(uint8_t *visitor) { return peek<AnimList *>(visitor + kVisitorList); }

U2MP_GUEST void visit_reset(uint8_t *visitor) {
    reinterpret_cast<void(U2MP_GUEST *)(void *)>(image.base + kVisitorReset)(visitor);
    poke<uint32_t>(visitor + kVisitorLastKey, 0);
}

U2MP_GUEST void visit_nothing(uint8_t *) {}

// Records an animation that has events; the key is its event table's id.
U2MP_GUEST void visit_anim(uint8_t *visitor, const uint8_t *anim, uint32_t, float phase) {
    poke<uint32_t>(visitor + kVisitorLastKey, 0);
    if (!anim || !guest_pointer(anim)) return;
    const uint8_t *table = peek_ptr(anim + 0x48);
    if (!guest_pointer(table) || peek<uint32_t>(table + 4) == 0 || !guest_pointer(peek_ptr(anim + 0x28))) return;
    const uint32_t key = peek<uint32_t>(table);
    poke<uint32_t>(visitor + kVisitorLastKey, key);
    visitor_list(visitor)->append({key, anim, phase, 1, 0});
}

U2MP_GUEST void visit_mark(uint8_t *visitor, uint32_t) {
    const uint32_t key = peek<uint32_t>(visitor + kVisitorLastKey);
    if (key == 0) return;
    AnimList *list = visitor_list(visitor);
    for (uint32_t i = list->count; i-- > 0;) {
        if (list->entry[i].key == key) {
            list->entry[i].mark = 1;
            return;
        }
    }
}

void *const g_visitor_vtable[7] = {
    reinterpret_cast<void *>(&visit_reset),   reinterpret_cast<void *>(&visit_nothing),
    reinterpret_cast<void *>(&visit_nothing), reinterpret_cast<void *>(&visit_anim),
    reinterpret_cast<void *>(&visit_mark),    reinterpret_cast<void *>(&visit_nothing),
    reinterpret_cast<void *>(&visit_nothing),
};

// ---- slot 4 arithmetic --------------------------------------------------------------------------
// min / max / select with the NaN behaviour the update needs: a NaN operand wins min and max; a
// NaN condition selects the second value.
float nan_min(float x, float y) {
    if (x != x) return x;
    if (y != y) return y;
    return x < y ? x : y;
}
float nan_max(float x, float y) {
    if (x != x) return x;
    if (y != y) return y;
    return x > y ? x : y;
}
float pick(float c, float x, float y) { return c >= 0.0f ? x : y; }

// What the update needs to know about one animation, read fresh from the game every time.
struct AnimFacts {
    const void *event_table;
    uint32_t looping;
    float last_frame;  // frame count - 1
    float duration;
};
AnimFacts facts_of(const uint8_t *anim) {
    const uint8_t *info = peek_ptr(anim + 0x28);
    AnimFacts f;
    f.event_table = peek_ptr(anim + 0x48);
    f.looping = peek<uint32_t>(anim + 0x24) & 1;
    f.last_frame = float(peek<uint16_t>(info + 0x22)) - 1.0f;
    const float frames_per_tick = peek<float>(info + 0x2c) * 30.0f;
    f.duration = f.last_frame * frames_per_tick;
    return f;
}

struct Update {
    void *events;
    float dt30;  // elapsed time in 30 Hz frames

    // New phase after moving `phase` by the elapsed frames, clamped to the clip.
    float advanced(const AnimFacts &f, float phase, bool forward) const {
        const float at = f.last_frame * phase;
        const float v = forward ? at + dt30 : at - dt30;
        const float frame = nan_max(nan_min(v, f.last_frame), 0.0f);
        const float ratio = frame / f.last_frame;
        return nan_max(nan_min(ratio, 1.0f), 0.0f);
    }

    void fire(const AnimFacts &f, bool forward, float from, float to) const {
        using FireFn = void(U2MP_GUEST *)(const void *, uint32_t, uint32_t, uint32_t, uint32_t, void *, float,
                                          float, float);
        const float start = f.duration * from;
        const float end = f.duration * to;
        reinterpret_cast<FireFn>(image.base + kFireEvents)(f.event_table, forward ? 1 : 0, f.looping, 0, 1, events,
                                                           start, end, f.duration);
    }

    void step_forward(AnimEntry &e, const AnimFacts &f, float phase, bool quiet) const {
        const float next = advanced(f, phase, true);
        const float old = e.phase;
        e.phase = pick(old - next, old, next);
        if (next > old && !quiet) fire(f, true, old, next);
    }

    void step_backward(AnimEntry &e, const AnimFacts &f, float phase, bool quiet) const {
        const float next = advanced(f, phase, false);
        const float old = e.phase;
        e.phase = pick(old - next, next, old);
        if (next < old && !quiet) fire(f, false, old, next);
    }
};

// The newest two snapshots disagree on keys, so start over from what moved between them.
void rebuild(AnimList &list, const AnimList &older, const AnimList &newer, uint32_t n) {
    list.clear();
    for (uint32_t i = 0; i < n; ++i) {
        const AnimEntry &a = older.entry[i];
        const AnimEntry &b = newer.entry[i];
        const bool looping = (peek<uint32_t>(a.anim + 0x24) & 1) != 0;
        bool forward = false, backward = false;
        if (looping) {
            if (a.phase - b.phase > 0.1f || a.phase < b.phase) forward = true;
            else if (a.phase > b.phase) backward = true;
        } else {
            if (a.phase < b.phase) forward = true;
            else if (a.phase > b.phase) backward = true;
        }
        if (forward) list.append({a.key, nullptr, pick(a.phase - 0.15f, a.phase, 0.0f), 1, 0});
        else if (backward) list.append({a.key, nullptr, pick(0.85f - a.phase, a.phase, 1.0f), 0, 0});
    }
}

// Same keys in the list and the older snapshot; the newer one differs.
void advance_main(AnimList &list, const AnimList &older, uint32_t n, const Update &u) {
    for (uint32_t i = 0; i < n; ++i) {
        const AnimEntry &a = older.entry[i];
        bool quiet = false;
        for (uint32_t j = 0; j < i; ++j)
            if (older.entry[j].key == a.key) quiet = true;
        const AnimFacts f = facts_of(a.anim);
        AnimEntry &e = list.entry[i];
        if (e.dir != 0) u.step_forward(e, f, a.phase, quiet);
        else u.step_backward(e, f, a.phase, quiet);
    }
}

// Same keys everywhere: follow the direction the two snapshots show, wrapping looped clips.
void advance_same(AnimList &list, const AnimList &older, const AnimList &newer, uint32_t n, const Update &u) {
    for (uint32_t i = 0; i < n; ++i) {
        const AnimEntry &a = older.entry[i];
        const AnimEntry &b = newer.entry[i];
        bool quiet = false;
        for (uint32_t j = 0; j < i; ++j)
            if (older.entry[j].key == a.key && std::fabs(older.entry[j].phase - a.phase) < 0.1f) quiet = true;
        const AnimFacts f = facts_of(a.anim);
        AnimEntry &e = list.entry[i];
        if (e.dir != 0) {
            if (b.phase > a.phase) u.step_forward(e, f, a.phase, quiet);
            else if (f.looping && a.phase - b.phase > 0.1f) e.phase = 0.0f;
        } else if (a.phase > b.phase) {
            u.step_backward(e, f, a.phase, quiet);
        } else if (f.looping && b.phase - a.phase > 0.1f) {
            e.phase = 1.0f;
        }
    }
}

std::atomic<int> g_slot_calls{0};

// NetAnimCmdGenerator slot 4: (generator, events list or NULL).
U2MP_GUEST void generator_slot4(const uint8_t *gen, void *events) {
    if (g_slot_calls.load() < 3 && g_slot_calls.fetch_add(1) < 3)
        log("netanim: generator slot called (generator %p)", static_cast<const void *>(gen));

    AnimList &list = list_of(gen);
    const uint32_t count = peek<uint32_t>(gen + 0x68);
    if (count < 3) {
        list.clear();
        keep_or_clear(list);
        return;
    }

    // The pair of snapshots around the generator's time (newest first).
    const uint8_t *snapshots = gen + 0x70;
    auto snapshot = [&](uint32_t i) { return snapshots + uint64_t(i) * 0x90; };
    const float now = peek<float>(gen + 0x960);
    uint32_t pair = 0;
    float frac = 0.0f;
    for (uint32_t i = 0; i + 1 < count; ++i) {
        const float t1 = peek<float>(snapshot(i + 1) + 4);
        if (t1 == 0.0f) continue;
        const float t0 = peek<float>(snapshot(i) + 4);
        if (t0 < now) {
            pair = i;
            frac = 0.0f;
            break;
        }
        if (t1 < now) {
            pair = i;
            frac = (t0 - now) / (t0 - t1);
            break;
        }
    }
    const uint8_t *s0 = snapshot(pair), *s1 = snapshot(pair + 1);

    // Collect the animations of both snapshots through one visitor object.
    AnimList newer{}, older{};
    alignas(16) uint8_t visitor[0x50] = {};
    poke<void *const *>(visitor, g_visitor_vtable);
    using ReaderFn = void(U2MP_GUEST *)(void *, const void *, const void *, uint32_t, uint32_t, uint32_t);
    const auto read_snapshot = reinterpret_cast<ReaderFn>(image.base + kSnapshotReader);
    const uint32_t word = peek<uint32_t>(gen + 0x24);
    poke<AnimList *>(visitor + kVisitorList, &newer);
    read_snapshot(visitor, s0, gen + 0x28, 0, 0, word);
    poke<AnimList *>(visitor + kVisitorList, &older);
    read_snapshot(visitor, s1, gen + 0x28, 1, 1, word);

    const float t0 = peek<float>(s0 + 4), t1 = peek<float>(s1 + 4);
    const float keep = 1.0f - frac;
    const float from_newer = keep * t0;
    const float from_older = t1 * frac;
    const float blended = from_newer + from_older;
    const float dt = blended - t1;

    if (!events) return;

    const Update u{events, dt * 30.0f};
    const uint32_t n = older.count;
    const bool newer_matches = newer.count == n && same_keys(newer, older, n);
    if (list.count == n && same_keys(list, older, n)) {
        if (n != 0) {
            if (newer_matches) advance_same(list, older, newer, n, u);
            else advance_main(list, older, n, u);
        }
    } else if (newer_matches) {
        rebuild(list, older, newer, n);
    }
    keep_or_clear(list);
}

struct Check {
    uint64_t offset;
    const unsigned char *bytes;
    size_t length;
};

template <size_t N> bool all_match(const Check (&checks)[N]) {
    for (const Check &c : checks)
        if (!image.matches(c.offset, c.bytes, c.length)) return false;
    return true;
}

}  // namespace

// The snapshot parser's assert blocks (each loads the assertion hook from the same global) end the
// parse instead: a garbled snapshot must not take the game down, and the parse's own epilogue is
// what the game uses when it runs out of data.
int skip_snapshot_asserts() {
    int patched = 0;
    for (uint64_t at = kSnapshotReader; at + 7 <= kSnapshotEpilogue && patched < 32; ++at) {
        const uint8_t *p = image.base + at;
        if (p[0] != 0x48 || p[1] != 0x8b || p[2] != 0x05) continue;
        if (at + 7 + int64_t(peek<int32_t>(p + 3)) != kAssertSig) continue;
        const int64_t delta = int64_t(image.base + kSnapshotEpilogue) - int64_t(image.base + at + 5);
        if (delta < INT32_MIN || delta > INT32_MAX) break;
        uint8_t jump[7] = {0xe9, 0, 0, 0, 0, 0x90, 0x90};
        poke<int32_t>(jump + 1, int32_t(delta));
        std::memcpy(image.base + at, jump, sizeof jump);
        ++patched;
        at += 6;
    }
    return patched;
}

void install_netanim(int &installed) {
    if (const char *env = std::getenv("UNCHARTED_U2MP_NETANIM"); env && std::strcmp(env, "0") == 0) {
        log("netanim: off (UNCHARTED_U2MP_NETANIM=0)");
        return;
    }

    const int assert_sites = skip_snapshot_asserts();
    if (assert_sites) log("netanim: snapshot parse aborts on asserts (%d sites)", assert_sites);

    static const unsigned char writer_site[] = {0x41, 0xff, 0xc2, 0x41, 0xff, 0xc6, 0xe9, 0x40, 0x1b, 0x00, 0x00};
    static const unsigned char writer_eq[] = {0x44, 0x3b, 0x5c, 0x24, 0x54, 0x0f, 0x84, 0xb4};
    static const unsigned char writer_jae[] = {0x73, 0x47};
    static const unsigned char reader_site[] = {0x4c, 0x89, 0xb5, 0x78, 0xfe, 0xff, 0xff, 0x48, 0x8b, 0x85,
                                                0x80, 0xfe, 0xff, 0xff, 0xff, 0xc0, 0x48, 0x89, 0x85, 0x80,
                                                0xfe, 0xff, 0xff, 0xe9, 0xff, 0x01, 0x00, 0x00};
    static const unsigned char reader_jne[] = {0x48, 0x85, 0xc0, 0x75, 0x35};
    static const unsigned char reader_entry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                                 0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0x88, 0x01, 0x00, 0x00};
    static const unsigned char index_site[] = {0x4c, 0x8d, 0x35, 0x3c, 0xc7, 0x81, 0x00};
    static const unsigned char prefill_site[] = {0x48, 0x8d, 0x0d, 0x16, 0x6f, 0xa1, 0x00};
    static const unsigned char depth_site[] = {0x41, 0x8b, 0x47, 0x0c, 0x49, 0x8b, 0x0f};
    static const unsigned char c_room[] = {0x8d, 0x42, 0x04}, c_clear[] = {0x83, 0xf9, 0x03},
                               c_add[] = {0x83, 0xc0, 0x04};
    static const Check stream_checks[] = {
        {kWriterSite, writer_site, sizeof writer_site}, {0x6363f6, writer_eq, sizeof writer_eq},
        {0x63646c, writer_jae, sizeof writer_jae},      {kReaderSite, reader_site, sizeof reader_site},
        {0x66bffb, reader_jne, sizeof reader_jne},      {kSnapshotReader, reader_entry, sizeof reader_entry},
        {kIndexSite, index_site, sizeof index_site},    {kPrefillSite, prefill_site, sizeof prefill_site},
        {kDepthSite, depth_site, sizeof depth_site},    {0x6367cb, c_room, sizeof c_room},
        {0x6368a0, c_clear, sizeof c_clear},            {0x6368b7, c_add, sizeof c_add},
    };
    if (!all_match(stream_checks)) {
        log("netanim: snapshot writer / reader bytes differ from 1.02, left alone (no clip phases)");
        return;
    }

    // Both requests are always made: their order and size decide where the stubs land.
    uint8_t *writer_stub = near_stub(512);
    uint8_t *reader_stub = near_stub(512);
    uint8_t *index_stub = near_stub(512);
    uint8_t *prefill_stub = near_stub(512);
    uint8_t *depth_stub = near_stub(512);
    if (!writer_stub || !reader_stub || !index_stub || !prefill_stub || !depth_stub) {
        log("netanim: no stub memory near the image");
        return;
    }
    build_writer_stub(writer_stub);
    build_reader_stub(reader_stub);
    build_index_stub(index_stub);
    build_prefill_stub(prefill_stub);
    build_depth_stub(depth_stub);
    log("netanim: stubs writer=%p reader=%p index=%p prefill=%p depth=%p", static_cast<void *>(writer_stub),
        static_cast<void *>(reader_stub), static_cast<void *>(index_stub), static_cast<void *>(prefill_stub),
        static_cast<void *>(depth_stub));
    if (!jump_to_stub(kWriterSite, kWriterSiteLength, writer_stub)) return;
    if (!jump_to_stub(kReaderSite, kReaderSiteLength, reader_stub)) return;
    if (!jump_to_stub(kIndexSite, sizeof index_site, index_stub)) return;
    if (!jump_to_stub(kPrefillSite, sizeof prefill_site, prefill_stub)) return;
    if (!jump_to_stub(kDepthSite, sizeof depth_site, depth_stub)) return;
    poke<uint8_t>(image.base + 0x6367cd, 10);
    poke<uint8_t>(image.base + 0x6368a2, 9);
    poke<uint8_t>(image.base + 0x6368b9, 10);
    installed += 5;
    log("netanim: snapshot clip phases restored (writer 0x6364b5, reader 0x66c035; both PCs need this build)");
    log("netanim: anim indices, bind-pose prefill and tree depth guard installed");

    static const unsigned char pad_player[8] = {0x41, 0x89, 0x84, 0x24, 0xbc, 0x00, 0x00, 0x00};
    static const unsigned char pad_other[6] = {0x89, 0x83, 0xbc, 0x00, 0x00, 0x00};
    const char *pad = std::getenv("UNCHARTED_U2MP_NETANIM_PAD");
    if (!(pad && std::strcmp(pad, "0") == 0) && image.matches(kPadPlayerSite, pad_player, sizeof pad_player) &&
        image.matches(kPadOtherSite, pad_other, sizeof pad_other)) {
        uint8_t *player_stub = near_stub(384);
        uint8_t *other_stub = near_stub(384);
        if (player_stub && other_stub) {
            build_pad_stub(player_stub, true);
            build_pad_stub(other_stub, false);
            if (jump_to_stub(kPadPlayerSite, sizeof pad_player, player_stub) &&
                jump_to_stub(kPadOtherSite, sizeof pad_other, other_stub)) {
                installed += 1;
                log("netanim: snapshot bit counts padded to whole words");
            }
        }
    } else {
        log("netanim: bit count padding off (UNCHARTED_U2MP_NETANIM_PAD=0)");
    }


}

}  // namespace u2mp
