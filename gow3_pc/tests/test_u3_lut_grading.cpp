// u3-lut-grading-test: the PS3 colour grading table (src/u3fix/lut_grading.cpp). Twelve parameter sets of multiplayer
// and co-op render settings (lut-enable, then the 35 grading values, as float bit patterns) are baked and each table's
// FNV-1a 64 hash compared with the reference hash: the hash of the reference bake, checked byte for byte against the
// PS3 job run instruction by instruction. Also: the caller's rounding mode survives a bake, red with no hue shift
// turns magenta as on PS3, all values zero are baked (as PS3 does), and which render settings ask for the grading.
#include "../src/u3fix/u3fix_hook.h"

#include <cassert>
#include <cfenv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

struct Set {
    const char *name;
    uint32_t lut[36];
    uint64_t hash;
};

const Set kSets[] = {
    {"dig-coop-stage c620c1d9",
     {0x3f800000, 0x3e2e147b, 0x3e23d70a, 0x3e6147ae, 0x00000000, 0x00000000, 0x00000000, 0x3ca3d70a, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x3c23d70a, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0xbd23d70a, 0x3dcccccd, 0x00000000, 0x00000000, 0x00000000, 0x00000000},
     0x3fcc5356a2e5a831ull},
    {"old-quarter-multi e9c2efb4",
     {0x3f800000, 0x00000000, 0x00000000, 0x00000000, 0x3e4ccccd, 0x3f800000, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000},
     0xa6a5c2868c1f3c20ull},
    {"village-multi-stage f1016174",
     {0x3f800000, 0xbc23d70a, 0x3ccccccd, 0x3d851eb8, 0x3c23d70a, 0x3dcccccd, 0x00000000, 0x00000000, 0x3cf5c28f,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000},
     0xaa291021d1589dd5ull},
    {"facility-multi (first set)",
     {0x3f800000, 0xbd23d70a, 0x3cf5c28f, 0xbd4ccccd, 0x3dcccccd, 0x3f000000, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x80000000, 0x00000000, 0x00000000, 0x00000000},
     0x694b5dc63b462ccfull},
    {"facility-multi (second set)",
     {0x3f800000, 0xbd23d70a, 0x3cf5c28f, 0xbd4ccccd, 0x3dcccccd, 0x3f000000, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x80000000, 0x00000000, 0x00000000, 0x00000000},
     0x694b5dc63b462ccfull},
    {"museum-multi 0d1375eb",
     {0x3f800000, 0x3d4ccccd, 0x3d75c28f, 0x3df5c28f, 0x3e800000, 0x3f19999a, 0x00000000, 0x00000000, 0x00000000,
      0x3a83126f, 0x00000000, 0x00000000, 0x3a83126f, 0x00000000, 0x00000000, 0x3a83126f, 0x00000000, 0x00000000,
      0x3a83126f, 0x00000000, 0x00000000, 0x3a83126f, 0x00000000, 0x00000000, 0x3a83126f, 0x00000000, 0x00000000,
      0xbc75c28f, 0x00000000, 0xbca465a5, 0x3a83126f, 0x00000000, 0x00000000, 0x3a83126f, 0x00000000, 0x00000000},
     0x23dcfb5fa3a2b802ull},
    {"ruins-multi c83507a0",
     {0x3f800000, 0xbd23d70a, 0x3be56042, 0x3de147ae, 0x3e4ccccd, 0x3f666666, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x3d8f5c29, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xbca3d70a, 0x00000000, 0x3d4ccccd, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xbd4ccccd, 0xbd23d70a, 0x00000000, 0x00000000, 0x00000000},
     0xc4633ea9781af433ull},
    {"cha-multi 094fa7f5",
     {0x3f800000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x3f800000, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
      0x3f800000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x3f800000, 0x3f800000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000},
     0xe0acf90bb0db0285ull},
    {"cha-multi 3e80d98f",
     {0x3f800000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x3f800000, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
      0x3f800000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x3f800000, 0x3f800000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000},
     0xe0acf90bb0db0285ull},
    {"ruins-multi bade5873",
     {0x3f800000, 0xbd75c28f, 0x3be56042, 0x3d8f5c29, 0x3e4ccccd, 0x3f666666, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xbca3d70a, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x80000000, 0xbdcccccd, 0x3cf5c28f, 0x00000000, 0x00000000, 0x00000000},
     0xac98ddae6f521decull},
    {"facility-multi 0b68a6e5",
     {0x3f800000, 0xbd23d70a, 0x3cf5c28f, 0xbd4ccccd, 0x3dcccccd, 0x3f000000, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x80000000, 0x00000000, 0x00000000, 0x00000000},
     0x694b5dc63b462ccfull},
    {"warzone-multi 3343b48d",
     {0x3f800000, 0xbc23d70a, 0x3ccccccd, 0x3d851eb8, 0x3c23d70a, 0x3dcccccd, 0x00000000, 0x00000000, 0x3cf5c28f,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000},
     0xaa291021d1589dd5ull},
};

uint64_t fnv1a(const unsigned char *p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

void to_floats(const uint32_t *bits, float *lut) { std::memcpy(lut, bits, 36 * 4); }

void set_f32(unsigned char *rs, unsigned offset, float v) { std::memcpy(rs + offset, &v, 4); }

}  // namespace

int main() {
    std::vector<unsigned char> table(32 * 32 * 32 * 4);
    int failed = 0;
    double slowest = 0;
    for (const Set &s : kSets) {
        float lut[36];
        to_floats(s.lut, lut);
        const auto start = std::chrono::steady_clock::now();
        u3fix::bake_ps3_grading_lut(lut, table.data());
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (ms > slowest) slowest = ms;
        const uint64_t h = fnv1a(table.data(), table.size());
        const bool ok = h == s.hash;
        if (!ok) failed++;
        std::printf("%-30s %016llx %s (%.2f ms)\n", s.name, static_cast<unsigned long long>(h), ok ? "ok" : "DIFFERS",
                    ms);
    }

    // the caller's rounding mode is kept, and does not change the table
    {
        float lut[36];
        to_floats(kSets[7].lut, lut);
        std::fesetround(FE_UPWARD);
        u3fix::bake_ps3_grading_lut(lut, table.data());
        const int mode = std::fegetround();
        std::fesetround(FE_TONEAREST);
        assert(mode == FE_UPWARD);
        assert(fnv1a(table.data(), table.size()) == kSets[7].hash);
    }

    // red with no hue shift at all (Old Quarter) comes out magenta at (31, 0, 0), as on PS3; grey stays grey
    {
        float lut[36];
        to_floats(kSets[1].lut, lut);
        u3fix::bake_ps3_grading_lut(lut, table.data());
        const unsigned char *red = &table[4 * 31];
        assert(red[0] == 255 && red[1] == 0 && red[2] == 255 && red[3] == 255);
        const unsigned char *grey = &table[4 * (16 + 32 * 16 + 1024 * 16)];
        assert(grey[0] == grey[1] && grey[1] == grey[2]);
    }

    // lut-enable on and all 35 values zero: PS3 bakes that too. The table is not the identity (the round trip in round
    // toward zero loses a step at many points), it equals Old Quarter's (whose toe changes nothing either), and its pure
    // reds come out magenta.
    {
        float lut[36] = {1.0f};
        u3fix::bake_ps3_grading_lut(lut, table.data());
        const uint64_t h = fnv1a(table.data(), table.size());
        std::printf("%-30s %016llx %s\n", "all values zero", static_cast<unsigned long long>(h),
                    h == kSets[1].hash ? "ok" : "DIFFERS");
        if (h != kSets[1].hash) failed++;
        const unsigned char *red = &table[4 * 31];
        assert(red[0] == 255 && red[1] == 0 && red[2] == 255);
        const unsigned char *pink = &table[4 * (31 + 32 * 3 + 1024 * 3)];
        assert(pink[0] == 255 && pink[1] == 24 && pink[2] == 255);
    }

    // which render settings ask for the grading (the net-game check is the hook's, not tested here)
    {
        std::vector<unsigned char> rs(0x800, 0);
        float lut[36];
        set_f32(rs.data(), 0x740, 1.0f);
        assert(u3fix::ps3_grading_inputs(rs.data(), lut));    // all values zero: graded, as on PS3
        assert(lut[0] == 1.0f && lut[1] == 0.0f && lut[35] == 0.0f);
        set_f32(rs.data(), 0x760, 0.25f);                     // shoulder r
        set_f32(rs.data(), 0x7e8, -0.5f);                      // grey luminance, the last value
        assert(u3fix::ps3_grading_inputs(rs.data(), lut));
        assert(lut[0] == 1.0f && lut[1] == 0.25f && lut[35] == -0.5f && lut[2] == 0.0f);
        set_f32(rs.data(), 0x740, 0.5f);                       // lut-enable must be above one half
        assert(!u3fix::ps3_grading_inputs(rs.data(), lut));
        set_f32(rs.data(), 0x740, 1.0f);
        const uint32_t one = 1;
        std::memcpy(rs.data() + 0x750, &one, 4);              // the record names a PS4 table: the game's choice
        assert(!u3fix::ps3_grading_inputs(rs.data(), lut));
    }

    std::printf("u3-lut-grading-test: %zu sets, %d differ, slowest bake %.2f ms\n", sizeof kSets / sizeof kSets[0], failed,
                slowest);
    return failed == 0 ? 0 : 1;
}
