// S01 part A: small matchmaking rules (party count, map vote winner, difficulty vote per game type, vote block open,
// Community buttons of the menu's catch-all). Written from spec S01_matchmaking_leftovers.md, sections 2, 3 and 4.1-4.5.

// ---- functions of other groups and of the C part (spec 2.2)
extern "C" int u3x_type_is_coop(uintptr_t base, int32_t type);
extern "C" void u3x_cinema(uintptr_t base, void *s);
extern "C" int u3h_show(uintptr_t base, void *s, uint32_t page, int silent);
extern "C" volatile uint32_t *u3h_m02_diag(void);

namespace s01a {

constexpr uintptr_t kSessionRoom = 0x18;     // u64: room id of the session (0 = none)
constexpr uintptr_t kSlotsStart = 0x680;     // first member slot
constexpr uintptr_t kSlotSize = 0x120;
constexpr int kSlotCount = 10;
constexpr uintptr_t kSlotInUse = 0x25;       // u8: slot holds a member
constexpr uintptr_t kSlotSecond = 0x59;      // u8: member has a second local player

constexpr uint32_t kTypeNoVote = 18;         // co-op type that PS3 1.19 leaves without a difficulty vote

constexpr uint32_t kM02Off = 0x800;          // eb[2] bit 11: Community buttons not handled
constexpr uint32_t kButtonCinema = 0xb85cb12au;
constexpr uint32_t kButtonLeaderboard = 0xfb59d351u;
constexpr uint32_t kPageLeaderboard = 0x8236d02au;

}  // namespace s01a

// 4.1: players in a session; a member with a second local player counts twice
extern "C" U3X_FN int u3x_party_players(const uint8_t *session) {
    using namespace s01a;
    if (rd<uint64_t>(session, kSessionRoom) == 0) return 0;
    int players = 0;
    for (int i = 0; i < kSlotCount; ++i) {
        const uint8_t *slot = session + kSlotsStart + static_cast<uintptr_t>(i) * kSlotSize;
        if (rd<uint8_t>(slot, kSlotInUse) == 0) continue;
        players += 1;
        if (rd<uint8_t>(slot, kSlotSecond) != 0) players += 1;
    }
    return players;
}

// 4.2: the second candidate wins only with strictly more votes
extern "C" U3X_FN int32_t u3x_vote_winner(const uint8_t *vote) {
    if (rd<int32_t>(vote, 0x14) > rd<int32_t>(vote, 0x10)) return rd<int32_t>(vote, 0xc);
    return rd<int32_t>(vote, 8);
}

// 4.3: a difficulty vote for the game's co-op types, except type 18
extern "C" U3X_FN int u3x_difficulty_vote_for(uintptr_t base, uint32_t game_type) {
    if (game_type == s01a::kTypeNoVote) return 0;
    return u3x_type_is_coop(base, static_cast<int32_t>(game_type)) == 1 ? 1 : 0;
}

// 4.4: open a vote block: no own choice, not closed, open, choices and counts cleared (in this order)
extern "C" U3X_FN void u3x_vote_open(uint8_t *vote) {
    *reinterpret_cast<int32_t *>(vote + 4) = -1;
    vote[1] = 0;
    vote[0] = 1;
    *reinterpret_cast<int32_t *>(vote + 8) = 0;
    *reinterpret_cast<int32_t *>(vote + 0xc) = 0;
    *reinterpret_cast<int32_t *>(vote + 0x10) = 0;
    *reinterpret_cast<int32_t *>(vote + 0x14) = 0;
}

// 4.5: the Community buttons (Cinema, Leaderboards) as PS3 1.19 handles them; 1 = handled
extern "C" U3X_FN int u3x_m02_catch(uintptr_t base, void *s, uint32_t id) {
    using namespace s01a;
    if (ext_eb()[2] & kM02Off) return 0;
    if (id == kButtonCinema) {
        u3h_m02_diag()[0] += 1;
        u3x_cinema(base, s);
        return 1;
    }
    if (id == kButtonLeaderboard) {
        u3h_m02_diag()[1] += 1;
        u3h_show(base, s, kPageLeaderboard, 0);
        return 1;
    }
    return 0;
}
