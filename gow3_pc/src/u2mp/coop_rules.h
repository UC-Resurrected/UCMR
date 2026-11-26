// u2mp co-op characters: which preset character a co-op player plays, kept game-free so
// tests/test_u2mp_coop_rules.cpp can check it (spec unlocks.md 5.8a).
//
// Each co-op net game (*net-games* record of a co-op game type) names three skin ids at record +0x70: the characters
// of co-op ranks 0, 1 and 2 (tracker +0x198, the rank that also picks the start spawner and net-player-coop-id). On
// PS3 every co-op player plays the character of their rank, whatever skin they equipped for competitive play.
#pragma once
#include <cstdint>

#include "start_rules.h"

namespace u2mp::coop {

constexpr int kRanks = 3;

// The skin id co-op rank `rank` plays in a net game of `type` whose record lists `chars` (kRanks ids, may be NULL);
// 0 when the game is not co-op, there is no list or the rank is not 0..2 (the player keeps their own skin).
constexpr uint32_t preset_id(int32_t type, const uint32_t *chars, int32_t rank) {
    return start::coop_type(type) && chars && rank >= 0 && rank < kRanks ? chars[rank] : 0;
}

}  // namespace u2mp::coop
