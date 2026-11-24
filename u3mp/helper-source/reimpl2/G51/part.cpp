// G51: anim decoder lookups, bit reader, float call shims, ice-snapshot commands (CreateAnimCmds).
// Written from spec G51_anim_lookups.md.

extern "C" {
// other groups and the C part
int32_t u3x_tab_find(uint8_t *table, uint32_t key);
uint8_t *u3h_find_art_group_name(uintptr_t base, const char *name);
const char *u3h_civ_package(int which);
int32_t u3h_pick_snapshot(uintptr_t base, uint8_t *generator, uint32_t *blend);
uint8_t *u3h_net_art_group(uintptr_t base, uint32_t row, uint32_t col);
uint8_t *u3h_anim_by_sid(uint8_t *group, uint32_t sid);
int32_t u3h_skeleton(uint8_t *generator);
uint64_t u3h_call_f32(uintptr_t fn, uint32_t f0, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
void u3h_decode(void *visitor, uint8_t *snapshot, uint8_t *generator, int32_t a4, int32_t a5);
void u3h_v_pose(void *visitor, uint32_t frame, uint8_t *anim, uint32_t index);
void u3h_v_ice(void *visitor, uint16_t type, uint32_t index, uint64_t p1, uint64_t p2, uint64_t p3);
void u3h_v_bind(void *visitor, uint32_t index);
void u3h_v_blend(void *visitor, uint32_t factor, uint32_t a, uint32_t b, uint32_t into, uint32_t mode);
void u3h_v_error(void *visitor, int code);

// this group
uint32_t u3x_float_call(uintptr_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
uint64_t u3x_call_float_arg(uintptr_t fn, uint32_t f0, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
int u3x_float_equal(uint32_t a, uint32_t b);
void u3x_bits_init(void *reader, uint8_t *buf, uint32_t nbytes);
uint32_t u3x_bits_read(void *reader, uint32_t n);
uint8_t *u3x_art_group_sid(uintptr_t base, uint32_t sid);
uint8_t *u3x_anim_by_index(uint8_t *group, int32_t index);
uint8_t *u3x_anim_by_sid(uint8_t *group, uint32_t sid);
uint8_t *u3x_stream_record(uintptr_t base, uint32_t id);
uint8_t *u3x_net_art_group(uintptr_t base, uint32_t row, uint32_t col);
int32_t u3x_generator_skeleton(uint8_t *generator);
int32_t u3x_generator_pick(uintptr_t base, uint8_t *generator, uint32_t *blend);
int u3x_anim_commands(uintptr_t base, const uint64_t *regs);
}

namespace g51 {

U3X_RO const char no_name[1] = {0};

// image offsets of the package name texts the game still holds
constexpr uintptr_t N1 = 0x145b8ff, N2 = 0x145f915, N3 = 0x145f953, N4 = 0x145f92c, N5 = 0x145b90e, N6 = 0x145f93e;

template <typename T> [[gnu::always_inline]] inline T &field(uint8_t *p, uintptr_t off) {
    return *reinterpret_cast<T *>(p + off);
}

[[gnu::always_inline]] inline uint32_t *ec() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0x4450); }

// the anim table of an art group, or null
[[gnu::always_inline]] inline uint8_t *anim_table(uint8_t *group) {
    if (!group) return nullptr;
    uint8_t *block = rd<uint8_t *>(group, 0x30);
    return block ? rd<uint8_t *>(block, 0x10) : nullptr;
}

// byte offset of 16-byte entry k: a wrapping 32-bit product, then widened
[[gnu::always_inline]] inline uintptr_t entry_offset(uint32_t k) { return uintptr_t(uint32_t(k * 16u)); }

// the anim behind entry k of an entry array
[[gnu::always_inline]] inline uint8_t *entry_anim(uint8_t *entries, uint32_t k) {
    uint8_t *slot = rd<uint8_t *>(entries, entry_offset(k) + 8);
    return slot ? rd<uint8_t *>(slot, 0) : nullptr;
}

// the package name of a cell (row 0..5, col 0..7): a text in the image, a civilian name, or the empty text
U3X_FN const char *cell_name(uintptr_t base, uint32_t row, uint32_t col) {
    uintptr_t off = 0;
    if (row == 0) {                       // players
        off = col == 0 ? N1 : col == 1 ? N2 : col == 2 ? N3 : col == 3 ? N4 : 0;
    } else if (row <= 3) {                // the three NPC classes share columns 0, 1 and 3
        if (col == 0) off = N5;
        else if (col == 1) off = N3;
        else if (col == 3) off = N6;
        else if (col == 2) {
            if (row == 3) return u3h_civ_package(0);
            off = row == 1 ? N2 : N4;
        } else if (col == 4 && row == 1) off = N4;
    } else if (col == 0) {                // civilian female, civilian child
        return u3h_civ_package(row == 4 ? 1 : 2);
    }
    return off ? reinterpret_cast<const char *>(base + off) : no_name;
}

// the ice copy (of the generator's two) whose time equals the pattern, or null
U3X_FN uint8_t *copy_holding(uint8_t *generator, uint32_t time) {
    for (uintptr_t off = 0x08; off <= 0x28; off += 0x20)
        if (u3x_float_equal(rd<uint32_t>(generator, off + 0x1c), time)) return generator + off;
    return nullptr;
}

// the first ice copy whose time is neither of the two patterns, or null
U3X_FN uint8_t *copy_free(uint8_t *generator, uint32_t time, uint32_t other_time) {
    for (uintptr_t off = 0x08; off <= 0x28; off += 0x20) {
        const uint32_t t = rd<uint32_t>(generator, off + 0x1c);
        if (!u3x_float_equal(t, time) && !u3x_float_equal(t, other_time)) return generator + off;
    }
    return nullptr;
}

// an ice command with a copy's pointers: P1, P2, then P0
[[gnu::always_inline]] inline void ice_command(uint8_t *visitor, uint16_t type, uint32_t instance, uint8_t *copy) {
    u3h_v_ice(visitor, type, instance, rd<uint64_t>(copy, 8), rd<uint64_t>(copy, 0x10), rd<uint64_t>(copy, 0));
}

[[gnu::always_inline]] inline int32_t error_of(uint8_t *visitor) {
    return *reinterpret_cast<const volatile int32_t *>(visitor + 0x154);
}

// one snapshot of a pair: play its finished ice copy, or decode it and ice the result into a copy
U3X_FN void snapshot_step(uint8_t *visitor, uint8_t *generator, uint8_t *snap, uint8_t *other, uint32_t instance,
                          int32_t depth, uint32_t group) {
    const uint32_t time = rd<uint32_t>(snap, 4);
    uint8_t *copy = copy_holding(generator, time);
    if (copy && rd<uint8_t>(copy, 0x18) == 0) {
        ice_command(visitor, 0xc, instance, copy);
        return;
    }
    u3h_decode(visitor, snap, generator, static_cast<int32_t>(instance), depth);
    if (error_of(visitor) != 0) return;
    if (!copy) {
        const uint32_t new_time = rd<uint32_t>(snap, 4);   // read again after the decode call
        copy = copy_free(generator, new_time, rd<uint32_t>(other, 4));
        if (!copy) {
            u3h_v_error(visitor, 9);
            return;
        }
        field<uint8_t>(copy, 0x18) = 1;
        field<uint32_t>(copy, 0x1c) = new_time;
    } else if (group == 1) {
        field<uint8_t>(copy, 0x18) = 0;
    }
    ice_command(visitor, 0xe, instance, copy);
}

// no snapshot pair: the generator's pose anim
U3X_FN void pose_commands(uintptr_t base, uint8_t *visitor, uint8_t *generator, uint32_t first) {
    uint8_t *group = u3h_net_art_group(base, rd<uint32_t>(generator, 0x60), rd<uint32_t>(generator, 0x2178));
    uint8_t *anim = group ? u3h_anim_by_sid(group, rd<uint32_t>(generator, 0x217c)) : nullptr;
    const int32_t skeleton = u3h_skeleton(generator);
    if (anim) {
        if (rd<int32_t>(anim, 0x40) == skeleton) {
            u3h_v_pose(visitor, 0, anim, first);
            return;
        }
        // made for another skeleton: retarget
        uint64_t joints = 0, valid = 0;
        const uint64_t done = u3h_call_f32(base + 0xdc4240, 0, reinterpret_cast<uint64_t>(anim),
                                           static_cast<uint32_t>(skeleton), reinterpret_cast<uint64_t>(&joints),
                                           reinterpret_cast<uint64_t>(&valid));
        if (done & 0xff) {
            ec()[103] += 1;
            u3h_v_ice(visitor, 0xc, first, joints, 0, valid);
            return;
        }
    }
    u3h_v_bind(visitor, first);
}

}  // namespace g51

// ---- call shims (calling-convention glue)

extern "C" U3X_FN uint32_t u3x_float_call(uintptr_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t result = fn;
    __asm__ volatile("movq %%rsp,%%rbx\n\tandq $-16,%%rsp\n\tcallq *%%rax\n\tmovq %%rbx,%%rsp\n\tmovd %%xmm0,%%eax"
                     : "+a"(result), "+D"(a0), "+S"(a1), "+d"(a2), "+c"(a3)
                     :
                     : "rbx", "r8", "r9", "r10", "r11", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
                       "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "st", "st(1)", "st(2)",
                       "st(3)", "st(4)", "st(5)", "st(6)", "st(7)", "memory", "cc");
    return static_cast<uint32_t>(result);
}

extern "C" U3X_FN uint64_t u3x_call_float_arg(uintptr_t fn, uint32_t f0, uint64_t a0, uint64_t a1, uint64_t a2,
                                              uint64_t a3) {
    uint64_t result = fn;
    __asm__ volatile("movq %%rsp,%%rbx\n\tandq $-16,%%rsp\n\tmovd %[f],%%xmm0\n\tcallq *%%rax\n\tmovq %%rbx,%%rsp"
                     : "+a"(result), "+D"(a0), "+S"(a1), "+d"(a2), "+c"(a3)
                     : [f] "r"(f0)
                     : "rbx", "r8", "r9", "r10", "r11", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
                       "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "st", "st(1)", "st(2)",
                       "st(3)", "st(4)", "st(5)", "st(6)", "st(7)", "memory", "cc");
    return result;
}

// ---- float pattern equality: numeric, a NaN equals nothing, the two zeros are equal

extern "C" U3X_FN int u3x_float_equal(uint32_t a, uint32_t b) {
    const uint32_t ma = a & 0x7fffffffu, mb = b & 0x7fffffffu;
    if (ma > 0x7f800000u || mb > 0x7f800000u) return 0;
    return (a == b || (ma | mb) == 0) ? 1 : 0;
}

// ---- bit reader

extern "C" U3X_FN void u3x_bits_init(void *reader, uint8_t *buf, uint32_t nbytes) {
    uint8_t *r = static_cast<uint8_t *>(reader);
    g51::field<uint8_t *>(r, 0) = buf;
    g51::field<uint32_t>(r, 0x08) = 0;
    g51::field<uint32_t>(r, 0x0c) = 2400;          // capacity of the 300-byte stream buffer, for the game's readers
    g51::field<uint32_t>(r, 0x10) = 0;
    g51::field<uint32_t>(r, 0x14) = nbytes * 8u;
    g51::field<uint32_t>(r, 0x18) = 0;
}

extern "C" U3X_FN uint32_t u3x_bits_read(void *reader, uint32_t n) {
    uint8_t *r = static_cast<uint8_t *>(reader);
    if (n == 0) return 0;
    const uint32_t pos = rd<uint32_t>(r, 0x08);
    if (rd<uint32_t>(r, 0x18) != 0 || n > 32 || pos + n > rd<uint32_t>(r, 0x14)) {
        g51::field<uint32_t>(r, 0x18) = 1;
        return 0;
    }
    const uint32_t *words = rd<const uint32_t *>(r, 0);
    const uint32_t word = pos / 32, bit = pos % 32;
    uint32_t value = (words[word] << bit) >> (32 - n);          // the part in the first word, first bit on top
    if (bit + n > 32) value |= words[word + 1] >> (64 - bit - n);
    g51::field<uint32_t>(r, 0x08) = pos + n;
    return value;
}

// ---- lookups in the game's animation data

extern "C" U3X_FN uint8_t *u3x_art_group_sid(uintptr_t base, uint32_t sid) {
    if (sid == 0) return nullptr;
    uint8_t *manager = at<uint8_t *>(base + 0x1b24b80 + 0x38);
    if (!manager) return nullptr;
    uint8_t *slots = rd<uint8_t *>(manager, 0x38);
    if (!slots) return nullptr;
    for (uint32_t s = 0; s < 0x15e; s++) {
        uint8_t *slot = slots + uintptr_t(s) * 0x2c0;
        if (rd<uint32_t>(slot, 0x20) == 0) continue;
        uint8_t *package = rd<uint8_t *>(slot, 0x18);
        if (!package) continue;
        const int32_t size = rd<int32_t>(package, 0x2c);
        uint8_t *table = rd<uint8_t *>(package, 0x30);
        if (size <= 0 || !table) continue;
        const uint32_t n = static_cast<uint32_t>(size);
        uint32_t e = sid % n;
        for (uint32_t tries = 0; tries < n; tries++) {
            const uint32_t name = rd<uint32_t>(table, g51::entry_offset(e));
            if (name == 0) break;
            if (name == sid) {
                uint8_t *group = rd<uint8_t *>(table, g51::entry_offset(e) + 8);
                if (group) return group;
                break;
            }
            if (++e == n) e = 0;
        }
    }
    return nullptr;
}

extern "C" U3X_FN uint8_t *u3x_anim_by_index(uint8_t *group, int32_t index) {
    uint8_t *table = g51::anim_table(group);
    if (!table) return nullptr;
    if (static_cast<uint32_t>(index) >= rd<uint32_t>(table, 0)) return nullptr;
    uint8_t *entries = rd<uint8_t *>(table, 8);
    if (!entries) return nullptr;
    return g51::entry_anim(entries, static_cast<uint32_t>(index));
}

extern "C" U3X_FN uint8_t *u3x_anim_by_sid(uint8_t *group, uint32_t sid) {
    uint8_t *table = g51::anim_table(group);
    if (!table) return nullptr;
    const int32_t k = u3x_tab_find(table, sid);
    if (k < 0) return nullptr;
    return g51::entry_anim(rd<uint8_t *>(table, 8), static_cast<uint32_t>(k));
}

extern "C" U3X_FN uint8_t *u3x_stream_record(uintptr_t base, uint32_t id) {
    const int32_t count = at<int32_t>(base + 0x1d46088);
    for (int32_t i = 0; i < count && i < 50; i++) {
        uint8_t *record = at<uint8_t *>(base + 0x1d46090 + uintptr_t(i) * 8);
        if (!record) continue;
        const uint32_t lists = rd<uint32_t>(record, 0);
        uint8_t *definition = rd<uint8_t *>(record, 0x30);
        if (!definition) continue;
        const uint32_t *lengths = rd<const uint32_t *>(definition, 0);
        const uint32_t *const *ids = rd<const uint32_t *const *>(definition, 8);
        if (!lengths || !ids) continue;
        for (uint32_t u = 0; u < lists; u++) {
            const uint32_t c = lengths[u];
            if (c == 0) break;                      // a zero-length list ends this record
            const uint32_t *list = ids[u];
            if (!list) continue;
            for (uint32_t k = 0; k < c; k++)
                if (list[k] == id) return record;
        }
    }
    return nullptr;
}

// ---- the generator: package of (row, column), skeleton, snapshot pick

extern "C" U3X_FN uint8_t *u3x_net_art_group(uintptr_t base, uint32_t row, uint32_t col) {
    if (row > 5 || col > 7) {
        g51::ec()[89] |= 0x80000000u;
        return nullptr;
    }
    return u3h_find_art_group_name(base, g51::cell_name(base, row, col));
}

extern "C" U3X_FN int32_t u3x_generator_skeleton(uint8_t *generator) {
    uint8_t *slot = rd<uint8_t *>(generator, 0x48);
    const uint32_t id = rd<uint32_t>(generator, 0x50);
    if (!slot || id == 0) return 0;
    uint8_t *object = rd<uint8_t *>(slot, 0);
    if (!object || rd<uint32_t>(object, 0x7c) != id) return 0;
    const uintptr_t vtable = rd<uintptr_t>(object, 0);
    return game_call<int32_t>(at<uintptr_t>(vtable + 0x190), object);
}

extern "C" U3X_FN int32_t u3x_generator_pick(uintptr_t base, uint8_t *generator, uint32_t *blend) {
    int32_t index = -1;
    *blend = F_ONE;
    u3x_call_float_arg(base + 0x6844f0, rd<uint32_t>(generator, 0x2180), reinterpret_cast<uint64_t>(generator),
                       reinterpret_cast<uint64_t>(&index), reinterpret_cast<uint64_t>(blend), 0);
    return index;
}

// ---- CreateAnimCmds (value stub of game function 0x6847d0): 1 = commands produced, 0 = the game's own function runs

extern "C" U3X_FN int u3x_anim_commands(uintptr_t base, const uint64_t *regs) {
    uint32_t *ec = g51::ec();
    if (ec[0] & 0x200u) return 0;
    uint8_t *generator = reinterpret_cast<uint8_t *>(regs[4]);
    uint8_t *commands = reinterpret_cast<uint8_t *>(regs[5]);
    const uint32_t group = static_cast<uint32_t>(regs[7]);
    const uint32_t first = static_cast<uint32_t>(regs[3]);
    if (!generator || !commands || group > 1) return 0;
    ec[84] += 1;

    uint32_t blend;
    const int32_t index = u3h_pick_snapshot(base, generator, &blend);

    // what a failure puts back: two command buffer words and the two ice copies
    const uint32_t saved_pos = rd<uint32_t>(commands, 0x0c), saved_top = rd<uint32_t>(commands, 0x10);
    uint64_t saved_ice[8];
    for (int i = 0; i < 8; i++) saved_ice[i] = rd<uint64_t>(generator, 8 + uintptr_t(i) * 8);

    alignas(16) uint8_t visitor[0x160];
    g51::field<uint64_t>(visitor, 0x120) = base;
    g51::field<uint8_t *>(visitor, 0x128) = nullptr;     // entries
    g51::field<uint8_t *>(visitor, 0x130) = commands;
    g51::field<uint32_t>(visitor, 0x148) = 0;            // channel
    g51::field<int32_t>(visitor, 0x150) = 1;             // kind: commands
    g51::field<int32_t>(visitor, 0x154) = 0;             // error

    if (index == -1) {
        g51::pose_commands(base, visitor, generator, first);
    } else {
        uint8_t *s0 = generator + 0xb0 + intptr_t(index) * 0x3f0;
        uint8_t *s1 = s0 + 0x3f0;
        g51::snapshot_step(visitor, generator, s0, s1, first, 0, group);
        if (g51::error_of(visitor) == 0) g51::snapshot_step(visitor, generator, s1, s0, first + 1, 1, group);
        if (g51::error_of(visitor) == 0) u3h_v_blend(visitor, blend, first, first + 1, first, 1);
    }

    const int32_t code = g51::error_of(visitor);
    if (code != 0) {
        g51::field<uint32_t>(commands, 0x0c) = saved_pos;
        g51::field<uint32_t>(commands, 0x10) = saved_top;
        for (int i = 0; i < 8; i++) g51::field<uint64_t>(generator, 8 + uintptr_t(i) * 8) = saved_ice[i];
        ec[85] += 1;
        if (code != 12) ec[81] += 1;
        return 0;
    }
    return 1;
}
