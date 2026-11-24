// G59 part 2: the lobby character and its re-dress (spec G59_lobby_scene.md, sections 4.8 to 4.12).
// Written from the behaviour description only.

extern "C" {
// C part (spec 2.2)
volatile uint32_t *u3h_s16_diag(void);
uint32_t u3h_crc_sid(const char *text);
uint32_t u3h_active_page(uintptr_t base);

// delivered here (spec 2.1, part 2)
uint8_t *u3x_s16_char(uintptr_t base, uint8_t *cm);
int u3x_h01_loaded(uintptr_t base, uint32_t id);
uint32_t u3x_h01_hash(uintptr_t base, const char *name);
void u3x_h01_tint(uintptr_t base, uint8_t *cm, uint8_t *ch, int32_t side);
void u3x_h01_redress(uintptr_t base, uint8_t *cm);
void u3x_h01_tick(uintptr_t base, uint8_t *cm);
}

namespace g59_char {

// texts whose game name ids are needed (ids are computed at run time)
U3X_RO const char NAME_PLAYER[] = {'P','l','a','y','e','r',0};
U3X_RO const char NAME_NONE[] = {'n','o','n','e',0};
U3X_RO const char NAME_ANIM_MP[] = {'a','n','i','m','-','p','r','o','t','o','-','m','u','l','t','i','p','l','a','y','e','r',0};
U3X_RO const char NAME_COLOR_PAGE[] = {'P','A','G','E','_','C','U','S','T','O','M','I','Z','E','_','C','O','L','O','R',0};

// image offsets (spec 3.1, 3.3)
constexpr uintptr_t IMG_EMPTY_TEXT = 0x13ac250;
constexpr uintptr_t IMG_NAME_ID_FN = 0x1702720;
constexpr uintptr_t IMG_MENU = 0x17bd080;
constexpr uintptr_t IMG_MATCH_STATE = 0x17d1868;
constexpr uintptr_t IMG_MP_ACTIVE = 0x17d5c61;
constexpr uintptr_t IMG_LEVEL_MANAGER = 0x1afcf10;
constexpr uintptr_t FN_FIND_PACKAGE = 0x10eb380;
constexpr uintptr_t FN_TYPE_INDEX = 0x995ec0;
constexpr uintptr_t FN_STORE_LOOK = 0x9964d0;
constexpr uintptr_t FN_TINT_PASS = 0x9965e0;
constexpr uintptr_t FN_SET_PART = 0xbc8630;
constexpr uintptr_t FN_SET_PART_REBUILD = 0xbcb7c0;
constexpr uintptr_t FN_APPLY_LOOK_COLOURS = 0xbc9e50;

// customisation manager and character fields (spec 3.5, 3.6)
constexpr uintptr_t CM_PACKAGES = 0x4;
constexpr uintptr_t CM_WANTED_PARTS = 0xe00;
constexpr uintptr_t CM_CHAR_HANDLE = 0xf60;
constexpr uintptr_t CM_CHAR_ID = 0xf68;
constexpr uintptr_t CM_ACTIVE = 0xf70;
constexpr uintptr_t CM_LOOKS = 0xf78;
constexpr uintptr_t CM_REDRESS_ASKED = 0x104c;
constexpr uintptr_t CH_TYPE = 0x30;
constexpr uintptr_t CH_PROCESS_ID = 0x7c;
constexpr uintptr_t CH_WORN_PARTS = 0x1b80;
constexpr uintptr_t CH_LOOK = 0x1c40;

constexpr int PARTS = 11;
constexpr int PACKAGES = 50;
constexpr int LOOK_BYTES = 0x28;
constexpr uint32_t PACKAGE_LOADED = 8;
constexpr uint32_t MSG_MODEL_CHANGED = 0x80e056d1u;   // message id of the sub-object call (spec 3.2)
constexpr uint32_t JUST_REDRESSED = 0x80000000u;      // D[14] bit 31

U3X_BSS uint32_t TINT_CALLS;
U3X_BSS uint32_t TINT_CHANGES;

// "usable name": not null, at least 0x10000 and below 2^47 (no alignment condition), first byte not 0
U3X_FN bool usable_name(const char *name) {
    const uintptr_t v = reinterpret_cast<uintptr_t>(name);
    return v >= 0x10000u && v < (1ull << 47) && *name != 0;
}

// the game's name id function: pointer read from the image at every call, not tested for null
U3X_FN uint32_t game_name_id(uintptr_t base, const char *name) {
    using NameId = uint32_t (*)(int32_t, const char *, int32_t);
    return at<NameId>(base + IMG_NAME_ID_FN)(0, name, 1);
}

U3X_FN const char *wanted_part(const uint8_t *cm, int32_t side, int k) {
    return rd<const char *>(cm, CM_WANTED_PARTS + 0x58u * static_cast<uint32_t>(side) + 8u * static_cast<uint32_t>(k));
}

U3X_FN bool is_player(const uint8_t *ch) {
    return rd<uint32_t>(ch, CH_TYPE) == u3h_crc_sid(NAME_PLAYER);
}

U3X_FN uint32_t ec_word(int n) {
    return rd<uint32_t>(u3h_ext(), 0x4450u + 4u * static_cast<uint32_t>(n));
}

U3X_FN void count(int word) {
    volatile uint32_t *d = u3h_s16_diag();
    const uint32_t v = d[word];
    d[word] = v + 1;
}

// method of an object at a byte offset in its function table
template <typename R, typename... A> [[gnu::always_inline]] inline R method(void *object, uintptr_t offset, A... args) {
    const uint8_t *table = rd<const uint8_t *>(object, 0);
    return rd<R (*)(void *, A...)>(table, offset)(object, args...);
}

// 4.12 step 7: animation package, every listed package, and every wanted part's package listed
U3X_FN bool ready(uintptr_t base, const uint8_t *cm, int32_t side) {
    if (u3x_h01_loaded(base, u3h_crc_sid(NAME_ANIM_MP)) != 1) return false;
    for (int i = 0; i < PACKAGES; i++) {
        const uint32_t entry = rd<uint32_t>(cm, CM_PACKAGES + 4u * static_cast<uint32_t>(i));
        if (entry != 0 && u3x_h01_loaded(base, entry) != 1) return false;
    }
    const uint32_t none = u3h_crc_sid(NAME_NONE);
    for (int k = 0; k < PARTS; k++) {
        const char *want = wanted_part(cm, side, k);
        if (!usable_name(want)) continue;
        const uint32_t id = u3h_crc_sid(want);
        if (id == none) continue;
        bool listed = false;
        for (int i = 0; i < PACKAGES && !listed; i++)
            listed = rd<uint32_t>(cm, CM_PACKAGES + 4u * static_cast<uint32_t>(i)) == id;
        if (!listed) return false;
    }
    return true;
}

}  // namespace g59_char

// 4.8: the live character the customisation manager shows
extern "C" U3X_FN uint8_t *u3x_s16_char(uintptr_t, uint8_t *cm) {
    using namespace g59_char;
    uint8_t **handle = rd<uint8_t **>(cm, CM_CHAR_HANDLE);
    const uint32_t id = rd<uint32_t>(cm, CM_CHAR_ID);
    if (!handle || id == 0 || !ptr_ok(handle)) return nullptr;
    uint8_t *ch = *handle;
    if (!ch || !ptr_ok(ch) || rd<uint32_t>(ch, CH_PROCESS_ID) != id) return nullptr;
    return ch;
}

// 4.9: is this actor package loaded
extern "C" U3X_FN int u3x_h01_loaded(uintptr_t base, uint32_t id) {
    using namespace g59_char;
    if (id == 0) return 0;
    const uint8_t *package = game_call<const uint8_t *>(base + FN_FIND_PACKAGE, reinterpret_cast<void *>(base + IMG_LEVEL_MANAGER), id);
    return package && ptr_ok(package) && rd<uint32_t>(package, 0x30) == PACKAGE_LOADED ? 1 : 0;
}

// 4.9: name id of a kit part; 0 for no name and for the "none" name
extern "C" U3X_FN uint32_t u3x_h01_hash(uintptr_t base, const char *name) {
    using namespace g59_char;
    if (!usable_name(name)) return 0;
    const uint32_t id = game_name_id(base, name);
    return id == u3h_crc_sid(NAME_NONE) ? 0 : id;
}

// 4.10: refresh the character's look record, and its colours when the record changed
extern "C" U3X_FN void u3x_h01_tint(uintptr_t base, uint8_t *cm, uint8_t *ch, int32_t side) {
    using namespace g59_char;
    TINT_CALLS = TINT_CALLS + 1;
    uint8_t *look = ch + CH_LOOK;
    uint8_t before[LOOK_BYTES];
    for (int i = 0; i < LOOK_BYTES; i++) before[i] = look[i];

    if (u3h_active_page(base) == u3h_crc_sid(NAME_COLOR_PAGE)) {
        // colour page: put the edited look on the character directly
        const uint8_t *edited = cm + CM_LOOKS + static_cast<intptr_t>(side) * LOOK_BYTES;
        for (int i = 0; i < LOOK_BYTES; i++) look[i] = edited[i];
    } else {
        game_call<void>(base + FN_STORE_LOOK, cm);
    }

    bool changed = false;
    for (int i = 0; i < LOOK_BYTES; i++) changed = changed || look[i] != before[i];
    if (changed) {
        TINT_CHANGES = TINT_CHANGES + 1;
        game_call<void>(base + FN_APPLY_LOOK_COLOURS, ch);
    }
}

// 4.11: put the wanted kit on the standing character
extern "C" U3X_FN void u3x_h01_redress(uintptr_t base, uint8_t *cm) {
    using namespace g59_char;
    volatile uint32_t *d = u3h_s16_diag();
    cm[CM_REDRESS_ASKED] = 0;

    uint8_t *ch = u3x_s16_char(base, cm);
    if (!ch || !is_player(ch)) { count(7); return; }
    const int32_t side = rd<int32_t>(cm, 0);
    if (side != 0 && side != 1) return;

    if (ext_eb()[2] & 0x08000000u) {
        const uint8_t *menu = reinterpret_cast<const uint8_t *>(base + IMG_MENU);
        const int32_t user = rd<uint8_t>(menu, 0xdf5) ? 1 : rd<int32_t>(menu, 0x2694);
        const int32_t index = game_call<int32_t>(base + FN_TYPE_INDEX, cm, user, side);
        if (static_cast<uint32_t>(index) >= 64u) return;
    }

    game_call<void>(base + FN_STORE_LOOK, cm);

    // a wanted part is worn only when its package is loaded; otherwise the empty name
    const char *empty = reinterpret_cast<const char *>(base + IMG_EMPTY_TEXT);
    const char *chosen[PARTS];
    uint32_t unloaded = 0;
    for (int k = 0; k < PARTS; k++) {
        const char *want = wanted_part(cm, side, k);
        chosen[k] = empty;
        if (usable_name(want)) {
            if (u3x_h01_loaded(base, u3h_crc_sid(want)) == 1) chosen[k] = want;
            else unloaded |= 1u << k;
        }
    }
    d[5] = (d[5] & 0xffffu) | (unloaded << 16);

    for (int k = 0; k < PARTS - 1; k++) {
        if (chosen[k][0] != 0) game_name_id(base, chosen[k]);   // result not used (spec 6.1)
        game_call<void>(base + FN_SET_PART, ch, chosen[k], static_cast<int32_t>(k));
    }
    game_call<void>(base + FN_SET_PART_REBUILD, ch, chosen[PARTS - 1], static_cast<int32_t>(PARTS - 1));
    count(3);

    // tell the character's sub-object that the model changed
    void *sub = method<void *>(ch, 0x1f8);
    if (sub && ptr_ok(sub)) method<void, uint32_t>(sub, 0x38, MSG_MODEL_CHANGED);

    game_call<void>(base + FN_TINT_PASS, cm);
    d[14] = d[14] | JUST_REDRESSED;
}

// 4.12: once per frame; re-dress when the worn kit differs from the wanted kit and all packages are loaded
extern "C" U3X_FN void u3x_h01_tick(uintptr_t base, uint8_t *cm) {
    using namespace g59_char;
    if (ec_word(1) & 1u) return;
    if (at<uint8_t>(base + IMG_MP_ACTIVE) == 0 || at<int32_t>(base + IMG_MATCH_STATE) == 2) return;

    volatile uint32_t *d = u3h_s16_diag();
    const uint32_t flags = d[14];
    if (flags & JUST_REDRESSED) {   // the frame after a re-dress is skipped
        d[14] = flags & ~JUST_REDRESSED;
        count(6);
        return;
    }
    if (rd<uint8_t>(cm, CM_ACTIVE) == 0) return;

    if (rd<uint8_t>(cm, CM_REDRESS_ASKED) != 0) {
        count(1);
        u3x_h01_redress(base, cm);   // and on to the compare in the same call (spec 6.4)
    }

    uint8_t *ch = u3x_s16_char(base, cm);
    if (!ch || !is_player(ch)) return;
    const int32_t side = rd<int32_t>(cm, 0);
    if (side != 0 && side != 1) return;

    if (!ready(base, cm, side)) { count(4); return; }

    for (int k = 0; k < PARTS; k++) {
        const uint32_t want = u3x_h01_hash(base, wanted_part(cm, side, k));
        const uint32_t worn = u3x_h01_hash(base, rd<const char *>(ch, CH_WORN_PARTS + 8u * static_cast<uint32_t>(k)));
        if (want != worn) {
            count(2);
            d[5] = (d[5] & 0xffff0000u) | (1u << k);
            u3x_h01_redress(base, cm);
            return;
        }
    }
    u3x_h01_tint(base, cm, ch, side);
}
