// G53 part A: the 1.19 player-info fields (message 0x3a) and the hat stat modifiers.
// Written from spec G53_player_info_hats.md, sections 4.1 to 4.7.

extern "C" uint8_t *u3h_np119(void);
extern "C" void u3h_taunt_want(uintptr_t base, uint32_t id);
extern "C" uint8_t *u3x_sym_table(void *s, uint32_t id);
extern "C" uint8_t *u3x_netgame_cur(uintptr_t base, void *s);
extern "C" int32_t u3x_netgame_type(uintptr_t base, void *s);
extern "C" uint32_t u3x_net_player_id(const uint8_t *np);

extern "C" void u3x_pinfo_send(uintptr_t base, uint8_t *msg);
extern "C" int u3x_pinfo_serial(uintptr_t base, uint64_t *regs, int write);
extern "C" void u3x_pinfo_process(uintptr_t base, const uint8_t *msg);
extern "C" void u3x_np_taunt_want(uintptr_t base, uint8_t *np);
extern "C" int u3x_booster_skip(uintptr_t base, void *s, uint64_t *regs);
extern "C" void u3x_hat_mods(uintptr_t base, void *s, uint8_t *np);
extern "C" int u3x_add_hat_mods(uint8_t *np, const uint8_t *rows, int32_t row_count, uint32_t item, uint32_t slot,
                                uint32_t *added);

namespace g53a {
namespace {

// image offsets
constexpr uintptr_t NGM = 0x17cb060;                // the net game manager
constexpr uintptr_t PLAYERS = NGM + 0x390;          // ten net player records
constexpr uintptr_t PLAYER_SIZE = 0x9c0;
constexpr int PLAYER_COUNT = 10;
constexpr uintptr_t PROFILES = 0x17dd610;           // pointer to the profile objects
constexpr uintptr_t PROFILE_SIZE = 0x7b00;
constexpr uintptr_t BITS_WRITE = 0x1272b50;
constexpr uintptr_t BITS_WRITE_BOOL = 0x1272c90;
constexpr uintptr_t BITS_READ = 0x1272c00;
constexpr uintptr_t BOOSTERS_ON = 0x73fbc0;
constexpr uintptr_t PLAYER_LOOK = 0x784b20;
constexpr uintptr_t LOOK_ITEM = 0x9952a0;

// switch bits of ec[0]
constexpr uint32_t SW_TAUNT_OFF = 8, SW_PINFO_OFF = 32, SW_HAT_MODS_OFF = 64;

constexpr uint32_t SYM_HAT_MODS = 0xb8a3581b;
constexpr uint32_t BOOSTER_LEFT_OUT = 0xdff8181f;
constexpr int32_t GAME_TYPE_LEFT_OUT = 0x13;
constexpr uint32_t MOD_LIST_MAX = 16;

// the first 12 code bytes a game function must have before it is called
U3X_RO const uint8_t code_player_look[12] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xfb, 0x40, 0x0f};
U3X_RO const uint8_t code_look_item[12] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54};
U3X_RO const uint8_t code_bits_write[12] = {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x44, 0x8b, 0x5f, 0x08, 0x45, 0x89};
U3X_RO const uint8_t code_bits_read[12] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x8b, 0x57, 0x08, 0x89, 0xd3};

// a word that is always 0 and never written: the booster loop's replayed load reads it
U3X_RO const uint32_t zero_word[1] = {0};

[[gnu::always_inline]] inline uint32_t *ec() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0x4450); }

template <typename T> [[gnu::always_inline]] inline void wr(void *p, uintptr_t off, T v) {
    *reinterpret_cast<T *>(static_cast<uint8_t *>(p) + off) = v;
}

U3X_FN bool code_is(uintptr_t base, uintptr_t off, const uint8_t *want) {
    const uint8_t *code = reinterpret_cast<const uint8_t *>(base + off);
    for (int i = 0; i < 12; i++)
        if (code[i] != want[i]) return false;
    return true;
}

// slot 0..9 of a net player record, -1 when the pointer is not one of the ten records
U3X_FN int player_slot(uintptr_t base, const uint8_t *np) {
    const int64_t d = static_cast<int64_t>(reinterpret_cast<uintptr_t>(np) - (base + PLAYERS));
    if (d < 0 || d % static_cast<int64_t>(PLAYER_SIZE) != 0) return -1;
    const int64_t q = d / static_cast<int64_t>(PLAYER_SIZE);
    return q < PLAYER_COUNT ? static_cast<int>(q) : -1;
}

// per-player info entry (C part): +0 taunt, +4 rank, +5 hat stats off, +6 profile flag, +7 processed
[[gnu::always_inline]] inline uint8_t *info_entry(uint8_t *array, int slot) { return array + 8 * slot; }

}  // namespace
}  // namespace g53a

// 4.1: fill the 1.19 fields of a player-info message that is about to be sent
U3X_FN void u3x_pinfo_send(uintptr_t base, uint8_t *msg) {
    using namespace g53a;
    uint32_t *e = ec();
    e[37]++;
    if (e[0] & SW_PINFO_OFF) return;
    const uint32_t user = rd<uint32_t>(msg, 0x14) >> 12;
    const uint8_t *profiles = at<uint8_t *>(base + PROFILES);
    if (!profiles || user > 3) return;
    const uint8_t *profile = profiles + user * PROFILE_SIZE;
    if (rd<uint32_t>(profile, 0x2164)) msg[0x25] |= 8;
    msg[0x69] = 0;   // no tournaments
    msg[0x6a] = rd<uint32_t>(profile, 0x215c) ? 1 : 0;
    wr<uint32_t>(msg, 0xc0, rd<uint32_t>(profile, 0x784));
}

// 4.2: the game's own body of the message, then the three 1.19 fields
U3X_FN int u3x_pinfo_serial(uintptr_t base, uint64_t *regs, int write) {
    using namespace g53a;
    uint32_t *e = ec();
    uint8_t *msg = reinterpret_cast<uint8_t *>(regs[4]);
    void *stream = reinterpret_cast<void *>(regs[5]);
    const uint32_t body = write ? e[41] : e[40];
    if ((e[0] & SW_PINFO_OFF) || !body) return 0;
    if (!code_is(base, BITS_WRITE, code_bits_write) || !code_is(base, BITS_READ, code_bits_read)) return 0;

    game_call<void>(base + body, msg, stream);
    if (write) {
        game_call<void>(base + BITS_WRITE, stream, static_cast<uint32_t>(msg[0x69]), uint32_t{8});
        game_call<void>(base + BITS_WRITE_BOOL, stream, static_cast<uint32_t>(msg[0x6a]));
        game_call<void>(base + BITS_WRITE, stream, rd<uint32_t>(msg, 0xc0), uint32_t{32});
        e[39]++;
    } else {
        msg[0x69] = static_cast<uint8_t>(game_call<uint32_t>(base + BITS_READ, stream, uint32_t{8}));
        msg[0x6a] = static_cast<uint8_t>(game_call<uint32_t>(base + BITS_READ, stream, uint32_t{1}));
        wr<uint32_t>(msg, 0xc0, game_call<uint32_t>(base + BITS_READ, stream, uint32_t{32}));
        e[38]++;
    }
    regs[8] = 0;
    return 1;
}

// 4.3: keep the received 1.19 fields for the player the message is about
U3X_FN void u3x_pinfo_process(uintptr_t base, const uint8_t *msg) {
    using namespace g53a;
    uint32_t *e = ec();
    if (e[0] & SW_PINFO_OFF) return;
    const uint32_t id = rd<uint32_t>(msg, 0x14);
    uint8_t *infos = u3h_np119();
    uint8_t *record = reinterpret_cast<uint8_t *>(base + PLAYERS);
    for (int i = 0; i < PLAYER_COUNT; i++, record += PLAYER_SIZE) {
        if (u3x_net_player_id(record) != id) continue;
        const uint8_t flags = msg[0x25];
        const uint32_t profile_flag = (flags >> 3) & 1;
        const uint8_t hats_off = msg[0x6a];
        const uint32_t taunt = rd<uint32_t>(msg, 0xc0);
        record[0x208] = flags & 7;
        uint8_t *info = info_entry(infos, i);
        info[6] = static_cast<uint8_t>(profile_flag);
        info[4] = msg[0x69];
        info[5] = hats_off;
        wr<uint32_t>(info, 0, taunt);
        info[7] = 1;
        e[42]++;
        e[43] = static_cast<uint32_t>(i) | (static_cast<uint32_t>(hats_off) << 8) | (profile_flag << 9);
        e[44] = taunt;
        return;
    }
}

// 4.4: ask for the taunt package of a net player
U3X_FN void u3x_np_taunt_want(uintptr_t base, uint8_t *np) {
    using namespace g53a;
    uint32_t *e = ec();
    e[45]++;
    if ((e[0] & (SW_TAUNT_OFF | SW_PINFO_OFF)) || !np) return;
    const int slot = player_slot(base, np);
    if (slot < 0) return;
    u3h_taunt_want(base, rd<uint32_t>(info_entry(u3h_np119(), slot), 0));
}

// 4.5: make the game read a level count of 0 for the one booster 1.19 leaves out in one game type
U3X_FN int u3x_booster_skip(uintptr_t base, void *s, uint64_t *regs) {
    using namespace g53a;
    uint32_t *e = ec();
    if (e[0] & SW_HAT_MODS_OFF) return 0;
    const uint8_t *booster = reinterpret_cast<const uint8_t *>(regs[8]);
    if (booster && rd<uint32_t>(booster, 4) == BOOSTER_LEFT_OUT && u3x_netgame_type(base, s) == GAME_TYPE_LEFT_OUT) {
        regs[6] = reinterpret_cast<uintptr_t>(zero_word) - 4;   // the game loads from rdx + rcx + 4
        regs[7] = 0;
        e[46]++;
    }
    return 0;
}

// 4.6: append the modifiers of the hat parts a net player wears to its rebuilt list
U3X_FN void u3x_hat_mods(uintptr_t base, void *s, uint8_t *np) {
    using namespace g53a;
    uint32_t *e = ec();
    if (e[0] & SW_HAT_MODS_OFF) return;
    if (!np) return;
    const int slot = player_slot(base, np);
    if (!at<uint8_t>(base + 0x17d5c58 + 9) || !at<uint32_t>(base + NGM + 0x6808)) return;
    const uint8_t *netgame = u3x_netgame_cur(base, s);
    if (netgame && rd<uint32_t>(netgame, 0x6c)) return;
    if (!code_is(base, PLAYER_LOOK, code_player_look) || !code_is(base, LOOK_ITEM, code_look_item)) return;
    if (!game_call<uint8_t>(base + BOOSTERS_ON, base + NGM)) return;
    const uint8_t *table = u3x_sym_table(s, SYM_HAT_MODS);
    if (!table) return;
    if (slot >= 0 && info_entry(u3h_np119(), slot)[5]) return;   // the player switched hat stats off
    e[48]++;
    const uint8_t *look = game_call<const uint8_t *>(base + PLAYER_LOOK, np, uint32_t{0});
    if (!look) return;
    const int32_t row_count = rd<int32_t>(table, 0);
    const uint8_t *rows = rd<const uint8_t *>(table, 8);
    if (!rows) return;
    for (uint32_t k = 2; k <= 6; k++) {
        const uint8_t *item = game_call<const uint8_t *>(base + LOOK_ITEM, uint64_t{0}, static_cast<uint32_t>(look[0]),
                                                         static_cast<uint32_t>(look[1]), k - 2,
                                                         static_cast<uint32_t>(look[k]));
        if (!item) continue;
        if (u3x_add_hat_mods(np, rows, row_count, rd<uint32_t>(item, 0), k, e + 47) < 0) {
            e[49]++;   // list full: the remaining parts are left out
            return;
        }
    }
}

// 4.7: append the pairs of every table row of one item; -1 when the list is full
U3X_FN int u3x_add_hat_mods(uint8_t *np, const uint8_t *rows, int32_t row_count, uint32_t item, uint32_t slot,
                            uint32_t *added) {
    using namespace g53a;
    for (int32_t r = 0; r < row_count; r++) {
        const uint8_t *row = rows + 16 * static_cast<intptr_t>(r);
        if (rd<uint32_t>(row, 0) != item) continue;
        const int32_t pair_count = rd<int32_t>(row, 4);
        const uint8_t *pairs = rd<const uint8_t *>(row, 8);
        if (!pairs) continue;
        for (int32_t j = 0; j < pair_count; j++) {
            const uint8_t *pair = pairs + 8 * static_cast<intptr_t>(j);
            const uint32_t count = rd<uint32_t>(np, 0x84c);
            if (count >= MOD_LIST_MAX) return -1;
            uint8_t *entry = np + 0x850 + 16 * static_cast<uintptr_t>(count);
            wr<uint32_t>(entry, 0, rd<uint32_t>(pair, 0));
            wr<uint32_t>(entry, 4, rd<uint32_t>(pair, 4));
            wr<uint32_t>(entry, 8, slot);
            entry[0xc] = 4;   // a hat entry
            entry[0xd] = 0;
            wr<uint32_t>(np, 0x84c, count + 1);
            ++*added;
        }
    }
    return 0;
}
