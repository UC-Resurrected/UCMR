// G46 part A: the kill record (frame of one kill, history record, victim entity, betrayal credit, kill points,
// enemy kill, player kill, assists). Written from spec G46_kill_record.md, sections 4.1 to 4.9.

// The kill context shared with part B (spec 2.2).
struct U3xKill {
    uintptr_t base;
    uint8_t *info;
    uint8_t *s;
    uint8_t *S;
    uint8_t *knp;
    uint8_t *vnp;
    uint8_t *victim_entity;
    uint8_t *krec;
    uint32_t flags;
    uint32_t killer_id, victim_id, weapon;
    uint32_t n;
    uint8_t counts, medals, coop, self_kill, on;
};

extern "C" {
// this part
void u3x_kr_port(uintptr_t base, uint8_t *info, void *s);
// part B
void u3x_kr_counted(U3xKill *k);
void u3x_kr_aftermath(U3xKill *k);
// C part, G44 and shared functions (spec 2.3)
uint8_t *u3h_kr_np_by_id(uintptr_t base, uint32_t id);
uint8_t *u3h_kr_prec(uint8_t *S, uint8_t *np);
void u3h_kr_stat(uintptr_t base, void *s, uint32_t id, uint32_t amount, uint8_t *np);
int u3h_kr_30feec(uintptr_t base);
int32_t u3h_kr_mode_type(uintptr_t base);
int u3x_kr_friendly(uintptr_t base, const uint8_t *a, const uint8_t *b);
void u3x_kill_popup(uintptr_t base, void *s, uint8_t *np, uint8_t *other, const uint8_t *info, int32_t pts, uint32_t type, uint32_t dbl);
void u3x_add_score(uintptr_t base, void *s, uint8_t *S, uint8_t *np, int32_t pts);
void u3x_team_score(uintptr_t base, void *s, uint8_t *S, uint32_t team, int32_t pts);
void u3x_award(uintptr_t base, void *s, uint8_t *S, uint8_t *np, int32_t pts, uint32_t type, uint32_t flag);
int u3x_kr_34fc04(uintptr_t base);
int32_t u3x_kr_37c984(uintptr_t base);
int32_t u3x_kr_37c914(uintptr_t base);
int32_t u3x_kr_37c9f4(uintptr_t base);
int32_t u3x_kill_points(uintptr_t base, uint32_t fn, uint8_t *np);
int u3x_double_points(uintptr_t base, const uint8_t *np);
int u3x_kr_30ffa8(uintptr_t base, const uint8_t *np, int b);
uint32_t u3x_weapon_stat_number(const uint32_t *fixed, const uint32_t *extra, uint32_t weapon);
uint8_t *u3x_handle_obj(uint8_t *h);
}

namespace g46a {

// image offsets: game data
constexpr uintptr_t IMG_NGM = 0x17cb060;          // net game manager
constexpr uintptr_t IMG_SCORE = 0x17dd780;        // score keeper
constexpr uintptr_t IMG_CLOCK = 0x17c3580;        // pointer to the game clock object
constexpr uintptr_t IMG_PAIR_RULE = 0x17dc230;    // first argument of 0x77c880
constexpr uintptr_t IMG_STAT_WEAPONS = 0x13a6170; // the 19 fixed statistics weapons
// image offsets: game functions
constexpr uintptr_t FN_MODE_ENTRY = 0x721050;
constexpr uintptr_t FN_COOP_FLAG = 0x722670;
constexpr uintptr_t FN_ONLINE = 0x7270f0;
constexpr uintptr_t FN_KILL_SCORES = 0x729b10;
constexpr uintptr_t FN_PAIR_RULE = 0x77c880;
constexpr uintptr_t FN_SYMBOL = 0x11f2bf0;
constexpr uintptr_t FN_PLACES = 0x7aa560;
// the game's kill point getters
constexpr uint32_t KP_KILL = 0x6621a0;
constexpr uint32_t KP_KILL_NO_SCORE = 0x662220;
constexpr uint32_t KP_KILL_F_YES = 0x6622a0;
constexpr uint32_t KP_KILL_F_NO = 0x662320;
constexpr uint32_t KP_ASSIST = 0x6623a0;
constexpr uint32_t KP_ASSIST_F_YES = 0x6624a0;
constexpr uint32_t KP_ASSIST_F_NO = 0x662520;
// ids
constexpr uint32_t SYM_EXTRA_STAT_WEAPONS = 0x9e354807;
constexpr uint32_t STAT_BONUS_KILLS = 0xb477093d;
constexpr uint32_t STAT_AVENGED = 0x5258a989;
constexpr uint32_t STAT_ASSISTS = 0x83daf02c;
// mode types
constexpr int32_t MODE_FFA = 0xe;
constexpr int32_t MODE_F = 0xf;
constexpr int32_t MODE_10 = 10;
constexpr int32_t MODE_12 = 0x12;
// message flags
constexpr uint32_t FL_KILLER_PLAYER = 1;
constexpr uint32_t FL_VICTIM_PLAYER = 2;
constexpr uint32_t FL_KIND1 = 4;
constexpr uint32_t FL_EXPLOSIVE = 0x20;
constexpr uint32_t FL_KIND2 = 0x40;
constexpr uint32_t FL_COOP_COUNTS = 0x200;
constexpr uint32_t FL_BONUS = 0x4000;
constexpr uint32_t FL_EXTRA_SCORE = 0x40000;
constexpr uint32_t FL_FIXED_SCORE = 0x200000;
// switches of the state object
constexpr uintptr_t S_SWITCHES = 0x3d78;
constexpr uint32_t SW_OFF = 0x200;
constexpr uint32_t SW_NO_HISTORY = 0x400;
constexpr uint32_t SW_NO_COUNTS = 0x800;
constexpr uint32_t SW_NO_MEDALS = 0x2000;
constexpr uint32_t SW_NO_UNCOUNTED_MARK = 0x10000000;
// kill history
constexpr uintptr_t S_COUNT = 0xe0;
constexpr uintptr_t S_RECORDS = 0xe4;
constexpr uintptr_t REC_SIZE = 0x28;
constexpr uint32_t F_SIXTEEN = 0x41800000;
constexpr int32_t AVENGE_TIME = 0x2ee;

// everything one kill needs: the context for part B and the values only this part uses
struct Run {
    U3xKill k;
    uint8_t *ext;    // the helper's state block
    uint8_t *ngm;
    uint8_t *r0;     // kill record 0
    uint8_t *vrec;   // the victim's score record
    const uint8_t *mode;  // the mode entry
    bool ffa;
};

// the betrayal mark: record 0's byte 0x21, read from the record at every use
[[gnu::always_inline]] inline bool betrayal(const Run &r) { return rd<uint8_t>(r.r0, 0x21) != 0; }

template <typename T> [[gnu::always_inline]] inline T &fld(uint8_t *p, uintptr_t off) {
    return *reinterpret_cast<T *>(p + off);
}

[[gnu::always_inline]] inline bool has(uint32_t word, uint32_t bit) { return (word & bit) != 0; }
[[gnu::always_inline]] inline int32_t team_of(const uint8_t *np) { return rd<int32_t>(np, 0x21c); }
[[gnu::always_inline]] inline uint32_t twice(int32_t v) { return static_cast<uint32_t>(v) << 1; }

[[gnu::always_inline]] inline void stat(Run &r, uint32_t id, uint32_t amount, uint8_t *np) {
    u3h_kr_stat(r.k.base, r.k.s, id, amount, np);
}
[[gnu::always_inline]] inline bool ask(Run &r) { return u3x_kr_friendly(r.k.base, r.k.knp, r.k.vnp) != 0; }

// one of the two local players
U3X_FN bool is_local(const Run &r, const uint8_t *np) {
    return np == rd<uint8_t *>(r.ngm, 0x370) || np == rd<uint8_t *>(r.ngm, 0x378);
}

// the game is online, or co-op cash is shown
U3X_FN bool online_or_shown(const Run &r) {
    if (game_call<uint8_t>(r.k.base + FN_ONLINE, r.ngm)) return true;
    return (rd<uint32_t>(u3h_ext(), 0x4824) & 1) != 0;
}

// 0 or 3 for a fixed-score kill, the game's normal kill score otherwise
U3X_FN int32_t kill_score(const Run &r) {
    if (has(r.k.flags, FL_FIXED_SCORE)) return u3x_kr_34fc04(r.k.base) ? 0 : 3;
    return u3x_kr_37c9f4(r.k.base);
}

// one more bonus kill in a score record, and its stat event
U3X_FN void bonus_count(Run &r, uint8_t *rec, uint8_t *np) {
    fld<uint8_t>(rec, 0x57)++;
    stat(r, STAT_BONUS_KILLS, fld<uint8_t>(rec, 0x57), np);
}

// a message coordinate in sixteenths, low 16 bits
[[gnu::always_inline]] inline uint16_t sixteenths(const uint8_t *info, uintptr_t off) {
    return static_cast<uint16_t>(f_to_int(f_mul(rd<uint32_t>(info, off), F_SIXTEEN)));
}

// 4.3: move the history down by one and write record 0
U3X_FN void history(Run &r) {
    U3xKill &k = r.k;
    uint8_t *recs = k.S + S_RECORDS;
    if (k.n) {
        uint32_t i = k.n - 1 < 0x1fd ? k.n - 1 : 0x1fd;
        for (;; i--) {
            uint8_t *to = recs + REC_SIZE * (i + 1);
            const uint8_t *from = recs + REC_SIZE * i;
            for (uint32_t b = 0; b < 0x26; b++) to[b] = from[b];
            if (!i) break;
        }
    }
    uint8_t *r0 = r.r0;
    const uint32_t fl = k.flags;
    fld<uint16_t>(r0, 2) = has(fl, FL_KILLER_PLAYER) ? static_cast<uint16_t>(k.killer_id) : 0;
    fld<uint16_t>(r0, 4) = has(fl, FL_VICTIM_PLAYER) ? static_cast<uint16_t>(k.victim_id) : 0;
    fld<int32_t>(r0, 8) = rd<int32_t>(k.info, 0);
    fld<uint8_t>(r0, 0) = 0;  // the kind byte is 0 while the game call below runs; its value is set further down
    uint8_t *slot = game_call<uint8_t *>(k.base + FN_SYMBOL, SYM_EXTRA_STAT_WEAPONS);
    const uint32_t *extra = slot ? rd<const uint32_t *>(slot, 0) : nullptr;
    fld<uint8_t>(r0, 0xc) = static_cast<uint8_t>(
        u3x_weapon_stat_number(reinterpret_cast<const uint32_t *>(k.base + IMG_STAT_WEAPONS), extra, k.weapon));
    for (uint32_t a = 0; a < 3; a++) {
        fld<uint16_t>(r0, 0xe + 2 * a) = sixteenths(k.info, 0x10 + 4 * a);
        fld<uint16_t>(r0, 0x14 + 2 * a) = sixteenths(k.info, 0x20 + 4 * a);
    }
    fld<int8_t>(r0, 6) = -1;
    fld<int8_t>(r0, 7) = -1;
    fld<int32_t>(r0, 0x1c) = rd<int32_t>(k.info, 0x6c);
    fld<uint8_t>(r0, 0x23) = (fl >> 27) & 1;
    fld<uint8_t>(r0, 0x24) = (fl >> 13) & 1;
    fld<uint8_t>(r0, 0) = has(fl, FL_EXPLOSIVE) ? 6 : has(fl, FL_KIND1) ? 1 : has(fl, FL_KIND2) ? 2 : 0;
    fld<uint8_t>(r0, 0x25) = 0;
    if (k.n < 0x200) {
        k.n++;
        fld<uint32_t>(k.S, S_COUNT) = k.n;
    }
    fld<uint32_t>(r.ext, 0x2bd0) = k.n;
}

// 4.4: the handle of a victim that is not a player
U3X_FN void victim_entity(Run &r) {
    U3xKill &k = r.k;
    uint32_t h = 0x811c9dc5u;
    for (uint32_t b = 0; b < 4; b++) h = (h ^ ((k.victim_id >> (8 * b)) & 0xff)) * 0x01000193u;
    const uint32_t size = rd<uint32_t>(r.ngm, 0x655c);
    uint8_t **table = rd<uint8_t **>(r.ngm, 0x6520);
    k.victim_entity = nullptr;
    if (size && table) {
        uint8_t *node = table[h % size];
        while (node && rd<uint32_t>(node, 0x10) != k.victim_id) node = rd<uint8_t *>(node, 0);
        if (node && node != table[size]) k.victim_entity = node + 0x18;
    }
    if (k.victim_entity && !has(rd<uint32_t>(k.s, S_SWITCHES), SW_NO_UNCOUNTED_MARK)) {
        const uint8_t *c = u3x_handle_obj(k.victim_entity);
        if (c && ((rd<uint64_t>(c, 0x708) >> 48) & 1)) fld<uint8_t>(r.r0, 0x25) = 1;
    }
}

// 4.6: the kill goes to the player who attacked the killer last
U3X_FN void betrayal_credit(Run &r) {
    U3xKill &k = r.k;
    const uint8_t *clock = at<uint8_t *>(k.base + IMG_CLOCK);
    const uint32_t now = clock ? rd<uint32_t>(clock, 0xc) : 0;
    // still running: one signed comparison of the two clock values
    const bool running = rd<int32_t>(k.knp, 0x9a0) > static_cast<int32_t>(now);
    uint8_t *att = u3h_kr_np_by_id(k.base, running ? rd<uint32_t>(k.knp, 0x9a4) : 0);
    if (!att) {
        if (!has(k.flags, FL_BONUS)) {
            const uint32_t other = static_cast<uint32_t>(1 - team_of(k.knp));
            u3x_team_score(k.base, k.s, k.S, other, kill_score(r));
        } else if (!u3x_kr_34fc04(k.base)) {
            // the killer's team is read after u3x_kr_34fc04 has returned
            const uint32_t other = static_cast<uint32_t>(1 - team_of(k.knp));
            u3x_team_score(k.base, k.s, k.S, other, u3x_kr_37c984(k.base));
        }
        return;
    }
    if (team_of(att) == team_of(k.knp)) return;
    uint8_t *rec = u3h_kr_prec(k.S, att);
    if (k.counts) fld<int16_t>(rec, 8)++;
    if (has(k.flags, FL_BONUS)) {
        fld<uint8_t>(rec, 0x57)++;
        u3x_add_score(k.base, k.s, k.S, att, u3x_kr_37c984(k.base));
        stat(r, STAT_BONUS_KILLS, fld<uint8_t>(rec, 0x57), att);
    } else {
        u3x_add_score(k.base, k.s, k.S, att, kill_score(r));
    }
}

// 4.7: score of the kill and the points to award; false = no award
U3X_FN bool kill_points(Run &r, int32_t *points) {
    U3xKill &k = r.k;
    const int32_t t = rd<int32_t>(r.mode, 0xc);  // the mode type, read again from the mode entry
    if (!game_call<uint8_t>(k.base + FN_KILL_SCORES, r.ngm)) {
        if (t == MODE_10 && k.on) {
            int32_t p = u3x_kill_points(k.base, KP_KILL, k.knp);
            if (!(rd<uint32_t>(r.ext, 0x4450) & 1)) {
                p = static_cast<int32_t>(twice(p));
                fld<uint32_t>(r.ext, 0x4490)++;
            }
            *points = p;
            return true;
        }
        if (r.ffa) {
            u3x_add_score(k.base, k.s, k.S, k.knp, 1);
            if (!k.on) return false;
            *points = u3x_kill_points(k.base, KP_KILL, k.knp);
            return true;
        }
        if (!k.on || k.coop) return false;
        *points = u3x_kill_points(k.base, KP_KILL_NO_SCORE, k.knp);
        return true;
    }
    u3x_add_score(k.base, k.s, k.S, k.knp, kill_score(r));
    if (has(k.flags, FL_EXTRA_SCORE)) {
        u3x_add_score(k.base, k.s, k.S, k.knp, u3x_kr_37c914(k.base));
        bonus_count(r, k.krec, k.knp);
    }
    if (!k.on) return false;
    if (t != MODE_F) {
        *points = u3x_kill_points(k.base, KP_KILL, k.knp);
    } else {
        const uint8_t which = game_call<uint8_t>(k.base + FN_PAIR_RULE, reinterpret_cast<uint8_t *>(k.base + IMG_PAIR_RULE), k.knp, k.vnp);
        *points = u3x_kill_points(k.base, which ? KP_KILL_F_YES : KP_KILL_F_NO, k.knp);
    }
    return true;
}

// 4.8: a player killed an enemy player
U3X_FN void enemy_kill(Run &r) {
    U3xKill &k = r.k;
    if (!has(k.flags, FL_BONUS)) {
        int32_t points = 0;
        if (kill_points(r, &points) && points) {
            uint32_t doubled = 0;
            if (u3x_double_points(k.base, k.knp)) {
                points = static_cast<int32_t>(twice(points));
                doubled = 1;
            }
            if (betrayal(r)) {
                points = static_cast<int32_t>(twice(points));
                doubled = 1;
            }
            if (is_local(r, k.knp)) u3x_kill_popup(k.base, k.s, k.knp, k.vnp, k.info, points, 0, doubled);
            u3x_award(k.base, k.s, k.S, k.knp, points, 0, 0);
        }
    } else {
        u3x_add_score(k.base, k.s, k.S, k.knp, u3x_kr_37c984(k.base));
        bonus_count(r, k.krec, k.knp);
    }
    if (r.ffa) return;
    // avenged a team mate: the victim killed someone of the killer's team a moment ago
    const uint32_t when = rd<uint32_t>(k.info, 0);
    for (uint32_t i = 1; i < k.n; i++) {
        const uint8_t *rec = k.S + S_RECORDS + REC_SIZE * i;
        if (static_cast<int32_t>(when - rd<uint32_t>(rec, 8)) > AVENGE_TIME) break;
        if (rd<int8_t>(rec, 6) != rd<int8_t>(rec, 7) && rd<uint16_t>(rec, 2) == static_cast<uint16_t>(k.victim_id) &&
            rd<uint16_t>(rec, 4) != static_cast<uint16_t>(k.killer_id) && rd<int8_t>(rec, 7) == team_of(k.knp)) {
            stat(r, STAT_AVENGED, 1, k.knp);
            break;
        }
    }
}

// 4.5: killer and victim are players; true = the kill counts as a kill
U3X_FN bool player_kill(Run &r) {
    U3xKill &k = r.k;
    if (ask(r)) {
        if (k.counts) fld<int16_t>(k.krec, 0x12)++;
        if (betrayal(r)) betrayal_credit(r);
    }
    k.on = online_or_shown(r);
    bool own_side = k.self_kill != 0;
    if (!own_side) {
        if (ask(r)) {
            own_side = true;
        } else if (!ask(r)) {
            enemy_kill(r);
        }
    }
    if (own_side) {
        if (!betrayal(r)) {
            // the other team scores
            const uint32_t t = team_of(k.vnp) == 0 ? 1 : 0;
            if (has(k.flags, FL_BONUS)) {
                u3x_team_score(k.base, k.s, k.S, t, u3x_kr_37c984(k.base));
            } else if (has(k.flags, FL_FIXED_SCORE)) {
                u3x_team_score(k.base, k.s, k.S, t, u3x_kr_34fc04(k.base) ? 0 : 3);
            }
        } else if (!k.self_kill) {
            ask(r);
        }
    }
    return !ask(r);
}

// 4.9: the assisting players
U3X_FN void assists(Run &r) {
    U3xKill &k = r.k;
    if (r.ffa) return;
    for (uint32_t i = 0; i < 8; i++) {
        int32_t kt = rd<int8_t>(r.r0, 6);
        if (u3h_kr_mode_type(k.base) == MODE_12 && kt == -1) kt = 1;
        const uint32_t id = rd<uint32_t>(k.info, 0x40 + 4 * i);
        uint8_t *a = id ? u3h_kr_np_by_id(k.base, id) : nullptr;
        if (!a || team_of(a) == rd<int8_t>(r.r0, 7) || team_of(a) != kt) continue;
        uint8_t *rec = u3h_kr_prec(k.S, a);
        if (k.counts) {
            fld<uint16_t>(rec, 0xe)++;
            fld<uint32_t>(r.ext, 0x2bf8)++;
        }
        if (online_or_shown(r)) {
            int32_t points;
            if (u3h_kr_mode_type(k.base) == MODE_F) {
                const uint8_t which = game_call<uint8_t>(k.base + FN_PAIR_RULE, reinterpret_cast<uint8_t *>(k.base + IMG_PAIR_RULE), a, k.vnp);
                points = u3x_kill_points(k.base, which ? KP_ASSIST_F_YES : KP_ASSIST_F_NO, a);
            } else {
                const uint32_t p = static_cast<uint32_t>(u3x_kill_points(k.base, KP_ASSIST, a));
                const uint32_t m = static_cast<uint32_t>(u3x_kr_30ffa8(k.base, k.knp, has(k.flags, FL_COOP_COUNTS) ? 1 : 0));
                points = static_cast<int32_t>(p * m);
            }
            if (u3x_double_points(k.base, k.knp)) points = static_cast<int32_t>(twice(points));
            if (is_local(r, a)) u3x_kill_popup(k.base, k.s, a, k.vnp, k.info, points, 1, 0);
            u3x_award(k.base, k.s, k.S, a, points, 0, 0);
        }
        stat(r, STAT_ASSISTS, fld<uint16_t>(rec, 0xe), a);
    }
}

}  // namespace g46a

// 4.1 and 4.2: one kill
extern "C" U3X_FN void u3x_kr_port(uintptr_t base, uint8_t *info, void *s) {
    using namespace g46a;
    Run r;
    U3xKill &k = r.k;
    k.base = base;
    k.info = info;
    k.s = static_cast<uint8_t *>(s);
    r.ext = u3h_ext();
    const uint32_t bits = rd<uint32_t>(k.s, S_SWITCHES);
    fld<uint32_t>(r.ext, 0x2bc0)++;
    if (has(bits, SW_OFF)) {
        fld<uint32_t>(r.ext, 0x2bfc)++;
        return;
    }
    r.ngm = reinterpret_cast<uint8_t *>(base + IMG_NGM);
    r.mode = game_call<const uint8_t *>(base + FN_MODE_ENTRY, r.ngm);
    if (!r.mode) return;
    fld<uint32_t>(r.ext, 0x2bc4)++;
    k.counts = !has(bits, SW_NO_COUNTS);
    k.medals = !has(bits, SW_NO_MEDALS);
    k.coop = game_call<uint8_t>(base + FN_COOP_FLAG, r.ngm) != 0;
    const int32_t t0 = rd<int32_t>(r.mode, 0xc);
    r.ffa = t0 == MODE_FFA;
    k.flags = rd<uint32_t>(info, 0x60);
    k.killer_id = rd<uint32_t>(info, 0x30);
    k.victim_id = rd<uint32_t>(info, 0x34);
    k.weapon = rd<uint32_t>(info, 0x38);
    fld<uint32_t>(r.ext, 0x2bdc) = k.flags;
    fld<uint32_t>(r.ext, 0x2be0) = k.killer_id;
    fld<uint32_t>(r.ext, 0x2be8) = k.victim_id;
    k.S = reinterpret_cast<uint8_t *>(base + IMG_SCORE);
    k.n = rd<uint32_t>(k.S, S_COUNT);
    r.r0 = k.S + S_RECORDS;
    k.victim_entity = nullptr;
    k.self_kill = 0;
    k.on = 0;

    if (!has(bits, SW_NO_HISTORY)) history(r);
    k.knp = has(k.flags, FL_KILLER_PLAYER) ? u3h_kr_np_by_id(base, k.killer_id) : nullptr;
    k.vnp = has(k.flags, FL_VICTIM_PLAYER) ? u3h_kr_np_by_id(base, k.victim_id) : nullptr;
    if (!has(k.flags, FL_VICTIM_PLAYER)) victim_entity(r);
    if (u3h_kr_30feec(base) && !has(k.flags, FL_COOP_COUNTS)) fld<uint8_t>(r.r0, 0x25) = 1;
    k.krec = u3h_kr_prec(k.S, k.knp);
    r.vrec = u3h_kr_prec(k.S, k.vnp);
    if (k.knp) fld<uint8_t>(r.r0, 6) = static_cast<uint8_t>(team_of(k.knp));
    if (k.vnp) fld<uint8_t>(r.r0, 7) = static_cast<uint8_t>(team_of(k.vnp));
    fld<uint8_t>(r.r0, 0x22) = (k.flags >> 20) & 1;
    fld<uint8_t>(r.r0, 0x21) = (k.flags >> 19) & 1;

    bool kill;
    if (k.knp && k.knp == k.vnp) {
        // suicide
        k.self_kill = 1;
        if (k.counts) {
            fld<int16_t>(r.vrec, 0xc)++;
            fld<int16_t>(k.krec, 0x10)++;
            fld<uint32_t>(r.ext, 0x2bd4)++;
        }
        kill = player_kill(r);
    } else {
        if (r.ffa) {
            fld<uint8_t>(r.r0, 6) = 0;
            fld<uint8_t>(r.r0, 7) = 1;
        }
        if (k.vnp && k.counts) {
            fld<int16_t>(r.vrec, 0xc)++;
            fld<uint32_t>(r.ext, 0x2bd4)++;
        }
        if (!k.knp) {
            // no player killer: only the assists and the places
            assists(r);
            game_call<void>(base + FN_PLACES, k.S);
            return;
        }
        if (k.vnp) {
            kill = player_kill(r);
        } else {
            k.on = online_or_shown(r);
            kill = true;
        }
    }
    if (kill) u3x_kr_counted(&k);
    u3x_kr_aftermath(&k);
    assists(r);
    game_call<void>(base + FN_PLACES, k.S);
}
