// G48 part A: the per-player, per-screen "spotted" flags (Spotter booster), by the spec G48_player_state.md
// sections 4.1 to 4.3.

extern "C" uint8_t *u3h_ext(void);
extern "C" uint32_t u3x_float_call(uintptr_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
extern "C" uint8_t *u3h_spot_flags(void);
extern "C" uint8_t *u3h_netgame(uintptr_t base, void *s);

extern "C" void u3x_spotter(uintptr_t base, void *s, uint8_t *spotter);
extern "C" void u3x_spot_clear(uintptr_t base, uint8_t *np);
extern "C" uint8_t u3x_spot_flag(uintptr_t base, uint8_t *np);
extern "C" void u3x_spot_read(uintptr_t base, uint8_t *np);

namespace g48a {

constexpr uintptr_t MANAGER = 0x17cb060;          // the net game manager
constexpr uintptr_t LOCAL_PLAYERS = 0x370;        // manager: local net player record per screen
constexpr uintptr_t RECORDS = 0x390;              // manager: the ten net player records
constexpr uintptr_t RECORD_SIZE = 0x9c0;
constexpr int RECORD_COUNT = 10;
constexpr uintptr_t NET_ACTIVE = 0x17d5c61;
constexpr uintptr_t HUD_DRAWN = 0x1806480;
constexpr uintptr_t BOOSTER_VALUE_FN = 0x689d20;
constexpr uintptr_t BOOSTER_VALUE_ARG = 0x17c3b78;
constexpr uintptr_t HAS_BOOSTER_FN = 0x793280;
constexpr uint32_t SPOTTER_RADIUS = 0x01ca807au;
constexpr uint32_t COUNTER_BOOSTER = 0x41479fa0u;
constexpr int32_t TYPE_NO_TEAMS = 0xe;            // game type in which team mates are spotted too

// the first 12 code bytes the two game functions must start with
U3X_RO const uint8_t value_fn_code[12] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54};
U3X_RO const uint8_t booster_fn_code[12] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x49};

[[gnu::always_inline]] inline uint32_t *ec() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0x4450); }

[[gnu::always_inline]] inline uint8_t *record(uintptr_t base, int i) {
    return reinterpret_cast<uint8_t *>(base + MANAGER + RECORDS + RECORD_SIZE * static_cast<uintptr_t>(i));
}

[[gnu::always_inline]] inline bool code_is(uintptr_t address, const uint8_t *expected) {
    for (int i = 0; i < 12; i++)
        if (at<uint8_t>(address + static_cast<uintptr_t>(i)) != expected[i]) return false;
    return true;
}

// the slot number of a net player record, or -1 when the address is not one of the ten records
[[gnu::always_inline]] inline int slot_of(uintptr_t base, const uint8_t *np) {
    const uintptr_t first = base + MANAGER + RECORDS;
    const uintptr_t a = reinterpret_cast<uintptr_t>(np);
    if (a < first) return -1;
    const uintptr_t d = a - first;
    if (d % RECORD_SIZE != 0 || d / RECORD_SIZE >= static_cast<uintptr_t>(RECORD_COUNT)) return -1;
    return static_cast<int>(d / RECORD_SIZE);
}

[[gnu::always_inline]] inline bool has_counter(uintptr_t base, uint8_t *np) {
    return (game_call<uint64_t, uint8_t *, uint32_t, uint32_t>(base + HAS_BOOSTER_FN, np, COUNTER_BOOSTER, 0u) & 0xff) != 0;
}

// squared distance spotter - target
[[gnu::always_inline]] inline uint32_t distance2(const uint8_t *spotter, const uint8_t *t) {
    const uint32_t dx = f_sub(rd<uint32_t>(spotter, 0), rd<uint32_t>(t, 0));
    const uint32_t dy = f_sub(rd<uint32_t>(spotter, 4), rd<uint32_t>(t, 4));
    const uint32_t dz = f_sub(rd<uint32_t>(spotter, 8), rd<uint32_t>(t, 8));
    return f_add(f_mul(dz, dz), f_add(f_mul(dx, dx), f_mul(dy, dy)));
}

}  // namespace g48a

// 4.1: sets the spotted flags of the living enemies inside the Spotter radius of `spotter`
U3X_FN void u3x_spotter(uintptr_t base, void *s, uint8_t *spotter) {
    using namespace g48a;
    ec()[50] += 1;
    if (!code_is(base + BOOSTER_VALUE_FN, value_fn_code) || !code_is(base + HAS_BOOSTER_FN, booster_fn_code)) return;

    const uint32_t radius = u3x_float_call(base + BOOSTER_VALUE_FN, base + BOOSTER_VALUE_ARG, SPOTTER_RADIUS,
                                           reinterpret_cast<uint64_t>(spotter), 0);
    if (!f_less(0u, radius)) return;
    const uint32_t radius2 = f_mul(radius, radius);

    uint8_t *const second = u3h_spot_flags();
    const bool both = (ec()[0] & 0x80u) == 0;      // second screen served too; read after the call (spec 8.2)

    for (int i = 0; i < RECORD_COUNT; i++) {
        uint8_t *const t = record(base, i);
        if (at<uint8_t>(base + NET_ACTIVE) == 0) continue;
        if (t == spotter) continue;
        const uint8_t *const game = u3h_netgame(base, s);
        const bool no_teams = game && rd<int32_t>(game, 0xc) == TYPE_NO_TEAMS;
        if (!no_teams && rd<int32_t>(t, 0x21c) == rd<int32_t>(spotter, 0x21c)) continue;
        if (rd<int32_t>(t, 0x39c) > rd<int32_t>(t, 0x3a0)) continue;   // dead

        for (int j = 0; j < (both ? 2 : 1); j++) {
            uint8_t *const flag = j == 0 ? t + 0x9b1 : second + i;
            if (*flag != 0) continue;
            if (!f_less(distance2(spotter, t), radius2)) continue;
            if (has_counter(base, t)) continue;
            if (both) {
                uint8_t *const local = at<uint8_t *>(base + MANAGER + LOCAL_PLAYERS + 8u * static_cast<uintptr_t>(j));
                if (local && has_counter(base, local)) continue;
            }
            *flag = 1;
            ec()[51 + j] += 1;
        }
    }
}

// 4.2: clears second-screen flags: the one of `np`, or (null) of every record that is not leaving
U3X_FN void u3x_spot_clear(uintptr_t base, uint8_t *np) {
    using namespace g48a;
    uint8_t *const second = u3h_spot_flags();
    if (np) {
        const int i = slot_of(base, np);
        if (i >= 0) second[i] = 0;
        return;
    }
    for (int i = 0; i < RECORD_COUNT; i++)
        if (rd<uint8_t>(record(base, i), 0x5e4) == 0) second[i] = 0;
}

// 4.3: the spotted flag of `np` for the screen being drawn
U3X_FN uint8_t u3x_spot_flag(uintptr_t base, uint8_t *np) {
    using namespace g48a;
    const uint8_t *const hud = at<const uint8_t *>(base + HUD_DRAWN);
    if ((ec()[0] & 0x80u) == 0 && hud && rd<int32_t>(hud, 0x1d0) == 1) {
        ec()[53] += 1;
        const int i = slot_of(base, np);
        if (i >= 0) return u3h_spot_flags()[i];
    }
    return rd<uint8_t>(np, 0x9b1);
}

// 4.3: stores that flag for the hook stub
U3X_FN void u3x_spot_read(uintptr_t base, uint8_t *np) {
    using namespace g48a;
    const uint32_t v = np ? u3x_spot_flag(base, np) : 0u;
    ec()[54] = v;
}
