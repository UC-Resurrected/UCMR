// u2mp solo co-op: types 4/8/9/12 require two players in the PS4 build; lower the check to one.
#include <cstring>

#include "u2mp_internal.h"

namespace u2mp {
namespace {

constexpr uint64_t kCoopMinimum = 0x63f60b;
constexpr unsigned char kTwoPlayers[] = {0xb9, 0x02, 0x00, 0x00, 0x00};
constexpr unsigned char kOnePlayer[] = {0xb9, 0x01, 0x00, 0x00, 0x00};

}  // namespace

void install_solocoop(int &installed) {
    if (!image.base || !image.executable(kCoopMinimum, sizeof kTwoPlayers) ||
        !image.matches(kCoopMinimum, kTwoPlayers, sizeof kTwoPlayers)) {
        log("solo co-op: minimum-player check at 0x%llx not found, left alone", (unsigned long long)kCoopMinimum);
        return;
    }
    std::memcpy(image.base + kCoopMinimum, kOnePlayer, sizeof kOnePlayer);
    log("solo co-op: co-op minimum players 2 -> 1");
    installed += 1;
}

}  // namespace u2mp
