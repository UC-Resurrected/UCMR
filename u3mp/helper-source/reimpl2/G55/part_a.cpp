// G55 part A: the gradient box behind a tab (menu item type 0x2d), written from spec G55 part A.

extern "C" uint32_t u3h_h13_mode(void);
extern "C" uint32_t *u3h_w03_diag(void);

extern "C" uint64_t u3x_gradient_texture(uintptr_t base);
extern "C" void u3x_box_quad(uintptr_t base, uint64_t context, uint64_t texture, const uint8_t *item, uint32_t x,
                             uint32_t y, uint32_t argb);
extern "C" int u3x_box_item(uintptr_t base, const uint64_t *r);

namespace g55a {

constexpr uintptr_t IMAGE_DIRECTORY = 0x1b24bc8;   // pointer to the UI image directory
constexpr uintptr_t FN_TEXTURE = 0x11b89b0;        // (key, index) -> texture handle
constexpr uintptr_t FN_QUAD = 0xe42310;            // quad draw
constexpr uint32_t GRADIENT_IMAGE = 0x6e9d9193u;
constexpr uint32_t FN_START = 0xe5894855u;         // push rbp; mov rbp, rsp
constexpr uint32_t TAB_BOX = 0x2d;

constexpr uint32_t F_255 = 0x437f0000u, F_INV255 = 0x3b808081u, F_TINY = 0x3a83126fu, F_DARK = 0x3e19999au,
                   F_HALF = 0x3f000000u;

// diagnostic slots: d[k] and ec[164 + k]
enum { SEEN, OFF, REFUSED, QUADS, NO_TEXTURE, LAST_COLOUR, LAST_POSITION, LAST_FLAGS };

[[gnu::always_inline]] inline bool usable4(uint64_t v) { return v >= 0x10000u && v < (1ull << 47) && !(v & 3); }
[[gnu::always_inline]] inline uint32_t *ec() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0x4450); }
[[gnu::always_inline]] inline void count(int k) {
    u3h_w03_diag()[k] += 1;
    ec()[164 + k] += 1;
}
[[gnu::always_inline]] inline void note(int k, uint32_t v) {
    u3h_w03_diag()[k] = v;
    ec()[164 + k] = v;
}

// a game function with up to three integer arguments, through the call block
U3X_FN uint64_t call(uintptr_t fn, uint64_t a, uint64_t b, uint64_t c) {
    GameCall g;
    uint64_t *q = reinterpret_cast<uint64_t *>(&g);
    for (uint32_t k = 0; k < sizeof(g) / 8; ++k) q[k] = 0;
    g.fn = fn;
    g.i[0] = a;
    g.i[1] = b;
    g.i[2] = c;
    return u3x_k26_call(&g);
}

// index of the gradient image in the class-0 image table, 0xffffffff when it cannot be found
U3X_FN uint32_t gradient_index(uintptr_t base) {
    const void *directory = at<const void *>(base + IMAGE_DIRECTORY);
    if (!ptr_ok(directory)) return 0xffffffffu;
    const void *table = rd<const void *>(directory, 0x20);
    if (!ptr_ok(table)) return 0xffffffffu;
    const uint32_t n = rd<uint16_t>(table, 2);
    if (n < 1 || n > 4095) return 0xffffffffu;
    const void *entries = rd<const void *>(table, 8);
    if (!ptr_ok(entries)) return 0xffffffffu;
    for (uint32_t k = 0; k < n; ++k)
        if (rd<uint32_t>(entries, k * 0x28 + 8) == GRADIENT_IMAGE) return k;
    return 0xffffffffu;
}

// one colour byte as a 0..1 float word
[[gnu::always_inline]] inline uint32_t unit(uint32_t colour, int shift) {
    return f_mul(f_from_int(static_cast<int32_t>((colour >> shift) & 0xff)), F_INV255);
}

// where an item of size `size` goes in a span, by alignment (0 start, 2 end, else centre)
[[gnu::always_inline]] inline uint32_t place(int32_t align, uint32_t start, uint32_t span, uint32_t size) {
    if (align == 2) return f_sub(f_add(start, span), size);
    if (align == 0) return start;
    return f_add(f_mul(f_sub(span, size), F_HALF), start);
}

[[gnu::always_inline]] inline uint32_t byte_of(uint32_t f) { return static_cast<uint32_t>(f_to_int(f_mul(f, F_255))); }

}  // namespace g55a

U3X_FN uint64_t u3x_gradient_texture(uintptr_t base) {
    using namespace g55a;
    return call(base + FN_TEXTURE, GRADIENT_IMAGE, gradient_index(base), 0);
}

U3X_FN void u3x_box_quad(uintptr_t base, uint64_t context, uint64_t texture, const uint8_t *item, uint32_t x,
                         uint32_t y, uint32_t argb) {
    using namespace g55a;
    const uint32_t flags = rd<uint32_t>(item, 0x10);
    const uint32_t tint = rd<uint32_t>(item, 0x24);

    // the colour times the item colour, byte by byte (product >> 8)
    uint32_t colour = 0;
    for (int shift = 0; shift < 32; shift += 8)
        colour |= ((((argb >> shift) & 0xff) * ((tint >> shift) & 0xff)) >> 8) << shift;
    uint32_t rgba[4] = {unit(colour, 16), unit(colour, 8), unit(colour, 0), unit(colour, 24)};

    const uint32_t x1 = f_add(x, rd<uint32_t>(item, 0x3c));
    const uint32_t y1 = f_add(y, rd<uint32_t>(item, 0x40));
    const uint32_t turn = (flags | (flags >> 24)) & 3;

    alignas(16) uint64_t context_block[6];
    alignas(16) uint64_t texture_block[4];
    alignas(16) uint32_t vertices[68];
    for (int k = 0; k < 6; ++k) context_block[k] = 0;
    for (int k = 0; k < 4; ++k) texture_block[k] = 0;
    for (int k = 0; k < 68; ++k) vertices[k] = 0;
    context_block[0] = context;
    texture_block[0] = texture;

    if (u3h_h13_mode() == 3) {   // experiment: opaque magenta, no texture
        rgba[0] = F_ONE;
        rgba[1] = 0;
        rgba[2] = F_ONE;
        rgba[3] = F_ONE;
        texture_block[0] = 0;
    }

    // corners clockwise from the top left; the texture's unit square in the same order, turned by `turn`
    for (uint32_t k = 0; k < 4; ++k) {
        uint32_t *v = vertices + 16 * k;
        const uint32_t t = (turn + k) & 3;
        v[0] = ((k + 1) & 2) ? x1 : x;
        v[1] = (k & 2) ? y1 : y;
        v[3] = ((t + 1) & 2) ? F_ONE : 0;
        v[4] = (t & 2) ? F_ONE : 0;
        for (int c = 0; c < 4; ++c) {
            v[8 + c] = rgba[c];
            v[12 + c] = F_ONE;
        }
    }

    call(base + FN_QUAD, reinterpret_cast<uint64_t>(context_block), reinterpret_cast<uint64_t>(texture_block),
         reinterpret_cast<uint64_t>(vertices));

    count(QUADS);
    note(LAST_COLOUR, colour);
    note(LAST_POSITION, (static_cast<uint32_t>(f_to_int(x)) & 0xffff) | (static_cast<uint32_t>(f_to_int(y)) << 16));
    note(LAST_FLAGS, flags);
}

U3X_FN int u3x_box_item(uintptr_t base, const uint64_t *r) {
    using namespace g55a;
    const uint64_t item_address = r[7];
    if (!usable4(item_address)) return 0;
    const uint8_t *item = reinterpret_cast<const uint8_t *>(item_address);
    if (rd<uint32_t>(item, 0) != TAB_BOX) return 0;
    count(SEEN);

    if (!(ext_ea()[240] & 4)) {
        count(OFF);
        return 0;
    }
    if (!usable4(r[6]) || !usable4(r[3])) {
        count(REFUSED);
        return 0;
    }
    const uint32_t *style = reinterpret_cast<const uint32_t *>(r[6]);
    const uint32_t *rect = reinterpret_cast<const uint32_t *>(r[3]);
    if (at<uint32_t>(base + FN_QUAD) != FN_START || at<uint32_t>(base + FN_TEXTURE) != FN_START) {
        count(REFUSED);
        return 0;
    }

    const uint32_t alpha = style[4];
    const uint32_t alpha_byte = byte_of(alpha);
    if (!(alpha_byte & 0xff) || f_less(style[0], F_TINY)) return 1;   // invisible: nothing to draw

    // style[1..3] and style[5] are read where they are used (the box colour again after the mode calls)
    const bool dark = f_less(style[1], F_DARK) && f_less(style[2], F_DARK) && f_less(style[3], F_DARK);
    const uint32_t dim = dark ? F_HALF : F_ONE;

    uint32_t x = place(static_cast<int32_t>(r[2]), rect[0], rect[2], rd<uint32_t>(item, 0x3c));
    uint32_t y = place(static_cast<int32_t>(r[11]), rect[1], rect[3], rd<uint32_t>(item, 0x40));
    x = f_add(x, style[8]);
    y = f_add(y, style[9]);

    const uint64_t texture = u3x_gradient_texture(base);
    if (!texture) {
        count(NO_TEXTURE);
        return 1;
    }

    uint64_t context = r[5];
    if (u3h_h13_mode() == 1) context = 0;   // experiment: the queued UI pass
    ec()[175] = context ? 1 : 0;

    const uint32_t shadow = style[5];
    if (f_less(F_TINY, shadow)) {   // drop shadow
        uint32_t strength = f_mul(shadow, F_HALF);
        if (f_less(F_ONE, strength)) strength = F_ONE;
        const uint32_t a = byte_of(f_mul(f_mul(f_mul(dim, strength), alpha), F_HALF));
        const uint32_t drop = f_from_int(f_to_int(f_add(shadow, F_HALF)));
        u3x_box_quad(base, context, texture, item, x, f_add(y, drop), a << 24);
    }

    const uint32_t red = style[1], green = style[2], blue = style[3];
    const uint32_t colour = ((byte_of(red) & 0xff) << 16) | ((byte_of(green) & 0xff) << 8) | (byte_of(blue) & 0xff) |
                            (alpha_byte << 24);
    u3x_box_quad(base, context, texture, item, x, y, colour);
    return 1;
}
