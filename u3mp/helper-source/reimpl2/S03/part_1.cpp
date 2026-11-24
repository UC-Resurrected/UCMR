// S03 part 1: menu data. Widget retags to the helper's datasource (enum 0xcf, LAN tag), the dispatcher's gate, the
// lobby button rules, the options rows source, the blank-row test and the lobby list's click registration.
// Written from spec S03_lobby_leftovers.md (sections 3 and 4.1 to 4.12). No float anywhere.

extern "C" const char *u3h_text(uintptr_t base, void *s, uint32_t id);
extern "C" uint32_t u3h_crc_sid(const char *text);

namespace s03_p1 {

// names whose name ids (u3h_crc_sid) are the widget / page / text ids
U3X_RO const char n_list_main_menu[] = "LIST_MAIN_MENU";
U3X_RO const char n_list_players[] = "LIST_PLAYERS";
U3X_RO const char n_list_cinema[] = "LIST_CINEMA";
U3X_RO const char n_tab_net_profile[] = "TAB_NET_PROFILE";
U3X_RO const char n_net_emblem_layer[] = "NET_EMBLEM_LAYER";
U3X_RO const char n_net_emblem_type[] = "NET_EMBLEM_TYPE";
U3X_RO const char n_list_boosters[] = "LIST_BOOSTERS";
U3X_RO const char n_list_medal_streaks[] = "LIST_MEDAL_STREAKS";
U3X_RO const char n_list_spoiler_class[] = "LIST_SPOILER_CLASS";
U3X_RO const char n_page_boosters[] = "PAGE_BOOSTERS";
U3X_RO const char n_page_net_treasures[] = "PAGE_NET_TREASURES";
U3X_RO const char n_page_eula[] = "PAGE_EULA";
U3X_RO const char n_page_credits[] = "PAGE_CREDITS";
U3X_RO const char n_menu_title_eula[] = "MENU_TITLE_EULA";
U3X_RO const char n_button_cinema_mpeg[] = "BUTTON_CINEMA_MPEG";
U3X_RO const char n_button_cinema_protect[] = "BUTTON_CINEMA_PROTECT";
U3X_RO const char n_button_cinema_unprotect[] = "BUTTON_CINEMA_UNPROTECT";
U3X_RO const char n_select_lan_profile[] = "SELECT_LAN_PROFILE";

constexpr uint32_t TYPE_SOURCE = 21;        // descriptor type "a datasource"
constexpr uint32_t ENUM_HELPER = 0xcf;      // the dispatcher entry the helper answers
constexpr uint32_t LAN_TAG = 0x4c414e00u;   // "LAN" + low byte
constexpr uint32_t WIDGET_SIZE = 0x68;
constexpr uint32_t MAX_WIDGETS = 64;

// game image offsets
constexpr uintptr_t EMPTY_TEXT = 0x13ac250;
constexpr uintptr_t ROW_VTABLE = 0x1678ef0;
constexpr uintptr_t CLICK_COUNT = 0x17be5dc;
constexpr uintptr_t CLICK_TABLE = 0x17be5e0;
constexpr uintptr_t LIST_CLICK_CB = 0x672c60;
constexpr uintptr_t MATCH_STATE = 0x17d1868;
constexpr uintptr_t MP_ACTIVE = 0x17d5c61;

// text ids without a known name
constexpr uint32_t TEXT_START_CUSTOM = 0x92db2b96u;
constexpr uint32_t TEXT_START = 0x63091cd0u;

U3X_BSS uint32_t gate[2];   // GATE: closed answers, last operation

template <typename T> [[gnu::always_inline]] inline void wr(void *p, uintptr_t off, T v) {
    *reinterpret_cast<T *>(static_cast<uint8_t *>(p) + off) = v;
}
[[gnu::always_inline]] inline uint32_t *ext_rb() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0x31b0); }
[[gnu::always_inline]] inline uint32_t *ext_ec() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0x4450); }

// a page's widget collection: its widgets and their number; false when absent or (limit != 0) above the limit
struct Widgets {
    uint8_t *items = nullptr;
    uint32_t count = 0;
};
U3X_FN bool widgets_of(uint8_t *page, Widgets &w, bool limit) {
    uint8_t *c = rd<uint8_t *>(page, 0x48);
    if (!c) return false;
    w.count = rd<uint32_t>(c, 0);
    w.items = rd<uint8_t *>(c, 8);
    if (!w.items) return false;
    return !limit || w.count <= MAX_WIDGETS;
}

[[gnu::always_inline]] inline uint32_t w_id(const uint8_t *w) { return rd<uint32_t>(w, 0x8); }
[[gnu::always_inline]] inline uint32_t w_type(const uint8_t *w) { return rd<uint32_t>(w, 0x30); }
[[gnu::always_inline]] inline uint32_t w_enum(const uint8_t *w) { return rd<uint32_t>(w, 0x34); }
[[gnu::always_inline]] inline uint32_t w_param(const uint8_t *w) { return rd<uint32_t>(w, 0x40); }

// point a widget's datasource at the helper with this tag
[[gnu::always_inline]] inline void retag(uint8_t *w, uint32_t tag) {
    wr<uint32_t>(w, 0x34, ENUM_HELPER);
    wr<uint32_t>(w, 0x40, tag);
}
// an untouched datasource widget (type 21, no parameter)
[[gnu::always_inline]] inline bool plain_source(const uint8_t *w) { return w_type(w) == TYPE_SOURCE && w_param(w) == 0; }

// the entry points (C linkage, so the namespace does not change their names)



// 4.1
extern "C" U3X_FN int u3x_retag_page(uint8_t *page, uint32_t count, int opts) {
    uint8_t *c = rd<uint8_t *>(page, 0x48);
    if (!c) return 0;
    const uint32_t n = rd<uint32_t>(c, 0);
    if (n != count && !(count == 9 && n == 10)) return 0;
    uint8_t *items = rd<uint8_t *>(c, 8);
    if (!items) return 0;
    const uint32_t main_menu = u3h_crc_sid(n_list_main_menu);
    const uint32_t players = u3h_crc_sid(n_list_players);
    const uint32_t cinema = u3h_crc_sid(n_list_cinema);
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t *w = items + i * WIDGET_SIZE;
        const uint32_t id = w_id(w), e = w_enum(w), p = w_param(w);
        uint32_t tag;
        if (id == main_menu) tag = LAN_TAG | 0xb3;
        else if (id == players) tag = LAN_TAG | 0xad;
        else if (id == cinema) tag = LAN_TAG | 0x39;
        else if (count == 9 && (e == 0x1e || e == 0x32 || e == 0x31 || (e == 0x30 && !(opts & 2)))) tag = LAN_TAG | e;
        else if (count == 9 && e == ENUM_HELPER && (p >> 8) == (LAN_TAG >> 8)) tag = p;
        else continue;
        if (w_type(w) != TYPE_SOURCE) return 0;   // earlier widgets stay retagged
        if (e == ENUM_HELPER && p == tag) continue;
        if (e != (tag & 0xff) || p != 0) return 0;
        retag(w, tag);
    }
    return 1;
}

// 4.2
extern "C" U3X_FN void u3x_retag_party(uint8_t *page) {
    Widgets ws;
    if (!widgets_of(page, ws, true)) return;
    const uint32_t players = u3h_crc_sid(n_list_players);
    const uint32_t profile = u3h_crc_sid(n_tab_net_profile);
    const uint32_t layer = u3h_crc_sid(n_net_emblem_layer);
    const uint32_t type = u3h_crc_sid(n_net_emblem_type);
    const uint32_t boosters = u3h_crc_sid(n_list_boosters);
    const uint32_t streaks = u3h_crc_sid(n_list_medal_streaks);
    const uint32_t spoiler = u3h_crc_sid(n_list_spoiler_class);
    for (uint32_t i = 0; i < ws.count; ++i) {
        uint8_t *w = ws.items + i * WIDGET_SIZE;
        if (!plain_source(w)) continue;
        const uint32_t id = w_id(w), e = w_enum(w);
        const bool hit = (id == players && e == 0xad) || (id == profile && e == 0xc3) || (id == layer && e == 0xb5) ||
                         (id == type && e == 0xbe) || (id == boosters && (e == 0xd3 || e == 0x43)) ||
                         (id == streaks && e == 0x44) || (id == spoiler && e == 0xbf) ||
                         e == 0xa6 || e == 0xa7 || e == 0xd9 || e == 0xd4 || e == 0x89 || e == 0x31 ||
                         (id == 0 && (e == 0x29 || e == 0xc2 || e == 0x6e));
        if (hit) retag(w, LAN_TAG | e);
    }
}

// 4.3
extern "C" U3X_FN void u3x_retag_lobby_box(uint8_t *page, int off) {
    if (off) return;
    Widgets ws;
    if (!widgets_of(page, ws, true)) return;
    for (uint32_t i = 0; i < ws.count; ++i) {
        uint8_t *w = ws.items + i * WIDGET_SIZE;
        if (plain_source(w) && w_enum(w) == 0x89) {
            retag(w, LAN_TAG | 0x89);
            ++ext_rb()[7];
        }
    }
}

// 4.4
extern "C" U3X_FN void u3x_retag_page_source(uint8_t *page) {
    if (ext_ea()[0] & (1u << 3)) return;
    const uint32_t id = rd<uint32_t>(page, 8), e = rd<uint32_t>(page, 0x58);
    uint32_t tag;
    if (id == u3h_crc_sid(n_page_boosters) && e == 0x43) tag = LAN_TAG | 0x43;
    else if (id == u3h_crc_sid(n_page_net_treasures) && (e == 0xd4 || e == 0xd5)) tag = LAN_TAG | 0xd5;
    else return;
    if (rd<uint32_t>(page, 0x54) != TYPE_SOURCE || rd<uint32_t>(page, 0x64) != 0) return;
    wr<uint32_t>(page, 0x58, ENUM_HELPER);
    wr<uint32_t>(page, 0x64, tag);
    ++ext_ea()[14];
}

// 4.5
extern "C" U3X_FN void u3x_retag_treasure_list(uint8_t *page) {
    if (rd<uint32_t>(page, 8) != u3h_crc_sid(n_page_net_treasures)) return;
    Widgets ws;
    if (!widgets_of(page, ws, true)) return;
    for (uint32_t i = 0; i < ws.count; ++i) {
        uint8_t *w = ws.items + i * WIDGET_SIZE;
        if (plain_source(w) && w_enum(w) == 0xd5) retag(w, LAN_TAG | 0xd5);
    }
}

// 4.6
extern "C" U3X_FN void u3x_retag_cinema_page(uint8_t *page) {
    if (ext_eb()[0] & 1u) return;
    Widgets ws;
    if (!widgets_of(page, ws, true)) return;
    const uint32_t mpeg = u3h_crc_sid(n_button_cinema_mpeg);
    const uint32_t protect = u3h_crc_sid(n_button_cinema_protect);
    const uint32_t unprotect = u3h_crc_sid(n_button_cinema_unprotect);
    for (uint32_t i = 0; i < ws.count; ++i) {
        uint8_t *w = ws.items + i * WIDGET_SIZE;
        if (!plain_source(w)) continue;
        const uint32_t id = w_id(w), e = w_enum(w);
        if ((id == mpeg && e == 0x3c) || (id == protect && e == 0x3d) || (id == unprotect && e == 0x3f)) {
            retag(w, LAN_TAG | e);
            ++ext_eb()[26];
        }
    }
}

// 4.7: the dispatcher's gate for widgets that bypass it (enum 0xcf)
extern "C" U3X_FN int u3x_native_gate_closed(uintptr_t base, int32_t *event) {
    if (at<uint8_t>(base + MP_ACTIVE)) return 0;
    wr<uintptr_t>(event, 0x40, base + EMPTY_TEXT);
    ++gate[0];
    gate[1] = static_cast<uint32_t>(event[0]);
    return 1;
}

// 4.8: only the party leader sees the set-up button
extern "C" U3X_FN int u3x_lobby_button_shown(int setup_button, int guest) {
    return (setup_button && guest) ? 0 : 1;
}

// 4.9
extern "C" U3X_FN uint32_t u3x_lobby_button_label(int setup_button, int guest, int custom_game, uint32_t own_label) {
    if (setup_button) return guest ? 0 : own_label;
    if (guest) return 1;   // the waiting text
    return custom_game ? TEXT_START_CUSTOM : TEXT_START;
}

// 4.10: the options rows (tags "LAH0".."LAH5")
extern "C" U3X_FN int u3x_rpm_source(uintptr_t base, int32_t *event, void *s, uint8_t *eu_row) {
    const uint8_t *d = rd<const uint8_t *>(event, 0x98);
    if (!d) return 0;
    const uint32_t tag = rd<uint32_t>(d, 0x1c);
    if (tag < 0x4c414830u || tag > 0x4c414835u) return 0;
    const uint32_t row = tag - 0x4c414830u;
    const int32_t op = event[0];
    const bool eula = row == 5 && base != 0 && at<uint8_t>(base + MP_ACTIVE) != 0 &&
                      at<uint32_t>(base + MATCH_STATE) != 2 && !(ext_ec()[1] & (1u << 14));
    if (row == 5 && eu_row) wr<uint32_t>(eu_row, 0x50, u3h_crc_sid(eula ? n_page_eula : n_page_credits));
    if (op == 8) event[1] = 1;
    else if (op == 1) wr<const char *>(event, 0x40, u3h_text(base, s, eula ? u3h_crc_sid(n_menu_title_eula) : rd<uint32_t>(d, 8)));
    else if (op == 6) event[1] = -1;
    return 1;
}

// 4.11: a style-6 row with an empty label is not drawn, measured or focused
extern "C" U3X_FN int u3x_k04_blank(uintptr_t base, uint8_t *row) {
    if (ext_eb()[2] & (1u << 9)) return 0;
    if (!ptr_ok(row) || rd<uint64_t>(row, 0) != base + ROW_VTABLE) return 0;
    uint8_t *o = rd<uint8_t *>(row, 0x18);
    if (!ptr_ok(o)) return 0;
    const uintptr_t *vt = rd<const uintptr_t *>(o, 0);
    if (!vt) return 0;
    const char *t = reinterpret_cast<const char *(*)(void *)>(vt[6])(o);
    return (!t || !t[0]) ? 1 : 0;
}

// 4.12: give LIST_MAIN_MENU the list click callback when SELECT_LAN_PROFILE has it
extern "C" U3X_FN int u3x_register_lobby_click(uintptr_t base) {
    const uint32_t n = at<uint32_t>(base + CLICK_COUNT);
    if (n > 128) return 0;
    const uint32_t main_menu = u3h_crc_sid(n_list_main_menu);
    const uint32_t lan_profile = u3h_crc_sid(n_select_lan_profile);
    const uintptr_t cb = base + LIST_CLICK_CB;
    uint8_t *table = &at<uint8_t>(base + CLICK_TABLE);
    bool ready = false;
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t *e = table + i * 24;
        const uint32_t id = rd<uint32_t>(e, 0);
        if (id == main_menu) return (rd<uintptr_t>(e, 8) == cb && rd<uintptr_t>(e, 16) == 0) ? 1 : 0;
        if (id == lan_profile && rd<uintptr_t>(e, 8) == cb) ready = true;
    }
    if (!ready || n == 128) return 0;
    uint8_t *e = table + n * 24;
    wr<uint64_t>(e, 0, 0);
    wr<uint64_t>(e, 8, 0);
    wr<uint64_t>(e, 16, 0);
    wr<uint32_t>(e, 0, main_menu);
    wr<uintptr_t>(e, 8, cb);
    at<uint32_t>(base + CLICK_COUNT) = n + 1;
    return 1;
}

}  // namespace s03_p1
