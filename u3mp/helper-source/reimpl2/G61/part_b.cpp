// G61 part B: menu item rewrite at the item layout, rank badge item, image item, tab icon item, slider track.
// Written from spec G61_territory_items_misc.md, part B.

extern "C" {
// other groups and the C part
void u3x_rank_icon_name(char *out, uint32_t value, uint32_t prestige_limit);
void *u3x_rank_back(uintptr_t base, void *s, uint32_t level);
void u3x_rank_tag(uintptr_t base, void *s, int32_t *ev, uint32_t v);
uint8_t *u3x_m04_map(void *s, uint32_t id);
int32_t u3h_icon_index(uintptr_t base, uint32_t id);
uint32_t u3h_crc_sid(const char *name);
uint32_t u3h_h13_mode(void);
uint32_t u3h_active_page(uintptr_t base);
volatile uint32_t *u3h_w10_diag(void);

// this part
int u3x_item_layout(uintptr_t base, void *s, uint64_t *r);
int u3x_rank_item(uintptr_t base, void *s, uint32_t *item);
void u3x_image_item(uint32_t *item, void *texture);
int u3x_tab_icon_item(uintptr_t base, uint32_t *item, const uint32_t *box);
void u3x_slider_track(uintptr_t base, const uint64_t *r);
}

namespace g61b {

// item types
constexpr uint32_t T_TEXTURE = 2, T_ICON = 3, T_BAR = 4, T_SPINNER = 0x10, T_BOX = 0x1b;
constexpr uint32_t T_MAP = 0x24, T_RANK = 0x29, T_TAB = 0x2d, T_NEW = 0x2f, T_PRESTIGE_NEW = 0x30, T_EQUIPPED_NEW = 0x31;

constexpr uint32_t NONE = 0xffffffffu;
constexpr uint32_t GRADIENT_ICON = 0x6e9d9193u;   // gradient bar texture, the tab box art
constexpr uint32_t TAB_COLOUR = 0xa09ca4aeu;      // own colour of a flat tab box
constexpr uint32_t TRACK_COLOUR = 0xff000000u;    // opaque black
constexpr uint32_t EMBLEM_PAGE = 0x85d761b5u;

constexpr uintptr_t IMG_TEXTURES = 0x1b31128;     // texture dictionary (first argument of native 20)
constexpr uintptr_t IMG_PRESTIGE_ICONS = 0x13a6670;
constexpr uintptr_t IMG_UI = 0x1b24bc8;
constexpr uintptr_t IMG_MP_ACTIVE = 0x17d5c61;
constexpr uintptr_t IMG_ICON_MATERIAL = 0x11b89b0;
constexpr uintptr_t IMG_ITEM_DRAW = 0x63d6f0;

U3X_RO const char icon_new_name[] = "general/net/menu-icons/icon-new";
U3X_RO const char icon_check_name[] = "general/net/menu-icons/icon-check";

using TextureByName = void *(*)(uintptr_t dictionary, const char *name);

// the group's own pointer test: like ptr_ok, but a multiple of 4 is enough
[[gnu::always_inline]] inline bool ptr4_ok(uint64_t v) {
    return v >= 0x10000u && v < (1ull << 47) && !(v & 3);
}

[[gnu::always_inline]] inline uint32_t *ext_ec() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0x4450); }

[[gnu::always_inline]] inline TextureByName native20(void *s) { return rd<TextureByName>(s, 928); }

// words 1..9 = 0, then no icon index, no second index, scale 1
[[gnu::always_inline]] inline void clear_item(uint32_t *item) {
    for (int i = 1; i <= 9; i++) item[i] = 0;
    item[7] = NONE;
    item[9] = NONE;
    item[10] = F_ONE;
}

// tab box (type 0x2d)
U3X_FN void tab_box(uintptr_t base, uint32_t *item, const uint64_t *r) {
    const uint32_t mode = u3h_h13_mode();   // always called: it records the mode
    if (mode != 2 || !(ext_ea()[240] & 4)) return;
    uint32_t *ec = ext_ec();
    if (ec[1] & (1u << 12)) {
        u3x_tab_icon_item(base, item, reinterpret_cast<const uint32_t *>(r[7]));
        return;
    }
    item[0] = T_BOX;
    ec[199] += 1;
    if (!(ext_eb()[3] & (1u << 30)) && item[9] == NONE) item[9] = TAB_COLOUR;
}

// map picture (type 0x24): the map's picture texture, or the spinner while there is none
U3X_FN void map_picture(uintptr_t base, void *s, uint32_t *item) {
    uint32_t *h = ext_ec() + 240;
    const uint32_t id = item[4];
    h[7] = id;
    const uint8_t *map = u3x_m04_map(s, id);
    const char *name = map ? rd<const char *>(map, 0x10) : nullptr;
    const TextureByName lookup = native20(s);
    void *texture = lookup && name ? lookup(base + IMG_TEXTURES, name) : nullptr;
    if (name) {
        uint32_t hash = 0;
        for (int i = 0; i < 64 && name[i]; i++) hash = hash * 31u + static_cast<uint8_t>(name[i]);
        h[11] = hash;
    }
    if (texture) {
        u3x_image_item(item, texture);
        h[3] += 1;
    } else {
        clear_item(item);
        item[0] = T_SPINNER;
        h[4] += 1;
    }
}

// "new", "equipped and new", "prestige badge while new" (types 0x2f, 0x31, 0x30): a plain UI icon
U3X_FN void marker_icon(uintptr_t base, uint32_t *item, uint32_t type) {
    uint32_t *h = ext_ec() + 240;
    uint32_t icon;
    if (type == T_EQUIPPED_NEW)
        icon = u3h_crc_sid(icon_check_name);
    else if (type == T_PRESTIGE_NEW && item[4] >= 1 && item[4] <= 3)
        icon = at<uint32_t>(base + IMG_PRESTIGE_ICONS + 4 * (item[4] - 1));
    else
        icon = u3h_crc_sid(icon_new_name);
    const int32_t index = u3h_icon_index(base, icon);
    if (index < 0) {
        h[5] += 1;
        return;
    }
    item[0] = T_ICON;
    item[1] = 0;
    item[2] = 0;
    item[3] = 0;
    item[4] = 0;
    item[5] = icon;
    item[6] = 1;
    item[7] = static_cast<uint32_t>(index);
    item[8] = 0;
    item[9] = NONE;
    h[type == T_NEW ? 0 : type == T_EQUIPPED_NEW ? 1 : 2] += 1;
}

// position of the gradient bar icon in the game's UI texture table; its entry in *entry. -1 = not there
U3X_FN int32_t gradient_index(uintptr_t base, const uint8_t **entry) {
    const uint8_t *ui = at<const uint8_t *>(base + IMG_UI);
    if (!ptr_ok(ui)) return -1;
    const uint8_t *table = rd<const uint8_t *>(ui, 0x20);
    if (!ptr_ok(table)) return -1;
    const uint32_t count = rd<uint16_t>(table, 2);
    if (count < 1 || count > 4095) return -1;
    const uint8_t *entries = rd<const uint8_t *>(table, 8);
    if (!ptr_ok(entries)) return -1;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = entries + i * 0x28u;
        if (rd<uint32_t>(e, 8) == GRADIENT_ICON) {
            *entry = e;
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

// the once-only record of the gradient icon: its table entry and its UI material
U3X_FN void gradient_record(uintptr_t base, uint32_t *ec, const uint8_t *entry, uint32_t index) {
    for (int i = 0; i < 10; i++) ec[177 + i] = rd<uint32_t>(entry, 4u * i);
    ec[176] = index | 0x80000000u;
    GameCall call;
    uint64_t *words = reinterpret_cast<uint64_t *>(&call);
    for (unsigned i = 0; i < sizeof(call) / 8; i++) words[i] = 0;
    call.fn = base + IMG_ICON_MATERIAL;
    call.i[0] = GRADIENT_ICON;
    call.i[1] = index;
    const uint64_t material = u3x_k26_call(&call);
    ec[187] = static_cast<uint32_t>(material);
    ec[188] = static_cast<uint32_t>(material >> 32);
    if (ptr_ok(reinterpret_cast<const void *>(material)))
        for (int i = 0; i < 8; i++) ec[189 + i] = rd<uint32_t>(reinterpret_cast<const void *>(material), 4u * i);
}

// (trunc(width) AND 0xffff) | trunc(height) << 16
[[gnu::always_inline]] inline uint32_t size_word(uint32_t w, uint32_t h) {
    return (static_cast<uint32_t>(f_to_int(w)) & 0xffffu) | (static_cast<uint32_t>(f_to_int(h)) << 16);
}

}  // namespace g61b

extern "C" U3X_FN void u3x_image_item(uint32_t *item, void *texture) {
    g61b::clear_item(item);
    item[0] = g61b::T_TEXTURE;
    *reinterpret_cast<void **>(item + 2) = texture;
}

extern "C" U3X_FN int u3x_rank_item(uintptr_t base, void *s, uint32_t *item) {
    using namespace g61b;
    uint32_t *ea = ext_ea();
    if (ea[170] & 1) return 0;
    const TextureByName lookup = native20(s);
    if (!lookup) return 0;

    const uint32_t v = item[4];
    ea[172] = v;
    const uint32_t level = (v >> 16) & 0x3fffu;
    const uint32_t limit = (ea[1] & 1) ? 4 : 6;
    // gives the prestige back texture its own turn at the loader; the answer is not needed here
    if (!(ext_eb()[2] & (1u << 20)) && level < limit) u3x_rank_back(base, s, level);

    char name[40];
    // the Legacy switch is read again after u3x_rank_back (spec B.4.2, clarification 1)
    u3x_rank_icon_name(name, v, (ea[1] & 1) ? 4 : 6);
    void *texture = lookup(base + IMG_TEXTURES, name);
    if (!texture) {
        ea[171] += 0x10000u;
        return 0;
    }
    u3x_image_item(item, texture);
    ea[171] += 1;

    // badge marker and rank word when G22 tags it, else 0 and 0
    int32_t tag[8];
    for (int i = 0; i < 8; i++) tag[i] = 0;
    u3x_rank_tag(base, s, tag, v);
    item[4] = static_cast<uint32_t>(tag[6]);
    item[5] = static_cast<uint32_t>(tag[7]);
    return 1;
}

extern "C" U3X_FN int u3x_tab_icon_item(uintptr_t base, uint32_t *item, const uint32_t *box) {
    using namespace g61b;
    const uint8_t *entry = nullptr;
    const int32_t found = gradient_index(base, &entry);
    if (found < 0) return 0;
    const uint32_t index = static_cast<uint32_t>(found);

    uint32_t *ec = ext_ec();
    if (ec[176] == 0) gradient_record(base, ec, entry, index);   // before the box is looked at

    uint32_t w = 0, h = 0;   // the cell's width and height, float patterns
    if (ptr4_ok(reinterpret_cast<uintptr_t>(box))) {
        w = box[0];
        h = box[1];
    }
    if (!(f_less(0, w) && f_less(0, h))) return 0;
    ec[197] = size_word(w, h);

    item[0] = T_ICON;
    item[1] = 0;
    item[2] = w;
    item[3] = h;
    item[4] = 0;
    item[5] = GRADIENT_ICON;
    item[6] = 0;
    item[7] = index;
    ec[174] += 1;
    return 1;
}

extern "C" U3X_FN int u3x_item_layout(uintptr_t base, void *s, uint64_t *r) {
    using namespace g61b;
    if (!ptr4_ok(r[5])) return 0;
    uint32_t *item = reinterpret_cast<uint32_t *>(r[5]);
    const uint32_t t = item[0];
    if (t < 0x24 || t > 0x31) return 0;

    uint32_t *ec = ext_ec();
    uint32_t *h = ec + 240;
    h[10] += 1;
    h[6] |= 1u << (t - 0x24);

    if (t == T_TAB) {
        tab_box(base, item, r);
        return 0;
    }
    if (t == T_RANK && u3x_rank_item(base, s, item) == 1) return 0;
    if (t == 0x25 || t == 0x26 || t == 0x27 || t == T_RANK || t == 0x2b || t == 0x2c) {   // types nobody draws
        h[8] += 1;
        h[9] = t;
        return 0;
    }
    if (ec[1] & (1u << 7)) return 0;
    if (t == T_MAP)
        map_picture(base, s, item);
    else if (t == T_NEW || t == T_PRESTIGE_NEW || t == T_EQUIPPED_NEW)
        marker_icon(base, item, t);
    return 0;
}

extern "C" U3X_FN void u3x_slider_track(uintptr_t base, const uint64_t *r) {
    using namespace g61b;
    volatile uint32_t *w = u3h_w10_diag();
    if (!ptr4_ok(r[7])) return;
    const uint8_t *item = reinterpret_cast<const uint8_t *>(r[7]);
    if (rd<uint32_t>(item, 0) != T_BAR) return;
    w[0] = w[0] + 1;

    const uint32_t sw = ext_ec()[3];
    bool skip = (sw & 1) != 0;
    if (!skip) skip = (sw & 2) ? at<uint8_t>(base + IMG_MP_ACTIVE) == 0 : u3h_active_page(base) != EMBLEM_PAGE;
    if (!skip) skip = !ptr4_ok(r[6]) || !ptr4_ok(r[3]);
    if (!skip) skip = at<uint32_t>(base + IMG_ITEM_DRAW) == 0;
    if (skip) {
        w[2] = w[2] + 1;
        return;
    }

    // the track: the bar's own box, flat and black
    alignas(16) uint8_t box[0x70];
    for (int i = 0; i < 0x70; i++) box[i] = item[i];
    *reinterpret_cast<uint32_t *>(box) = T_BOX;
    *reinterpret_cast<uint32_t *>(box + 0x24) = TRACK_COLOUR;

    GameCall call;
    uint64_t *words = reinterpret_cast<uint64_t *>(&call);
    for (unsigned i = 0; i < sizeof(call) / 8; i++) words[i] = 0;
    call.fn = base + IMG_ITEM_DRAW;
    call.i[0] = r[4];
    call.i[1] = r[5];
    call.i[2] = r[6];
    call.i[3] = reinterpret_cast<uintptr_t>(box);
    call.i[4] = r[3];
    call.i[5] = r[2];
    call.s[0] = r[11];
    call.s[1] = r[12];
    u3x_k26_call(&call);

    w[1] = w[1] + 1;
    w[3] = size_word(rd<uint32_t>(item, 0x3c), rd<uint32_t>(item, 0x40));
}
