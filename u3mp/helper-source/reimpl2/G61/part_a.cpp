// G61 part A: Territory particle re-make (spec G61_territory_items_misc.md, Part A).

extern "C" uint32_t *u3h_h08_state(int which);
extern "C" int u3x_h08_on(void);
extern "C" int u3x_h08_handle_ok(const uint8_t *h);
extern "C" void u3x_h08_kill(uintptr_t base, uint8_t *h);
extern "C" void u3x_h08_one(uintptr_t base, uint8_t *p, int32_t team, uint32_t now, uint32_t *seen);

namespace g61a {

constexpr uintptr_t MAKE_PARTICLES = 0x7b2040;   // game: make the owner-coloured particles of a territory
constexpr uintptr_t MANAGER = 0x1a4d910;         // particle manager
constexpr uint64_t WRITER = 1ull << 63;          // lock word: a writer holds it
constexpr int LOCK_TRIES = 100000;

// territory fields
constexpr uintptr_t T_OWNER = 0x34c, T_CAPTURER = 0x350, T_END = 0x374, T_SECONDS = 0x37c;
constexpr uintptr_t T_HANDLES_B = 0x3b0, T_HANDLES_A = 0x3f0, T_GENERATION = 0x3f8, T_EXTRA = 0x438;

// clock ticks
constexpr int32_t REMAKE_DELAY = 150, CAPTURE_DELAY = 450, AID_DELAY = 3000;

template <typename T> [[gnu::always_inline]] inline T &field(uint8_t *p, uintptr_t off) {
    return *reinterpret_cast<T *>(p + off);
}

[[gnu::always_inline]] inline uint32_t *ext_ec() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0x4450); }

// a clock value that is never 0, and the time it stands for
[[gnu::always_inline]] inline uint32_t stamp_of(uint32_t now) { return now | 1u; }
[[gnu::always_inline]] inline int32_t ticks_since(uint32_t now, uint32_t then) { return static_cast<int32_t>(now - then); }
[[gnu::always_inline]] inline int32_t ticks_since_stamp(uint32_t now, uint32_t stamp) { return ticks_since(now, stamp & ~1u); }

// the object a handle points at when the handle is valid, else null
[[gnu::always_inline]] inline uint8_t *handle_object(const uint8_t *h) {
    uint8_t *const *slot = rd<uint8_t *const *>(h, 0);
    const uint32_t generation = rd<uint32_t>(h, 8);
    if (!slot || !generation) return nullptr;
    uint8_t *object = *slot;
    if (!object || rd<uint32_t>(object, 0) != generation) return nullptr;
    return object;
}

// kill the eight particles of a territory: B then A, handle by handle
U3X_FN void kill_all(uintptr_t base, uint8_t *p) {
    for (uintptr_t j = 0; j < 4; ++j) {
        u3x_h08_kill(base, p + T_HANDLES_B + 16 * j);
        u3x_h08_kill(base, p + T_HANDLES_A + 16 * j);
    }
}

// the five slot arrays of the C part
struct Slots {
    uint64_t *address;
    uint32_t *generation;
    uint32_t *stamp;
    int32_t *owner;
    uint32_t *handled_end;
};

}  // namespace g61a

extern "C" U3X_FN int u3x_h08_handle_ok(const uint8_t *h) {
    return g61a::handle_object(h) ? 1 : 0;
}

extern "C" U3X_FN void u3x_h08_kill(uintptr_t base, uint8_t *h) {
    using namespace g61a;
    if (!handle_object(h)) return;

    uint8_t *manager = reinterpret_cast<uint8_t *>(base + MANAGER);
    uint64_t *lock = reinterpret_cast<uint64_t *>(manager + 0x10);

    // take the lock as a reader
    bool locked = false;
    for (int i = 0; i < LOCK_TRIES && !locked; ++i) {
        uint64_t readers = __atomic_load_n(lock, __ATOMIC_RELAXED) & ~WRITER;
        locked = __atomic_compare_exchange_n(lock, &readers, readers + 1, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
    if (!locked) return;   // the handle stays as it is

    if (rd<uint8_t>(manager, 0xfbd)) {
        // the object may have changed while we waited: look again
        uint8_t *object = *rd<uint8_t *const *>(h, 0);
        if (object && rd<uint32_t>(object, 0) == rd<uint32_t>(h, 8)) object[4] |= 1;   // "kill me"
    }
    __atomic_fetch_sub(lock, 1, __ATOMIC_SEQ_CST);

    // the handle is cleared also when the flag was not set
    field<uint64_t>(h, 8) = 0;
    field<uint64_t>(h, 0) = 0;
}

extern "C" U3X_FN void u3x_h08_one(uintptr_t base, uint8_t *p, int32_t team, uint32_t now, uint32_t *seen) {
    using namespace g61a;

    // the start values all later steps work with
    const int32_t own = field<int32_t>(p, T_OWNER);
    const int32_t cap = field<int32_t>(p, T_CAPTURER);
    const uint32_t end = field<uint32_t>(p, T_END);
    const uint32_t generation = field<uint32_t>(p, T_GENERATION);

    Slots s;
    s.address = reinterpret_cast<uint64_t *>(u3h_h08_state(0));
    s.generation = u3h_h08_state(1);
    s.stamp = u3h_h08_state(2);
    s.owner = reinterpret_cast<int32_t *>(u3h_h08_state(3));
    s.handled_end = u3h_h08_state(4);

    uint32_t *ea = ext_ea();
    uint32_t *ec = ext_ec();
    uint32_t *diag = ec + 216;
    const uint64_t address = reinterpret_cast<uintptr_t>(p);

    // 1. the territory's slot: its own, else the first empty one (registered now)
    int k = -1, empty = -1;
    for (int i = 0; i < 8; ++i) {
        if (s.address[i] == address) { k = i; break; }
        if (!s.address[i] && empty < 0) empty = i;
    }
    if (k < 0 && empty >= 0) {
        k = empty;
        s.address[k] = address;
        s.owner[k] = own;
        s.handled_end[k] = 0;
        s.generation[k] = generation;
        s.stamp[k] = 0;
        ea[147 + k] = stamp_of(now);
        ea[155] &= ~(1u << k);
    }
    const bool has_slot = k >= 0;

    if (has_slot) {
        // 2. seen in this scan
        *seen |= 1u << k;
        // 3. owner change
        if (s.owner[k] != own) {
            diag[2] += 1;
            s.owner[k] = own;
            s.stamp[k] = stamp_of(now);
            const uint32_t owner_byte = static_cast<uint32_t>(own) & 0xff;
            diag[13] = s.generation[k] == generation ? owner_byte + 0x100 : owner_byte;
        }
        // 4. no change pending: follow the particle generation
        if (!s.stamp[k]) s.generation[k] = generation;
    }

    // 5. diagnostics, always (before the switch test, also without a slot)
    diag[4] = (static_cast<uint32_t>(own) & 0xff) | (static_cast<uint32_t>(cap) & 0xff) << 8 |
              (static_cast<uint32_t>(team) & 0xff) << 16 | 1u << 24;
    diag[5] = end;
    diag[6] = now;
    diag[7] = field<uint32_t>(p, T_SECONDS);
    diag[8] = (u3x_h08_handle_ok(p + T_HANDLES_A) ? 1u : 0u) | (u3x_h08_handle_ok(p + T_HANDLES_B) ? 2u : 0u) |
              (field<uint32_t>(p, T_EXTRA) ? 4u : 0u);
    diag[9] = static_cast<uint32_t>(address);

    // 6.
    if (!u3x_h08_on() || !has_slot) return;

    // 7. test aid: give a still unowned territory an owner, once
    const uint32_t aid = (ext_eb()[3] >> 19) & 3;
    if (aid && own == -1 && (team == 0 || team == 1) && !(ea[155] & (1u << k)) &&
        ticks_since_stamp(now, ea[147 + k]) >= AID_DELAY) {
        const int32_t owner = (aid & 1) ? team : (team ^ 1);
        ea[155] |= 1u << k;
        kill_all(base, p);
        field<int32_t>(p, T_OWNER) = owner;
        field<uint64_t>(p, T_END) = 0;
        s.owner[k] = owner;
        game_call<void, uint8_t *>(base + MAKE_PARTICLES, p);
        s.generation[k] = field<uint32_t>(p, T_GENERATION);
        s.stamp[k] = 0;
        ea[156] += 1;
        ea[157] = (static_cast<uint32_t>(owner) & 0xff) | static_cast<uint32_t>(k) << 8 |
                  (static_cast<uint32_t>(team) & 0xff) << 16;
        return;
    }

    // 8. re-make after an owner change the game did not follow with new particles
    if (s.stamp[k] && ticks_since_stamp(now, s.stamp[k]) >= REMAKE_DELAY) {
        if (generation == s.generation[k] && own != -1) {
            kill_all(base, p);
            game_call<void, uint8_t *>(base + MAKE_PARTICLES, p);
            diag[12] += 1;
        }
        s.stamp[k] = 0;
        s.generation[k] = field<uint32_t>(p, T_GENERATION);
    }

    // 9. finish a capture whose owner event did not arrive (owner as read at the start)
    if (end && cap != -1 && own != cap && ticks_since(now, end) >= CAPTURE_DELAY && s.handled_end[k] != end) {
        s.handled_end[k] = end;
        kill_all(base, p);
        field<int32_t>(p, T_OWNER) = cap;
        field<uint64_t>(p, T_END) = 0;
        s.owner[k] = cap;
        game_call<void, uint8_t *>(base + MAKE_PARTICLES, p);
        s.generation[k] = field<uint32_t>(p, T_GENERATION);
        s.stamp[k] = 0;
        diag[3] += 1;
        diag[10] = (static_cast<uint32_t>(cap) & 0xff) | (static_cast<uint32_t>(own) & 0xff) << 8;
    }
}
