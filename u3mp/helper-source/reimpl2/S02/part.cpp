// S02: level and actor wants (spec S02_level_actor_wants.md). Keeps the PS3 session-wide packages wanted (the
// "multi-all" level, the actors of PS3's multi-all group, anim-proto-multiplayer) with the want-list keep bit, drops
// them at NetShutdown, puts back PS3's co-op Shade animation want, and guards the match setup's emblem setup.

// ---- functions of other groups and of the C part (spec 2.2)
extern "C" uint8_t *u3x_level_want_entry(uintptr_t base, uint32_t sid);
extern "C" int u3x_actor_want(uintptr_t base, void *s, const char *name);
extern "C" uint32_t u3x_level_state(uintptr_t base, void *s, uint32_t sid);
extern "C" int32_t u3x_netgame_type(uintptr_t base, void *s);
extern "C" int u3h_stage_unwant(uintptr_t base, uint32_t sid);
extern "C" uint32_t u3h_crc_sid(const char *text);
extern "C" void u3h_so_note(const char *a, const char *name, int32_t v, const char *b);
extern "C" const char *u3h_multi_all_actor(uint32_t i);

namespace s02 {

// game image offsets (spec 3.1, 3.2, 3.5)
constexpr uintptr_t LEVEL_MGR = 0x1afcf10;        // the level manager: slot array pointer at +0, bitmap at +8
constexpr uintptr_t ACTOR_COUNT = 0x1afcf48;      // actor want list: count
constexpr uintptr_t ACTOR_CAP = 0x1afcf4c;        // actor want list: capacity
constexpr uintptr_t ACTOR_LIST = 0x1afcf50;       // actor want list: entries
constexpr uintptr_t MATCH_STATE = 0x17d1868;      // net game manager's match state (2 = in a match)
constexpr uintptr_t EMBLEM_STATE = 0x17c3440;     // emblem state, first byte = dirty
constexpr uintptr_t EMBLEM_TABLE_ID0 = 0x1b67e6c; // module id of the emblem table's first entry (0 = not filled)
constexpr uintptr_t NO_MODULE = 0x1b3af40;        // native 9's "no module" sentinel
constexpr uintptr_t PROTO_NAME = 0x145f915;       // the text anim-proto-multiplayer

constexpr uint32_t SLOT_COUNT = 0xd2;
constexpr uint32_t SLOT_SIZE = 0x250;
constexpr uint32_t ENTRY_SIZE = 0x18;
constexpr uint32_t LIST_MAX = 4096;
constexpr uint8_t KEEP = 2;                       // keep bit of a want entry's flags byte

constexpr uint32_t SID_PROTO = 0x67adb399;        // anim-proto-multiplayer
constexpr uint32_t SID_MULTI_ALL = 0x240b27b8;    // multi-all (level)
constexpr int32_t NETGAME_COOP_SHADE = 8;

// switch bits of eb[1]
constexpr uint32_t SW_PROTO_OFF = 0x10;
constexpr uint32_t SW_WEAPON_SET = 0x04000000;
constexpr uint32_t SW_MULTI_ALL_OFF = 0x08000000;
constexpr uint32_t WEAPON_SET_SIZE = 12;
constexpr uint32_t MAX_NAMES = 64;

// own texts
U3X_RO const char multi_all_name[] = "multi-all";
U3X_RO const char shade_anim_name[] = "anim-shade-mp";
U3X_RO const char empty_name[] = "";
U3X_RO const char level_note[] = "multi-all level wanted (common-multi sound bank, kill chime), flags ";
// where the emblem table init loads its three module ids as immediates (spec 3.5)
U3X_RO const uint32_t emblem_id_at[3] = {0x66b756, 0x66b808, 0x66b8aa};

// diagnostics (spec 3.7)
U3X_BSS uint32_t ma_d[8];
U3X_BSS uint32_t ma_x[8];
U3X_BSS uint32_t ma_sid[64];
U3X_BSS uint32_t ma_n;
U3X_BSS uint32_t ml_d[4];

[[gnu::always_inline]] inline uint32_t switches() { return ext_eb()[1]; }
[[gnu::always_inline]] inline uint32_t &sw_word(uint32_t off) { return *reinterpret_cast<uint32_t *>(u3h_ext() + off); }
[[gnu::always_inline]] inline uintptr_t native(void *s, uint32_t k) { return rd<uintptr_t>(s, 768 + 8 * k); }
[[gnu::always_inline]] inline uint8_t &flags_of(uint8_t *e) { return e[0xe]; }

}  // namespace s02

// 4.1: the first actor want list entry with this sid, or null
extern "C" U3X_FN uint8_t *u3x_actor_want_entry(uintptr_t base, uint32_t sid) {
    using namespace s02;
    const uint32_t count = at<uint32_t>(base + ACTOR_COUNT);
    uint8_t *list = at<uint8_t *>(base + ACTOR_LIST);
    if (!ptr_ok(list) || count > LIST_MAX) return nullptr;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t *e = list + ENTRY_SIZE * i;
        if (rd<uint32_t>(e, 8) == sid) return e;
    }
    return nullptr;
}

// 4.2: want anim-proto-multiplayer and give its entry the keep bit
extern "C" U3X_FN void u3x_proto_keep(uintptr_t base, void *s) {
    using namespace s02;
    if (switches() & SW_PROTO_OFF) return;
    uint8_t *e = u3x_actor_want_entry(base, SID_PROTO);
    if (!e) {
        if (u3x_actor_want(base, s, reinterpret_cast<const char *>(base + PROTO_NAME)) == 2) ext_eb()[44]++;
        e = u3x_actor_want_entry(base, SID_PROTO);
    }
    if (e && !(flags_of(e) & KEEP)) {
        flags_of(e) |= KEEP;
        ext_eb()[43]++;
    }
}

// 4.3: clear the proto entry's keep bit and remove it (with or without the bit)
extern "C" U3X_FN void u3x_proto_drop(uintptr_t base) {
    using namespace s02;
    if (switches() & SW_PROTO_OFF) return;
    uint8_t *e = u3x_actor_want_entry(base, SID_PROTO);
    if (!e) return;
    flags_of(e) &= static_cast<uint8_t>(~KEEP);
    ext_eb()[45] += static_cast<uint32_t>(u3h_stage_unwant(base, SID_PROTO));
}

// 4.4: the loaded actor slot with this sid (the game's walk without its assertion); a non-actor match is noted
extern "C" U3X_FN uint8_t *u3x_actor_find(uintptr_t base, uint32_t sid) {
    using namespace s02;
    uint8_t *slots = at<uint8_t *>(base + LEVEL_MGR);
    if (!ptr_ok(slots)) return nullptr;
    for (uint32_t i = 0; i < SLOT_COUNT; i++) {
        const uint64_t used = at<uint64_t>(base + LEVEL_MGR + 8 + 8 * (i / 64));
        if (!((used >> (i % 64)) & 1)) continue;
        uint8_t *slot = slots + SLOT_SIZE * i;
        if (rd<uint32_t>(slot, 0x74) != sid) continue;
        if (rd<uint16_t>(slot, 0) == 2) return slot;
        ma_d[7] = sid;
    }
    return nullptr;
}

// 4.5: want the multi-all actors and keep them wanted; record how many are loaded
extern "C" U3X_FN void u3x_multi_all_actors_keep(uintptr_t base, void *s) {
    using namespace s02;
    if (switches() & SW_MULTI_ALL_OFF) return;

    if (ma_n == 0) {   // learn the list's sids once
        uint32_t taken = 0;
        while (taken < MAX_NAMES) {
            const char *name = u3h_multi_all_actor(taken);
            if (!name || !name[0]) break;
            ma_sid[taken] = u3h_crc_sid(name);
            taken++;
        }
        ma_n = taken;
    }

    const uint32_t n = (switches() & SW_WEAPON_SET) ? WEAPON_SET_SIZE : ma_n;
    uint32_t loaded = 0, miss = 0, present = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t sid = ma_sid[i];
        uint8_t *e = u3x_actor_want_entry(base, sid);
        if (!e && !u3x_level_want_entry(base, sid)) {
            // past the list's end (bit 26 with a short list, spec open question 3) the old code passed an empty name
            const char *name = u3h_multi_all_actor(i);
            const int r = u3x_actor_want(base, s, name ? name : empty_name);
            if (r == 2) ma_d[0]++;
            else if (r == 0) ma_d[2]++;
            e = u3x_actor_want_entry(base, sid);
        }
        if (e && !(flags_of(e) & KEEP)) ma_d[1]++;

        // keep bit on every entry of this sid, duplicates included
        const uint32_t count = at<uint32_t>(base + ACTOR_COUNT);
        uint8_t *list = at<uint8_t *>(base + ACTOR_LIST);
        if (ptr_ok(list) && count <= LIST_MAX) {
            for (uint32_t j = 0; j < count; j++) {
                uint8_t *x = list + ENTRY_SIZE * j;
                if (rd<uint32_t>(x, 8) == sid) flags_of(x) |= KEEP;
            }
        }

        uint8_t *slot = u3x_actor_find(base, sid);
        if (ptr_ok(slot) && rd<uint32_t>(slot, 0x30) == 8) {
            loaded++;
        } else if (miss == 0) {
            miss = sid;
            ma_x[2] = ptr_ok(slot) ? ((rd<uint32_t>(slot, 0x30) & 0xff) | static_cast<uint32_t>(rd<uint16_t>(slot, 0)) << 8)
                                   : 0xffffu;
            if (e) {
                const uintptr_t first = at<uintptr_t>(base + ACTOR_LIST);
                const uint32_t index = static_cast<uint32_t>((reinterpret_cast<uintptr_t>(e) - first) / ENTRY_SIZE);
                ma_x[3] = flags_of(e) | static_cast<uint32_t>(rd<uint16_t>(e, 0xc)) << 8 | index << 16;
            } else {
                ma_x[3] = 0xffffffffu;
            }
        }
        if (e) present++;
    }

    ma_d[4] = loaded;
    ma_d[5] = n;
    ma_d[6] = miss;
    ma_x[0] = at<uint32_t>(base + ACTOR_COUNT);
    ma_x[1] = at<uint32_t>(base + ACTOR_CAP);
    ma_x[4] = present;
    ma_x[5]++;
    uint8_t *proto = u3x_actor_want_entry(base, SID_PROTO);
    ma_x[6] = proto ? flags_of(proto) : 0xffu;
}

// 4.6: remove the remembered multi-all actors that still carry the keep bit
extern "C" U3X_FN void u3x_multi_all_actors_drop(uintptr_t base) {
    using namespace s02;
    if (switches() & SW_MULTI_ALL_OFF) return;
    for (uint32_t i = 0; i < ma_n; i++) {
        uint8_t *e = u3x_actor_want_entry(base, ma_sid[i]);
        if (e && (flags_of(e) & KEEP)) {
            flags_of(e) &= static_cast<uint8_t>(~KEEP);
            ma_d[3] += static_cast<uint32_t>(u3h_stage_unwant(base, ma_sid[i]));
        }
    }
}

// 4.7: request the multi-all level (outside a match) and keep it wanted
extern "C" U3X_FN void u3x_multi_all_level_keep(uintptr_t base, void *s) {
    using namespace s02;
    if (switches() & SW_MULTI_ALL_OFF) return;
    uint8_t *e = u3x_level_want_entry(base, SID_MULTI_ALL);
    if (!e && at<uint32_t>(base + MATCH_STATE) != 2) {
        const uintptr_t hash = native(s, 40);
        const uintptr_t request = native(s, 41);
        if (!hash || !request) return;
        uint64_t block[3] = {0, 0, 0};   // 0x18-byte request block
        block[0] = game_call<uint64_t>(hash, static_cast<const char *>(multi_all_name));
        block[1] = SID_MULTI_ALL | static_cast<uint64_t>(0x0a) << 48;   // sid at 8, flags 0x0a at 0xe
        game_call<void>(request, reinterpret_cast<void *>(base + LEVEL_MGR), static_cast<void *>(block));
        ml_d[0]++;
        e = u3x_level_want_entry(base, SID_MULTI_ALL);
        if (ml_d[0] == 1) u3h_so_note(level_note, nullptr, e ? flags_of(e) : 0xff, nullptr);
    }
    if (e && !(flags_of(e) & KEEP)) {
        flags_of(e) |= KEEP;
        ml_d[1]++;
    }
    const uint32_t state = u3x_level_state(base, s, SID_MULTI_ALL);
    ml_d[3] = (e ? flags_of(e) : 0xffu) | state << 8;
}

// 4.8: clear the multi-all level's keep bit and remove it (with or without the bit)
extern "C" U3X_FN void u3x_multi_all_level_drop(uintptr_t base) {
    using namespace s02;
    if (switches() & SW_MULTI_ALL_OFF) return;
    uint8_t *e = u3x_level_want_entry(base, SID_MULTI_ALL);
    if (!e) return;
    flags_of(e) &= static_cast<uint8_t>(~KEEP);
    ml_d[2] += static_cast<uint32_t>(u3h_stage_unwant(base, SID_MULTI_ALL));
}

// 4.9: hook kind 43, PS3's co-op Shade animation want (for this match only, no keep bit)
extern "C" U3X_FN void u3x_shade_anim_want(uintptr_t base, void *s) {
    using namespace s02;
    const int32_t type = u3x_netgame_type(base, s);
    sw_word(0x2184)++;                               // sw[21]: kind 43 calls
    sw_word(0x218c) = static_cast<uint32_t>(type);   // sw[23]: last net game type
    if (type == NETGAME_COOP_SHADE && u3x_actor_want(base, s, shade_anim_name) == 2) sw_word(0x2188)++;   // sw[22]
}

// 4.10: hook kind 10, the match setup's emblem setup, run only when its three data modules resolve
extern "C" U3X_FN void u3x_match_emblem_setup(uintptr_t base, void *s) {
    using namespace s02;
    const uintptr_t find = native(s, 9);
    const uintptr_t setup = native(s, 10);
    const uintptr_t fill = native(s, 11);

    if (fill && (rd<uint32_t>(s, 0x3d78) & 0x40000000u) && at<uint32_t>(base + EMBLEM_TABLE_ID0) == 0) {
        at<uint8_t>(base + EMBLEM_STATE) = 1;
        game_call<void>(fill, reinterpret_cast<void *>(base + EMBLEM_STATE));
    }

    bool ok = find && setup && at<uint32_t>(base + EMBLEM_TABLE_ID0) != 0;
    for (uint32_t k = 0; ok && k < 3; k++) {
        const uint32_t id = at<uint32_t>(base + emblem_id_at[k]);
        if (game_call<uintptr_t>(find, id, 1) == base + NO_MODULE) ok = false;
    }
    if (ok) game_call<void>(setup);
    else *reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(s) + 1896) += 1;
}
