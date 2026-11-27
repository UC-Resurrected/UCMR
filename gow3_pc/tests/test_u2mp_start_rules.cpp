// u2mp start rules test (no game needed): ninja -C out/gpu u2mp-start-rules-test
// Owner rule 2026-11-25 on the pure rules of start_rules.h: custom games start with 1 player in every game mode,
// matchmaking needs 2 (co-op 1); a solo custom Elimination is not ended for having one team.
#include <cstdint>
#include <cstdio>

#include "../src/u2mp/start_rules.h"

using namespace u2mp::start;

static int failures = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            ++failures;                                                  \
        }                                                                \
    } while (0)

static const int32_t kCompetitive[] = {0, 1, 2, 3, 5, 6, 7, 10, 11};
static const int32_t kCoop[] = {4, 8, 9, 12};

static void test_custom_any_mode_solo() {
    for (const int32_t t : kCompetitive) {
        CHECK(min_players(kCustom, t) == 1);
        CHECK(may_start(kCustom, t, 1));
        CHECK(may_start(kCustom, t, 10));
    }
    for (const int32_t t : kCoop) CHECK(may_start(kCustom, t, 1));
}

static void test_matchmaking_minimum() {
    for (const int32_t t : kCompetitive) {
        CHECK(min_players(kMatchmaking, t) == 2);
        CHECK(!may_start(kMatchmaking, t, 1));
        CHECK(may_start(kMatchmaking, t, 2));
    }
    for (const int32_t t : kCoop) {
        CHECK(min_players(kMatchmaking, t) == 1);
        CHECK(may_start(kMatchmaking, t, 1));
    }
}

static void test_playlist_minimum() {
    const int32_t coop_only[] = {4, 8, 9, 12};
    const int32_t mixed[] = {4, 0};
    const int32_t elimination[] = {10};
    CHECK(playlist_min_players(coop_only, 4) == 1);
    CHECK(playlist_min_players(mixed, 2) == 2);
    CHECK(playlist_min_players(elimination, 1) == 2);
    CHECK(playlist_min_players(nullptr, 0) == 2);   // unknown playlist
}

static void test_one_team_refusal() {
    CHECK(!one_team_refused(10, 1, 1, 1, 0));   // solo Elimination starts
    CHECK(!one_team_refused(10, 1, 1, 0, 1));
    CHECK(!one_team_refused(10, 1, 0, 0, 0));   // no room yet: alone
    CHECK(one_team_refused(10, 2, 2, 0, 2));    // two players, both on team 2 (PS3 box)
    CHECK(one_team_refused(10, 3, 3, 3, 0));
    CHECK(!one_team_refused(10, 2, 2, 1, 1));
    CHECK(!one_team_refused(0, 2, 2, 2, 0));    // other types never
}

static void test_in_match() {
    CHECK(solo_custom(kCustom, 1));
    CHECK(solo_custom(kCustom, 0));
    CHECK(!solo_custom(kCustom, 2));
    CHECK(!solo_custom(kMatchmaking, 1));
    // Game over for one team: Elimination only, never in a solo custom game.
    CHECK(!one_team_ends_game(kCustom, 10, 1));
    CHECK(one_team_ends_game(kCustom, 10, 2));
    CHECK(one_team_ends_game(kMatchmaking, 10, 1));
    CHECK(!one_team_ends_game(kCustom, 0, 2));
    CHECK(!one_team_ends_game(kMatchmaking, 8, 1));   // co-op reaches the same test in matchmaking
    // Round with one team alive: solo custom runs to the time limit while its player lives.
    CHECK(round_runs_to_time(kCustom, 1, 0));
    CHECK(round_runs_to_time(kCustom, 1, 1));
    CHECK(!round_runs_to_time(kCustom, 1, -1));
    CHECK(!round_runs_to_time(kCustom, 2, 0));
    CHECK(!round_runs_to_time(kMatchmaking, 1, 0));
}

// Map pack maps: only when every PC has the pack; the Ruins (base version without map pack 1) also when none has.
static void test_map_packs() {
    const uint32_t base = 1, p1 = 1 | 2, p2 = 1 | 4, both = 1 | 2 | 4;
    CHECK(pack_map_same_for_all(0, false, base, both));     // base maps always
    CHECK(pack_map_same_for_all(-1, false, base, base));    // unknown map: the game decides
    CHECK(pack_map_same_for_all(1, false, p1, p1));         // Facility, everyone has map pack 1
    CHECK(!pack_map_same_for_all(1, false, base, p1));      // Facility, one PC lacks it
    CHECK(!pack_map_same_for_all(1, false, base, base));    // Facility, nobody has it
    CHECK(pack_map_same_for_all(2, false, both & p2, both)); // Highrise, everyone has map pack 2
    CHECK(!pack_map_same_for_all(2, false, p1, both));      // Highrise, a PC with only map pack 1
    CHECK(pack_map_same_for_all(1, true, p1, both));        // Ruins: all map pack 1 -> map pack version everywhere
    CHECK(pack_map_same_for_all(1, true, base, base));      // Ruins: nobody has it -> base version everywhere
    CHECK(!pack_map_same_for_all(1, true, base, p1));       // Ruins, mixed: host or member without the pack
    CHECK(!pack_map_same_for_all(1, true, base, p2 | p1));
}

int main() {
    test_map_packs();
    test_custom_any_mode_solo();
    test_matchmaking_minimum();
    test_playlist_minimum();
    test_one_team_refusal();
    test_in_match();
    if (failures) {
        std::printf("u2mp start rules: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("u2mp start rules: all passed\n");
    return 0;
}
