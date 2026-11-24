// G46 part B: the kill record's counted kill, kill statistics, co-op AI victim, aftermath, streaks and weapon groups.
// Written from spec G46_kill_record.md (4.10 to 4.15).

// The kill context, filled by part A and only read here (spec 2.2).
#ifndef U3X_KILL_DEFINED
#define U3X_KILL_DEFINED
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
#endif

extern "C" {
// C part and other groups (spec 2.3)
void u3h_kr_stat(uintptr_t base, void *s, uint32_t id, uint32_t amount, uint8_t *np);
uint32_t u3h_crc_sid(const char *name);
int u3x_kr_friendly(uintptr_t base, const uint8_t *a, const uint8_t *b);
uint32_t u3x_kr_sid_cat(uintptr_t base, uint32_t h, const char *text);
void u3x_kr_prof_add(uintptr_t base, const uint8_t *np, uint32_t n, uint32_t amount);
void u3x_kill_popup(uintptr_t base, void *s, uint8_t *np, uint8_t *other, const uint8_t *info, int32_t pts, uint32_t type, uint32_t dbl);
void u3x_award(uintptr_t base, void *s, uint8_t *S, uint8_t *np, int32_t pts, uint32_t type, uint32_t flag);
int32_t u3x_ai_kill_points(uintptr_t base, uint32_t w, uint32_t id, uint8_t *knp, int b);
void u3x_booster_kill(uintptr_t base, void *s, uint8_t *knp, uint32_t flags, uint32_t w);
void u3x_weapon_boosters(uintptr_t base, void *s, uint8_t *S, uint8_t *knp);
void u3x_category_medals(uintptr_t base, void *s, uint8_t *S, uint8_t *knp, const uint8_t *info, int team_mode);
void u3x_three_of_each(uintptr_t base, void *s, uint8_t *S, uint8_t *knp);
int u3x_score_member(const uint8_t *S, uint32_t id);
uint8_t *u3x_handle_obj(uint8_t *h);
int u3x_has_tag(const uint8_t *bucket_head, const uint8_t *tags, uint32_t id);
// the medal checks of G45
void u3x_kr_379c18(uintptr_t base, void *s, uint8_t *knp, uint8_t *info);
void u3x_kr_37952c(uintptr_t base, void *s, uint8_t *S, uint8_t *knp);
void u3x_kr_3817b8(uintptr_t base, void *s, uint8_t *S, uint8_t *knp, uint8_t *info, int team_mode);
void u3x_kr_37992c(uintptr_t base, void *s, uint8_t *S, uint8_t *knp, uint8_t *info);
void u3x_kr_37ab00(uintptr_t base, void *s, uint8_t *S, uint8_t *knp, uint8_t *info);
void u3x_kr_37a4bc(uintptr_t base, void *s, uint8_t *S, uint8_t *knp);
void u3x_kr_379ed8(uintptr_t base, void *s, uint8_t *S);
void u3x_kr_379ff0(uintptr_t base, void *s, uint8_t *S, uint8_t *knp);
void u3x_kr_379430(uintptr_t base, void *s, uint8_t *S, uint8_t *knp);
void u3x_kr_3791ec(uintptr_t base, void *s, uint8_t *S, uint8_t *knp, uint8_t *vnp, uint8_t *info);
void u3x_kr_37a914(uintptr_t base, void *s, uint8_t *S, uint8_t *knp);
void u3x_kr_37a16c(uintptr_t base, void *s, uint8_t *S, uint8_t *knp);
void u3x_kr_37a32c(uintptr_t base, void *s, uint8_t *S, uint8_t *knp, uint8_t *info);
// this part
void u3x_kr_counted(U3xKill *k);
void u3x_kr_aftermath(U3xKill *k);
int u3x_kr_weapon_group(uintptr_t base, uint32_t weapon);
}

namespace g46b {

// image offsets
constexpr uintptr_t NGM = 0x17cb060;               // net game manager
constexpr uintptr_t MATCH_RUNS = 0x17d5c61;        // u8: a multiplayer match runs
constexpr uintptr_t STAT_WEAPONS = 0x13a6170;      // 19 u32: the fixed statistics weapons
constexpr uintptr_t WEAPON_SLOTS = 0x13a66d0;      // u32 entries 1 .. 0x1a: the weapon slots
constexpr uintptr_t MISSION_TRACKERS = 0x17da200;  // one tracker of 0x1018 bytes per user
constexpr uintptr_t AI_NOTE_OBJECT = 0x17c3404;
constexpr uintptr_t TAG_TABLE = 0x1b3efd8;
constexpr uintptr_t FN_COOP_FLAG = 0x722670;
constexpr uintptr_t FN_WEAPON_KILL_NOTE = 0x773b50;
constexpr uintptr_t FN_AI_KILLED_NOTE = 0x661ca0;

// the helper's state block (u32 words)
constexpr uintptr_t EXT_KILLS = 0x2bd8;
constexpr uintptr_t EXT_CHECKS_1 = 0x2bec;
constexpr uintptr_t EXT_CHECKS_2 = 0x2bf0;
constexpr uintptr_t EXT_CHECKS_3 = 0x2bf4;
constexpr uintptr_t EXT_AI_CHARACTERS = 0x31e0;
constexpr uintptr_t EXT_AI_NOT_COUNTED = 0x31e4;   // low 16 bits only
constexpr uintptr_t EXT_AI_STATS = 0x31e8;
constexpr uintptr_t EXT_AI_LAST_STAT = 0x31ec;

// switch bits of the u32 at s + 0x3d78
constexpr uintptr_t SWITCHES = 0x3d78;
constexpr uint32_t SW_NO_POINTS = 0x4000;
constexpr uint32_t SW_NO_AI_CHARACTER = 0x8000000;

// stat events
constexpr uint32_t ST_MATCH_KILLS = 0x5c494554;
constexpr uint32_t ST_FLAG_2000000 = 0x15fd6bcd;
constexpr uint32_t ST_FLAG_40000000 = 0x1e8153a6;
constexpr uint32_t ST_FLAG_8000000_COUNT = 0xd4f91f9a;
constexpr uint32_t ST_FLAG_8000000_GROUP6 = 0xbb000af1;
constexpr uint32_t ST_FLAG_8000000_GROUP4 = 0x5398047b;
constexpr uint32_t ST_WEAPON_3D2F108B = 0xc2b00a95;
constexpr uint32_t ST_FLAG_4000000 = 0x78573763;
constexpr uint32_t ST_FLAG_10000000 = 0xd323c81d;
constexpr uint32_t ST_FLAG_20000000 = 0xdc380255;
constexpr uint32_t ST_FLAG_100 = 0x76f0c8c8;
constexpr uint32_t ST_FLAG_200 = 0x77240247;
constexpr uint32_t ST_FLAG_400 = 0xcff265ed;
constexpr uint32_t ST_FLAG_8000 = 0x87cbf9cb;
constexpr uint32_t ST_FLAG_800 = 0x3838b544;
constexpr uint32_t ST_FLAG_1000 = 0x68774d94;
constexpr uint32_t ST_FLAG_20000 = 0xb4210460;
constexpr uint32_t ST_FLAG_400000 = 0x6685348e;
constexpr uint32_t ST_FLAG_800000 = 0x0b854093;
constexpr uint32_t ST_FLAG_1000000 = 0xcd644ff7;
constexpr uint32_t ST_FLAG_2000_COUNT = 0xa622ff01;
constexpr uint32_t ST_FLAG_10000 = 0xe5ad0b24;
constexpr uint32_t ST_LONG_SHOT = 0xd294ccd1;
constexpr uint32_t ST_FIRST_KILL = 0x1bdbd578;
constexpr uint32_t ST_AI_MARKED = 0x0c65fecd;
constexpr uint32_t ST_AI_SPECIAL_STATE = 0x9c498eb3;
constexpr uint32_t ST_AI_KIND_2 = 0x35cfda1f;
constexpr uint32_t ST_AI_KIND_4 = 0x38771768;
constexpr uint32_t ST_FLAG_80 = 0x744a942b;
constexpr uint32_t ST_FLAG_4 = 0xb759f1c7;
constexpr uint32_t ST_FLAG_40 = 0xaa22db16;
constexpr uint32_t ST_FLAG_10 = 0x5cff7436;
constexpr uint32_t ST_OWN_SIDE_FLAG_100000 = 0xffc1c7b5;
constexpr uint32_t ST_FLAG_8 = 0x89c96ded;
constexpr uint32_t ST_KILLS_WITHOUT_DYING = 0x58b0d53d;
constexpr uint32_t ST_DEATHS_WITHOUT_KILL = 0x6f593a9b;

// single ids
constexpr uint32_t WEAPON_GROUP4_SPECIAL = 0x3d2f108b;
constexpr uint32_t WEAPON_KIND2_COUNT = 0xb934b2b2;
constexpr uint32_t TAG_ID = 0xfd2c9c09;
constexpr uintptr_t TAG_BUCKET = 0x9e8;            // that tag's bucket head inside the tag table

constexpr uint32_t PROF_AI_KILLS = 0x17b;
constexpr uint32_t PROF_GROUP6_KILLS = 0x17c;
constexpr uint32_t PROF_STEALTH_KILLS = 0x17d;

constexpr uint32_t F_900 = 0x44610000;             // 900.0 = 30 squared

U3X_RO const char killed_suffix[] = {'-', 'k', 'i', 'l', 'l', 'e', 'd', 0};
U3X_RO const char stealth_weapon_name[] = {'n', 'e', 't', '-', 'k', 'i', 'l', 'l', '-', 's', 't', 'e', 'a', 'l', 't', 'h', 0};

template <typename T> [[gnu::always_inline]] inline T &field(uint8_t *p, uintptr_t off) { return *reinterpret_cast<T *>(p + off); }

[[gnu::always_inline]] inline void count_ext(uintptr_t off) { ++field<uint32_t>(u3h_ext(), off); }

[[gnu::always_inline]] inline void stat(const U3xKill *k, uint32_t id, uint32_t amount) {
    u3h_kr_stat(k->base, k->s, id, amount, k->knp);
}

// kill record i of the score keeper (record 0 is the newest)
[[gnu::always_inline]] inline uint8_t *record(const U3xKill *k, uint32_t i) { return k->S + 0xe4 + 0x28 * static_cast<uintptr_t>(i); }
[[gnu::always_inline]] inline uint16_t rec_killer(const uint8_t *r) { return rd<uint16_t>(r, 2); }
[[gnu::always_inline]] inline uint16_t rec_victim(const uint8_t *r) { return rd<uint16_t>(r, 4); }
[[gnu::always_inline]] inline int8_t rec_killer_team(const uint8_t *r) { return rd<int8_t>(r, 6); }
[[gnu::always_inline]] inline int8_t rec_victim_team(const uint8_t *r) { return rd<int8_t>(r, 7); }

[[gnu::always_inline]] inline bool is_local(uintptr_t base, const uint8_t *np) {
    const uint8_t *ngm = reinterpret_cast<const uint8_t *>(base + NGM);
    return np == rd<const uint8_t *>(ngm, 0x370) || np == rd<const uint8_t *>(ngm, 0x378);
}

[[gnu::always_inline]] inline bool f_is_nan(uint32_t a) { return (a & 0x7fffffffu) > 0x7f800000u; }

// 4.11 step 11: the squared distance between killer and victim is at least 900
U3X_FN bool long_shot(const uint8_t *info) {
    uint32_t sum = 0;
    for (uintptr_t axis = 0; axis < 12; axis += 4) {
        const uint32_t d = f_sub(rd<uint32_t>(info, 0x10 + axis), rd<uint32_t>(info, 0x20 + axis));
        sum = f_add(sum, f_mul(d, d));
    }
    return !f_is_nan(sum) && !f_less(sum, F_900);
}

// 4.11: the kill statistics
U3X_FN void kill_statistics(const U3xKill *k) {
    const uintptr_t base = k->base;
    const uint32_t flags = k->flags;
    uint8_t *krec = k->krec;

    if (flags & 0x2000000) stat(k, ST_FLAG_2000000, ++field<uint8_t>(krec, 0x65));
    if (flags & 0x40000000) stat(k, ST_FLAG_40000000, 1);

    bool weapon_step = true;
    if (flags & 0x8000000) {
        uint32_t c = 0;   // records of all killers
        for (uint32_t i = 0; i < k->n; ++i)
            if (rd<uint8_t>(record(k, i), 0x23)) ++c;
        stat(k, ST_FLAG_8000000_COUNT, c);
        const int group = u3x_kr_weapon_group(base, k->weapon);
        if (group == 6) stat(k, ST_FLAG_8000000_GROUP6, 1);
        else if (group == 4) stat(k, ST_FLAG_8000000_GROUP4, 1);
        else weapon_step = false;
    }
    if (weapon_step && k->weapon == WEAPON_GROUP4_SPECIAL) stat(k, ST_WEAPON_3D2F108B, 1);

    if (flags & 0x4000000) stat(k, ST_FLAG_4000000, 1);
    if (flags & 0x10000000) stat(k, ST_FLAG_10000000, 1);
    if (flags & 0x20000000) stat(k, ST_FLAG_20000000, 1);
    if (flags & 0x100) stat(k, ST_FLAG_100, ++field<uint8_t>(krec, 0x56));
    if (flags & 0x200) stat(k, ST_FLAG_200, ++field<uint16_t>(krec, 0x58));

    if (flags & 0x400) stat(k, ST_FLAG_400, 1);
    if (flags & 0x8000) stat(k, ST_FLAG_8000, 1);
    if (flags & 0x800) stat(k, ST_FLAG_800, 1);
    if (flags & 0x1000) stat(k, ST_FLAG_1000, 1);
    if (flags & 0x20000) stat(k, ST_FLAG_20000, 1);
    if (flags & 0x400000) stat(k, ST_FLAG_400000, 1);
    if (flags & 0x800000) stat(k, ST_FLAG_800000, 1);
    if (flags & 0x1000000) stat(k, ST_FLAG_1000000, 1);

    // the victim kind's "...-killed" event
    stat(k, u3x_kr_sid_cat(base, rd<uint32_t>(k->info, 0x70), killed_suffix), 1);

    if (flags & 0x2000) {
        const uint16_t me = static_cast<uint16_t>(k->killer_id);
        uint32_t c = 0;
        for (uint32_t i = 0; i < k->n; ++i) {
            const uint8_t *r = record(k, i);
            if (rd<uint8_t>(r, 0x24) && rec_killer(r) == me && rec_killer_team(r) != rec_victim_team(r)) ++c;
        }
        stat(k, ST_FLAG_2000_COUNT, c);
    } else if (flags & 0x10000) {
        stat(k, ST_FLAG_10000, 1);
    }

    if (u3x_kr_weapon_group(base, k->weapon) == 4 && long_shot(k->info)) stat(k, ST_LONG_SHOT, 1);

    uint8_t &first_kill_done = at<uint8_t>(base + NGM + 0x31d);
    if (!first_kill_done) {
        stat(k, ST_FIRST_KILL, 1);
        first_kill_done = 1;
    }

    if (k->medals) {
        u3x_kr_379c18(base, k->s, k->knp, k->info);
        u3x_kr_37952c(base, k->s, k->S, k->knp);
        count_ext(EXT_CHECKS_1);
    }
}

// a stat event of the co-op AI victim step, with its diagnostics
U3X_FN void ai_stat(const U3xKill *k, uint32_t id, uint32_t amount) {
    stat(k, id, amount);
    count_ext(EXT_AI_STATS);
    field<uint32_t>(u3h_ext(), EXT_AI_LAST_STAT) = id;
}

// 4.12: the co-op AI victim step
U3X_FN void coop_ai_victim(const U3xKill *k) {
    const uintptr_t base = k->base;

    uint8_t *character = nullptr;
    if (k->victim_entity && !(rd<uint32_t>(k->s, SWITCHES) & SW_NO_AI_CHARACTER)) character = u3x_handle_obj(k->victim_entity);
    if (character) {
        count_ext(EXT_AI_CHARACTERS);
        if ((rd<uint64_t>(character, 0x708) >> 48) & 1) {   // not counted
            ++field<uint16_t>(u3h_ext(), EXT_AI_NOT_COUNTED);
            return;
        }
    }

    int32_t time = rd<int32_t>(k->info, 0);
    if (!(rd<uint32_t>(k->s, SWITCHES) & SW_NO_POINTS))
        game_call<void>(base + FN_AI_KILLED_NOTE, reinterpret_cast<uint8_t *>(base + AI_NOTE_OBJECT),
                        static_cast<uint32_t>(rd<uint8_t>(k->info, 0x64)), &time);

    const int32_t points = u3x_ai_kill_points(base, k->weapon, rd<uint32_t>(k->info, 0x68), k->knp, (k->flags >> 9) & 1);
    if (is_local(base, k->knp)) {
        if (k->on) u3x_kill_popup(base, k->s, k->knp, nullptr, k->info, points, 0, 0);
        u3x_kr_prof_add(base, k->knp, PROF_AI_KILLS, 1);
    }
    if (k->on) u3x_award(base, k->s, k->S, k->knp, points, 0, 0);

    if (!character) return;

    if ((rd<uint64_t>(character, 0x708) >> 49) & 1) ai_stat(k, ST_AI_MARKED, 1);

    const uint8_t *special = rd<const uint8_t *>(character, 0xbf8);
    if (special && rd<uint32_t>(special, 4)) {
        ai_stat(k, ST_AI_SPECIAL_STATE, 1);
        return;
    }
    const uint8_t *tags = rd<const uint8_t *>(character, 0x38);
    const uint8_t *table = reinterpret_cast<const uint8_t *>(base + TAG_TABLE);
    if (tags && rd<uint8_t>(table, 0) && u3x_has_tag(table + TAG_BUCKET, tags, TAG_ID)) return;

    const int32_t kind = rd<int32_t>(character, 0xc60);
    if (kind == 2) {
        if (k->weapon == WEAPON_KIND2_COUNT) ai_stat(k, ST_AI_KIND_2, ++field<uint8_t>(k->krec, 0x64));
    } else if (kind == 4) {
        ai_stat(k, ST_AI_KIND_4, 1);
    }
}

// 4.14: the streaks (the victim is a player)
U3X_FN void streaks(const U3xKill *k) {
    const uintptr_t base = k->base;
    const uint8_t *r0 = record(k, 0);

    if ((k->flags & 8) && rec_killer_team(r0) != rec_victim_team(r0)) {
        stat(k, ST_FLAG_8, 1);
        ++field<uint8_t>(k->krec, 0x6a);
    }

    // the killer's kills since his last death
    const uint16_t killer = static_cast<uint16_t>(k->killer_id);
    uint32_t c = 0;
    for (uint32_t i = 0; i < k->n; ++i) {
        const uint8_t *r = record(k, i);
        if (rd<uint8_t>(r, 0x25)) continue;
        if (rec_victim(r) == killer) break;
        if (rec_killer(r) != killer) continue;
        if (rec_victim_team(r) == rec_killer_team(r0)) break;  // R0 read again at every use
        ++c;
    }
    stat(k, ST_KILLS_WITHOUT_DYING, c);

    if (k->medals) u3x_kr_37a32c(base, k->s, k->S, k->knp, k->info);

    // the victim's deaths since his last kill
    const uint16_t victim = static_cast<uint16_t>(k->victim_id);
    c = 0;
    for (uint32_t i = 0; i < k->n; ++i) {
        const uint8_t *r = record(k, i);
        if (rd<uint8_t>(r, 0x25)) continue;
        if (rec_killer(r) == victim) break;
        if (rec_victim(r) != victim) continue;
        if (rec_victim_team(r) == rec_killer_team(r0)) break;  // R0 read again at every use
        ++c;
    }
    u3h_kr_stat(base, k->s, ST_DEATHS_WITHOUT_KILL, c, k->vnp);

    if (k->medals) {
        u3x_kr_3791ec(base, k->s, k->S, k->knp, k->vnp, k->info);
        u3x_kr_37a914(base, k->s, k->S, k->knp);
        u3x_three_of_each(base, k->s, k->S, k->knp);
        u3x_kr_37a16c(base, k->s, k->S, k->knp);
        count_ext(EXT_CHECKS_3);
    }
}

// 4.13: what runs only for a counted record (byte 0x25 clear)
U3X_FN void counted_aftermath(const U3xKill *k) {
    const uintptr_t base = k->base;
    const uint32_t flags = k->flags;
    uint8_t *krec = k->krec;
    const uint8_t *r0 = record(k, 0);

    if (!k->self_kill) {
        if (k->vnp && !u3x_kr_friendly(base, k->knp, k->vnp) && (flags & 0x80)) stat(k, ST_FLAG_80, ++field<uint16_t>(krec, 0x54));
        if (flags & 4) stat(k, ST_FLAG_4, ++field<uint16_t>(krec, 0x48));
        if (flags & 0x40) stat(k, ST_FLAG_40, ++field<uint16_t>(krec, 0x4e));
    }
    if (flags & 0x10) stat(k, ST_FLAG_10, 1);

    if (k->medals) u3x_kr_3817b8(base, k->s, k->S, k->knp, k->info, k->coop);

    if (u3x_kr_weapon_group(base, k->weapon) == 6) {
        if (is_local(base, k->knp)) {
            u3x_kr_prof_add(base, k->knp, PROF_GROUP6_KILLS, 1);
            if (k->weapon == u3h_crc_sid(stealth_weapon_name)) u3x_kr_prof_add(base, k->knp, PROF_STEALTH_KILLS, 1);
        }
        if (k->medals) u3x_kr_37992c(base, k->s, k->S, k->knp, k->info);
    } else if (k->medals) {
        u3x_category_medals(base, k->s, k->S, k->knp, k->info, k->coop);
    }

    if (k->medals) {
        u3x_kr_37ab00(base, k->s, k->S, k->knp, k->info);
        u3x_kr_37a4bc(base, k->s, k->S, k->knp);
        u3x_kr_379ed8(base, k->s, k->S);
        u3x_kr_379ff0(base, k->s, k->S, k->knp);
        u3x_kr_379430(base, k->s, k->S, k->knp);
        count_ext(EXT_CHECKS_2);
    }

    if (rd<uint8_t>(r0, 0x22) && rec_killer_team(r0) == rec_victim_team(r0) && !k->self_kill && !rd<uint8_t>(r0, 0x21))
        stat(k, ST_OWN_SIDE_FLAG_100000, 1);

    if (k->vnp) streaks(k);
}

}  // namespace g46b

// 4.15: 6 for a close-combat / stealth kill kind, 4 for a thrown explosive, else 0; both sets are read from the image
extern "C" U3X_FN int u3x_kr_weapon_group(uintptr_t base, uint32_t weapon) {
    const uint32_t *stat_weapons = reinterpret_cast<const uint32_t *>(base + g46b::STAT_WEAPONS);
    for (uint32_t i = 1; i <= 13; ++i) {
        const bool close_combat = i == 1 || (i >= 6 && i <= 9) || i == 13;
        if (close_combat && stat_weapons[i] == weapon) return 6;
    }
    const uint32_t *slots = reinterpret_cast<const uint32_t *>(base + g46b::WEAPON_SLOTS);
    for (uint32_t j = 20; j <= 23; ++j)
        if (slots[j] == weapon) return 4;
    return 0;
}

// 4.10: a kill of an enemy player or of a non-player victim
extern "C" U3X_FN void u3x_kr_counted(U3xKill *k) {
    using namespace g46b;
    const uintptr_t base = k->base;
    uint8_t *krec = k->krec;
    uint8_t *ngm = reinterpret_cast<uint8_t *>(base + NGM);

    if (k->counts) {
        ++field<int16_t>(krec, 8);
        count_ext(EXT_KILLS);

        if (at<uint8_t>(base + MATCH_RUNS) && game_call<uint8_t>(base + FN_COOP_FLAG, ngm) && rd<int32_t>(k->knp, 0x21c) == 1)
            ++field<int16_t>(krec, 0x6c);

        const int member = u3x_score_member(k->S, k->killer_id);
        if (member >= 0) {
            const uint32_t *slots = reinterpret_cast<const uint32_t *>(base + WEAPON_SLOTS);
            uint32_t slot = 0;   // any other weapon
            for (uint32_t j = 1; j <= 0x1a; ++j)
                if (slots[j] == k->weapon) { slot = j; break; }
            ++field<int16_t>(k->S, 0x60cc + 0x130 * static_cast<uintptr_t>(member) + 0xee + 2 * slot);
        }
    }

    stat(k, ST_MATCH_KILLS, rd<uint16_t>(krec, 8));

    if (is_local(base, k->knp)) {
        uint8_t *tracker = reinterpret_cast<uint8_t *>(base + MISSION_TRACKERS + 0x1018 * static_cast<uintptr_t>(rd<uint32_t>(k->knp, 0x1e8)));
        game_call<void>(base + FN_WEAPON_KILL_NOTE, tracker, k->weapon, static_cast<uint32_t>(1), static_cast<uint32_t>(0), k->weapon,
                        static_cast<uint32_t>(rd<uint8_t>(k->info, 0x3c)));
    }

    if (!rd<uint8_t>(record(k, 0), 0x25)) kill_statistics(k);

    u3x_booster_kill(base, k->s, k->knp, k->flags, k->weapon);

    if (k->victim_entity && k->counts) ++field<int16_t>(krec, 0xa);

    if (k->coop) coop_ai_victim(k);
}

// 4.13: after the kill, counted or not
extern "C" U3X_FN void u3x_kr_aftermath(U3xKill *k) {
    using namespace g46b;
    if (!rd<uint8_t>(record(k, 0), 0x25)) counted_aftermath(k);
    if (is_local(k->base, k->knp)) u3x_weapon_boosters(k->base, k->s, k->S, k->knp);
}
