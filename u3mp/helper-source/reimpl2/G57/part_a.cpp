// G57 part A: the emblem compositor (written from spec G57_emblem_compositor.md, sections 4.1 to 4.5).
// u3x_emb_render plans and draws the four layers of the manager's current emblem into a pixel buffer and makes it
// the texture of one emblem record; u3x_emb_pass runs it for the two record slots.

// ---- other groups and the C part (spec 2.2)
extern "C" uint8_t *u3h_emb_h(uintptr_t b, void *s, uintptr_t handle);
extern "C" uint8_t *u3h_emb_cache(uintptr_t b, const char *name);
extern "C" void *u3h_emb_alloc(uintptr_t b, uint64_t n);
extern "C" void u3h_emb_free(uintptr_t b, void *p);
extern "C" int u3h_emb_dds(uintptr_t b, const char *name, void *dds);
extern "C" void u3h_emb_quad(uint8_t *pix, uint32_t size, const void *dds, const uint32_t *uv, const void *mask_dds, const uint32_t *c16);
extern "C" void u3h_emb_decal(uint8_t *pix, uint32_t size, const void *dds, const uint32_t *q, const uint32_t *c16);
extern "C" int u3h_emb_texcreate(uintptr_t b, void *rec, const uint8_t *pix, uint32_t size);
extern "C" uint32_t u3h_sincos(uint32_t x, uint32_t cosine);
extern "C" uint32_t u3h_h16q16(uint32_t f);
extern "C" int u3h_emb_is_record(uint64_t p);
extern "C" uint8_t *u3h_emb_records(uint8_t **flag);
extern "C" uint8_t *u3x_emb_rec_slot(uintptr_t b, uint32_t sl);
extern "C" uint32_t u3x_emb_size(uintptr_t b);

// ---- delivered here
extern "C" void u3x_emb_render(uintptr_t b, void *s, uint32_t sl, void *rec, uint32_t size);
extern "C" void u3x_emb_pass(uintptr_t b, void *s);
extern "C" void u3x_emb_layer_uv(uint32_t *uv, uint32_t s, uint32_t cs, uint32_t sn);
extern "C" void u3x_emb_drip_quad(uint32_t *q, const uint8_t *point, uint32_t grid, uint32_t s, uint32_t cs, uint32_t sn, uint32_t half);

namespace g57a {

constexpr uintptr_t MANAGER = 0x17c3440;        // the emblem manager
constexpr uintptr_t LAYER_TEXTURE = 0x66bc30;   // game: texture info of layer n
constexpr uintptr_t H_ICONS = 0x1b67e60;        // + n * 0x18
constexpr uintptr_t H_PALETTE = 0x1b67ec0;
constexpr uintptr_t H_RANGE = 0x1b67ee0;        // + n * 0x18

constexpr uint32_t F_HALF = 0x3f000000u;
constexpr uint32_t F_MINUS_HALF = 0xbf000000u;
constexpr uint32_t F_QUARTER = 0x3e800000u;
constexpr uint32_t F_EIGHTH = 0x3e000000u;
constexpr uint32_t F_SIXTEENTH = 0x3d800000u;
constexpr uint32_t F_BYTE_STEP = 0x3b808081u;    // 1/255
constexpr uint32_t F_ANGLE_STEP = 0x3cc9d9b5u;   // 2 pi / 255

constexpr int KEY_BYTES = 40;

U3X_RO const char mask_name[] = {'e','m','b','l','e','m','s','/','s','p','r','a','y','-','0','1',0};
U3X_RO const char drips_name[] = {'e','m','b','l','e','m','s','/','e','m','b','l','e','m','-','1','6','-','d','r','i','p','s',0};

// G56's decoded-texture block (88 bytes); only the file pointer is ours to look at
struct Decoded {
    void *file;
    uint64_t rest[10];
};

// what planning keeps of one layer
struct Layer {
    uint32_t red, green, blue;   // palette entry, f32 patterns
    uint32_t scale, angle;       // f32 patterns
    const uint8_t *data;         // where this layer's 8 bytes were read (chosen once, at the start of its planning)
    uint8_t *texture;            // texture info block of the layer's icon, or null
    const uint8_t *decals;       // the icon's decal set, or null
    uint32_t plan;               // 0 not drawn, 1 drawn, 3 drawn with drips
};

[[gnu::always_inline]] inline uint32_t *ec() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0x4450); }

[[gnu::always_inline]] inline bool loaded(const uint8_t *info) { return info && rd<uint8_t>(info, 0x10) != 0; }

// the 8 bytes of layer n: the editor's when the player index is negative, else the rendered player's
[[gnu::always_inline]] inline const uint8_t *layer_data(const uint8_t *m, uint32_t n) {
    return m + (rd<int32_t>(m, 0x60) < 0 ? 8 : 0x2c) + 8 * n;
}

// a byte of the layer data as an index into a counted game list (0 without a list or with an empty one)
[[gnu::always_inline]] inline uint32_t pick(const uint8_t *list, uint8_t value) {
    if (!list) return 0;
    const int32_t count = rd<int32_t>(list, 0);
    return count > 0 ? value % static_cast<uint32_t>(count) : 0;
}

// spec 4.1: one layer, the game is asked in the order icon list, palette, range, layer texture, mask, drips
U3X_FN void plan_layer(uintptr_t b, void *s, uint32_t sl, uint32_t n, Layer &l, uint8_t *&mask_info, uint8_t *&drips_info) {
    uint8_t *m = reinterpret_cast<uint8_t *>(b + MANAGER);
    const uint8_t *d = layer_data(m, n);
    l.data = d;

    const uint8_t *icons = u3h_emb_h(b, s, H_ICONS + n * 0x18);
    const uint32_t icon = pick(icons, d[0]);

    const uint8_t *palette = u3h_emb_h(b, s, H_PALETTE);
    const uint32_t colour = pick(palette, d[4]);
    const uint8_t *entries = palette ? rd<const uint8_t *>(palette, 8) : nullptr;
    l.red = entries ? rd<uint32_t>(entries, colour * 12) : 0;
    l.green = entries ? rd<uint32_t>(entries, colour * 12 + 4) : 0;
    l.blue = entries ? rd<uint32_t>(entries, colour * 12 + 8) : 0;

    const uint8_t *range = u3h_emb_h(b, s, H_RANGE + n * 0x18);
    const uint32_t lo = range ? rd<uint32_t>(range, 0) : 0;
    const uint32_t hi = range ? rd<uint32_t>(range, 4) : 0;
    l.scale = f_madd(f_sub(hi, lo), f_mul(f_from_int(d[2]), F_BYTE_STEP), lo);
    l.angle = f_mul(f_from_int(d[1]), F_ANGLE_STEP);

    l.texture = game_call<uint8_t *>(b + LAYER_TEXTURE, m, static_cast<int32_t>(n));
    mask_info = u3h_emb_cache(b, mask_name);

    l.plan = 0;
    l.decals = nullptr;
    if (icon == 0) return;
    if (!loaded(l.texture) || (sl == 1 && !loaded(mask_info))) {
        m[2] = 1;
        return;
    }
    l.plan = 1;
    const uint8_t *icon_entries = rd<const uint8_t *>(icons, 8);
    if (icon_entries) l.decals = rd<const uint8_t *>(icon_entries, icon * 0x18 + 0x10);
    drips_info = u3h_emb_cache(b, drips_name);
    if (sl == 1 && l.decals) {
        if (loaded(drips_info)) l.plan = 3;
        else m[2] = 1;
    }
}

// spec 4.2 "Layers": one planned layer into the pixels; false when one of its files could not be loaded
U3X_FN bool draw_layer(uintptr_t b, uint32_t sl, uint8_t *pix, uint32_t size, const Layer &l, Decoded &mask, Decoded &drips,
                       const uint8_t *mask_info, const uint8_t *drips_info) {
    Decoded t;
    t.file = nullptr;
    if (!u3h_emb_dds(b, reinterpret_cast<const char *>(l.texture + 0x20), &t)) return false;
    if (sl == 1 && !mask.file && !u3h_emb_dds(b, reinterpret_cast<const char *>(mask_info + 0x20), &mask)) {
        u3h_emb_free(b, t.file);
        return false;
    }

    const uint32_t cs = u3h_sincos(l.angle, 1);
    const uint32_t sn = u3h_sincos(l.angle, 0);
    uint32_t uv[8];
    u3x_emb_layer_uv(uv, l.scale, cs, sn);
    uint32_t c16[3];
    c16[0] = u3h_h16q16(l.blue);
    c16[1] = u3h_h16q16(l.green);
    c16[2] = u3h_h16q16(l.red);
    u3h_emb_quad(pix, size, &t, uv, sl == 1 ? &mask : nullptr, c16);
    u3h_emb_free(b, t.file);

    if (!(l.plan & 2)) return true;
    if (!drips.file && !u3h_emb_dds(b, reinterpret_cast<const char *>(drips_info + 0x20), &drips)) return false;
    const int32_t count = rd<int32_t>(l.decals, 4);
    const uint8_t *points = rd<const uint8_t *>(l.decals, 8);
    const uint32_t grid = f_from_int(rd<int32_t>(l.decals, 0));
    const uint32_t half = f_madd(l.scale, F_SIXTEENTH, F_SIXTEENTH);
    if (points) {
        for (int32_t k = 0; k < count; ++k) {
            uint32_t q[8];
            u3x_emb_drip_quad(q, points + static_cast<int64_t>(k) * 12, grid, l.scale, cs, sn, half);
            u3h_emb_decal(pix, size, &drips, q, c16);
        }
    }
    return true;
}

// the test aid: exchange row y with row size - 1 - y
U3X_FN void flip_rows(uint8_t *pix, uint32_t size) {
    const uint64_t row = static_cast<uint64_t>(size) * 4;
    for (uint32_t y = 0; y < size / 2; ++y) {
        uint8_t *top = pix + y * row;
        uint8_t *bottom = pix + (size - 1 - y) * row;
        for (uint64_t i = 0; i < row; ++i) {
            const uint8_t keep = top[i];
            top[i] = bottom[i];
            bottom[i] = keep;
        }
    }
}

}  // namespace g57a

// spec 4.3: the texture coordinates of the target's corners for a picture turned by the angle and shrunk by the scale
extern "C" U3X_FN void u3x_emb_layer_uv(uint32_t *uv, uint32_t s, uint32_t cs, uint32_t sn) {
    using namespace g57a;
    const uint32_t pos = f_div(F_HALF, s);
    const uint32_t neg = f_div(F_MINUS_HALF, s);   // its own division
    const uint32_t pc = f_mul(pos, cs), ps = f_mul(pos, sn);
    const uint32_t ns = f_mul(neg, sn), nc = f_mul(neg, cs);
    const uint32_t pc_h = f_add(pc, F_HALF), ps_h = f_add(ps, F_HALF);
    const uint32_t ns_h = f_add(ns, F_HALF), nc_h = f_add(nc, F_HALF);
    uv[0] = f_add(ps, nc_h);
    uv[1] = f_sub(F_ONE, f_add(nc, ns_h));
    uv[2] = f_add(ns, nc_h);
    uv[3] = f_sub(F_ONE, f_add(pc, ns_h));
    uv[4] = f_add(ns, pc_h);
    uv[5] = f_sub(F_ONE, f_add(pc, ps_h));
    uv[6] = f_add(ps, pc_h);
    uv[7] = f_sub(F_ONE, f_add(nc, ps_h));
}

// spec 4.4: where one drip goes on the target (q[0..3]) and which picture of the 4 x 4 drips texture it shows (q[4..7])
extern "C" U3X_FN void u3x_emb_drip_quad(uint32_t *q, const uint8_t *point, uint32_t grid, uint32_t s, uint32_t cs, uint32_t sn, uint32_t half) {
    using namespace g57a;
    const uint32_t cell = rd<uint32_t>(point, 0);
    const uint32_t uc = f_madd(f_from_int(static_cast<int32_t>(cell & 3)), F_QUARTER, F_EIGHTH);
    const uint32_t vc = f_madd(f_from_int(static_cast<int32_t>(3u - (cell >> 2))), F_QUARTER, F_EIGHTH);

    // the point in -1 .. 1, scaled
    uint32_t yn = f_div(f_from_int(rd<int32_t>(point, 8)), grid);
    yn = f_sub(f_add(yn, yn), F_ONE);
    uint32_t xn = f_div(f_from_int(rd<int32_t>(point, 4)), grid);
    xn = f_sub(f_add(xn, xn), F_ONE);
    const uint32_t ys = f_mul(yn, s);
    const uint32_t xs = f_mul(xn, s);

    // turned, then moved to 0 .. 1
    uint32_t yr = f_madd(xs, sn, f_mul(ys, cs));
    uint32_t xr = f_madd(xs, cs, f_mul(ys, sn) ^ 0x80000000u);
    yr = f_madd(yr, F_HALF, F_HALF);
    xr = f_madd(xr, F_HALF, F_HALF);

    q[0] = f_sub(xr, half);
    q[1] = f_add(xr, half);
    q[2] = f_sub(yr, half);
    q[3] = f_add(yr, half);
    q[4] = f_sub(uc, F_EIGHTH);
    q[5] = f_add(uc, F_EIGHTH);
    q[6] = f_sub(F_ONE, f_add(vc, F_EIGHTH));
    q[7] = f_sub(F_ONE, f_sub(vc, F_EIGHTH));
}

// spec 4.1 and 4.2
extern "C" U3X_FN void u3x_emb_render(uintptr_t b, void *s, uint32_t sl, void *rec, uint32_t size) {
    using namespace g57a;
    uint8_t *m = reinterpret_cast<uint8_t *>(b + MANAGER);
    uint8_t *record = static_cast<uint8_t *>(rec);
    if (sl == 0) m[2] = 0;

    Layer layers[4];
    uint8_t *mask_info = nullptr, *drips_info = nullptr;
    for (uint32_t n = 0; n < 4; ++n) plan_layer(b, s, sl, n, layers[n], mask_info, drips_info);

    // the key: what this composite is made of
    uint8_t key[KEY_BYTES];
    for (uint32_t n = 0; n < 4; ++n) {
        const uint8_t *d = layers[n].data;
        for (int i = 0; i < 8; ++i) key[n * 8 + i] = d[i];
    }
    key[32] = static_cast<uint8_t>(sl);
    key[33] = static_cast<uint8_t>(size >> 8);
    key[34] = static_cast<uint8_t>(layers[0].plan | layers[1].plan << 2 | layers[2].plan << 4 | layers[3].plan << 6);
    key[35] = 1;
    key[36] = key[37] = key[38] = key[39] = 0;

    if (rd<uint64_t>(record, 8) != 0) {
        bool same = true;
        for (int i = 0; i < KEY_BYTES; ++i) same = same && record[0x28 + i] == key[i];
        if (same) {
            ec()[124] += 1;
            return;
        }
    }

    const uint64_t bytes = static_cast<uint64_t>(size) * size * 4;
    uint8_t *pix = static_cast<uint8_t *>(u3h_emb_alloc(b, bytes));
    if (!pix) {
        ec()[122] = 4;
        m[2] = 1;
        return;
    }
    for (uint64_t i = 0; i < bytes; ++i) pix[i] = 0;

    Decoded mask, drips;
    mask.file = nullptr;
    drips.file = nullptr;
    bool complete = true;
    for (uint32_t n = 0; n < 4; ++n) {
        if (!(layers[n].plan & 1)) continue;
        if (!draw_layer(b, sl, pix, size, layers[n], mask, drips, mask_info, drips_info)) {
            m[2] = 1;
            complete = false;
        }
    }
    if (mask.file) u3h_emb_free(b, mask.file);
    if (drips.file) u3h_emb_free(b, drips.file);

    if (ec()[0] & 0x8000u) flip_rows(pix, size);

    uint32_t covered = 0;
    for (uint64_t i = 3; i < bytes; i += 4) covered += pix[i] != 0;
    ec()[149 + (sl & 1)] = covered;

    if (u3h_emb_texcreate(b, record, pix, size)) {
        for (int i = 0; i < KEY_BYTES; ++i) record[0x28 + i] = complete ? key[i] : 0;
        ec()[113] += 1;
        ec()[125] = size;
    } else {
        m[2] = 1;
    }
    u3h_emb_free(b, pix);
}

// spec 4.5: both record slots, while a render is wanted
extern "C" U3X_FN void u3x_emb_pass(uintptr_t b, void *s) {
    using namespace g57a;
    uint8_t *flag = nullptr;
    u3h_emb_records(&flag);
    if (!*flag) return;
    for (uint32_t sl = 0; sl < 2; ++sl) {
        uint8_t *r = u3x_emb_rec_slot(b, sl);
        if (!r || !u3h_emb_is_record(reinterpret_cast<uint64_t>(r))) continue;
        const uint32_t n = u3x_emb_size(b);
        if (n == 0) continue;
        u3x_emb_render(b, s, sl, r, n);
        if (rd<uint8_t>(reinterpret_cast<const uint8_t *>(b + MANAGER), 2) != 0) ec()[114] += 1;
    }
}
