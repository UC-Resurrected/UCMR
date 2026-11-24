// G61 part D: net anim groups (spec G61_territory_items_misc.md, Part D; search of G51_anim_lookups.md 4.6).

extern "C" uint8_t *u3x_art_group_sid(uintptr_t base, uint32_t sid);
extern "C" uint8_t *u3x_net_art_group(uintptr_t base, uint32_t row, uint32_t col);
extern "C" uint8_t *u3x_find_art_group(const uint8_t *slots, uint32_t id);
extern "C" uint8_t *u3x_art_group_name(uintptr_t base, const char *name);
extern "C" void u3x_player_net_fill(uintptr_t base, uint8_t *player);

namespace g61d {

constexpr uintptr_t NAME_ID_FN = 0x1702720;   // pointer to the game's name hash: fn(0, name, 1)
constexpr uintptr_t MP_ACTIVE = 0x17d5c61;    // u8
constexpr uint32_t PACKAGE_SLOTS = 350;
constexpr uintptr_t PACKAGE_SLOT_SIZE = 0x2c0;

using NameId = uint32_t (*)(int zero, const char *name, int one);

// the art group with that name id in one package's hash table, or null
[[gnu::always_inline]] inline uint8_t *in_package(const uint8_t *package, uint32_t id) {
    const int32_t n = rd<int32_t>(package, 0x2c);
    const uint8_t *table = rd<const uint8_t *>(package, 0x30);
    if (n <= 0 || !table) return nullptr;
    const uint32_t size = static_cast<uint32_t>(n);
    uint32_t i = id % size;
    for (uint32_t tries = 0; tries < size; ++tries) {
        const uint8_t *entry = table + 16 * static_cast<uintptr_t>(i);
        const uint32_t name = rd<uint32_t>(entry, 0);
        if (!name) return nullptr;                       // an empty entry ends this package
        if (name == id) return rd<uint8_t *>(entry, 8);  // may be null: the next package is tried
        if (++i == size) i = 0;
    }
    return nullptr;
}

}  // namespace g61d

extern "C" U3X_FN uint8_t *u3x_find_art_group(const uint8_t *slots, uint32_t id) {
    using namespace g61d;
    if (!slots) return nullptr;
    for (uint32_t k = 0; k < PACKAGE_SLOTS; ++k) {
        const uint8_t *slot = slots + k * PACKAGE_SLOT_SIZE;
        if (!rd<uint32_t>(slot, 0x20)) continue;   // not in use
        const uint8_t *package = rd<const uint8_t *>(slot, 0x18);
        if (!package) continue;
        if (uint8_t *group = in_package(package, id)) return group;
    }
    return nullptr;
}

extern "C" U3X_FN uint8_t *u3x_art_group_name(uintptr_t base, const char *name) {
    using namespace g61d;
    if (!name || !*name) return nullptr;
    const NameId name_id = at<NameId>(base + NAME_ID_FN);
    if (!name_id) return nullptr;
    return u3x_art_group_sid(base, name_id(0, name, 1));
}

extern "C" U3X_FN void u3x_player_net_fill(uintptr_t base, uint8_t *player) {
    using namespace g61d;
    if (!player || !at<uint8_t>(base + MP_ACTIVE)) return;

    uint8_t *state = u3h_ext();
    const uint32_t *fsw = reinterpret_cast<const uint32_t *>(state + 0x3a70);
    uint32_t *pna = reinterpret_cast<uint32_t *>(state + 0x3c30);

    if (fsw[0] & (1u << 3)) {   // the fix is switched off
        pna[4] += 1;
        return;
    }
    uint8_t **groups = rd<uint8_t **>(player, 0x1c20);
    if (!groups) {
        pna[3] += 1;
        return;
    }

    // all eight entries are written, the player characters' packages being row 0
    uint32_t found = 0, count = 0;
    for (uint32_t col = 0; col < 8; ++col) {
        uint8_t *group = u3x_net_art_group(base, 0, col);
        groups[col] = group;
        if (group) {
            found |= 1u << col;
            count += 1;
        }
    }
    pna[1] += count;
    pna[0] += 1;
    pna[2] = found;
}
