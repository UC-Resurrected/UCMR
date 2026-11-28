// Uncharted 3 1.02, 28 Nov 2026: the PS3 colour grading of the render settings, dropped by the PS4 build.
//
// Every render-settings record has a colour-grading block: lut-enable (+0x740) and, on PS4 after seven PS4-only words, 35
// parameters at +0x760 (shoulder r, g, b; toe strength, toe angle; then hue shift, saturation and luminance for the
// global, red, green, blue, cyan, yellow, magenta, white, black and grey zones). PS3 1.19 turns that block into a
// 32x32x32 RGBA8 colour table every frame, from the current (blended) settings, whenever lut-enable is above one half,
// and the post pass looks every pixel up in it. The PS4 build only picks ready-made tables named by its own words
// (+0x750 count, +0x754 / +0x758 names, +0x75c blend); no level names one, so every map shows the identity table and the
// PS3 grading (15 multiplayer and co-op records, e.g. Facility, Museum, Ruins, Chateau, Warzone) is lost.
//
// The bake below follows our behaviour description of the PS3 job (u3-postlut-spec): the parameters quantised to
// 16-bit fixed point as PS3 hands them over, a per-channel shoulder and toe curve, an RGB -> HSV round trip with
// luminance zones (black below 0.2, white above 0.8) and six hue zones, the zone shifts added up, and the result back
// to RGB. It must run in single precision with round toward zero and fused multiply-adds where PS3 has them: plain
// float rounding is off by up to 199 steps on Chateau, whose hue shifts put many colours on sector edges. A colour
// whose hue is exactly 0 (r > g = b) and whose hue shifts add up to exactly 0 comes out magenta: PS3 does that too.
// The table layout is the one of the PS4 identity table (index r + 32 g + 1024 b, lattice values 8k + k/4), so it drops
// into the PS4 post pass unchanged.
//
// Only multiplayer and co-op (net games) are graded: the PS4 campaign never had the grading (some campaign records do
// turn lut-enable on), so the campaign keeps the PS4 look. The net-game flag is the one retarget_mode.cpp watches.
//
// The hook wraps the PS4 LUT update (called with the LUT state and the current render settings each frame). After the
// game's own update, in a net game, when lut-enable is above one half and the record names no PS4 table (as PS3: even
// when all 35 values are zero), it asks for a table of those values (a bake thread makes it, only when the values
// changed), puts each finished table into a 3D texture made the way the game makes its default table, and points the
// state's sampled table (+0x18) at it. Otherwise the game's choice stays; when that is still our table (say after a map change), the state gets its
// default table (+0x30) back. Two textures take turns, so a new table never overwrites the one the frame in flight
// samples. A new table shows a frame or two after the values change (PS3 bakes in the same frame).
// U3_PS3_GRADING=0 (or the word grading-ps4 in u3-switches.txt) keeps the PS4 behaviour: no hook.
#include "u3fix.h"
#include "u3fix_hook.h"

#include <cfenv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

namespace u3fix {

namespace {

// ---- the bake (single precision, round toward zero, denormals as zero, no infinities) ----

float flush(float x) {
    uint32_t u;
    std::memcpy(&u, &x, 4);
    if ((u & 0x7f800000u) == 0) u &= 0x80000000u;
    std::memcpy(&x, &u, 4);
    return x;
}
float from_bits(uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
constexpr float kLargest = 3.4028235e38f;
// each operation through a volatile, so the compiler neither folds nor reorders it (the file is also built with
// -ffp-contract=off -frounding-math)
float add(float a, float b) { volatile float r = flush(a) + flush(b); return flush(r); }
float sub(float a, float b) { volatile float r = flush(a) - flush(b); return flush(r); }
float mul(float a, float b) { volatile float r = flush(a) * flush(b); return flush(r); }
float fma_add(float a, float b, float c) { return flush(std::fmaf(flush(a), flush(b), flush(c))); }     // a b + c
float fma_sub(float a, float b, float c) { return flush(std::fmaf(flush(a), flush(b), -flush(c))); }    // a b - c
float fma_rsub(float a, float b, float c) { return flush(-std::fmaf(flush(a), flush(b), -flush(c))); }  // c - a b
float no_inf(float x) { return std::isinf(x) ? std::copysign(kLargest, x) : x; }
// the reciprocal estimate (1/0 = the largest float), then one refinement step
float estimate(float d) {
    if (d == 0.0f) return kLargest;
    volatile float r = 1.0f / d;
    return flush(r);
}
float reciprocal(float d) {
    const float r = estimate(d);
    return no_inf(fma_add(fma_rsub(r, d, 1.0f), r, r));
}
float divide(float n, float d) {
    const float r = estimate(d), q = mul(n, r);
    return fma_add(fma_rsub(q, d, n), r, q);
}
float clamp01(float x) { return x > 0.0f ? (x < 1.0f ? x : 1.0f) : 0.0f; }
// as PS3 hands the values to the bake: the product with 32767 rounded to nearest, truncated to a 16-bit integer
float to_q15(float v) {
    volatile float p = v * 32767.0f;
    return static_cast<float>(static_cast<int16_t>(static_cast<int32_t>(p)));
}

struct Zones {
    float hue, sat, lum;
};

void bake_table(const float *lut, unsigned char *rgba) {
    // quantise (round to nearest)
    std::fesetround(FE_TONEAREST);
    // the zones in the order the sums take them: global, red, green, blue, cyan, magenta, yellow, black, white (grey is
    // not used); lut[] index of each zone's hue shift
    constexpr int kZone[9] = {6, 9, 12, 15, 18, 24, 21, 30, 27};
    float qz[9][3];
    for (int z = 0; z < 9; z++)
        for (int k = 0; k < 3; k++) qz[z][k] = to_q15(lut[kZone[z] + k]);
    const float q_toe_strength = to_q15(lut[4]), q_toe_angle = to_q15(lut[5]);
    const float q_black = to_q15(0.2f), q_white = to_q15(0.8f);   // the luminance zone levels PS3 uses

    std::fesetround(FE_TOWARDZERO);
    const float c15 = from_bits(0x38000100u);    // 1/32767 as PS3 has it
    const float k255 = from_bits(0x3f808081u);   // 256/255
    const float k360 = from_bits(0x3b360b61u);   // 1/360
    // shoulder per channel: a line A x below the split point p, above it the parabola through (1, 1) that meets the line
    // at p with the same slope
    float split[3], slope[3], qa[3], qd[3], qe[3];
    for (int c = 0; c < 3; c++) {
        const float s = lut[1 + c];
        split[c] = fma_add(s, -0.5f, 0.5f);
        slope[c] = divide(fma_add(s, 0.5f, 0.5f), split[c]);
        const float den = fma_sub(split[c], sub(2.0f, split[c]), 1.0f);   // -(1 - p)^2
        qa[c] = divide(sub(slope[c], 1.0f), den);
        qd[c] = fma_add(qa[c], mul(split[c], -2.0f), slope[c]);
        qe[c] = add(sub(-qa[c], qd[c]), 1.0f);
    }
    // toe: below the toe strength y becomes y (angle + (1 - angle) y / strength)
    float toe = mul(q_toe_strength, c15);
    if (toe < 1e-4f) toe = from_bits(0x38d1b717u);
    const float toe_inv = reciprocal(toe), toe_angle = mul(q_toe_angle, c15);
    Zones zone[9];
    for (int z = 0; z < 9; z++) zone[z] = {mul(qz[z][0], c15), mul(qz[z][1], c15), mul(qz[z][2], c15)};
    const float black = mul(q_black, c15), white = mul(q_white, c15);

    for (int bi = 0; bi < 32; bi++)
        for (int gi = 0; gi < 32; gi++)
            for (int ri = 0; ri < 32; ri++) {
                const int index[3] = {ri, gi, bi};
                float ch[3];
                for (int c = 0; c < 3; c++) {
                    const int byte = 8 * index[c] + (index[c] >> 2);   // the PS4 identity table's value
                    const float x = mul(static_cast<float>(byte) * (1.0f / 256.0f), k255);
                    const float y = (split[c] > x) ? mul(slope[c], x) : fma_add(fma_add(qa[c], x, qd[c]), x, qe[c]);
                    float t = mul(toe_inv, y);
                    if (t > 1.0f) t = 1.0f;
                    const float ay = mul(toe_angle, y);
                    ch[c] = fma_add(t, sub(y, ay), ay);
                }
                const float r = ch[0], g = ch[1], b = ch[2];
                // RGB -> HSV, hue in turns; green wins ties, so greys get hue 1/3
                float lo = (g > r) ? r : g, hi = (g > r) ? g : r;
                lo = (b > lo) ? lo : b;
                hi = (b > hi) ? b : hi;
                const float delta = sub(hi, lo), inv_delta = reciprocal(delta), inv_hi = reciprocal(hi);
                const float sat = (hi == 0.0f) ? 0.0f : mul(inv_hi, delta);
                float sector = (hi == r) ? mul(sub(g, b), inv_delta) : fma_add(sub(r, g), inv_delta, 4.0f);
                if (hi == g) sector = fma_add(inv_delta, sub(b, r), 2.0f);
                float degrees = mul(60.0f, sector);
                if (degrees < 0.0f) degrees = add(360.0f, degrees);
                const float hue = mul(degrees, k360), value = hi;
                // zone weights: black and white by value, the six hues (full within 20 degrees, none beyond 40) inside
                const float w_black = clamp01(mul(100.0f, sub(black, value)));
                const float w_white = clamp01(mul(100.0f, sub(value, white)));
                const float mid = sub(1.0f, add(w_white, w_black));
                const float x6 = mul(hue, 6.0f);
                float w[7];
                for (int k = 0; k < 7; k++) w[k] = sub(1.0f, clamp01(fma_add(std::fabs(sub(x6, static_cast<float>(k))), 3.0f, -1.0f)));
                const float w_red = (w[0] > w[6]) ? w[0] : w[6];
                const float weight[8] = {mul(w_red, mid), mul(w[2], mid), mul(w[4], mid), mul(w[3], mid),
                                         mul(w[5], mid),  mul(w[1], mid), w_black,         w_white};
                float hs = zone[0].hue, ss = zone[0].sat, ls = zone[0].lum;
                for (int z = 0; z < 8; z++) {
                    hs = fma_add(zone[z + 1].hue, weight[z], hs);
                    ss = fma_add(zone[z + 1].sat, weight[z], ss);
                    ls = fma_add(zone[z + 1].lum, weight[z], ls);
                }
                // a hue shift of +1 turns the hue back half a turn; saturation and luminance shifts are added
                const float h2 = fma_add(-0.5f, hs, hue);
                const float s2 = clamp01(add(ss, sat)), v2 = clamp01(add(value, ls));
                // back into 0..1, where 0 itself becomes 1 (the magenta case below)
                const float hw = (h2 < 1.0f) ? ((h2 > 0.0f) ? h2 : add(h2, 1.0f)) : sub(h2, 1.0f);
                const float x2 = mul(hw, 6.0f);
                float whole = std::trunc(x2);
                if (!(whole > 0.0f)) whole = 0.0f;
                const float f = sub(x2, whole);
                const float p = mul(sub(1.0f, s2), v2), q = mul(fma_rsub(f, s2, 1.0f), v2);
                const float t = mul(fma_rsub(sub(1.0f, f), s2, 1.0f), v2);
                float o[3] = {v2, v2, v2};
                const auto set = [&o](float a, float b2, float c) { o[0] = a; o[1] = b2; o[2] = c; };
                if (s2 == 0.0f) {
                } else if (x2 < 1.0f) {
                    set(v2, t, p);
                } else if (x2 < 2.0f) {
                    set(q, v2, p);
                } else if (x2 < 3.0f) {
                    set(p, v2, t);
                } else if (x2 < 4.0f) {
                    set(p, q, v2);
                } else if (x2 < 5.0f) {
                    set(t, p, v2);
                } else {
                    set(v2, p, q);   // also x2 = 6
                }
                unsigned char *px = rgba + 4 * (ri + 32 * gi + 1024 * bi);
                for (int c = 0; c < 3; c++) {
                    const float u = (o[c] > 0.0f) ? o[c] : 0.0f;
                    const float scaled = mul(mul(k255, u), 256.0f);
                    px[c] = scaled >= 256.0f ? 255 : static_cast<unsigned char>(static_cast<uint32_t>(scaled));
                }
                px[3] = 255;
            }
}

// ---- the hook ----

constexpr unsigned kTableBytes = 32 * 32 * 32 * 4;

// UpdateLut_Texture's start: its frame, clearing the blend pair, and the test of lut-enable and the PS4 table count
constexpr unsigned char kUpdate[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0x58, 0x02, 0x00, 0x00,
    0x48, 0x8d, 0x0d, 0x1d, 0xc8, 0x88, 0x00, 0x48, 0x8b, 0x01, 0x48, 0x89, 0x45, 0xd0, 0x48, 0xc7, 0x47, 0x08, 0x00, 0x00,
    0x00, 0x00, 0x48, 0xc7, 0x07, 0x00, 0x00, 0x00, 0x00, 0xc7, 0x47, 0x10, 0x00, 0x00, 0x80, 0xbf, 0x48, 0x85, 0xf6, 0x0f,
    0x84, 0xeb, 0x06, 0x00, 0x00, 0xc5, 0xfa, 0x10, 0x05, 0x4f, 0x08, 0x5f, 0x00, 0xc5, 0xf8, 0x2e, 0x86, 0x40, 0x07, 0x00,
    0x00, 0x0f, 0x83, 0xd5, 0x06, 0x00, 0x00, 0x83, 0xbe, 0x50, 0x07, 0x00, 0x00, 0x00};
constexpr unsigned kMoved = 10;   // push rbp; mov rbp, rsp; push r15; push r14; push r13 (no rip-relative operand)
// where the default table is made: memory kind 0xe, the shape and the texels, the texture call, the view call, both
// stored as the default (+0x30) and the sampled table (+0x18)
constexpr unsigned char kDefault[] = {0xc7, 0x45, 0xb8, 0x0e, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x7d, 0xb8, 0x4c, 0x8d, 0x7d,
                                      0xa0, 0x48, 0x8d, 0x55, 0x90, 0x4c, 0x89, 0xfe, 0xe8, 0xab, 0xec, 0x01, 0x00, 0x48,
                                      0x89, 0xc6, 0xe8, 0xc3, 0x96, 0x01, 0x00, 0x48, 0x8b, 0x9d, 0x60, 0xff, 0xff, 0xff,
                                      0x48, 0x89, 0x43, 0x30, 0x48, 0x89, 0x43, 0x18};
constexpr unsigned kMakeTextureCall = 22, kMakeViewCall = 30;
// inside the texture call, its per-level copy of the texels: the level's place and size, the tiling of the level, and
// the copy into the texture's memory in the GPU's layout
constexpr unsigned char kLevel[] = {0x45, 0x31, 0xc0, 0x48, 0x8d, 0x7d, 0xb8, 0x48, 0x8d, 0x75, 0xb0, 0x4c, 0x89, 0xe2,
                                    0x44, 0x89, 0xe9, 0xe8, 0x9a, 0xd7, 0x18, 0xff, 0x31, 0xc9, 0x4c, 0x89, 0xff, 0x4c,
                                    0x89, 0xe6, 0x44, 0x89, 0xea, 0xe8, 0xfa, 0xc9, 0x18, 0xff};
constexpr unsigned kSpanCall = 17, kTilingCall = 33;
constexpr unsigned char kCopy[] = {0x4c, 0x01, 0xf7, 0x44, 0x89, 0xe8, 0x48, 0xc1, 0xe0, 0x04, 0x48, 0x8b, 0x8d, 0x78,
                                   0xff, 0xff, 0xff, 0x48, 0x8b, 0x34, 0x01, 0x4c, 0x89, 0xfa, 0xe8, 0xd4, 0xde, 0x18, 0xff};
constexpr unsigned kCopyCall = 24;

using UpdateFn = void(__attribute__((sysv_abi)) *)(unsigned char *state, const unsigned char *settings);
// (memory kind, shape, per-level texels) -> texture
using MakeTextureFn = unsigned char *(__attribute__((sysv_abi)) *)(const uint32_t *kind, const void *shape, const void *levels);
// (unused, texture) -> the view the LUT state holds
using MakeViewFn = unsigned char *(__attribute__((sysv_abi)) *)(void *unused, unsigned char *texture);
// (offset out, size out, texture, level, slice): where a level lies in the texture's memory
using SpanFn = uint32_t(__attribute__((sysv_abi)) *)(uint64_t *offset, uint64_t *size, const unsigned char *texture,
                                                     uint32_t level, uint32_t slice);
// (tiling description out, texture, level, slice)
using TilingFn = uint32_t(__attribute__((sysv_abi)) *)(void *tiling, const unsigned char *texture, uint32_t level, uint32_t slice);
// (destination, linear texels, tiling description): the copy in the GPU's layout
using CopyFn = uint32_t(__attribute__((sysv_abi)) *)(void *dst, const void *src, const void *tiling);

UpdateFn g_update = nullptr;
MakeTextureFn g_make_texture = nullptr;
MakeViewFn g_make_view = nullptr;
SpanFn g_span = nullptr;
TilingFn g_tiling = nullptr;
CopyFn g_copy = nullptr;
// the game's net-game flag (non-zero in multiplayer and co-op, 0 in the campaign; image 0x17d5c61, see retarget_mode.cpp)
constexpr uint64_t kNetGame = 0x17d5c61;
const unsigned char *g_net_game = nullptr;

// What the LUT update and the bake thread share (made once and never freed: the thread lives as long as the process).
// A bake takes 5..10 ms and region blends change the settings every frame, so it runs on a thread of its own; the LUT
// update asks for the latest values and puts each finished table into a texture (well under a millisecond).
struct Shared {
    std::mutex lock;
    std::condition_variable wake;
    bool thread_started = false;
    float asked[36] = {};          // the latest values asked for
    bool asked_valid = false;
    bool asked_new = false;        // not yet taken by the bake thread
    std::vector<unsigned char> done = std::vector<unsigned char>(kTableBytes);   // the latest finished table
    bool done_new = false;         // not yet put into a texture
    float done_values[36] = {};
    double done_ms = 0;
    // the two textures (and the views the LUT state holds) that take turns; game thread only
    unsigned char *texture[2] = {};
    unsigned char *view[2] = {};
    int current = -1;              // the view with the newest table, -1 before the first
    bool broken = false;
    unsigned reports = 0;
};
Shared &shared() {
    static Shared *s = new Shared;
    return *s;
}

void bake_thread() {
    Shared &s = shared();
    std::vector<unsigned char> table(kTableBytes);
    std::unique_lock<std::mutex> hold(s.lock);
    for (;;) {
        s.wake.wait(hold, [&s] { return s.asked_new; });
        float lut[36];
        std::memcpy(lut, s.asked, sizeof lut);
        s.asked_new = false;
        hold.unlock();
        const auto start = std::chrono::steady_clock::now();
        bake_ps3_grading_lut(lut, table.data());
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        hold.lock();
        s.done.swap(table);   // an older table not yet used is dropped: only the latest counts
        std::memcpy(s.done_values, lut, sizeof lut);
        s.done_ms = ms;
        s.done_new = true;
    }
}

uint64_t rel_target(const unsigned char *image, uint64_t call) {
    int32_t rel;
    std::memcpy(&rel, image + call + 1, 4);
    return call + 5 + static_cast<int64_t>(rel);
}

// a 32x32x32 RGBA8 3D texture made as the game makes its default table, filled with the given table; its view
unsigned char *make_texture(const unsigned char *table) {
    // shape: width, height, depth 32, one level; then the format words of the default table
    const uint64_t shape[2] = {0x0001002000200020ull, 0x000000080001001cull};
    struct Level {
        const void *texels;
        uint32_t row, slice;
    } level = {table, 32 * 4, 32 * 32 * 4};
    const uint32_t kind = 0xe;   // the memory kind the default table uses
    unsigned char *texture = g_make_texture(&kind, shape, &level);
    return texture ? g_make_view(nullptr, texture) : nullptr;
}

// copies a table into an existing texture's memory, in the GPU's layout, as the texture call does when it makes one
bool refill(unsigned char *texture, const unsigned char *table) {
    uint64_t offset = 0, bytes = 0;
    g_span(&offset, &bytes, texture, 0, 0);
    if (bytes < kTableBytes || bytes > (16u << 20)) return false;
    alignas(32) unsigned char tiling[256] = {};
    g_tiling(tiling, texture, 0, 0);
    uint32_t word0, word1;
    std::memcpy(&word0, texture, 4);
    std::memcpy(&word1, texture + 4, 4);
    const uint64_t base = (static_cast<uint64_t>(word1 & 0x3f) << 32 | word0) << 8;   // the texture's base address
    if (!base) return false;
    g_copy(reinterpret_cast<unsigned char *>(base + offset), table, tiling);
    return true;
}

// puts the finished table into a texture: the first one makes both textures, later ones fill the texture the frame in
// flight does not sample
bool use_done_table(Shared &s) {
    if (s.current < 0) {
        for (int i = 0; i < 2; i++) {
            s.view[i] = make_texture(s.done.data());
            if (!s.view[i]) return false;
            std::memcpy(&s.texture[i], s.view[i] + 8, 8);   // the view holds its texture after one word
            if (!s.texture[i]) return false;
        }
        s.current = 0;
        return true;
    }
    const int next = 1 - s.current;
    if (!refill(s.texture[next], s.done.data())) return false;
    s.current = next;
    return true;
}

void apply(unsigned char *state, const unsigned char *settings) {
    Shared &s = shared();
    std::lock_guard<std::mutex> hold(s.lock);
    unsigned char *sampled;
    std::memcpy(&sampled, state + 0x18, 8);
    const bool ours = sampled && (sampled == s.view[0] || sampled == s.view[1]);
    float lut[36];
    if (s.broken || !settings || *g_net_game == 0 || !ps3_grading_inputs(settings, lut)) {
        if (ours) std::memcpy(state + 0x18, state + 0x30, 8);   // the game's default table again
        return;
    }
    if (!s.asked_valid || std::memcmp(lut, s.asked, sizeof lut) != 0) {
        std::memcpy(s.asked, lut, sizeof lut);
        s.asked_valid = s.asked_new = true;
        if (!s.thread_started) {
            try {
                std::thread(bake_thread).detach();
            } catch (const std::system_error &) {
                s.broken = true;
                std::printf("Uncharted 3: PS3 colour grading: no bake thread; PS4 default table kept\n");
                std::fflush(stdout);
                return;
            }
            s.thread_started = true;
        }
        s.wake.notify_one();
    }
    if (s.done_new) {
        s.done_new = false;
        if (!use_done_table(s)) {
            s.broken = true;
            std::printf("Uncharted 3: PS3 colour grading: could not make or fill its texture; PS4 default table kept\n");
            std::fflush(stdout);
            if (ours) std::memcpy(state + 0x18, state + 0x30, 8);
            return;
        }
        if (s.reports < 40) {
            s.reports++;
            const float *v = s.done_values;
            std::printf("Uncharted 3: PS3 colour grading table baked (%.1f ms) for render settings %p: shoulder %.3f %.3f %.3f, "
                        "toe %.3f/%.3f, global hue/sat/lum %.3f %.3f %.3f\n",
                        s.done_ms, static_cast<const void *>(settings), v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8]);
            std::fflush(stdout);
        }
    }
    // until the first table is done the game's default (identity) table stays
    if (s.current >= 0) std::memcpy(state + 0x18, &s.view[s.current], 8);
}

// the game's update, then ours
__attribute__((sysv_abi)) void update_lut(unsigned char *state, const unsigned char *settings) {
    g_update(state, settings);
    if (state) apply(state, settings);
}

}  // namespace

bool ps3_grading_inputs(const unsigned char *settings, float lut[36]) {
    float enable;
    uint32_t ps4_tables;
    std::memcpy(&enable, settings + 0x740, 4);
    std::memcpy(&ps4_tables, settings + 0x750, 4);
    if (!(enable > 0.5f) || ps4_tables != 0) return false;
    lut[0] = enable;
    std::memcpy(lut + 1, settings + 0x760, 35 * 4);
    return true;   // all zero too: PS3 bakes those as well
}

void bake_ps3_grading_lut(const float lut[36], unsigned char *rgba) {
    std::fenv_t saved;
    std::fegetenv(&saved);
    bake_table(lut, rgba);
    std::fesetenv(&saved);
}

bool install_ps3_lut_grading(unsigned char *image, uint64_t size) {
    const char *env = std::getenv("U3_PS3_GRADING");
    if ((env && env[0] == '0') || user_switch("grading-ps4")) {
        std::printf("Uncharted 3: PS3 colour grading off (U3_PS3_GRADING=0 or grading-ps4): PS4 identity table\n");
        std::fflush(stdout);
        return false;
    }
    const uint64_t update = find_unique(image, size, kUpdate, sizeof kUpdate);
    const uint64_t made = find_unique(image, size, kDefault, sizeof kDefault);
    const uint64_t level = find_unique(image, size, kLevel, sizeof kLevel);
    const uint64_t copy = find_unique(image, size, kCopy, sizeof kCopy);
    if (size <= kNetGame || update == kNotFound || made == kNotFound || level == kNotFound || copy == kNotFound) {
        std::printf("Uncharted 3: PS3 colour grading: LUT update not found, not applied\n");
        std::fflush(stdout);
        return false;
    }
    g_net_game = image + kNetGame;
    g_make_texture = reinterpret_cast<MakeTextureFn>(image + rel_target(image, made + kMakeTextureCall));
    g_make_view = reinterpret_cast<MakeViewFn>(image + rel_target(image, made + kMakeViewCall));
    g_span = reinterpret_cast<SpanFn>(image + rel_target(image, level + kSpanCall));
    g_tiling = reinterpret_cast<TilingFn>(image + rel_target(image, level + kTilingCall));
    g_copy = reinterpret_cast<CopyFn>(image + rel_target(image, copy + kCopyCall));
    unsigned char *stub = stub_page_near(image, size);
    if (!stub) return false;
    // stub + 0: mov rax, update_lut; jmp rax. stub + 16: the function's first instructions, then jmp [rip] back to the
    // rest of it (the way the game's own update is called from update_lut).
    const uint64_t handler = reinterpret_cast<uint64_t>(&update_lut);
    stub[0] = 0x48; stub[1] = 0xb8;
    std::memcpy(stub + 2, &handler, 8);
    stub[10] = 0xff; stub[11] = 0xe0;
    unsigned char *original = stub + 16;
    std::memcpy(original, image + update, kMoved);
    original[kMoved] = 0xff; original[kMoved + 1] = 0x25;
    std::memset(original + kMoved + 2, 0, 4);
    const uint64_t back = reinterpret_cast<uint64_t>(image + update + kMoved);
    std::memcpy(original + kMoved + 6, &back, 8);
    g_update = reinterpret_cast<UpdateFn>(original);
    if (!finish_stub(stub) || !jump_to(image, update, kMoved, stub)) return false;
    std::printf("Uncharted 3: PS3 colour grading of the render settings in net games at the LUT update %#llx "
                "(U3_PS3_GRADING=0 turns it off)\n",
                static_cast<unsigned long long>(update));
    std::fflush(stdout);
    return true;
}

}  // namespace u3fix
