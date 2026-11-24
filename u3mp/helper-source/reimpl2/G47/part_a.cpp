// G47 Part A: segmented progress rings (marker ring, HUD counter ring), written from spec G47_hud.md.

extern "C" {
uint8_t *u3h_ext(void);
uint32_t u3h_sincos(uint32_t x, uint32_t cosine);
void u3h_ring_colour(uint32_t *col);
uint32_t *u3h_t009_diag(void);
uint32_t u3x_float_call(uintptr_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);  // G51 4.1

void u3x_marker_ring(uintptr_t b, uint32_t scale, uint32_t p, uint32_t depth, uint32_t x, uint32_t y,
                     const uint8_t *desc, const uint32_t *col);
int u3x_marker_ring_site(uintptr_t b, const uint64_t *r, int site);
void u3x_counter_ring(uintptr_t b, uint32_t scale, uint32_t p, uint32_t r1, uint32_t r2, uint32_t depth, uint32_t x,
                      uint32_t y, const uint8_t *desc, const uint32_t *col);
int u3x_counter_ring_site(uintptr_t b, const uint64_t *r);
}

namespace g47a {

constexpr uint32_t F_ZERO = 0;
constexpr uint32_t F_HALF = 0x3f000000u;
constexpr uint32_t F_TEN = 0x41200000u;         // quads of an empty ring at scale 1
constexpr uint32_t F_THIRTY = 0x41f00000u;      // quads of a full ring at scale 1
constexpr uint32_t F_TEX_RADIUS = 0x3ee66666u;  // 0.45, the sprite's outer radius in texture units
constexpr uint32_t F_TWO_PI = 0x40c90fdbu;
constexpr uint32_t F_HALF_PI = 0x3fc90fdbu;
constexpr uint32_t F_BYTE = 0x3b808081u;        // 1 / 255
constexpr int32_t MAX_QUADS = 512;

constexpr uintptr_t FN_TEXTURE = 0x11b89b0;
constexpr uintptr_t FN_QUAD = 0xe42310;
constexpr uintptr_t FN_PROGRESS = 0x745240;
constexpr uint32_t FUNCTION_START = 0xe5894855u;

// how many quads a ring of this scale and progress gets
U3X_FN int32_t quad_count(uint32_t scale, uint32_t p) {
    uint32_t q = p;
    if ((p & 0x80000000u) && p != 0x80000000u) q = F_ZERO;
    if (!f_less(f_sub(q, F_ONE), F_ZERO)) q = F_ONE;
    const uint32_t count = f_madd(f_sub(F_ONE, q), F_TEN, f_mul(q, F_THIRTY));
    const uint32_t m = f_less(f_sub(F_ONE, scale), F_ZERO) ? scale : F_ONE;
    return f_to_int(f_mul(m, count));
}

// one corner of a ring quad: on the circle of `radius` round (x, y), texture radius `tex`
[[gnu::always_inline]] inline void corner(uint32_t *v, uint32_t c, uint32_t s, uint32_t radius, uint32_t tex,
                                          uint32_t x, uint32_t y, const uint32_t *col, uint32_t alpha) {
    v[0] = f_madd(c, radius, x);
    v[1] = f_madd(s, radius, y);
    v[3] = f_madd(c, tex, F_HALF);
    v[4] = f_madd(s, tex, F_HALF);
    v[8] = col[0];
    v[9] = col[1];
    v[10] = col[2];
    v[11] = alpha;
    for (int i = 12; i < 16; ++i) v[i] = F_ONE;
}

// draws the ring as n quads; false when the sprite has no texture
U3X_FN bool draw_quads(uintptr_t b, uint32_t rin, uint32_t rout, uint32_t p, int32_t n, uint32_t depth, uint32_t x,
                       uint32_t y, const uint8_t *desc, const uint32_t *col) {
    const uint64_t texture = game_call<uint64_t>(b + FN_TEXTURE, rd<uint64_t>(desc, 0), rd<uint32_t>(desc, 8));
    if (!texture) return false;

    const uint32_t ratio = f_div(f_mul(rin, F_TEX_RADIUS), rout);
    const uint32_t sweep = f_mul(p, F_TWO_PI);
    const uint32_t inv = f_div(F_ONE, f_from_int(n));
    const uint32_t alpha = f_mul(col[3], at<uint32_t>(b + 0x16eafe0));   // always screen 0's alpha

    for (int32_t i = 0; i < n; ++i) {
        const uint32_t angle0 = f_madd(inv, f_mul(f_from_int(i), sweep), F_HALF_PI);
        const uint32_t angle1 = f_madd(inv, f_mul(f_from_int(i + 1), sweep), F_HALF_PI);
        const uint32_t c0 = u3h_sincos(angle0, 1);
        const uint32_t s0 = u3h_sincos(angle0, 0);
        const uint32_t c1 = u3h_sincos(angle1, 1);
        const uint32_t s1 = u3h_sincos(angle1, 0);

        alignas(16) uint32_t v[68];
        for (int k = 0; k < 68; ++k) v[k] = 0;
        corner(v, c0, s0, rin, ratio, x, y, col, alpha);
        corner(v + 16, c0, s0, rout, F_TEX_RADIUS, x, y, col, alpha);
        corner(v + 32, c1, s1, rout, F_TEX_RADIUS, x, y, col, alpha);
        corner(v + 48, c1, s1, rin, ratio, x, y, col, alpha);
        v[0x40] = depth;

        uint64_t context[6];
        for (int k = 0; k < 6; ++k) context[k] = 0;
        uint64_t tex[4];
        tex[0] = texture;
        for (int k = 1; k < 4; ++k) tex[k] = 0;
        game_call<void>(b + FN_QUAD, context, tex, v);
    }
    return true;
}

[[gnu::always_inline]] inline bool starts_as_function(uintptr_t b, uintptr_t offset) {
    return at<uint32_t>(b + offset) == FUNCTION_START;
}

}  // namespace g47a

extern "C" U3X_FN void u3x_marker_ring(uintptr_t b, uint32_t scale, uint32_t p, uint32_t depth, uint32_t x, uint32_t y,
                                       const uint8_t *desc, const uint32_t *col) {
    using namespace g47a;
    uint32_t *t9 = u3h_t009_diag();
    const int32_t n = quad_count(scale, p);
    t9[6] = static_cast<uint32_t>(n);
    t9[5] = p;
    t9[8] = scale;
    if (n <= 0) return;
    if (n > MAX_QUADS) { t9[7] += 1; return; }
    if (draw_quads(b, f_mul(scale, 0x41c00000u), f_mul(scale, 0x42040000u), p, n, depth, x, y, desc, col))
        t9[2] += 1;
    else
        t9[3] += 1;
}

extern "C" U3X_FN void u3x_counter_ring(uintptr_t b, uint32_t scale, uint32_t p, uint32_t r1, uint32_t r2,
                                        uint32_t depth, uint32_t x, uint32_t y, const uint8_t *desc,
                                        const uint32_t *col) {
    using namespace g47a;
    const int32_t n = quad_count(scale, p);
    if (n <= 0) return;
    uint32_t *eb = ext_eb();
    if (n > MAX_QUADS) { eb[212] += 1; return; }
    if (draw_quads(b, f_mul(scale, r1), f_mul(scale, r2), p, n, depth, x, y, desc, col))
        eb[211] += 1;
    else
        eb[212] += 1;   // "no texture" shares the refusal word
}

extern "C" U3X_FN int u3x_marker_ring_site(uintptr_t b, const uint64_t *r, int site) {
    using namespace g47a;
    uint32_t *t9 = u3h_t009_diag();
    t9[site] += 1;

    const uint8_t *frame = reinterpret_cast<const uint8_t *>(r[2]);
    const uint8_t *list = reinterpret_cast<const uint8_t *>(site == 0 ? r[0] : r[1]);
    if (!ptr_ok(frame) || !ptr_ok(list)) { t9[7] += 1; return 0; }

    const uint64_t k = site == 0 ? *reinterpret_cast<const uint64_t *>(frame - 0x1c0) : r[0];
    if (k >= 0x300 || k % 0x60) { t9[7] += 1; return 0; }

    const uint8_t *entry = list + 0x10 + k;
    const uint32_t p = rd<uint32_t>(entry, 0x44);
    if ((p & 0x80000000u) || (p & 0x7f800000u) == 0x7f800000u) return 0;   // negative, -0, infinity, NaN: no ring

    const uint32_t packed = rd<uint32_t>(entry, 0x34);
    t9[4] = packed;
    const uint32_t v3 = f_mul(f_from_int(static_cast<int32_t>(packed >> 24)), F_BYTE);
    const uint32_t v2 = f_mul(f_from_int(static_cast<int32_t>((packed >> 16) & 0xff)), F_BYTE);
    const uint32_t v1 = f_mul(f_from_int(static_cast<int32_t>((packed >> 8) & 0xff)), F_BYTE);
    const uint32_t v0 = f_mul(f_from_int(static_cast<int32_t>(packed & 0xff)), F_BYTE);

    uint32_t colour[4];
    const uint32_t *ec = reinterpret_cast<const uint32_t *>(u3h_ext() + 0x4450);
    if (ec[2] & 0x20) {   // build switch: the other byte order
        colour[0] = v3; colour[1] = v2; colour[2] = v1; colour[3] = v0;
    } else {              // the word is alpha, red, green, blue from the top
        colour[0] = v2; colour[1] = v1; colour[2] = v0; colour[3] = v3;
    }
    u3h_ring_colour(colour);

    const uint32_t scale = f_mul(*reinterpret_cast<const uint32_t *>(frame - 0x254), 0x3fe00000u);
    u3x_marker_ring(b, scale, p, site == 0 ? F_ZERO : F_ONE, *reinterpret_cast<const uint32_t *>(frame - 0x1a0),
                    *reinterpret_cast<const uint32_t *>(frame - 0x19c), list + 0x5d0, colour);
    return 0;
}

extern "C" U3X_FN int u3x_counter_ring_site(uintptr_t b, const uint64_t *r) {
    using namespace g47a;
    uint32_t *eb = ext_eb();
    eb[210] += 1;

    const uint8_t *object = reinterpret_cast<const uint8_t *>(r[4]);
    const uint8_t *frame = reinterpret_cast<const uint8_t *>(r[14]);
    if (!ptr_ok(object) || !ptr_ok(frame)) { eb[212] += 1; return 0; }

    const uint8_t *hud = *reinterpret_cast<const uint8_t *const *>(frame - 0x11a8);
    const uint8_t *style = rd<const uint8_t *>(object, 0x128);
    if (!ptr_ok(hud) || !ptr_ok(style)) { eb[212] += 1; return 0; }

    if (!starts_as_function(b, FN_PROGRESS) || !starts_as_function(b, FN_TEXTURE) || !starts_as_function(b, FN_QUAD)) {
        eb[212] += 1;
        return 0;
    }

    const uint32_t p = u3x_float_call(b + FN_PROGRESS, b + 0x17cb060, 0, 0, 0);
    eb[213] = p;

    uint32_t colour[4];
    for (int i = 0; i < 4; ++i) colour[i] = rd<uint32_t>(style, 0x18 + 4 * i);
    u3h_ring_colour(colour);

    u3x_counter_ring(b, 0x3ef33333u, p, 0x41f00000u, 0x42480000u, F_ZERO, rd<uint32_t>(object, 0x120),
                     rd<uint32_t>(object, 0x124), hud + 0x1a90 + 0x5d0, colour);
    return 0;
}
