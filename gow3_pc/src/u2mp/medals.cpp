// u2mp medals: the results page falls back to the pop-up's default medal for medals whose
// picture the Collection's net.pak lacks.
#include <cstdint>
#include <cstring>

#include "frontend3.h"
#include "u2mp_internal.h"

namespace u2mp {
void install_medals(int &installed);

namespace {

constexpr uint64_t kIconLookup = 0x555140;
constexpr uint64_t kResultsLookup = 0x590561;
constexpr uint64_t kMedalElement = 0x1130008 + 0x310;

struct MedalIcon {
    uint64_t sid;
    uint64_t table;   // the image table 0x555140 looked in (1 = net), not a material index
};

using LookupFn = MedalIcon(U2MP_GUEST *)(const char *, int);

U2MP_GUEST MedalIcon medal_icon_trace(const char *name, int table) {
    MedalIcon icon = reinterpret_cast<LookupFn>(image.base + kIconLookup)(name, table);
    const bool medal_icon = name && std::strstr(name, "medal-icons");
    const int64_t old_table = int64_t(icon.table);
    if (name && (std::strstr(name, "medal") || std::strstr(name, "Medal"))) {
        static int logged = 0;
        if (logged < 128) {
            log("medal icon trace: %s table %d -> sid %08x table %d", name, table, uint32_t(icon.sid), int(icon.table));
            ++logged;
        }
    }
    if (!medal_icon) return icon;

    static MedalIcon last_good;
    if (uint32_t(icon.sid) && menu_net_material(uint32_t(icon.sid))) {
        last_good = icon;
        return icon;
    }

    icon.table = uint64_t(-1);   // the HUD table: the default medal lives there
    const uint64_t medal = *image.at<uint64_t>(kMedalElement);
    MedalIcon fallback{};
    if (medal) {
        fallback.sid = *reinterpret_cast<const uint64_t *>(medal + 0x4c);
        fallback.table = uint64_t(-1);
    }
    if (!uint32_t(fallback.sid) || !menu_net_material(uint32_t(fallback.sid))) {
        for (const char *known : {"general/net/medal-icons/no-death", "general/net/medal-icons/streak",
                                  "general/net/medal-icons/commando"}) {
            MedalIcon good = reinterpret_cast<LookupFn>(image.base + kIconLookup)(known, 1);
            if (uint32_t(good.sid) && menu_net_material(uint32_t(good.sid))) {
                fallback = good;
                break;
            }
        }
    }
    if (!uint32_t(fallback.sid) || !menu_net_material(uint32_t(fallback.sid))) fallback = last_good;
    if (!uint32_t(fallback.sid)) return icon;
    static int logged = 0;
    if (logged < 16) {
        ++logged;
        log("medals: %s has no icon (sid %08x table %d), using sid %08x", name, uint32_t(icon.sid), int(old_table),
            uint32_t(fallback.sid));
    }
    return fallback;
}

int trace_icon_lookups() {
    int traced = 0;
    for (uint64_t i = 0; i + 5 <= image.size; ++i) {
        if (image.base[i] != 0xe8) continue;
        int32_t rel = 0;
        std::memcpy(&rel, image.base + i + 1, 4);
        if (int64_t(i) + 5 + rel != int64_t(kIconLookup)) continue;
        if (retarget_call(i, kIconLookup, reinterpret_cast<void *>(&medal_icon_trace), "medal icon trace"))
            ++traced;
    }
    return traced;
}

}  // namespace

void install_medals(int &installed) {
    static bool logged = false;
    if (retarget_call(kResultsLookup, kIconLookup, reinterpret_cast<void *>(&medal_icon_trace),
                      "medal results icon")) {
        ++installed;
        if (!logged) {
            logged = true;
            log("medals: results page uses the pop-up's default medal for missing icons");
        }
    }
    const int traced = trace_icon_lookups();
    if (traced) {
        ++installed;
        log("medals: icon trace on %d call sites", traced);
    }
}

}  // namespace u2mp
