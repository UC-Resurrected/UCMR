// G42 part B: the follow camera and the per-frame camera half of the death / spectate cameras.
// Written from spec G42_death_cameras.md (sections 3, 4.5 to 4.8, 8).

extern "C" {
// other groups, the C part and part A (declared, not written here)
int u3h_mp_code_is(uintptr_t base, uint32_t off, const char *hex12);
int u3h_net_coop_mode(uintptr_t base, void *s);
int u3h_net_late_joining(uintptr_t base);
int32_t u3h_netgame_type(uintptr_t base, void *s);
int u3h_np_alive(void *s, uint8_t *p);
int u3h_np_has_effect(void *s, uint8_t *p, uint32_t h);
void u3h_spawn_fade(uintptr_t base, void *s, uint8_t *p, int on);
int u3h_cam_fixed(uintptr_t base, void *s, uint8_t *p, int32_t idx);
int u3x_coop_spectate(uintptr_t base, void *s, uint8_t *p, uint8_t *menu);
uint32_t u3x_pad_pressed(uintptr_t base, int32_t screen);
uint8_t *u3x_screen_menu(uintptr_t base, int32_t screen);
uint32_t u3x_menu_page(const uint8_t *m);
int u3x_menu_back(uintptr_t base, uint8_t *m, int silent);
void u3x_menu_close(uintptr_t base, uint8_t *m);
uint64_t u3x_call_float_arg(uintptr_t fn, uint32_t f0, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
void u3x_frame_identity(uint8_t *bf);
uint32_t u3x_xz_dist2(const uint32_t *a, const uint32_t *b);
int u3x_bf_handle_valid(uintptr_t base, const uint8_t *bf);

// this part
void u3x_follow_fx(uintptr_t base, void *s, uint8_t *p);
void u3x_follow_aim(uintptr_t base, void *s, uint8_t *p, uint8_t *q);
void u3x_follow_update(uintptr_t base, void *s, uint8_t *p);
void u3x_camera_tick(uintptr_t base, void *s, uint8_t *p, int idx);
}

namespace g42b {

// the usual function start, as the text u3h_mp_code_is compares with
U3X_RO const char usual_start[] = {'5','5','4','8','8','9','e','5','4','1','5','7','4','1','5','6','4','1','5','5','4','1','5','4', 0};

// image offsets
constexpr uint32_t FN_NEXT_PLAYER = 0x7868a0;   // the game's "follow the next player"
constexpr uint32_t FN_ALL_IN = 0x746ab0;
constexpr uint32_t FN_LOCATORS = 0x73a4e0;
constexpr uint32_t FN_ENTITY_LOCATION = 0x10e6f30;
constexpr uint32_t FN_CHARACTER_FRAME = 0x685b80;
constexpr uint32_t FN_EFFECT_UPDATE = 0xc1ccc0;
constexpr uint32_t MANAGER = 0x17cb060;
constexpr uint32_t CAMERA_TYPE = 0x1ae95e0;

constexpr int32_t FIVE_SECONDS = 1500;

template <typename T> [[gnu::always_inline]] inline T &field(uint8_t *o, uintptr_t off) {
    return *reinterpret_cast<T *>(o + off);
}

[[gnu::always_inline]] inline bool code_ok(uintptr_t base, uint32_t off) {
    return u3h_mp_code_is(base, off, usual_start) != 0;
}

// a - b as a 32-bit signed difference
[[gnu::always_inline]] inline int32_t since(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
}

// the object a handle (slot pointer, id at +8) names, or null when it is stale
[[gnu::always_inline]] inline uint8_t *handle_object(const uint8_t *handle) {
    const uint8_t *slot = rd<const uint8_t *>(handle, 0);
    const uint32_t id = rd<uint32_t>(handle, 8);
    if (!slot || !id) return nullptr;
    uint8_t *object = rd<uint8_t *>(slot, 0);
    if (!object || rd<uint32_t>(object, 0x7c) != id) return nullptr;
    return object;
}

[[gnu::always_inline]] inline uint8_t *camera_manager(uintptr_t base, int32_t screen) {
    return handle_object(reinterpret_cast<const uint8_t *>(base + 0x1d52b80 + 0x10 * static_cast<int64_t>(screen)));
}

[[gnu::always_inline]] inline uint8_t *manager(uintptr_t base) {
    return reinterpret_cast<uint8_t *>(base + MANAGER);
}

// net player record i (0..9), null while multiplayer is off
[[gnu::always_inline]] inline uint8_t *record(uintptr_t base, int32_t i) {
    if (!at<uint8_t>(base + 0x17d5c61)) return nullptr;
    return manager(base) + 0x390 + 0x9c0 * static_cast<uintptr_t>(i);
}

[[gnu::always_inline]] inline bool in_game(const uint8_t *q) {
    return q && rd<uint8_t>(q, 0xe1) && !rd<uint8_t>(q, 0x5e4);
}

[[gnu::always_inline]] inline uint32_t *follow_counters() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0x20f0); }
[[gnu::always_inline]] inline uint32_t *coop_counters() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0x100); }

[[gnu::always_inline]] inline uint8_t next_player(uintptr_t base, uint8_t *p, int32_t direction, int32_t first) {
    return game_call<uint8_t, uint8_t *, int32_t, int32_t>(base + FN_NEXT_PLAYER, p, direction, first);
}

// leave the followed player and show the fixed camera `step` after the current one
[[gnu::always_inline]] inline void fixed_step(uintptr_t base, void *s, uint8_t *p, int32_t step) {
    field<uint8_t>(p, 0x381) = 0;
    field<uint8_t>(p, 0x380) = 0;
    const uint32_t index = static_cast<uint32_t>(rd<int32_t>(p, 0x378)) + static_cast<uint32_t>(step);
    u3h_cam_fixed(base, s, p, static_cast<int32_t>(index));
    coop_counters()[14] += 1;
}

// follow the next player in a direction
[[gnu::always_inline]] inline void follow_step(uintptr_t base, uint8_t *p, int32_t direction) {
    field<uint8_t>(p, 0x380) = 0;
    if (next_player(base, p, direction, 0)) field<uint8_t>(p, 0x381) = 1;
    coop_counters()[15] += 1;
}

// index of the camera locator nearest to p (horizontally); 0 when none is usable
U3X_FN int32_t nearest_locator(uintptr_t base, uint8_t *p) {
    uint8_t *m = manager(base);
    const int32_t count = rd<int32_t>(m, 0x6a20);
    alignas(16) uint32_t location[8];   // not cleared: the game fills it
    uint32_t best_distance = 0x7f7fffffu;
    int32_t best = 0;
    for (int32_t k = 0; k < count && k < 65; k++) {
        uint8_t *entity = rd<uint8_t *>(m, 0x6820 + 8 * static_cast<uintptr_t>(k));
        if (!entity) continue;
        game_call<void, uint32_t *, uint8_t *>(base + FN_ENTITY_LOCATION, location, entity);
        const uint32_t distance = u3x_xz_dist2(reinterpret_cast<const uint32_t *>(p), location);
        if (distance < best_distance) {   // compared as patterns
            best_distance = distance;
            best = k;
        }
    }
    return best;
}

// nobody is followed and there is no camera: take the nearest fixed camera and remove the body
U3X_FN void take_nearest_camera(uintptr_t base, void *s, uint8_t *p, bool coop) {
    int32_t players = 0;
    bool someone_lives = false;
    for (int32_t i = 0; i < 10; i++) {
        const uint8_t *q = record(base, i);
        if (!in_game(q)) continue;
        players++;
        const bool hunter = coop && rd<int32_t>(q, 0x21c) == 1;
        if (rd<int32_t>(q, 0x39c) <= rd<int32_t>(q, 0x3a0) && !hunter) someone_lives = true;
    }
    if (!someone_lives && players != 1) return;
    if (!code_ok(base, FN_LOCATORS) || !code_ok(base, FN_ENTITY_LOCATION)) return;
    game_call<void, uint8_t *>(base + FN_LOCATORS, manager(base));
    const int32_t best = nearest_locator(base, p);
    if (!u3h_cam_fixed(base, s, p, best)) return;
    field<uint8_t>(p, 0x381) = 0;
    field<int32_t>(p, 0x37c) = -1;
    field<uint8_t>(p, 0x380) = 0;
    uint8_t *body = handle_object(p + 0x10);
    if (body) {
        const uintptr_t kill = rd<uintptr_t>(s, 768 + 8 * 57);
        game_call<void, uint8_t *>(kill, body);
    }
}

}  // namespace g42b

// 4.5: the damage screen effect of p's screen gets its update with full health
extern "C" U3X_FN void u3x_follow_fx(uintptr_t base, void *s, uint8_t *p) {
    using namespace g42b;
    const int32_t screen = rd<int32_t>(p, 0x1e8);
    if (screen < 0 || screen > 1) return;
    uint8_t *effect = at<uint8_t *>(base + 0x16fd020 + 8 * static_cast<uintptr_t>(screen));
    if (!effect) return;
    const uint8_t *table = rd<const uint8_t *>(effect, 0);
    if (!table || rd<uintptr_t>(table, 0x28) != base + FN_EFFECT_UPDATE) return;
    if (!code_ok(base, FN_EFFECT_UPDATE)) return;

    uint32_t hurt = 0;
    const uint8_t *clock = at<const uint8_t *>(base + 0x17c3580);
    // the timer is later than the clock: a plain signed comparison, no difference formed
    if (clock && rd<int32_t>(p, 0x9a0) > rd<int32_t>(clock, 0xc)) hurt = 1;
    else if (u3h_np_has_effect(s, p, 0x116b3043u)) hurt = 1;
    else if (at<uint8_t>(base + 0x17d7b05)) hurt = 1;

    u3x_call_float_arg(base + FN_EFFECT_UPDATE, F_ONE, reinterpret_cast<uint64_t>(effect), hurt, 0, 0);
    uint32_t *fcam = follow_counters();
    fcam[0] += 1;
    fcam[1] = hurt;
}

// 4.6: p's follow camera looks from the camera frame of the followed player q
extern "C" U3X_FN void u3x_follow_aim(uintptr_t base, void *, uint8_t *p, uint8_t *q) {
    using namespace g42b;
    uint32_t *fcam = follow_counters();
    uint8_t *character = handle_object(q + 0x10);
    if (!character) { fcam[4] += 1; return; }

    // the identity bound frame and the default fov
    alignas(16) uint8_t frame[0x60];
    for (int i = 0; i < 0x60; i++) frame[i] = 0;
    field<uint32_t>(frame, 0) = 0x7fccccccu;
    field<uint32_t>(frame, 4) = 0x7fccccccu;
    field<uint32_t>(frame, 0x3c) = F_ONE;
    field<uint32_t>(frame, 0x5c) = F_ONE;
    uint32_t fov = 0x42960000u;

    const uint32_t cls = rd<uint32_t>(character, 0x30);
    if (cls == 0xf2c86c22u) {   // a net character: the game gives frame and fov
        if (!code_ok(base, FN_CHARACTER_FRAME) ||
            !game_call<uint8_t, uint8_t *, uint8_t *, uint32_t *>(base + FN_CHARACTER_FRAME, character + 0x400, frame, &fov)) {
            fcam[4] += 1;
            return;
        }
        fcam[2] += 1;
    } else if (cls == 0xa5a63e9bu) {   // a local Player: where the target's screen camera is
        const uint8_t *cm = camera_manager(base, rd<int32_t>(q, 0x1e8));
        if (!cm) { fcam[4] += 1; return; }
        for (int i = 0; i < 0x20; i++) frame[0x40 + i] = cm[0x4c0 + i];
        fcam[3] += 1;
    } else {
        fcam[8] += 1;
    }

    if (at<uint8_t>(base + 0x17d7b10)) { fcam[5] += 1; return; }
    uint8_t *camera = handle_object(p + 0x388);
    if (!camera) return;
    if (rd<uintptr_t>(camera, 0xf0) != base + CAMERA_TYPE) { fcam[6] += 1; return; }

    // a body that is gone: drop the handle, keep the parent location
    if (rd<uint64_t>(frame, 8) != 0 && !u3x_bf_handle_valid(base, frame)) {
        field<uint64_t>(frame, 8) = 0;
        field<uint32_t>(frame, 0x10) = 0;
    }
    for (int i = 8; i < 0x60; i++) camera[0x230 + i] = frame[i];
    field<uint32_t>(camera, 0x294) = fov;
    fcam[7] = fov;
}

// 4.7: keep following the target, or move on when it can no longer be followed
extern "C" U3X_FN void u3x_follow_update(uintptr_t base, void *s, uint8_t *p) {
    using namespace g42b;
    if (!rd<uint8_t>(p, 0x381) || !handle_object(p + 0x388)) return;
    if (!code_ok(base, FN_NEXT_PLAYER)) return;
    u3x_follow_fx(base, s, p);

    const bool elimination = u3h_netgame_type(base, s) == 10;
    const bool any_team = elimination || rd<uint8_t>(manager(base), 0x34e) != 0;
    const int32_t index = rd<int32_t>(p, 0x37c);
    uint8_t *target = index >= 0 && index < 10 ? record(base, index) : nullptr;
    // p's own team is read from memory at each use, after the call before it
    if (in_game(target) && u3h_np_alive(s, target) &&
        (any_team || rd<int32_t>(target, 0x21c) == rd<int32_t>(p, 0x21c) ||
         (u3h_net_coop_mode(base, s) && rd<int32_t>(p, 0x21c) == 1))) {
        u3x_follow_aim(base, s, p, target);
        return;
    }

    if (!next_player(base, p, 1, 0)) {
        field<uint8_t>(p, 0x381) = 0;
        field<uint8_t>(p, 0x380) = 0;
        u3h_cam_fixed(base, s, p, rd<int32_t>(p, 0x378));
    }
    coop_counters()[16] += 1;
}

// 4.8: once per frame for local net player p
extern "C" U3X_FN void u3x_camera_tick(uintptr_t base, void *s, uint8_t *p, int) {
    using namespace g42b;
    uint8_t *m = manager(base);
    const int32_t now = at<int32_t>(base + 0x17b4ca4);
    const int32_t death = rd<int32_t>(p, 0x39c);
    const int32_t team = rd<int32_t>(p, 0x21c);

    // 1. fades
    const bool coop = u3h_net_coop_mode(base, s) != 0;
    const bool joining = u3h_net_late_joining(base) != 0;
    bool late = joining && (!coop || team != 1);
    if (rd<uint8_t>(m, 0x6a7c)) u3h_spawn_fade(base, s, p, 1);
    if (late && !(rd<uint32_t>(u3h_ext(), 0x4450) & 2) && u3h_netgame_type(base, s) == 10) late = false;
    if (late) u3h_spawn_fade(base, s, p, 1);
    if (code_ok(base, FN_ALL_IN) && (!rd<uint8_t>(m, 0x31e) || !game_call<uint8_t, uint8_t *>(base + FN_ALL_IN, m))) {
        u3h_spawn_fade(base, s, p, 1);
        field<uint32_t>(u3h_ext(), 0x20a0) += 1;
    }

    // 2. alive, late or match over: no death camera, the class page goes away
    uint8_t *menu = u3x_screen_menu(base, rd<int32_t>(p, 0x1e8));
    const bool alive = u3h_np_alive(s, p) != 0;
    if (alive || late || rd<uint8_t>(m, 0x6a24)) {
        if (u3x_menu_page(menu) == 0xd3a938b7u) {
            u3x_menu_back(base, menu, 0);
            u3x_menu_close(base, menu);
        }
        return;
    }

    // 3. the co-op hunter's own spectate step
    const bool long_dead = since(now, death) > FIVE_SECONDS;
    if ((death == 0 || long_dead) && coop && team == 1 && u3x_coop_spectate(base, s, p, menu)) return;

    // 4. preconditions
    if (!code_ok(base, FN_NEXT_PLAYER)) return;
    if (!camera_manager(base, rd<int32_t>(p, 0x1e8))) return;

    // 5. choosing a camera
    const bool gone = long_dead || !handle_object(p + 0x10);
    if (!rd<uint8_t>(p, 0x381)) {
        bool chosen = false;
        if (gone) {
            if (next_player(base, p, 1, 1)) {
                field<uint8_t>(p, 0x380) = 0;
                reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(s) + 0x3d78)[12] += 1;
            }
            chosen = rd<uint8_t>(p, 0x381) != 0;
        }
        if (!chosen && !handle_object(p + 0x388)) {
            if (gone) take_nearest_camera(base, s, p, coop);
            return;
        }
    }

    // 6. stepping: by itself after 5 s, or by the pad while no menu is up
    const bool following = rd<uint8_t>(p, 0x381) != 0;
    if (rd<uint8_t>(p, 0x380) && since(now, rd<int32_t>(p, 0x384)) > FIVE_SECONDS) {
        const uint32_t index = static_cast<uint32_t>(rd<int32_t>(p, 0x378)) + 1u;
        u3h_cam_fixed(base, s, p, static_cast<int32_t>(index));
    } else {
        const uint8_t *menu0 = at<const uint8_t *>(base + 0x16e3188);
        if (menu0 && rd<uint32_t>(menu0, 8) == 0) {
            const uint32_t pressed = u3x_pad_pressed(base, rd<int32_t>(p, 0x1e8));
            const bool right = (pressed & 0x20) != 0;
            const bool left = (pressed & 0x80) != 0;
            if (!following) {
                if (right) fixed_step(base, s, p, 1);
                else if (left) fixed_step(base, s, p, -1);
            } else {
                if (left) follow_step(base, p, 1);
                else if (right) follow_step(base, p, -1);
            }
        }
    }

    // 7.
    u3x_follow_update(base, s, p);
}
