// S08 part A: Shade melee knock-back moves restored (spec S08_structure_walks.md, Part A).
// PS4 lost the two Shade knock-back moves that PS3 builds right after the two armor attack moves. They are the armor
// moves with other move ids and animation names, so: clone the armor moves, rename them, append them to lists 1 and 6.

namespace s08_a {

// Shade move parameter record (PS3 1.19 identifiers). The game keeps the text pointers: permanent read-only data.
U3X_RO const char shade_enter_anim[] = "sha-melee-shade-attack-knock-back-a-hero-enter";
U3X_RO const char shade_anim[] = "sha-melee-shade-attack-knock-back-a-hero";
constexpr uint32_t SHADE_ID_A = 0x4b5f37f8u;
constexpr uint32_t SHADE_ID_B = 0xfc353425u;

// armor move ids (the check that the sources are what we expect)
constexpr uint32_t ARMOR_ID_A = 0xbb180a56u;
constexpr uint32_t ARMOR_ID_B = 0x5055663bu;

constexpr uintptr_t ARMOR_LIST1 = 0x1b90690;   // source of clone 0
constexpr uintptr_t ARMOR_LIST6 = 0x1b908b0;   // source of clone 1
constexpr uintptr_t REGISTRY = 0x1b92780;      // + list * 0x800: 256 pointer slots
constexpr uintptr_t COUNTS = 0x1b99780;        // + list * 4: u32 used slots
constexpr uint32_t MOVE_SIZE = 0x220;
constexpr uint32_t BLOCK0 = 0xf8, BLOCK1 = 0x1d8;   // the two parameter blocks of a move

// the helper state positions (they stay): mel[0..3] and the two clone areas
inline uint32_t *mel() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0x2768); }
inline uint8_t *clone_area(int k) { return u3h_ext() + 0x2780 + k * MOVE_SIZE; }

U3X_FN bool source_ok(const uint8_t *m) {
    return rd<uint32_t>(m, BLOCK0 + 0x10) == ARMOR_ID_A && rd<uint32_t>(m, BLOCK0 + 0x14) == ARMOR_ID_B &&
           rd<uint32_t>(m, BLOCK1 + 0x10) == ARMOR_ID_A;
}

U3X_FN void rename_block(uint8_t *blk) {
    *reinterpret_cast<const char **>(blk + 0) = shade_enter_anim;
    *reinterpret_cast<const char **>(blk + 8) = shade_anim;
    *reinterpret_cast<uint32_t *>(blk + 0x10) = SHADE_ID_A;
    *reinterpret_cast<uint32_t *>(blk + 0x14) = SHADE_ID_B;
}

}  // namespace s08_a

extern "C" U3X_FN void u3x_melee_restore(uintptr_t base) {
    using namespace s08_a;
    uint32_t *m = mel();
    if (m[0] != 0) return;   // done already

    // both sources checked before anything is written; a failure retries on the next init
    if (!source_ok(reinterpret_cast<const uint8_t *>(base + ARMOR_LIST1)) ||
        !source_ok(reinterpret_cast<const uint8_t *>(base + ARMOR_LIST6))) {
        m[3] += 1;
        return;
    }

    for (int k = 0; k < 2; ++k) {
        const uint8_t *s = reinterpret_cast<const uint8_t *>(base + (k == 0 ? ARMOR_LIST1 : ARMOR_LIST6));
        const uintptr_t list = k == 0 ? 1 : 6;
        uint8_t *c = clone_area(k);
        for (uint32_t i = 0; i < MOVE_SIZE; ++i) c[i] = s[i];
        rename_block(c + BLOCK0);
        rename_block(c + BLOCK1);

        uint32_t &count = at<uint32_t>(base + COUNTS + list * 4);
        if (count >= 256) {   // list full: the clone stays written but is not appended
            m[3] += 1;
            continue;
        }
        at<uint8_t *>(base + REGISTRY + list * 0x800 + uintptr_t(count) * 8) = c;
        count += 1;
        m[0] += 1;
        m[1 + k] = count;
    }
}
