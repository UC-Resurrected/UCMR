// G45 part B: medal conditions over the kill history (players and teams).
// Written from spec/G45_medal_conditions.md, sections 3 and 4.7 to 4.14.

extern "C" {
void u3h_kr_stat(uintptr_t base, void *s, uint32_t id, uint32_t amount, uint8_t *np);
uint32_t u3x_net_player_id(const uint8_t *np);
int u3x_net_player_in_game(const uint8_t *np);

void u3x_kr_379ed8(uintptr_t base, void *s, uint8_t *S);
void u3x_kr_379ff0(uintptr_t base, void *s, uint8_t *S, uint8_t *knp);
void u3x_kr_379430(uintptr_t base, void *s, uint8_t *S, uint8_t *knp);
void u3x_kr_3791ec(uintptr_t base, void *s, uint8_t *S, uint8_t *knp, uint8_t *vnp, uint8_t *info);
void u3x_kr_37a914(uintptr_t base, void *s, uint8_t *S, uint8_t *knp);
void u3x_kr_37a16c(uintptr_t base, void *s, uint8_t *S, uint8_t *knp);
void u3x_kr_37a32c(uintptr_t base, void *s, uint8_t *S, uint8_t *knp, uint8_t *info);
int u3x_kr_3590b0(uintptr_t base, uint8_t *np);
}

namespace g45b {

// stat event ids, one per condition (4.7 reports under the record's objective value)
constexpr uint32_t EV_LEAST_KILLED = 0x610ae399u;       // 4.8
constexpr uint32_t EV_FLAGGED_KILLS = 0x8d70ef29u;      // 4.9
constexpr uint32_t EV_TRADE_KILL = 0x9d715fdau;         // 4.10
constexpr uint32_t EV_ALL_OPPONENTS_DOWN = 0x4a9da8a5u; // 4.11
constexpr uint32_t EV_STREAK_ENDED = 0xf69e35a3u;       // 4.12
constexpr uint32_t EV_REVENGE = 0x6623a3f4u;            // 4.13

// script symbols
constexpr uint32_t SYM_MIN_OPPONENTS = 0xa84641ccu;
constexpr uint32_t SYM_MIN_STREAK = 0xdd9356ecu;

constexpr uint32_t EFFECT_UP = 0xc054e78bu;
constexpr int32_t MODE_FREE_FOR_ALL = 0xe;
constexpr uint32_t SLOTS = 10;

// image offsets
constexpr uintptr_t FN_MODE_ENTRY = 0x721050;
constexpr uintptr_t FN_SYMBOL = 0x11f2bf0;
constexpr uintptr_t FN_HAS_EFFECT = 0x793280;
constexpr uintptr_t NET_GAME_MANAGER = 0x17cb060;
constexpr uintptr_t SLOTS_FLAG = 0x17d5c61;

// ---- kill history
U3X_FN static uint32_t record_count(const uint8_t *S) { return rd<uint32_t>(S, 0xe0); }
U3X_FN static const uint8_t *record(const uint8_t *S, uint32_t i) { return S + 0xe4 + 0x28 * static_cast<uintptr_t>(i); }
U3X_FN static uint32_t killer_of(const uint8_t *r) { return rd<uint16_t>(r, 2); }
U3X_FN static uint32_t victim_of(const uint8_t *r) { return rd<uint16_t>(r, 4); }
U3X_FN static int32_t killer_team(const uint8_t *r) { return rd<int8_t>(r, 6); }
U3X_FN static int32_t victim_team(const uint8_t *r) { return rd<int8_t>(r, 7); }
U3X_FN static uint32_t team_pair(const uint8_t *r) { return rd<uint16_t>(r, 6); }
U3X_FN static bool enemy_kill(const uint8_t *r) { return killer_team(r) != victim_team(r); }
U3X_FN static bool uncounted(const uint8_t *r) { return rd<uint8_t>(r, 0x25) != 0; }
U3X_FN static int32_t time_of(const uint8_t *r) { return rd<int32_t>(r, 8); }
U3X_FN static bool flag_a(const uint8_t *r) { return rd<uint8_t>(r, 0x21) != 0; }
U3X_FN static bool flag_b(const uint8_t *r) { return rd<uint8_t>(r, 0x22) != 0; }

// ---- net players
U3X_FN static uint8_t *slot_address(uintptr_t base, uint32_t i) {
    return reinterpret_cast<uint8_t *>(base + NET_GAME_MANAGER + 0x390 + 0x9c0 * static_cast<uintptr_t>(i));
}
// the player of slot i, or null while the game has no slots
U3X_FN static uint8_t *slot(uintptr_t base, uint32_t i) {
    if (!at<uint8_t>(base + SLOTS_FLAG) || i >= SLOTS) return nullptr;
    return slot_address(base, i);
}
U3X_FN static uint32_t in_game_count(uintptr_t base) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < SLOTS; ++i)
        if (u3x_net_player_in_game(slot_address(base, i))) ++count;
    return count;
}
// the lowest slot whose player has the id, or -1
U3X_FN static int slot_of_id(uintptr_t base, uint32_t id) {
    if (!id) return -1;
    for (uint32_t i = 0; i < SLOTS; ++i)
        if (u3x_net_player_id(slot_address(base, i)) == id) return static_cast<int>(i);
    return -1;
}
U3X_FN static int32_t team_of(const uint8_t *np) { return rd<int32_t>(np, 0x21c); }
U3X_FN static int32_t death_time(const uint8_t *np) { return rd<int32_t>(np, 0x39c); }
U3X_FN static int32_t spawn_time(const uint8_t *np) { return rd<int32_t>(np, 0x3a0); }

// ---- game lookups (one game call each)
U3X_FN static int32_t mode_type(uintptr_t base) {
    const uint8_t *entry = game_call<const uint8_t *, uint8_t *>(base + FN_MODE_ENTRY,
                                                                 reinterpret_cast<uint8_t *>(base + NET_GAME_MANAGER));
    return entry ? rd<int32_t>(entry, 0xc) : -1;
}
U3X_FN static bool free_for_all(uintptr_t base) { return mode_type(base) == MODE_FREE_FOR_ALL; }
// the data of a script symbol, or null
U3X_FN static const uint8_t *symbol_data(uintptr_t base, uint32_t id) {
    const uint8_t *const *held = game_call<const uint8_t *const *, uint32_t>(base + FN_SYMBOL, id);
    return held ? *held : nullptr;
}
// the same player (no game call), or team mates in a mode with teams
U3X_FN static bool friendly(uintptr_t base, const uint8_t *a, const uint8_t *b) {
    if (a == b) return true;
    if (free_for_all(base)) return false;
    return a && b && team_of(a) == team_of(b);
}

// the character handle of the player points to a live object
U3X_FN static bool character_valid(const uint8_t *np) {
    const uint8_t *const *holder = rd<const uint8_t *const *>(np, 0x10);
    const uint32_t id = rd<uint32_t>(np, 0x18);
    if (!holder || !id) return false;
    const uint8_t *object = *holder;
    return object && rd<uint32_t>(object, 0x7c) == id;
}

}  // namespace g45b

// 4.7 objective chain
extern "C" U3X_FN void u3x_kr_379ed8(uintptr_t base, void *s, uint8_t *S) {
    using namespace g45b;
    const uint8_t *r0 = record(S, 0);
    const int32_t objective = rd<int32_t>(r0, 0x1c);
    if (!objective) return;
    uint32_t count = 1;
    for (uint32_t i = 1; i < record_count(S); ++i) {
        const uint8_t *r = record(S, i);
        if (team_pair(r) != team_pair(r0)) continue;
        if (rd<int32_t>(r, 0x1c) != objective) break;
        ++count;
    }
    // to the whole team of the killer: the first m slots, m = number of in-game slots;
    // the event id and the team are record 0's values at the moment of each report
    const uint32_t m = in_game_count(base);
    for (uint32_t i = 0; i < m; ++i) {
        uint8_t *p = slot(base, i);
        if (p && team_of(p) == killer_team(r0)) u3h_kr_stat(base, s, rd<uint32_t>(r0, 0x1c), count, p);
    }
}

// 4.8 least-killed enemy
extern "C" U3X_FN void u3x_kr_379ff0(uintptr_t base, void *s, uint8_t *S, uint8_t *knp) {
    using namespace g45b;
    uint8_t kills[SLOTS];   // the kills of this player on each slot, 8 bits wide
    for (uint32_t i = 0; i < SLOTS; ++i) *static_cast<volatile uint8_t *>(&kills[i]) = 0;   // (no memset call)
    const uint32_t me = u3x_net_player_id(knp) & 0xffffu;
    for (uint32_t i = 0; i < record_count(S); ++i) {
        const uint8_t *r = record(S, i);
        if (killer_of(r) != me) continue;
        const int j = slot_of_id(base, victim_of(r));
        if (j >= 0) ++kills[j];
    }
    uint32_t least = 0x10;
    uint32_t m = in_game_count(base);
    if (m > SLOTS) m = SLOTS;
    for (uint32_t i = 0; i < m; ++i) {
        const uint8_t *p = slot(base, i);
        if (!friendly(base, knp, p) && kills[i] <= least) least = kills[i];
    }
    u3h_kr_stat(base, s, EV_LEAST_KILLED, least, knp);
}

// 4.9 flagged kills
extern "C" U3X_FN void u3x_kr_379430(uintptr_t base, void *s, uint8_t *S, uint8_t *knp) {
    using namespace g45b;
    const uint8_t *r0 = record(S, 0);
    const uint32_t killer = killer_of(r0);
    if (killer != (u3x_net_player_id(knp) & 0xffffu) || victim_of(r0) == killer || !flag_a(r0)) return;
    uint32_t count = 1;
    for (uint32_t i = 1; i < record_count(S); ++i) {
        const uint8_t *r = record(S, i);
        if (killer_of(r) == killer && victim_of(r) != killer && flag_a(r)) ++count;
    }
    u3h_kr_stat(base, s, EV_FLAGGED_KILLS, count, knp);
}

// 4.10 trade kills
extern "C" U3X_FN void u3x_kr_3791ec(uintptr_t base, void *s, uint8_t *S, uint8_t *knp, uint8_t *vnp, uint8_t *info) {
    using namespace g45b;
    const int32_t now = rd<int32_t>(info, 0);
    const uint32_t killer = rd<uint32_t>(info, 0x30) & 0xffffu;
    const bool explosive = (rd<uint32_t>(info, 0x60) & 0x20u) != 0;

    // the killer died after its last spawn and before this explosive kill
    if (enemy_kill(record(S, 0))) {
        int32_t died = 0;
        for (uint32_t i = 0; i < record_count(S); ++i) {
            const uint8_t *r = record(S, i);
            if (!uncounted(r) && victim_of(r) == killer) { died = time_of(r); break; }
        }
        if (spawn_time(knp) < died && died < now && explosive) u3h_kr_stat(base, s, EV_TRADE_KILL, 1, knp);
    }
    // an explosive of the victim killed an enemy at the same moment
    // (the victim id is taken from the message here, after the report above)
    const uint32_t victim = rd<uint32_t>(info, 0x34) & 0xffffu;
    for (uint32_t i = 0; i < record_count(S); ++i) {
        const uint8_t *r = record(S, i);
        if (time_of(r) < now) break;
        if (killer_of(r) == victim && enemy_kill(r) && rd<uint8_t>(r, 0) == 6) {
            u3h_kr_stat(base, s, EV_TRADE_KILL, 1, vnp);
            return;
        }
    }
}

// 4.11 all opponents down
extern "C" U3X_FN void u3x_kr_37a914(uintptr_t base, void *s, uint8_t *S, uint8_t *knp) {
    using namespace g45b;
    const uint8_t *least = symbol_data(base, SYM_MIN_OPPONENTS);
    if (!least) return;
    bool all = true;
    int32_t count = 0;
    const uint32_t me = u3x_net_player_id(knp) & 0xffffu;
    for (uint32_t i = 0; i < SLOTS; ++i) {
        uint8_t *p = slot(base, i);
        if (!p || !u3x_net_player_in_game(p) || friendly(base, knp, p)) continue;
        if (u3x_kr_3590b0(base, p)) return;
        const uint32_t victim = u3x_net_player_id(p) & 0xffffu;
        // the newest death of the opponent decides
        for (uint32_t j = 0; j < record_count(S); ++j) {
            const uint8_t *r = record(S, j);
            if (victim_of(r) != victim) continue;
            if (killer_of(r) == me) ++count;
            else all = false;
            break;
        }
    }
    if (all && rd<int32_t>(least, 0) <= count) u3h_kr_stat(base, s, EV_ALL_OPPONENTS_DOWN, 1, knp);
}

// 4.12 streak ended
extern "C" U3X_FN void u3x_kr_37a16c(uintptr_t base, void *s, uint8_t *S, uint8_t *knp) {
    using namespace g45b;
    const uint8_t *least = symbol_data(base, SYM_MIN_STREAK);
    if (!least) return;
    const uint8_t *r0 = record(S, 0);
    const uint32_t victim = victim_of(r0);
    const bool ffa = free_for_all(base);
    if (killer_of(r0) == victim) return;
    const bool marked = flag_a(r0) || flag_b(r0);
    if (!enemy_kill(r0) && !ffa && !marked) return;
    uint32_t count = 0;
    for (uint32_t i = 1; i < record_count(S); ++i) {
        const uint8_t *r = record(S, i);
        if (uncounted(r)) continue;
        if (victim_of(r) == victim) break;
        if (killer_of(r) != victim) continue;
        if (victim_team(r) == victim_team(r0) && !ffa && !marked) break;   // a team kill ends the run
        ++count;
    }
    if (rd<uint32_t>(least, 0) <= count) u3h_kr_stat(base, s, EV_STREAK_ENDED, 1, knp);
}

// 4.13 revenge
extern "C" U3X_FN void u3x_kr_37a32c(uintptr_t base, void *s, uint8_t *S, uint8_t *knp, uint8_t *info) {
    using namespace g45b;
    const uint32_t killer = rd<uint32_t>(info, 0x30) & 0xffffu;
    const uint32_t victim = rd<uint32_t>(info, 0x34) & 0xffffu;
    const bool ffa = free_for_all(base);
    const uint8_t *r0 = record(S, 0);
    if (!enemy_kill(r0) && !ffa) return;
    if (record_count(S) < 2) return;
    if (spawn_time(knp) < death_time(knp)) return;   // the killer is down
    for (uint32_t i = 1; i < record_count(S); ++i) {
        const uint8_t *r = record(S, i);
        if (uncounted(r)) continue;
        if (victim_of(r) == victim && killer_of(r) == killer) return;   // already killed this victim since
        if (victim_of(r) == killer && (killer_team(r) == victim_team(r0) || ffa)) {
            if (killer_of(r) == victim) u3h_kr_stat(base, s, EV_REVENGE, 1, knp);
            return;
        }
    }
}

// 4.14 player is up
extern "C" U3X_FN int u3x_kr_3590b0(uintptr_t base, uint8_t *np) {
    using namespace g45b;
    if (game_call<uint64_t, uint8_t *, uint32_t, uint32_t>(base + FN_HAS_EFFECT, np, EFFECT_UP, 0u) & 0xffu) return 1;
    if (character_valid(np) && death_time(np) <= spawn_time(np)) return 1;
    return rd<uint8_t>(np, 0x770);
}
