// G61 part C: submesh draw diagnostic (spec G61_territory_items_misc.md, Part C). Runs on several render threads:
// every counter update is one atomic operation.

extern "C" uint8_t *u3h_f4_state(void);
extern "C" uint32_t *u3h_f4_current(void);
extern "C" volatile uint8_t *u3h_f4_entry(uint8_t *instance);
extern "C" int u3x_f4_submesh(uintptr_t base, uint64_t *r);

namespace g61c {

constexpr uintptr_t VARIANT_TABLE = 0x13a7bf0;   // u32 per draw type, 27 entries, read from the game
constexpr uint32_t VARIANT_TYPES = 27;
constexpr uintptr_t NO_OVERRIDE = 0x18c6994;     // u8
constexpr uintptr_t MP_ACTIVE = 0x17d5c61, MP_LAN = 0x17d5c60;   // u8 each
constexpr int64_t SUBMESH_SIZE = 0x3a0;

// reasons a submesh is not drawn
constexpr uint32_t WHY_NO_GEOMETRY = 1, WHY_PASS = 2, WHY_HIDDEN = 4, WHY_NO_SHADER = 8, WHY_MODE4 = 0x10,
                   WHY_TYPE_1A = 0x20, WHY_VARIANT_RANGE = 0x40;

// the experiment's candidates, in this order (the helper's own choice, not a game table)
U3X_RO const uint8_t BORROW_FROM[5] = {0, 1, 2, 3, 9};

[[gnu::always_inline]] inline void count(uint32_t *word) { __atomic_fetch_add(word, 1u, __ATOMIC_RELAXED); }
[[gnu::always_inline]] inline void set_bits(volatile uint8_t *byte, uint32_t bits) {
    __atomic_fetch_or(byte, static_cast<uint8_t>(bits), __ATOMIC_RELAXED);
}

// the shader word of variant v of a submesh (v is a signed index)
[[gnu::always_inline]] inline uint64_t *shader_slot(uint8_t *sub, int32_t v) {
    return reinterpret_cast<uint64_t *>(sub + 0x40 + static_cast<int64_t>(v) * 8);
}
[[gnu::always_inline]] inline bool has_shader(uint8_t *sub, int32_t v) { return *shader_slot(sub, v) != 0; }

}  // namespace g61c

extern "C" U3X_FN int u3x_f4_submesh(uintptr_t base, uint64_t *r) {
    using namespace g61c;

    const uint32_t pass = static_cast<uint32_t>(r[6]);
    const uint32_t type = static_cast<uint32_t>(r[7]);
    const int32_t selector = static_cast<int32_t>(r[14]);
    const uint32_t row = pass & 15;

    uint8_t *state = u3h_f4_state();
    uint32_t *hdr = reinterpret_cast<uint32_t *>(state);
    uint32_t *why = reinterpret_cast<uint32_t *>(state + 0x280);
    uint32_t *ring = reinterpret_cast<uint32_t *>(state + 0x2c0);

    // 1. every call
    count(&hdr[4]);

    // 2, 3. the submesh must be an element of its instance's array
    uint8_t *sub = reinterpret_cast<uint8_t *>(r[4]);
    uint8_t *inst = reinterpret_cast<uint8_t *>(r[3] - 0x170);
    bool accepted = ptr_ok(sub) && ptr_ok(inst);
    int64_t offset = 0;
    if (accepted) {
        uint8_t *array = rd<uint8_t *>(inst, 0x168);
        const int32_t n = rd<int32_t>(inst, 0x164);
        accepted = ptr_ok(array) && n > 0 && n <= 4096;
        if (accepted) {
            offset = static_cast<int64_t>(reinterpret_cast<uintptr_t>(sub) - reinterpret_cast<uintptr_t>(array));
            accepted = reinterpret_cast<uintptr_t>(sub) >= reinterpret_cast<uintptr_t>(array) &&
                       offset < n * SUBMESH_SIZE && offset % SUBMESH_SIZE == 0;
        }
    }
    if (!accepted) {
        count(&hdr[5]);
        return 0;
    }

    // 4. index of the submesh, bit of the pass
    const uint32_t index = static_cast<uint32_t>(offset / SUBMESH_SIZE);
    const uint32_t pass_bit = pass < 7 ? 1u << pass : 0x80u;

    // 5. asked for in this pass
    volatile uint8_t *entry = u3h_f4_entry(inst);
    uint32_t *cur = u3h_f4_current() + row * 8;
    set_bits(entry + 2, pass_bit);
    count(&cur[5]);
    if (rd<int32_t>(sub, 0x328) >= 0) count(&why[7]);   // has an LOD index (not a reason)

    // 6. the reasons that need no variant
    uint32_t reason = 0;
    if (!rd<uint64_t>(sub, 0x10) || !rd<uint64_t>(sub, 0x18)) {
        reason = WHY_NO_GEOMETRY;
    } else if (type == 0x1a) {
        reason = WHY_TYPE_1A;
    } else if (!(rd<uint64_t>(sub, 0x240 + 8 * static_cast<uintptr_t>(pass / 64)) >> (pass % 64) & 1)) {
        reason = WHY_PASS;
    } else if (!rd<uint8_t>(sub, 0x256)) {
        reason = WHY_HIDDEN;
    } else {
        // 7. the variant this draw type takes
        int32_t v = type < VARIANT_TYPES ? static_cast<int32_t>(at<uint32_t>(base + VARIANT_TABLE + 4 * type)) : 0;
        const bool sel = selector == 1;
        const bool ovr = rd<uint8_t>(sub, 0x255) != 0 && at<uint8_t>(base + NO_OVERRIDE) == 0;

        if (type == 0) {
            if (sel && has_shader(sub, 0x1b)) v = 8;
        } else if (type == 1) {
            if (sel && has_shader(sub, 0x1b)) v = 0x1b;
        } else if (type == 3) {
            if (ovr) {
                v = 2;
                if (sel && has_shader(sub, 3)) v = 3;
            } else if (sel && has_shader(sub, 1)) {
                v = 1;
            }
            if (has_shader(sub, 0x1f) && v != 1) v = 0x1f;
        } else if (type == 4 || type == 5) {
            if (sel && has_shader(sub, 0x1c)) v = 0x1c;
        } else if (type == 9) {
            v = 0x11;
            if (sel && has_shader(sub, 0x12)) v = 0x12;
        } else if (type == 0xe) {
            if (rd<int32_t>(sub, 0x250) == 4) reason = WHY_MODE4;
            v = 0xe;
        } else if (type == 0xf) {
            if (ovr) v = 0x14;
        }

        if (!reason) {
            if (v >= 0x20) {
                reason = WHY_VARIANT_RANGE;
            } else {
                if (!has_shader(sub, v) && type == 3 && has_shader(sub, 9)) v = 9;
                if (!has_shader(sub, v)) {
                    reason = WHY_NO_SHADER;
                    const uint32_t i = __atomic_fetch_add(&hdr[7], 1u, __ATOMIC_RELAXED) & 31;
                    ring[i] = (rd<uint32_t>(inst, 0xd4) & 0x3ff) | (index & 63) << 10 | row << 16 |
                              (static_cast<uint32_t>(v) & 63) << 20 | (type & 63) << 26;
                    count(&cur[7]);
                    // the opt-in experiment: borrow the first shader present among variants 0, 1, 2, 3, 9
                    const bool multiplayer = at<uint8_t>(base + MP_ACTIVE) || at<uint8_t>(base + MP_LAN);
                    if (multiplayer && (ext_eb()[3] & (1u << 4))) {
                        for (int32_t from : BORROW_FROM) {
                            if (!has_shader(sub, from)) continue;
                            *shader_slot(sub, v) = *shader_slot(sub, from);
                            reason = 0;
                            break;
                        }
                    }
                }
            }
        }
    }

    // 8. the outcome
    if (reason) {
        set_bits(entry + 4, reason);
        for (uint32_t j = 0; j < 7; ++j)
            if (reason >> j & 1) count(&why[j]);
    } else {
        set_bits(entry + 3, pass_bit);
        count(&cur[6]);
    }
    return 0;
}
