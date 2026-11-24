// G59 part 3: the preview look on the character pages (spec G59_lobby_scene.md, 4.13 to 4.16).
// Written from the description only.

extern "C" {
// part 2 of this group
uint8_t *u3x_s16_char(uintptr_t base, uint8_t *cm);
void u3x_h01_tick(uintptr_t base, uint8_t *cm);
// C part
volatile uint32_t *u3h_s16_diag(void);
uint32_t u3h_crc_sid(const char *text);
uint32_t u3h_active_page(uintptr_t base);
// this part
int u3x_s17_page(uint32_t page);
int u3x_s17_apply(uintptr_t base, uint8_t *cm, int own_kill);
int u3x_s16_refresh(uintptr_t base, uint64_t *r);
void u3x_s16_tick(uintptr_t base);
}

namespace g59_look {

// ---- image offsets (spec 3.1, 3.3)
constexpr uintptr_t FN_TYPE_INDEX = 0x995ec0;     // (cm, user, side) -> index of the character type the menu asks for
constexpr uintptr_t FN_SYMBOL = 0x11f2bf0;        // (symbol id) -> pointer to a pointer, or null
constexpr uintptr_t FN_BUILD_LOOK = 0x997a40;     // (0x40 buffer, cm, user, type index, side)
constexpr uintptr_t FN_KILL = 0x11a6250;          // (character)
constexpr uintptr_t FN_CM_UPDATE = 0x997230;      // (cm)
constexpr uintptr_t FN_CM_ACTIVATE = 0x997670;    // (cm)
constexpr uintptr_t AT_REGISTRY = 0x16e2f88;      // pointer: model registry entries
constexpr uintptr_t AT_REGISTRY_COUNT = 0x18bed80;   // s32
constexpr uintptr_t AT_SCENE = 0x17bba90;         // menu scene, u32 state at +0
constexpr uintptr_t AT_MENU = 0x17bd080;
constexpr uintptr_t AT_MATCH_STATE = 0x17d1868;
constexpr uintptr_t AT_MP_ACTIVE = 0x17d5c61;     // u8
constexpr uintptr_t AT_CM = 0x17f9938;            // the player-customisation manager

// ---- ids (spec 3.2)
constexpr uint32_t ID_PLAYER = 0xa5a63e9bu;       // name id of "Player": type id of a local player character
constexpr uint32_t ID_TYPE_TABLE = 0xbd3bb3b9u;   // script symbol of the character type table

// ---- fields
constexpr uintptr_t CM_ACTIVE = 0xf70, CM_CURSOR = 0xf74, CM_MODE_A = 0xfc8, CM_MODE_B = 0xfc9;
constexpr uintptr_t CH_TYPE = 0x30, CH_LOOK = 0x1c40;
constexpr uint32_t LOOK_SIZE = 0x28;
constexpr uint32_t STATE_ENTERED = 2, MATCH_RUNNING = 2;

// ---- switches
constexpr uint32_t EB2_REFRESH_OFF = 0x02000000u, EB2_PAGES_OFF = 0x04000000u, EB2_LIMIT_64 = 0x08000000u;

// The nine character pages; the first one is the main look page. Each name ends with a NUL.
constexpr int PAGE_COUNT = 9;
U3X_RO const char page_names[] =
    "PAGE_BODY_TYPE\0"
    "PAGE_BODY_BASE\0"
    "PAGE_BODY_TORSO\0"
    "PAGE_BODY_HEAD\0"
    "PAGE_BODY_ARMS\0"
    "PAGE_BODY_LEGS\0"
    "PAGE_BODY_FEET\0"
    "PAGE_CUSTOMIZE_COLOR\0"
    "PAGE_MULTIPLAYER_CUSTOMIZE_CHARACTER\0";

// ---- own memory
U3X_BSS static uint32_t page_ids[PAGE_COUNT];
U3X_BSS static uint8_t page_ids_ready;
U3X_BSS static int32_t last_cursor;
U3X_BSS static uint8_t page_was_open;

// the page ids, made from the names at first use
U3X_FN static const uint32_t *pages() {
    if (!page_ids_ready) {
        const char *name = page_names;
        for (int i = 0; i < PAGE_COUNT; i++) {
            page_ids[i] = u3h_crc_sid(name);
            while (*name) name++;
            name++;
        }
        page_ids_ready = 1;
    }
    return page_ids;
}

// ec[1] bit 0: set = the look application of this part is on
inline bool look_on() { return (rd<uint32_t>(u3h_ext(), 0x4450 + 4 * 1) & 1) != 0; }

inline void bump(volatile uint32_t *word) { const uint32_t v = *word; *word = v + 1; }

// 1 for user 1 when the menu says so, else the menu's own user
inline int32_t menu_user(uintptr_t base) {
    const uint8_t *menu = &at<uint8_t>(base + AT_MENU);
    return rd<uint8_t>(menu, 0xdf5) ? 1 : rd<int32_t>(menu, 0x2694);
}

// the character type table, or null when it is not usable
U3X_FN static const uint8_t *type_table(uintptr_t base) {
    const uint8_t *slot = game_call<const uint8_t *, uint32_t>(base + FN_SYMBOL, ID_TYPE_TABLE);
    if (!slot || !ptr_ok(slot)) return nullptr;
    const uint8_t *table = rd<const uint8_t *>(slot, 0);
    if (!table || !ptr_ok(table)) return nullptr;
    return table;
}

// a virtual call without further arguments
template <typename R> inline R method(uint8_t *object, uintptr_t table_offset) {
    const uintptr_t fn = rd<uintptr_t>(rd<const uint8_t *>(object, 0), table_offset);
    return game_call<R, uint8_t *>(fn, object);
}

// the registry's id of the model whose object id is `current`: the highest matching entry wins; 0 = none
U3X_FN static uint32_t registered_model(uintptr_t base, uint32_t current) {
    const uint8_t *entries = at<const uint8_t *>(base + AT_REGISTRY);
    if (!ptr_ok(entries)) return 0;
    for (int32_t i = at<int32_t>(base + AT_REGISTRY_COUNT) - 1; i >= 0; i--) {
        const uint8_t *entry = entries + static_cast<uintptr_t>(i) * 0x28;
        const uint8_t *model = rd<const uint8_t *>(entry, 0);
        if (model && rd<uint32_t>(model, 0x50) == current) return rd<uint32_t>(entry, 0x20);
    }
    return 0;
}

}  // namespace g59_look

// 4.13: is `page` one of the nine character pages
extern "C" U3X_FN int u3x_s17_page(uint32_t page) {
    const uint32_t *ids = g59_look::pages();
    for (int i = 0; i < g59_look::PAGE_COUNT; i++)
        if (ids[i] == page) return 1;
    return 0;
}

// 4.14: puts the look the menu asks for on the shown character
extern "C" U3X_FN int u3x_s17_apply(uintptr_t base, uint8_t *cm, int own_kill) {
    using namespace g59_look;
    volatile uint32_t *D = u3h_s16_diag();

    uint8_t *ch = u3x_s16_char(base, cm);
    if (!ch) { bump(&D[4]); return 0; }
    if (rd<uint32_t>(ch, CH_TYPE) != ID_PLAYER) {
        const uint32_t seen = D[11];
        if ((seen & 0xff) != 0xff) D[11] = seen + 1;
        return 0;
    }

    const int32_t user = menu_user(base);
    const int32_t side = rd<int32_t>(cm, 0);
    const int32_t index = game_call<int32_t, uint8_t *, int32_t, int32_t>(base + FN_TYPE_INDEX, cm, user, side);

    // the index must be a character type (or below 64 when that switch is set), the side 0 or 1
    const uint8_t *table = type_table(base);
    uint32_t limit = table ? rd<uint32_t>(table, 0) : 0;
    if (ext_eb()[2] & EB2_LIMIT_64) limit = 64;
    if (static_cast<uint32_t>(index) >= limit || (side != 0 && side != 1)) { bump(&D[14]); return 0; }

    // the look the menu asks for
    uint64_t built[8];
    for (int i = 0; i < 8; i++) built[i] = 0;
    uint8_t *fresh = reinterpret_cast<uint8_t *>(built);
    game_call<void, uint8_t *, uint8_t *, int32_t, int32_t, int32_t>(base + FN_BUILD_LOOK, fresh, cm, user, index, side);

    D[5] = rd<uint32_t>(cm, CM_CURSOR);
    D[6] = static_cast<uint32_t>(side) + 0x100u * rd<uint8_t>(cm, CM_MODE_A) + 0x10000u * rd<uint8_t>(cm, CM_ACTIVE) +
           0x1000000u * rd<uint8_t>(cm, CM_MODE_B);
    D[7] = fresh[0] + 0x100u * (static_cast<uint32_t>(index) & 0xffffffu);

    uint8_t *look = ch + CH_LOOK;
    bool changed = false;
    for (uint32_t i = 0; i < LOOK_SIZE; i++)
        if (look[i] != fresh[i]) changed = true;
    if (own_kill && !changed) return 1;
    if (changed) {
        // stored also when own_kill is 0 and the answer below is 0 (kept: spec section 6, item 6)
        for (uint32_t i = 0; i < LOOK_SIZE; i++) look[i] = fresh[i];
        bump(&D[1]);
    }

    table = type_table(base);
    if (!table) return 0;
    const uint8_t *rows = rd<const uint8_t *>(table, 8);
    if (!ptr_ok(rows)) return 0;

    const uint32_t current = method<uint32_t>(ch, 0x190);
    const uint32_t registered = registered_model(base, current);
    const uint32_t wanted = rd<uint32_t>(rows, static_cast<uintptr_t>(fresh[0]) * 0x70 + 0x58);
    if (registered != wanted) {
        // another model: the character goes, the scene places a new one
        bump(&D[2]);
        if (!own_kill) return 0;   // the game's own function kills it
        game_call<void, uint8_t *>(base + FN_KILL, ch);
    } else if (changed) {
        method<void>(ch, 0x4f8);   // same model: the look in place
        bump(&D[3]);
    }
    if (own_kill) game_call<void, uint8_t *>(base + FN_CM_UPDATE, cm);
    return 1;
}

// 4.15: hook at the entry of the game's preview refresh; 1 = the game function is skipped
extern "C" U3X_FN int u3x_s16_refresh(uintptr_t base, uint64_t *r) {
    using namespace g59_look;
    bump(&u3h_s16_diag()[0]);
    if ((ext_eb()[2] & EB2_REFRESH_OFF) || !look_on()) return 0;
    if (r[4] != base + AT_CM) return 0;
    return u3x_s17_apply(base, &at<uint8_t>(base + AT_CM), 0);
}

// 4.16: once per frame
extern "C" U3X_FN void u3x_s16_tick(uintptr_t base) {
    using namespace g59_look;
    volatile uint32_t *D = u3h_s16_diag();
    uint8_t *cm = &at<uint8_t>(base + AT_CM);
    const uint32_t page = u3h_active_page(base);

    // diagnostics of the main look page
    if (page == pages()[0]) {
        bump(&D[8]);
        const int32_t cursor = rd<int32_t>(cm, CM_CURSOR);
        if (cursor != last_cursor) { last_cursor = cursor; bump(&D[9]); }
        D[10] = rd<uint8_t>(cm, CM_MODE_A) + 0x100u * rd<uint8_t>(cm, CM_ACTIVE) +
                0x10000u * at<uint8_t>(base + AT_SCENE) + 0x1000000u * (u3x_s16_char(base, cm) ? 1u : 0u);
    }

    u3x_h01_tick(base, cm);
    if (ext_eb()[2] & EB2_PAGES_OFF) return;

    // a character page in the lobby: multiplayer active, scene entered, match state NOT "in a match"
    const bool open = u3x_s17_page(page) == 1 && at<uint8_t>(base + AT_MP_ACTIVE) != 0 &&
                      at<uint32_t>(base + AT_SCENE) == STATE_ENTERED && at<uint32_t>(base + AT_MATCH_STATE) != MATCH_RUNNING;
    if (!open) {
        if (page_was_open) {
            // the page was just left: back to the look the menu asks for
            page_was_open = 0;
            if (look_on() && at<uint32_t>(base + AT_SCENE) == STATE_ENTERED && rd<uint8_t>(cm, CM_ACTIVE))
                u3x_s17_apply(base, cm, 1);
        }
        return;
    }
    page_was_open = 1;

    // D[11] byte 3 counts the waits for a character (one per 15 page frames); byte 2 keeps the last finished count
    uint32_t wait = D[11];
    if (!u3x_s16_char(base, cm)) {
        if (D[13] % 15 == 0 && (wait >> 24) < 255) wait += 0x1000000u;
    } else if (wait >> 24) {
        wait = (wait & 0xffffu) | ((wait >> 24) << 16);
    }
    D[11] = wait;

    if (!rd<uint8_t>(cm, CM_ACTIVE)) {
        game_call<void, uint8_t *>(base + FN_CM_ACTIVATE, cm);
        bump(&D[12]);
    }
    bump(&D[13]);
    if (look_on()) u3x_s17_apply(base, cm, 1);
}
