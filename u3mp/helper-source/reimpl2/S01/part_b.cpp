// S01 part B: the offline matchmaking stand-in's per-frame tick and its fill sequence (spec S01 4.6, 4.7).
// Written from the behaviour description only.

// ---- other parts, other groups and the C part (prototypes as the spec gives them)
extern "C" uint32_t *u3h_m03(void);
extern "C" volatile uint32_t *u3h_m03_diag(void);
extern "C" void u3h_m03_stop(uintptr_t base, int cancel);
extern "C" int32_t u3h_m03_pick(uintptr_t base, void *s);
extern "C" int32_t u3h_m05_cand(uintptr_t base, void *s, int32_t not_map);
extern "C" uint32_t *u3h_m05_vt(void);
extern "C" int u3h_n05_backout(uintptr_t base);
extern "C" void u3h_n05_note(int k, uint32_t v);
extern "C" uint32_t *u3h_n02_gone(void);
extern "C" uint32_t *u3h_n02_tarm(void);
extern "C" volatile uint32_t *u3h_n02_diag(void);
extern "C" int32_t *u3h_n04_idx(void);
extern "C" volatile uint32_t *u3h_n04_diag(void);
extern "C" int u3x_n04_close(uintptr_t base, void *s);
extern "C" int u3x_n09_open(uintptr_t base, void *s);
extern "C" int u3x_n09_tick(uintptr_t base, uint32_t now);
extern "C" void u3x_start_game_now(uintptr_t base, void *s);
extern "C" uint32_t u3h_active_page(uintptr_t base);
extern "C" uint8_t *u3h_active_page_object(uintptr_t base);
extern "C" uint8_t *u3x_sym_table(void *s, uint32_t id);
extern "C" int32_t u3x_net_clock(uintptr_t base);
extern "C" int u3h_onb_mode(void);
extern "C" int32_t u3x_vote_winner(const uint8_t *vote);   // part A

extern "C" void u3x_m03_tick(uintptr_t base, void *s);
extern "C" void u3x_m05_tick(uintptr_t base, void *s, uint32_t now, uint32_t dt);

namespace s01b {

// image offsets
constexpr uintptr_t CLOCK = 0x17b4ba4;      // game clock (u32)
constexpr uintptr_t NET = 0x17d5c58;        // net state object
constexpr uintptr_t NS = 0x1b68810;         // net state (u32)
constexpr uintptr_t NS_CLOCK = 0x1b68814;   // net state clock (u32)
constexpr uintptr_t MENU = 0x17bd080;       // menu object
constexpr uintptr_t SEARCH_PHASE = 0x17d79e8;   // status line search phase (s32)
constexpr uintptr_t NGM = 0x17cb060;        // net game manager
constexpr uintptr_t NET_MENU_MODE = 0x17d603c;
constexpr uintptr_t VOTE_MAP = 0x17d7760;   // V
constexpr uintptr_t VOTE_DIFF = 0x17d7784;  // W

constexpr uint32_t PAGE_MM = 0xe35f0f1a;
constexpr uint32_t POPUP_STARTING = 0x453470dd;
constexpr uint32_t NETGAME_TABLE = 0x3b9a067d;
constexpr uint32_t KNUTH_HASH = 2654435761u;   // 2^32 / golden ratio (TAOCP vol. 3, 6.4)

// stand-in state words
enum { M_ARMED = 0, M_RUNNING = 1, M_START = 2, M_PHASE = 3, M_FOUND = 4, M_MAX = 5, M_SEL = 6, M_PCLOCK = 7 };

U3X_FN inline uint32_t *ec() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0x4450); }
U3X_FN inline bool standin_on() { return (ext_eb()[2] & 0x2000u) == 0; }
U3X_FN inline bool no_start() { return (ext_eb()[2] & 0x4000u) != 0; }
U3X_FN inline bool fill_mode() { return standin_on() && (ext_eb()[3] & 0x10000u) == 0; }
U3X_FN inline bool n002_on() { return (ext_eb()[3] & 0x20000u) == 0; }
U3X_FN inline bool n004_on() { return (ec()[2] & 0x4u) == 0; }

U3X_FN inline void set_status(uintptr_t base, int32_t st) { at<int32_t>(base + NET + 0x40) = st; }
U3X_FN inline void set_status_for(uintptr_t base, int32_t st, uint32_t seconds) {
    at<int32_t>(base + NET + 0x40) = st;
    at<uint32_t>(base + NET + 0x2c) = static_cast<uint32_t>(u3x_net_clock(base)) + 300u * seconds;
}

U3X_FN inline bool backout_page(uint32_t pg) {
    return pg == 0x0b7714b5u || pg == 0x53cbb5f2u || pg == 0x1e66a744u || pg == 0x37656388u;
}

// player count of a playlist entry, 2 .. 10 (10 without entry)
U3X_FN inline uint32_t clamp_max(const uint8_t *p) {
    uint32_t v = p ? rd<uint32_t>(p, 0x1c) : 10u;
    if (v < 2) v = 2;
    if (v > 10) v = 10;
    return v;
}

using Native2 = void (*)(void *ngm, int32_t index, void *zero, uint32_t mode);
U3X_FN inline Native2 native2(void *s) { return rd<Native2>(s, 768 + 8 * 2); }

// the match start shared by both sequences; the net game selection is skipped when select is false
U3X_FN inline void start(uintptr_t base, void *s, Native2 n2, int32_t idx, bool select) {
    if (select) n2(reinterpret_cast<void *>(base + NGM), idx, nullptr, 0xcu);
    at<uint32_t>(base + NET_MENU_MODE) = 2;
    u3h_m03_stop(base, 0);
    u3h_m03_diag()[5] += 1;
    ext_ea()[94] = 1;
    ext_ea()[90] += 1;
    u3x_start_game_now(base, s);
}

// step 4 of 4.6: the stand-in is armed but not running yet
U3X_FN void not_running(uintptr_t base, uint32_t *m, uint32_t pg, uint32_t ns, uint32_t now) {
    const uint8_t *top = u3h_active_page_object(base);
    const uint32_t tr = top ? rd<uint32_t>(top, 0x10) : 0;
    const bool okran = ns == 20 &&
        static_cast<int32_t>(at<uint32_t>(base + NS_CLOCK) - *u3h_n02_tarm()) >= 0;

    if (okran && (pg == PAGE_MM || tr == PAGE_MM)) {
        m[M_RUNNING] = 1;
        m[M_START] = now;
        m[M_PCLOCK] = now;
        m[M_PHASE] = 1;
        m[M_FOUND] = 1;
        m[M_SEL] = 0;
        m[M_MAX] = clamp_max(rd<const uint8_t *>(m, 0x20));
        at<int32_t>(base + SEARCH_PHASE) = 0;
        at<uint8_t>(base + NET + 0x18) = 0;
        set_status(base, 1);
        u3h_m03_diag()[1] += 1;
        u3h_n05_note(7, now - *u3h_n02_tarm());
        if (pg != PAGE_MM) u3h_n05_note(0x100, 1);
    } else {
        const uint8_t *popup = at<const uint8_t *>(base + MENU + 0x898);
        volatile uint32_t *d = u3h_n02_diag();
        if (popup && rd<uint32_t>(popup, 0x10) == POPUP_STARTING) {
            *u3h_n02_gone() = 0;
            d[0] += 1;
        } else {
            uint32_t *gone = u3h_n02_gone();
            if (*gone == 0) {
                u3h_n05_note(5, ns | (static_cast<uint32_t>(okran) << 16));
                u3h_n05_note(6, tr);
                const uint32_t ui = at<uint32_t>(base + MENU + 8) & 0xff;
                const uint32_t topi = static_cast<uint32_t>(at<int32_t>(base + MENU + 0xdb0)) & 0xff;
                u3h_n05_note(4, ui | (topi << 8));
            }
            if (pg == 0) u3h_n05_note(0x101, 1);
            if (!okran) {
                *gone += 1;
                if (*gone >= 3) {
                    m[M_ARMED] = 0;
                    d[1] += 1;
                }
            } else if (*gone < 3) {
                *gone += 1;
            }
            if (m[M_ARMED] != 0 && now - *u3h_n02_tarm() > 4500u) {
                m[M_ARMED] = 0;
                d[1] += 0x10000;
            }
        }
    }
    volatile uint32_t *d = u3h_n02_diag();
    d[2] = ns;
    d[3] = pg ? pg : tr;
}

}  // namespace s01b

extern "C" U3X_FN void u3x_m03_tick(uintptr_t base, void *s) {
    using namespace s01b;
    // 1. a "leave Find Game" back-out under way
    if (n002_on() && standin_on() && u3h_onb_mode() == 2 && u3h_n05_backout(base)) return;
    uint32_t *m = u3h_m03();
    // 2. not armed
    if (m[M_ARMED] == 0) return;
    // 3.
    const uint32_t pg = u3h_active_page(base);
    const uint32_t ns = at<uint32_t>(base + NS);
    const uint32_t now = at<uint32_t>(base + CLOCK);
    // 4.
    if (m[M_RUNNING] == 0) {
        not_running(base, m, pg, ns, now);
        return;
    }
    // 5. running: the player backed out
    if (backout_page(pg)) {
        u3h_m03_stop(base, 1);
        return;
    }
    // 6.
    volatile uint32_t *nd = u3h_n02_diag();
    nd[2] = ns;
    nd[3] = pg;
    nd[7] = static_cast<uint32_t>(at<int32_t>(base + NET + 0x40));
    // 7. left the matchmaking state
    if (ns != 20) {
        nd[5] = ((nd[5] & 0xffff0000u) + 0x10000u) | (ns & 0xffffu);
        u3h_m03_stop(base, 0);
        return;
    }
    // 8.
    const uint32_t dt = now - m[M_PCLOCK];
    if (fill_mode()) {
        u3x_m05_tick(base, s, now, dt);
        return;
    }
    // 9. the short sequence
    switch (m[M_PHASE]) {
    case 1:
        at<int32_t>(base + SEARCH_PHASE) = dt < 600 ? 0 : dt < 1200 ? 2 : 4;
        if (dt >= 1800) {
            at<uint8_t>(base + NET + 0x18) = 1;
            m[M_PHASE] = 2;
            m[M_PCLOCK] = now;
        }
        break;
    case 2:
        if (dt >= 360 && m[M_FOUND] < m[M_MAX]) {
            m[M_FOUND] += 1;
            m[M_PCLOCK] = now;
        }
        if (m[M_FOUND] >= m[M_MAX] && dt >= 360) {
            set_status_for(base, 2, 5);
            m[M_PHASE] = 3;
            m[M_PCLOCK] = now;
        }
        break;
    case 3:
        if (dt >= 1500) {
            set_status_for(base, 6, 10);
            m[M_PHASE] = 4;
            m[M_PCLOCK] = now;
        }
        break;
    case 4:
        if (dt >= 3000) {
            if (no_start()) {
                set_status(base, 5);
                m[M_PHASE] = 5;
            } else {
                const int32_t idx = u3h_m03_pick(base, s);
                u3h_m03_diag()[6] = static_cast<uint32_t>(idx);
                const Native2 n2 = native2(s);
                if (!n2 || idx < 0) {
                    set_status(base, 5);
                    m[M_PHASE] = 5;
                } else {
                    start(base, s, n2, idx, true);
                    return;
                }
            }
        }
        break;
    default:
        break;
    }
    // 10.
    volatile uint32_t *md = u3h_m03_diag();
    md[2] = m[M_PHASE];
    md[3] = m[M_FOUND];
}

extern "C" U3X_FN void u3x_m05_tick(uintptr_t base, void *s, uint32_t now, uint32_t dt) {
    using namespace s01b;
    uint32_t *m = u3h_m03();
    uint8_t *v = reinterpret_cast<uint8_t *>(base + VOTE_MAP);
    uint8_t *w = reinterpret_cast<uint8_t *>(base + VOTE_DIFF);
    switch (m[M_PHASE]) {
    case 1:   // searching
        at<int32_t>(base + SEARCH_PHASE) = dt < 600 ? 0 : 2;
        if (dt >= 600) {
            m[M_PHASE] = 2;
            m[M_PCLOCK] = now;
        }
        break;
    case 2:   // slots filling
        if (dt >= 360 && m[M_FOUND] < m[M_MAX]) {
            m[M_FOUND] += 1;
            m[M_PCLOCK] = now;
        }
        if (m[M_FOUND] >= m[M_MAX] && dt >= 600) {
            set_status_for(base, 2, 15);
            m[M_PHASE] = 3;
            m[M_PCLOCK] = now;
        }
        break;
    case 3:   // "Voting starts in": open the map vote with two candidates
        if (dt >= 4500) {
            const uint8_t *t = u3x_sym_table(s, NETGAME_TABLE);
            const int32_t c0 = u3h_m05_cand(base, s, -1);
            int32_t c1 = -1;
            if (c0 >= 0 && t) {
                const uint8_t *rec = rd<const uint8_t *>(t, 8) + 0xd0 * static_cast<int64_t>(c0);
                c1 = u3h_m05_cand(base, s, rd<int32_t>(rec, 4));
            }
            if (c1 < 0) c1 = c0;
            if (c0 < 0) {
                set_status(base, 5);
                m[M_PHASE] = 6;
                break;
            }
            *reinterpret_cast<int32_t *>(v + 8) = c0;
            *reinterpret_cast<int32_t *>(v + 0xc) = c1;
            *reinterpret_cast<int32_t *>(v + 0x10) = 0;
            *reinterpret_cast<int32_t *>(v + 0x14) = 0;
            *reinterpret_cast<int32_t *>(v + 4) = 2;
            v[1] = 0;
            v[0] = 1;
            if (ec()[2] & 0x10u) w[0] = 1;
            set_status_for(base, 3, 10);
            m[M_PHASE] = 4;
            m[M_PCLOCK] = now;
            *u3h_m05_vt() = now;
        }
        break;
    case 4: {   // map vote: stand-in votes, then its end
        uint32_t *vt = u3h_m05_vt();
        uint32_t *n_first = reinterpret_cast<uint32_t *>(v + 0x10);   // counts as s32, added with wrapping
        uint32_t *n_second = reinterpret_cast<uint32_t *>(v + 0x14);
        const uint32_t cast = *n_first + *n_second;
        if (now - *vt >= 300u && cast < m[M_MAX]) {
            *vt = now;
            if (((now * KNUTH_HASH) >> 9) % 3 == 0)
                *n_second += 1;
            else
                *n_first += 1;
        }
        if (dt >= 3000) {
            if (n004_on()) {
                if (u3x_n04_close(base, s) == 0) {
                    set_status(base, 5);
                    m[M_PHASE] = 6;
                } else if (u3x_n09_open(base, s) != 0) {
                    m[M_PHASE] = 7;
                    m[M_PCLOCK] = now;
                    *u3h_m05_vt() = now;
                } else {
                    m[M_PHASE] = 5;
                    m[M_PCLOCK] = now;
                }
            } else {
                v[1] = 1;
                set_status_for(base, 6, 5);
                m[M_PHASE] = 5;
                m[M_PCLOCK] = now;
            }
        }
        break;
    }
    case 7:   // difficulty vote
        if (u3x_n09_tick(base, now) != 0) {
            m[M_PHASE] = 5;
            m[M_PCLOCK] = now;
        }
        break;
    case 5: {   // "Game starting in"
        const bool on = n004_on();
        const bool due = on ? u3x_net_clock(base) >= at<int32_t>(base + NET + 0x2c) : dt >= 1500;
        if (!due) break;
        if (no_start()) {
            set_status(base, 5);
            m[M_PHASE] = 6;
            break;
        }
        const int32_t idx = u3x_vote_winner(v);
        u3h_m03_diag()[6] = static_cast<uint32_t>(idx);
        const Native2 n2 = native2(s);
        if (!n2 || idx < 0) {
            set_status(base, 5);
            m[M_PHASE] = 6;
            break;
        }
        v[0] = 0;
        v[1] = 0;
        w[0] = 0;
        const bool already = on && *u3h_n04_idx() == idx;
        if (already) u3h_n04_diag()[3] += 1;
        start(base, s, n2, idx, !already);
        return;
    }
    default:   // 0, 6 and anything else
        break;
    }
    volatile uint32_t *md = u3h_m03_diag();
    md[2] = m[M_PHASE] | 0x100u;
    md[3] = m[M_FOUND];
}
