// S03 part 3: lobby scene and character (scene shutdown, live team side, character art name, custom game selection).
// Written from spec S03_lobby_leftovers.md sections 4.17 to 4.20. No float anywhere.

// ---- called functions of the C part and of other groups (spec 2.2)
extern "C" uint32_t u3h_crc_sid(const char *text);
extern "C" uint8_t *u3x_sym_table(void *s, uint32_t id);
extern "C" uint8_t *u3x_pm_ngentry(uintptr_t base, void *s);
extern "C" void u3x_custom_side(int32_t *custom, int side, int inplace, const void *shown, uint32_t shown_id,
                                void (*refresh)(int32_t *custom));
extern "C" void u3h_cg_log(const char *text, int32_t value);

namespace s03_p3 {

// game image offsets (interface facts of spec 3.3)
constexpr uintptr_t SCENE = 0x17bba90;        // menu scene SC
constexpr uintptr_t MENU = 0x17bd080;         // the menu object
constexpr uintptr_t NGM = 0x17cb060;          // net game manager
constexpr uintptr_t SELECTED_NG = 0x17d167c;  // s16 selected net game (= NGM + 0x661c)
constexpr uintptr_t MATCH_STATE = 0x17d1868;
constexpr uintptr_t NET_MENU_MODE = 0x17d603c;
constexpr uintptr_t CM = 0x17f9938;           // customisation manager, word 0 = side
constexpr uintptr_t FRONT_END = 0x1b68810;

constexpr uintptr_t SCENE_LEAVE = 0x5b7040;   // (SC, 1)
constexpr uintptr_t SCENE_TEAM_PASS = 0x5b6e40;
constexpr uintptr_t CHAR_EXIT = 0x5b47c0;
constexpr uintptr_t CHAR_DESTROY = 0x5b6c40;
constexpr uintptr_t PLAYER_TEAM = 0x7927c0;
constexpr uintptr_t CM_REFRESH = 0x995d50;
constexpr uintptr_t SYMBOL_VALUE = 0x11f2bf0;

constexpr uint32_t SYM_NET_GAMES = 0x3b9a067d;
constexpr uint32_t SYM_CHARACTERS = 0xbd3bb3b9;

U3X_RO const char name_none[] = "none";
U3X_RO const char log_made[] = "lobby selection made the match's net game: ";
U3X_RO const char log_was[] = "  ... it was net game ";

// own diagnostics of e03_side: D[0] side changes, D[1] exits started, D[2] characters destroyed, D[3] exit cap hits,
// D[4] exits cancelled because the lobby went away, D[5] last change (side | old << 8 | side << 16), D[6] 0 idle /
// 1 exiting, D[7] exit frames
U3X_BSS uint32_t side_d[8];

inline uint8_t *ptr_at(const void *p, uintptr_t off) { return rd<uint8_t *>(p, off); }

// "usable" of 4.19: inside the user address range, any alignment
inline bool usable(const void *p) {
    const uintptr_t v = reinterpret_cast<uintptr_t>(p);
    return v >= 0x10000u && v < (1ull << 47);
}

// the scene character's object when the scene's handle is valid, else null
inline uint8_t *scene_character(uintptr_t base) {
    const uint8_t *sc = reinterpret_cast<const uint8_t *>(base + SCENE);
    uint8_t *h = ptr_at(sc, 0x1460);
    const uint32_t id = rd<uint32_t>(sc, 0x1468);
    if (!h || !id || !ptr_ok(h)) return nullptr;
    uint8_t *obj = ptr_at(h, 0);
    if (!ptr_ok(obj) || rd<uint32_t>(obj, 0x7c) != id) return nullptr;
    return obj;
}

// the lobby is up and settled: menu up and idle, scene in state 2 / phase 2, not in a match, not in Start Game
inline bool lobby_ok(uintptr_t base) {
    const uint8_t *menu = reinterpret_cast<const uint8_t *>(base + MENU);
    const uint8_t *sc = reinterpret_cast<const uint8_t *>(base + SCENE);
    if (rd<int32_t>(menu, 8) == 0 || rd<int32_t>(menu, 0xc) != 0) return false;
    if (rd<int32_t>(sc, 0) != 2 || rd<int32_t>(sc, 4) != 2) return false;
    if (at<uint32_t>(base + MATCH_STATE) == 2) return false;
    const uint32_t fe = at<uint32_t>(base + FRONT_END);
    return fe < 0x2c || fe > 0x2e;
}

}  // namespace s03_p3

// 4.17: PS3 NetShutdown's scene part: leave the menu scene, then its team pass
extern "C" U3X_FN void u3x_lobby_scene_shutdown(uintptr_t base) {
    using namespace s03_p3;
    if (!base || (ext_eb()[1] & (1u << 3))) return;
    void *sc = reinterpret_cast<void *>(base + SCENE);
    game_call<void>(base + SCENE_LEAVE, sc, 1);
    game_call<void>(base + SCENE_TEAM_PASS, sc);
    ext_eb()[231] += 1;
}

// 4.18: keep CM's side equal to the local player's team in the lobby and re-place the scene character
extern "C" U3X_FN void u3x_e03_side(uintptr_t base, void *s) {
    using namespace s03_p3;
    uint32_t *d = side_d;
    const uint32_t eb1 = ext_eb()[1];
    if (eb1 & (1u << 29)) { d[6] = 0; return; }

    const bool ok = lobby_ok(base);
    uint8_t *ch = scene_character(base);
    if (d[6] == 1 && (!ok || !ch)) {
        d[6] = 0;
        if (!ok) d[4] += 1;
    }
    if (!ok) return;

    uint8_t *sc = reinterpret_cast<uint8_t *>(base + SCENE);
    int32_t *cm = reinterpret_cast<int32_t *>(base + CM);
    uint8_t *player = ptr_at(reinterpret_cast<void *>(base + NGM), 0x370);
    if (ptr_ok(player) && u3x_pm_ngentry(base, s)) {
        const int32_t side = game_call<int32_t>(base + PLAYER_TEAM, player);
        if ((side == 0 || side == 1) && side != cm[0]) {
            const uint32_t us = static_cast<uint32_t>(side);
            d[5] = us | (static_cast<uint32_t>(cm[0]) & 0xff) << 8 | (us & 0xffff) << 16;
            const int inplace = (ext_eb()[1] >> 28) & 1;   // read here, after the team call
            u3x_custom_side(cm, side, inplace, ptr_at(sc, 0x1460), rd<uint32_t>(sc, 0x1468),
                            reinterpret_cast<void (*)(int32_t *)>(base + CM_REFRESH));
            d[0] += 1;
            if (ch && inplace == 0 && d[6] == 0) { d[6] = 1; d[7] = 0; d[1] += 1; }
        }
    }

    if (d[6] != 1 || !ch) return;
    game_call<void>(base + CHAR_EXIT, sc);
    bool busy = false;
    uint8_t *o = ptr_at(ch, 0x710);
    if (ptr_ok(o)) {
        uint8_t *vt = ptr_at(o, 0);
        if (ptr_ok(vt)) busy = static_cast<uint8_t>(game_call<uint64_t>(rd<uintptr_t>(vt, 0x60), o)) != 0;
    }
    if (busy && ++d[7] < 240) return;   // exit still playing: wait, at most 240 frames
    if (busy) d[3] += 1;
    game_call<void>(base + CHAR_DESTROY, sc);
    d[2] += 1;
    d[6] = 0;
}

// 4.19: the art name of character slot 0 for a character Init descriptor (desc[0] character, desc[2] part), or null
extern "C" U3X_FN const char *u3x_lg01_slot0(uintptr_t base, const uint8_t *desc) {
    using namespace s03_p3;
    uint8_t **x = game_call<uint8_t **>(base + SYMBOL_VALUE, SYM_CHARACTERS);
    if (!usable(x) || !usable(*x)) return nullptr;
    const uint8_t *table = *x;
    int32_t i = desc[0];
    if (!(i < rd<int32_t>(table, 0))) i = 0;
    const uint8_t *chars = ptr_at(table, 8);
    if (!usable(chars)) return nullptr;
    const uint8_t *l = ptr_at(chars + static_cast<uintptr_t>(i) * 0x70, 0x28);
    if (!usable(l)) return nullptr;
    const uint8_t *sym = ptr_at(l, 8);
    if (!usable(sym)) return nullptr;
    uint8_t **y = game_call<uint8_t **>(base + SYMBOL_VALUE, rd<uint32_t>(sym, 0));
    if (!usable(y) || !usable(*y)) return nullptr;
    const uint8_t *list = *y;
    int32_t j = desc[2];
    if (!(j < rd<int32_t>(list, 0))) j = 0;
    const uint8_t *parts = ptr_at(list, 8);
    if (!usable(parts)) return nullptr;
    const uint8_t *part = parts + static_cast<uintptr_t>(j) * 0x38;
    if (rd<uint32_t>(part, 0x10) == u3h_crc_sid(name_none)) return nullptr;
    const char *name = rd<const char *>(part, 8);
    return usable(name) ? name : nullptr;
}

// 4.20: make the lobby's net game selection the match's net game. The selection and the selected net game are the
// same word in the game, so want = was and the select call never runs there; the full rule is kept (spec 6.1).
extern "C" U3X_FN void u3x_cg_commit_selection(uintptr_t base, void *s) {
    using namespace s03_p3;
    if (at<uint32_t>(base + NET_MENU_MODE) != 2) return;
    uint8_t *table = u3x_sym_table(s, SYM_NET_GAMES);
    using Select = void (*)(void *ngm, int32_t index, const void *kept, uint32_t size);
    Select sel = rd<Select>(s, 768 + 2 * 8);
    if (!table || !sel) return;

    uint8_t *ngm = reinterpret_cast<uint8_t *>(base + NGM);
    const uint8_t *entries = ptr_at(table, 8);
    const uint32_t n = rd<uint32_t>(table, 0);
    const int32_t want = at<int16_t>(base + SELECTED_NG);
    const int32_t was = rd<int16_t>(ngm, 0x661c);
    if (want < 0 || static_cast<uint32_t>(want) >= n || want == was || !entries) return;

    const bool same = was >= 0 && static_cast<uint32_t>(was) < n &&
                      rd<uint32_t>(entries + static_cast<uintptr_t>(was) * 0xd0, 0xc) ==
                          rd<uint32_t>(entries + static_cast<uintptr_t>(want) * 0xd0, 0xc);
    uint64_t kept[3];
    for (int k = 0; k < 3; ++k) kept[k] = rd<uint64_t>(ngm, 0x6a38 + 8 * k);
    sel(ngm, want, same ? kept : nullptr, 0xc);
    u3h_cg_log(log_made, want);
    u3h_cg_log(log_was, was);
}
