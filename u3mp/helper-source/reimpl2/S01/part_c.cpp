// S01 part C: the online matchmaking driver (u3x_onm_tick) and its failure step (u3x_onm_fail).
// Written from spec S01_matchmaking_leftovers.md (4.8 - 4.10) and the facts of G36 3.5 it refers to.

// ---- functions of other groups, of the C part and of the shared tools (spec 2.2)
extern "C" {
int u3h_onb_mode(void);
void u3h_onm_watch(uintptr_t base, void *s);
int u3h_n05_backout(uintptr_t base);
uint32_t *u3h_onm(void);
volatile uint32_t *u3h_onm_diag(void);
uint32_t *u3h_m03(void);
void u3h_onm_state(uintptr_t base, uint32_t state);
uint8_t *u3h_mm_bg(void);
void u3h_mm_log(const char *a, int32_t v, const char *b);
uint32_t *u3h_cg_t0_ptr(void);
int32_t u3h_mq_find(uintptr_t base, uint32_t type, uint32_t *idx, int remove);
int32_t u3h_mq_get_np(uintptr_t base, uint32_t idx, uint8_t *from, void *data, uint32_t *len);
int32_t u3h_mq_send(uintptr_t base, const uint8_t *to, uint32_t type, const void *data, uint32_t len);
uint32_t u3h_members(uint8_t *session, uint8_t **out);
int u3h_fr_strcmp(uintptr_t base, const void *a, const void *b);
int u3h_show(uintptr_t base, void *s, uint32_t page, int silent);
uint32_t u3h_active_page(uintptr_t base);
uint8_t *u3h_active_page_object(uintptr_t base);
int u3x_party_leader(uintptr_t base);
uint8_t *u3x_find_member(uint8_t *session, uint32_t id);
int u3x_online_search(uintptr_t base);
void u3x_mm_ev6(uintptr_t base, uint32_t fn, uint32_t page);
uint8_t *u3x_sym_table(void *s, uint32_t id);
const uint8_t *u3x_pick_playlist(const uint8_t *table, uint32_t picked);
int32_t u3x_net_clock(uintptr_t base);

void u3x_onm_tick(uintptr_t base, void *s);
void u3x_onm_fail(uintptr_t base);
}

namespace s01c {

// image offsets
constexpr uintptr_t CLOCK = 0x17b4ba4;      // game clock, 300 per second
constexpr uintptr_t NS = 0x1b68810;         // net state
constexpr uintptr_t NS_CLOCK = 0x1b68814;   // net state's clock
constexpr uintptr_t NET = 0x17d5c58;        // net state object
constexpr uintptr_t MS = 0x17d6420;         // match session
constexpr uintptr_t PS = 0x17dc298;         // party session
constexpr uintptr_t MGR = 0x17dd768;        // pointer to the session manager
constexpr uintptr_t MENU = 0x17bd080;
constexpr uintptr_t PROFILES = 0x17dd610;
constexpr uintptr_t SEARCH_PHASE = 0x17d79e8;
constexpr uintptr_t START_FLAG_A = 0x16e7171;
constexpr uintptr_t START_FLAG_B = 0x16e7172;
constexpr uintptr_t PARTY_TAG = 0x17d6010;  // 0x14 bytes carried by the 0x113 message
constexpr uintptr_t MENU_BACK = 0x652200;

// ids and pages
constexpr uint32_t PAGE_MM = 0xe35f0f1a;
constexpr uint32_t MSG_ROOM = 0x113;
constexpr uint32_t SYM_PLAYLISTS = 0xc1b0be0d;
constexpr uint32_t RESULT_HANDLER = 0x6a6830;

// manager table entries
constexpr int E_CREATE = 4, E_SEARCH = 5, E_JOIN = 6, E_LEAVE = 7;

// online driver state words (u3h_onm)
enum : int { O_ARMED, O_RUNNING, O_PHASE, O_ARMED_AT, O_PHASE_AT, O_SELECTED, O_MAX, O_ROOMS, O_MIN, O_RESEARCH_AT,
             O_IN_ROOM, O_MERGING, O_WATCHDOG };
// stand-in state words (u3h_m03)
enum : int { M_ARMED, M_RUNNING, M_START_AT, M_PHASE, M_FOUND, M_MAX, M_SELECTED, M_PHASE_AT };

U3X_RO const char t_follow_ok[] = "party leader's match room received (0x113)";
U3X_RO const char t_follow_bad[] = "0x113 received but not from this party's leader: ignored";
U3X_RO const char t_follow_join[] = "following the party leader: joining its match room (304)";
U3X_RO const char t_search_fail[] = "search could not be sent";
U3X_RO const char t_search_sent[] = "search sent (309), playlist minimum ";
U3X_RO const char t_players[] = " players";
U3X_RO const char t_no_result[] = "no search result in 30 s (or the search was replaced), net state ";
U3X_RO const char t_result[] = "search result (310): rooms found ";
U3X_RO const char t_host_fail[] = "hosting failed (303 not sent)";
U3X_RO const char t_hosting[] = "no room found: hosting one (303), up to ";
U3X_RO const char t_no_room[] = "no room joined / created in 30 s (no 305)";
U3X_RO const char t_in_room[] = "in the match room (305), members ";
U3X_RO const char t_count[] = "match room members now ";
U3X_RO const char t_minimum[] = "playlist minimum reached: vote / countdown with ";
U3X_RO const char t_told[] = "told the party (0x113) to follow into the match room, members told: ";

U3X_BSS uint32_t last_count;   // last logged member count; starts 0, never reset

U3X_FN uint32_t clock(uintptr_t base) { return at<uint32_t>(base + CLOCK); }
U3X_FN uint8_t *net(uintptr_t base) { return reinterpret_cast<uint8_t *>(base + NET); }
U3X_FN uint8_t *ms(uintptr_t base) { return reinterpret_cast<uint8_t *>(base + MS); }
U3X_FN uint8_t *ps(uintptr_t base) { return reinterpret_cast<uint8_t *>(base + PS); }

template <typename T> [[gnu::always_inline]] inline void put(uint8_t *p, uintptr_t off, T v) {
    *reinterpret_cast<T *>(p + off) = v;
}

// the session manager when usable (pointer and its table pointer both non-null), else null
U3X_FN uint8_t *manager(uintptr_t base) {
    uint8_t *m = at<uint8_t *>(base + MGR);
    if (!m || !rd<uint64_t>(m, 0)) return nullptr;
    return m;
}
U3X_FN uintptr_t entry(uint8_t *m, int k) { return rd<const uintptr_t *>(m, 0)[k]; }

U3X_FN void leave_join(uintptr_t base, uint8_t *m, uint64_t room) {
    game_call<int64_t>(entry(m, E_LEAVE), m, ms(base));
    game_call<int64_t>(entry(m, E_JOIN), m, ms(base), room);
}

// the picked playlist (G36 3.5): the profile's id is read first, then one symbol lookup
U3X_FN const uint8_t *picked_playlist(uintptr_t base, void *s) {
    const uint8_t *prof = at<const uint8_t *>(base + PROFILES);
    const uint32_t id = prof ? rd<uint32_t>(prof, 0x774) : 0;
    const uint8_t *table = u3x_sym_table(s, SYM_PLAYLISTS);
    return u3x_pick_playlist(table, id);
}
U3X_FN uint32_t clamp_max(const uint8_t *p) {
    uint32_t v = p ? rd<uint32_t>(p, 0x1c) : 10;
    if (v < 2) v = 2;
    if (v > 10) v = 10;
    return v;
}
U3X_FN uint32_t clamp_min(const uint8_t *p, uint32_t max) {
    uint32_t v = p ? rd<uint32_t>(p, 0x18) : 2;
    if (v < 2) v = 2;
    if (v > max) v = max;
    return v;
}

U3X_FN bool backout_page(uint32_t pg) {
    return pg == 0x0b7714b5 || pg == 0x53cbb5f2 || pg == 0x1e66a744 || pg == 0x37656388;
}

U3X_FN void set_status(uintptr_t base, int32_t st) { put<int32_t>(net(base), 0x40, st); }

// pg is the matchmaking page, or (only when it is not) the raw top page is
U3X_FN bool on_mm_page(uintptr_t base, uint32_t pg) {
    if (pg == PAGE_MM) return true;
    const uint8_t *top = u3h_active_page_object(base);
    return (top ? rd<uint32_t>(top, 0x10) : 0) == PAGE_MM;
}

// 4.9 stop(cancel)
U3X_FN void stop(uintptr_t base, bool cancel) {
    uint32_t *o = u3h_onm();
    put<int32_t>(net(base), 0x40, 0);
    put<uint8_t>(net(base), 0x18, 0);
    if (cancel) {
        u3h_onm_state(base, 0);
        at<uint8_t>(base + START_FLAG_A) = 0;
        at<uint8_t>(base + START_FLAG_B) = 0;
    }
    o[O_ARMED] = 0;
    o[O_RUNNING] = 0;
    o[O_PHASE] = 0;
    *u3h_mm_bg() = 0;
    u3h_onm_diag()[6] += 1;
}

// 4.9 host: create a room for O max players; -1 without a usable manager
U3X_FN int32_t host(uintptr_t base) {
    uint8_t *m = manager(base);
    if (!m) return -1;
    const int64_t r = game_call<int64_t>(entry(m, E_CREATE), m, ms(base), u3h_onm()[O_MAX], 0u, 0xffffffffu, 0u);
    return static_cast<int32_t>(static_cast<uint32_t>(r));
}

// 4.9 re-search: a new search while hosting; merging = 1 when it went out
U3X_FN void research(uintptr_t base) {
    uint8_t *m = manager(base);
    if (!m) return;
    const int64_t r = game_call<int64_t>(entry(m, E_SEARCH), m, ms(base), 0u, 0u, -1, 0, 0, 0u);
    if (r >= 0) u3h_onm()[O_MERGING] = 1;
}

// 4.9 members: MS member slots in use
U3X_FN uint32_t members(uintptr_t base) {
    const uint8_t *slot = ms(base) + 0x680;
    uint32_t n = 0;
    for (int i = 0; i < 10; i++, slot += 0x120)
        if (rd<uint8_t>(slot, 0x25)) n++;
    return n;
}

// 4.9 ready(cnt): hand the room over to the stand-in's vote / countdown
U3X_FN void ready(uintptr_t base, void *s, uint32_t cnt) {
    uint32_t *m = u3h_m03();
    uint32_t *o = u3h_onm();
    const uint32_t now = clock(base);
    *reinterpret_cast<const uint8_t **>(reinterpret_cast<uint8_t *>(m) + 0x20) = picked_playlist(base, s);
    m[M_MAX] = o[O_MAX];
    m[M_FOUND] = cnt;
    m[M_SELECTED] = 0;
    m[M_ARMED] = 1;
    m[M_RUNNING] = 1;
    m[M_PHASE] = 3;
    m[M_START_AT] = now;
    m[M_PHASE_AT] = now;
    set_status(base, 2);
    put<int32_t>(net(base), 0x2c, static_cast<int32_t>(static_cast<uint32_t>(u3x_net_clock(base)) + 300u * 15u));
    o[O_ARMED] = 0;
    o[O_RUNNING] = 0;
    o[O_PHASE] = 0;
    u3h_onm_diag()[2] = 9;
}

// 4.9 tell the party(room): the leader sends 0x113 with the room to every other member
U3X_FN void tell_party(uintptr_t base, uint64_t room) {
    uint8_t *p = ps(base);
    if (!rd<uint8_t>(p, 0xc8) || !rd<uint64_t>(p, 0x18) || !u3x_party_leader(base) || !room) return;
    uint8_t *list[10];
    uint32_t n = u3h_members(p, list);
    if (n > 10) n = 10;
    uint8_t d[0x30];
    for (int i = 0; i < 0x30; i++) d[i] = 0;
    for (int i = 0; i < 0x14; i++) d[i] = at<uint8_t>(base + PARTY_TAG + i);
    put<uint64_t>(d, 0x28, room);
    int32_t told = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t own = rd<uint32_t>(p, 0x12e4);  // read anew for each member
        if (rd<uint32_t>(list[i], 0x2c) == own) continue;
        u3h_mq_send(base, list[i], MSG_ROOM, d, 0x30);
        told++;
    }
    u3h_mm_log(t_told, told, nullptr);
}

// 4.8 step 5: follow the party leader into its match room
U3X_FN void follow_leader(uintptr_t base, void *s) {
    uint32_t ix = 0;
    if (u3h_mq_find(base, MSG_ROOM, &ix, 0) != 0) return;
    uint8_t from[0x24];
    uint8_t d[0x30];
    for (int i = 0; i < 0x24; i++) from[i] = 0;
    for (int i = 0; i < 0x30; i++) d[i] = 0;
    uint32_t len = 0x30;
    if (u3h_mq_get_np(base, ix, from, d, &len) != 0) return;

    uint8_t *p = ps(base);
    const uint64_t room = rd<uint64_t>(d, 0x28);
    uint8_t *leader = u3x_find_member(p, rd<uint32_t>(p, 0x12e8));
    const bool ok = room != 0 && rd<uint8_t>(p, 0xc8) != 0 && leader != nullptr && u3x_party_leader(base) == 0 &&
                    u3h_fr_strcmp(base, leader, from) == 0;
    u3h_mm_log(ok ? t_follow_ok : t_follow_bad, -1, nullptr);
    if (!ok || rd<uint64_t>(ms(base), 0x18) == room) return;
    uint8_t *m = manager(base);
    if (!m) return;

    if (u3h_active_page(base) != PAGE_MM) u3h_show(base, s, PAGE_MM, 0);
    uint32_t *o = u3h_onm();
    const uint8_t *pl = picked_playlist(base, s);
    o[O_MAX] = clamp_max(pl);
    o[O_MIN] = clamp_min(pl, o[O_MAX]);
    o[O_ARMED] = 1;
    o[O_RUNNING] = 1;
    o[O_PHASE] = 2;
    o[O_IN_ROOM] = 0;
    o[O_MERGING] = 0;
    o[O_SELECTED] = 0;
    const uint32_t now = clock(base);
    o[O_ARMED_AT] = now;
    o[O_PHASE_AT] = now;
    o[O_RESEARCH_AT] = now;
    at<int32_t>(base + SEARCH_PHASE) = 0;
    put<uint8_t>(net(base), 0x18, 1);
    set_status(base, 1);
    leave_join(base, m, room);
    u3h_mm_log(t_follow_join, -1, nullptr);
    const uint32_t t = clock(base);
    *u3h_cg_t0_ptr() = t ? t : 1;
}

// 4.8 step 7: the driver is armed but not running yet
U3X_FN void start_search(uintptr_t base, void *s, uint32_t pg, uint32_t ns, uint32_t now) {
    uint32_t *o = u3h_onm();
    const bool okran = ns == 20 && static_cast<int32_t>(at<uint32_t>(base + NS_CLOCK) - o[O_ARMED_AT]) >= 0;
    if (okran && on_mm_page(base, pg)) {
        o[O_RUNNING] = 1;
        o[O_PHASE] = 1;
        o[O_PHASE_AT] = now;
        o[O_SELECTED] = 0;
        const uint8_t *pl = picked_playlist(base, s);
        o[O_MAX] = clamp_max(pl);
        o[O_MIN] = clamp_min(pl, o[O_MAX]);
        o[O_IN_ROOM] = 0;
        o[O_MERGING] = 0;
        at<int32_t>(base + SEARCH_PHASE) = 0;
        put<uint8_t>(net(base), 0x18, 0);
        if (u3x_online_search(base) < 0) {
            u3h_mm_log(t_search_fail, -1, nullptr);
            u3x_onm_fail(base);
            return;
        }
        u3h_mm_log(t_search_sent, static_cast<int32_t>(o[O_MIN]), t_players);
        set_status(base, 1);
        u3h_onm_diag()[1] += 1;
    } else if (now - o[O_ARMED_AT] > 4500) {
        o[O_ARMED] = 0;
    }
}

// 4.8 step 10: waiting for the search result
U3X_FN void phase_search(uintptr_t base, uint32_t ns, uint32_t now, uint32_t dt) {
    uint32_t *o = u3h_onm();
    if (ns != 6 || dt >= 9000) {
        u3h_mm_log(t_no_result, static_cast<int32_t>(ns), nullptr);
        u3x_onm_fail(base);
        return;
    }
    at<int32_t>(base + SEARCH_PHASE) = dt < 3000 ? 0 : dt < 6000 ? 2 : 4;
    uint8_t *session = ms(base);
    if (!rd<uint8_t>(session, 0xb0)) return;
    const uint32_t rooms = rd<uint32_t>(session, 0x218);
    o[O_ROOMS] = rooms;
    u3h_onm_diag()[3] = rooms;
    u3h_mm_log(t_result, static_cast<int32_t>(rooms), nullptr);
    if (rd<int32_t>(session, 0xb4) < 0) {
        stop(base, true);
        return;
    }
    o[O_PHASE_AT] = now;
    o[O_RESEARCH_AT] = now;
    if (o[O_ROOMS]) {  // read again: the log call may have changed it
        o[O_PHASE] = 2;
        put<uint8_t>(net(base), 0x18, 1);
        *u3h_mm_bg() = 1;
        u3x_mm_ev6(base, RESULT_HANDLER, 0);
        u3h_onm_state(base, 0xb);
        u3h_onm_diag()[4] += 1;
        return;
    }
    if (host(base) < 0) {
        u3h_mm_log(t_host_fail, -1, nullptr);
        u3x_onm_fail(base);
        return;
    }
    u3h_mm_log(t_hosting, static_cast<int32_t>(o[O_MAX]), t_players);
    o[O_PHASE] = 2;
    put<uint8_t>(net(base), 0x18, 1);
    u3h_onm_diag()[5] += 1;
}

// 4.8 step 11: in (or on the way into) a match room
U3X_FN void phase_room(uintptr_t base, void *s, uint32_t ns, uint32_t now, uint32_t dt) {
    uint32_t *o = u3h_onm();
    uint8_t *session = ms(base);
    if (ns == 0xb || ns == 0xf || ns == 0x28 || ns == 0x13 || ns == 3) *u3h_mm_bg() = 1;
    if (ns == 0x13) u3h_onm_state(base, 0xb);
    if (!rd<uint64_t>(session, 0x18)) {
        if (dt >= 9000) {
            u3h_mm_log(t_no_room, -1, nullptr);
            u3x_onm_fail(base);
        }
        return;
    }
    if (!o[O_IN_ROOM]) {
        o[O_IN_ROOM] = 1;
        o[O_PHASE_AT] = now;
        o[O_RESEARCH_AT] = now;
        *u3h_mm_bg() = 0;
        u3h_onm_state(base, 0x12);
        u3h_mm_log(t_in_room, static_cast<int32_t>(members(base)), nullptr);
        tell_party(base, rd<uint64_t>(session, 0x18));
    }
    const uint32_t cnt = members(base);
    u3h_onm_diag()[3] = cnt;
    if (cnt != last_count) {
        last_count = cnt;
        u3h_mm_log(t_count, static_cast<int32_t>(cnt), nullptr);
    }
    if (cnt >= o[O_MIN]) {
        u3h_mm_log(t_minimum, static_cast<int32_t>(cnt), t_players);
        ready(base, s, cnt);
        return;
    }
    const bool hosting = rd<uint32_t>(session, 0x12e4) == rd<uint32_t>(session, 0x12e8);
    if (o[O_MERGING] && rd<uint8_t>(session, 0xb0)) {
        o[O_MERGING] = 0;
        for (uint32_t i = 0; i < rd<uint32_t>(session, 0x218) && i < 20; i++) {
            const uint64_t r = rd<uint64_t>(session, 0x220 + 56 * i);
            if (!r || r == rd<uint64_t>(session, 0x18)) continue;
            uint8_t *m = manager(base);
            if (!m) break;
            leave_join(base, m, r);
            o[O_IN_ROOM] = 0;
            o[O_PHASE_AT] = now;
            u3h_onm_diag()[4] += 1;
            break;
        }
    } else if (hosting && now - o[O_RESEARCH_AT] >= 9000) {
        o[O_RESEARCH_AT] = now;
        research(base);
    }
}

}  // namespace s01c

// 4.8: the online driver, every frame
extern "C" U3X_FN void u3x_onm_tick(uintptr_t base, void *s) {
    using namespace s01c;
    if (u3h_onb_mode() != 1) return;
    u3h_onm_watch(base, s);

    uint32_t *o = u3h_onm();
    // watchdog on the match start ("Starting Game..." for more than 60 s)
    const uint32_t ns0 = at<uint32_t>(base + NS);
    if (ns0 == 0x2e || ns0 == 0x2f) {
        const uint32_t t = clock(base);
        if (o[O_WATCHDOG] == 0) {
            o[O_WATCHDOG] = t ? t : 1;
        } else if (t - o[O_WATCHDOG] > 18000) {
            o[O_WATCHDOG] = 0;
            u3x_onm_fail(base);
        }
    } else {
        o[O_WATCHDOG] = 0;
    }

    if (!(ext_eb()[3] & 0x20000u) && u3h_n05_backout(base)) return;
    follow_leader(base, s);

    if (!o[O_ARMED]) return;
    const uint32_t pg = u3h_active_page(base);
    const uint32_t ns = at<uint32_t>(base + NS);
    const uint32_t now = clock(base);
    u3h_onm_diag()[7] = ns;

    if (!o[O_RUNNING]) {
        start_search(base, s, pg, ns, now);
        return;
    }
    if (backout_page(pg)) {
        stop(base, true);
        return;
    }
    const uint32_t dt = now - o[O_PHASE_AT];
    u3h_onm_diag()[2] = o[O_PHASE];
    if (o[O_PHASE] == 1)
        phase_search(base, ns, now, dt);
    else if (o[O_PHASE] == 2)
        phase_room(base, s, ns, now, dt);
}

// 4.10: the game's matchmaking failure path
extern "C" U3X_FN void u3x_onm_fail(uintptr_t base) {
    using namespace s01c;
    uint8_t *n = net(base);
    put<uint8_t>(n, 0x28, 1);
    put<int32_t>(n, 0x40, 0);
    put<uint8_t>(n, 0x18, 0);
    game_call<void>(base + MENU_BACK, reinterpret_cast<void *>(base + MENU), 0);
    u3h_onm_state(base, 0);
    at<uint8_t>(base + START_FLAG_A) = 0;
    at<uint8_t>(base + START_FLAG_B) = 0;
    uint32_t *o = u3h_onm();
    o[O_ARMED] = 0;
    o[O_RUNNING] = 0;
    o[O_PHASE] = 0;
    *u3h_mm_bg() = 0;
    u3h_onm_diag()[6] += 1;
}
