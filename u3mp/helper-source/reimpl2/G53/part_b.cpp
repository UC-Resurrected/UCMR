// G53 part B: the call bridge and the hat-stat icons of the in-match player card.
// Written from spec G53_player_info_hats.md, sections 4.8 to 4.12.

extern "C" uint8_t *u3h_np119(void);
extern "C" uint8_t *u3x_sym_table(void *s, uint32_t id);
// class of a stat id (C part): 0 = none, 1 = inverse, 2 = higher helps, 3 = lower helps
extern "C" int u3h_stat_class(uint32_t stat);

extern "C" uint64_t u3x_call8(uintptr_t fn, uint32_t f0, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t s0, uint64_t s1);
extern "C" uint8_t *u3x_stat_icon(uintptr_t base, void *s, uint32_t stat, uint32_t m);
extern "C" uint8_t *u3x_hat_icon(uintptr_t base, void *s, uint8_t *np, int32_t i);
extern "C" void u3x_hat_icon_count(uintptr_t base, void *s, const uint64_t *regs);
extern "C" void u3x_hat_icon_draw(uintptr_t base, void *s, const uint64_t *regs);

namespace g53b {
namespace {

// image offsets
constexpr uintptr_t PLAYERS = 0x17cb060 + 0x390;    // ten net player records
constexpr uintptr_t PLAYER_SIZE = 0x9c0;
constexpr int PLAYER_COUNT = 10;
constexpr uintptr_t PLAYER_LOOK = 0x784b20;
constexpr uintptr_t LOOK_ITEM = 0x9952a0;
constexpr uintptr_t TEXTURE_BY_NAME = 0x1189430;
constexpr uintptr_t TEXTURE_DICT = 0x1b31128;
constexpr uintptr_t PANEL_MAKE = 0x640ac0;
constexpr uintptr_t PANEL_DRAW = 0x63d6f0;
constexpr uintptr_t PANEL_DRAW_OWNER = 0x17bd090;

constexpr uint32_t SW_HAT_ICONS_OFF = 8192;   // ec[0] bit 13

constexpr uint32_t SYM_HAT_MODS = 0xb8a3581b;
constexpr uint32_t SYM_STAT_ICONS = 0x53ed596a;

constexpr uint32_t F_TWO = 0x40000000u, F_FOUR = 0x40800000u, F_MINUS_FOUR = 0xc0800000u;

// stat classes as u3h_stat_class answers them
constexpr int CLASS_INVERSE = 1, CLASS_UP = 2, CLASS_LOW = 3;

// places in the frame of the card's draw function, below its frame pointer
constexpr uintptr_t FP_PLAYER = 0x12b0, FP_CONTEXT = 0x11f8, FP_KICKBACK = 0x11e0, FP_STYLE = 0x450, FP_PANEL = 0x420;

// the first 12 code bytes a game function must have before it is called
U3X_RO const uint8_t code_player_look[12] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xfb, 0x40, 0x0f};
U3X_RO const uint8_t code_look_item[12] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54};

[[gnu::always_inline]] inline uint32_t *ec() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0x4450); }

U3X_FN bool code_is(uintptr_t base, uintptr_t off, const uint8_t *want) {
    const uint8_t *code = reinterpret_cast<const uint8_t *>(base + off);
    for (int i = 0; i < 12; i++)
        if (code[i] != want[i]) return false;
    return true;
}

// slot 0..9 of a net player record, -1 when the pointer is not one of the ten records
U3X_FN int player_slot(uintptr_t base, const uint8_t *np) {
    const int64_t d = static_cast<int64_t>(reinterpret_cast<uintptr_t>(np) - (base + PLAYERS));
    if (d < 0 || d % static_cast<int64_t>(PLAYER_SIZE) != 0) return -1;
    const int64_t q = d / static_cast<int64_t>(PLAYER_SIZE);
    return q < PLAYER_COUNT ? static_cast<int>(q) : -1;
}

// does a multiplier count as the "better" change of its stat
U3X_FN bool is_better(uint32_t stat, uint32_t m) {
    const int cls = u3h_stat_class(stat);
    if (cls == CLASS_INVERSE) return !f_less(f_div(F_ONE, m), F_ONE);
    if (cls == CLASS_UP) return true;
    if (cls == CLASS_LOW) return f_less(m, F_ONE);
    return !f_less(m, F_ONE);
}

// a texture item of a panel (48 bytes)
struct alignas(16) PanelItem {
    uint32_t kind, pad0;
    const uint8_t *texture;
    uint32_t pad1[3], word_1c, pad2, word_24, scale, pad3;
};
struct alignas(16) Words4 { uint32_t v[4]; };
struct alignas(16) Words2 { uint32_t v[2]; };

}  // namespace
}  // namespace g53b

// 4.8: call a game function with six integer arguments, one float pattern and two stack words
U3X_FN uint64_t u3x_call8(uintptr_t fn, uint32_t f0, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                          uint64_t a5, uint64_t s0, uint64_t s1) {
    GameCall c;
    c.fn = fn;
    c.i[0] = a0; c.i[1] = a1; c.i[2] = a2; c.i[3] = a3; c.i[4] = a4; c.i[5] = a5;
    c.x[0] = f0;
    for (int k = 1; k < 5; k++) c.x[k] = 0;
    c.s[0] = s0;
    c.s[1] = s1;
    c.al = 0;
    return u3x_k26_call(&c);
}

// 4.9: the icon texture of a stat for a multiplier, or null
U3X_FN uint8_t *u3x_stat_icon(uintptr_t base, void *s, uint32_t stat, uint32_t m) {
    using namespace g53b;
    const uint8_t *table = u3x_sym_table(s, SYM_STAT_ICONS);
    if (!table) return nullptr;
    const int32_t count = rd<int32_t>(table, 0);
    const uint8_t *entries = rd<const uint8_t *>(table, 8);
    if (!entries) return nullptr;
    const uint8_t *found = nullptr;   // the last entry of the stat counts
    for (int32_t k = 0; k < count; k++) {
        const uint8_t *entry = entries + 0x28 * static_cast<intptr_t>(k);
        if (rd<uint32_t>(entry, 0) == stat) found = entry;
    }
    if (!found) return nullptr;
    const char *name = rd<const char *>(found, is_better(stat, m) ? 8 : 0x18);
    if (!name) return nullptr;
    return game_call<uint8_t *>(base + TEXTURE_BY_NAME, base + TEXTURE_DICT, name);
}

// 4.10: the icon texture for stat i of the hat a net player wears, or null
U3X_FN uint8_t *u3x_hat_icon(uintptr_t base, void *s, uint8_t *np, int32_t i) {
    using namespace g53b;
    if (!np) return nullptr;
    if (!code_is(base, PLAYER_LOOK, code_player_look) || !code_is(base, LOOK_ITEM, code_look_item)) return nullptr;
    const uint8_t *look = game_call<const uint8_t *>(base + PLAYER_LOOK, np, uint32_t{0});
    const uint8_t *table = u3x_sym_table(s, SYM_HAT_MODS);
    const int slot = player_slot(base, np);
    if (slot >= 0 && u3h_np119()[8 * slot + 5]) return nullptr;   // the player switched hat stats off
    if (!table) return nullptr;
    if (!look) return nullptr;
    const uint8_t *hat = game_call<const uint8_t *>(base + LOOK_ITEM, uint64_t{0}, static_cast<uint32_t>(look[0]),
                                                    static_cast<uint32_t>(look[1]), uint32_t{0},
                                                    static_cast<uint32_t>(look[2]));
    if (!hat) return nullptr;
    const int32_t row_count = rd<int32_t>(table, 0);
    const uint8_t *rows = rd<const uint8_t *>(table, 8);
    if (!rows) return nullptr;
    const uint32_t item = rd<uint32_t>(hat, 0);
    for (int32_t r = 0; r < row_count; r++) {
        const uint8_t *row = rows + 16 * static_cast<intptr_t>(r);
        if (rd<uint32_t>(row, 0) != item) continue;
        // the first row of the item decides
        if (i >= rd<int32_t>(row, 4)) return nullptr;
        const uint8_t *pairs = rd<const uint8_t *>(row, 8);
        if (!pairs) return nullptr;
        const uint8_t *pair = pairs + 8 * static_cast<intptr_t>(i);
        return u3x_stat_icon(base, s, rd<uint32_t>(pair, 0), rd<uint32_t>(pair, 4));
    }
    return nullptr;
}

// 4.11: the card's icon count = booster icons + kickback icon + hat icons
U3X_FN void u3x_hat_icon_count(uintptr_t base, void *s, const uint64_t *regs) {
    using namespace g53b;
    uint32_t *e = ec();
    const uintptr_t fp = regs[14];
    uint8_t *player = at<uint8_t *>(fp - FP_PLAYER);
    uint32_t n = static_cast<uint32_t>(regs[13]);
    if (at<uint64_t>(fp - FP_KICKBACK)) n++;
    if (!(e[0] & SW_HAT_ICONS_OFF)) {
        uint32_t hats = 0;
        if (u3x_hat_icon(base, s, player, 0)) hats++;
        if (u3x_hat_icon(base, s, player, 1)) hats++;
        n += hats;
        e[110]++;
        e[112] = hats;
    }
    e[105] = n;
}

// 4.12: draw the hat icons into the card's next icon slots
U3X_FN void u3x_hat_icon_draw(uintptr_t base, void *s, const uint64_t *regs) {
    using namespace g53b;
    uint32_t *e = ec();
    if (e[0] & SW_HAT_ICONS_OFF) return;
    const uintptr_t fp = regs[14];
    uint8_t *player = at<uint8_t *>(fp - FP_PLAYER);
    const uint64_t context = at<uint64_t>(fp - FP_CONTEXT);
    uint32_t x = e[106];
    const uint32_t y = e[107], w = e[108], h = e[109];
    if (at<uint64_t>(fp - FP_KICKBACK)) x = f_add(x, f_add(w, F_FOUR));   // the kickback icon took a slot
    for (int32_t i = 0; i < 2; i++) {
        const uint8_t *texture = u3x_hat_icon(base, s, player, i);
        if (!texture) continue;
        PanelItem item;
        item.kind = 2;   // a texture item
        item.pad0 = 0;
        item.texture = texture;
        item.pad1[0] = item.pad1[1] = item.pad1[2] = 0;
        item.word_1c = 0xffffffffu;
        item.pad2 = 0;
        item.word_24 = 0xffffffffu;
        item.scale = F_ONE;
        item.pad3 = 0;
        Words4 rect;
        rect.v[0] = f_add(x, F_TWO);
        rect.v[1] = f_add(y, F_TWO);
        rect.v[2] = f_add(w, F_MINUS_FOUR);
        rect.v[3] = f_add(h, F_MINUS_FOUR);
        Words2 size;
        size.v[0] = rect.v[2];
        size.v[1] = rect.v[3];
        Words4 zero;
        zero.v[0] = zero.v[1] = zero.v[2] = zero.v[3] = 0;
        u3x_call8(base + PANEL_MAKE, at<uint32_t>(fp - FP_STYLE), fp - FP_PANEL, reinterpret_cast<uintptr_t>(&item), 0,
                  reinterpret_cast<uintptr_t>(&size), reinterpret_cast<uintptr_t>(&zero), 0xffffffffu, 0, 0);
        u3x_call8(base + PANEL_DRAW, 0, base + PANEL_DRAW_OWNER, context, fp - FP_STYLE, fp - FP_PANEL,
                  reinterpret_cast<uintptr_t>(&rect), 0, 0, 0xffffffffu);
        e[111]++;
        x = f_add(x, f_add(w, F_FOUR));
    }
}
