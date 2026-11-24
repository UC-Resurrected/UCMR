// S08 part E: weapon skin materials set on the submesh slots (spec S08_structure_walks.md, Part E).
// After the game's skin apply, each draw instance with a pending material swap {old name, new name} gets the new
// material (looked up by name in its material table) set on every slot whose current material is named like the old
// one, through the game's own slot setter. Only null tests, as the game structures are trusted here.

namespace s08_e {

constexpr uintptr_t SET_SLOT_MATERIAL = 0xe9a350;   // void fn(void *slot, void *material, void *resource)

template <typename T> inline T *ptr_at(const uint8_t *p, uintptr_t off) { return rd<T *>(p, off); }

// name text of a material: m + 0x10 -> +0 -> +0; null at any step = no name
U3X_FN const char *material_name(const uint8_t *m) {
    if (!m) return nullptr;
    const uint8_t *a = ptr_at<uint8_t>(m, 0x10);
    if (!a) return nullptr;
    const uint8_t *b = ptr_at<uint8_t>(a, 0);
    if (!b) return nullptr;
    return ptr_at<const char>(b, 0);
}

// whole-text equality; a missing name never equals anything
U3X_FN bool same_text(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

U3X_FN void apply_instance(uintptr_t base, uint8_t *inst) {
    if (rd<uint32_t>(inst, 0xd8) == 0) return;
    const uint8_t *swap = ptr_at<uint8_t>(inst, 0x160);
    if (!swap) return;
    const uint8_t *g = ptr_at<uint8_t>(inst, 0x1b0);
    if (!g) return;
    void *resource = ptr_at<void>(g, 0x18);
    const uint8_t *table = ptr_at<uint8_t>(g, 0x10);
    if (!resource || !table) return;
    const char *old_name = ptr_at<const char>(swap, 0);
    const char *new_name = ptr_at<const char>(swap, 8);
    if (!old_name || !new_name || !*old_name || !*new_name || same_text(old_name, new_name)) return;

    // first material of the table named like the new name
    uint8_t *mats = ptr_at<uint8_t>(table, 0x10);
    if (!mats) return;
    const uint32_t nmat = rd<uint32_t>(table, 0);
    uint8_t *new_mat = nullptr;
    for (uint32_t k = 0; k < nmat && !new_mat; ++k)
        if (same_text(material_name(mats + uintptr_t(k) * 0x70), new_name)) new_mat = mats + uintptr_t(k) * 0x70;
    if (!new_mat) return;

    // every slot still showing the old material
    uint8_t *slots = ptr_at<uint8_t>(inst, 0x328);
    if (!slots) return;
    const uint32_t nslot = rd<uint32_t>(inst, 0x324);
    for (uint32_t k = 0; k < nslot; ++k) {
        uint8_t *slot = slots + uintptr_t(k) * 0x3a0;
        if (same_text(material_name(ptr_at<uint8_t>(slot, 0x38)), old_name))
            game_call<void>(base + SET_SLOT_MATERIAL, static_cast<void *>(slot), static_cast<void *>(new_mat), resource);
    }
}

}  // namespace s08_e

extern "C" U3X_FN void u3x_skin_material_apply(uintptr_t base, uint8_t *o) {
    using namespace s08_e;
    const uint8_t *holder = ptr_at<uint8_t>(o, 0x130);
    if (!holder) return;
    const uint8_t *parts = ptr_at<uint8_t>(holder, 0x10);
    if (!parts) return;
    // both counts are read again at every step: the slot setter may change them
    for (int32_t j = 0; j < rd<int32_t>(holder, 0x18); ++j) {
        const uint8_t *part = parts + uintptr_t(j) * 0x68;
        for (uint32_t k = 0; k < rd<uint8_t>(part, 0x60); ++k) {
            uint8_t *inst = ptr_at<uint8_t>(part, uintptr_t(k) * 0x18);
            if (inst) apply_instance(base, inst);
        }
    }
}
