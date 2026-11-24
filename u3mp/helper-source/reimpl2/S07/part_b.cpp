// S07 part B: emblem records, visitor command writers, three datasources (spec S07 4.14 to 4.18).

// ---- other groups and the C part (S07 2.2)
extern "C" uint32_t *u3x_v_command(void *visitor, uint16_t type, uint16_t words);
extern "C" void u3x_v_max(void *visitor, uint32_t index);
extern "C" int u3h_gate_closed(uintptr_t base, int32_t *ev);
extern "C" void u3h_set_text(int32_t *ev, const char *t);
extern "C" uint8_t *u3x_sym_table(void *s, uint32_t id);
extern "C" int u3x_has_digit(const char *text);
extern "C" uint32_t u3x_page_tab_text(uintptr_t base, void *s);
extern "C" void u3x_players_met_fetch(uintptr_t base, void *s);

namespace s07b {

// image offsets and ids (interface facts)
constexpr uintptr_t EMB_MANAGER = 0x17c3440;      // emblem manager (dirty byte at +0)
constexpr uintptr_t EMB_PART_IDS = 0x1b67e6c;     // three part table symbol ids, 0x18 apart
constexpr uintptr_t MODULE_NONE = 0x1b3af40;      // "find module" sentinel
constexpr uintptr_t MP_ACTIVE = 0x17d5c61;        // u8 multiplayer active
constexpr uintptr_t NET_GAME_SEL = 0x17d167c;     // s32 selected net game index
constexpr uintptr_t PLAYERS_MET_LIST = 0x75e2e0;  // game's players-met list source

constexpr uint32_t TAG_LIMIT = 0x4c414e29;
constexpr uint32_t TAG_PLAYERS_MET = 0x4c414e15;
constexpr uint32_t SYM_NET_GAMES = 0x3b9a067d;
constexpr uint32_t SYM_RULE_SETS = 0xf985c7b6;
constexpr uint32_t TAB_PLAYERS_MET = 0xacb77201;

// visitor record fields (G50 3.2)
constexpr uintptr_t V_ABORT = 0x14c, V_ACTIVE = 0x14d, V_KIND = 0x150;

// entry k of the state's native function table (may be null)
[[gnu::always_inline]] inline uintptr_t native(void *s, int k) { return rd<uintptr_t>(s, 0x300 + 8u * k); }

// the source descriptor's tag matches (descriptor pointer at ev byte 0x98, tag u32 at +0x1c)
[[gnu::always_inline]] inline bool tag_is(const int32_t *ev, uint32_t tag) {
    const uint8_t *d = rd<const uint8_t *>(ev, 0x98);
    return d && rd<uint32_t>(d, 0x1c) == tag;
}

// bounded text writer: a character goes in only while the position is before end
struct Out {
    char *p, *end;
    [[gnu::always_inline]] void c(char ch) { if (p < end) *p++ = ch; }
};

}  // namespace s07b

// ---- 4.14 emblem records

extern "C" U3X_FN uint8_t *u3x_emb_rec_slot(uintptr_t b, uint32_t sl) {
    const uintptr_t m = b + s07b::EMB_MANAGER;
    // whole byte offset formed in 32 bits (wraps), then zero-extended
    uint64_t r = at<uint64_t>(m + static_cast<uint32_t>(0x50u + 8u * sl));
    if (r == 0) r = at<uint64_t>(m + static_cast<uint32_t>(0x68u + 8u * sl));
    return reinterpret_cast<uint8_t *>(r);
}

extern "C" U3X_FN uint32_t u3x_emb_size(uintptr_t b) {
    const uintptr_t m = b + s07b::EMB_MANAGER;
    if (at<uint64_t>(m + 0x50) || at<uint64_t>(m + 0x58)) return 0x80;
    if (at<uint64_t>(m + 0x68) || at<uint64_t>(m + 0x70)) return 0x100;
    return 0;
}

// ---- 4.15 visitor commands

extern "C" U3X_FN void u3x_v_bind(void *visitor, uint32_t index) {
    uint32_t *w = u3x_v_command(visitor, 0xd, 2);
    if (!w) return;
    w[1] = index;
    u3x_v_max(visitor, index);
}

extern "C" U3X_FN void u3x_v_copy(void *visitor, uint32_t a, uint32_t b) {
    uint32_t *w = u3x_v_command(visitor, 0xf, 3);
    if (!w) return;
    w[1] = a;
    w[2] = b;
    u3x_v_max(visitor, a);
    u3x_v_max(visitor, b);
}

extern "C" U3X_FN void u3x_v_sub2(void *visitor) {
    uint8_t *v = static_cast<uint8_t *>(visitor);
    if (rd<int32_t>(v, s07b::V_KIND) != 2) return;
    if (v[s07b::V_ACTIVE]) v[s07b::V_ABORT] = 1;   // second marker while active: abort
    else v[s07b::V_ACTIVE] = 1;
}

// ---- 4.16 emblem part tables ready

extern "C" U3X_FN int u3x_emblem_ready(uintptr_t base, void *s) {
    using Find = void *(*)(uint32_t, int);
    using Init = void (*)(uint8_t *);
    const uintptr_t find = s07b::native(s, 9);
    if (!find) return 0;
    const uintptr_t init = s07b::native(s, 11);
    if (init && at<uint8_t>(base + s07b::MP_ACTIVE) && at<uint32_t>(base + s07b::EMB_PART_IDS) == 0) {
        at<uint8_t>(base + s07b::EMB_MANAGER) = 1;
        reinterpret_cast<Init>(init)(reinterpret_cast<uint8_t *>(base + s07b::EMB_MANAGER));
    }
    for (uint32_t k = 0; k < 3; ++k) {
        const uint32_t id = at<uint32_t>(base + s07b::EMB_PART_IDS + k * 0x18u);
        if (id == 0) return 0;
        // a null result counts as found; only the sentinel means missing
        if (reinterpret_cast<uintptr_t>(reinterpret_cast<Find>(find)(id, 1)) == base + s07b::MODULE_NONE) return 0;
    }
    return 1;
}

// ---- 4.17 limit row datasource (0x29)

extern "C" U3X_FN int u3x_limit_source(uintptr_t base, int32_t *ev, void *s) {
    if (!s07b::tag_is(ev, s07b::TAG_LIMIT)) return 0;
    if (u3h_gate_closed(base, ev)) return 1;
    const uintptr_t row = s07b::native(s, 22);
    if (!row) return 1;
    reinterpret_cast<void (*)(int32_t *)>(row)(ev);

    if (ev[0] != 2 || ev[0x15] != 0 || !rd<uintptr_t>(s, 656)) return 1;
    const char *label = rd<const char *>(ev, 0x40);
    if (!label || u3x_has_digit(label)) return 1;
    const uint8_t *games = u3x_sym_table(s, s07b::SYM_NET_GAMES);
    const uint8_t *rules = u3x_sym_table(s, s07b::SYM_RULE_SETS);
    if (!games || !rules) return 1;

    const int32_t sel = at<int32_t>(base + s07b::NET_GAME_SEL);   // full s32 (kept, see spec 6.4)
    if (sel < 0 || sel >= rd<int32_t>(games, 0)) return 1;
    const int32_t type = rd<int32_t>(rd<const uint8_t *>(games, 8) + static_cast<intptr_t>(sel) * 0xd0, 0xc);
    if (type < 0 || type >= rd<int32_t>(rules, 0)) return 1;
    const uint8_t *rule = rd<const uint8_t *>(rules, 8) + static_cast<intptr_t>(type) * 0x58;

    const int64_t v = static_cast<int64_t>(rd<int32_t>(rule, 0x24)) * ev[1] + rd<int32_t>(rule, 0x1c);
    char *buf = reinterpret_cast<char *>(static_cast<uint8_t *>(s) + 0x2c00);
    s07b::Out o{buf, buf + 120};
    uint64_t mag = static_cast<uint64_t>(v);
    if (v < 0) { o.c('-'); mag = 0 - mag; }
    char digits[20];
    int n = 0;
    do { digits[n++] = static_cast<char>('0' + mag % 10); mag /= 10; } while (mag);
    while (n) o.c(digits[--n]);
    o.c(' ');                                  // always, even for an empty label
    for (const char *t = label; *t; ++t) o.c(*t);
    *o.p = 0;
    u3h_set_text(ev, buf);
    return 1;
}

// ---- 4.18 players met datasource

extern "C" U3X_FN int u3x_players_met_source(uintptr_t base, int32_t *ev, void *s) {
    if (!s07b::tag_is(ev, s07b::TAG_PLAYERS_MET)) return 0;
    if (u3h_gate_closed(base, ev)) return 1;
    if (ev[0] == 9 && u3x_page_tab_text(base, s) == s07b::TAB_PLAYERS_MET) {
        u3x_players_met_fetch(base, s);
        return 1;
    }
    game_call<void>(base + s07b::PLAYERS_MET_LIST, ev);
    return 1;
}
