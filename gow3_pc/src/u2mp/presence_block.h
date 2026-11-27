// u2mp presence data: the 88-byte block PS3 1.10 sends to its friends (spec parity-g-friends.md 1.2), kept
// game-free so tests/test_u2mp_presence.cpp can check it.
//
// Every player of this game (PS3 in RPCS3 and PC) sends the same block with the same field order and byte order
// (big-endian, as the PS3 writes it), so a PS3 friend and a PC friend read each other the same way. A reader takes a
// block only when it is exactly 88 bytes long and its first word is its own build key.
//
// PC builds up to 0.10.172 sent their own "U2MP" data (version 1: 32 bytes, 2: 48, 3: 80, little-endian); it is
// still read (decode_legacy) so those friends keep showing while they update. Invites still carry that layout.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace u2mp::presence {

constexpr size_t kBlockSize = 88;

// What the player does (block +0x04): 0 in multiplayer, no net game running (party, lobby, matchmaking, menus);
// 1 a net game is running; 2 a film (cinema) is being played back.
enum : uint8_t { kActLobby = 0, kActMatch = 1, kActFilm = 2 };

struct Block {
    uint32_t key = 0;          // +0x00 build key (the 0x4C room key: netInfo +0)
    uint8_t activity = kActLobby;  // +0x04
    uint8_t party = 1;         // +0x05 players in the party room (1 alone)
    uint8_t players = 0;       // +0x06 players in the running net game (activity 1), else 0
    uint8_t party_hidden = 0;  // +0x07 the party room is hidden: friends are offered no Join Party
    uint8_t mode = 0;          // +0x08 the multiplayer menu mode (netInfo +0x2c4: 1 matchmaking, 2 custom, 3 cinema,
                               //       4 leaderboards): the friends list row icon
    uint16_t score[2] = {0, 0};    // +0x0c / +0x10 the two team scores (u32 each; activity 1), else 0
    int32_t net_game = -1;     // +0x14 *net-games* index of the game played (activity 1 / 2), else -1
    int32_t difficulty = -1;   // +0x18 co-op difficulty 0..4 (activity 1 / 2), else -1
    uint64_t match_end = 0;    // +0x20 the match's end, Unix UTC seconds; 0 none (no time limit, co-op, not playing)
    uint64_t party_room = 0;   // +0x28 the party room id, 0 none
    // +0x30.. the career values (NetPlayerData): cash earned, cash left (earned minus spent, at least 0), kills,
    // deaths, wins, losses, headshots, two times played in seconds (the ticker shows their sum), one more counter.
    uint32_t cash = 0, cash_left = 0, kills = 0, deaths = 0, wins = 0, losses = 0, headshots = 0, time_a = 0,
             time_b = 0, extra = 0;
};

namespace detail {
inline void put32(uint8_t *p, uint32_t v) {
    p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}
inline void put64(uint8_t *p, uint64_t v) {
    put32(p, uint32_t(v >> 32));
    put32(p + 4, uint32_t(v));
}
inline uint32_t get32(const uint8_t *p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}
inline uint64_t get64(const uint8_t *p) { return uint64_t(get32(p)) << 32 | get32(p + 4); }
}  // namespace detail

// The block as it goes out (unused bytes +0x09..0x0b and +0x1c..0x1f are 0).
inline void encode(const Block &b, uint8_t out[kBlockSize]) {
    using namespace detail;
    std::memset(out, 0, kBlockSize);
    put32(out + 0x00, b.key);
    out[0x04] = b.activity;
    out[0x05] = b.party;
    out[0x06] = b.players;
    out[0x07] = b.party_hidden;
    out[0x08] = b.mode;
    put32(out + 0x0c, b.score[0]);
    put32(out + 0x10, b.score[1]);
    put32(out + 0x14, uint32_t(b.net_game));
    put32(out + 0x18, uint32_t(b.difficulty));
    put64(out + 0x20, b.match_end);
    put64(out + 0x28, b.party_room);
    const uint32_t career[10] = {b.cash, b.cash_left, b.kills,  b.deaths, b.wins,
                                 b.losses, b.headshots, b.time_a, b.time_b, b.extra};
    for (int i = 0; i < 10; ++i) put32(out + 0x30 + 4 * i, career[i]);
}

// A received block: false unless it is exactly 88 bytes (the key is the caller's check).
inline bool decode(const uint8_t *data, size_t len, Block &b) {
    using namespace detail;
    b = Block{};
    if (!data || len != kBlockSize) return false;
    b.key = get32(data);
    b.activity = data[0x04];
    b.party = data[0x05];
    b.players = data[0x06];
    b.party_hidden = data[0x07];
    b.mode = data[0x08];
    b.score[0] = uint16_t(get32(data + 0x0c));
    b.score[1] = uint16_t(get32(data + 0x10));
    b.net_game = int32_t(get32(data + 0x14));
    b.difficulty = int32_t(get32(data + 0x18));
    b.match_end = get64(data + 0x20);
    b.party_room = get64(data + 0x28);
    uint32_t *career[10] = {&b.cash, &b.cash_left, &b.kills,  &b.deaths, &b.wins,
                            &b.losses, &b.headshots, &b.time_a, &b.time_b, &b.extra};
    for (int i = 0; i < 10; ++i) *career[i] = get32(data + 0x30 + 4 * i);
    return true;
}

// ---- PC data of 0.10.172 and older ("U2MP", little-endian) --------------------------------------
constexpr uint32_t kLegacyMagic = 0x504d3255u;   // "U2MP"
constexpr uint32_t kLegacyV1Size = 32, kLegacyV2Size = 48, kLegacyV3Size = 80;
// States of the old data: kOldMenu = this game, not in multiplayer.
enum : uint8_t { kOldMenu = 0, kOldParty = 1, kOldCustom = 2, kOldMatchmaking = 3, kOldMatch = 4, kOldCinema = 5 };
struct Legacy {
    uint8_t magic[4];
    uint8_t version, state, members, max;
    uint8_t key[4];
    uint8_t kind[4];
    uint8_t room[8];        // the room a friend joins (0 = not joinable now)
    uint8_t party_room[8];  // the party room (0 none)
    // version 2
    uint8_t level, party, difficulty, pad2;
    uint8_t net_game[2];
    uint8_t pad3[2];
    uint8_t match_end[8];
    // version 3
    uint8_t cash[4], kills[4], wins[4], losses[4], deaths[4], headshots[4], seconds[4];
    uint8_t pad4[4];
};
static_assert(sizeof(Legacy) == kLegacyV3Size, "old presence data");

namespace detail {
inline uint32_t le32(const uint8_t *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline uint64_t le64(const uint8_t *p) { return uint64_t(le32(p)) | uint64_t(le32(p + 4)) << 32; }
inline void putle32(uint8_t *p, uint32_t v) {
    p[0] = uint8_t(v), p[1] = uint8_t(v >> 8), p[2] = uint8_t(v >> 16), p[3] = uint8_t(v >> 24);
}
inline void putle64(uint8_t *p, uint64_t v) {
    putle32(p, uint32_t(v));
    putle32(p + 4, uint32_t(v >> 32));
}
}  // namespace detail

// The room an old PC (or an invite) names.
struct OldRoom {
    uint32_t key = 0, kind = 0;
    uint8_t state = 0, members = 0, max = 0, version = 0;
    uint64_t room = 0;
};

// The head of the old data (an invite, or old presence): false without the magic or under 32 bytes.
inline bool decode_old_room(const uint8_t *data, size_t len, OldRoom &r) {
    r = OldRoom{};
    if (!data || len < kLegacyV1Size || detail::le32(data) != kLegacyMagic || data[4] < 1) return false;
    r.version = data[4];
    r.state = data[5];
    r.members = data[6];
    r.max = data[7];
    r.key = detail::le32(data + 8);
    r.kind = detail::le32(data + 12);
    r.room = detail::le64(data + 16);
    return true;
}

// The old head for an invite attachment (32 bytes, version 1 layout; older builds read it as before).
inline size_t encode_old_room(const OldRoom &r, uint8_t out[kLegacyV1Size]) {
    std::memset(out, 0, kLegacyV1Size);
    detail::putle32(out, kLegacyMagic);
    out[4] = 1;
    out[5] = r.state;
    out[6] = r.members;
    out[7] = r.max;
    detail::putle32(out + 8, r.key);
    detail::putle32(out + 12, r.kind);
    detail::putle64(out + 16, r.room);
    return kLegacyV1Size;
}

// ---- PS3 invite attachment (PS3 0x43a164: message main type INVITE, 16 bytes) ----------------------
// +0 u32 build key, +4 unused (not cleared on PS3), +8 u64 party room id (0 when the sender has none); big-endian.
constexpr size_t kPs3InviteSize = 16;
inline size_t encode_ps3_invite(uint32_t key, uint64_t party_room, uint8_t out[kPs3InviteSize]) {
    std::memset(out, 0, kPs3InviteSize);
    detail::put32(out, key);
    detail::put64(out + 8, party_room);
    return kPs3InviteSize;
}
// False unless it is exactly 16 bytes (an old PC head is 32 or more and starts with "U2MP").
inline bool decode_ps3_invite(const uint8_t *data, size_t len, OldRoom &r) {
    r = OldRoom{};
    if (!data || len != kPs3InviteSize) return false;
    r.key = detail::get32(data);
    r.room = detail::get64(data + 8);
    r.kind = 1;   // a party room
    return true;
}

// Old presence data as a block with the PS3 meaning. False for "this game, not in multiplayer" (state menu) and
// for data that is not old presence. `details` is false for version 1 (only "in multiplayer" is known).
inline bool decode_legacy(const uint8_t *data, size_t len, Block &b, OldRoom &join, bool &details) {
    using namespace detail;
    b = Block{};
    details = false;
    if (!decode_old_room(data, len, join)) return false;
    const uint8_t v = join.version;
    if ((v == 2 && len < kLegacyV2Size) || (v >= 3 && len < kLegacyV3Size)) return false;
    if (v >= 2 && join.state == kOldMenu) return false;
    b.key = join.key;
    b.party_room = le64(data + 24);
    if (v == 1) return true;
    details = true;
    b.activity = join.state == kOldMatch ? kActMatch : join.state == kOldCinema ? kActFilm : kActLobby;
    b.party = data[33];
    b.difficulty = data[34];
    b.net_game = int16_t(uint16_t(data[36] | data[37] << 8));
    b.match_end = le64(data + 40);
    b.mode = join.state == kOldMatchmaking ? 1 : join.state == kOldCustom ? 2 : join.state == kOldCinema ? 3 : 0;
    if (v >= 3) {
        b.cash = le32(data + 48);
        b.kills = le32(data + 52);
        b.wins = le32(data + 56);
        b.losses = le32(data + 60);
        b.deaths = le32(data + 64);
        b.headshots = le32(data + 68);
        b.time_a = le32(data + 72);
    }
    return true;
}

// ---- PC join hint (presence comment; PS3 never sets or reads the comment) --------------------------
// A PC in a custom game lobby or a matchmaking room has left its party room (online.cpp join_succeeded), so the
// block's party room is 0; the hint names the room a PC friend joins instead: "u2mp-join <room hex> <kind>
// <members> <max>" (room 0: nothing to join). Every PC sends it, so a reader also knows a PC friend by it.
inline std::string join_hint(uint64_t room, uint32_t kind, unsigned members, unsigned max) {
    char s[64];
    std::snprintf(s, sizeof s, "u2mp-join %llx %u %u %u", (unsigned long long)room, unsigned(kind), members, max);
    return s;
}
inline bool parse_join_hint(const char *s, size_t cap, OldRoom &r) {
    r = OldRoom{};
    if (!s) return false;
    const std::string t(s, strnlen(s, cap));
    unsigned long long room = 0;
    unsigned kind = 0, members = 0, max = 0;
    if (std::sscanf(t.c_str(), "u2mp-join %llx %u %u %u", &room, &kind, &members, &max) != 4) return false;
    r.room = room;
    r.kind = kind;
    r.members = uint8_t(std::min(members, 255u));
    r.max = uint8_t(std::min(max, 255u));
    return true;
}

}  // namespace u2mp::presence
