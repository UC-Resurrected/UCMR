// G48 part C: filling the player state message (spec G48_player_state.md, sections 2.3, 3.5, 3.7, 4.7 to 4.11).
// Written from the behaviour description only.

// ---- functions of other groups and of the C part (spec 2.4)
extern "C" uint32_t u3x_float_call(uintptr_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
extern "C" int32_t u3h_netgame_type(uintptr_t base, void *s);
extern "C" void u3h_weapons_fill(uintptr_t base, uint8_t *msg, uint8_t *pl);
extern "C" uint32_t u3h_np_code(uint8_t *np);
extern "C" uint8_t *u3h_np_by_id(uintptr_t base, uint32_t id);
extern "C" int32_t u3h_net_now(uintptr_t base);
extern "C" uint32_t *u3h_repl_error(void);
extern "C" uint32_t u3h_anim_pack_full(uintptr_t base, uint8_t *msg, uint8_t *bs, uint8_t *actor, uint8_t **tabs,
                                       int32_t p5, int32_t p6);
extern "C" void u3h_repl_loop(uintptr_t base, uint8_t *actor, uint8_t **tabs, uint8_t *msg, uint32_t bits);

// ---- this part's entry points (spec 2.3)
extern "C" uint32_t u3x_player_fill(uintptr_t base, void *s, uint8_t *msg, uint8_t *pl);
extern "C" void u3x_weapons_fill(uintptr_t base, uint8_t *msg, uint8_t *pl);
extern "C" uint32_t u3x_weapon_item(uintptr_t base, uint32_t id);
extern "C" uint32_t u3x_weapon_fixed_id(uintptr_t base, uint32_t index);
extern "C" uint32_t u3x_weapon_mode_119(uint32_t mode);
extern "C" int u3x_has_eff(uint8_t *c, uint32_t sid);
extern "C" uint8_t *u3x_char_np(uintptr_t base, uint8_t *c);

namespace g48c {

// image offsets (spec 3.1, 3.4)
constexpr uintptr_t IMG_LOCAL_NP = 0x17cb3d0;      // manager + 0x370: local net player record of screen 0, 1
constexpr uintptr_t IMG_NET_ACTIVE = 0x17d5c61;    // u8: multiplayer active
constexpr uintptr_t IMG_SESSION = 0x1b28330;       // u64: a net session exists
constexpr uintptr_t IMG_CLOCK_OBJECT = 0x17c3580;  // pointer: net clock object, s32 time at +0xc
constexpr uintptr_t IMG_BOOSTER_ARG = 0x17c3b78;   // first argument of the booster value function
constexpr uintptr_t IMG_CAMERAS_A = 0x17b39f0;
constexpr uintptr_t IMG_CAMERAS_B = 0x17b3138;
constexpr uintptr_t IMG_FIXED_WEAPONS = 0x13a6170; // 19 u32: fixed weapon ids, numbers 0..18
constexpr uintptr_t FN_BOOSTER_VALUE = 0x689d20;
constexpr uintptr_t FN_SCREEN_CAMERA = 0x4ef790;
constexpr uintptr_t FN_SCRIPT_SYMBOL = 0x11f2e30;

// ids and numbers
constexpr uint32_t WEAPON_NUMBER_19 = 0x2daa4324;  // the one fixed weapon PS3 1.19 added (number 19)
constexpr uint32_t IMAGE_WEAPON_COUNT = 19;        // numbers held by the image
constexpr uint32_t FIXED_WEAPON_COUNT = 20;        // first number of the extra list
constexpr uint32_t SYM_EXTRA_WEAPONS = 0x9e354807;
constexpr uint32_t CLASS_LOCAL_PLAYER = 0xa5a63e9b;
constexpr uint32_t CLASS_NET_CHARACTER = 0xf2c86c22;
constexpr uint32_t CLASS_CARRIED = 0x310f465e;
constexpr uint32_t EFF_NO_MOVE_1 = 0x5ec9a3e7, EFF_NO_MOVE_2 = 0xe9be68ac;
constexpr uint32_t EFF_HELD_1 = 0xd69dcc37, EFF_HELD_2 = 0x3eaffc07, EFF_HELD_3 = 0x16b32ade;
constexpr uint32_t EFF_ON_SCREEN = 0x974e6226;
constexpr uint32_t F_TICK = 0x3b5a740e;            // 1/300 s
constexpr uint32_t F_BELOW_ONE = 0x3f7fffff;
constexpr int32_t TARGET_TICKS = 900;
constexpr uint32_t STREAM_BYTES = 300;
constexpr uint32_t GAME_TYPE_NO_TARGET = 0xe;

// flag bits of message +0x148
constexpr uint32_t FLAG_PLAYER = 1, FLAG_MOVE_1 = 2, FLAG_STATE_3 = 4, FLAG_TEST_ZERO = 8, FLAG_MOVE_2 = 0x20,
                   FLAG_HELD = 0x40;

// the bit stream block handed to the packer
struct alignas(16) Stream {
    uint8_t *buffer;
    uint32_t position;
    uint32_t capacity;
};
struct alignas(16) AimBlock {
    uint64_t w[8];
};

template <typename T> [[gnu::always_inline]] inline void put(uint8_t *p, uintptr_t off, T v) {
    *reinterpret_cast<T *>(p + off) = v;
}
[[gnu::always_inline]] inline uint8_t *ptr_at(const void *p, uintptr_t off) { return rd<uint8_t *>(p, off); }

// method M of object O: the function at offset M of O's virtual table, O first; the result is a machine word
template <typename... A> [[gnu::always_inline]] inline uint64_t method(uint8_t *o, uintptr_t m, A... args) {
    return game_call<uint64_t, uint8_t *, A...>(rd<uintptr_t>(rd<const uint8_t *>(o, 0), m), o, args...);
}

[[gnu::always_inline]] inline uint32_t &ec(uint32_t n) { return reinterpret_cast<uint32_t *>(u3h_ext() + 0x4450)[n]; }

// the local net player record of a screen, null for a screen other than 0 and 1
[[gnu::always_inline]] inline uint8_t *local_np(uintptr_t base, int32_t screen) {
    if (screen != 0 && screen != 1) return nullptr;
    return at<uint8_t *>(base + IMG_LOCAL_NP + 8u * static_cast<uint32_t>(screen));
}

[[gnu::always_inline]] inline uint32_t np_code_or_zero(uint8_t *np) { return np ? u3h_np_code(np) : 0; }

// 4.7 step 9: the animation bit stream of the message; stores and returns nothing but the message fields
U3X_FN void pack_animation(uintptr_t base, uint8_t *msg, uint8_t *pl) {
    uint8_t **tabs = rd<uint8_t **>(pl, 0x1c20);
    for (uint32_t i = 0; i < STREAM_BYTES; i++) {
        uint8_t *b = msg + 0x18 + i;
        __asm__("" : "+r"(b));   // keeps this a plain loop (no library call)
        *b = 0;
    }
    Stream stream;
    stream.buffer = msg + 0x18;
    stream.position = 0;
    stream.capacity = STREAM_BYTES * 8;
    uint32_t *error = u3h_repl_error();
    *error = 0;
    uint32_t bits = u3h_anim_pack_full(base, msg, reinterpret_cast<uint8_t *>(&stream), pl, tabs, 1, 3);
    if (*error != 0) {
        ec(71) += 1;
        bits = 0;
    }
    put<uint32_t>(msg, 0x144, bits);
    u3h_repl_loop(base, pl, tabs, msg, bits);
}

// 4.7 steps 12 and 13: true when the player may mark a target (a camera of his screen exists)
U3X_FN bool may_target(uintptr_t base, void *s, uint8_t *pl, int32_t screen) {
    bool on = false;
    const char *name = reinterpret_cast<const char *>(method(ptr_at(pl, 0x520), 0x78));
    if (name && name[0] != 0) {
        on = true;
    } else {
        uint8_t *c = u3x_char_np(base, pl);
        uint32_t v = u3x_float_call(base + FN_BOOSTER_VALUE, base + IMG_BOOSTER_ARG, EFF_ON_SCREEN,
                                    reinterpret_cast<uint64_t>(c), 0);
        on = f_less(F_BELOW_ONE, v);
    }
    int32_t type = u3h_netgame_type(base, s);
    if (static_cast<uint32_t>(type) == GAME_TYPE_NO_TARGET || !on) return false;
    uint64_t list = base + IMG_CAMERAS_A;
    if (game_call<void *, int32_t, uint64_t *>(base + FN_SCREEN_CAMERA, screen, &list)) return true;
    list = base + IMG_CAMERAS_B;
    return game_call<void *, int32_t, uint64_t *>(base + FN_SCREEN_CAMERA, screen, &list) != nullptr;
}

// 4.7 step 14: who the player aims at
U3X_FN void mark_target(uintptr_t base, uint8_t *msg, uint8_t *pl, uint8_t *np) {
    AimBlock block;
    for (uint32_t i = 0; i < 8; i++) {
        uint64_t *w = &block.w[i];
        __asm__("" : "+r"(w));
        *w = 0;
    }
    uint8_t *aimed = reinterpret_cast<uint8_t *>(method(ptr_at(pl, 0x528), 0xc8, &block, static_cast<uint32_t>(0)));
    if (!aimed) return;
    uint8_t *target = u3x_char_np(base, aimed);
    if (!target) return;
    int32_t now = rd<int32_t>(at<uint8_t *>(base + IMG_CLOCK_OBJECT), 0xc);
    put<int32_t>(target, 0x9a8, static_cast<int32_t>(static_cast<uint32_t>(now) + TARGET_TICKS));
    put<uint32_t>(target, 0x9ac, np_code_or_zero(np));
    put<uint32_t>(msg, 0x210, u3h_np_code(target));
    ec(72) += 1;
}

// the 1.19 wire number of the weapon in a slot object
[[gnu::always_inline]] inline uint32_t number_of(uintptr_t base, uint8_t *weapon) {
    return u3x_weapon_item(base, rd<uint32_t>(weapon, 0x6ec));
}

// 4.8 steps 1 and 2: a main weapon slot: its number, two ammunition bytes and its state
[[gnu::always_inline]] inline void main_slot(uintptr_t base, uint8_t *msg, uint8_t *weapon, uintptr_t number_off,
                                             uintptr_t ammo_a_off, uintptr_t ammo_b_off, uintptr_t state_off) {
    if (!weapon) return;
    put<uint32_t>(msg, number_off, number_of(base, weapon));
    put<uint8_t>(msg, ammo_a_off, rd<uint8_t>(weapon, 0x6f0));
    put<uint8_t>(msg, ammo_b_off, rd<uint8_t>(weapon, 0x6f2));
    put<uint8_t>(msg, state_off, static_cast<uint8_t>(method(weapon, 0x438)));
}

}  // namespace g48c

// 4.10: 1 when the character's current effect record has id sid
extern "C" U3X_FN int u3x_has_eff(uint8_t *c, uint32_t sid) {
    const uint8_t *const *slot = rd<const uint8_t *const *>(c, 0x80);
    if (!slot) return 0;
    const uint8_t *entry = *slot;
    if (!entry) return 0;
    const uint8_t *record = rd<const uint8_t *>(entry, 0x10);
    return rd<uint32_t>(record, 0x18) == sid ? 1 : 0;
}

// 4.11: the net player record behind a character
extern "C" U3X_FN uint8_t *u3x_char_np(uintptr_t base, uint8_t *c) {
    if (!c || at<uint8_t>(base + g48c::IMG_NET_ACTIVE) == 0) return nullptr;
    const uint32_t cls = rd<uint32_t>(c, 0x30);
    if (cls == g48c::CLASS_LOCAL_PLAYER) return g48c::local_np(base, rd<int32_t>(c, 0x1c68));
    if (cls == g48c::CLASS_NET_CHARACTER) {
        const uint32_t id = rd<uint32_t>(c, 0x3f8);
        return id ? u3h_np_by_id(base, id) : nullptr;
    }
    return nullptr;
}

// 2.3: the game's weapon mode in 1.19's numbering
extern "C" U3X_FN uint32_t u3x_weapon_mode_119(uint32_t mode) {
    if (mode == 0) return 0;
    if (mode == 1) return 1;
    if (mode == 5) return 3;
    return 4;
}

// 3.5: the weapon id of fixed number index
extern "C" U3X_FN uint32_t u3x_weapon_fixed_id(uintptr_t base, uint32_t index) {
    if (index < g48c::IMAGE_WEAPON_COUNT) return at<uint32_t>(base + g48c::IMG_FIXED_WEAPONS + 4u * index);
    if (index == g48c::IMAGE_WEAPON_COUNT) return g48c::WEAPON_NUMBER_19;
    return 0;
}

// 4.9: the wire number of a weapon id: fixed numbers first, then the script's extra list
extern "C" U3X_FN uint32_t u3x_weapon_item(uintptr_t base, uint32_t id) {
    for (uint32_t n = 0; n < g48c::FIXED_WEAPON_COUNT; n++)
        if (u3x_weapon_fixed_id(base, n) == id) return n;
    const uint8_t *extra = game_call<const uint8_t *, uint32_t>(base + g48c::FN_SCRIPT_SYMBOL, g48c::SYM_EXTRA_WEAPONS);
    if (!extra) return 0;
    const int32_t count = rd<int32_t>(extra, 0);
    const uint32_t *ids = rd<const uint32_t *>(extra, 8);
    if (!ids) return 0;
    for (int32_t k = 0; k < count; k++)
        if (ids[k] == id) return g48c::FIXED_WEAPON_COUNT + static_cast<uint32_t>(k);
    return 0;
}

// 4.8: weapons, carried object, weapon mode, animation-control float and cover state
extern "C" U3X_FN void u3x_weapons_fill(uintptr_t base, uint8_t *msg, uint8_t *pl) {
    using namespace g48c;
    uint8_t *wc = ptr_at(pl, 0x520);
    auto slot = [wc](uint64_t n) { return reinterpret_cast<uint8_t *>(method(wc, 0x70, n)); };

    // the ammunition bytes are crossed between slots 0 and 1, the state bytes are not (spec 6.5)
    main_slot(base, msg, slot(0), 0x1f0, 0x231, 0x233, 0x234);
    main_slot(base, msg, slot(1), 0x1f4, 0x230, 0x232, 0x235);

    if (uint8_t *w = slot(2)) {
        if (static_cast<int32_t>(method(w, 0x398)) > 0) put<uint32_t>(msg, 0x1f8, number_of(base, w));
    }
    if (uint8_t *w = slot(5)) put<uint32_t>(msg, 0x1fc, number_of(base, w));

    // carried object: its spawner's name id, else its own
    uint8_t *carried = reinterpret_cast<uint8_t *>(method(ptr_at(pl, 0x688), 0x120, static_cast<uint64_t>(0)));
    if (carried && rd<uint32_t>(carried, 0x30) == CLASS_CARRIED) {
        const uint8_t *spawner = reinterpret_cast<const uint8_t *>(method(carried, 0xe8));
        put<uint32_t>(msg, 0x208, spawner ? rd<uint32_t>(spawner, 0x28) : rd<uint32_t>(carried, 0x440));
    }

    put<uint32_t>(msg, 0x200, u3x_weapon_mode_119(static_cast<uint32_t>(method(wc, 0x60))));
    put<uint32_t>(msg, 0x204, static_cast<uint32_t>(method(wc, 0x198)));

    // animation-control float, through two pointers
    const uint8_t *p = ptr_at(pl, 0x138);
    const uint8_t *p1 = p ? ptr_at(p, 0x40) : nullptr;
    const uint8_t *p2 = p1 ? ptr_at(p1, 0x10) : nullptr;
    if (p2) {
        put<uint32_t>(msg, 0x15c, rd<uint32_t>(p2, 0x120));
    } else {
        put<uint32_t>(msg, 0x15c, 0);
        ec(69) += 1;
    }

    // cover state 0 / 1 / 2
    put<uint32_t>(msg, 0x20c, 0);
    uint8_t *cover = ptr_at(pl, 0x5f8);
    if (static_cast<uint8_t>(method(cover, 0x68)) != 0)
        put<uint32_t>(msg, 0x20c, static_cast<uint8_t>(method(cover, 0x70)) != 0 ? 2u : 1u);
}

// 4.7: fills the message for player character pl; result = the bit count stored in the message
extern "C" U3X_FN uint32_t u3x_player_fill(uintptr_t base, void *s, uint8_t *msg, uint8_t *pl) {
    using namespace g48c;

    // 1. flags; each bit is OR-ed into the word in memory as soon as it is decided (spec 8.3)
    uint32_t *const flags = reinterpret_cast<uint32_t *>(msg + 0x148);
    *flags |= FLAG_PLAYER;
    const uint8_t *state = ptr_at(pl, 0x198);
    if (at<uint64_t>(base + IMG_SESSION) != 0 && state && rd<int32_t>(state, 0x50) == 3) {
        *flags |= FLAG_STATE_3;
        if (static_cast<uint8_t>(method(ptr_at(pl, 0x598), 0xa8)) == 0) *flags |= FLAG_TEST_ZERO;
    }
    // 2. move flags, unless one of the two "no move" effects runs
    if (!u3x_has_eff(pl, EFF_NO_MOVE_1) && !u3x_has_eff(pl, EFF_NO_MOVE_2)) {
        const int32_t mode = static_cast<int32_t>(method(ptr_at(pl, 0x550), 0x80));
        if (mode == 1) *flags |= FLAG_MOVE_1;
        if (mode == 2) *flags |= FLAG_MOVE_2;   // set although the game does not send it (spec 6.1)
    }
    // 3. held / frozen
    if (rd<uint8_t>(pl, 0x1338) != 0 || u3x_has_eff(pl, EFF_HELD_1) || u3x_has_eff(pl, EFF_HELD_2) ||
        u3x_has_eff(pl, EFF_HELD_3))
        *flags |= FLAG_HELD;

    // 4. the player's own net player record
    const int32_t screen = rd<int32_t>(pl, 0x1c68);
    uint8_t *np = local_np(base, screen);

    // 5. ids, time, look direction, stance
    put<uint32_t>(msg, 0x14c, np_code_or_zero(np));
    put<uint32_t>(msg, 0x150, rd<uint32_t>(pl, 0x3c0));
    put<uint32_t>(msg, 0x14, f_mul(f_from_int(u3h_net_now(base)), F_TICK));
    put<uint32_t>(msg, 0x154, rd<uint32_t>(pl, 0x3a8));
    for (uint32_t i = 0; i < 4; i++) put<uint32_t>(msg, 0x160 + 4 * i, rd<uint32_t>(pl, 0x3b0 + 4 * i));
    put<uint32_t>(msg, 0x158, static_cast<uint32_t>(method(ptr_at(pl, 0x548), 0x98)));

    // 6. bound frame
    if (const uint8_t *pose = ptr_at(pl, 0x538)) {
        for (uint32_t i = 0; i < 0x60; i += 4) put<uint32_t>(msg, 0x170 + i, rd<uint32_t>(pose, 0x140 + i));
    }

    // 7. net player byte, weapon pose id (first time)
    put<uint8_t>(msg, 0x1de, np ? rd<uint8_t>(np, 0x430) : static_cast<uint8_t>(0));
    put<uint16_t>(msg, 0x1dc, static_cast<uint16_t>(method(ptr_at(pl, 0x310), 0x70)));

    // 8. lock target
    if (const uint8_t *lock = reinterpret_cast<const uint8_t *>(method(ptr_at(pl, 0x668), 0x60))) {
        put<uint8_t>(msg, 0x1e0, 1);
        put<uint32_t>(msg, 0x1e4, rd<uint32_t>(lock, 0x98));
        put<uint32_t>(msg, 0x1e8, rd<uint32_t>(lock, 0x9ac));
        put<uint32_t>(msg, 0x1ec, rd<uint32_t>(lock, 0x9b0));
    }

    // 9. animation bit stream
    pack_animation(base, msg, pl);

    // 10. more values; the pose id is asked a second time (spec 6.3)
    put<uint32_t>(msg, 0x1d0, rd<uint32_t>(pl, 0x39c));
    put<uint32_t>(msg, 0x1d4, np ? rd<uint32_t>(np, 0x398) : 0u);
    put<uint16_t>(msg, 0x1dc, static_cast<uint16_t>(method(ptr_at(pl, 0x310), 0x70)));
    put<uint32_t>(msg, 0x1d8, np ? rd<uint32_t>(np, 0x5a4) : 0u);

    // 11. weapons
    u3h_weapons_fill(base, msg, pl);

    // 12 to 14. who the player aims at
    if (may_target(base, s, pl, screen)) mark_target(base, msg, pl, np);

    // 15. the bit count as the message holds it now
    return rd<uint32_t>(msg, 0x144);
}
