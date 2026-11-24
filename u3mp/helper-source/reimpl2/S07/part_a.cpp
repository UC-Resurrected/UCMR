// S07 part A (spec S07_texts_helpers_leftovers.md, sections 4.1 to 4.13): rank / XP and other texts, profile and
// friends helpers, Oasis ids. Written from the behaviour description only.

// ---- other groups, C part (spec 2.2)
extern "C" uint32_t u3h_crc_sid(const char *name);
extern "C" int u3x_strcasecmp(const char *a, const char *b);

// Quick-action menu (layout fixed by spec 2.1; the full declaration lives with G18 / G20 / G34, so it is only named
// here and accessed by offset): u32 argument +0, u32 page +4, s32 count +8, up to 8 (text, action) u32 pairs at +0xc.
struct QuickActions;

// ---- part A exports (spec 2.1)
extern "C" {
int u3x_is_oasis(uint32_t map_id);
int u3x_count_quit(uint8_t *record);
int u3x_last_game_third(int32_t game_type, uint32_t subtitle, uint32_t difficulty, const uint32_t *difficulty_texts,
                        uint32_t *text_id);
uint32_t u3x_time_ago(uint32_t seconds, uint32_t *text_id);
void u3x_time_limit_text(char *out, char *end, const uint8_t *rule, int32_t count, const char *label);
int32_t u3x_xp_scale(int32_t xp, int32_t legacy);
int32_t u3x_level_threshold(const int32_t *table, int32_t count, int32_t level, int32_t legacy);
int32_t u3x_level_for_xp(const int32_t *table, int32_t count, int32_t xp, int32_t legacy);
uint32_t u3x_rank_word(int32_t level, uint32_t legacy, uint32_t badge_bits);
int u3x_friend_in_game(const uint8_t *entry, const char *title);
int32_t u3x_icon_index(const uint8_t *table, uint32_t id);
int u3x_quick_action_add(QuickActions *q, uint32_t text, uint32_t action);
uint8_t *u3x_player_profile(uintptr_t base);
}

namespace s07a {

// map names whose string ids are the Oasis map ids (spec 3, 4.1)
U3X_RO const char oasis_plain[] = "oasis";
U3X_RO const char oasis_survival[] = "oasis-survival";
U3X_RO const char oasis_shade[] = "oasis-shade";

// single text ids, kept by value (spec 3, 7.1)
constexpr uint32_t TXT_SECONDS = 0x72cd40d5u;
constexpr uint32_t TXT_MINUTES = 0x6498bce2u;
constexpr uint32_t TXT_HOURS = 0x624258c1u;
constexpr uint32_t TXT_DAYS = 0x60f0bee3u;
constexpr uint32_t TXT_WEEKS = 0xd250139cu;
constexpr uint32_t TXT_NO_SUBTITLE_A = 0x22a276ecu;
constexpr uint32_t TXT_NO_SUBTITLE_B = 0x6f96651fu;

// float patterns of the XP factors
constexpr uint32_t F_2_5 = 0x40200000u;   // 2.5
constexpr uint32_t F_5_0 = 0x40a00000u;   // 5.0

// image offsets (spec 3)
constexpr uintptr_t IMG_PROFILES = 0x17dd610;      // pointer to the profile array
constexpr uintptr_t IMG_SPLIT_SCREEN = 0x17bde75;  // u8
constexpr uintptr_t IMG_MENU_USER = 0x17bf714;     // u32
constexpr uintptr_t PROFILE_SIZE = 0x7b00;

// bounded writer: a character is stored only while the position is before the end
struct Out {
    char *p;
    char *end;
    [[gnu::always_inline]] void put(char c) {
        if (p < end) *p++ = c;
    }
};

}  // namespace s07a

// 4.1
extern "C" U3X_FN int u3x_is_oasis(uint32_t map_id) {
    using namespace s07a;
    if (map_id == u3h_crc_sid(oasis_plain)) return 1;
    if (map_id == u3h_crc_sid(oasis_survival)) return 1;
    if (map_id == u3h_crc_sid(oasis_shade)) return 1;
    return 0;
}

// 4.2 (record never null; not checked)
extern "C" U3X_FN int u3x_count_quit(uint8_t *record) {
    uint32_t *in_match = reinterpret_cast<uint32_t *>(record + 0x558);
    if (*in_match == 0) return 0;
    *in_match = 0;
    *reinterpret_cast<uint32_t *>(record + 0x5b0) += 1;
    *reinterpret_cast<uint32_t *>(record + 0x1f24) += 1;
    return 1;
}

// 4.3
extern "C" U3X_FN int u3x_last_game_third(int32_t game_type, uint32_t subtitle, uint32_t difficulty,
                                          const uint32_t *difficulty_texts, uint32_t *text_id) {
    using namespace s07a;
    if (game_type == 4 || game_type == 11) {
        if (difficulty >= 5) return 2;
        *text_id = difficulty_texts[difficulty];
        return 1;
    }
    if (subtitle == 0 || subtitle == TXT_NO_SUBTITLE_A || subtitle == TXT_NO_SUBTITLE_B) return 0;
    *text_id = subtitle;
    return 1;
}

// 4.4
extern "C" U3X_FN uint32_t u3x_time_ago(uint32_t seconds, uint32_t *text_id) {
    using namespace s07a;
    if (seconds < 120) {
        *text_id = TXT_SECONDS;
        return seconds < 2 ? 2 : seconds;
    }
    const uint32_t minutes = seconds / 60;
    if (minutes <= 119) {
        *text_id = TXT_MINUTES;
        return minutes;
    }
    const uint32_t hours = minutes / 60;
    if (hours <= 47) {
        *text_id = TXT_HOURS;
        return hours;
    }
    const uint32_t days = hours / 24;
    if (days <= 13) {
        *text_id = TXT_DAYS;
        return days;
    }
    *text_id = TXT_WEEKS;
    return days / 7;
}

// 4.5
extern "C" U3X_FN void u3x_time_limit_text(char *out, char *end, const uint8_t *rule, int32_t count,
                                           const char *label) {
    const int64_t value = static_cast<int64_t>(rd<int32_t>(rule, 0x18)) * count + rd<int32_t>(rule, 0x10);
    s07a::Out w{out, end};
    uint64_t magnitude = static_cast<uint64_t>(value);
    if (value < 0) {
        w.put('-');
        magnitude = 0 - magnitude;
    }
    char digits[20];
    int n = 0;
    do {
        digits[n++] = static_cast<char>('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude);
    while (n) w.put(digits[--n]);
    if (label[0] != 0) {
        w.put(' ');
        for (const char *c = label; *c; ++c) w.put(*c);
    }
    *w.p = 0;   // NUL at the write position (may be end itself)
}

// 4.6: single-precision steps, bit for bit
extern "C" U3X_FN int32_t u3x_xp_scale(int32_t xp, int32_t legacy) {
    using namespace s07a;
    if (legacy == 4) return f_to_int(f_mul(f_from_int(xp), F_2_5));
    if (legacy == 5) return f_to_int(f_mul(f_from_int(xp), F_5_0));
    return xp;
}

// 4.7: raise to 0 first, then lower to count - 1 (count <= 0 reads before the table: kept, spec 6.1)
extern "C" U3X_FN int32_t u3x_level_threshold(const int32_t *table, int32_t count, int32_t level, int32_t legacy) {
    int32_t i = level;
    if (i < 0) i = 0;
    if (i > count - 1) i = count - 1;
    return u3x_xp_scale(table[i], legacy);
}

// 4.8
extern "C" U3X_FN int32_t u3x_level_for_xp(const int32_t *table, int32_t count, int32_t xp, int32_t legacy) {
    for (int32_t level = 0; level + 1 < count; ++level) {
        if (u3x_xp_scale(table[level], legacy) <= xp && xp < u3x_xp_scale(table[level + 1], legacy)) return level;
    }
    return count - 1;
}

// 4.9
extern "C" U3X_FN uint32_t u3x_rank_word(int32_t level, uint32_t legacy, uint32_t badge_bits) {
    return badge_bits | (static_cast<uint32_t>(level) & 0xffffu) | ((legacy & 0x3fffu) << 16);
}

// 4.10
extern "C" U3X_FN int u3x_friend_in_game(const uint8_t *entry, const char *title) {
    const int32_t state = rd<int32_t>(entry, 0x1b0);
    if (state == 1) return 1;
    if (state != 2) return 0;
    return u3x_strcasecmp(reinterpret_cast<const char *>(entry + 0x28), title) == 0 ? 1 : 0;
}

// 4.11
extern "C" U3X_FN int32_t u3x_icon_index(const uint8_t *table, uint32_t id) {
    if (!table) return -1;
    const uint32_t count = rd<uint16_t>(table, 2);
    const uint8_t *entries = rd<const uint8_t *>(table, 8);
    if (!entries) return -1;
    for (uint32_t k = 0; k < count; ++k) {
        if (rd<uint32_t>(entries, 40 * static_cast<uintptr_t>(k) + 8) == id) return static_cast<int32_t>(k);
    }
    return -1;
}

// 4.12 (a negative count passes the cap: kept, spec 6.2)
extern "C" U3X_FN int u3x_quick_action_add(QuickActions *q, uint32_t text, uint32_t action) {
    uint8_t *m = reinterpret_cast<uint8_t *>(q);
    int32_t *count = reinterpret_cast<int32_t *>(m + 8);
    const int32_t n = *count;
    if (n > 7) return 0;
    uint32_t *pair = reinterpret_cast<uint32_t *>(m + 0xc + 8 * static_cast<intptr_t>(n));
    pair[0] = text;
    pair[1] = action;
    // raise the count as it is in memory now (a stored pair may have landed on it)
    *count = static_cast<int32_t>(static_cast<uint32_t>(*count) + 1u);
    return 1;
}

// 4.13
extern "C" U3X_FN uint8_t *u3x_player_profile(uintptr_t base) {
    using namespace s07a;
    uint8_t *profiles = at<uint8_t *>(base + IMG_PROFILES);
    if (!profiles) return nullptr;
    const uint32_t user = at<uint8_t>(base + IMG_SPLIT_SCREEN) != 0 ? 1u : at<uint32_t>(base + IMG_MENU_USER);
    if (user >= 4) return nullptr;
    return profiles + static_cast<uintptr_t>(user) * PROFILE_SIZE;
}
