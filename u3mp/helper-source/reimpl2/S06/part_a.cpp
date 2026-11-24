// S06 part A: net ids and connection entries, kill-record lookups, late-join rules (spec S06_net_kill_leftovers.md,
// sections 4.1 to 4.17). Written from the behaviour description only.

// ---- other groups, C part and shared functions (S06 2.2)
struct GtStream;   // G12 3.8 (defined by G12; only pointers are used here)
extern "C" uint8_t *u3h_cm_entry(uintptr_t base, int i);
extern "C" void u3x_gt_write(GtStream *s, uint32_t value, int count);
extern "C" int u3x_gt_base_header(uintptr_t base, uint8_t *sub, void *b);
extern "C" int64_t u3x_gt_vcall(void *sub, int slot, void *a, void *b);
extern "C" void u3h_gt_stamp(uintptr_t base, uint8_t *sub, int off);
extern "C" volatile uint32_t *u3h_bc_state(int which);
extern "C" int32_t u3x_loc_now(uintptr_t base);

namespace s06a {

constexpr uintptr_t CM_LAN_RECORD = 0x28;    // LAN address book record size
constexpr uintptr_t NAME_BYTES = 0x24;       // name field of a record
constexpr uintptr_t ENTRY_SIZE = 0x220;      // connection entry size
constexpr uint32_t EXTRA_BASE = 0x13;        // first statistics number of the extra weapon list

// record name equals `name`: bytes compared up to and including the record's first 0, or all 0x24 bytes
U3X_FN bool lan_name_matches(const uint8_t *rec, const char *name) {
    for (uintptr_t k = 0; k < NAME_BYTES; ++k) {
        const uint8_t c = rec[k];
        if (c != static_cast<uint8_t>(name[k])) return false;
        if (c == 0) return true;
    }
    return true;
}

}  // namespace s06a

// 4.1 late join: is the round mostly over by time
extern "C" U3X_FN int u3x_round_mostly_over(int32_t limit, int32_t elapsed) {
    if (limit <= 0) return 0;
    const int32_t left = static_cast<int32_t>(static_cast<uint32_t>(limit) - static_cast<uint32_t>(elapsed));
    return 4 * static_cast<int64_t>(left) < limit ? 1 : 0;
}

// 4.2 late join: is the match nearly decided by score
extern "C" U3X_FN int u3x_match_nearly_decided(int32_t lead_wins, int32_t rounds, int32_t round) {
    const int32_t last = static_cast<int32_t>(static_cast<uint32_t>(rounds) - 1u);
    return (lead_wins > 1 || round >= last) ? 1 : 0;
}

// 4.3 weapon bar answers
extern "C" U3X_FN int u3x_comp_weapon_answer(uint32_t query, uint32_t weapon, int locked, int32_t *answer) {
    if (query == 0xa1443de7u) { *answer = locked != 0 ? 1 : 0; return 1; }
    if (query == 0x1720a1edu) { *answer = static_cast<int32_t>(weapon); return 1; }
    return 0;
}

// 4.4 LAN address book: update the address of a known name, else add (36-byte copy, count keeps growing)
extern "C" U3X_FN void u3x_lan_address(uint32_t *count, uint8_t *records, const char *name, uint32_t ip) {
    const uint32_t n = *count;
    const uint32_t look = n < 32 ? n : 32;
    for (uint32_t k = 0; k < look; ++k) {
        uint8_t *rec = records + s06a::CM_LAN_RECORD * k;
        if (s06a::lan_name_matches(rec, name)) {
            *reinterpret_cast<uint32_t *>(rec + s06a::NAME_BYTES) = ip;
            return;
        }
    }
    uint8_t *rec = records + s06a::CM_LAN_RECORD * (n & 31u);
    for (uintptr_t k = 0; k < s06a::NAME_BYTES; ++k) rec[k] = static_cast<uint8_t>(name[k]);
    *reinterpret_cast<uint32_t *>(rec + s06a::NAME_BYTES) = ip;
    *count = n + 1u;
}

// 4.5 net player id
extern "C" U3X_FN uint32_t u3x_net_player_id(const uint8_t *np) {
    uint32_t low = rd<uint32_t>(np, 0x1e4) & 0xfffu;
    if (rd<uint8_t>(np, 0x1ec) != 0 && low != 0) low += 0x800u;
    return low + 0x1000u * rd<uint32_t>(np, 0x1e8);
}

// 4.5 tracker id: same layout, same value
extern "C" U3X_FN uint32_t u3x_rp_trk_id(const uint8_t *tr) {
    return u3x_net_player_id(tr);
}

// 4.6 tag test through one tag table bucket
extern "C" U3X_FN int u3x_has_tag(const uint8_t *bucket_head, const uint8_t *tags, uint32_t id) {
    const uint8_t *node = rd<const uint8_t *>(bucket_head, 0);
    for (int walked = 0; walked < 4096; ++walked) {
        if (node == nullptr || node == bucket_head) return 0;
        if (rd<uint32_t>(node, 0x10) == id) {
            const uint32_t b = rd<uint32_t>(node, 0x14);
            const uint64_t word = rd<uint64_t>(tags, 0x20 + 8 * static_cast<uintptr_t>(b >> 6));
            return static_cast<int>((word >> (b & 63u)) & 1u);
        }
        node = rd<const uint8_t *>(node, 0);
    }
    return 0;
}

// 4.7 index of a member in the score keeper, -1 when absent
extern "C" U3X_FN int u3x_score_member(const uint8_t *S, uint32_t id) {
    const uint32_t count = rd<uint32_t>(S, 0);
    const uint32_t look = count < 128 ? count : 128;
    for (uint32_t k = 0; k < look; ++k)
        if (rd<uint32_t>(S, 0x50e4 + 0xa4 * static_cast<uintptr_t>(k)) == id) return static_cast<int>(k);
    return -1;
}

// 4.8 weapon -> statistics number
extern "C" U3X_FN uint32_t u3x_weapon_stat_number(const uint32_t *fixed, const uint32_t *extra, uint32_t weapon) {
    for (uint32_t k = 0; k < s06a::EXTRA_BASE; ++k)   // the 19 fixed entries, numbers 0 .. 0x12
        if (fixed[k] == weapon) return k;
    if (extra == nullptr) return 0;
    const uint32_t count = extra[0];
    const uint32_t look = count < 64 ? count : 64;
    const uint32_t *ids = rd<const uint32_t *>(extra, 8);
    for (uint32_t k = 0; k < look; ++k)
        if (ids[k] == weapon) return s06a::EXTRA_BASE + k;
    return 0;
}

// 4.9 statistics number -> weapon (no 64 cap, wrapping bound)
extern "C" U3X_FN uint32_t u3x_weapon_for_stat(const uint32_t *fixed, const uint32_t *extra, uint32_t number) {
    if (number < s06a::EXTRA_BASE) return fixed[number];
    if (extra == nullptr) return 0;
    if (number >= extra[0] + s06a::EXTRA_BASE) return 0;
    const uint32_t *ids = rd<const uint32_t *>(extra, 8);
    return ids[number - s06a::EXTRA_BASE];
}

// 4.10 connection entry in use with this address and port
extern "C" U3X_FN uint8_t *u3x_entry_by_address(uint8_t *entries, uint32_t ip, uint32_t port) {
    for (uintptr_t i = 0; i < 32; ++i) {
        uint8_t *e = entries + s06a::ENTRY_SIZE * i;
        if (rd<uint8_t>(e, 4) != 0 && rd<uint32_t>(e, 0x2c) == ip && rd<uint16_t>(e, 0x30) == static_cast<uint16_t>(port))
            return e;
    }
    return nullptr;
}

// 4.11 connection entry with this id (in use or not)
extern "C" U3X_FN uint8_t *u3x_entry_by_id(uintptr_t base, int32_t id) {
    for (int i = 0; i < 32; ++i) {
        uint8_t *e = u3h_cm_entry(base, i);
        if (rd<int32_t>(e, 0) == id) return e;
    }
    return nullptr;
}

// 4.12 connection state: -1 not connected, 2 connected
extern "C" U3X_FN int32_t u3x_connection_state(uintptr_t base, int32_t id) {
    const uint8_t *e = u3x_entry_by_id(base, id);
    if (e == nullptr || rd<uint8_t>(e, 4) == 0 || rd<int32_t>(e, 0x34) == 0) return -1;
    return 2;
}

// 4.13 double points event running for the other team
extern "C" U3X_FN int u3x_double_points(uintptr_t base, const uint8_t *np) {
    if (at<int32_t>(base + 0x17dd624) == 0) return 0;
    const int32_t team = np != nullptr ? rd<int32_t>(np, 0x21c) : 0;
    return at<int32_t>(base + 0x17dd634) != team ? 1 : 0;
}

// 4.14 index of a hunter AI, or the (capped) count when absent
extern "C" U3X_FN uint32_t u3x_hunter_index(uintptr_t base, int32_t id) {
    const uintptr_t list = base + 0x17cb060;
    const uint32_t count = at<uint32_t>(list + 0x318);
    const uint32_t look = count < 32 ? count : 32;
    for (uint32_t i = 0; i < look; ++i)
        if (at<int32_t>(list + 0x18 + 0x18 * static_cast<uintptr_t>(i) + 0x10) == id) return i;
    return look;
}

// 4.15 reliable header write
extern "C" U3X_FN void u3x_gt_header(const uint8_t *sub, void *b, uint32_t flag) {
    GtStream *s = static_cast<GtStream *>(b);
    u3x_gt_write(s, flag, 1);
    u3x_gt_write(s, rd<uint32_t>(sub, 8) & 0x7ffu, 11);
    u3x_gt_write(s, rd<uint32_t>(sub, 0xc) & 0x3ffu, 10);
    u3x_gt_write(s, rd<uint32_t>(sub, 0x14), 32);
}

// 4.16 data packet read: stamp, header, payload
extern "C" U3X_FN int u3x_gt_base_read(uintptr_t base, uint8_t *sub, void *b) {
    u3h_gt_stamp(base, sub, 0xa8);
    const int r = u3x_gt_base_header(base, sub, b);
    volatile uint32_t *where = u3h_bc_state(0);
    volatile uint32_t *reads = u3h_bc_state(1);
    if (r != 0) {
        *where = 3;
        *reads = *reads + 1u;
        u3x_gt_vcall(sub, 1, b, nullptr);
        ext_eb()[102] += 1u;
    }
    *where = 2;
    return r;
}

// 4.17 link timeout: 1 when the entry's link went quiet for over 20 s and no kick was made in the last 10 s
extern "C" U3X_FN int u3x_link_timeout(uintptr_t base, uint8_t *e, int32_t *kick_at) {
    const intptr_t dist = reinterpret_cast<intptr_t>(e) - reinterpret_cast<intptr_t>(u3h_cm_entry(base, 0));
    // quotient toward zero, then cut to signed 32 bits (spec Clarification 4)
    const int32_t i = static_cast<int32_t>(static_cast<int64_t>(dist) / static_cast<int64_t>(s06a::ENTRY_SIZE));
    const int32_t now = u3x_loc_now(base);
    if (i < 0 || i > 31) return 0;

    const uint32_t t = rd<uint32_t>(e, 0x40 + 0xa8);
    bool timed_out = false;
    if ((t & 0x7fffffffu) != 0) {
        const uint8_t *clock = at<const uint8_t *>(base + 0x1b24b98);
        const uint32_t game_now = f_mul(f_from_int(rd<int32_t>(clock, 0x114)), 0x3b5a740eu);
        timed_out = f_less(0x41a00000u /*20.0*/, f_sub(game_now, t));
    }
    if (!timed_out) { kick_at[i] = 0; return 0; }

    const int32_t last = kick_at[i];
    if (last != 0 && static_cast<int32_t>(static_cast<uint32_t>(now) - static_cast<uint32_t>(last)) < 3000) return 0;
    kick_at[i] = now != 0 ? now : 1;
    return 1;
}
