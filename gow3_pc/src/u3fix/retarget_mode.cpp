// Uncharted 3 1.02, 20 Nov 2026: skeleton retargets use their multiplayer settings while a net game runs, as on PS3.
//
// Each entry of the skeleton retarget list (dc1 skel-retargets) carries three switches: active, single player and
// multiplayer. The PS3 game copies the multiplayer switch into the active one for every entry when a multiplayer or
// co-op game starts, and the single-player switch back when the campaign starts. The PS4 game was released without
// multiplayer and never makes the switch, so co-op ran with the campaign's retargets; one of them (Cutter's animations
// on the MP Drake body, off in multiplayer) leaves the rescued Drake of the co-op Monastery in his bind pose.
//
// The game keeps the list's entries in its retarget table (image 0x1c09d00, 0x28 bytes per entry, the first word a
// pointer to the list entry: +0 active, +4 single player, +8 multiplayer; count at 0x1c0a480). A small thread watches
// the net-game flag (image 0x17d5c61). When a net game starts, every entry's active switch takes its multiplayer value;
// when it ends, the values the game had before are put back, so the campaign stays exactly as the PS4 game has it.
#include "u3fix.h"
#include "u3fix_hook.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace u3fix {

namespace {

constexpr uint64_t kTable = 0x1c09d00, kCount = 0x1c0a480, kNetGame = 0x17d5c61;
constexpr uint32_t kMaxEntries = 48;   // the table's size

uint32_t read32(const unsigned char *p) { uint32_t v; std::memcpy(&v, p, 4); return v; }

unsigned char *list_entry(unsigned char *image, uint32_t i) {
    unsigned char *e = nullptr;
    std::memcpy(&e, image + kTable + 0x28ull * i, sizeof e);
    return e;
}

void watch(unsigned char *image) {
    uint32_t saved[kMaxEntries] = {};
    bool multiplayer = false;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const uint32_t count = read32(image + kCount);
        if (count == 0 || count > kMaxEntries) continue;
        const bool net = image[kNetGame] != 0;
        if (net == multiplayer) continue;
        uint32_t active = 0;
        for (uint32_t i = 0; i < count; i++) {
            unsigned char *e = list_entry(image, i);
            if (!e) continue;
            uint32_t v;
            if (net) {
                saved[i] = read32(e);
                v = read32(e + 8);
            } else {
                v = saved[i];
            }
            std::memcpy(e, &v, 4);
            active += v != 0;
        }
        multiplayer = net;
        std::printf("Uncharted 3: skeleton retargets use their %s settings (%u of %u active)\n",
                    net ? "multiplayer" : "original", active, count);
        std::fflush(stdout);
    }
}

}  // namespace

bool install_retarget_mode(unsigned char *image, uint64_t size) {
    if (!image || size <= kCount + 4) return false;
    // only in the build the offsets are for: the retarget list loader, mov edi,0xfe014fd2 at image 0xdcb5ae
    constexpr unsigned char kLoader[] = {0xbf, 0xd2, 0x4f, 0x01, 0xfe, 0xe8};
    if (find_unique(image, size, kLoader, sizeof kLoader) != 0xdcb5ae) return false;
    std::thread(watch, image).detach();
    std::printf("Uncharted 3: skeleton retargets follow the multiplayer settings in net games, as on PS3\n");
    std::fflush(stdout);
    return true;
}

}  // namespace u3fix
