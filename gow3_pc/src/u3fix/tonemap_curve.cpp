// Uncharted 3 1.02, 15 Nov 2026: the render settings' "improved" tonemap curve, used by PS3 and dropped by the PS4 build.
//
// The tonemap block of the render settings holds two curves: the filmic curve (+0x710 white point, +0x714..+0x728 its
// strengths, +0x72c filmic exposure bias) and the improved curve (+0x730 use-improved-curve, +0x734 improved exposure
// bias, +0x738 black point, +0x73c white point). PS3 1.19 builds its tonemap constants from the improved curve whenever
// the curve type (+0x70c) is filmic and use-improved-curve is not below one half: the Hejl / Burgess-Dawson filmic curve
// v (6.2 v + 0.5) / (v (6.2 v + 1.7) + 0.06), divided by the improved white point, of the colour after the levels scaled
// by 2^improved-exposure-bias, less the black point; the filmic fields are not used then. The PS4 build only has the
// filmic curve and never reads the four fields. No PS4 campaign level sets them, but PS3 multiplayer maps do: Museum has
// improved bias -4.1 and filmic bias -1.4, so the port showed it about 2.7 stops brighter than PS3, with the filmic
// shoulder instead of the improved one (also Ruins, Shambhala and Warzone variants, the co-op stage).
//
// The PS4 tonemap pixel shader evaluates y = c4 + (c5 x + c6) / (x^2 + c0 x + c1) with x = levels scale * colour +
// levels offset (c36..c38, c40..c42), the same form PS3 fills with either curve, so the improved curve needs only other
// constants. The constant builder (image 0xed9bd0) stores the curve (c0..c6) first and the levels (c36..c43) last; the fix
// runs right after that last store and, when the curve applies, rewrites c0, c1, c4..c6 and folds the exposure and the
// black point into the levels scale and offset, as PS3 does. It does nothing while the game's "T1 style filmic curve"
// debug switch is off (the builder then uses a fixed curve of its own instead of the render settings).
// Default since 16 Nov 2026: the improved curve as a display value, except for Borneo co-op's render settings (filmic).
#include "u3fix.h"
#include "u3fix_hook.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace u3fix {

// Two ways to show the improved curve (neither is the default, see install_improved_tonemap_curve). The Hejl /
// Burgess-Dawson curve (HBD) already contains a display gamma, while the PS4 tonemap shader always raises the curve's
// result to 1/2.2. display = true writes the curve whose 1/2.2 power is HBD itself: y = HBD(v)^2.2 / w^2.2, in the
// shader's form c4 + (c5 v + c6) / (v^2 + c0 v + c1) with y(0) = 0; the constants are a fit made for this port
// (after the 1/2.2 power within 0.0017 of HBD(v) for v in 1e-4..32; 8-bit display steps are 0.0039), and the white
// point scales c4..c6 by w^-2.2. display = false keeps the curve's own constants (HBD gamma-encoded again).
bool improved_tonemap_curve(float *constants, const unsigned char *settings, bool t1_curve, bool display) {
    uint32_t curve_type;
    float use, bias, black, white;
    std::memcpy(&curve_type, settings + 0x70c, 4);
    std::memcpy(&use, settings + 0x730, 4);
    std::memcpy(&bias, settings + 0x734, 4);
    std::memcpy(&black, settings + 0x738, 4);
    std::memcpy(&white, settings + 0x73c, 4);
    if (!t1_curve || curve_type != 1 || use < 0.5f) return false;
    if (display) {
        constexpr float c0 = 0.445557023f, c1 = 0.0275066962f, c5 = -0.447434753f, c4 = 1.00402214f;
        const float s = std::pow(white, -2.2f);
        constants[0] = c0;
        constants[1] = c1;
        constants[4] = c4 * s;
        constants[5] = c5 * s;
        constants[6] = -c4 * c1 * s;
    } else {
        // the Hejl / Burgess-Dawson curve with its x^2 term divided out: v (v + a) / (v^2 + b v + k) = 1 + ((a - b) v - k) / (...)
        const float a = 0.5f / 6.2f, b = 1.7f / 6.2f, k = 0.06f / 6.2f;
        constants[0] = b;
        constants[1] = k;
        constants[4] = 1.0f / white;
        constants[5] = (a - b) / white;
        constants[6] = -k / white;
    }
    const float scale = std::exp2(bias);
    for (int i = 0; i < 3; i++) {
        constants[36 + i] *= scale;
        constants[40 + i] -= black;
    }
    return true;
}

// The default: the improved curve as a display value for the render settings that use it, except Borneo's. Of the
// multiplayer and co-op render settings that turn the improved curve on (Museum, Ruins, Warzone and Borneo: dig-adv,
// dig-coop-stage), Borneo's alone keep the auto exposure below 2 EV (range 0.25..1.75; the others reach 2.25 to 4.875).
// The owner's tests (15 / 16 Nov 2026): Museum shown with the improved curve is close to PS3, Borneo with it far too dark
// and right with the filmic curve. So the curve applies when the exposure maximum (+0x6f8) is at least 2.
bool improved_curve_by_default(const unsigned char *settings) {
    float mx;
    std::memcpy(&mx, settings + 0x6f8, 4);
    return mx >= 2.0f;
}

namespace {

std::atomic<uint64_t> g_frames{0};
std::atomic<uint32_t> g_last{0}, g_reports{0};

std::atomic<uint64_t> g_seen{0}, g_candidate{0};
std::atomic<uint32_t> g_seen_reports{0}, g_stable{0};

// gow3 15 Nov 2026 diagnostic (Village / Museum / Borneo brightness against PS3): the tonemap fields of the live render
// settings the constant builder gets (which of a level's render settings is in use, and whether the improved curve
// applies), printed when they change and then hold for 30 frames (region blends in between are not printed; 2 decimals;
// at most 80 lines). UNCHARTED_TONEMAP_LOG=0 turns it off.
void log_settings(const unsigned char *settings, uint32_t t1_curve) {
    static const bool on = [] {
        const char *env = std::getenv("UNCHARTED_TONEMAP_LOG");
        return !(env && env[0] == '0');
    }();
    if (!on || g_seen_reports.load(std::memory_order_relaxed) >= 80) return;
    uint32_t method, curve;
    float mn, mx, set, fbias, use, ibias, black, white;
    std::memcpy(&method, settings + 0x6f0, 4);
    std::memcpy(&mn, settings + 0x6f4, 4);
    std::memcpy(&mx, settings + 0x6f8, 4);
    std::memcpy(&set, settings + 0x708, 4);
    std::memcpy(&curve, settings + 0x70c, 4);
    std::memcpy(&fbias, settings + 0x72c, 4);
    std::memcpy(&use, settings + 0x730, 4);
    std::memcpy(&ibias, settings + 0x734, 4);
    std::memcpy(&black, settings + 0x738, 4);
    std::memcpy(&white, settings + 0x73c, 4);
    uint64_t key = 1469598103934665603ull;
    const float q[] = {mn, mx, set, fbias, use, ibias, white};
    for (float v : q) key = (key ^ static_cast<uint64_t>(std::lround(v * 100.0f) & 0xffff)) * 1099511628211ull;
    key = (key ^ (method | curve << 4 | (t1_curve & 0xff) << 8)) * 1099511628211ull;
    if (key == g_seen.load(std::memory_order_relaxed)) return;
    if (g_candidate.exchange(key, std::memory_order_relaxed) != key) {
        g_stable.store(0, std::memory_order_relaxed);
        return;
    }
    if (g_stable.fetch_add(1, std::memory_order_relaxed) < 30) return;
    g_seen.store(key, std::memory_order_relaxed);
    g_seen_reports.fetch_add(1, std::memory_order_relaxed);
    const bool applies = (t1_curve & 0xff) != 0 && curve == 1 && use >= 0.5f;
    std::printf("Uncharted 3: tonemap settings: method %u range %.3f..%.3f set-point %.3f curve %u filmic bias %.3f, "
                "improved curve %s (use %.2f bias %.3f black %.4f white %.3f), T1 %u\n",
                method, mn, mx, set, curve, fbias, applies ? "on" : "off", use, ibias, black, white, t1_curve & 0xff);
    std::fflush(stdout);
}

// 0 filmic (no rewrite), 1 improved curve as a display value, 2 improved curve then gamma, 3 (default) the improved curve
// as a display value where improved_curve_by_default says so, else filmic
std::atomic<int> g_mode{3};

}  // namespace

// The owner's switch file: "u3-switches.txt" in the --user folder (the launcher passes --user; the game's command line
// is read back here, so no environment variable is needed), one word per line. Read once.
bool user_switch(const char *word) {
    static const std::vector<std::string> words = [] {
        std::vector<std::string> out;
        std::string user;
#ifdef _WIN32
        for (int i = 1; i + 1 < __argc; i++)
            if (__argv[i] && std::strcmp(__argv[i], "--user") == 0) user = __argv[i + 1];
#else
        if (FILE *f = std::fopen("/proc/self/cmdline", "rb")) {
            std::string all, prev;
            char ch;
            while (std::fread(&ch, 1, 1, f) == 1) all.push_back(ch);
            std::fclose(f);
            for (size_t p = 0; p < all.size();) {
                const std::string arg = all.c_str() + p;
                if (prev == "--user") user = arg;
                prev = arg;
                p += arg.size() + 1;
            }
        }
#endif
        if (user.empty()) return out;
        if (FILE *f = std::fopen((user + "/u3-switches.txt").c_str(), "rb")) {
            char line[256];
            while (std::fgets(line, sizeof line, f)) {
                std::string s(line);
                while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
                if (!s.empty()) out.push_back(s);
            }
            std::fclose(f);
        }
        return out;
    }();
    for (const std::string &w : words)
        if (w == word) return true;
    return false;
}

namespace {

// called from the stub with the constant block (r12), the render settings (rbx) and the T1 switch (r15b)
__attribute__((sysv_abi)) void tonemap_hook(float *constants, const unsigned char *settings, uint32_t t1_curve) {
    if (settings) log_settings(settings, t1_curve);
    const int mode = g_mode.load(std::memory_order_relaxed);
    if (!constants || !settings || mode == 0 || (mode == 3 && !improved_curve_by_default(settings)) ||
        !improved_tonemap_curve(constants, settings, (t1_curve & 0xff) != 0, mode != 2))
        return;
    g_frames.fetch_add(1, std::memory_order_relaxed);
    uint32_t bias_bits;
    std::memcpy(&bias_bits, settings + 0x734, 4);
    if (g_last.exchange(bias_bits, std::memory_order_relaxed) != bias_bits && g_reports.fetch_add(1) < 20) {
        float bias, black, white;
        std::memcpy(&bias, settings + 0x734, 4);
        std::memcpy(&black, settings + 0x738, 4);
        std::memcpy(&white, settings + 0x73c, 4);
        std::printf("Uncharted 3: improved tonemap curve (as on PS3): exposure bias %.3f, black %.4f, white %.3f\n",
                    bias, black, white);
        std::fflush(stdout);
    }
}

// the levels stores at the end of the constant builder: vmovaps [r12+0x90], xmm3; the offset lanes; vmovaps [r12+0xa0],
// xmm0; vmovss xmm1, [r13+0x7d8]
constexpr unsigned char kSite[] = {
    0xc4, 0xc1, 0x78, 0x29, 0x9c, 0x24, 0x90, 0x00, 0x00, 0x00, 0xc4, 0xe3, 0x79, 0x04, 0xc0, 0x15, 0xc4, 0xe3, 0x79, 0x04,
    0xc9, 0x15, 0xc4, 0xe3, 0x79, 0x04, 0xd2, 0x15, 0xc5, 0xf8, 0x15, 0xc2, 0xc5, 0xf1, 0x73, 0xd9, 0x04, 0xc5, 0xf8, 0x15,
    0xc1, 0xc4, 0xc1, 0x78, 0x29, 0x84, 0x24, 0xa0, 0x00, 0x00, 0x00, 0xc4, 0xc1, 0x7a, 0x10, 0x8d, 0xd8, 0x07, 0x00, 0x00};
constexpr unsigned kStore = 41;       // offset of vmovaps [r12+0xa0], xmm0 in kSite
constexpr unsigned kStoreLength = 10;

void put32(unsigned char *p, unsigned &n, int32_t v) { std::memcpy(p + n, &v, 4); n += 4; }

// mov [rsp+disp], r64 (store) or mov r64, [rsp+disp] (load); reg 0..15
void gpr_slot(unsigned char *p, unsigned &n, unsigned reg, int32_t disp, bool store) {
    p[n++] = static_cast<unsigned char>(0x48 | (reg >= 8 ? 4 : 0));
    p[n++] = store ? 0x89 : 0x8b;
    p[n++] = static_cast<unsigned char>(0x84 | (reg & 7) << 3);
    p[n++] = 0x24;
    put32(p, n, disp);
}

// movups [rsp+disp], xmmN (store) or movups xmmN, [rsp+disp] (load)
void xmm_slot(unsigned char *p, unsigned &n, unsigned reg, int32_t disp, bool store) {
    if (reg >= 8) p[n++] = 0x44;
    p[n++] = 0x0f;
    p[n++] = store ? 0x11 : 0x10;
    p[n++] = static_cast<unsigned char>(0x84 | (reg & 7) << 3);
    p[n++] = 0x24;
    put32(p, n, disp);
}

}  // namespace

bool install_improved_tonemap_curve(unsigned char *image, uint64_t size) {
    // Default: the improved curve as a display value where the render settings use it, except Borneo's
    // (improved_curve_by_default); Borneo co-op keeps the PS4 filmic curve (owner's tests, 15 / 16 Nov 2026: Borneo
    // right only with the filmic curve, Museum close to PS3 with the improved one). Words in u3-switches.txt:
    // tonemap-filmic (or UNCHARTED_IMPROVED_TONEMAP=0) the filmic curve everywhere; tonemap-improved (=1) the improved
    // curve as a display value everywhere it is on, Borneo too; tonemap-gamma (=gamma) its own constants, then the gamma.
    const char *env = std::getenv("UNCHARTED_IMPROVED_TONEMAP");
    const bool filmic = (env && env[0] == '0') || user_switch("tonemap-filmic");
    const bool gamma = (env && std::strcmp(env, "gamma") == 0) || user_switch("tonemap-gamma");
    const bool display = (env && env[0] == '1') || user_switch("tonemap-improved");
    const int mode = filmic ? 0 : gamma ? 2 : display ? 1 : 3;
    g_mode.store(mode, std::memory_order_relaxed);
    const uint64_t site = find_unique(image, size, kSite, sizeof(kSite));
    if (site == kNotFound) return false;
    unsigned char *stub = stub_page_near(image, size);
    if (!stub) return false;
    const uint64_t at = site + kStore;
    // The stub stores the levels offsets as the replaced instruction did, steps over the 128 bytes below the stack
    // pointer (PS4 code may keep values there), keeps every register the System V convention lets the handler change
    // (rax, rcx, rdx, rsi, rdi, r8-r11, xmm0-15), calls the handler on an aligned stack and jumps back.
    static const unsigned kSaved[] = {0, 1, 2, 6, 7, 8, 9, 10, 11};   // rax rcx rdx rsi rdi r8 r9 r10 r11
    unsigned n = 0;
    std::memcpy(stub + n, image + at, kStoreLength); n += kStoreLength;      // vmovaps [r12+0xa0], xmm0
    const unsigned char enter[] = {0x48, 0x8d, 0x64, 0x24, 0x80,             // lea rsp, [rsp-0x80]
                                   0x55,                                     // push rbp
                                   0x48, 0x89, 0xe5,                         // mov rbp, rsp
                                   0x48, 0x83, 0xe4, 0xf0,                   // and rsp, -16
                                   0x48, 0x81, 0xec, 0x80, 0x01, 0x00, 0x00};  // sub rsp, 0x180
    std::memcpy(stub + n, enter, sizeof enter); n += sizeof enter;
    for (unsigned i = 0; i < 16; i++) xmm_slot(stub, n, i, static_cast<int32_t>(16 * i), true);
    for (unsigned i = 0; i < 9; i++) gpr_slot(stub, n, kSaved[i], static_cast<int32_t>(0x100 + 8 * i), true);
    const unsigned char args[] = {0x4c, 0x89, 0xe7,         // mov rdi, r12
                                  0x48, 0x89, 0xde,         // mov rsi, rbx
                                  0x41, 0x0f, 0xb6, 0xd7};  // movzx edx, r15b
    std::memcpy(stub + n, args, sizeof args); n += sizeof args;
    const uint64_t handler = reinterpret_cast<uint64_t>(&tonemap_hook);
    stub[n++] = 0x48; stub[n++] = 0xb8; std::memcpy(stub + n, &handler, 8); n += 8;   // mov rax, handler
    stub[n++] = 0xff; stub[n++] = 0xd0;                                              // call rax
    for (unsigned i = 0; i < 9; i++) gpr_slot(stub, n, kSaved[i], static_cast<int32_t>(0x100 + 8 * i), false);
    for (unsigned i = 0; i < 16; i++) xmm_slot(stub, n, i, static_cast<int32_t>(16 * i), false);
    const unsigned char leave[] = {0x48, 0x89, 0xec,                                // mov rsp, rbp
                                   0x5d,                                            // pop rbp
                                   0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00};  // lea rsp, [rsp+0x80]
    std::memcpy(stub + n, leave, sizeof leave); n += sizeof leave;
    stub[n++] = 0xff; stub[n++] = 0x25; std::memset(stub + n, 0, 4); n += 4;          // jmp [rip]
    const uint64_t back = reinterpret_cast<uint64_t>(image + at + kStoreLength);
    std::memcpy(stub + n, &back, 8); n += 8;
    if (!finish_stub(stub) || !jump_to(image, at, kStoreLength, stub)) return false;
    std::printf("Uncharted 3: improved tonemap curve of the render settings at the constant builder %#llx (%s)\n",
                static_cast<unsigned long long>(at),
                mode == 0   ? "filmic curve as on PS4 (tonemap-filmic); tonemap settings log only"
                : mode == 2 ? "improved curve, gamma-encoded again (tonemap-gamma)"
                : mode == 1 ? "improved curve as a display value everywhere (tonemap-improved)"
                            : "improved curve as a display value, filmic where the exposure stays below 2 EV (Borneo)");
    std::fflush(stdout);
    return true;
}

}  // namespace u3fix
