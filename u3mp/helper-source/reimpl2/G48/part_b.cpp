// G48 part B: distance, the player state message (build, fill, send, release) and the send loop, by the spec
// G48_player_state.md sections 4.4 to 4.6.

extern "C" uint8_t *u3h_ext(void);
extern "C" uint32_t u3h_player_fill(uintptr_t base, void *s, uint8_t *msg, uint8_t *player);

extern "C" uint32_t u3x_distance(const uint8_t *a, const uint8_t *b);
extern "C" void u3x_send_player_state(uintptr_t base, void *s, uint8_t *player, uint32_t mask);
extern "C" void u3x_replication(uintptr_t base, void *s, uint8_t *np, uint8_t *player);

namespace g48b {

constexpr uintptr_t MANAGER = 0x17cb060;          // the net game manager
constexpr uintptr_t RECORDS = 0x390;              // manager: the ten net player records
constexpr uintptr_t RECORD_SIZE = 0x9c0;
constexpr int RECORD_COUNT = 10;
constexpr uintptr_t NET_CLOCK = 0x17b4ca4;        // ticks of 1/300 s
constexpr uintptr_t NET_ACTIVE = 0x17d5c61;
constexpr uintptr_t SOURCE_NAME = 0x13ac250;      // text handed to the allocator context
constexpr uintptr_t MESSAGE_CLASS = 0x16d5b90;
constexpr uintptr_t ALLOC_ENTER_FN = 0xda1590;
constexpr uintptr_t ALLOC_FN = 0xd9e990;
constexpr uintptr_t ALLOC_LEAVE_FN = 0xda18b0;
constexpr uintptr_t SEND_FN = 0x7336e0;
constexpr uintptr_t OBJECT_SEND_FN = 0x6ae2a0;

constexpr uint32_t F_TICK = 0x3b5a740eu;          // 1/300
constexpr uint32_t F_TINY = 0x00800000u;          // smallest normal float
constexpr uint32_t F_NEAR = 0x41700000u;          // 15.0: near distance and near rate
constexpr uint32_t F_BAND = 0x3d888889u;          // 1/15
constexpr uint32_t F_FAR_RATE = 0x41000000u;      // 8.0
constexpr uint32_t F_UNSET = 0x7fccccccu;         // the game's "unset" mark

[[gnu::always_inline]] inline uint32_t *ec() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0x4450); }

template <typename T> [[gnu::always_inline]] inline void put(uint8_t *p, uintptr_t off, T v) {
    *reinterpret_cast<T *>(p + off) = v;
}

[[gnu::always_inline]] inline uint8_t *record(uintptr_t base, int i) {
    return reinterpret_cast<uint8_t *>(base + MANAGER + RECORDS + RECORD_SIZE * static_cast<uintptr_t>(i));
}

// the function at byte offset `method` of the object's virtual table
[[gnu::always_inline]] inline uintptr_t method_of(const uint8_t *object, uintptr_t method) {
    return rd<uintptr_t>(rd<const uint8_t *>(object, 0), method);
}

// the net player id of a record
[[gnu::always_inline]] inline uint32_t player_id(const uint8_t *np) {
    uint32_t id = rd<uint32_t>(np, 0x1e4) & 0xfffu;
    if (rd<uint8_t>(np, 0x1ec) != 0 && id != 0) id += 0x800u;
    return id + static_cast<uint32_t>(rd<int32_t>(np, 0x1e8)) * 0x1000u;
}

[[gnu::always_inline]] inline uint32_t distance(const uint8_t *a, const uint8_t *b) {
    const uint32_t dx = f_sub(rd<uint32_t>(a, 0), rd<uint32_t>(b, 0));
    const uint32_t dy = f_sub(rd<uint32_t>(a, 4), rd<uint32_t>(b, 4));
    const uint32_t dz = f_sub(rd<uint32_t>(a, 8), rd<uint32_t>(b, 8));
    const uint32_t d2 = f_add(f_add(f_mul(dx, dx), f_mul(dy, dy)), f_mul(dz, dz));
    return f_less(F_TINY, d2) ? f_root(d2) : 0u;
}

// the values a fresh message starts with (spec 3.7); bytes not named there keep what the allocator returned
[[gnu::always_inline]] inline void message_start(uintptr_t base, uint8_t *m) {
    put<uint64_t>(m, 0, base + MESSAGE_CLASS);
    put<uint8_t>(m, 8, 0x10);      // message type
    put<uint8_t>(m, 9, 2);
    put<uint8_t>(m, 0xa, 1);       // reference count
    put<uint8_t>(m, 0xb, 1);
    put<uint8_t>(m, 0xc, 1);
    put<uint8_t>(m, 0xd, 0);
    put<uint32_t>(m, 0x148, 0);
    put<uint32_t>(m, 0x154, 0);
    put<uint32_t>(m, 0x158, 0);
    put<uint32_t>(m, 0x15c, 0);
    // the bound frame: unset marks, then zeros with two 1.0
    put<uint32_t>(m, 0x170, F_UNSET);
    put<uint32_t>(m, 0x174, F_UNSET);
    put<uint64_t>(m, 0x178, 0);
    put<uint32_t>(m, 0x180, 0);
    for (uintptr_t o = 0x190; o < 0x1d0; o += 4) put<uint32_t>(m, o, (o == 0x1ac || o == 0x1cc) ? F_ONE : 0u);
    put<uint32_t>(m, 0x1d0, 0);
    put<uint32_t>(m, 0x1d4, 0);
    put<uint32_t>(m, 0x1d8, 0);
    put<uint16_t>(m, 0x1dc, 0);
    put<uint8_t>(m, 0x1de, 0);
    put<uint8_t>(m, 0x1e0, 0);
    for (uintptr_t o = 0x1e4; o < 0x204; o += 4) put<uint32_t>(m, o, 0);   // lock target, weapon numbers, weapon mode
    put<uint32_t>(m, 0x204, 1);
    put<uint32_t>(m, 0x208, 0);
    put<uint32_t>(m, 0x20c, 0);
    put<uint32_t>(m, 0x210, 0);
    put<uint64_t>(m, 0x220, 0);
    put<uint64_t>(m, 0x228, 0);
    put<uint8_t>(m, 0x230, 0);
    put<uint8_t>(m, 0x231, 0);
    put<uint8_t>(m, 0x232, 0xff);
    put<uint8_t>(m, 0x233, 0xff);
    put<uint8_t>(m, 0x234, 0);
    put<uint8_t>(m, 0x235, 0);
}

// 4.5 for a player that is not null: one message built, filled, sent to the slots of `mask` and released
U3X_FN static void send_state(uintptr_t base, void *s, uint8_t *player, uint32_t mask) {
    uint32_t context = 0x14;
    game_call<void, uint32_t *, uintptr_t, uint64_t, uintptr_t>(base + ALLOC_ENTER_FN, &context, base + SOURCE_NAME,
                                                                 uint64_t{0}, base + SOURCE_NAME);
    uint32_t pool[2];
    pool[0] = 0x14;
    pool[1] = 0;
    uint8_t *const m = game_call<uint8_t *, uint64_t, uint32_t *, uint64_t>(base + ALLOC_FN, uint64_t{0x240}, pool,
                                                                            uint64_t{0x10});
    if (m) {
        message_start(base, m);
        uint32_t bits = u3h_player_fill(base, s, m, player);
        if (bits != 0) {
            bits = (bits + 31u) & ~31u;            // only whole 32-bit words survive the game's writer
            put<uint32_t>(m, 0x144, bits);
        }
        uint32_t *const e = ec();
        e[61] += 1;
        e[62] = bits;
        if (bits > e[63]) e[63] = bits;
        if (bits != 0) {
            game_call<void, uintptr_t, uint8_t *, uint32_t>(base + SEND_FN, base + MANAGER, m, mask);
            e[64] += 1;
            e[65] = mask;
        }
        // release our reference
        const uint8_t left = static_cast<uint8_t>(rd<uint8_t>(m, 0xa) - 1);
        put<uint8_t>(m, 0xa, left);
        if (left == 0) game_call<void, uint8_t *>(method_of(m, 8), m);
    }
    game_call<void>(base + ALLOC_LEAVE_FN);
}

}  // namespace g48b

// 4.4: distance between two positions as a float pattern; 0 for NaN and tiny values
U3X_FN uint32_t u3x_distance(const uint8_t *a, const uint8_t *b) {
    return g48b::distance(a, b);
}

// 4.5
U3X_FN void u3x_send_player_state(uintptr_t base, void *s, uint8_t *player, uint32_t mask) {
    if (!player) return;
    g48b::send_state(base, s, player, mask);
}

// 4.6: decides for which remote players the state of local net player `np` is due, and sends it to them
U3X_FN void u3x_replication(uintptr_t base, void *s, uint8_t *np, uint8_t *player) {
    using namespace g48b;
    if (ec()[0] & 0x100u) return;
    if (!player || rd<const uint8_t *>(player, 0x138) == nullptr) return;
    ec()[60] += 1;

    const uint32_t now = f_mul(f_from_int(at<int32_t>(base + NET_CLOCK)), F_TICK);
    const int32_t screen = rd<int32_t>(np, 0x1e8);
    uint32_t mask = 0;

    for (int i = 0; i < RECORD_COUNT; i++) {
        uint8_t *const q = record(base, i);
        if (at<uint8_t>(base + NET_ACTIVE) == 0) continue;
        if (rd<uint8_t>(q, 0xe1) == 0) continue;           // not joined
        if (rd<uint8_t>(q, 0x5e4) != 0) continue;          // leaving
        if (rd<int32_t>(q, 0x1e8) == 1) continue;          // served through its partner
        if (screen != 0 && screen != 1) continue;

        // the nearer of the record and its split-screen partner
        uint32_t d = distance(q, np);
        uint32_t partner_id = rd<uint32_t>(q, 0x1e4) & 0xfffu;
        if (rd<int32_t>(q, 0x1e8) == 0) partner_id += 0x1000u;
        if (partner_id != 0) {
            for (int j = 0; j < RECORD_COUNT; j++) {
                const uint8_t *const p = record(base, j);
                if (player_id(p) != partner_id) continue;
                const uint32_t dp = distance(p, np);
                if (!f_less(f_sub(d, dp), 0u)) d = dp;
                break;
            }
        }

        uint32_t *const due = reinterpret_cast<uint32_t *>(q + 0x5f0 + 4 * static_cast<uintptr_t>(screen));
        if (f_less(now, *due)) continue;
        mask |= 1u << i;

        // 15 sends per second up to 15 m, falling to 8 per second at 30 m and beyond
        uint32_t f = f_mul(f_sub(d, F_NEAR), F_BAND);
        if (f_less(f, 0u)) f = 0u;
        if (!f_less(f, F_ONE)) f = F_ONE;
        const uint32_t rate = f_add(f_mul(f_sub(F_ONE, f), F_NEAR), f_mul(f, F_FAR_RATE));
        *due = f_add(now, f_div(F_ONE, rate));
    }
    if (mask == 0) return;

    send_state(base, s, player, mask);

    // what the player rides sends its own state to the same slots
    const uint8_t *const ride = rd<const uint8_t *>(player, 0x6c0);
    if (ride) {
        using Asked = uint8_t *(*)(const uint8_t *);
        if (reinterpret_cast<Asked>(method_of(ride, 0x58))(ride) != nullptr) {
            uint8_t *const ridden = reinterpret_cast<Asked>(method_of(ride, 0x58))(ride);
            game_call<void, uint8_t *, uint32_t>(base + OBJECT_SEND_FN, ridden, mask);
            ec()[73] += 1;
        }
    }
}
