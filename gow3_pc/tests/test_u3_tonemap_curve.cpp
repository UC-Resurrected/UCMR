// u3-tonemap-test: the PS3 improved tonemap curve (src/u3fix/tonemap_curve.cpp) on made-up render settings. The rewritten
// constants, put through the PS4 tonemap shader's formula, must give the Hejl / Burgess-Dawson curve of the exposed colour
// less the black point, divided by the white point; with the curve off the constants must stay as they were.
#include "../src/u3fix/u3fix_hook.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

void set_u32(unsigned char *rs, unsigned offset, uint32_t v) { std::memcpy(rs + offset, &v, 4); }
void set_f32(unsigned char *rs, unsigned offset, float v) { std::memcpy(rs + offset, &v, 4); }

// the PS4 tonemap pixel shader's curve for one channel
float shader_curve(const float *c, int channel, float colour) {
    const float x = c[36 + channel] * colour + c[40 + channel];
    return (c[5] * x + c[6]) / (x * (x + c[0]) + c[1]) + c[4];
}

float hbd(float v) { return v * (6.2f * v + 0.5f) / (v * (6.2f * v + 1.7f) + 0.06f); }

void fill(float *c) {
    for (int i = 0; i < 48; i++) c[i] = 0.25f + 0.01f * static_cast<float>(i);
}

}  // namespace

int main() {
    unsigned char rs[0x800] = {};
    set_u32(rs, 0x70c, 1);         // filmic curve type
    set_f32(rs, 0x72c, -1.4f);     // filmic bias (not used by the improved curve)
    set_f32(rs, 0x730, 1.0f);      // use-improved-curve
    set_f32(rs, 0x734, -4.1f);     // improved exposure bias
    set_f32(rs, 0x738, 0.003f);    // black point
    set_f32(rs, 0x73c, 0.95f);     // white point

    float c[48], before[48];
    fill(c);
    std::memcpy(before, c, sizeof c);
    // gamma mode: the curve's own constants (the PS4 shader then raises them to 1/2.2)
    assert(u3fix::improved_tonemap_curve(c, rs, true, false));
    const float q = std::exp2(-4.1f);
    int checked = 0;
    for (int channel = 0; channel < 3; channel++) {
        for (float colour = 0.01f; colour < 200.0f; colour *= 1.7f) {
            const float v = q * before[36 + channel] * colour + before[40 + channel] - 0.003f;
            const float want = hbd(v) / 0.95f;
            const float got = shader_curve(c, channel, colour);
            if (std::fabs(got - want) > 2e-5f * std::fmax(1.0f, std::fabs(want))) {
                std::printf("channel %d colour %g: shader %.7f, curve %.7f\n", channel, colour, got, want);
                return 1;
            }
            checked++;
        }
    }
    // display mode (default): after the shader's clamp and 1/2.2 power, the display value is HBD(v) / w within 0.002
    fill(c);
    assert(u3fix::improved_tonemap_curve(c, rs, true));
    float worst = 0;
    for (int channel = 0; channel < 3; channel++) {
        for (float colour = 0.001f; colour < 2000.0f; colour *= 1.1f) {
            const float v = q * before[36 + channel] * colour + before[40 + channel] - 0.003f;
            if (v < 1e-4f) continue;
            const float want = std::fmin(1.0f, hbd(v) / 0.95f);
            const float y = std::fmin(1.0f, std::fmax(0.0f, shader_curve(c, channel, colour)));
            const float got = std::pow(y, 1.0f / 2.2f);
            worst = std::fmax(worst, std::fabs(got - want));
            if (std::fabs(got - want) > 0.002f) {
                std::printf("display: channel %d colour %g: %.5f, curve %.5f\n", channel, colour, got, want);
                return 1;
            }
            checked++;
        }
    }
    std::printf("display mode: worst display error %.5f\n", worst);
    // only the curve and the levels change
    for (int i = 0; i < 48; i++) {
        const bool curve = i == 0 || i == 1 || (i >= 4 && i <= 6) || (i >= 36 && i <= 38) || (i >= 40 && i <= 42);
        if (!curve) assert(c[i] == before[i]);
    }

    // not used: below one half, another curve type, the T1 switch off
    const float uses[] = {0.0f, 0.49f};
    for (float use : uses) {
        set_f32(rs, 0x730, use);
        fill(c);
        assert(!u3fix::improved_tonemap_curve(c, rs, true));
        assert(std::memcmp(c, before, sizeof c) == 0);
    }
    set_f32(rs, 0x730, 0.5f);
    fill(c);
    assert(u3fix::improved_tonemap_curve(c, rs, true));
    set_u32(rs, 0x70c, 0);
    fill(c);
    assert(!u3fix::improved_tonemap_curve(c, rs, true));
    assert(std::memcmp(c, before, sizeof c) == 0);
    set_u32(rs, 0x70c, 1);
    fill(c);
    assert(!u3fix::improved_tonemap_curve(c, rs, false));
    assert(std::memcmp(c, before, sizeof c) == 0);

    // the default choice: Borneo co-op's render settings (exposure range 0.25..1.75) keep the filmic curve; Museum
    // (2.375..4.875), Ruins (0.25..2.25) and Warzone (-4..4) get the improved one
    const float maxima[][2] = {{1.75f, 0}, {1.99f, 0}, {2.0f, 1}, {2.25f, 1}, {4.0f, 1}, {4.875f, 1}};
    for (const auto &m : maxima) {
        set_f32(rs, 0x6f8, m[0]);
        assert(u3fix::improved_curve_by_default(rs) == (m[1] != 0));
    }

    std::printf("u3-tonemap-test: %d curve points match, unused cases unchanged\n", checked);
    return 0;
}
