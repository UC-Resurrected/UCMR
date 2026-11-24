// G47 Part B: the ring and the icons of a HUD-net element (knocked down, grabbed, needs help), written from spec
// G47_hud.md.

extern "C" {
uint8_t *u3h_ext(void);
void u3h_ring_colour(uint32_t *col);
void u3x_counter_ring(uintptr_t b, uint32_t scale, uint32_t p, uint32_t r1, uint32_t r2, uint32_t depth, uint32_t x,
                      uint32_t y, const uint8_t *desc, const uint32_t *col);

void u3x_ring_colour(uintptr_t base, uint32_t *colour, uint32_t mode, uint32_t alpha, uint32_t progress, int32_t ticks);
void u3x_icon_quad(uint32_t *v, uint32_t x, uint32_t y, uint32_t half, uint32_t alpha, uint32_t depth);
void u3x_mode_icon(uintptr_t b, const uint8_t *el, const uint8_t *mod, uint32_t x, uint32_t y, uint32_t mode, Log log);
void u3x_grapple_icon(uintptr_t b, const uint8_t *el, const uint8_t *mod, uint32_t x, uint32_t y, uint32_t *diag,
                      void (*seen)(uintptr_t, const uint8_t *, uint32_t, uint64_t),
                      void (*drawn)(uintptr_t, uint32_t, uint32_t));
int u3x_hudnet_ring(uintptr_t b, const uint64_t *r,
                    void (*grapple)(uintptr_t, const uint8_t *, const uint8_t *, uint32_t, uint32_t), Log log);
}

namespace g47b {

constexpr uint32_t F_HALF = 0x3f000000u;
constexpr uintptr_t FN_TEXTURE = 0x11b89b0;
constexpr uintptr_t FN_QUAD = 0xe42310;
constexpr uint32_t FUNCTION_START = 0xe5894855u;

// module sprite descriptors
constexpr uintptr_t DESC_DOWN = 0x10b0;
constexpr uintptr_t DESC_GRAPPLE = 0x10bc;
constexpr uintptr_t DESC_RING = 0x10ec;

// the mode icon's counters: per mode (0..7) calls, drawn, no texture, line written; then lines written in all
struct IconState {
    uint32_t calls[8], drawn[8], no_texture[8], line_written[8], lines;
};
U3X_BSS IconState u3x_icons;

U3X_RO const char txt_head[] = "U3MP down: ";
U3X_RO const char txt_down[] = "knocked-down icon";
U3X_RO const char txt_mode[] = "icon of mode ";
U3X_RO const char txt_none[] = ": no material (the icon texture is not loaded), not drawn";
U3X_RO const char txt_slot[] = ": slot sid ";
U3X_RO const char txt_index[] = " index ";
U3X_RO const char txt_material[] = ", material ";
U3X_RO const char txt_drawn[] = ", drawn ";

[[gnu::always_inline]] inline uint64_t texture_of(uintptr_t b, const uint8_t *desc) {
    return game_call<uint64_t>(b + FN_TEXTURE, rd<uint64_t>(desc, 0), rd<uint32_t>(desc, 8));
}

// draws the icon texture as a square round (x, y); gives back its half size and alpha
U3X_FN void draw_icon(uintptr_t b, const uint8_t *el, uint64_t texture, uint32_t x, uint32_t y, uint32_t *half_out,
                      uint32_t *alpha_out) {
    const uint32_t half = f_mul(f_mul(rd<uint32_t>(el, 8), F_HALF), 0x42700000u);

    uint32_t screen = 0;
    const uint8_t *hud = at<const uint8_t *>(b + 0x1806480);
    if (ptr_ok(hud)) {
        screen = rd<uint32_t>(hud, 0x1d0);
        if (screen > 1) screen = 0;
    }
    const uint32_t alpha = f_mul(rd<uint32_t>(el, 0x3c), at<uint32_t>(b + 0x16eafe0 + 4 * screen));

    alignas(16) uint32_t v[68];
    u3x_icon_quad(v, x, y, half, alpha, rd<uint32_t>(el, 0x10));
    uint64_t context[6];
    for (int i = 0; i < 6; ++i) context[i] = 0;
    uint64_t tex[4];
    tex[0] = texture;
    for (int i = 1; i < 4; ++i) tex[i] = 0;
    game_call<void>(b + FN_QUAD, context, tex, v);

    *half_out = half;
    *alpha_out = alpha;
}

// one log line per mode at most, 20 in all
[[gnu::always_inline]] inline bool line_allowed(Log log, uint32_t m) {
    if (!log || u3x_icons.line_written[m] || u3x_icons.lines >= 20) return false;
    u3x_icons.line_written[m] = 1;
    u3x_icons.lines += 1;
    return true;
}

[[gnu::always_inline]] inline void subject(Line &l, uint32_t mode) {
    l.s(txt_head);
    if (mode == 1) l.s(txt_down);
    else l.s(txt_mode).d(mode);
}

}  // namespace g47b

extern "C" U3X_FN void u3x_ring_colour(uintptr_t base, uint32_t *colour, uint32_t mode, uint32_t alpha,
                                       uint32_t progress, int32_t ticks) {
    // the game's own colour vectors: red (knocked down), light blue (grabbed), blue (the rest)
    const uintptr_t source = base + (mode == 1 ? 0x1422250 : mode == 2 ? 0x1422240 : 0x1422230);
    for (int i = 0; i < 3; ++i) colour[i] = at<uint32_t>(source + 4 * i);
    colour[3] = alpha;

    if (f_less(progress, 0x3e800000u)) {   // below a quarter: blink white three times a second
        const uint32_t cycles = f_mul(f_mul(f_from_int(ticks), 0x3b5a740eu), 0x40400000u);
        const uint32_t part = f_sub(cycles, f_from_int(f_to_int(cycles)));
        if (f_less(part, g47b::F_HALF)) colour[0] = colour[1] = colour[2] = F_ONE;
    }
}

extern "C" U3X_FN void u3x_icon_quad(uint32_t *v, uint32_t x, uint32_t y, uint32_t half, uint32_t alpha,
                                     uint32_t depth) {
    for (int i = 0; i < 68; ++i) v[i] = 0;
    const uint32_t left = f_sub(x, half), right = f_add(x, half);
    const uint32_t top = f_sub(y, half), bottom = f_add(y, half);
    for (int corner = 0; corner < 4; ++corner) {
        uint32_t *c = v + 16 * corner;
        const bool on_right = corner == 1 || corner == 2;
        const bool on_bottom = corner >= 2;
        c[0] = on_right ? right : left;
        c[1] = on_bottom ? bottom : top;
        c[3] = on_right ? F_ONE : 0;
        c[4] = on_bottom ? F_ONE : 0;
        c[8] = c[9] = c[10] = F_ONE;
        c[11] = alpha;
        for (int i = 12; i < 16; ++i) c[i] = F_ONE;
    }
    v[0x40] = depth;
}

extern "C" U3X_FN void u3x_mode_icon(uintptr_t b, const uint8_t *el, const uint8_t *mod, uint32_t x, uint32_t y,
                                     uint32_t mode, Log log) {
    using namespace g47b;
    const uint32_t m = mode & 7;
    const uint8_t *desc = mod + (mode == 1 ? DESC_DOWN : DESC_GRAPPLE);

    u3x_icons.calls[m] += 1;
    // the index is read once: the same value goes to the lookup and into the "drawn" line
    const uint32_t index = rd<uint32_t>(desc, 8);
    const uint64_t texture = game_call<uint64_t>(b + FN_TEXTURE, rd<uint64_t>(desc, 0), index);
    if (!texture) {
        u3x_icons.no_texture[m] += 1;
        if (line_allowed(log, m)) {
            Line l;
            subject(l, mode);
            log(b, l.s(txt_none).z());
        }
        return;
    }

    uint32_t half, alpha;
    draw_icon(b, el, texture, x, y, &half, &alpha);
    u3x_icons.drawn[m] += 1;
    if (line_allowed(log, m)) {
        Line l;
        subject(l, mode);
        l.s(txt_slot).x(rd<uint32_t>(desc, 0), 8).s(txt_index).d(index);
        l.s(txt_material).x(texture & 0xffffffffffffull, 12).s(txt_drawn).d(u3x_icons.drawn[m]);
        log(b, l.z());
    }
}

extern "C" U3X_FN void u3x_grapple_icon(uintptr_t b, const uint8_t *el, const uint8_t *mod, uint32_t x, uint32_t y,
                                        uint32_t *diag, void (*seen)(uintptr_t, const uint8_t *, uint32_t, uint64_t),
                                        void (*drawn)(uintptr_t, uint32_t, uint32_t)) {
    using namespace g47b;
    const uint8_t *desc = mod + DESC_GRAPPLE;
    const uint32_t index = rd<uint32_t>(desc, 8);
    diag[0] += 1;
    diag[3] = index;
    diag[4] = rd<uint32_t>(desc, 0);

    // the flag byte of every tracked live player
    bool grabbed = false;
    for (int i = 0; i < 10; ++i) {
        const uint8_t *tracker = reinterpret_cast<const uint8_t *>(b + 0x17cb060 + 0x390 + 0x9c0 * i);
        uint32_t value = 0;
        const uint8_t *const *handle = rd<const uint8_t *const *>(tracker, 0x10);
        const uint32_t id = rd<uint32_t>(tracker, 0x18);
        if (rd<uint8_t>(tracker, 0xe1) && ptr_ok(handle) && id) {
            const uint8_t *player = *handle;
            if (ptr_ok(player) && rd<uint32_t>(player, 0x7c) == id) {
                const uint8_t flags = rd<uint8_t>(player, 0x39c);
                value = 0x100 + flags;
                if (flags & 4) grabbed = true;
            }
        }
        diag[5 + i] = value;
    }
    if (grabbed) diag[15] += 1;

    const uint64_t texture = texture_of(b, desc);
    seen(b, desc, index, texture);
    if (!texture) { diag[2] += 1; return; }

    uint32_t half, alpha;
    draw_icon(b, el, texture, x, y, &half, &alpha);
    diag[1] += 1;
    drawn(b, half, alpha);
}

extern "C" U3X_FN int u3x_hudnet_ring(uintptr_t b, const uint64_t *r,
                                      void (*grapple)(uintptr_t, const uint8_t *, const uint8_t *, uint32_t, uint32_t),
                                      Log log) {
    using namespace g47b;
    uint32_t *eb = ext_eb();
    eb[250] += 1;

    const uint8_t *el = reinterpret_cast<const uint8_t *>(r[3]);
    const uint8_t *frame = reinterpret_cast<const uint8_t *>(r[14]);
    if (!ptr_ok(el) || !ptr_ok(frame)) { eb[253] += 1; return 0; }

    const uint32_t mode = rd<uint32_t>(el, 0x50);
    eb[254] = mode;
    if (mode == 3) return 0;
    if (mode != 1 && mode != 2 && mode != 5) { eb[252] += 1; return 0; }

    const uint8_t *mod = *reinterpret_cast<const uint8_t *const *>(frame - 0x110);
    if (!ptr_ok(mod)) { eb[253] += 1; return 0; }
    if (at<uint32_t>(b + FN_TEXTURE) != FUNCTION_START || at<uint32_t>(b + FN_QUAD) != FUNCTION_START) {
        eb[253] += 1;
        return 0;
    }

    const uint32_t p = rd<uint32_t>(el, 0x54);
    uint32_t colour[4];
    u3x_ring_colour(b, colour, mode, rd<uint32_t>(el, 0x3c), p, at<int32_t>(b + 0x17b4a90 + 0x94));
    const uint32_t x = *reinterpret_cast<const uint32_t *>(frame - 0x160);
    const uint32_t y = *reinterpret_cast<const uint32_t *>(frame - 0x15c);
    u3h_ring_colour(colour);

    u3x_counter_ring(b, rd<uint32_t>(el, 8), p, 0x42200000u, 0x42480000u, rd<uint32_t>(el, 0x10), x, y,
                     mod + DESC_RING, colour);
    eb[251] += 1;   // counted whether or not a ring was drawn

    if (mode == 2) grapple(b, el, mod, x, y);
    else if (mode == 1) u3x_mode_icon(b, el, mod, x, y, 1, log);
    return 0;
}
