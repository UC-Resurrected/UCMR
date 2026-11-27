// u2mp start rules: who may start a match, and which in-match "too few players" ends apply, kept game-free so
// tests/test_u2mp_start_rules.cpp can check them (spec session.md 5.x Start Match, parity-n-inlined.md 4 NI2).
//
// Owner rule (2026-11-25): the minimum players are a matchmaking limit only. Matchmaking needs 2 players in every
// game mode and 1 in co-op (types 4, 8, 9, 12); a custom game starts with 1 player in every game mode.
#pragma once
#include <cstddef>
#include <cstdint>

namespace u2mp::start {

// Page mode (netInfo +0x2c4) that the multiplayer pages set and that stays set during the match.
constexpr int32_t kMatchmaking = 1;
constexpr int32_t kCustom = 2;

// Game type 10 (Elimination): rounds, and a match that ends when only one team is left.
constexpr int32_t kElimination = 10;

constexpr bool coop_type(int32_t type) { return type == 4 || type == 8 || type == 9 || type == 12; }

// Players a match of `type` needs before it may begin in page mode `mode`.
constexpr int min_players(int32_t mode, int32_t type) { return mode == kCustom || coop_type(type) ? 1 : 2; }

constexpr bool may_start(int32_t mode, int32_t type, int players) { return players >= min_players(mode, type); }

// Matchmaking minimum of a playlist: the largest minimum of its game types (2 unless every type is co-op); 2 for a
// playlist with no known types.
inline int playlist_min_players(const int32_t *types, size_t count) {
    int most = count ? 1 : 2;
    for (size_t i = 0; i < count; ++i)
        if (min_players(kMatchmaking, types[i]) > most) most = min_players(kMatchmaking, types[i]);
    return most;
}

// Custom game, Elimination: Start Match refuses "every player is on one team" (PS3) only when there is someone to
// put on the other team. `with_data` = party members with member data, team1 / team2 = those on each team.
constexpr bool one_team_refused(int32_t type, int players, int with_data, int team1, int team2) {
    return type == kElimination && players > 1 && (team1 == with_data || team2 == with_data);
}

// A custom game that began with one player (NetGameManager +0x1b90 = players in the start event).
constexpr bool solo_custom(int32_t mode, int32_t start_players) { return mode == kCustom && start_players <= 1; }

// In-match check "only one team has players left -> game over" (PS4 HostUpdate). `type` is what the PS4 code tests
// there outside matchmaking: only Elimination ends that way; not in a solo custom game, where one team is all there
// is.
constexpr bool one_team_ends_game(int32_t mode, int32_t type, int32_t start_players) {
    return type == kElimination && !solo_custom(mode, start_players);
}

// Elimination round check, when the living players are all on one team (`alive_team`, -1 when nobody is alive):
// true = no instant round end, the round runs to its time limit as when both teams are alive. Only in a solo custom
// game while its player lives; a dead solo player ends the round as before.
constexpr bool round_runs_to_time(int32_t mode, int32_t start_players, int32_t alive_team) {
    return solo_custom(mode, start_players) && alive_team != -1;
}

// Map packs of a net game's map (PS3 GetRandomNetGameIndexPair 0x49bdb8 and Start Match): a map of map pack `pack`
// (1 or 2) is played only when every PC of the session has that pack (`packs`: the session's map-pack bits, 1 << pack,
// ANDed for `all`, ORed for `any`, this PC included). The Ruins is the PC's one exception: the PS4 game ships its base
// version, which a PC without map pack 1 plays (dlc.cpp), so it is played as well when no PC has the pack
// (`has_base`); with some PCs having it and others not they would load different maps. Base maps (pack 0) always.
constexpr bool pack_map_same_for_all(int32_t pack, bool has_base, uint32_t all, uint32_t any) {
    if (pack != 1 && pack != 2) return true;
    const uint32_t bit = 1u << pack;
    return (all & bit) || (has_base && !(any & bit));
}

}  // namespace u2mp::start
