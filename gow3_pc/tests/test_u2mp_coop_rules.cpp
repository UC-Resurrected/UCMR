// u2mp co-op character test (no game needed): ninja -C out/gpu u2mp-coop-rules-test
// coop_rules.h: in a co-op net game each co-op rank plays the record's preset character (spec unlocks.md 5.8a); the
// three maps' lists are the PS4 net.bin *net-games* records 113..115 (+0x70).
#include <cstdint>
#include <cstdio>
#include <initializer_list>

#include "../src/u2mp/coop_rules.h"

using namespace u2mp;

static int failures = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            ++failures;                                                  \
        }                                                                \
    } while (0)

// The game's string id: CRC-32 of the name, MSB first, polynomial 0x04C11DB7, no init, no final xor.
static constexpr uint32_t sid(const char *s, uint32_t crc = 0) {
    for (; *s; ++s) {
        crc ^= uint32_t(uint8_t(*s)) << 24;
        for (int i = 0; i < 8; ++i) crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
    }
    return crc;
}

// The record lists as the game data has them (ids), checked against the skin package names.
static const uint32_t kNepal[3] = {0xb095d270u, 0x0981d712u, 0x25b427e5u};
static const uint32_t kVillage[3] = {0x357f631bu, 0x8b78521eu, 0x5bc011a5u};
static const uint32_t kSanctuary[3] = {0x357f631bu, 0x7fe83011u, 0x8b78521eu};

static void test_game_data_names() {
    CHECK(kNepal[0] == sid("mp-hero") && kNepal[1] == sid("mp-hero-chloe") && kNepal[2] == sid("mp-hero-sullivan"));
    CHECK(kVillage[0] == sid("mp-hero-parka") && kVillage[1] == sid("mp-hero-elena-winter") &&
          kVillage[2] == sid("mp-hero-rescuer"));
    CHECK(kSanctuary[0] == sid("mp-hero-parka") && kSanctuary[1] == sid("mp-hero-chloe-winter") &&
          kSanctuary[2] == sid("mp-hero-elena-winter"));
}

static void test_rank_picks_preset() {
    for (const int32_t type : {4, 8, 9, 12}) {
        for (int r = 0; r < coop::kRanks; ++r) {
            CHECK(coop::preset_id(type, kNepal, r) == kNepal[r]);
            CHECK(coop::preset_id(type, kVillage, r) == kVillage[r]);
            CHECK(coop::preset_id(type, kSanctuary, r) == kSanctuary[r]);
        }
    }
}

static void test_no_preset() {
    // Competitive game types keep the player's own skin.
    for (const int32_t type : {0, 1, 2, 3, 5, 6, 7, 10, 11}) CHECK(coop::preset_id(type, kVillage, 0) == 0);
    // No list, or a rank that is not 0..2.
    CHECK(coop::preset_id(4, nullptr, 0) == 0);
    CHECK(coop::preset_id(4, kVillage, -1) == 0);
    CHECK(coop::preset_id(4, kVillage, 3) == 0);
}

int main() {
    test_game_data_names();
    test_rank_picks_preset();
    test_no_preset();
    if (failures) {
        std::printf("u2mp coop rules: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("u2mp coop rules: all passed\n");
    return 0;
}
