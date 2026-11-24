// S09 part A: small game rules that the C part calls through one-line wrappers (spec S09_small_rules.md, 2.1 part A).

// ---- functions of other groups, of the C part and file-private helpers (spec 2.2)
extern "C" void u3x_rp_assert(uintptr_t base, uintptr_t a, uintptr_t b, uintptr_t c, uint32_t line);
extern "C" uint32_t u3x_net_player_id(const uint8_t *np);
extern "C" void u3h_show_dialog(uintptr_t base, void *s, uint32_t body, uint32_t title, uint32_t action, int32_t buttons,
                                uint32_t extra, uint32_t close);
extern "C" int32_t u3x_net_clock(uintptr_t base);
extern "C" uint32_t u3x_weapon_for_stat(const uint32_t *fixed, const uint32_t *extra, uint32_t number);

namespace s09a {

// film bit-stream reader assert (3.1): condition, source file, function name, line
constexpr uintptr_t ASSERT_COND = 0x14b92fc;
constexpr uintptr_t ASSERT_FILE = 0x14b9313;
constexpr uintptr_t ASSERT_FUNC = 0x14b9357;   // "ReadBits" (also used for writer overruns, 6.1)
constexpr uint32_t ASSERT_LINE = 0xac;

// sessions (3.3)
constexpr uintptr_t SLOTS = 0x680, SLOT_SIZE = 0x120, SLOT_IN_USE = 0x25;
constexpr uint32_t SLOT_COUNT = 10;

// menu user (3.3)
constexpr uintptr_t MENU_USER1 = 0x17bde75, MENU_USER = 0x17bf714;

// splitscreen dialog texts (3.5)
constexpr uint32_t SPLIT_BODY = 0x7c31f4d0, SPLIT_TITLE = 0xc6467f2d;

// net state and clocks (3.3)
constexpr uintptr_t NET_STATE = 0x1b68810, NET_STATE_TIME = 0x1b68814, GAME_CLOCK = 0x17b4ba4;
constexpr uintptr_t NET_OBJ = 0x17d5c58, NET_DEADLINE = 0x2c;

// stat weapons (3.3, 3.5)
constexpr uintptr_t STAT_WEAPONS = 0x13a6170;
constexpr uint32_t STAT_FIXED = 0x13;
constexpr uintptr_t DC_LOOKUP = 0x11f2bf0;
constexpr uint32_t EXTRA_WEAPONS_SYMBOL = 0x9e354807;

[[gnu::always_inline]] inline void swap_bytes(uint8_t *p, int n) {
    for (int i = 0, j = n - 1; i < j; ++i, --j) {
        const uint8_t t = p[i];
        p[i] = p[j];
        p[j] = t;
    }
}

}  // namespace s09a

// 4.1: raise the game's bit-stream assert (reader's values, for reads and writes alike)
extern "C" U3X_FN void u3x_stream_assert(uintptr_t base) {
    using namespace s09a;
    u3x_rp_assert(base, ASSERT_COND, ASSERT_FILE, ASSERT_FUNC, ASSERT_LINE);
}

// 4.2: PS3 <-> PS4 byte order of an invite record: key (+0), party id (+8), send time (+0x28); the rest untouched
extern "C" U3X_FN void u3x_invite_blob_swap(uint8_t *blob) {
    s09a::swap_bytes(blob + 0x0, 4);
    s09a::swap_bytes(blob + 0x8, 8);
    s09a::swap_bytes(blob + 0x28, 8);
}

// 4.3: np_code is the existing u3x_net_player_id (G12 3.1); the C wrapper forwards to it, nothing new here.

// 4.4: number of the 10 session member slots in use
extern "C" U3X_FN uint32_t u3x_session_member_count(const uint8_t *session) {
    using namespace s09a;
    uint32_t n = 0;
    for (uint32_t i = 0; i < SLOT_COUNT; ++i)
        if (rd<uint8_t>(session, SLOTS + SLOT_SIZE * i + SLOT_IN_USE) != 0) ++n;
    return n;
}

// 4.5: the user the menu acts for: 1 when the user-1 byte is set, else the whole (unmasked) menu user word
extern "C" U3X_FN uint32_t u3x_menu_user(uintptr_t base) {
    using namespace s09a;
    if (at<uint8_t>(base + MENU_USER1) != 0) return 1;
    return at<uint32_t>(base + MENU_USER);
}

// 4.6: Splitscreen menu action without a second user: the game's notice dialog
extern "C" U3X_FN void u3x_splitscreen_dialog(uintptr_t base, void *s) {
    using namespace s09a;
    u3h_show_dialog(base, s, SPLIT_BODY, SPLIT_TITLE, 0, 0, 0, 0);
}

// 4.7: enter a net state: state first, then the game clock at that moment
extern "C" U3X_FN void u3x_net_state_set(uintptr_t base, uint32_t state) {
    using namespace s09a;
    at<volatile uint32_t>(base + NET_STATE) = state;
    at<volatile uint32_t>(base + NET_STATE_TIME) = at<volatile uint32_t>(base + GAME_CLOCK);
}

// 4.8: 1 when the net clock has reached the status deadline (plain signed 32-bit compare, clock read first)
extern "C" U3X_FN int u3x_net_deadline_due(uintptr_t base) {
    using namespace s09a;
    const int32_t now = u3x_net_clock(base);
    const int32_t deadline = at<int32_t>(base + NET_OBJ + NET_DEADLINE);
    return now >= deadline ? 1 : 0;
}

// 4.9: weapon id of kill stat number index: fixed list below 0x13, else the game's extra weapon list
extern "C" U3X_FN uint32_t u3x_kr_statw(uintptr_t base, uint32_t index) {
    using namespace s09a;
    const uint32_t *fixed = reinterpret_cast<const uint32_t *>(base + STAT_WEAPONS);
    if (index < STAT_FIXED) return u3x_weapon_for_stat(fixed, nullptr, index);  // no game call
    void **ref = game_call<void **>(base + DC_LOOKUP, EXTRA_WEAPONS_SYMBOL);
    const uint32_t *list = ref ? static_cast<const uint32_t *>(*ref) : nullptr;
    return u3x_weapon_for_stat(fixed, list, index);
}
