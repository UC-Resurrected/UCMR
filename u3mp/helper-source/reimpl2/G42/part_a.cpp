// G42 part A: the fixed death camera (spec G42_death_cameras.md, sections 4.1 to 4.4).

extern "C" int u3h_net_coop_mode(uintptr_t base, void *s);
extern "C" uint32_t u3x_entity_tag(const uint8_t *e, uint32_t tag, uint32_t fallback);

extern "C" uint32_t u3x_entity_sid(uintptr_t base, const uint8_t *e);
extern "C" uint32_t u3x_xz_dist2(const uint32_t *a, const uint32_t *b);
extern "C" int u3x_bf_handle_valid(uintptr_t base, const uint8_t *bf);
extern "C" int u3x_cam_fixed(uintptr_t base, void *s, uint8_t *p, int32_t idx);

namespace g42a {

// image offsets
constexpr uintptr_t MANAGER = 0x17cb060;        // net game manager
constexpr uintptr_t NET_CLOCK = 0x17b4ca4;
constexpr uintptr_t BODY_TABLE = 0x16e2f30;     // pointer to the rigid-body handle table
constexpr uintptr_t CAMERA_TYPE = 0x1ae95e0;
constexpr uintptr_t STREAMER = 0x1afcf10;
constexpr uintptr_t CAMERA_MANAGERS = 0x1d52b80;
constexpr uintptr_t FN_MATCH_LEVEL = 0x72fb00;
constexpr uintptr_t FN_LIST_LOCATORS = 0x73a4e0;
constexpr uintptr_t FN_ADD_CAMERA = 0x101b530;
constexpr uintptr_t FN_ENTITY_LOCATION = 0x10e6f30;

// the helper's own safety check: expected first bytes of the game functions it calls
U3X_RO const uint8_t start_usual[12] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54};
U3X_RO const uint8_t start_level[12] = {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x89, 0xfb, 0xbf, 0x7d, 0x06};

U3X_FN bool code_is(uintptr_t base, uintptr_t off, const uint8_t *expected) {
    const uint8_t *code = reinterpret_cast<const uint8_t *>(base + off);
    for (int i = 0; i < 12; i++)
        if (code[i] != expected[i]) return false;
    return true;
}

// the object a 16-byte handle (slot pointer, id) names, or null when it is stale
U3X_FN uint8_t *handle_object(const uint8_t *handle) {
    const uint8_t *const *slot = rd<const uint8_t *const *>(handle, 0);
    const uint32_t id = rd<uint32_t>(handle, 8);
    if (!slot || id == 0) return nullptr;
    const uint8_t *object = *slot;
    if (!object || rd<uint32_t>(object, 0x7c) != id) return nullptr;
    return const_cast<uint8_t *>(object);
}

template <typename T> [[gnu::always_inline]] inline void put(uint8_t *p, uintptr_t off, T value) {
    *reinterpret_cast<T *>(p + off) = value;
}

}  // namespace g42a

// The name id under which the match's level lists entity e (0 = not listed).
extern "C" U3X_FN uint32_t u3x_entity_sid(uintptr_t base, const uint8_t *e) {
    using namespace g42a;
    const uint8_t *level = game_call<const uint8_t *, uintptr_t>(base + FN_MATCH_LEVEL, base + MANAGER);
    if (!level) return 0;
    const uint8_t *lists = rd<const uint8_t *>(level, 0x190);
    if (!lists) return 0;
    const uint8_t *entries = rd<const uint8_t *>(lists, 0x18);
    const int32_t count = rd<int32_t>(lists, 0x20);
    if (!entries) return 0;
    for (int32_t i = 0; i < count; i++) {
        const uint8_t *entry = entries + 16 * static_cast<intptr_t>(i);
        if (rd<const uint8_t *>(entry, 8) == e) return rd<uint32_t>(entry, 0);
    }
    return 0;
}

// Squared horizontal distance of two positions, as a float pattern with the sign bit cleared.
extern "C" U3X_FN uint32_t u3x_xz_dist2(const uint32_t *a, const uint32_t *b) {
    const uint32_t dx = f_sub(a[0], b[0]);
    const uint32_t dz = f_sub(a[2], b[2]);
    const uint32_t xx = f_mul(dx, dx);
    const uint32_t zz = f_mul(dz, dz);
    return f_add(xx, zz) & 0x7fffffffu;
}

// 1 when the body handle of bound frame bf still names its body.
extern "C" U3X_FN int u3x_bf_handle_valid(uintptr_t base, const uint8_t *bf) {
    using namespace g42a;
    const uintptr_t slot = rd<uintptr_t>(bf, 8);
    const uint32_t id = rd<uint32_t>(bf, 0x10);
    if (!slot || id == 0) return 0;
    const uint8_t *body = at<const uint8_t *>(slot);
    if (!body) return 0;
    const uint32_t body_id = rd<uint32_t>(body, 0x50);
    const bool same = body_id == id || (id <= 0xfffeu && (body_id & 0xffffu) == id);
    if (!same) return 0;
    const uint64_t number = (static_cast<uint64_t>(slot) - at<uint64_t>(base + BODY_TABLE)) / 8;
    return rd<uint16_t>(body, 0x54) == static_cast<uint16_t>(number) ? 1 : 0;
}

// Puts net player p's screen on a fixed camera: at camera locator idx, or at the co-op hunter's selected AI.
extern "C" U3X_FN int u3x_cam_fixed(uintptr_t base, void *s, uint8_t *p, int32_t idx) {
    using namespace g42a;
    if (!code_is(base, FN_LIST_LOCATORS, start_usual) || !code_is(base, FN_ADD_CAMERA, start_usual) ||
        !code_is(base, FN_MATCH_LEVEL, start_level))
        return 0;

    uint8_t *manager = reinterpret_cast<uint8_t *>(base + MANAGER);
    game_call<void, uint8_t *>(base + FN_LIST_LOCATORS, manager);

    // a co-op hunter may watch the AI it selected
    const int coop = u3h_net_coop_mode(base, s);
    int32_t ai = 0;
    if (coop && rd<int32_t>(p, 0x21c) == 1) ai = rd<int32_t>(p, 0x600);

    const int32_t locators = rd<int32_t>(manager, 0x6a20);
    if (locators <= 0 && ai == 0) return 0;

    uint8_t *locator = nullptr;
    if (locators > 0) {
        if (idx < 0) idx += locators;
        const int32_t slot = idx % locators;
        if (slot < 0) return 0;
        locator = rd<uint8_t *>(manager, 0x6820 + 8 * static_cast<uintptr_t>(slot));
    }
    if (!locator && ai == 0) return 0;

    // end the camera the player has now
    if (uint8_t *old_camera = handle_object(p + 0x388)) {
        const uintptr_t end = rd<uintptr_t>(rd<const uint8_t *>(old_camera, 0), 0x158);
        game_call<void, uint8_t *>(end, old_camera);
    }

    // spawn parameters
    alignas(16) uint8_t block[0x60];
    for (unsigned i = 0; i < sizeof block; i++) block[i] = 0;
    put<uint32_t>(block, 0, 0x50000064u);
    put<uint32_t>(block, 4, locator ? u3x_entity_sid(base, locator) : 0u);
    put<uint32_t>(block, 0xc, 0x42aa0000u);    // fov 85
    put<uint32_t>(block, 0x10, 0xbf800000u);   // -1
    put<uint32_t>(block, 0x14, 0xbf800000u);

    const int64_t screen = rd<int32_t>(p, 0x1e8);
    uint8_t *camera_manager = handle_object(reinterpret_cast<const uint8_t *>(base + CAMERA_MANAGERS) + 0x10 * screen);

    uint8_t *place = locator;
    if (ai != 0) {
        const uintptr_t find_spawner = rd<uintptr_t>(s, 768 + 8 * 58);
        const uintptr_t streamer = base + STREAMER;
        const uint32_t ai_id = static_cast<uint32_t>(ai);
        if (uint8_t *spawner = game_call<uint8_t *, uintptr_t, uint32_t>(find_spawner, streamer, ai_id)) {
            const uint32_t camera_id = u3x_entity_tag(spawner, 0x24f280a0u, ai_id);
            if (uint8_t *camera_spawner = game_call<uint8_t *, uintptr_t, uint32_t>(find_spawner, streamer, camera_id)) {
                place = camera_spawner;
                put<uint32_t>(block, 4, camera_id);
            } else {
                place = spawner;
                put<uint32_t>(block, 4, ai_id);
            }
            reinterpret_cast<uint32_t *>(u3h_ext() + 0x100)[11] += 1;
        }
    }

    const bool screen_location = rd<uint8_t>(p, 0x7a0) != 0 && camera_manager;
    if (!place && !screen_location) return 0;
    if (screen_location) {
        for (unsigned i = 0; i < 0x20; i++) block[0x20 + i] = camera_manager[0x4c0 + i];
    } else {
        game_call<void, uint8_t *, uint8_t *>(base + FN_ENTITY_LOCATION, block + 0x20, place);
    }
    block[0x50] = 1;
    block[0x51] = 1;
    if (!camera_manager) return 0;

    uint64_t type = base + CAMERA_TYPE;
    uint64_t flags = 0x0000000200000000ull;
    uint8_t *camera = game_call<uint8_t *, uint8_t *, uint64_t *, uint64_t *, uint8_t *>(base + FN_ADD_CAMERA, camera_manager,
                                                                                      &type, &flags, block);

    put<uint64_t>(p, 0x388, camera ? rd<uint64_t>(camera, 0x70) : 0);
    put<uint32_t>(p, 0x390, camera ? rd<uint32_t>(camera, 0x7c) : 0);
    put<int32_t>(p, 0x378, idx);
    put<int32_t>(p, 0x384, at<int32_t>(base + NET_CLOCK));
    reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(s) + 0x3d78)[11] += 1;
    return camera ? 1 : 0;
}
