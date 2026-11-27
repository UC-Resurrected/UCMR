// u2mp rewrite tests, group "loading" (loading.cpp): stand-ins for what the loading screen calls in menuimages.cpp
// (menu_net_material, menu_draw_quad). Compiled once per namespace by run.sh; every call is recorded (float arguments
// as bit patterns) and the material comes from the test (hxl::material, test_loading.cpp).
#include <cstring>

#include "frontend3.h"
#include "harness.h"

namespace hxl {
uint8_t *material(uint32_t id);
}

namespace {
unsigned fb(float f) {
    unsigned u;
    std::memcpy(&u, &f, 4);
    return u;
}
}  // namespace

namespace u2mp {

uint8_t *menu_net_material(uint32_t id) {
    uint8_t *m = hxl::material(id);
    hx::rec("menu_net_material %08x -> %s", id, hx::where(m).c_str());
    return m;
}

void menu_draw_quad(uint8_t *material, float x, float y, float w, float h, float u0, float v0, float u1, float v1,
                    float alpha, float rgb) {
#ifdef u2mp   // the old build (-Du2mp=u2mp_old)
    // (2026-11-27, spec loading.md steps 7 and 9) PS3 places the band and the outer edge quad by their two corners;
    // the old code passed the second corner as the size. Its calls get the size the new code passes.
    if (x == 128.0f && y == 22.0f && w == 1152.0f && h == 278.0f) w = 1024.0f, h = 256.0f;
    if (x == -10.0f && y == -10.0f && w == 1290.0f && h == 730.0f) w = 1300.0f, h = 740.0f;
#endif
    hx::rec("menu_draw_quad %s %08x %08x %08x %08x uv %08x %08x %08x %08x alpha %08x rgb %08x", hx::where(material).c_str(),
            fb(x), fb(y), fb(w), fb(h), fb(u0), fb(v0), fb(u1), fb(v1), fb(alpha), fb(rgb));
}

}  // namespace u2mp
