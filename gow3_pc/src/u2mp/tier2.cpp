// u2mp tier 2: data the PS4 image no longer carries (event fallbacks, MP->SP twins, co-op points).
// Every patch checks the bytes it replaces first and logs what it did.
#include <cstdint>
#include <cstring>
#include "u2mp_internal.h"
namespace u2mp {
namespace {
constexpr uint64_t fDcFind = 0xa15300;
constexpr uint64_t fSymbolLookup = 0xa15e70;
constexpr uint64_t fBulletSettings = 0xb08950;
// The hand grenade settings the thrower's own event carries; a received grenade can arrive
// without its settings id, and the projectile init asserts instead of accepting that.
constexpr uint32_t kHandGrenadeSettings = 0x769e6113;
using LookupFn = void *(U2MP_GUEST *)(uint32_t);
using FindFn = void *(U2MP_GUEST *)(uint32_t);
void *find_slot(uint32_t id) {
    return reinterpret_cast<FindFn>(image.base + fDcFind)(id);
}
void *find_object(uint32_t id) {
    void *slot = find_slot(id);
    return slot ? *reinterpret_cast<void **>(slot) : nullptr;
}
void *lookup_symbol(uint32_t id) {
    return reinterpret_cast<LookupFn>(image.base + fSymbolLookup)(id);
}
void *find_settings(uint32_t id) {
    unsigned char *object = static_cast<unsigned char *>(find_object(id));
    return object ? *reinterpret_cast<void **>(object + 0xb0) : nullptr;
}
bool patch_bytes(uint64_t offset, const unsigned char *from, const unsigned char *to, size_t length,
                 const char *name) {
    if (!image.executable(offset, length) || !image.matches(offset, from, length)) {
        log("%s (0x%llx): bytes differ, left alone", name, (unsigned long long)offset);
        return false;
    }
    std::memcpy(image.base + offset, to, length);
    log("%s (0x%llx): patched", name, (unsigned long long)offset);
    return true;
}
bool patch_unknown_event() {
    static const unsigned char from[] = {0xcd, 0x41};   // int 0x41, the assert trap
    static const unsigned char to[] = {0x90, 0x90};
    return patch_bytes(0x61818d, from, to, sizeof from, "unknown net event guard");
}
bool patch_coop_points() {
    static const unsigned char from[] = {0x4c, 0x8b, 0x2d, 0x23, 0xf2, 0xf8, 0x00};
    static const unsigned char to[] = {0xe9, 0x87, 0x00, 0x00, 0x00, 0x90, 0x90};
    constexpr uint64_t site = 0x5ded4e, zero_points = 0x5dedda;
    static_assert(zero_points - (site + 5) == 0x87, "co-op points branch");
    return patch_bytes(site, from, to, sizeof from, "co-op points fallback");
}
constexpr uint32_t kNetWeapons = 0x837a26b6;
constexpr uint32_t kSpWeapons = 0x974cf651;
constexpr size_t kMaxWeapons = 64, kWeaponRecord = 0x300;

struct WeaponEntry {
    uint32_t type;
    uint32_t pad;
    uint64_t record;
};
struct WeaponTable {
    int32_t count;
    int32_t pad;
    uint64_t entries;
};
alignas(16) unsigned char weapon_records[kMaxWeapons][kWeaponRecord];
alignas(16) WeaponEntry weapon_entries[kMaxWeapons];
alignas(16) WeaponTable weapon_table;
uint64_t weapon_built_from;

void *build_net_weapons() {
    void *sp = find_object(kSpWeapons);
    if (!sp) sp = lookup_symbol(kSpWeapons);
    if (!sp) return nullptr;
    int32_t count = 0;
    uint64_t entries = 0;
    std::memcpy(&count, sp, 4);
    std::memcpy(&entries, static_cast<unsigned char *>(sp) + 8, 8);
    if (count <= 0 || count > int32_t(kMaxWeapons) || !entries) return nullptr;
    int32_t n = 0;
    for (int32_t i = 0; i < count; ++i) {
        const auto *entry = reinterpret_cast<const unsigned char *>(uintptr_t(entries)) + size_t(i) * 16;
        uint32_t type = 0;
        uint64_t record = 0;
        std::memcpy(&type, entry, 4);
        std::memcpy(&record, entry + 8, 8);
        if (!record) continue;
        std::memcpy(weapon_records[n], reinterpret_cast<const void *>(uintptr_t(record)), kWeaponRecord);
        uint32_t npc_damage = 0;
        std::memcpy(&npc_damage, weapon_records[n] + 0x88, 4);
        std::memcpy(weapon_records[n] + 0x84, &npc_damage, 4);
        weapon_entries[n] = {type, 0, uint64_t(uintptr_t(weapon_records[n]))};
        ++n;
    }
    weapon_table = {n, 0, uint64_t(uintptr_t(weapon_entries))};
    weapon_built_from = uint64_t(uintptr_t(sp));
    log("net weapons: built %d records from the single-player table (player damage = NPC damage)", n);
    return &weapon_table;
}


bool sp_weapon_damage_patched;
bool patch_sp_weapon_damage() {
    if (sp_weapon_damage_patched) return true;
    void *sp = find_object(kSpWeapons);
    if (!sp) sp = lookup_symbol(kSpWeapons);
    static bool reported;
    if (!reported) {
        reported = true;
        log("net weapons: patch try table %p", sp);
    }
    if (!sp) return false;
    int32_t count = 0;
    uint64_t entries = 0;
    std::memcpy(&count, sp, 4);
    std::memcpy(&entries, static_cast<unsigned char *>(sp) + 8, 8);
    if (count <= 0 || count > 512 || entries < 0x10000 || entries > 0x7fffffffffffULL) return false;
    log("net weapons: table %p count %d entries %p", sp, count, reinterpret_cast<void *>(uintptr_t(entries)));
    unsigned changed = 0;
    for (int32_t i = 0; i < count; ++i) {
        const auto *entry = reinterpret_cast<const unsigned char *>(uintptr_t(entries)) + size_t(i) * 16;
        uint64_t record = 0;
        std::memcpy(&record, entry + 8, 8);
        if (!record || record < 0x10000 || record > 0x7fffffffffffULL) continue;
        auto *rec = reinterpret_cast<unsigned char *>(uintptr_t(record));
        uint32_t player = 0, npc = 0;
        std::memcpy(&player, rec + 0x84, 4);
        std::memcpy(&npc, rec + 0x88, 4);
        if (player == 0 && npc != 0) {
            std::memcpy(rec + 0x84, &npc, 4);
            ++changed;
        }
    }
    sp_weapon_damage_patched = true;
    log("net weapons: %u of %d single-player records now use their NPC damage for players", changed, count);
    return true;
}
uint32_t twin_id(uint32_t id) {
    switch (id) {
        case 0x0420a575u: return 0xe74d0f3cu;
        case 0x28246312u: return 0x4688c06eu;
        case 0x3e2aeffeu: return 0xe74d0f3cu;
        case 0x81b237bbu: return 0xb3d1e3bfu;
        default: return id;
    }
}
void log_twin(uint32_t id, uint32_t twin) {
    static uint32_t told[8];
    static unsigned count;
    unsigned i = 0;
    while (i < count && told[i] != id) ++i;
    if (i == count && count < 8) {
        told[count++] = id;
        log("net explosion %08x not loaded -> SP twin %08x", id, twin);
    }
}
LookupFn original_lookup;
U2MP_GUEST void *lookup_twin(uint32_t id) {
    if (void *object = find_object(id)) return object;
    if (id == kNetWeapons) {
        if (!weapon_built_from || weapon_built_from != uint64_t(uintptr_t(find_object(kSpWeapons)))) {
            if (void *table = build_net_weapons()) return table;
        }
        return &weapon_table;
    }
    const uint32_t twin = twin_id(id);
    if (twin != id) {
        if (void *object = find_object(twin)) {
            log_twin(id, twin);
            return object;
        }
    }
    return original_lookup ? original_lookup(id) : nullptr;
}
bool install_twin_lookup() {
    static const unsigned char prologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
    if (!image.matches(fSymbolLookup, prologue, sizeof prologue)) {
        log("net explosion twin lookup (0x%llx): bytes differ, left alone", (unsigned long long)fSymbolLookup);
        return false;
    }
    unsigned char *trampoline = near_stub(32);
    if (!trampoline) {
        log("net explosion twin lookup: no stub memory near the image");
        return false;
    }
    std::memcpy(trampoline, image.base + fSymbolLookup, 6);
    const uint64_t resume = uint64_t(uintptr_t(image.base + fSymbolLookup + 6));
    trampoline[6] = 0x48;
    trampoline[7] = 0xb8;
    std::memcpy(trampoline + 8, &resume, 8);
    trampoline[16] = 0xff;
    trampoline[17] = 0xe0;
    original_lookup = reinterpret_cast<LookupFn>(trampoline);
    return replace_function(fSymbolLookup, 6, reinterpret_cast<void *>(lookup_twin), "net explosion twin lookup");
}
void log_bullet(uint32_t id, uint32_t fallback) {
    static uint32_t told[8];
    static unsigned count;
    unsigned i = 0;
    while (i < count && told[i] != id) ++i;
    if (i == count && count < 8) {
        told[count++] = id;
        log("bullet settings %08x not loaded -> hand grenade %08x", id, fallback);
    }
}
U2MP_GUEST void *get_bullet_settings(void *object) {
    const uint32_t id = *reinterpret_cast<const uint32_t *>(static_cast<unsigned char *>(object) + 0x324);
    if (void *settings = find_settings(id)) return settings;
    if (id != kHandGrenadeSettings) {
        if (void *settings = find_settings(kHandGrenadeSettings)) {
            log_bullet(id, kHandGrenadeSettings);
            return settings;
        }
    }
    static bool said = false;
    if (!said) {
        said = true;
        log("bullet settings %08x and fallback %08x not loaded", id, kHandGrenadeSettings);
    }
    return nullptr;
}

// A received grenade can carry a zero bullet-settings id; Init asserts and then looks up id 0, which
// leaves the projectile without settings and spins. Give it the thrower's hand-grenade settings before
// the lookup.
bool install_projectile_fallback() {
    constexpr uint64_t branch = 0xb05781, load = 0xb057b8, lookup = 0xb057bf;
    static const unsigned char branch_from[] = {0x75, 0x3c};
    static const unsigned char branch_to[] = {0xeb, 0x35};
    static const unsigned char load_from[] = {0x41, 0x8b, 0xbd, 0x24, 0x03, 0x00, 0x00};
    if (!image.matches(branch, branch_from, sizeof branch_from) ||
        !image.matches(load, load_from, sizeof load_from)) {
        log("projectile bullet-settings fallback: bytes differ, left alone");
        return false;
    }
    unsigned char *stub = near_stub(64);
    if (!stub) {
        log("projectile bullet-settings fallback: no stub memory near the image");
        return false;
    }
    static const unsigned char body[] = {
        0x41, 0x8b, 0xbd, 0x24, 0x03, 0x00, 0x00,  // mov edi, [r13+0x324]
        0x85, 0xff,                                // test edi, edi
        0x75, 0x0c,                                // jne +12
        0xbf, 0x13, 0x61, 0x9e, 0x76,              // mov edi, 0x769e6113
        0x41, 0x89, 0xbd, 0x24, 0x03, 0x00, 0x00,  // mov [r13+0x324], edi
        0xe9, 0x00, 0x00, 0x00, 0x00,              // jmp lookup
    };
    std::memcpy(stub, body, sizeof body);
    const int64_t rel = int64_t(uintptr_t(image.base + lookup)) - int64_t(uintptr_t(stub + sizeof body));
    if (rel < INT32_MIN || rel > INT32_MAX) {
        log("projectile bullet-settings fallback: stub out of range");
        return false;
    }
    std::memcpy(stub + sizeof body - 4, &rel, 4);
    const int64_t site_rel = int64_t(uintptr_t(stub)) - int64_t(uintptr_t(image.base + load + 5));
    if (site_rel < INT32_MIN || site_rel > INT32_MAX) {
        log("projectile bullet-settings fallback: site out of range");
        return false;
    }
    std::memcpy(image.base + branch, branch_to, sizeof branch_to);
    unsigned char *site = image.base + load;
    site[0] = 0xe9;
    std::memcpy(site + 1, &site_rel, 4);
    site[5] = 0x90;
    site[6] = 0x90;
    log("projectile bullet-settings fallback installed (Init 0xb057b8)");
    return true;
}
bool install_bullet_settings() {
    static const unsigned char prologue[] = {0x55, 0x48, 0x89, 0xe5, 0x8b, 0xbf};
    if (!image.matches(fBulletSettings, prologue, sizeof prologue)) {
        log("GetBulletSettings (0x%llx): bytes differ, left alone", (unsigned long long)fBulletSettings);
        return false;
    }
    return replace_function(fBulletSettings, 6, reinterpret_cast<void *>(get_bullet_settings),
                            "GetBulletSettings");
}
}  // namespace
void install_tier2(int &installed) {
    installed += patch_unknown_event();
    installed += patch_coop_points();
    installed += install_twin_lookup();
    installed += install_bullet_settings();
    installed += install_projectile_fallback();
}

bool tier2_patch_weapon_damage() { return patch_sp_weapon_damage(); }
}  // namespace u2mp
