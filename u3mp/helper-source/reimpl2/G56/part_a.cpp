// G56 part A: emblem maths and CPU rasterizer (written from spec G56_emblem_raster.md, sections 0, 3.3, 3.4, 3.10,
// 4.1 to 4.4, 4.14 to 4.21). Pure: no game call; the only stored state is the six sine / cosine coefficient copies.
// Floats are 32-bit patterns, one f_* operation = one rounding to single precision.

extern "C" {
uint32_t u3x_sincos119(uint32_t x, uint32_t cosine);
void u3x_sincos_bind(uintptr_t b);
int64_t u3x_fq32(uint32_t f);
int32_t u3x_log2q8(uint32_t f);
uint32_t u3x_h16q16(uint32_t f);
uint32_t u3x_emb_dxt(const void *dds, uint32_t level, int32_t x, int32_t y);
void u3x_emb_lod(const void *dds, uint32_t rho2, void *lod);
uint32_t u3x_emb_bil(const void *dds, uint32_t level, int64_t U, int64_t V);
uint64_t u3x_emb_a32(const void *dds, const void *lod, int64_t U, int64_t V);
void u3x_emb_blend(uint8_t *px, const uint32_t *c16, uint64_t a32);
uint32_t u3x_emb_rho2(uint32_t du, uint32_t dv, uint32_t tw, uint32_t th, uint32_t size);
uint32_t u3x_emb_fmax(uint32_t a, uint32_t b);
void u3x_emb_quad(uint8_t *pix, uint32_t size, const void *dds, const uint32_t *uv, const void *mask,
                  const uint32_t *c16);
void u3x_emb_decal(uint8_t *pix, uint32_t size, const void *dds, const uint32_t *q, const uint32_t *c16);
}

namespace g56a {

// ---- sine / cosine constants (generating rule beside each)
constexpr uint32_t SC_K = 0x3f22f983u;      // 2 / pi, nearest single
constexpr uint32_t SC_H = 0xbfc90fdau;      // -(pi / 2 truncated to 24 bits)
constexpr uint32_t SC_W = 0xb3a22169u;      // -(pi / 2 - the truncated part), nearest single
constexpr uint32_t SC_HALF = 0x3f000000u;   // 0.5
constexpr uint32_t SC_T = 0x39800000u;      // 2^-12
constexpr uint32_t SIGN = 0x80000000u;
constexpr uint32_t QUIET_NAN = 0x7fc00000u;
constexpr uint32_t INF = 0x7f800000u;

// the game's vector sine / cosine constant block: six rows of 16 bytes, first f32 of each row
constexpr uintptr_t COEF_BLOCK = 0x13f0760u;
enum { C2, C1, S2, S1, C0, S0, COEF_COUNT };   // row order in the game image
U3X_BSS uint32_t coef[COEF_COUNT];

// ---- decoded-texture block (spec 3.3) and level triple (spec 3.4)
constexpr uintptr_t DDS_FILE = 0x00, DDS_W = 0x08, DDS_H = 0x0c, DDS_LAST = 0x10, DDS_BS = 0x14, DDS_OFF = 0x18;
struct Lod { uint32_t l0, l1, f; };

constexpr uint64_t ONE32 = 1ull << 32;   // coverage 1.0

// edge length of a level in texels, at least 1
[[gnu::always_inline]] inline int32_t level_len(uint32_t len0, uint32_t level) {
    const int32_t n = static_cast<int32_t>(len0 >> level);
    return n < 1 ? 1 : n;
}

[[gnu::always_inline]] inline int32_t clamp_texel(int32_t v, int32_t len) {
    if (v < 0) return 0;
    return v > len - 1 ? len - 1 : v;
}

// 5-bit red of a 565 colour as 8 bits
[[gnu::always_inline]] inline uint32_t red8(uint32_t c565) {
    const uint32_t r = c565 >> 11;
    return (r << 3) | (r >> 2);
}

// a * b in 64 bits, wrapping, as a signed value
[[gnu::always_inline]] inline int64_t mul_wrap(int64_t a, int64_t b) {
    return static_cast<int64_t>(static_cast<uint64_t>(a) * static_cast<uint64_t>(b));
}
[[gnu::always_inline]] inline int64_t add_wrap(int64_t a, int64_t b) {
    return static_cast<int64_t>(static_cast<uint64_t>(a) + static_cast<uint64_t>(b));
}
[[gnu::always_inline]] inline int64_t sub_wrap(int64_t a, int64_t b) {
    return static_cast<int64_t>(static_cast<uint64_t>(a) - static_cast<uint64_t>(b));
}

// centre of pixel p in Q32 pixel units: (2p + 1) / 2
[[gnu::always_inline]] inline int64_t pixel_centre(uint32_t p) {
    return static_cast<int64_t>(static_cast<uint64_t>(2u * p + 1u) << 31);
}

}  // namespace g56a

// ---- 4.1 sine / cosine the way PS3 1.19 computes it

extern "C" U3X_FN void u3x_sincos_bind(uintptr_t b) {
    for (uint32_t i = 0; i < g56a::COEF_COUNT; ++i) g56a::coef[i] = at<uint32_t>(b + g56a::COEF_BLOCK + 0x10u * i);
}

extern "C" U3X_FN uint32_t u3x_sincos119(uint32_t x, uint32_t cosine) {
    using namespace g56a;
    const uint32_t mag = x & ~SIGN;
    if (mag > INF) return QUIET_NAN;
    if (mag == INF) return x;
    if (mag == 0) return cosine ? F_ONE : x;

    // nearest quadrant, half away from zero
    uint32_t t = f_mul(x, SC_K);
    t = f_less(0, t) ? f_add(t, SC_HALF) : f_sub(t, SC_HALF);
    const int32_t n = f_to_int(t);
    const uint32_t q = (static_cast<uint32_t>(n) & 3u) + cosine;
    const uint32_t nf = f_from_int(n);

    // reduced argument: x - n * pi / 2 in two parts
    uint32_t r = f_madd(nf, SC_H, x);
    r = f_madd(nf, SC_W, r);

    if (f_less(SC_T | SIGN, r) && f_less(r, SC_T)) {
        if (q & 1u) r = F_ONE;
    } else {
        const uint32_t r2 = f_mul(r, r);
        if (q & 1u) {
            uint32_t p = f_madd(r2, coef[C2], coef[C1]);
            p = f_madd(r2, p, coef[C0]);
            r = f_madd(r2, p, F_ONE);
        } else {
            const uint32_t r3 = f_mul(r, r2);
            uint32_t p = f_madd(r2, coef[S2], coef[S1]);
            p = f_madd(r2, p, coef[S0]);
            r = f_madd(r3, p, r);
        }
    }
    return (q & 2u) ? r ^ SIGN : r;
}

// ---- 4.2 f32 to Q32, round half away from zero, capped shift (no saturation)

extern "C" U3X_FN int64_t u3x_fq32(uint32_t f) {
    const int32_t e = static_cast<int32_t>((f >> 23) & 0xffu);
    if (e == 0) return 0;
    const uint64_t m = (f & 0x7fffffu) | 0x800000u;
    int32_t k = e - 118;
    uint64_t magnitude;
    if (k >= 0) {
        if (k > 38) k = 38;
        magnitude = m << k;
    } else if (k < -40) {
        magnitude = 0;
    } else {
        magnitude = (m + (1ull << (-k - 1))) >> -k;
    }
    return static_cast<int64_t>((f & g56a::SIGN) ? 0 - magnitude : magnitude);
}

// ---- 4.3 256 * log2 f, rounded (sign and a zero exponent field ignored)

extern "C" U3X_FN int32_t u3x_log2q8(uint32_t f) {
    const int32_t exponent = static_cast<int32_t>((f >> 23) & 0xffu) - 127;
    uint64_t m = (f & 0x7fffffu) | 0x800000u;   // [1, 2) with 23 fraction bits
    uint32_t bits = 0;
    for (int i = 0; i < 9; ++i) {               // nine fraction bits of log2 m by repeated squaring
        m = (m * m) >> 23;
        bits <<= 1;
        if (m >= (1u << 24)) {
            bits |= 1u;
            m >>= 1;
        }
    }
    return 256 * exponent + static_cast<int32_t>((bits + 1u) >> 1);
}

// ---- 4.4 f32 through a 16-bit float, as value * 65536

extern "C" U3X_FN uint32_t u3x_h16q16(uint32_t f) {
    if ((f & g56a::SIGN) || f == 0) return 0;
    const int32_t exponent = static_cast<int32_t>(f >> 23) - 127;
    if (exponent < -25) return 0;
    if (exponent > 15) return 0x7fffffffu;

    // round the 24-bit mantissa to binary16's precision, ties to even
    const uint32_t m = (f & 0x7fffffu) | 0x800000u;
    const int32_t drop = 13 + (exponent < -14 ? -14 - exponent : 0);
    uint64_t kept = m >> drop;
    const uint32_t rest = m & ((1u << drop) - 1u);
    const uint32_t half = 1u << (drop - 1);
    if (rest > half || (rest == half && (kept & 1u))) ++kept;

    // kept has the weight 2^(exponent - 23 + drop); times 65536, floor
    const int32_t shift = exponent - 7 + drop;
    const uint64_t v = shift >= 0 ? kept << shift : kept >> -shift;
    return v > 0x7fffffffu ? 0x7fffffffu : static_cast<uint32_t>(v);
}

// ---- 4.14 one texel's red, 0..255

extern "C" U3X_FN uint32_t u3x_emb_dxt(const void *dds, uint32_t level, int32_t x, int32_t y) {
    using namespace g56a;
    const int32_t lw = level_len(rd<uint32_t>(dds, DDS_W), level);
    const int32_t lh = level_len(rd<uint32_t>(dds, DDS_H), level);
    x = clamp_texel(x, lw);
    y = clamp_texel(y, lh);

    const uint32_t bs = rd<uint32_t>(dds, DDS_BS);
    const uint32_t block = static_cast<uint32_t>(y >> 2) * static_cast<uint32_t>((lw + 3) >> 2) + static_cast<uint32_t>(x >> 2);
    const uint8_t *colour = rd<const uint8_t *>(dds, DDS_FILE) + rd<uint32_t>(dds, DDS_OFF + 4u * level) +
                            static_cast<uint64_t>(block) * bs + (bs == 16 ? 8 : 0);

    const uint32_t c0 = rd<uint16_t>(colour, 0), c1 = rd<uint16_t>(colour, 2);
    const uint32_t r0 = red8(c0), r1 = red8(c1);
    const uint32_t sel = (rd<uint32_t>(colour, 4) >> (2 * (4 * (y & 3) + (x & 3)))) & 3u;
    if (sel == 0) return r0;
    if (sel == 1) return r1;
    if (bs == 16 || c0 > c1) return sel == 2 ? (2 * r0 + r1) / 3 : (r0 + 2 * r1) / 3;
    return sel == 2 ? (r0 + r1) / 2 : 0;
}

// ---- 4.15 levels for a footprint

extern "C" U3X_FN void u3x_emb_lod(const void *dds, uint32_t rho2, void *lod) {
    using namespace g56a;
    Lod *out = static_cast<Lod *>(lod);
    out->l0 = 0;
    out->l1 = 0;
    out->f = 0;
    if ((rho2 & SIGN) || rho2 == 0) return;

    // log2 of texels per pixel in 1/256 steps, level bias -1
    const int32_t lam = ((u3x_log2q8(rho2) + 1) >> 1) - 256;
    if (lam <= 0) return;
    const uint32_t last = rd<uint32_t>(dds, DDS_LAST);
    uint32_t d = static_cast<uint32_t>(lam);
    if (d > last * 256u) d = last * 256u;
    out->l0 = d >> 8;
    out->f = d & 255u;
    out->l1 = out->l0 + 1;
    if (out->l1 > last) {
        out->l1 = last;
        out->f = 0;
    }
}

// ---- 4.16 bilinear red * 65536 at Q32 texture coordinates

extern "C" U3X_FN uint32_t u3x_emb_bil(const void *dds, uint32_t level, int64_t U, int64_t V) {
    using namespace g56a;
    const int64_t lw = level_len(rd<uint32_t>(dds, DDS_W), level);
    const int64_t lh = level_len(rd<uint32_t>(dds, DDS_H), level);
    const int64_t tu = sub_wrap(mul_wrap(U, lw), 1ll << 31);   // texel space, centres at half
    const int64_t tv = sub_wrap(mul_wrap(V, lh), 1ll << 31);
    const int32_t x = static_cast<int32_t>(tu >> 32), y = static_cast<int32_t>(tv >> 32);
    const uint32_t fx = static_cast<uint32_t>(tu >> 24) & 255u, fy = static_cast<uint32_t>(tv >> 24) & 255u;

    const uint32_t x1 = static_cast<uint32_t>(x) + 1u, y1 = static_cast<uint32_t>(y) + 1u;
    const uint32_t a = u3x_emb_dxt(dds, level, x, y);
    const uint32_t c = u3x_emb_dxt(dds, level, static_cast<int32_t>(x1), y);
    const uint32_t d = u3x_emb_dxt(dds, level, x, static_cast<int32_t>(y1));
    const uint32_t e = u3x_emb_dxt(dds, level, static_cast<int32_t>(x1), static_cast<int32_t>(y1));
    return (a * (256u - fx) + c * fx) * (256u - fy) + (d * (256u - fx) + e * fx) * fy;
}

// ---- 4.17 trilinear red as coverage, 0 .. 2^32

extern "C" U3X_FN uint64_t u3x_emb_a32(const void *dds, const void *lod, int64_t U, int64_t V) {
    const g56a::Lod *l = static_cast<const g56a::Lod *>(lod);
    const uint64_t r0 = u3x_emb_bil(dds, l->l0, U, V);
    uint64_t r;
    if (l->f == 0) {
        r = r0 * 256u;
    } else {
        const uint64_t r1 = u3x_emb_bil(dds, l->l1, U, V);
        r = r0 * (256u - l->f) + r1 * l->f;
    }
    return (r << 8) / 255u;
}

// ---- 4.18 source over on one B, G, R, A pixel (64-bit, wrapping)

extern "C" U3X_FN void u3x_emb_blend(uint8_t *px, const uint32_t *c16, uint64_t a32) {
    const uint64_t a = a32 > g56a::ONE32 ? g56a::ONE32 : a32;
    const uint64_t ia = g56a::ONE32 - a;
    const uint64_t round = 1ull << 47;
    for (int k = 0; k < 4; ++k) {
        const uint64_t source = k < 3 ? c16[k] : 65536u;   // alpha accumulates with source 1.0
        const uint64_t v = (255u * source * a + px[k] * 65536ull * ia + round) >> 48;
        px[k] = static_cast<uint8_t>(v > 255u ? 255u : v);
    }
}

// ---- 4.19 footprint of one target axis, and the float maximum

extern "C" U3X_FN uint32_t u3x_emb_rho2(uint32_t du, uint32_t dv, uint32_t tw, uint32_t th, uint32_t size) {
    const uint32_t fs = f_from_int(static_cast<int32_t>(size));
    const uint32_t a = f_div(f_mul(du, f_from_int(static_cast<int32_t>(tw))), fs);
    const uint32_t c = f_div(f_mul(dv, f_from_int(static_cast<int32_t>(th))), fs);
    return f_add(f_mul(a, a), f_mul(c, c));
}

extern "C" U3X_FN uint32_t u3x_emb_fmax(uint32_t a, uint32_t b) {
    return f_less(a, b) ? b : a;
}

// ---- 4.20 one rotated layer over the whole target

extern "C" U3X_FN void u3x_emb_quad(uint8_t *pix, uint32_t size, const void *dds, const uint32_t *uv, const void *mask,
                                    const uint32_t *c16) {
    using namespace g56a;
    // origin corner and the two edge vectors in Q32
    const int64_t u1 = u3x_fq32(uv[2]), v1 = u3x_fq32(uv[3]);
    const int64_t dux = sub_wrap(u3x_fq32(uv[4]), u1), dvx = sub_wrap(u3x_fq32(uv[5]), v1);
    const int64_t duy = sub_wrap(u3x_fq32(uv[0]), u1), dvy = sub_wrap(u3x_fq32(uv[1]), v1);

    // levels of the layer from the larger of the two axis footprints
    const uint32_t tw = rd<uint32_t>(dds, DDS_W), th = rd<uint32_t>(dds, DDS_H);
    const uint32_t rx = u3x_emb_rho2(f_sub(uv[4], uv[2]), f_sub(uv[5], uv[3]), tw, th, size);
    const uint32_t ry = u3x_emb_rho2(f_sub(uv[0], uv[2]), f_sub(uv[1], uv[3]), tw, th, size);
    Lod layer_lod;
    u3x_emb_lod(dds, u3x_emb_fmax(rx, ry), &layer_lod);

    // levels of the mask, stretched over the whole target
    Lod mask_lod;
    mask_lod.l0 = 0;
    mask_lod.l1 = 0;
    mask_lod.f = 0;
    if (mask) {
        const uint32_t k = f_div(f_from_int(static_cast<int32_t>(rd<uint32_t>(mask, DDS_W))),
                                 f_from_int(static_cast<int32_t>(size)));
        u3x_emb_lod(mask, f_mul(k, k), &mask_lod);
    }

    const int sh = size == 256 ? 9 : 8;   // 2^sh = 2 * size for the sizes in use; 8 for every other size
    if (size < 3) return;
    for (uint32_t py = 1; py <= size - 2; ++py) {
        for (uint32_t px = 1; px <= size - 2; ++px) {
            const int64_t ox = static_cast<int64_t>(static_cast<uint64_t>(2u * px + 1u));
            const int64_t oy = static_cast<int64_t>(static_cast<uint64_t>(2u * py + 1u));
            const int64_t u = add_wrap(u1, add_wrap(mul_wrap(dux, ox), mul_wrap(duy, oy)) >> sh);
            const int64_t v = add_wrap(v1, add_wrap(mul_wrap(dvx, ox), mul_wrap(dvy, oy)) >> sh);
            uint64_t a = u3x_emb_a32(dds, &layer_lod, u, v);
            if (mask) {
                const uint64_t m = u3x_emb_a32(mask, &mask_lod, ox << (32 - sh), oy << (32 - sh));
                a = static_cast<uint64_t>((static_cast<unsigned __int128>(a) * m) >> 32);
            }
            if (a) u3x_emb_blend(pix + (static_cast<uint64_t>(py) * size + px) * 4u, c16, a);
        }
    }
}

// ---- 4.21 one axis-aligned quad

extern "C" U3X_FN void u3x_emb_decal(uint8_t *pix, uint32_t size, const void *dds, const uint32_t *q,
                                     const uint32_t *c16) {
    using namespace g56a;
    // the quad in target pixels
    const uint32_t fs = f_from_int(static_cast<int32_t>(size));
    const uint32_t x0 = f_mul(q[0], fs), x1 = f_mul(q[1], fs), y0 = f_mul(q[2], fs), y1 = f_mul(q[3], fs);
    const uint32_t dx = f_sub(x1, x0), dy = f_sub(y1, y0);
    if (!(f_less(0, dx) && f_less(0, dy))) return;

    // levels from the larger of the two axis footprints
    const uint32_t du = f_sub(q[5], q[4]), dv = f_sub(q[7], q[6]);
    const uint32_t ru = f_div(f_mul(du, f_from_int(static_cast<int32_t>(rd<uint32_t>(dds, DDS_W)))), dx);
    const uint32_t rv = f_div(f_mul(dv, f_from_int(static_cast<int32_t>(rd<uint32_t>(dds, DDS_H)))), dy);
    Lod lod;
    u3x_emb_lod(dds, u3x_emb_fmax(f_mul(ru, ru), f_mul(rv, rv)), &lod);

    // bounds, start coordinates and gradients in Q32
    const int64_t qx0 = u3x_fq32(x0), qx1 = u3x_fq32(x1), qy0 = u3x_fq32(y0), qy1 = u3x_fq32(y1);
    const int64_t qu0 = u3x_fq32(q[4]), qv0 = u3x_fq32(q[6]);
    const int64_t gu = u3x_fq32(f_div(du, dx)), gv = u3x_fq32(f_div(dv, dy));

    if (size < 3) return;
    for (uint32_t py = 1; py <= size - 2; ++py) {
        const int64_t cy = pixel_centre(py);
        if (!(qy0 <= cy && cy < qy1)) continue;
        const int64_t v = add_wrap(qv0, mul_wrap(gv, sub_wrap(cy, qy0) >> 16) >> 16);
        for (uint32_t px = 1; px <= size - 2; ++px) {
            const int64_t cx = pixel_centre(px);
            if (!(qx0 <= cx && cx < qx1)) continue;
            const int64_t u = add_wrap(qu0, mul_wrap(gu, sub_wrap(cx, qx0) >> 16) >> 16);
            const uint64_t a = u3x_emb_a32(dds, &lod, u, v);
            if (a) u3x_emb_blend(pix + (static_cast<uint64_t>(py) * size + px) * 4u, c16, a);
        }
    }
}
