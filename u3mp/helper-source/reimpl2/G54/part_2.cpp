// G54 part 2: the lobby's package list (kit models, optional taunt packages), content flags, diagnostics.
// Written from spec G54_taunt_kit_packages.md (sections 3, 4.4 to 4.7, 6).

extern "C" uint32_t u3h_crc_sid(const char *text);
extern "C" void u3h_s19_count(int what, uint32_t v);
extern "C" const char *u3h_taunt_row_name(uint32_t row);
extern "C" const uint8_t *u3h_content_packs(uint32_t *count);

extern "C" const char *u3x_pkg_taunt_name(uint32_t sid, char out[64]);
extern "C" int u3x_pkg_taunt_slot(uint8_t *m, uint32_t *slot, const char *name);
extern "C" int u3x_pkg_taunt_tick(uintptr_t base, void *s, uint8_t *m);
extern "C" void u3x_loadout_tick(uintptr_t base, void *s);

namespace g54_part2 {

// image offsets
constexpr uintptr_t LEVEL_FLAG = 0x1afcf0a;       // byte in front of the level manager
constexpr uintptr_t LEVEL_MANAGER = 0x1afcf10;
constexpr uintptr_t CONTENT_FLAGS = 0x17c3a40;    // one byte per content pack number
constexpr uintptr_t SAVED_SEED = 0x17cb3b0;       // s32, -1 = no synced random sequence open
constexpr uintptr_t PROFILES = 0x17dd610;         // pointer to the local users' profile objects
constexpr uintptr_t PACKAGE_MANAGER = 0x17f9938;

constexpr uint32_t SID_NO_TAUNT = 0x0e69839du;
constexpr uint32_t SID_NO_PART = 0xf114b39du;
constexpr uint32_t STATE_FAILED = 12;

constexpr int ENTRIES = 50;   // package list entries
constexpr int PLAYERS = 2, PARTS = 11;

using Request = void (*)(uint8_t *manager);                          // native 49
using FindActor = const uint8_t *(*)(void *level_manager, uint32_t sid);   // native 50

// this group's pointer test: a range only, no alignment condition
[[gnu::always_inline]] inline bool plausible(const void *p) {
    const uintptr_t v = reinterpret_cast<uintptr_t>(p);
    return v >= 0x10000u && v < (1ull << 47);
}

[[gnu::always_inline]] inline uint32_t *ts() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0xb80); }
[[gnu::always_inline]] inline uint32_t *ec() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0x4450); }

// the package list of manager m
[[gnu::always_inline]] inline uint32_t &list_id(uint8_t *m, int i) { return *reinterpret_cast<uint32_t *>(m + 4 + 4 * i); }
[[gnu::always_inline]] inline char *list_name(uint8_t *m, int i) { return reinterpret_cast<char *>(m + 0xcc + 0x40 * i); }

// copies at most 63 characters and ends the text
[[gnu::always_inline]] inline void copy_name(char *to, const char *from) {
    int n = 0;
    while (n < 63 && from[n]) { to[n] = from[n]; n++; }
    to[n] = 0;
}

[[gnu::always_inline]] inline void list_put(uint8_t *m, int i, const char *name, uint32_t sid) {
    copy_name(list_name(m, i), name);
    list_id(m, i) = sid;
}

[[gnu::always_inline]] inline void list_clear(uint8_t *m, int i) {
    list_id(m, i) = 0;
    list_name(m, i)[0] = 0;
}

// wanted kit part k of local player p when it is a non-empty name, else null
[[gnu::always_inline]] inline const char *part_name(const uint8_t *m, int p, int k) {
    const char *name = rd<const char *>(m, 0xe00 + 0x58 * p + 8 * k);
    return name && name[0] ? name : nullptr;
}

[[gnu::always_inline]] inline bool active(const uint8_t *m) { return rd<uint8_t>(m, 0xf70) != 0; }

// low byte of a load request's status, `missing` when the request pointer is null
[[gnu::always_inline]] inline uint32_t request_status(const uint8_t *actor, uint32_t q, uint32_t missing) {
    const uint8_t *request = rd<const uint8_t *>(actor, 0x80 + 8ull * q);
    return request ? (rd<uint32_t>(request, 0x20) & 0xff) : missing;
}

}  // namespace g54_part2

// The package name of one of the newer taunts (rows 15 and up of the taunt name list).
U3X_FN const char *u3x_pkg_taunt_name(uint32_t sid, char out[64]) {
    using namespace g54_part2;
    if (sid == 0 || sid == SID_NO_TAUNT) return nullptr;
    uint32_t *t = ts();
    for (int i = 12; i < 16; i++)
        if (t[i] == sid) return nullptr;   // its package failed to load before

    for (uint32_t row = 15;; row++) {
        const char *name = u3h_taunt_row_name(row);
        if (!name) break;
        copy_name(out, name);
        if (u3h_crc_sid(out) == sid) return out;
    }
    t[9]++;
    t[10] = sid;
    return nullptr;
}

// One owner's entry in the list: releases what the owner had and lists `name` (null = nothing) for it.
U3X_FN int u3x_pkg_taunt_slot(uint8_t *m, uint32_t *slot, const char *name) {
    using namespace g54_part2;
    const uint32_t sid = name ? u3h_crc_sid(name) : 0;

    int owned = -1;
    if (slot[0] >= 1 && slot[0] <= static_cast<uint32_t>(ENTRIES) && list_id(m, static_cast<int>(slot[0]) - 1) == slot[1])
        owned = static_cast<int>(slot[0]) - 1;
    if (owned >= 0 && slot[1] == sid) return 0;

    bool listed = false;
    int free_one = -1;
    if (sid) {
        for (int i = 0; i < ENTRIES; i++) {
            const uint32_t v = list_id(m, i);
            if (i != owned && v == sid) { listed = true; break; }
            if (v == 0 && free_one < 0) free_one = i;
        }
    }

    int changed = 0;
    if (owned >= 0) {
        list_clear(m, owned);
        changed = 1;
        if (free_one < 0 || owned < free_one) free_one = owned;
    }
    slot[0] = 0;
    slot[1] = 0;
    if (!sid || listed || free_one < 0) return changed;

    list_put(m, free_one, name, sid);
    slot[0] = static_cast<uint32_t>(free_one) + 1;
    slot[1] = sid;
    ts()[8]++;
    return 1;
}

// The taunt part of the tick: the equipped taunts of the two local users as list entries.
U3X_FN int u3x_pkg_taunt_tick(uintptr_t base, void *s, uint8_t *m) {
    using namespace g54_part2;
    uint32_t *t = ts();
    const FindActor find = rd<FindActor>(s, 768 + 8 * 50);
    int changed = 0;

    if (!(ec()[0] & 16)) {   // taunt packages in the lobby list are off: release all owners
        for (int k = 0; k < 3; k++) changed |= u3x_pkg_taunt_slot(m, t + 2 * k, nullptr);
        return changed;
    }

    // packages that failed to load are remembered and released
    if (find) {
        for (int k = 0; k < 3; k++) {
            const uint32_t sid = t[2 * k + 1];
            if (!sid) continue;
            const uint8_t *actor = find(reinterpret_cast<void *>(base + LEVEL_MANAGER), sid);
            if (!plausible(actor) || rd<uint32_t>(actor, 0x30) != STATE_FAILED) continue;
            int at_word = 12;
            while (at_word < 15 && t[at_word]) at_word++;
            t[at_word] = t[2 * k + 1];   // read again after find's call
            t[11]++;
            changed |= u3x_pkg_taunt_slot(m, t + 2 * k, nullptr);
        }
    }

    const uint8_t *profiles = at<const uint8_t *>(base + PROFILES);
    for (int p = 0; p < 2; p++) {
        char text[64];
        const char *name = nullptr;
        if (plausible(profiles)) {
            const uint8_t *profile = profiles + 0x7b00ull * p;
            if (rd<uint32_t>(profile, 0x7ae8) == 3) name = u3x_pkg_taunt_name(rd<uint32_t>(profile, 0x784), text);
        }
        changed |= u3x_pkg_taunt_slot(m, t + 2 * p, name);
    }
    changed |= u3x_pkg_taunt_slot(m, t + 4, nullptr);   // the retired owner is only released
    return changed;
}

// The package list tick: content flags, kit parts in and out of the list, the list request, diagnostics.
U3X_FN void u3x_loadout_tick(uintptr_t base, void *s) {
    using namespace g54_part2;
    uint8_t *m = reinterpret_cast<uint8_t *>(base + PACKAGE_MANAGER);
    void *level_manager = reinterpret_cast<void *>(base + LEVEL_MANAGER);
    uint32_t *d = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(s) + 0x3e40);
    const Request request = rd<Request>(s, 768 + 8 * 49);
    const FindActor find = rd<FindActor>(s, 768 + 8 * 50);
    if (!request) return;

    at<uint8_t>(base + LEVEL_FLAG) = 1;
    if (at<int32_t>(base + SAVED_SEED) == 0) {
        at<int32_t>(base + SAVED_SEED) = -1;
        ec()[30]++;
    }
    if (!(rd<uint32_t>(s, 0x20c0) & 0x80)) {
        uint32_t count = 0;
        const uint8_t *packs = u3h_content_packs(&count);
        for (uint32_t i = 0; i < count; i++) at<uint8_t>(base + CONTENT_FLAGS + packs[i]) = 1;
    }
    d[0]++;

    const bool lobby = active(m);
    bool changed = false;

    // entries that no wanted part and no owner slot stands for are dropped
    if (lobby && !(ext_eb()[2] & 0x10000000u)) {
        uint32_t wanted[PLAYERS * PARTS];
        int wanted_count = 0;
        for (int p = 0; p < PLAYERS; p++)
            for (int k = 0; k < PARTS; k++) {
                const char *name = part_name(m, p, k);
                if (!name) continue;
                const uint32_t sid = u3h_crc_sid(name);
                if (sid != SID_NO_PART) wanted[wanted_count++] = sid;
            }
        const uint32_t *t = ts();
        for (int i = 0; i < ENTRIES; i++) {
            const uint32_t v = list_id(m, i);
            if (!v) continue;
            bool keep = false;
            for (int w = 0; w < wanted_count && !keep; w++) keep = wanted[w] == v;
            for (int k = 0; k < 3 && !keep; k++) keep = t[2 * k] == static_cast<uint32_t>(i) + 1 && t[2 * k + 1] == v;
            if (keep) continue;
            list_clear(m, i);
            changed = true;
            u3h_s19_count(0, 0);
        }
    }

    // wanted parts that are not listed yet go into free entries
    if (lobby) {
        for (int p = 0; p < PLAYERS; p++)
            for (int k = 0; k < PARTS; k++) {
                const char *name = part_name(m, p, k);
                if (!name) continue;
                const uint32_t sid = u3h_crc_sid(name);
                if (sid == SID_NO_PART) continue;
                bool listed = false;
                int free_one = -1;
                for (int i = 0; i < ENTRIES; i++) {
                    const uint32_t v = list_id(m, i);
                    if (v == sid) { listed = true; break; }
                    if (v == 0 && free_one < 0) free_one = i;
                }
                if (listed) continue;
                if (free_one < 0) { u3h_s19_count(1, 0); continue; }
                list_put(m, free_one, name, sid);
                changed = true;
            }
    }

    if (active(m) && u3x_pkg_taunt_tick(base, s, m)) changed = true;

    uint32_t used = 0;
    for (int i = 0; i < ENTRIES; i++)
        if (list_id(m, i)) used++;
    d[3] = used;
    u3h_s19_count(2, used);

    // the request: on a change, and every 64 ticks while there is something to keep
    if (changed || (used && !(d[0] & 63) && (active(m) || (rd<uint32_t>(s, 0x20c0) & 2)))) {
        if (changed) d[1]++;
        request(m);
        d[2]++;
    }

    if (!find) return;

    // per part: its name id and the state of its package
    for (int p = 0; p < PLAYERS; p++)
        for (int k = 0; k < PARTS; k++) {
            uint32_t *pair = d + 4 + 2 * (PARTS * p + k);
            if (active(m)) {
                const char *name = part_name(m, p, k);
                if (name) pair[0] = u3h_crc_sid(name);
            }
            const uint8_t *actor = pair[0] ? find(level_manager, pair[0]) : nullptr;
            uint32_t state = 0xff, requests = 0, status = 0xffff;
            if (actor) {
                state = rd<uint32_t>(actor, 0x30);
                requests = rd<uint32_t>(actor, 0x78);
                if (requests > 0) status = (status & 0xff00u) | request_status(actor, 0, 0xfe);
                if (requests > 1) status = (status & 0x00ffu) | (request_status(actor, 1, 0xfe) << 8);
            }
            pair[1] = (state & 0xff) + status * 0x100u + (requests & 0xff) * 0x1000000u;
        }

    // the first failed package among the first 20 pairs is reported in D[44..47]
    for (int j = 0; j < 20; j++) {
        if ((d[5 + 2 * j] & 0xff) != STATE_FAILED) continue;
        const uint32_t sid = d[4 + 2 * j];
        const uint8_t *actor = find(level_manager, sid);
        if (!actor) break;
        const uint32_t requests = rd<uint32_t>(actor, 0x78);
        uint32_t nibbles[2] = {0, 0};
        for (uint32_t i = 0; i < 16 && i < requests; i++)
            nibbles[i >> 3] |= (request_status(actor, i, 15) & 15) << (4 * (i & 7));
        const uint8_t *definition = rd<const uint8_t *>(actor, 0x110);
        d[44] = d[4 + 2 * j];   // read again after find's call
        d[45] = nibbles[0];
        d[46] = nibbles[1] | (requests << 24);
        d[47] = definition ? 0x80000000u | rd<uint16_t>(definition, 0x14) | (static_cast<uint32_t>(rd<uint16_t>(definition, 0x18)) << 16) : 0;
        break;
    }
}
