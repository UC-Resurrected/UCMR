// G44 part B: booster rewards of a kill and the category medals (spec G44_kill_rewards.md, sections 4.15 to 4.21).
// Written from the behaviour description only.

extern "C" {
// C part and other groups (spec 2.4)
uint32_t u3x_net_player_id(const uint8_t *np);
uint32_t u3h_kr_statw(uintptr_t base, uint32_t index);
uint8_t *u3h_kr_np_by_id(uintptr_t base, uint32_t id);
uint8_t *u3h_kr_prec(uint8_t *S, uint8_t *np);
void u3h_kr_stat(uintptr_t base, void *s, uint32_t id, uint32_t amount, uint8_t *np);
uint32_t u3h_crc_sid(const char *name);
uint32_t u3x_float_call(uintptr_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);

// this part (spec 2.2)
uint8_t *u3x_weapon_definition(uintptr_t base, uint32_t w);
void u3x_give_grenades(uintptr_t base, uint8_t *inventory, int32_t n, void *out);
void u3x_booster_kill(uintptr_t base, void *s, uint8_t *knp, uint32_t flags, uint32_t w);
void u3x_weapon_boosters(uintptr_t base, void *s, uint8_t *S, uint8_t *knp);
void u3x_category_medals(uintptr_t base, void *s, uint8_t *S, uint8_t *knp, const uint8_t *info, int team_mode);
void u3x_three_of_each(uintptr_t base, void *s, uint8_t *S, uint8_t *knp);
}

namespace g44b {

// ---- image offsets (spec 3.3 to 3.5)
constexpr uintptr_t NET_GAME_MANAGER = 0x17cb060;
constexpr uintptr_t SCRIPT_SYMBOL_FN = 0x11f2e30;
constexpr uintptr_t WEAPON_DATA_FN = 0xd415e0;
constexpr uintptr_t OBJECT_DEFINITION_FN = 0xd436b0;
constexpr uintptr_t BOOSTER_VALUE_FN = 0x689d20;
constexpr uintptr_t BOOSTER_OWNER = 0x17c3b78;
constexpr uintptr_t IDENTITY_ROTATION = 0x1402ac0;
constexpr uintptr_t KICKBACK_POINTS = 0x17d79f0;
constexpr uintptr_t KICKBACK_STRIDE = 0x20;   // per local user, as the spec states it (section 6, note 4)

// ---- helper state block counters, switch word of the state object
constexpr uintptr_t COUNT_BOOSTER_VALUES = 0x2d90;
constexpr uintptr_t COUNT_AMMUNITION = 0x2d94;
constexpr uintptr_t COUNT_GRENADES = 0x2d98;
constexpr uintptr_t COUNT_KICKBACK = 0x2d9c;
constexpr uintptr_t SWITCHES = 0x3d78;
constexpr uint32_t SWITCH_NO_BOOSTERS = 0x10000;

// ---- score keeper and kill records
constexpr uintptr_t KILL_COUNT = 0xe0;
constexpr uintptr_t KILL_RECORDS = 0xe4;
constexpr uintptr_t KILL_RECORD_SIZE = 0x28;
constexpr uint8_t KILL_KIND_EXPLOSIVE = 6;
constexpr uintptr_t SETS_REWARDED = 0x4c;

// ---- ids without a known name (spec 3.7)
constexpr uint32_t SYMBOL_WEAPON_DEFINITIONS = 0x61d589d9;
constexpr uint32_t WEAPON_HAND_GRENADE = 0x0b8bd6d6;
constexpr uint32_t WEAPON_SINGLE_STAT = 0x159df996;      // its kill is one stat event
constexpr uint32_t WEAPON_AMMO_ALWAYS = 0xc92d29c1;      // ammunition booster for every kill
constexpr uint32_t WEAPON_AMMO_KIND_1 = 0x5136f850;      // ammunition booster for a kill of kind 1
constexpr uint32_t WEAPON_COUNTED = 0x33e29a1b;          // its kills are counted over the history
constexpr uint32_t STAT_SINGLE_WEAPON = 0xa828aa55;
constexpr uint32_t STAT_COUNTED_WEAPON = 0xcc5567c6;
constexpr uint32_t EFFECT_AMMO_SHARE = 0x0a1b799a;
constexpr uint32_t EFFECT_GRENADE_KILL = 0xfca72e93;
constexpr uint32_t EFFECT_STEALTH_KICKBACK = 0x8e5e947e;
constexpr uint32_t EFFECT_WEAPON_AMMO = 0x966ccd32;
constexpr uint32_t EFFECT_STEALTH_LONG_GUN = 0xed737cfb;
constexpr uint32_t EFFECT_STEALTH_GRENADES = 0x43d8284d;
constexpr uint32_t CATEGORY_NONE = 0xfffffffd;

// ---- ids that are the string ids of these names
U3X_RO const char name_stealth[] = {'n','e','t','-','k','i','l','l','-','s','t','e','a','l','t','h',0};
U3X_RO const char name_propane[] = {'p','r','o','p','a','n','e',0};
U3X_RO const char name_sniper_kills[] = {'n','e','t','-','s','n','i','p','e','r','-','k','i','l','l','s',0};
U3X_RO const char name_sniper_streak[] = {'n','e','t','-','s','n','i','p','e','r','-','s','t','r','e','a','k',0};
U3X_RO const char name_power_kills[] = {'n','e','t','-','p','o','w','e','r','-','w','e','a','p','o','n','-','k','i','l','l','s',0};
U3X_RO const char name_kill_propane[] = {'n','e','t','-','k','i','l','l','-','p','r','o','p','a','n','e',0};
U3X_RO const char name_triple_threat[] = {'n','e','t','-','k','i','l','l','-','t','r','i','p','l','e','-','t','h','r','e','a','t',0};
U3X_RO const char name_ammo_award[] = {'a','m','m','o','-','a','w','a','r','d',0};

// the stat name of each weapon category; an empty name = the category has no stat
constexpr uint32_t CATEGORIES = 6;
U3X_RO const char category_stat_names[CATEGORIES][20] = {
    {'n','e','t','-','k','i','l','l','-','p','i','s','t','o','l',0},
    {'n','e','t','-','k','i','l','l','-','l','o','n','g','g','u','n',0},
    {'n','e','t','-','k','i','l','l','-','g','r','e','n','a','d','e',0},
    {0},
    {'n','e','t','-','k','i','l','l','-','t','u','r','r','e','t',0},
    {'n','e','t','-','k','i','l','l','-','h','e','a','v','y',0},
};

// ---- small pieces

U3X_FN void count(uintptr_t offset) {
    ++*reinterpret_cast<uint32_t *>(u3h_ext() + offset);
}

[[gnu::always_inline]] inline bool switch_set(const void *s, uint32_t bit) {
    return (rd<uint32_t>(s, SWITCHES) & bit) != 0;
}

// one of the two local players' records
U3X_FN bool is_local(uintptr_t base, const uint8_t *np) {
    const uintptr_t ngm = base + NET_GAME_MANAGER;
    return np == at<const uint8_t *>(ngm + 0x370) || np == at<const uint8_t *>(ngm + 0x378);
}

// the character a net player's handle still points at, or null
U3X_FN uint8_t *character_of(const uint8_t *np) {
    uint8_t *const *handle = rd<uint8_t *const *>(np, 0x10);
    const uint32_t handle_id = rd<uint32_t>(np, 0x18);
    if (!handle || !handle_id) return nullptr;
    uint8_t *who = *handle;
    if (!who || rd<uint32_t>(who, 0x7c) != handle_id) return nullptr;
    return who;
}

// the inventory of a net player's character, or null
U3X_FN uint8_t *inventory_of(const uint8_t *np) {
    const uint8_t *who = character_of(np);
    return who ? rd<uint8_t *>(who, 0x520) : nullptr;
}

// the function in slot `slot` of an object's virtual table
[[gnu::always_inline]] inline uintptr_t virtual_slot(const uint8_t *object, uintptr_t slot) {
    return rd<uintptr_t>(rd<const uint8_t *>(object, 0), slot);
}

// the value of a booster effect for a player, as a float pattern
U3X_FN uint32_t booster_value(uintptr_t base, uint32_t effect, uint8_t *np, void *scratch) {
    const uint32_t value = u3x_float_call(base + BOOSTER_VALUE_FN, base + BOOSTER_OWNER, effect,
                                          reinterpret_cast<uintptr_t>(np), reinterpret_cast<uintptr_t>(scratch));
    count(COUNT_BOOSTER_VALUES);
    return value;
}

[[gnu::always_inline]] inline bool positive(uint32_t value) { return f_less(0, value); }

// whole part of (integer × float value)
[[gnu::always_inline]] inline int32_t scaled(int32_t amount, uint32_t value) {
    return f_to_int(f_mul(f_from_int(amount), value));
}

U3X_FN void add_ammunition(uint8_t *inventory, uint32_t weapon, int32_t amount, void *scratch) {
    struct Order { uint32_t weapon; int32_t amount; uint64_t zero; } order;
    order.weapon = weapon;
    order.amount = amount;
    order.zero = 0;
    game_call<void>(virtual_slot(inventory, 0xe8), inventory, &order, scratch);
    count(COUNT_AMMUNITION);
}

U3X_FN void add_kickback_points(uintptr_t base, const uint8_t *np, int32_t points) {
    int32_t &word = at<int32_t>(base + KICKBACK_POINTS + KICKBACK_STRIDE * rd<uint32_t>(np, 0x1e8));
    const int32_t sum = static_cast<int32_t>(static_cast<uint32_t>(word) + static_cast<uint32_t>(points));
    word = sum < 0 ? 0 : sum;
    count(COUNT_KICKBACK);
}

U3X_FN uint8_t *weapon_data(uintptr_t base, uint32_t weapon) {
    return game_call<uint8_t *>(base + WEAPON_DATA_FN, weapon);
}

// weapon data (not null) of a sniper rifle
[[gnu::always_inline]] inline bool is_sniper(const uint8_t *data) {
    const uint8_t *tuning = rd<const uint8_t *>(data, 0);
    return tuning && rd<int32_t>(tuning, 0x17c) == 4;
}

[[gnu::always_inline]] inline bool is_power(const uint8_t *data) { return rd<int32_t>(data, 0x20) != 0; }

// the definition of a weapon object (pick-up size at +0x58), or null
U3X_FN const uint8_t *object_definition(uintptr_t base, const uint8_t *weapon) {
    return game_call<const uint8_t *>(base + OBJECT_DEFINITION_FN, weapon);
}

[[gnu::always_inline]] inline const uint8_t *kill_record(const uint8_t *S, uint32_t i) {
    return S + KILL_RECORDS + KILL_RECORD_SIZE * static_cast<uintptr_t>(i);
}

struct Kill {
    const uint8_t *r;
    uint8_t kind() const { return rd<uint8_t>(r, 0); }
    uint16_t killer() const { return rd<uint16_t>(r, 2); }
    uint16_t victim() const { return rd<uint16_t>(r, 4); }
    bool teams_differ() const { return rd<int8_t>(r, 6) != rd<int8_t>(r, 7); }
    uint8_t weapon_index() const { return rd<uint8_t>(r, 0xc); }
    bool counted() const { return rd<uint8_t>(r, 0x25) == 0; }
};

}  // namespace g44b

// 4.17: the definition of a weapon from the script's weapon table (open addressing, home slot = id modulo slots)
U3X_FN uint8_t *u3x_weapon_definition(uintptr_t base, uint32_t w) {
    const uint8_t *table = game_call<const uint8_t *>(base + g44b::SCRIPT_SYMBOL_FN, g44b::SYMBOL_WEAPON_DEFINITIONS);
    if (!table) return nullptr;
    const uint32_t slots = rd<uint32_t>(table, 0);
    if (!slots || !w) return nullptr;
    const uint32_t *keys = rd<const uint32_t *>(table, 8);
    uint8_t *const *definitions = rd<uint8_t *const *>(table, 0x10);
    uint32_t slot = w % slots;
    for (uint32_t tried = 0; tried < slots; ++tried) {
        if (keys[slot] == w) return definitions[slot];
        if (!keys[slot]) return nullptr;
        if (++slot == slots) slot = 0;
    }
    return nullptr;
}

// 4.16: a grenade pick-up of n grenades for an inventory
U3X_FN void u3x_give_grenades(uintptr_t base, uint8_t *inventory, int32_t n, void *out) {
    alignas(16) uint8_t block[0xa0];
    for (uint32_t i = 0; i < sizeof block; ++i) block[i] = 0;
    const uint8_t *rotation = reinterpret_cast<const uint8_t *>(base + g44b::IDENTITY_ROTATION);
    for (uint32_t i = 0; i < 16; ++i) {
        block[0x10 + i] = rotation[i];
        block[0x30 + i] = rotation[i];
    }
    block[0x6c] = 0xff;
    *reinterpret_cast<uint32_t *>(block + 0x78) = g44b::WEAPON_HAND_GRENADE;
    *reinterpret_cast<int32_t *>(block + 0x7c) = n;
    block[0x90] = 0xff;
    game_call<void>(g44b::virtual_slot(inventory, 0xf0), inventory, static_cast<void *>(block), uint32_t{0}, uint32_t{0}, out);
    g44b::count(g44b::COUNT_GRENADES);
}

// 4.18: what the boosters of the player linked to the killer give for this kill
U3X_FN void u3x_booster_kill(uintptr_t base, void *s, uint8_t *knp, uint32_t flags, uint32_t w) {
    using namespace g44b;
    uint8_t *player = u3h_kr_np_by_id(base, rd<uint32_t>(knp, 0x674));
    if (!player || !is_local(base, player) || switch_set(s, SWITCH_NO_BOOSTERS)) return;

    if (uint8_t *inventory = inventory_of(player)) {
        uint64_t scratch = 0;
        if (!(flags & 0x20)) {
            // a share of a pick-up for the weapon in hand
            const uint32_t share = booster_value(base, EFFECT_AMMO_SHARE, player, &scratch);
            if (positive(share)) {
                const uint8_t *held = game_call<const uint8_t *>(virtual_slot(inventory, 0x80), inventory);
                if (held) {
                    const uint8_t *definition = object_definition(base, held);
                    if (definition)
                        add_ammunition(inventory, rd<uint32_t>(held, 0x6ec), scaled(rd<int32_t>(definition, 0x58), share), &scratch);
                }
            }
        } else {
            const int32_t grenades = f_to_int(booster_value(base, EFFECT_GRENADE_KILL, player, &scratch));
            if (grenades) u3x_give_grenades(base, inventory, grenades, &scratch);
        }
    }

    if (w == u3h_crc_sid(name_stealth))
        add_kickback_points(base, player, f_to_int(booster_value(base, EFFECT_STEALTH_KICKBACK, player, nullptr)));
}

// 4.19: the boosters that depend on the weapon of the newest kill
U3X_FN void u3x_weapon_boosters(uintptr_t base, void *s, uint8_t *S, uint8_t *knp) {
    using namespace g44b;
    if (switch_set(s, SWITCH_NO_BOOSTERS)) return;
    const Kill newest{kill_record(S, 0)};
    const uint32_t weapon = u3h_kr_statw(base, newest.weapon_index());
    uint8_t *inventory = inventory_of(knp);
    if (!inventory) return;
    uint64_t scratch = 0;

    if (weapon == WEAPON_AMMO_ALWAYS || (weapon == WEAPON_AMMO_KIND_1 && newest.kind() == 1)) {
        const int32_t amount = f_to_int(booster_value(base, EFFECT_WEAPON_AMMO, knp, &scratch));
        if (amount > 0) add_ammunition(inventory, weapon, amount, &scratch);
    }

    if (weapon == u3h_crc_sid(name_stealth)) {
        // pick-ups for the long gun
        const int32_t pickups = f_to_int(booster_value(base, EFFECT_STEALTH_LONG_GUN, knp, &scratch));
        if (pickups) {
            const uint8_t *long_gun = game_call<const uint8_t *>(virtual_slot(inventory, 0x78), inventory);
            if (long_gun) {
                const uint8_t *definition = object_definition(base, long_gun);
                if (definition) {
                    const uint32_t amount = static_cast<uint32_t>(pickups) * static_cast<uint32_t>(rd<int32_t>(definition, 0x58));
                    add_ammunition(inventory, rd<uint32_t>(long_gun, 0x6ec), static_cast<int32_t>(amount), &scratch);
                }
            }
        }
        // grenades: as ammunition while the player still holds some, else as a pick-up
        const int32_t grenades = f_to_int(booster_value(base, EFFECT_STEALTH_GRENADES, knp, &scratch));
        if (grenades) {
            const uint8_t *held = game_call<const uint8_t *>(virtual_slot(inventory, 0x70), inventory, uint32_t{2});
            if (held && game_call<int32_t>(virtual_slot(held, 0x398), held) > 0)
                add_ammunition(inventory, rd<uint32_t>(held, 0x6ec), grenades, &scratch);
            else
                u3x_give_grenades(base, inventory, grenades, &scratch);
        }
    }
}

// 4.20: the stat events counted over the killer's history, and the ammunition booster of every third / fifth kill
U3X_FN void u3x_category_medals(uintptr_t base, void *s, uint8_t *S, uint8_t *knp, const uint8_t *info, int team_mode) {
    using namespace g44b;
    if (!Kill{kill_record(S, 0)}.teams_differ()) return;

    const uint32_t weapon = rd<uint32_t>(info, 0x38);
    const uint16_t killer = static_cast<uint16_t>(rd<uint32_t>(info, 0x30));
    const uint8_t *definition = u3x_weapon_definition(base, weapon);
    const uint32_t category = definition ? rd<uint32_t>(definition, 8) : CATEGORY_NONE;

    if (weapon == WEAPON_SINGLE_STAT) u3h_kr_stat(base, s, STAT_SINGLE_WEAPON, 1, knp);

    uint32_t same_category = 0, sniper = 0, sniper_run = 0, power = 0, counted_weapon = 0;
    bool run_ended = false;
    // the kill count is read again every round: calls inside the loop may change it
    for (uint32_t i = 0; i < rd<uint32_t>(S, KILL_COUNT); ++i) {
        const Kill kill{kill_record(S, i)};
        if (!kill.counted()) continue;
        if (kill.killer() == killer && kill.teams_differ()) {
            const uint32_t used = u3h_kr_statw(base, kill.weapon_index());
            if (used == weapon) {
                ++same_category;
            } else {
                const uint8_t *other = u3x_weapon_definition(base, used);
                if (other && rd<uint32_t>(other, 8) == category) ++same_category;
            }
            const uint8_t *data = weapon_data(base, used);
            if (data && is_sniper(data)) {
                ++sniper;
                if (!run_ended) ++sniper_run;
            } else {
                run_ended = true;
            }
            if (data && is_power(data)) ++power;
            if (used == WEAPON_COUNTED) ++counted_weapon;
        } else if (kill.victim() == killer) {
            run_ended = true;
        }
    }

    if (const uint8_t *data = weapon_data(base, weapon)) {
        if (is_sniper(data)) {
            u3h_kr_stat(base, s, u3h_crc_sid(name_sniper_kills), sniper, knp);
            u3h_kr_stat(base, s, u3h_crc_sid(name_sniper_streak), sniper_run, knp);
        }
        if (is_power(data)) u3h_kr_stat(base, s, u3h_crc_sid(name_power_kills), power, knp);
    }

    if (weapon == WEAPON_COUNTED) u3h_kr_stat(base, s, STAT_COUNTED_WEAPON, counted_weapon, knp);

    if (category < CATEGORIES && category_stat_names[category][0])
        u3h_kr_stat(base, s, u3h_crc_sid(category_stat_names[category]), same_category, knp);

    if (category <= 1 && !switch_set(s, SWITCH_NO_BOOSTERS)) {
        const uint32_t every = team_mode ? 5 : 3;
        if (same_category % every == 0 && is_local(base, knp)) {
            uint64_t scratch = 0;
            const uint32_t share = booster_value(base, u3h_crc_sid(name_ammo_award), knp, &scratch);
            if (positive(share)) {
                const uint8_t *data = weapon_data(base, weapon);
                uint8_t *who = character_of(knp);
                const uint8_t *tuning = data && !is_power(data) ? rd<const uint8_t *>(data, 0) : nullptr;
                uint8_t *inventory = tuning && who ? rd<uint8_t *>(who, 0x520) : nullptr;
                if (inventory) add_ammunition(inventory, weapon, scaled(rd<int32_t>(tuning, 4), share), &scratch);
            }
        }
    }

    if (weapon == u3h_crc_sid(name_propane)) u3h_kr_stat(base, s, u3h_crc_sid(name_kill_propane), 1, knp);
}

// 4.21: one stat event when the killer has a new set of three pistol, three explosive and three long gun kills
U3X_FN void u3x_three_of_each(uintptr_t base, void *s, uint8_t *S, uint8_t *knp) {
    using namespace g44b;
    const uint16_t me = static_cast<uint16_t>(u3x_net_player_id(knp));
    uint32_t pistol = 0, explosive = 0, long_gun = 0;
    // the kill count is read again every round: calls inside the loop may change it
    for (uint32_t i = 0; i < rd<uint32_t>(S, KILL_COUNT); ++i) {
        const Kill kill{kill_record(S, i)};
        if (!kill.counted() || kill.killer() != me || kill.victim() == me || !kill.teams_differ()) continue;
        if (kill.kind() == KILL_KIND_EXPLOSIVE) {
            ++explosive;
            continue;
        }
        const uint8_t *definition = u3x_weapon_definition(base, u3h_kr_statw(base, kill.weapon_index()));
        if (!definition) continue;
        const uint32_t category = rd<uint32_t>(definition, 8);
        if (category == 0) ++pistol;
        else if (category == 1) ++long_gun;
    }
    uint32_t sets = pistol / 3;
    if (explosive / 3 < sets) sets = explosive / 3;
    if (long_gun / 3 < sets) sets = long_gun / 3;

    uint8_t *record = u3h_kr_prec(S, knp);
    uint16_t &rewarded = *reinterpret_cast<uint16_t *>(record + SETS_REWARDED);
    if (rewarded < sets) {
        rewarded = static_cast<uint16_t>(sets);
        u3h_kr_stat(base, s, u3h_crc_sid(name_triple_threat), 1, knp);
    }
}
