// G60 part B: the round screen's overlay draw (spec G60_round_screen.md, sections 4.3, 4.4, 3.5).
// Draws the menu page of the highest visible slot of a round-screen object over the running game.

// ---- other groups and the C part (prototypes as the spec gives them)
extern "C" uint32_t u3h_crc_sid(const char *name);

namespace g60b {

// image offsets: game functions
constexpr uintptr_t FN_LAYOUT = 0x624500;
constexpr uintptr_t FN_FRAME = 0x627520;
constexpr uintptr_t FN_INPUT_STATE = 0x658050;
constexpr uintptr_t FN_PAD_COMMANDS = 0x10dda70;
constexpr uintptr_t FN_FADE_QUAD = 0x63c430;

// image offsets: game data
constexpr uintptr_t NGM = 0x17cb060;
constexpr uintptr_t NO_OVERLAY = 0x17b4a5f;
constexpr uintptr_t CLOCK = 0x17b4a90;          // +0x114 s32 ticks of 1/300 s, +0x11c f32 frame time
constexpr uintptr_t PAD_OBJECT = 0x16e2fa0;
constexpr uintptr_t MENU_SCREEN0 = 0x17bd080;
constexpr uintptr_t MENU_USER = 0x17c23a0;
constexpr uintptr_t FADE_STRENGTH = 0x16e8110;

// float patterns
constexpr uint32_t F_ZERO = 0x00000000u;
constexpr uint32_t F_MINUS_ONE = 0xbf800000u;
constexpr uint32_t F_TWENTY = 0x41a00000u;
constexpr uint32_t F_300 = 0x43960000u;
constexpr uint32_t F_1280 = 0x44a00000u;
constexpr uint32_t F_720 = 0x44340000u;

// The joypad commands that stay enabled while a page is drawn (3.5): eight names, NUL-separated, ended by an empty
// name. Their words are the string ids of the names.
U3X_RO const char pad_cmd_names[] =
    "medal-streak\0look\0look-press\0melee-attack\0melee-attack-hold\0melee-attack-release\0melee-attack-repeat\0"
    "buddy-up\0";
// Pad command 9 (3.5): its name is unknown (spec 8, question 1), so its word cannot be generated from a name; it is
// the only word of the list given by value.
constexpr uint32_t PAD_CMD_9 = 0xc1e54d53u;

// layout output buffer of 0x320 bytes (2.3)
U3X_BSS uint8_t layout_out[0x320];

U3X_FN uint32_t &eb(int i) { return ext_eb()[i]; }

// "Valid entry E of object O" (3.2): the function pointer, or 0 when there is none.
U3X_FN uintptr_t valid_entry(const uint8_t *o, uintptr_t e) {
    if (!ptr_ok(o)) return 0;
    const uint8_t *table = rd<const uint8_t *>(o, 0);
    if (!ptr_ok(table)) return 0;
    const uintptr_t fn = rd<uintptr_t>(table, e);
    return ptr_ok(reinterpret_cast<const void *>(fn)) ? fn : 0;
}

// The draw context passed to the page and its children (4.4 step 5).
struct alignas(16) Context {
    uint64_t part;           // +0x00 menu + 0x10
    uint32_t xf[10];         // +0x08 transform
    uint32_t rect[4];        // +0x30 screen rectangle
    uint32_t seconds;        // +0x40 game clock in seconds (f32)
    uint8_t open;            // +0x44
    uint8_t pad0[3];
    uint32_t user_word;      // +0x48 menu u32 +0x13fc
    uint32_t pad1;
};
static_assert(sizeof(Context) == 0x50);
static_assert(__builtin_offsetof(Context, rect) == 0x30);
static_assert(__builtin_offsetof(Context, seconds) == 0x40);
static_assert(__builtin_offsetof(Context, user_word) == 0x48);

struct UpdateRecord {
    uint64_t menu;
    int32_t state;
    uint32_t frame_time;
};
static_assert(sizeof(UpdateRecord) == 16);

// CHILDREN (4.4): draw the page's children, the focused one last over the fade quad.
U3X_FN void draw_children(uintptr_t base, const uint8_t *page, Context *ctx) {
    int32_t n = rd<int32_t>(page, 0xac);
    if (n < 0) n = 0;
    uint8_t *const *kids = rd<uint8_t *const *>(page, 0xb0);
    if (n > 64 || (n > 0 && !ptr_ok(kids))) {
        eb(183) += 1;
        return;
    }
    uint8_t *focus = nullptr;
    for (int32_t i = 0; i < n; ++i) {
        uint8_t *c = kids[i];
        if (ptr_ok(c) && rd<uint8_t>(c, 8) != 0) { focus = c; break; }
    }
    for (int32_t i = 0; i < n; ++i) {
        uint8_t *c = kids[i];
        if (c == focus) continue;
        if (const uintptr_t fn = valid_entry(c, 0x18)) game_call<void>(fn, c, static_cast<void *>(nullptr), ctx);
    }
    const uint32_t saved = at<uint32_t>(base + FADE_STRENGTH);
    at<uint32_t>(base + FADE_STRENGTH) = F_ONE;
    game_call<void>(base + FN_FADE_QUAD, static_cast<void *>(nullptr), static_cast<void *>(nullptr));
    if (focus) {
        if (const uintptr_t fn = valid_entry(focus, 0x18)) game_call<void>(fn, focus, static_cast<void *>(nullptr), ctx);
    }
    at<uint32_t>(base + FADE_STRENGTH) = saved;
}

// PAGE DRAW (4.4): find the page with this id in the menu, update it and draw it with its children.
U3X_FN void page_draw(uintptr_t base, uint8_t *menu, uint32_t id, uint32_t alpha, uint32_t x, uint32_t y) {
    if (rd<uint32_t>(menu, 0x08) != 0) {
        eb(183) += 1;
        return;
    }
    uint8_t *page = nullptr;
    for (int i = 0; i < 160; ++i) {
        uint8_t *p = rd<uint8_t *>(menu, 0x390 + 8 * static_cast<uintptr_t>(i));
        if (!p) break;
        if (ptr_ok(p) && rd<uint32_t>(p, 0x10) == id) { page = p; break; }
    }
    if (!page || !valid_entry(page, 0x10) || !valid_entry(page, 0x30)) {
        eb(183) += 1;
        return;
    }

    const uint32_t g = rd<uint8_t>(menu, 0xdf5);
    at<uint32_t>(base + MENU_USER) = g;
    *reinterpret_cast<uint32_t *>(menu + 0x2694) = g;

    // draw context: zeroed, then the fields; the transform is the identity-with-alpha combined with the offset one
    Context ctx;
    {
        volatile uint64_t *w = reinterpret_cast<volatile uint64_t *>(&ctx);
        for (unsigned i = 0; i < sizeof(Context) / 8; ++i) w[i] = 0;
    }
    ctx.part = reinterpret_cast<uint64_t>(menu + 0x10);
    for (int i = 0; i < 4; ++i) ctx.xf[i] = f_mul(F_ONE, F_ONE);
    ctx.xf[4] = f_mul(F_ONE, alpha);
    ctx.xf[5] = f_add(F_ZERO, F_ZERO);
    ctx.xf[6] = f_add(F_ZERO, F_ZERO);
    ctx.xf[7] = f_mul(F_ONE, F_ONE);
    ctx.xf[8] = f_add(x, F_ZERO);
    ctx.xf[9] = f_add(y, F_ZERO);
    ctx.rect[0] = 0;
    ctx.rect[1] = 0;
    ctx.rect[2] = F_1280;
    ctx.rect[3] = F_720;
    ctx.seconds = f_div(f_from_int(at<int32_t>(base + CLOCK + 0x114)), F_300);
    ctx.open = 0;
    ctx.user_word = rd<uint32_t>(menu, 0x13fc);

    int32_t st = game_call<int32_t>(base + FN_INPUT_STATE, menu, uint64_t{0}, uint64_t{0});
    if (st == 8) st = 5;
    else if (st != 1 && st != 2) st = 0;

    // joypad commands that stay enabled while the page is up
    uint8_t *pads = at<uint8_t *>(base + PAD_OBJECT);
    if (ptr_ok(pads)) {
        alignas(16) uint32_t cmds[10];
        const char *name = pad_cmd_names;
        int k = 0;
        while (*name) {
            cmds[k++] = u3h_crc_sid(name);
            while (*name) ++name;
            ++name;
        }
        cmds[k++] = PAD_CMD_9;
        cmds[k] = 0;
        const uint32_t n = g == 0 ? 2u : 3u;
        game_call<void>(base + FN_PAD_COMMANDS, pads + 0x7e0 + 0x1d8 * static_cast<uintptr_t>(n), n, uint32_t{2},
                        static_cast<uint32_t *>(cmds), uint32_t{0x24});
    }

    // activate, then update (entries read again at call time, no further check)
    game_call<void>(rd<uintptr_t>(rd<const uint8_t *>(page, 0), 0x30), page, int32_t{1});
    UpdateRecord rec;
    rec.menu = reinterpret_cast<uint64_t>(menu);
    rec.state = st;
    rec.frame_time = at<uint32_t>(base + CLOCK + 0x11c);
    game_call<void>(rd<uintptr_t>(rd<const uint8_t *>(page, 0), 0x10), page, &rec);

    // layout, then the page's own frame with the rectangle the layout returned
    {
        volatile uint8_t *b = layout_out;
        for (unsigned i = 0; i < sizeof(layout_out); ++i) b[i] = 0;
    }
    game_call<void>(base + FN_LAYOUT, page, &ctx, static_cast<uint8_t *>(layout_out));
    alignas(16) uint32_t rect[4];
    for (int i = 0; i < 4; ++i) rect[i] = rd<uint32_t>(layout_out, 4 * static_cast<uintptr_t>(i));
    game_call<void>(base + FN_FRAME, page, static_cast<void *>(nullptr), &ctx, static_cast<void *>(ctx.xf),
                    static_cast<uint32_t *>(rect));

    draw_children(base, page, &ctx);

    eb(182) += 1;
    eb(184) = id;
}

}  // namespace g60b

// 4.3: draw the highest visible slot of screen scr's round-screen object (only screen 0 is drawn).
extern "C" U3X_FN void u3x_r19_draw(uintptr_t base, void *, uint8_t *obj, int scr) {
    using namespace g60b;
    if (at<uint8_t>(base + NO_OVERLAY) != 0) return;

    uint8_t *slot = nullptr;
    for (int n = 3; n >= 0; --n) {
        uint8_t *sl = obj + 0x08 + 24 * n;
        if ((rd<uint32_t>(sl, 4) & 0x7fffffffu) != 0) { slot = sl; break; }   // alpha != 0.0 (a NaN counts)
    }
    if (!slot) return;
    if (scr != 0) {
        eb(186) += 1;
        return;
    }

    // easing of the fade: e is the eased alpha, y the vertical slide offset
    const uint32_t a = rd<uint32_t>(slot, 4);
    uint32_t e, k;
    if (rd<uint8_t>(slot, 0) != 0) {
        const uint32_t t = f_sub(F_ONE, a);
        e = f_sub(F_ONE, f_mul(t, t));
        k = f_mul(F_MINUS_ONE, F_TWENTY);
    } else {
        e = f_mul(a, a);
        k = f_mul(F_ONE, F_TWENTY);
    }
    const uint32_t y = f_mul(f_sub(F_ONE, e), k);
    const uint32_t alpha = f_mul(a, F_ONE);

    uint8_t *menu = &at<uint8_t>(base + MENU_SCREEN0);
    page_draw(base, menu, rd<uint32_t>(slot, 16), alpha, F_ZERO, y);

    // background dim (also when the page draw was refused)
    if (rd<uint8_t>(slot, 12) == 0) return;
    if (at<uint8_t>(base + NGM + 0x6cb0) != 0) return;
    if (rd<uint8_t>(menu, 0xdd4) == 0) {
        *reinterpret_cast<uint32_t *>(menu + 0xdcc) = e;
        menu[0xdd4] = 1;
        eb(187) += 1;
    }
}
