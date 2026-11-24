// G45 part A: medal conditions over the kill history (weapons and float maths).
// Written from spec/G45_medal_conditions.md, sections 3 and 4.1 to 4.6.

extern "C" {
void u3h_kr_stat(uintptr_t base, void *s, uint32_t id, uint32_t amount, uint8_t *np);
uint8_t *u3h_kr_prec(uint8_t *S, uint8_t *np);
uint32_t u3x_net_player_id(const uint8_t *np);
uint32_t u3x_weapon_for_stat(const uint32_t *fixed, const uint32_t *extra, uint32_t number);
uint32_t u3x_weapon_stat_number(const uint32_t *fixed, const uint32_t *extra, uint32_t weapon);
int u3x_kr_weapon_group(uintptr_t base, uint32_t weapon);

void u3x_kr_379c18(uintptr_t base, void *s, uint8_t *knp, uint8_t *info);
void u3x_kr_37952c(uintptr_t base, void *s, uint8_t *S, uint8_t *knp);
void u3x_kr_3817b8(uintptr_t base, void *s, uint8_t *S, uint8_t *knp, uint8_t *info, int team_mode);
void u3x_kr_37992c(uintptr_t base, void *s, uint8_t *S, uint8_t *knp, uint8_t *info);
void u3x_kr_37ab00(uintptr_t base, void *s, uint8_t *S, uint8_t *knp, uint8_t *info);
void u3x_kr_37a4bc(uintptr_t base, void *s, uint8_t *S, uint8_t *knp);
}

namespace g45a {

// stat event ids, one per condition
constexpr uint32_t EV_STEEP_ABOVE = 0x427bfc52u;        // 4.1: victim steeply above the killer
constexpr uint32_t EV_STEEP_BELOW = 0x8a14a6a4u;        // 4.1: victim steeply below the killer
constexpr uint32_t EV_DEATHS_BEFORE_KILL = 0x5852e7edu; // 4.2
constexpr uint32_t EV_EXPLOSIVE_MULTI = 0x5bc68f9cu;    // 4.3
constexpr uint32_t EV_EXPLOSIVE_MULTI_E2 = 0x2de62476u; // 4.3: team mode, weapon E2
constexpr uint32_t EV_EXPLOSIVE_MULTI_E3 = 0x55917637u; // 4.3: team mode, weapon E3
constexpr uint32_t EV_GROUP6_RUN = 0xd2b04848u;         // 4.4: run of group-6 kills
constexpr uint32_t EV_GROUP6_Q5 = 0x7fdbcbbfu;          // 4.4: kills with Q5 / Q5b
constexpr uint32_t EV_CHAIN = 0xf2c76c63u;              // 4.5: first chain
constexpr uint32_t EV_CHAIN_SPECIAL = 0x4bae48b4u;      // 4.5: chain of the two special weapons
constexpr uint32_t EV_LONG_RANGE = 0xa8932480u;         // 4.6

// script symbols
constexpr uint32_t SYM_EXTRA_WEAPONS = 0x9e354807u;
constexpr uint32_t SYM_CHAIN_WINDOW = 0x47efb8e4u;
constexpr uint32_t SYM_CHAIN_WINDOW_SPECIAL = 0xee602459u;
constexpr uint32_t SYM_LONG_RANGE = 0x42b7081bu;

// float patterns
constexpr uint32_t F_FIVE = 0x40a00000u, F_MINUS_FIVE = 0xc0a00000u;
constexpr uint32_t F_SIN60 = 0x3f5db3d7u, F_MINUS_SIN60 = 0xbf5db3d7u;
constexpr uint32_t F_PER_TICK = 0x3b5a740eu;      // 1/300
constexpr uint32_t F_SIXTEENTH = 0x3d800000u;     // 1/16

// image offsets
constexpr uintptr_t FN_SYMBOL = 0x11f2bf0;
constexpr uintptr_t TABLE_F = 0x13a6170;          // fixed weapon table
constexpr uintptr_t TABLE_G = 0x13a66d0;          // second weapon table
constexpr uintptr_t Q5B_IMMEDIATE = 0xc478bb;     // unaligned u32 inside a game instruction

// ---- kill history
U3X_FN static uint32_t record_count(const uint8_t *S) { return rd<uint32_t>(S, 0xe0); }
U3X_FN static const uint8_t *record(const uint8_t *S, uint32_t i) { return S + 0xe4 + 0x28 * static_cast<uintptr_t>(i); }
U3X_FN static uint32_t killer_of(const uint8_t *r) { return rd<uint16_t>(r, 2); }
U3X_FN static uint32_t victim_of(const uint8_t *r) { return rd<uint16_t>(r, 4); }
U3X_FN static bool enemy_kill(const uint8_t *r) { return rd<int8_t>(r, 6) != rd<int8_t>(r, 7); }
U3X_FN static bool uncounted(const uint8_t *r) { return rd<uint8_t>(r, 0x25) != 0; }
U3X_FN static int32_t time_of(const uint8_t *r) { return rd<int32_t>(r, 8); }
U3X_FN static uint32_t weapon_number_of(const uint8_t *r) { return rd<uint8_t>(r, 0xc); }

// ---- float
U3X_FN static bool f_nan(uint32_t a) { return (a & 0x7fffffffu) > 0x7f800000u; }
// a <= b (false when either is a NaN)
U3X_FN static bool f_le(uint32_t a, uint32_t b) { return !f_nan(a) && !f_nan(b) && !f_less(b, a); }
U3X_FN static uint32_t seconds(int32_t ticks) { return f_mul(f_from_int(ticks), F_PER_TICK); }

// one coordinate of a record's killer-to-victim difference, in units
U3X_FN static uint32_t record_delta(const uint8_t *r, uintptr_t coordinate) {
    const int32_t k = rd<int16_t>(r, 0xe + 2 * coordinate);
    const int32_t v = rd<int16_t>(r, 0x14 + 2 * coordinate);
    return f_mul(f_sub(f_from_int(k), f_from_int(v)), F_SIXTEENTH);
}
U3X_FN static uint32_t squared_distance(const uint8_t *r) {
    uint32_t sum = 0;
    for (uintptr_t c = 0; c < 3; ++c) {
        const uint32_t d = record_delta(r, c);
        sum = f_add(sum, f_mul(d, d));
    }
    return sum;
}

// ---- script symbols and weapons
// the data of a script symbol, or null (one game call)
U3X_FN static const uint8_t *symbol_data(uintptr_t base, uint32_t id) {
    const uint8_t *const *held = game_call<const uint8_t *const *, uint32_t>(base + FN_SYMBOL, id);
    return held ? *held : nullptr;
}
U3X_FN static const uint32_t *fixed_weapons(uintptr_t base) { return reinterpret_cast<const uint32_t *>(base + TABLE_F); }
U3X_FN static uint32_t weapon_f(uintptr_t base, uint32_t k) { return at<uint32_t>(base + TABLE_F + 4 * k); }
U3X_FN static uint32_t weapon_g(uintptr_t base, uint32_t k) { return at<uint32_t>(base + TABLE_G + 4 * k); }
// the extra weapon list (one lookup); may be null
U3X_FN static const uint32_t *extra_weapons(uintptr_t base) {
    return reinterpret_cast<const uint32_t *>(symbol_data(base, SYM_EXTRA_WEAPONS));
}
// weapon id of a weapon stat number: the extra list is looked up only for numbers past the fixed table
U3X_FN static uint32_t weapon_of_number(uintptr_t base, uint32_t number) {
    const uint32_t *extra = number < 0x13 ? nullptr : extra_weapons(base);
    return u3x_weapon_for_stat(fixed_weapons(base), extra, number);
}
// weapon stat number of a weapon id: always one lookup
U3X_FN static uint32_t number_of_weapon(uintptr_t base, uint32_t weapon) {
    return u3x_weapon_stat_number(fixed_weapons(base), extra_weapons(base), weapon);
}
U3X_FN static uint32_t weapon_of_record(uintptr_t base, const uint8_t *r) { return weapon_of_number(base, weapon_number_of(r)); }
// the weapon id the game compares with at 0xc478bb (unaligned)
U3X_FN static uint32_t weapon_q5b(uintptr_t base) {
    const uint8_t *p = reinterpret_cast<const uint8_t *>(base + Q5B_IMMEDIATE);
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}

// 4.5: the length of a chain of the player's enemy kills, each within the window (first value of the symbol data,
// read again for every record) of the next newer one. The special chain looks at every record and counts only
// the two special weapons (numbers n1, n2); the plain one skips uncounted records.
U3X_FN static uint32_t chain_length(const uint8_t *S, const uint8_t *info, const uint8_t *window, uint32_t me,
                                    bool special, uint32_t n1, uint32_t n2) {
    int32_t last = rd<int32_t>(info, 0);
    uint32_t count = 1;
    for (uint32_t i = 1; i < record_count(S); ++i) {
        const uint8_t *r = record(S, i);
        if (!special && uncounted(r)) continue;
        if (f_less(rd<uint32_t>(window, 0), f_sub(seconds(last), seconds(time_of(r))))) break;
        if (killer_of(r) != me || !enemy_kill(r)) continue;
        if (special && weapon_number_of(r) != n1 && weapon_number_of(r) != n2) continue;
        ++count;
        last = time_of(r);
    }
    return count;
}

}  // namespace g45a

// 4.1 steep kill
extern "C" U3X_FN void u3x_kr_379c18(uintptr_t base, void *s, uint8_t *knp, uint8_t *info) {
    using namespace g45a;
    const uint32_t dx = f_sub(rd<uint32_t>(info, 0x20), rd<uint32_t>(info, 0x10));
    const uint32_t dy = f_sub(rd<uint32_t>(info, 0x24), rd<uint32_t>(info, 0x14));
    const uint32_t dz = f_sub(rd<uint32_t>(info, 0x28), rd<uint32_t>(info, 0x18));
    if (!f_le(F_FIVE, dy) && !f_le(dy, F_MINUS_FIVE)) return;
    const uint32_t length = f_root(f_add(f_add(f_mul(dz, dz), f_mul(dx, dx)), f_mul(dy, dy)));
    const uint32_t share = f_div(dy, length);
    if (f_less(F_SIN60, share)) u3h_kr_stat(base, s, EV_STEEP_ABOVE, 1, knp);
    else if (f_less(share, F_MINUS_SIN60)) u3h_kr_stat(base, s, EV_STEEP_BELOW, 1, knp);
}

// 4.2 deaths before this kill
extern "C" U3X_FN void u3x_kr_37952c(uintptr_t base, void *s, uint8_t *S, uint8_t *knp) {
    using namespace g45a;
    const uint32_t me = u3x_net_player_id(knp) & 0xffffu;
    uint32_t count = 0;
    for (uint32_t i = 1; i < record_count(S); ++i) {
        const uint8_t *r = record(S, i);
        if (killer_of(r) == me) break;
        if (victim_of(r) == me) ++count;
    }
    u3h_kr_stat(base, s, EV_DEATHS_BEFORE_KILL, count, knp);
}

// 4.3 explosive multi-kill
extern "C" U3X_FN void u3x_kr_3817b8(uintptr_t base, void *s, uint8_t *S, uint8_t *knp, uint8_t *info, int team_mode) {
    using namespace g45a;
    if (!(rd<uint32_t>(info, 0x60) & 0x20u)) return;
    const uint32_t weapon = rd<uint32_t>(info, 0x38);
    const uint32_t killer = rd<uint32_t>(info, 0x30) & 0xffffu;
    const uint32_t now = rd<uint32_t>(info, 0);
    uint32_t count = 0;
    for (uint32_t i = 0; i < record_count(S); ++i) {
        const uint8_t *r = record(S, i);
        if (uncounted(r) || killer_of(r) != killer || !enemy_kill(r)) continue;
        // within one second of the message time, on either side
        const int32_t ticks = static_cast<int32_t>(static_cast<uint32_t>(time_of(r)) - now);
        if (!f_less(seconds(ticks) & 0x7fffffffu, F_ONE)) continue;
        if (weapon_of_record(base, r) == weapon) ++count;
    }
    const uint32_t amount = count & 0xffu;
    uint32_t event = EV_EXPLOSIVE_MULTI;
    bool silent = false;
    if (team_mode) {
        if (weapon == weapon_f(base, 10)) silent = true;                        // E1
        else if (weapon == weapon_g(base, 17)) event = EV_EXPLOSIVE_MULTI_E2;   // E2
        else if (weapon == weapon_f(base, 5)) event = EV_EXPLOSIVE_MULTI_E3;    // E3
    }
    if (!silent) u3h_kr_stat(base, s, event, amount, knp);
    // the best explosive multi-kill of the player, also when nothing was reported
    uint8_t *best = u3h_kr_prec(S, knp) + 0x52;
    if (amount > *best) *best = static_cast<uint8_t>(amount);
}

// 4.4 group-6 weapon counts
extern "C" U3X_FN void u3x_kr_37992c(uintptr_t base, void *s, uint8_t *S, uint8_t *knp, uint8_t *info) {
    using namespace g45a;
    if (!enemy_kill(record(S, 0))) return;
    const uint32_t killer = rd<uint32_t>(info, 0x30);   // the full id, compared with the 16-bit record fields
    const uint32_t weapon = rd<uint32_t>(info, 0x38);

    // kills with this weapon, and kills with this weapon or any group-6 weapon
    uint32_t same = 0, group = 0;
    for (uint32_t i = 0; i < record_count(S); ++i) {
        const uint8_t *r = record(S, i);
        if (uncounted(r) || killer_of(r) != killer || !enemy_kill(r)) continue;
        const uint32_t x = weapon_of_record(base, r);
        if (x == weapon) { ++same; ++group; }
        else if (u3x_kr_weapon_group(base, x) == 6) ++group;
    }
    u3h_kr_stat(base, s, weapon_f(base, 1), group, knp);   // the id of Q0 is the event id

    // the newest run of group-6 kills, ended by another weapon or by the player being killed
    uint32_t run = 0;
    for (uint32_t i = 0; i < record_count(S); ++i) {
        const uint8_t *r = record(S, i);
        if (killer_of(r) == killer && enemy_kill(r)) {
            if (u3x_kr_weapon_group(base, weapon_of_record(base, r)) != 6) break;
            ++run;
        } else if (victim_of(r) == killer) {
            break;
        }
    }
    u3h_kr_stat(base, s, EV_GROUP6_RUN, run, knp);

    if (weapon == weapon_f(base, 6) || weapon == weapon_f(base, 7) || weapon == weapon_f(base, 8) ||
        weapon == weapon_f(base, 9))
        u3h_kr_stat(base, s, weapon, same, knp);            // Q1 to Q4: the weapon id is the event id
    else if (weapon == weapon_f(base, 13) || weapon == weapon_q5b(base))
        u3h_kr_stat(base, s, EV_GROUP6_Q5, same, knp);      // Q5, Q5b
}

// 4.5 chains
extern "C" U3X_FN void u3x_kr_37ab00(uintptr_t base, void *s, uint8_t *S, uint8_t *knp, uint8_t *info) {
    using namespace g45a;
    const uint8_t *window = symbol_data(base, SYM_CHAIN_WINDOW);
    const uint32_t me = u3x_net_player_id(knp) & 0xffffu;
    if (!window) return;
    const uint8_t *r0 = record(S, 0);
    if (enemy_kill(r0)) u3h_kr_stat(base, s, EV_CHAIN, chain_length(S, info, window, me, false, 0, 0), knp);

    const uint32_t n1 = number_of_weapon(base, weapon_g(base, 14));   // C1
    const uint32_t n2 = number_of_weapon(base, weapon_g(base, 15));   // C2
    const uint8_t *window2 = symbol_data(base, SYM_CHAIN_WINDOW_SPECIAL);
    if (!window2) return;
    if (weapon_number_of(r0) != n1 && weapon_number_of(r0) != n2) return;
    if (!enemy_kill(r0)) return;
    u3h_kr_stat(base, s, EV_CHAIN_SPECIAL, chain_length(S, info, window2, me, true, n1, n2), knp);
}

// 4.6 long range
extern "C" U3X_FN void u3x_kr_37a4bc(uintptr_t base, void *s, uint8_t *S, uint8_t *knp) {
    using namespace g45a;
    const uint8_t *range = symbol_data(base, SYM_LONG_RANGE);
    if (!range) return;
    const uint32_t me = u3x_net_player_id(knp) & 0xffffu;
    const uint8_t *r0 = record(S, 0);
    if (killer_of(r0) != me || !enemy_kill(r0)) return;
    const uint32_t far2 = f_mul(rd<uint32_t>(range, 0), rd<uint32_t>(range, 0));
    if (!f_le(far2, squared_distance(r0))) return;
    uint32_t count = 1;
    for (uint32_t i = 1; i < record_count(S); ++i) {
        const uint8_t *r = record(S, i);
        if (!uncounted(r) && killer_of(r) == me && enemy_kill(r) && f_le(far2, squared_distance(r))) ++count;
    }
    u3h_kr_stat(base, s, EV_LONG_RANGE, count, knp);
}
