// G54 part 1: the taunt package wants of a match and the "previous taunt" store.
// Written from spec G54_taunt_kit_packages.md (sections 3, 4.1 to 4.3, 6).

extern "C" int u3h_mp_code_is(uintptr_t base, uint32_t off, const char *hex12);
extern "C" const char *u3h_taunt_row_name(uint32_t row);

extern "C" void u3x_taunt_want(uintptr_t base, uint32_t id);
extern "C" void u3x_taunt_start_record(uintptr_t base, uint8_t *controller);
extern "C" void u3x_match_taunt_wants(uintptr_t base);

namespace g54_part1 {

// image offsets
constexpr uintptr_t FN_SYMBOL_LOOKUP = 0x11f31b0;   // (symbol id, module id, 0) -> data or null
constexpr uintptr_t FN_NAME_HASH = 0x10e95b0;       // (name) -> 64-bit hash
constexpr uintptr_t FN_ACTOR_WANT = 0x10ebb90;      // (level manager, request block)
constexpr uintptr_t PTR_NAME_ID_FN = 0x1702720;     // pointer to (0, name, 0) -> 32-bit name id
constexpr uintptr_t LEVEL_MANAGER = 0x1afcf10;
constexpr uintptr_t PLAYER0_HANDLE = 0x1bb9500;
constexpr uintptr_t PLAYER0_PROCESS = 0x1bb9508;

constexpr uint32_t SID_TAUNT_TABLE = 0xb2b6e512u;
constexpr uint32_t SID_NET_MODULE = 0x4240ef2eu;
constexpr uint32_t SID_NO_TAUNT = 0x0e69839du;

// first 12 code bytes of the three game functions, as u3h_mp_code_is wants them
U3X_RO const char code_lookup[] = {'5','5','4','8','8','9','e','5','4','1','5','6','5','3','4','8','8','3','e','c','1','0','4','9',0};
U3X_RO const char code_hash[] = {'5','5','4','8','8','9','e','5','4','1','5','7','4','1','5','6','4','1','5','5','4','1','5','4',0};
U3X_RO const char code_want[] = {'5','5','4','8','8','9','e','5','4','1','5','7','4','1','5','6','5','3','5','0','4','9','8','9',0};

// ec words of the helper's state block
enum : uint32_t { EC_SWITCHES = 0, EC_SETUP_RUNS = 31, EC_WANTED = 32, EC_LAST_WANTED = 33, EC_NO_NAME = 34,
                  EC_STARTS = 35, EC_LAST_PREVIOUS = 36 };
[[gnu::always_inline]] inline uint32_t *ec() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0x4450); }

// the previous-taunt store: a record with a null controller is free; records are never released
struct Previous {
    uint8_t *controller;
    uint32_t player;
    uint32_t taunt;
};
U3X_BSS static Previous store[8];

// request block of the actor want (0x18 bytes)
struct Request {
    uint64_t hash;
    uint32_t sid;
    uint16_t number;
    uint8_t flags;
    uint8_t zero0;
    uint64_t zero1;
};

using NameId = uint32_t (*)(uint64_t, const char *, uint64_t);

// process id of a controller's owner, 0 without owner
[[gnu::always_inline]] inline uint32_t owner_process(const uint8_t *controller) {
    const uint8_t *owner = rd<const uint8_t *>(controller, 0x118);
    return owner ? rd<uint32_t>(owner, 0x7c) : 0;
}

}  // namespace g54_part1

// Wants the actor package of taunt `id`.
U3X_FN void u3x_taunt_want(uintptr_t base, uint32_t id) {
    using namespace g54_part1;
    if (id == SID_NO_TAUNT) return;
    if (!u3h_mp_code_is(base, static_cast<uint32_t>(FN_SYMBOL_LOOKUP), code_lookup)) return;
    if (!u3h_mp_code_is(base, static_cast<uint32_t>(FN_NAME_HASH), code_hash)) return;
    if (!u3h_mp_code_is(base, static_cast<uint32_t>(FN_ACTOR_WANT), code_want)) return;

    const uint8_t *table = game_call<const uint8_t *, uint32_t, uint32_t, uint32_t>(base + FN_SYMBOL_LOOKUP, SID_TAUNT_TABLE,
                                                                                   SID_NET_MODULE, 0u);
    if (!table) return;
    const uint32_t count = rd<uint32_t>(table, 0);
    const uint8_t *rows = rd<const uint8_t *>(table, 8);

    // the first row after row 0 that carries the id; only that row is used
    uint32_t row = 1;
    while (row < count && rd<uint32_t>(rows, 12ull * row) != id) row++;
    if (row >= count) return;

    uint32_t *e = ec();
    const NameId name_id = at<NameId>(base + PTR_NAME_ID_FN);
    const char *name = u3h_taunt_row_name(row);
    if (!name || !name_id || name_id(0, name, 0) != id) {
        e[EC_NO_NAME]++;
        return;
    }

    Request request;
    request.hash = game_call<uint64_t, const char *>(base + FN_NAME_HASH, name);
    request.sid = name_id(0, name, 0);   // asked a second time, after the hash
    request.number = 0;
    request.flags = 1;                   // an actor package wanted in the foreground
    request.zero0 = 0;
    request.zero1 = 0;
    game_call<void, void *, Request *>(base + FN_ACTOR_WANT, reinterpret_cast<void *>(base + LEVEL_MANAGER), &request);

    e[EC_WANTED]++;
    e[EC_LAST_WANTED] = id;
}

// Remembers, per controller and player, the taunt that is current before a new one starts.
U3X_FN void u3x_taunt_start_record(uintptr_t, uint8_t *controller) {
    using namespace g54_part1;
    if (!controller) return;
    const uint32_t player = owner_process(controller);
    uint32_t *e = ec();

    int same = -1, free_one = -1;
    for (int i = 0; i < 8; i++) {
        if (store[i].controller == controller && store[i].player == player) { same = i; break; }
        if (!store[i].controller && free_one < 0) free_one = i;
    }
    const int pick = same >= 0 ? same : free_one >= 0 ? free_one : static_cast<int>(e[EC_STARTS] & 7);

    const uint32_t previous = rd<uint32_t>(controller, 0x128);
    store[pick].controller = controller;
    store[pick].player = player;
    store[pick].taunt = previous;

    e[EC_STARTS]++;
    e[EC_LAST_PREVIOUS] = previous;
}

// Match setup: wants the local player's current taunt and, for the menu's lobby character, the previous one.
U3X_FN void u3x_match_taunt_wants(uintptr_t base) {
    using namespace g54_part1;
    uint32_t *e = ec();
    e[EC_SETUP_RUNS]++;
    if (e[EC_SWITCHES] & 8) return;

    const uint64_t handle = at<uint64_t>(base + PLAYER0_HANDLE);
    const uint32_t process = at<uint32_t>(base + PLAYER0_PROCESS);
    if (!handle || !process) return;
    const uint8_t *player = at<const uint8_t *>(handle);
    if (!player || rd<uint32_t>(player, 0x7c) != process) return;
    uint8_t *controller = rd<uint8_t *>(player, 0x6e8);
    if (!controller) return;

    // entry 6 of the controller's function table: its current taunt id
    const uintptr_t current_fn = rd<uintptr_t>(rd<const uint8_t *>(controller, 0), 6 * 8);
    u3x_taunt_want(base, game_call<uint32_t, uint8_t *>(current_fn, controller));

    if (rd<uint8_t>(player, 0xcea)) {
        const uint32_t owner = owner_process(controller);
        uint32_t previous = 0;   // nothing recorded: "taunt 0" is wanted
        for (int i = 0; i < 8; i++)
            if (store[i].controller == controller && store[i].player == owner) { previous = store[i].taunt; break; }
        u3x_taunt_want(base, previous);
    }
}
