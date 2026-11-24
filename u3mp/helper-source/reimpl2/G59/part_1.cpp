// G59 part 1: lobby menu scene enter / leave, and the two multiplayer particle modules.
// Written from spec G59_lobby_scene.md (sections 3, 4.1 to 4.7, 6). No float in this part.

// ---- the C part and other groups (spec 2.2) ----
extern "C" uint8_t *u3h_netgame_entry(uintptr_t base, void *s);
extern "C" int u3h_coop(uintptr_t base, void *s);
extern "C" uint32_t *u3h_scene_was(void);
extern "C" uint32_t u3h_crc_sid(const char *text);

// ---- this part (spec 2.1) ----
extern "C" int u3x_scene_hunter(uintptr_t base, void *s, const uint8_t *p);
extern "C" int u3x_scene_free(uintptr_t base, void *s);
extern "C" void u3x_lobby_scene(uintptr_t base, void *s);
extern "C" uint32_t u3x_fnv1a_id(uint32_t id);
extern "C" int u3x_module_loaded(const uint8_t *modules, uint32_t id);
extern "C" int u3x_part_module_wanted(uintptr_t base, uint32_t id);
extern "C" void u3x_part_modules_tick(uintptr_t base);

namespace g59_part1 {

// names whose ids the game's name-id rule generates (spec 3.2)
U3X_RO const char NAME_TASK_ONLINE[] = "TaskManagerOnline";
U3X_RO const char NAME_MODULE_0[] = "part-multiplayer";
U3X_RO const char NAME_MODULE_1[] = "part-obj-shade-char";

// image offsets: game data (spec 3.3)
constexpr uintptr_t MENU_SCENE = 0x17bba90;
constexpr uintptr_t MENU = 0x17bd080;
constexpr uintptr_t NET_PLAYER_0 = 0x17cb3d0;
constexpr uintptr_t MATCH_STATE = 0x17d1868;
constexpr uintptr_t MP_ACTIVE = 0x17d5c61;
constexpr uintptr_t VOTE_CLOSED = 0x17d7761;
constexpr uintptr_t CUSTOM_MANAGER = 0x17f9938;
constexpr uintptr_t PARTICLE_MANAGER = 0x1a4d910;
constexpr uintptr_t WANT_MAP = 0x1a4d960;
constexpr uintptr_t VIEW_HOLDER_HANDLE = 0x1b3ee38;
constexpr uintptr_t PLAYER_HANDLE = 0x1bb9500;

// image offsets: game functions (spec 3.1)
constexpr uintptr_t FN_SCENE_LEAVE = 0x5b7040;
constexpr uintptr_t FN_SCENE_CAMERA = 0x5b4e60;
constexpr uintptr_t FN_SCENE_ENTER = 0x5b7020;
constexpr uintptr_t FN_SCENE_TEAMS = 0x5b6e40;
constexpr uintptr_t FN_PLAYER_LOOK = 0x784b20;
constexpr uintptr_t FN_LOOK_LOADED = 0x996a60;
constexpr uintptr_t FN_MODULE_WANT = 0xeb4110;
constexpr uintptr_t FN_MODULE_GIVE_BACK = 0xeb41e0;

constexpr int32_t IN_A_MATCH = 2;
constexpr int32_t COOP_ADVENTURE = 11;
constexpr int32_t TEAM_HUNTERS = 1;
constexpr int MAX_CHAIN_NODES = 4096;

[[gnu::always_inline]] inline void count(uint32_t *words, int n) { words[n] = words[n] + 1; }

[[gnu::always_inline]] inline uint32_t *ext_ec() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0x4450); }

// the object of a live handle {pointer to pointer, u32 process id}, or null
U3X_FN const uint8_t *handle_object(const uint8_t *handle) {
    const uint8_t *const *slot = rd<const uint8_t *const *>(handle, 0);
    const uint32_t id = rd<uint32_t>(handle, 8);
    if (!slot || !id) return nullptr;
    const uint8_t *object = *slot;
    if (!object || rd<uint32_t>(object, 0x7c) != id) return nullptr;
    return object;
}

// why the menu scene has to be left: 1 = the menu is off, 2 = an online task screen holds the view, 0 = no reason
U3X_FN int leave_reason(uintptr_t base) {
    const uint8_t *menu = reinterpret_cast<const uint8_t *>(base + MENU);
    if (rd<int32_t>(menu, 8) == 0 || rd<int32_t>(menu, 0xc) != 0) return 1;
    const uintptr_t handle = (ext_eb()[1] & 0x100u) ? PLAYER_HANDLE : VIEW_HOLDER_HANDLE;
    const uint8_t *holder = handle_object(reinterpret_cast<const uint8_t *>(base + handle));
    if (holder && rd<uint32_t>(holder, 0x30) == u3h_crc_sid(NAME_TASK_ONLINE)) return 2;
    return 0;
}

// may the scene be entered this frame (the camera already ran)
U3X_FN bool may_enter(uintptr_t base, void *s) {
    uint8_t *cm = reinterpret_cast<uint8_t *>(base + CUSTOM_MANAGER);
    if (rd<uint8_t>(cm, 0xf70) == 0) return true;   // manager not active: nothing forbids it

    const uint8_t *popup = rd<const uint8_t *>(reinterpret_cast<const uint8_t *>(base + MENU), 0x898);
    if (popup && rd<uint32_t>(popup, 0x10) != 0) {
        if (at<uint8_t>(base + VOTE_CLOSED) != 0) return false;
        if (u3x_scene_free(base, s) == 0) return false;
    }

    uint8_t *player = at<uint8_t *>(base + NET_PLAYER_0);
    if (!player) return false;
    if (u3x_scene_hunter(base, s, player) != 0) {
        // a hunter whose look is completely loaded gets no menu scene
        void *look = game_call<void *, uint8_t *, uint32_t>(base + FN_PLAYER_LOOK, player, 0u);
        const uint32_t loaded = game_call<uint32_t, uint8_t *, void *>(base + FN_LOOK_LOADED, cm, look);
        if (loaded & 0xffu) return false;
    }
    return true;
}

}  // namespace g59_part1

// 4.1: is net player p a co-op hunter
extern "C" U3X_FN int u3x_scene_hunter(uintptr_t base, void *s, const uint8_t *p) {
    using namespace g59_part1;
    if (at<uint8_t>(base + MP_ACTIVE) == 0) return 0;
    if (u3h_coop(base, s) == 0) return 0;
    return rd<int32_t>(p, 0x21c) == TEAM_HUNTERS ? 1 : 0;
}

// 4.2: may the scene be entered while a popup is up
extern "C" U3X_FN int u3x_scene_free(uintptr_t base, void *s) {
    using namespace g59_part1;
    const uint8_t *entry = u3h_netgame_entry(base, s);
    if (entry && rd<int32_t>(entry, 0xc) == COOP_ADVENTURE) return 0;
    const uint8_t *player = at<const uint8_t *>(base + NET_PLAYER_0);
    if (!player) return 0;
    return u3x_scene_hunter(base, s, player) == 0 ? 1 : 0;
}

// 4.3: the scene's frame step
extern "C" U3X_FN void u3x_lobby_scene(uintptr_t base, void *s) {
    using namespace g59_part1;
    if (ext_eb()[1] & 8u) return;
    count(ext_eb(), 152);

    void *scene = reinterpret_cast<void *>(base + MENU_SCENE);
    const int reason = leave_reason(base);
    if (reason) {
        game_call<void, void *, int32_t>(base + FN_SCENE_LEAVE, scene, 1);
        count(ext_eb(), 154);
        count(ext_eb(), reason == 1 ? 28 : 29);
        *u3h_scene_was() = 0;   // nothing reads this cell; the write is kept
    } else {
        game_call<void, void *>(base + FN_SCENE_CAMERA, scene);
        if (may_enter(base, s)) {
            game_call<void, void *>(base + FN_SCENE_ENTER, scene);
            count(ext_eb(), 153);
            if (at<int32_t>(base + MATCH_STATE) == IN_A_MATCH) count(ext_eb(), 157);
        }
    }

    if (at<int32_t>(base + MATCH_STATE) == IN_A_MATCH) {
        game_call<void, void *>(base + FN_SCENE_TEAMS, scene);
        count(ext_eb(), 155);
    }
}

// 4.4: public 32-bit FNV-1a over the id's four bytes, lowest first
extern "C" U3X_FN uint32_t u3x_fnv1a_id(uint32_t id) {
    uint32_t hash = 0x811c9dc5u;
    for (int i = 0; i < 4; i++) {
        hash ^= (id >> (8 * i)) & 0xffu;
        hash *= 0x01000193u;
    }
    return hash;
}

// 4.5: is the module among the particle manager's loaded-module records
extern "C" U3X_FN int u3x_module_loaded(const uint8_t *modules, uint32_t id) {
    const uint32_t records = rd<uint32_t>(modules, 0x8c8);
    if (records > 64) return 0;
    for (uint32_t k = 0; k < records; k++)
        if (rd<uint32_t>(modules, 0x298 + 0x30 * static_cast<uintptr_t>(k)) == id) return 1;
    return 0;
}

// 4.6: is the module in the want map: 1 yes, 0 no, -1 the map is not usable
extern "C" U3X_FN int u3x_part_module_wanted(uintptr_t base, uint32_t id) {
    using namespace g59_part1;
    const uint8_t *map = reinterpret_cast<const uint8_t *>(base + WANT_MAP);
    const int32_t buckets = rd<int32_t>(map, 0x4c);
    if (buckets < 1 || buckets > 0x100000) return -1;
    const uint8_t *const *heads = rd<const uint8_t *const *>(map, 0x10);
    if (!ptr_ok(heads)) return -1;

    const uint8_t *node = heads[u3x_fnv1a_id(id) % static_cast<uint32_t>(buckets)];
    for (int seen = 0; seen < MAX_CHAIN_NODES && node; seen++) {
        if (!ptr_ok(node)) return -1;
        if (rd<uint32_t>(node, 0x10) == id) return node == heads[buckets] ? 0 : 1;   // the end node does not count
        node = rd<const uint8_t *>(node, 0);
    }
    return 0;   // also for a chain longer than the limit
}

// 4.7: want the two particle modules while multiplayer runs, give them back afterwards
extern "C" U3X_FN void u3x_part_modules_tick(uintptr_t base) {
    using namespace g59_part1;
    uint32_t *ec = ext_ec();
    uint32_t mine = ec[209];
    const bool on = !(ec[1] & 8u) && at<uint8_t>(base + MP_ACTIVE) != 0;
    count(ec, 204);

    void *map = reinterpret_cast<void *>(base + WANT_MAP);
    const uint32_t ids[2] = {u3h_crc_sid(NAME_MODULE_0), u3h_crc_sid(NAME_MODULE_1)};

    for (int k = 0; k < 2; k++) {
        const uint32_t bit = 1u << k;
        if (on && !(mine & bit)) {
            const int wanted = u3x_part_module_wanted(base, ids[k]);
            if (wanted < 0) {
                count(ec, 207);
                continue;
            }
            if (wanted == 0) {
                game_call<void, void *, uint32_t>(base + FN_MODULE_WANT, map, ids[k]);
                count(ec, 205);
            }
            mine |= bit;   // also when the game wanted it already (no reference taken)
        } else if (!on && (mine & bit)) {
            if (u3x_part_module_wanted(base, ids[k]) > 0)
                game_call<void, void *, uint32_t>(base + FN_MODULE_GIVE_BACK, map, ids[k]);
            mine &= ~bit;
            count(ec, 206);
        }
    }

    // status word: own marks, loaded flags, want answers as they are now
    const uint8_t *manager = reinterpret_cast<const uint8_t *>(base + PARTICLE_MANAGER);
    uint32_t status = mine;
    status |= static_cast<uint32_t>(u3x_module_loaded(manager, ids[0])) * 0x100u;
    status |= static_cast<uint32_t>(u3x_module_loaded(manager, ids[1])) * 0x200u;
    status |= (static_cast<uint32_t>(u3x_part_module_wanted(base, ids[0])) & 0xffu) * 0x10000u;
    status |= (static_cast<uint32_t>(u3x_part_module_wanted(base, ids[1])) & 0xffu) * 0x1000000u;
    ec[208] = status;
    ec[209] = mine;
}
