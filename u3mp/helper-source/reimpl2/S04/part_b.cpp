// S04 part B: film tab bar update (u3x_k23_update) and the cinema system module config (u3x_cinema_config_push).
// Written from spec S04_film_leftovers.md, part B.

// Functions of other groups (own glue, stays as it is).
extern "C" int u3x_k23_rep(uintptr_t base, uint8_t *pad, uint32_t m);
extern "C" void u3x_k23_action(uintptr_t base, uint32_t id);
extern "C" int u3x_k29_eok(const void *p);

namespace s04b {

// Film action ids used as compare values (interface ids, spec 3 "Tab ids").
constexpr uint32_t WIDE = 0xc1f65aebu;
constexpr uint32_t CAM1 = 0xff502160u;
constexpr uint32_t CAM2 = 0x806c9824u;
constexpr uint32_t CAM0 = 0x887c7d6eu;
constexpr uint32_t T682 = 0x682c6868u;
constexpr uint32_t TE81 = 0xe8184ce8u;
constexpr uint32_t T3CF = 0x3cf5deedu;
constexpr uint32_t TA65 = 0xa65c61bau;
constexpr uint32_t T382 = 0x38231b34u;
constexpr uint32_t T516 = 0x516bfbb8u;
constexpr uint32_t TBD8 = 0xbd8d7974u;

// Image offsets.
constexpr uintptr_t FILM_STATE = 0x17c3ba8;      // +0x1e68 camera mode, +0x31c flags
constexpr uintptr_t DISPLAY_WIDTH = 0x18c6b58;
constexpr uintptr_t NGM_BYTE_A = 0x17d1d10;
constexpr uintptr_t NGM_BYTE_B = 0x17d1d11;
constexpr uintptr_t FILM_BYTE_A = 0x17c3571;
constexpr uintptr_t FILM_BYTE_B = 0x17c3572;
constexpr uintptr_t READER_WORD = 0x17c5d00;
constexpr uintptr_t GAME_CLOCK = 0x17b4ba4;
constexpr uintptr_t SCREENSHOT = 0x1acab73;
constexpr uintptr_t PADS = 0x16e2fa0;
constexpr uintptr_t MENU_PAGE_UP = 0x17bd088;
constexpr uintptr_t SYS_OPTIONS = 0x1b24b80;
constexpr uintptr_t SOUND_START = 0x1007610;
constexpr uintptr_t SOUND_PLAY = 0x119b250;

constexpr uintptr_t CONFIG_TABLE = 0x1809440;    // entries of 0x30 bytes
constexpr uintptr_t CONFIG_COUNT = 0x1809620;
constexpr uintptr_t NAME_CINEMA = 0x145e698;     // "multi-player-cinema", 20 bytes with its NUL
constexpr uintptr_t NAME_SINGLE = 0x14be998;     // "single-player", 14 bytes with its NUL
constexpr uintptr_t SINGLE_MODULES = 0x16fb760;  // PS4's single-player module list
constexpr uint32_t CONFIG_MAX = 10;

// Sound object for the cursor click: one zeroed block, kept between calls.
U3X_BSS uint8_t click_sound[0x200];

// Tab rule: is the tab with this id shown? Reads the game state each time.
U3X_FN bool tab_shown(uintptr_t base, uint32_t id) {
    const uintptr_t film = base + FILM_STATE;
    if (id == WIDE && at<int32_t>(base + DISPLAY_WIDTH) != 1280) return false;

    const int32_t camera = at<int32_t>(film + 0x1e68);
    if (at<uint8_t>(base + NGM_BYTE_B) != 0 || at<uint8_t>(base + NGM_BYTE_A) != 0) {
        if (id == CAM1 && camera != 2) return false;
        if (id == CAM2 || id == T682) return false;
    } else {
        if (id == CAM1 && camera != 1) return false;
        if (id == CAM2 && camera != 2) return false;
    }
    if (id == CAM0 && camera != 0) return false;

    if (at<uint32_t>(film + 0x31c) & 1) {
        if (id == TE81 || id == CAM0 || id == TA65 || id == CAM2 || id == T382 || id == T516 || id == CAM1) return false;
    } else {
        const bool on = at<uint8_t>(base + FILM_BYTE_A) != 0 ? at<uint8_t>(base + FILM_BYTE_B) != 0 : true;
        if (id == T3CF && !on) return false;
        if (id == TE81 && on) return false;
    }

    if (at<int32_t>(base + READER_WORD) != 0) return id != T682 && id != TBD8 && id != T382;
    return id != T516;
}

// n bytes at a and b equal?
U3X_FN bool same_bytes(const uint8_t *a, const uint8_t *b, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i)
        if (a[i] != b[i]) return false;
    return true;
}

}  // namespace s04b

extern "C" U3X_FN void u3x_k23_update(uintptr_t base, uint8_t *el) {
    using namespace s04b;
    uint32_t *eb = ext_eb();

    // 1. clock first, then the visible count is cleared
    const uint32_t now = at<uint32_t>(base + GAME_CLOCK);
    *reinterpret_cast<uint32_t *>(el + 0x1d0) = 0;

    // 2. no update while a screenshot is taken
    if (at<uint8_t>(base + SCREENSHOT) != 0) { eb[202] += 1; return; }

    // 3. visible tab list
    int32_t count = rd<int32_t>(el, 0x15c);
    if (count > 14) count = 14;
    uint32_t v = 0;
    for (int32_t i = 0; i < count; ++i) {
        uint8_t *tab = el + 0xc + 24 * i;
        if (tab_shown(base, rd<uint32_t>(tab, 0))) {
            *reinterpret_cast<uint8_t **>(el + 0x160 + 8 * v) = tab;
            v += 1;
        }
    }
    *reinterpret_cast<uint32_t *>(el + 0x1d0) = v;
    eb[206] = v;

    // 4. pad 0
    uint8_t *pads = at<uint8_t *>(base + PADS);
    if (!ptr_ok(pads)) return;
    uint8_t *pad = pads + 0x7e0;

    // 5. cursor move, unless cancel is held
    if (!(rd<uint32_t>(pad, 0x9c) & 0x4000)) {
        const uint32_t waiting = rd<uint32_t>(el, 8);
        if (waiting != 0) {
            u3x_k23_action(base, waiting);
            *reinterpret_cast<uint32_t *>(el + 8) = 0;
        }
        const int32_t old = rd<int32_t>(el, 0);
        int32_t c = old;
        if (u3x_k23_rep(base, pad, 0x80)) c -= 1;
        if (u3x_k23_rep(base, pad, 0x20)) c += 1;
        if (c < 0) c = 0;                                   // clamp to 0 first ...
        const int32_t last = static_cast<int32_t>(v) - 1;
        if (c > last) c = last;                             // ... then to the last tab (-1 when none)
        *reinterpret_cast<int32_t *>(el + 0) = c;

        const uint32_t half = 0x3f000000u;
        const uint32_t smooth_old = f_mul(rd<uint32_t>(el, 4), half);
        const uint32_t cursor = f_from_int(c);
        const uint32_t smooth_new = f_mul(cursor, half);
        *reinterpret_cast<uint32_t *>(el + 4) = f_add(smooth_old, smooth_new);

        if (c != old) {
            game_call<void>(base + SOUND_START, static_cast<void *>(click_sound), 0xf57d53ceu, 0u);
            game_call<void>(base + SOUND_PLAY, static_cast<void *>(click_sound), 0u);
        }
    }

    // 6. time stamp and alpha fade
    const bool page_up = at<uint32_t>(base + MENU_PAGE_UP) != 0;
    *reinterpret_cast<uint32_t *>(el + 0x1d8) = page_up ? now - 3000u : now;
    const uint32_t target = page_up ? 0u : F_ONE;
    const uint32_t alpha_old = f_mul(rd<uint32_t>(el, 0x1d4), 0x3f333333u);
    const uint32_t alpha_add = f_mul(target, 0x3e99999au);
    const uint32_t alpha = f_add(alpha_old, alpha_add);
    *reinterpret_cast<uint32_t *>(el + 0x1d4) = alpha;

    // 7. bar not shown enough yet
    if (f_less(alpha, 0x3f666666u)) return;

    // 8. confirm button pressed?
    const uint8_t *options = at<uint8_t *>(base + SYS_OPTIONS);
    const uint32_t confirm = (ptr_ok(options) && rd<uint8_t>(options, 0x39b4) == 1) ? 0x4000u : 0x2000u;
    if (!(rd<uint32_t>(pad, 0x94) & confirm)) return;

    // 9. tab under the cursor
    const int32_t c = rd<int32_t>(el, 0);
    const uint8_t *tab = (c >= 0 && c < static_cast<int32_t>(v)) ? rd<uint8_t *>(el, 0x160 + 8 * c) : nullptr;
    if (!u3x_k29_eok(tab)) { eb[205] += 1; return; }

    // 10. send its page, keep its second page waiting
    u3x_k23_action(base, rd<uint32_t>(tab, 0));
    *reinterpret_cast<uint32_t *>(el + 8) = rd<uint32_t>(tab, 4);
}

extern "C" U3X_FN int u3x_cinema_config_push(uintptr_t base, const uint64_t *r) {
    using namespace s04b;
    const uint8_t *cinema = reinterpret_cast<const uint8_t *>(base + NAME_CINEMA);
    if (r[4] != base + NAME_CINEMA) return 0;

    uint32_t *eb = ext_eb();
    eb[199] += 1;
    const uint32_t n = at<uint32_t>(base + CONFIG_COUNT);
    uint64_t list = base + SINGLE_MODULES;
    uint64_t value = 0;

    // scan: an existing cinema entry ends everything; the last single-player entry gives list and value
    const uint8_t *single = reinterpret_cast<const uint8_t *>(base + NAME_SINGLE);
    for (uint32_t i = 0; i < n && i < CONFIG_MAX; ++i) {
        const uint8_t *e = reinterpret_cast<const uint8_t *>(base + CONFIG_TABLE + 0x30 * i);
        if (same_bytes(e, cinema, 20)) return 0;
        if (same_bytes(e, single, 14)) {
            list = rd<uint64_t>(e, 0x20);
            value = rd<uint64_t>(e, 0x28);
        }
    }

    if (n >= CONFIG_MAX) { eb[198] += 1; return 0; }

    uint8_t *e = reinterpret_cast<uint8_t *>(base + CONFIG_TABLE + 0x30 * static_cast<uintptr_t>(n));
    for (uint32_t i = 0; i < 20; ++i) e[i] = cinema[i];
    for (uint32_t i = 20; i < 32; ++i) e[i] = 0;
    *reinterpret_cast<uint64_t *>(e + 0x20) = list;
    *reinterpret_cast<uint64_t *>(e + 0x28) = value;
    at<uint32_t>(base + CONFIG_COUNT) = n + 1;
    eb[197] += 1;
    return 0;
}
