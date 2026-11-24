// S06 part B: per-frame net and HUD jobs (alive test, local players' respawn tick, scoreboard levels, HUD material
// names, score-plate textures, weapon panel origin, co-op buddy marker icon, rank-up popup gate and test aid).
// Written from spec S06_net_kill_leftovers.md, part B (sections 3, 4.18 - 4.25, 6).

// ---- functions of other parts / groups / the C part (prototypes as the spec gives them)
extern "C" uint32_t u3h_crc_sid(const char *p);
extern "C" const uint32_t *u3h_e01_origin(int which);   // 0: PS4 origin pair (tested), 1: PS3 1.19 pair (stored)
extern "C" int u3x_np_has_effect(void *s, uint8_t *p, uint32_t h);
extern "C" int u3x_handle_obj_valid(uint8_t *h);
extern "C" void u3x_respawn_tick(uintptr_t base, void *s, uint8_t *p, int idx);
extern "C" int u3x_net_player_in_game(const uint8_t *np);
extern "C" uint32_t u3x_net_player_id(const uint8_t *np);

namespace s06b {

// Asset names for the string-id rule (3.9), each zero-terminated, the list ends with an empty name.
U3X_RO const char hud_names[] =
    "general/hud/alpha-mask-icon:default\0"
    "general/hud/alpha-only-icon:default\0"
    "general/hud/buttons/up-button-pulse-big:default\0"
    "general/hud/store:locked\0"
    "general/hud/store:purchased\0"
    "general/hud/trophy:blank\0"
    "general/hud/trophy:outline\0"
    "general/hud/trophy:solid\0"
    "general/menu/gradient-bar:gradient-bar\0"
    "general/net/flag-icons:treasure\0"
    "general/net/medals/medal-sheen:default\0"
    "general/net/mode-buttons:check-mark\0"
    "general/net/mode-buttons:notification\0"
    "general/net/mode-buttons:question-mark\0"
    "general/net/mode-buttons:warning\0"
    "general/net/treasures:treasure-chest\0";

U3X_RO const char plate_names[] =
    "general/net/mini-score:default\0"
    "general/net/mini-score:hud-round-score-border\0"
    "general/net/mini-score:hud-round-score-fill\0"
    "general/net/message-back:default\0";

// The helper's own rank-up test words (4.25): ranks 10, 20, 30, 50, 80, then prestige 1, 2, 3.
U3X_RO const uint32_t rank_test_words[8] = {9, 19, 29, 49, 79, 0x10000, 0x10001, 0x10002};

constexpr uint32_t SYM_HUD_SETTINGS = 0x9353ed16u;
constexpr uint32_t EFFECT_ALIVE = 0xc054e78bu;
constexpr uint32_t POPUP_RANK_UP = 0xc2e4ad43u;

struct IdPair { uint32_t ps4, ps3; };   // id of the name as written (':'), id of its slash form ('/')

// Fills out[] with the id pairs of the names in list; returns how many (at most max).
U3X_FN int name_ids(const char *list, IdPair *out, int max) {
    int n = 0;
    while (*list && n < max) {
        char slash[96];
        int k = 0;
        bool swapped = false;
        for (; list[k] && k < 95; ++k) {
            char c = list[k];
            if (c == ':' && !swapped) { c = '/'; swapped = true; }
            slash[k] = c;
        }
        slash[k] = 0;
        out[n].ps4 = u3h_crc_sid(list);
        out[n].ps3 = u3h_crc_sid(slash);
        ++n;
        while (*list) ++list;
        ++list;   // past the terminator
    }
    return n;
}

// Native n of the state object s (function pointer at s + 0x300 + 8n).
[[gnu::always_inline]] inline void *native(void *s, int n) { return rd<void *>(s, 0x300 + 8 * n); }

// Value of the hud-settings symbol through native 13, or null when the native is null or the value is not ptr_ok.
U3X_FN uint8_t *hud_settings(void *s) {
    void *f = native(s, 13);
    if (!f) return nullptr;
    uint8_t *v = reinterpret_cast<uint8_t *(*)(uint32_t)>(f)(SYM_HUD_SETTINGS);
    return ptr_ok(v) ? v : nullptr;
}

}  // namespace s06b

// 4.18: a net player counts as alive.
extern "C" U3X_FN int u3x_np_alive(void *s, uint8_t *p) {
    if (u3x_np_has_effect(s, p, s06b::EFFECT_ALIVE) != 0) return 1;
    if (u3x_handle_obj_valid(p + 0x10) != 0 && rd<int32_t>(p, 0x39c) <= rd<int32_t>(p, 0x3a0)) return 1;
    return rd<uint8_t>(p, 0x770) != 0 ? 1 : 0;
}

// 4.19: respawn tick of the two local players (hook kinds 17 / 18).
extern "C" U3X_FN void u3x_net_local_players_tick(uintptr_t base, uint8_t *mgr, void *s, int variant) {
    uint32_t *passes = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(s) + 0x3d44);
    *passes = *passes + 1;   // counted on every call
    if (!s06b::native(s, 53) || !s06b::native(s, 62)) return;
    for (int i = 0; i < 2; ++i) {
        uint8_t *p = rd<uint8_t *>(mgr, 0x370 + 8 * i);
        if (!p) continue;
        if (variant == 17 && rd<uint8_t>(p, 0x5e4) != 0) continue;   // leaving player (variant 17 only)
        u3x_respawn_tick(base, s, p, i);
    }
}

// 4.20: fill scoreboard levels a guest stored as 0 before its member data arrived.
extern "C" U3X_FN void u3x_x53_levels(uintptr_t base) {
    for (int i = 0; i < 10; ++i) {
        uint8_t *np = reinterpret_cast<uint8_t *>(base + 0x17cb3f0 + static_cast<uintptr_t>(i) * 0x9c0);
        if (!u3x_net_player_in_game(np)) continue;
        uint8_t *r = game_call<uint8_t *>(base + 0x7a3400, reinterpret_cast<void *>(base + 0x17d6420), np + 0x5b8);
        if (!r) continue;
        const uint8_t level = rd<uint8_t>(r, 0xe);
        if (level == 0) continue;
        uint32_t *own = reinterpret_cast<uint32_t *>(np + 0x204);
        if (*own == 0) *own = level;
        const uint32_t id = u3x_net_player_id(np);
        if (id == 0) continue;
        const uintptr_t keepers[2] = {base + 0x17dd780, base + 0x17e8388};   // score keeper, second snapshot
        for (uintptr_t k : keepers) {
            for (int j = 0; j < 16; ++j) {   // always 16 records, the member count is not consulted
                uint8_t *rec = reinterpret_cast<uint8_t *>(k + 0x50e4 + static_cast<uintptr_t>(j) * 0xa4);
                if (rd<uint32_t>(rec, 0) == id && rec[0xb] == 0) rec[0xb] = level;
            }
        }
    }
}

// 4.21: rename the 16 HUD material ids of the UI's material table to their PS3 (slash-form) ids, once per table.
extern "C" U3X_FN void u3x_hud_alias_names(uintptr_t base, void *s) {
    uint32_t *done = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(s) + 0x3e00);   // low 32 bits only
    uint8_t *ui = at<uint8_t *>(base + 0x1b24bc8);
    uint8_t *table = ui ? rd<uint8_t *>(ui, 0x20) : nullptr;
    if (!table) { *done = 0; return; }
    const uint32_t low = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(table));
    if (*done == low) return;
    const uint32_t count = rd<uint16_t>(table, 2);
    uint8_t *entries = rd<uint8_t *>(table, 8);
    if (!entries) return;   // retried next call, D unchanged
    s06b::IdPair ids[16];
    const int n = s06b::name_ids(s06b::hud_names, ids, 16);
    const uint32_t limit = count < 1024 ? count : 1024;
    for (uint32_t e = 0; e < limit; ++e) {
        uint32_t *id = reinterpret_cast<uint32_t *>(entries + static_cast<uintptr_t>(e) * 0x28 + 8);
        for (int k = 0; k < n; ++k) {
            if (*id == ids[k].ps4) { *id = ids[k].ps3; break; }
        }
    }
    *done = low;
}

// 4.22: score-plate sprite texture ids to their PS3 (slash-form) ids.
extern "C" U3X_FN void u3x_k47_hud(uintptr_t base, void *s, uint32_t *diag) {
    (void)base;
    if (ext_eb()[1] & 0x2000u) return;
    uint8_t *v = s06b::hud_settings(s);
    if (!v) return;
    uint8_t *score = rd<uint8_t *>(v, 0x78);
    if (!ptr_ok(score)) return;
    diag[0] = diag[0] + 1;   // every pass that gets here
    diag[2] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(score));
    s06b::IdPair ids[4];
    const int n = s06b::name_ids(s06b::plate_names, ids, 4);
    for (uintptr_t off = 0x08; off <= 0x118; off += 8) {
        uint8_t *d = rd<uint8_t *>(score, off);
        if (!ptr_ok(d)) continue;
        uint32_t *tex = reinterpret_cast<uint32_t *>(d);
        for (int k = 0; k < n; ++k) {
            if (*tex == ids[k].ps4) { *tex = ids[k].ps3; diag[1] = diag[1] + 1; break; }
        }
    }
}

// 4.23: weapon panel origin from PS4's to PS3 1.19's (32-bit patterns only).
extern "C" U3X_FN void u3x_e01_hud(uintptr_t base, void *s, uint32_t *diag) {
    (void)base;
    if (ext_eb()[1] & 0x80000000u) return;
    uint8_t *v = s06b::hud_settings(s);
    if (!v) return;
    uint8_t *w = rd<uint8_t *>(v, 0x28);
    if (!ptr_ok(w)) return;
    diag[0] = diag[0] + 1;   // every pass that gets here
    diag[2] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(w));
    uint32_t *xy = reinterpret_cast<uint32_t *>(w);
    const uint32_t x = xy[0], y = xy[1];
    const uint32_t *ps4 = u3h_e01_origin(0);
    const uint32_t *ps3 = u3h_e01_origin(1);
    if (x == ps4[0] && y == ps4[1]) {
        xy[0] = ps3[0];
        xy[1] = ps3[1];
        diag[1] = diag[1] + 1;
    } else if (!(x == ps3[0] && y == ps3[1])) {
        diag[3] = x;
    }
}

// 4.24: hook 219, the co-op grappled buddy marker uses the grapple icon pair.
extern "C" U3X_FN void u3x_ci_icon(uint64_t *r, uint32_t *diag) {
    const uint8_t *m = reinterpret_cast<const uint8_t *>(r[3]);
    const uint8_t *h = reinterpret_cast<const uint8_t *>(r[4]);   // not tested
    r[10] = rd<uint64_t>(h, 0x10e0);
    r[11] = rd<uint32_t>(h, 0x10e8);
    diag[0] = diag[0] + 1;
    if (!ptr_ok(m) || rd<uint32_t>(m, 0x50) != 2) return;
    // the u64 at +0x10bc is only 4-aligned: read it as two halves
    const uint64_t g = static_cast<uint64_t>(rd<uint32_t>(h, 0x10bc)) |
                       (static_cast<uint64_t>(rd<uint32_t>(h, 0x10c0)) << 32);
    const int32_t x = rd<int32_t>(h, 0x10c4);
    if (static_cast<uint32_t>(g) == 0 || x < 0) { diag[2] = diag[2] + 1; return; }
    r[10] = g;
    r[11] = static_cast<uint32_t>(x);
    diag[1] = diag[1] + 1;
}

// 4.25: rank-up popup sRGB gate and the timed rank-up test aid.
extern "C" U3X_FN void u3x_rank_popup_tick(uintptr_t base) {
    uint32_t *eb = ext_eb();
    uint32_t *ea = ext_ea();
    uint8_t *o = at<uint8_t *>(base + 0x18061c8);   // player 0's HUD popup object, read once

    // (1) gate: a rank-up popup on screen
    if (!(eb[3] & 0x400000u) && ptr_ok(o)) {
        const uint8_t *p = rd<uint8_t *>(o, 0x2b8);
        if (ptr_ok(p) && rd<uint32_t>(p, 0) == s06b::POPUP_RANK_UP) {
            ea[240] |= 0x20u;
            ea[166] = ea[166] + 1;
        }
    }

    // (2) test aid: push rank-up popups at 21 s, then every 12 s after match start
    if (!(eb[3] & 0x800000u)) return;
    if (ea[123] == 0) { ea[167] = 0; return; }
    if (!ptr_ok(o)) return;
    const uint32_t n = ea[167];
    if (n >= 8) return;
    const uint32_t now = at<uint32_t>(base + 0x17b4ba4);
    if (static_cast<int32_t>(now - (ea[123] & ~1u)) < static_cast<int32_t>(6300 + 3600 * n)) return;

    alignas(16) uint32_t entry[18];
    for (int k = 0; k < 18; ++k) reinterpret_cast<volatile uint32_t *>(entry)[k] = 0;
    const uint32_t word = s06b::rank_test_words[n];
    entry[0] = s06b::POPUP_RANK_UP;
    entry[4] = 0xffffffffu;
    entry[5] = 0xffffffffu;
    entry[9] = 0x40800000u;   // 4.0
    entry[11] = word;
    const uint64_t owner = base + 0x1805fa0;
    *reinterpret_cast<volatile uint64_t *>(base + 0x1806480) = owner;
    *reinterpret_cast<volatile uint64_t *>(base + 0x1b24bd8) = owner;
    game_call<void>(base + 0xc22cb0, static_cast<void *>(o), static_cast<void *>(entry));
    ea[167] = n + 1;
    ea[168] = word;
}
