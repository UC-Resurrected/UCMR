// S03 part 2: the lobby stage (unwant, heap check, stage guard, match cleanup). Written from
// spec\S03_lobby_leftovers.md sections 3 and 4.13 to 4.16 (natives from G39 3.5).

// ---- functions of other parts, other groups and the C part (prototypes as the specs give them)
extern "C" {
// part 1
int u3x_retag_page(uint8_t *page, uint32_t count, int opts);
void u3x_retag_party(uint8_t *page);
void u3x_retag_lobby_box(uint8_t *page, int off);
void u3x_retag_page_source(uint8_t *page);
void u3x_retag_treasure_list(uint8_t *page);
void u3x_retag_cinema_page(uint8_t *page);
int u3x_native_gate_closed(uintptr_t base, int32_t *event);
int u3x_lobby_button_shown(int setup_button, int guest);
uint32_t u3x_lobby_button_label(int setup_button, int guest, int custom_game, uint32_t own_label);
int u3x_rpm_source(uintptr_t base, int32_t *event, void *s, uint8_t *eu_row);
int u3x_k04_blank(uintptr_t base, uint8_t *row);
int u3x_register_lobby_click(uintptr_t base);
// part 3
void u3x_lobby_scene_shutdown(uintptr_t base);
void u3x_e03_side(uintptr_t base, void *s);
const char *u3x_lg01_slot0(uintptr_t base, const uint8_t *desc);
void u3x_cg_commit_selection(uintptr_t base, void *s);
// other groups and the C part
uint32_t u3h_crc_sid(const char *text);
int u3x_mp_lan_or_online(uintptr_t base);
int32_t u3x_loc_now(uintptr_t base);
uint8_t *u3x_netgame_cur(uintptr_t base, void *s);
// part 2 (this file)
int u3x_stage_unwant(uintptr_t base, uint32_t sid);
uint32_t u3x_stage_wont_fit(uintptr_t base, uint8_t *level);
void u3x_stage_guard(uintptr_t base, void *s);
void u3x_match_stage_cleanup(uintptr_t base, void *s, void *find, void *show);
}

namespace s03_part2 {

// the fallback lobby stage's name; map entries keep pointing at it for the rest of the session
U3X_RO const char fallback_stage[] = "san-upper-multi-stage";

constexpr uintptr_t LEVEL_MANAGER = 0x1afcf10;
constexpr uintptr_t STAGE_NAME = 0x17d2518;
constexpr uintptr_t HEAP_TOTAL = 0x16e7be0;
constexpr uintptr_t HEAP_USED = 0x16e7bf0;
constexpr uintptr_t HEAP_PEAK = 0x16e7cac;
constexpr uintptr_t HEAP_PACKAGE_MAX = 0x18bed94;
constexpr uint32_t MAP_TABLE = 0x0dec97a4;
constexpr uint32_t ENTRY = 0x18;   // want list entry size
constexpr uint32_t MAP_ENTRY = 0x98;

// natives (G39 3.5)
using SymbolFn = uint8_t *(*)(uint32_t id);
using HashFn = uint64_t (*)(const char *name);
using RequestFn = void (*)(void *manager, void *block);
using FindFn = uint8_t *(*)(void *manager, uint32_t sid);
using ShowFn = void (*)(void *level, int show);

U3X_FN void *native(void *s, int k) {
    return rd<void *>(s, 768 + 8 * static_cast<uintptr_t>(k));
}

U3X_FN uint32_t *stg() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0xb00); }
U3X_FN uint32_t *bad() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0xb40); }
U3X_FN uint32_t *sw() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0x2130); }

// a want list of the level manager: count at +cnt, entries pointer at +cnt + 8; null when it must be skipped
U3X_FN uint8_t *want_list(uintptr_t base, uintptr_t cnt, uint32_t &count) {
    uint8_t *lm = reinterpret_cast<uint8_t *>(base + LEVEL_MANAGER);
    uint8_t *entries = rd<uint8_t *>(lm, cnt + 8);
    count = rd<uint32_t>(lm, cnt);
    if (!ptr_ok(entries) || count > 4096) return nullptr;
    return entries;
}

U3X_FN bool in_list(uintptr_t base, uintptr_t cnt, uint32_t sid) {
    uint32_t count;
    uint8_t *entries = want_list(base, cnt, count);
    if (!entries) return false;
    for (uint32_t i = 0; i < count; ++i)
        if (rd<uint32_t>(entries, i * ENTRY + 8) == sid) return true;
    return false;
}

// true when the level want list (+0x28) or the actor want list (+0x38) holds this sid
U3X_FN bool wanted(uintptr_t base, uint32_t sid) {
    return in_list(base, 0x28, sid) || in_list(base, 0x38, sid);
}

// removes every entry with this sid from one list; answers the number removed
U3X_FN int unwant_list(uintptr_t base, uintptr_t cnt, uint32_t sid) {
    uint32_t count;
    uint8_t *entries = want_list(base, cnt, count);
    if (!entries) return 0;
    int removed = 0;
    uint32_t i = 0;
    while (i < count) {
        if (rd<uint32_t>(entries, i * ENTRY + 8) != sid) { ++i; continue; }
        for (uint32_t j = i; j + 1 < count; ++j) {          // later entries down by one, front to back
            uint64_t *to = reinterpret_cast<uint64_t *>(entries + j * ENTRY);
            const uint64_t *from = to + 3;
            to[0] = from[0];
            to[1] = from[1];
            to[2] = from[2];
        }
        --count;
        *reinterpret_cast<uint32_t *>(base + LEVEL_MANAGER + cnt) = count;
        ++removed;
    }
    return removed;
}

// the map table (script symbol through native 13) when it is usable, else null; count and entries out
// the map table V (out) and its entries; null unless both plausible and 0 < count < 512
U3X_FN uint8_t *map_entries(void *s, int32_t &count, uint8_t *&v) {
    SymbolFn sym = reinterpret_cast<SymbolFn>(native(s, 13));
    v = sym ? sym(MAP_TABLE) : nullptr;
    if (!ptr_ok(v)) return nullptr;
    uint8_t *e = rd<uint8_t *>(v, 8);
    count = rd<int32_t>(v, 0);
    if (!ptr_ok(e) || count <= 0 || count >= 512) return nullptr;
    return e;
}

U3X_FN uint8_t *map_entries(void *s, int32_t &count) {
    uint8_t *v = nullptr;
    return map_entries(s, count, v);
}

}  // namespace s03_part2

// 4.13
extern "C" U3X_FN int u3x_stage_unwant(uintptr_t base, uint32_t sid) {
    int removed = s03_part2::unwant_list(base, 0x28, sid);
    removed += s03_part2::unwant_list(base, 0x38, sid);
    return removed;
}

// 4.14: pages a waiting package needs when they will not fit in the loading heap, else 0
extern "C" U3X_FN uint32_t u3x_stage_wont_fit(uintptr_t base, uint8_t *level) {
    using namespace s03_part2;
    uint32_t *g = stg();
    const uint32_t total = at<uint32_t>(base + HEAP_TOTAL);
    const uint32_t used = static_cast<uint32_t>(at<uint64_t>(base + HEAP_USED));
    g[8] = total;
    g[9] = used;
    g[10] = at<uint32_t>(base + HEAP_PEAK);
    const uint32_t free_pages = total > used ? total - used : 0;
    g[6] = free_pages;
    const uint32_t n = rd<uint32_t>(level, 0x78);
    if (n > 16 || total == 0 || total > 4096) return 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t *p = rd<uint8_t *>(level, 0x80 + 8 * static_cast<uintptr_t>(i));
        if (!ptr_ok(p) || rd<uint32_t>(p, 0x20) != 7 || rd<uint16_t>(p, 0x238) != 0xa79) continue;
        const uint32_t need = rd<uint32_t>(p, 0x248);
        g[7] = need;
        if (need >= at<uint32_t>(base + HEAP_PACKAGE_MAX) || need + 10u > free_pages) return need;   // 0 pages: "fits"
    }
    return 0;
}

// 4.15: falls back to the retail lobby stage when the lobby's stage fails, cannot fit or takes too long
extern "C" U3X_FN void u3x_stage_guard(uintptr_t base, void *s) {
    using namespace s03_part2;
    FindFn find = reinterpret_cast<FindFn>(native(s, 42));
    ShowFn show = reinterpret_cast<ShowFn>(native(s, 45));
    HashFn hash = reinterpret_cast<HashFn>(native(s, 40));
    RequestFn request = reinterpret_cast<RequestFn>(native(s, 41));
    if (!find || !show || !hash || !request) return;
    if (u3x_mp_lan_or_online(base) == 0) return;
    if (rd<uint32_t>(s, 0x20c0) & 0x40) return;

    uint32_t *g = stg();
    void *lm = reinterpret_cast<void *>(base + LEVEL_MANAGER);
    char *name = reinterpret_cast<char *>(base + STAGE_NAME);
    g[15] += 1;
    if (name[0] == 0) { g[0] = 0; return; }
    uint32_t sid = u3h_crc_sid(name);
    const uint32_t fsid = u3h_crc_sid(fallback_stage);

    uint8_t *level = find(lm, sid);
    if (!ptr_ok(level)) level = nullptr;
    if (g[13] != 0 && sid == fsid && level) {   // the fallback was not there yet when it was requested
        show(level, 1);
        level[2] |= 0x10;
        g[13] = 0;
    }

    if (!wanted(base, sid)) { g[0] = 0; return; }
    const int32_t now = u3x_loc_now(base);
    if (sid != g[0]) { g[0] = sid; g[1] = static_cast<uint32_t>(now); g[2] = 0; g[7] = 0; }

    const uint32_t st = level ? rd<uint32_t>(level, 0x30) : 0xff;
    g[14] = st;
    if (st == 8) { g[1] = static_cast<uint32_t>(now); g[2] = 0; return; }
    g[2] += 1;

    uint32_t reason;
    if (st == 0xc) reason = 1;
    else if (level && st >= 2 && st < 8 && u3x_stage_wont_fit(base, level) != 0) reason = 3;
    else if (static_cast<uint32_t>(now) - g[1] >= 6000 || g[2] >= 1500) reason = 2;
    else return;

    g[4] = sid;
    g[5] = reason;
    if (sid == fsid) { g[12] = 1; return; }   // the fallback itself fails: give up

    // the loader's mode 3: hide and unwant the failing stage
    if (level) show(level, 0);
    u3x_stage_unwant(base, sid);
    uint32_t *b = bad();
    bool known = false;
    for (int i = 0; i < 16; ++i) if (b[i] == sid) known = true;
    if (!known) {
        for (int i = 0; i < 16; ++i) if (b[i] == 0) { b[i] = sid; break; }
    }

    // map entries with this stage now name the fallback
    int32_t count = 0;
    if (uint8_t *e = map_entries(s, count)) {
        for (int32_t i = 0; i < count; ++i) {
            uint8_t *m = e + static_cast<uintptr_t>(i) * MAP_ENTRY;
            if (rd<uint32_t>(m, 0x88) != sid) continue;
            *reinterpret_cast<const char **>(m + 0x80) = fallback_stage;
            *reinterpret_cast<uint32_t *>(m + 0x88) = fsid;
            g[11] += 1;
        }
    }

    // the loader's mode 1: name, want request, show
    for (int i = 0;; ++i) {
        name[i] = fallback_stage[i];
        if (fallback_stage[i] == 0) break;
    }
    alignas(8) uint8_t block[ENTRY];
    for (uint32_t i = 0; i < ENTRY; ++i) block[i] = 0;
    *reinterpret_cast<uint64_t *>(block) = hash(fallback_stage);
    *reinterpret_cast<uint32_t *>(block + 8) = fsid;
    block[0xe] = 3;
    request(lm, block);
    uint8_t *f = find(lm, fsid);
    if (ptr_ok(f)) {
        show(f, 1);
        f[2] |= 0x10;
        g[13] = 0;
    } else {
        g[13] = 1;
    }
    g[0] = fsid;
    g[1] = static_cast<uint32_t>(now);
    g[2] = 0;
    g[3] += 1;
}

// 4.16: in a match, hides and unwants the lobby stages of the maps not played
extern "C" U3X_FN void u3x_match_stage_cleanup(uintptr_t base, void *s, void *find, void *show) {
    using namespace s03_part2;
    uint32_t *w = sw();
    w[16] += 1;
    uint8_t *game = u3x_netgame_cur(base, s);
    int32_t count = 0;
    uint8_t *v = nullptr;
    uint8_t *e = map_entries(s, count, v);
    if (!game || !e) { w[20] += 1; return; }

    // the range test above is made once; the count is read again before every entry (it may change during a call)
    const int32_t map = rd<int32_t>(game, 4);
    uint32_t cur = 0;
    for (int32_t i = 0; i < rd<int32_t>(v, 0); ++i) {
        uint8_t *m = e + static_cast<uintptr_t>(i) * MAP_ENTRY;
        if (rd<int32_t>(m, 0) == map) { cur = rd<uint32_t>(m, 0x88); break; }
    }
    if (cur == 0) { w[20] += 1; return; }
    w[18] = cur;

    void *lm = reinterpret_cast<void *>(base + LEVEL_MANAGER);
    for (int32_t i = 0; i < rd<int32_t>(v, 0); ++i) {
        const uint32_t sid = rd<uint32_t>(e + static_cast<uintptr_t>(i) * MAP_ENTRY, 0x88);
        if (sid == 0 || sid == cur || !wanted(base, sid)) continue;
        uint8_t *level = reinterpret_cast<FindFn>(find)(lm, sid);
        if (ptr_ok(level)) reinterpret_cast<ShowFn>(show)(level, 0);
        w[17] += static_cast<uint32_t>(u3x_stage_unwant(base, sid));
        w[19] = sid;
    }
}
