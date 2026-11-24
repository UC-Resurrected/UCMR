// u2mp party roster wire tests (gow3_pc/src/u2mp/roster_wire.h). Build: ninja -C gow3_pc/out/gpu u2mp-roster-test
// A roster written by the host must read back with every member and both skins; a reader that lost the
// second member made the client re-add itself with no skins and resend its info on every roster.
#include "../src/u2mp/roster_wire.h"

#include <cstdio>
#include <string>
#include <vector>

using u2mp::roster_wire::Entry;

static int failures = 0;

static void check(bool ok, const char *what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

static bool same(const Entry &a, const Entry &b) {
    return a.slot == b.slot && a.rank == b.rank && a.name == b.name && a.hero == b.hero && a.villain == b.villain;
}

int main() {
    // Two members, both with hero and villain (a villain name longer than 10 bytes once read as the next slot).
    const std::vector<Entry> two = {{0, 12, "Tha_14", "mp-hero-doughnut", "mp-hero-navarro"},
                                    {1, 79, "jonaschild", "mp-hero-red", "mp-hero-flynn"}};
    std::vector<uint8_t> msg{2};
    for (const Entry &e : two) u2mp::roster_wire::put(msg, e);
    std::vector<Entry> got = u2mp::roster_wire::read(msg.data(), msg.size(), 1, 10);
    check(got.size() == 2, "two members read back");
    for (size_t i = 0; i < got.size() && i < two.size(); ++i) check(same(got[i], two[i]), "member fields read back");

    // Empty skins and a short villain (its length byte must not be taken for a slot).
    const std::vector<Entry> three = {{0, 0, "Host", "", ""}, {3, 5, "B", "mp-hero-x", "v"}, {10, 255, "", "", ""}};
    msg.assign(1, 2);
    for (const Entry &e : three) u2mp::roster_wire::put(msg, e);
    got = u2mp::roster_wire::read(msg.data(), msg.size(), 1, 10);
    check(got.size() == 3, "three members with empty skins read back");
    for (size_t i = 0; i < got.size() && i < three.size(); ++i) check(same(got[i], three[i]), "empty-skin member fields");

    // An older sender: a single entry that ends after the name, or after the hero.
    const uint8_t name_only[] = {2, 0, 5, 7, 'D', 'r', 'a', 'k', 'e'};
    got = u2mp::roster_wire::read(name_only, sizeof name_only, 1, 10);
    check(got.size() == 1 && got[0].name == "Drake" && got[0].rank == 7 && got[0].hero.empty(), "name-only entry");
    const uint8_t with_hero[] = {2, 1, 1, 0, 'A', 2, 'h', 'h'};
    got = u2mp::roster_wire::read(with_hero, sizeof with_hero, 1, 10);
    check(got.size() == 1 && got[0].hero == "hh" && got[0].villain.empty(), "entry ending after the hero");

    // A message cut inside the second member's hero keeps the first member whole, the second with its name only,
    // and stops; a slot above the limit stops too.
    msg.assign(1, 2);
    for (const Entry &e : two) u2mp::roster_wire::put(msg, e);
    std::vector<uint8_t> cut_msg(msg.begin(), msg.end() - 20);
    got = u2mp::roster_wire::read(cut_msg.data(), cut_msg.size(), 1, 10);
    check(got.size() == 2 && same(got[0], two[0]) && got[1].name == "jonaschild" && got[1].hero.empty(),
          "cut message keeps the complete parts");
    const uint8_t bad_slot[] = {2, 11, 0, 0, 0, 0};
    check(u2mp::roster_wire::read(bad_slot, sizeof bad_slot, 1, 10).empty(), "slot above the limit is not read");

    if (failures == 0) std::printf("u2mp roster wire: all tests passed\n");
    return failures == 0 ? 0 : 1;
}
