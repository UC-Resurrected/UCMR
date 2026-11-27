// u2mp presence block tests (gow3_pc/src/u2mp/presence_block.h). Build: ninja -C gow3_pc/out/gpu u2mp-presence-test
// The PS3 88-byte block (spec parity-g-friends.md 1.2): field offsets and big-endian byte order, the exact-size
// rule, the old PC "U2MP" data of 0.10.172 and older read with the PS3 meaning, the invite head and the PC join hint.
#include "../src/u2mp/presence_block.h"

#include <cstdio>
#include <cstring>

namespace pres = u2mp::presence;

static int failures = 0;

static void check(bool ok, const char *what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

static uint32_t be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

int main() {
    pres::Block b;
    b.key = 0x0009004e;
    b.activity = pres::kActMatch;
    b.party = 3;
    b.players = 8;
    b.party_hidden = 1;
    b.mode = 2;
    b.score[0] = 0x1234;
    b.score[1] = 7;
    b.net_game = 17;
    b.difficulty = -1;
    b.match_end = 0x0000000165432100ull;
    b.party_room = 0x1122334455667788ull;
    b.cash = 100000, b.cash_left = 2500, b.kills = 11, b.deaths = 12, b.wins = 13, b.losses = 14, b.headshots = 15;
    b.time_a = 3600, b.time_b = 60, b.extra = 9;
    uint8_t d[pres::kBlockSize];
    std::memset(d, 0xcc, sizeof d);
    pres::encode(b, d);

    // Layout: offsets and big-endian words, as the PS3 writes them.
    static const uint8_t head[] = {0x00, 0x09, 0x00, 0x4e, 1, 3, 8, 1, 2, 0, 0, 0};
    check(std::memcmp(d, head, sizeof head) == 0, "key, activity, party, players, hidden, mode, zero padding");
    check(be32(d + 0x0c) == 0x1234 && be32(d + 0x10) == 7, "team scores as u32 at +0x0c / +0x10");
    check(be32(d + 0x14) == 17 && be32(d + 0x18) == 0xffffffffu, "net game +0x14, difficulty -1 at +0x18");
    check(be32(d + 0x1c) == 0, "padding +0x1c is 0");
    check(be32(d + 0x20) == 0x00000001 && be32(d + 0x24) == 0x65432100, "match end u64 at +0x20");
    check(be32(d + 0x28) == 0x11223344 && be32(d + 0x2c) == 0x55667788, "party room u64 at +0x28");
    check(be32(d + 0x30) == 100000 && be32(d + 0x34) == 2500 && be32(d + 0x38) == 11 && be32(d + 0x3c) == 12,
          "cash, cash left, kills, deaths");
    check(be32(d + 0x40) == 13 && be32(d + 0x44) == 14 && be32(d + 0x48) == 15, "wins, losses, headshots");
    check(be32(d + 0x4c) == 3600 && be32(d + 0x50) == 60 && be32(d + 0x54) == 9, "two times played, last counter");

    pres::Block r;
    check(pres::decode(d, sizeof d, r), "an 88-byte block is read");
    check(r.key == b.key && r.activity == b.activity && r.party == 3 && r.players == 8 && r.party_hidden == 1 &&
              r.mode == 2 && r.score[0] == 0x1234 && r.score[1] == 7 && r.net_game == 17 && r.difficulty == -1 &&
              r.match_end == b.match_end && r.party_room == b.party_room && r.cash == 100000 &&
              r.cash_left == 2500 && r.kills == 11 && r.deaths == 12 && r.wins == 13 && r.losses == 14 &&
              r.headshots == 15 && r.time_a == 3600 && r.time_b == 60 && r.extra == 9,
          "every field round-trips");
    check(!pres::decode(d, 87, r) && !pres::decode(d, 89, r) && !pres::decode(nullptr, 88, r),
          "only exactly 88 bytes are a block (PS3 0x48f020)");

    // Defaults: lobby, no net game, alone.
    pres::Block lobby;
    lobby.key = 0x0009004e;
    pres::encode(lobby, d);
    check(d[4] == 0 && d[5] == 1 && be32(d + 0x14) == 0xffffffffu && be32(d + 0x18) == 0xffffffffu &&
              be32(d + 0x20) == 0 && be32(d + 0x24) == 0,
          "lobby defaults: activity 0, party 1, net game -1, difficulty -1, no end");

    // Old PC data (little-endian "U2MP"): version 3 in a custom game lobby.
    pres::Legacy old{};
    const uint8_t magic[4] = {'U', '2', 'M', 'P'};
    std::memcpy(old.magic, magic, 4);
    old.version = 3, old.state = pres::kOldCustom, old.members = 2, old.max = 10;
    const uint8_t key[4] = {0x4e, 0x00, 0x09, 0x00};
    std::memcpy(old.key, key, 4);
    old.kind[0] = 2;
    old.room[0] = 0x21, old.room[1] = 0x43;
    old.party_room[0] = 0x99;
    old.party = 2, old.difficulty = 1, old.net_game[0] = 5;
    old.cash[0] = 0x10, old.kills[0] = 3, old.seconds[1] = 1;
    bool details = false;
    pres::OldRoom join;
    const uint8_t *raw = reinterpret_cast<const uint8_t *>(&old);
    check(pres::decode_legacy(raw, sizeof old, r, join, details) && details, "old version 3 data is read");
    check(r.key == 0x0009004e && r.activity == pres::kActLobby && r.mode == 2 && r.party == 2 && r.difficulty == 1 &&
              r.net_game == 5 && r.party_room == 0x99 && r.cash == 0x10 && r.kills == 3 && r.time_a == 256,
          "old data with the PS3 meaning (custom lobby = activity 0, mode 2)");
    check(join.room == 0x4321 && join.kind == 2 && join.members == 2 && join.max == 10 && join.version == 3,
          "the old data's joinable room");
    old.state = pres::kOldMenu;
    check(!pres::decode_legacy(raw, sizeof old, r, join, details), "old state menu = not in multiplayer");
    old.state = pres::kOldMatch;
    check(pres::decode_legacy(raw, sizeof old, r, join, details) && r.activity == pres::kActMatch, "old match");
    check(!pres::decode_legacy(raw, 79, r, join, details), "old version 3 needs 80 bytes");
    old.version = 1, old.state = pres::kOldMenu;
    check(pres::decode_legacy(raw, 32, r, join, details) && !details && r.net_game == -1,
          "old version 1: in multiplayer, nothing more known");
    check(!pres::decode_legacy(raw, 31, r, join, details), "under 32 bytes: no old data");
    check(!pres::decode_legacy(d, sizeof d, r, join, details), "a PS3 block is not old data");

    // Invite head (32 bytes, the old version 1 layout older builds read).
    pres::OldRoom inv;
    inv.key = 0x0009004e, inv.kind = 1, inv.state = pres::kOldParty, inv.members = 4, inv.max = 10, inv.room = 0xabcdef;
    uint8_t m[pres::kLegacyV1Size];
    check(pres::encode_old_room(inv, m) == 32 && std::memcmp(m, magic, 4) == 0 && m[4] == 1, "invite head");
    pres::OldRoom back;
    check(pres::decode_old_room(m, sizeof m, back) && back.room == 0xabcdef && back.kind == 1 && back.members == 4 &&
              back.max == 10 && back.key == 0x0009004e && back.state == pres::kOldParty,
          "invite head round-trips");

    // PS3 invite attachment (16 bytes: key, unused, party room; big-endian).
    uint8_t p3[pres::kPs3InviteSize];
    check(pres::encode_ps3_invite(0x0009004e, 0x0102030405060708ull, p3) == 16 && be32(p3) == 0x0009004e &&
              be32(p3 + 4) == 0 && be32(p3 + 8) == 0x01020304 && be32(p3 + 12) == 0x05060708,
          "PS3 invite layout");
    check(pres::decode_ps3_invite(p3, 16, back) && back.key == 0x0009004e && back.room == 0x0102030405060708ull &&
              back.kind == 1,
          "PS3 invite read: a party room");
    check(!pres::decode_ps3_invite(p3, 15, back) && !pres::decode_ps3_invite(m, sizeof m, back) &&
              !pres::decode_old_room(p3, 16, back),
          "the PS3 form and the PC head are never taken for each other");

    // PC join hint (presence comment).
    const std::string h = pres::join_hint(0x1a2b3c4d5e6full, 2, 3, 10);
    check(h == "u2mp-join 1a2b3c4d5e6f 2 3 10", "join hint text");
    check(pres::parse_join_hint(h.c_str(), 64, back) && back.room == 0x1a2b3c4d5e6full && back.kind == 2 &&
              back.members == 3 && back.max == 10 && back.version == 0,
          "join hint parsed");
    check(pres::parse_join_hint(pres::join_hint(0, 0, 0, 0).c_str(), 64, back) && back.room == 0,
          "a PC with nothing to join still sends a hint");
    check(!pres::parse_join_hint("", 64, back) && !pres::parse_join_hint("hello", 64, back) &&
              !pres::parse_join_hint(nullptr, 64, back),
          "an empty or other comment is no hint (PS3 friends)");

    if (failures) return 1;
    std::printf("u2mp presence: all tests passed\n");
    return 0;
}
