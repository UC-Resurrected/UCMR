// G47 Part C: the mission-complete message and the HUD line queue (written from spec G47_hud.md, Part C).

extern "C" {
const char *u3x_ms_text(uintptr_t base, uint32_t id);
int u3x_ms_len(const char *a);
void u3x_ms_itoa(char *b, int32_t v);
void u3x_ms_replace(char *buf, int n, const char *tag, const char *rep);
int32_t u3x_ms_icon_find(const uint8_t *t, uint32_t id);
void u3x_ms_icon(uintptr_t base, uint32_t id, uint32_t *ic);
int u3x_ms_hud(uintptr_t base, int user, const char *txt, uint32_t type, int32_t level, const uint32_t *ic);
void u3x_mission_done(uintptr_t base, const uint8_t *trk, const int32_t *def);
}

namespace g47c {

// game functions (image offsets)
constexpr uintptr_t FN_TEXT = 0xf32ae0;          // (text system, text id) -> localized text or null
constexpr uintptr_t FN_NUMBER_TEXT = 0xf32080;   // (buffer, size, value)
constexpr uintptr_t FN_FORMAT_TEXT = 0xf32bd0;   // (text system, buffer, size, text id, three text arguments)
constexpr uintptr_t FN_GIVE_CASH = 0x660fb0;     // (null, user, amount)
constexpr uintptr_t FN_BOOSTER = 0x687d30;       // (booster table, booster id) -> definition or null

// game data (image offsets)
constexpr uintptr_t TEXT_SYSTEM = 0x17026b8;
constexpr uintptr_t MENU_USER0 = 0x17bd080;
constexpr uintptr_t MENU_USER1 = 0x17bfa10;
constexpr uintptr_t GAME_CLOCK = 0x17b4a90 + 0x114;   // s32, ticks of 1/300 s
constexpr uintptr_t IMAGE_DIRECTORY = 0x1b24bc8;
constexpr uintptr_t PROFILE_ARRAY = 0x17dd610;
constexpr uintptr_t BOOSTER_TABLE = 0x17c3b40;

// HUD message queue of a menu
constexpr int SLOTS = 4;
constexpr int SLOT_TEXT_SIZE = 0x80;
constexpr uintptr_t QUEUE_START_TIME = 0x26a4;
constexpr uintptr_t QUEUE_TEXTS = 0x26a8;
constexpr uintptr_t QUEUE_PARAMETERS = 0x28a8;
constexpr uintptr_t PARAMETER_SIZE = 0x30;

// profile object
constexpr uintptr_t PROFILE_SIZE = 0x7b00;
constexpr uintptr_t PROFILE_DONE_BITS = 0xecc;
constexpr uintptr_t PROFILE_ACTIVE_PROGRESS = 0xf4c;
constexpr uintptr_t PROFILE_ACTIVE_ID = 0x1864;
constexpr uintptr_t PROFILE_ACTIVE_START = 0x1868;

constexpr uint32_t TEXT_MISSION_COMPLETE = 0xc423e4f5u;   // line with two placeholders: name, cash
constexpr uint32_t TEXT_REWARD_LINE = 0x065ba916u;        // line with the tags [A], [B], [TEXT]
constexpr uint32_t MESSAGE_REWARD = 0x2e;
constexpr uint32_t F_300 = 0x43960000u;
constexpr uint32_t NO_INDEX = 0xffffffffu;

constexpr uint32_t FIRST_REWARD_MISSION = 0x100;   // below: cash missions
constexpr uint32_t DONE_BIT_COUNT = 0x400;         // missions from here on have no done bit
constexpr int REST_LIMIT = 255;                    // characters kept of the text after a replaced tag

U3X_RO const char tag_a[] = {'[', 'A', ']', 0};
U3X_RO const char tag_b[] = {'[', 'B', ']', 0};
U3X_RO const char tag_text[] = {'[', 'T', 'E', 'X', 'T', ']', 0};
U3X_RO const char text_zero[] = {'0', 0};

// diagnostic words ms[0..7]
enum { MS_CALLS, MS_CASH_LINES, MS_REWARD_LINES, MS_MARKED, MS_LAST_ID, MS_CASH_PAID, MS_ALREADY_DONE, MS_NOT_QUEUED };

[[gnu::always_inline]] inline uint32_t *ms_words() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0x2da0); }

template <typename T> [[gnu::always_inline]] inline void put(uint8_t *p, uintptr_t off, T value) {
    *reinterpret_cast<T *>(p + off) = value;
}

// does the text at `at` start with the tag
[[gnu::always_inline]] inline bool starts_with(const char *at, const char *tag, int tag_length) {
    for (int k = 0; k < tag_length; k++)
        if (at[k] != tag[k]) return false;
    return true;
}

}  // namespace g47c

// the localized text of an id, or the game's fallback text
extern "C" U3X_FN const char *u3x_ms_text(uintptr_t base, uint32_t id) {
    using namespace g47c;
    const uintptr_t text_system = base + TEXT_SYSTEM;
    const char *text = game_call<const char *>(base + FN_TEXT, text_system, id);
    return text ? text : at<const char *>(text_system + 0x10);
}

extern "C" U3X_FN int u3x_ms_len(const char *a) {
    int n = 0;
    while (a[n]) n++;
    return n;
}

// v in decimal, with its 0 byte
extern "C" U3X_FN void u3x_ms_itoa(char *b, int32_t v) {
    uint32_t rest = static_cast<uint32_t>(v);
    if (v < 0) {
        *b++ = '-';
        rest = 0u - rest;
    }
    char digits[10];
    int count = 0;
    do {
        digits[count++] = static_cast<char>('0' + rest % 10);
        rest /= 10;
    } while (rest);
    while (count) *b++ = digits[--count];
    *b = 0;
}

// replaces the first `tag` in buf by `rep`; the text never grows beyond n - 1 characters
extern "C" U3X_FN void u3x_ms_replace(char *buf, int n, const char *tag, const char *rep) {
    using namespace g47c;
    const int length = u3x_ms_len(buf);
    const int tag_length = u3x_ms_len(tag);
    const int rep_length = u3x_ms_len(rep);

    int found = -1;
    for (int i = 0; i + tag_length <= length; i++)
        if (starts_with(buf + i, tag, tag_length)) {
            found = i;
            break;
        }
    if (found < 0) return;

    // the text after the tag is set aside (the replacement may be written over it)
    char rest[REST_LIMIT];
    int rest_length = length - (found + tag_length);
    if (rest_length > REST_LIMIT) rest_length = REST_LIMIT;
    for (int k = 0; k < rest_length; k++) rest[k] = buf[found + tag_length + k];

    int pos = found;
    for (int k = 0; k < rep_length && pos < n - 1; k++) buf[pos++] = rep[k];
    for (int k = 0; k < rest_length && pos < n - 1; k++) buf[pos++] = rest[k];
    buf[pos] = 0;
}

// index of the entry with this id in an image table, or -1
extern "C" U3X_FN int32_t u3x_ms_icon_find(const uint8_t *t, uint32_t id) {
    if (!t) return -1;
    const int32_t count = rd<uint16_t>(t, 2);
    const uint8_t *entries = rd<const uint8_t *>(t, 8);
    if (!entries) return -1;
    for (int32_t i = 0; i < count; i++)
        if (rd<uint32_t>(entries + 0x28 * static_cast<uintptr_t>(i), 8) == id) return i;
    return -1;
}

// where the icon is: second image table first, then the first one
extern "C" U3X_FN void u3x_ms_icon(uintptr_t base, uint32_t id, uint32_t *ic) {
    using namespace g47c;
    ic[0] = id;
    ic[1] = 1;
    ic[2] = NO_INDEX;
    const uint8_t *directory = at<const uint8_t *>(base + IMAGE_DIRECTORY);
    if (!directory) return;
    const int32_t in_second = u3x_ms_icon_find(rd<const uint8_t *>(directory, 0x28), id);
    if (in_second >= 0) {
        ic[2] = static_cast<uint32_t>(in_second);
        return;
    }
    ic[1] = 0;
    ic[2] = static_cast<uint32_t>(u3x_ms_icon_find(rd<const uint8_t *>(directory, 0x20), id));
}

// puts a line into the first free slot of the user's HUD message queue; 1 = queued
extern "C" U3X_FN int u3x_ms_hud(uintptr_t base, int user, const char *txt, uint32_t type, int32_t level, const uint32_t *ic) {
    using namespace g47c;
    uint8_t *menu = reinterpret_cast<uint8_t *>(base + (user ? MENU_USER1 : MENU_USER0));
    uint32_t *ms = ms_words();

    const int length = u3x_ms_len(txt);
    int slot = 0;
    if (length < SLOT_TEXT_SIZE)
        while (slot < SLOTS && menu[QUEUE_TEXTS + static_cast<uintptr_t>(SLOT_TEXT_SIZE * slot)]) slot++;
    if (length >= SLOT_TEXT_SIZE || slot == SLOTS) {
        ms[MS_NOT_QUEUED] += 1;
        return 0;
    }

    char *text = reinterpret_cast<char *>(menu + QUEUE_TEXTS + static_cast<uintptr_t>(SLOT_TEXT_SIZE * slot));
    for (int k = 0; k <= length; k++) text[k] = txt[k];

    uint8_t *par = menu + QUEUE_PARAMETERS + PARAMETER_SIZE * static_cast<uintptr_t>(slot);
    put<uint32_t>(par, 0x00, type);
    put<uint64_t>(par, 0x08, 0);
    put<int32_t>(par, 0x10, level);
    put<uint32_t>(par, 0x14, ic[0]);
    put<uint32_t>(par, 0x18, ic[1]);
    put<uint32_t>(par, 0x1c, ic[2]);
    put<uint32_t>(par, 0x20, 0);
    put<uint32_t>(par, 0x24, 0xffffffffu);
    put<uint32_t>(par, 0x28, F_ONE);

    // the first slot starts showing now
    if (slot == 0) put<uint32_t>(menu, QUEUE_START_TIME, f_div(f_from_int(at<int32_t>(base + GAME_CLOCK)), F_300));
    return 1;
}

// a mission was completed: HUD line, cash or booster reward line, done bit
extern "C" U3X_FN void u3x_mission_done(uintptr_t base, const uint8_t *trk, const int32_t *def) {
    using namespace g47c;
    // the mission id and the user are read before anything is counted
    const uint32_t id = static_cast<uint32_t>(def[0]);
    const int32_t user = rd<int32_t>(trk, 0x1004);
    uint32_t *ms = ms_words();
    ms[MS_CALLS] += 1;
    ms[MS_LAST_ID] = id;

    if (user != 0 && user != 1) return;
    uint8_t *profiles = at<uint8_t *>(base + PROFILE_ARRAY);
    if (!profiles) return;
    uint8_t *profile = profiles + PROFILE_SIZE * static_cast<uintptr_t>(user);

    const bool has_bit = id < DONE_BIT_COUNT;
    uint8_t *done_byte = profile + PROFILE_DONE_BITS + id / 8;   // used only when has_bit
    const uint8_t done_mask = static_cast<uint8_t>(1u << (id & 7));
    const bool was_done = has_bit && (*done_byte & done_mask);

    // a newly completed mission is no longer the active one
    if (!was_done && rd<uint32_t>(profile, PROFILE_ACTIVE_ID) == id) {
        put<uint32_t>(profile, PROFILE_ACTIVE_ID, 0);
        put<uint32_t>(profile, PROFILE_ACTIVE_START, 0);
        put<uint32_t>(profile, PROFILE_ACTIVE_PROGRESS, 0);
    }

    char line[SLOT_TEXT_SIZE];
    if (id < FIRST_REWARD_MISSION) {
        // cash mission: line with name and cash, then the cash (every time, also when it was done before);
        // the reward is read anew for each use (the game calls between may change it)
        char cash_text[0x20];
        game_call<void>(base + FN_NUMBER_TEXT, static_cast<char *>(cash_text), int32_t{0x20}, def[0x24 / 4]);
        const char *name = u3x_ms_text(base, static_cast<uint32_t>(def[0x04 / 4]));
        game_call<void>(base + FN_FORMAT_TEXT, base + TEXT_SYSTEM, static_cast<char *>(line), int32_t{SLOT_TEXT_SIZE},
                        TEXT_MISSION_COMPLETE, name, static_cast<const char *>(cash_text), static_cast<const char *>(nullptr));
        uint32_t no_icon[3];
        no_icon[0] = 0;
        no_icon[1] = 0;
        no_icon[2] = NO_INDEX;
        if (u3x_ms_hud(base, user, line, 0, 0, no_icon)) ms[MS_CASH_LINES] += 1;
        game_call<void>(base + FN_GIVE_CASH, static_cast<void *>(nullptr), user, def[0x24 / 4]);
        ms[MS_CASH_PAID] += static_cast<uint32_t>(def[0x24 / 4]);
    } else {
        // reward mission: line with the unlocked booster and its icon, once
        const uint8_t *booster = nullptr;
        if (at<uint32_t>(base + BOOSTER_TABLE + 0x2c))
            booster = game_call<const uint8_t *>(base + FN_BOOSTER, base + BOOSTER_TABLE, static_cast<uint32_t>(def[0x34 / 4]));
        if (booster) {
            // the done bit is read again here, after the booster lookup
            if (has_bit && (*done_byte & done_mask)) {
                ms[MS_ALREADY_DONE] += 1;
                return;
            }
            const char *pattern = u3x_ms_text(base, TEXT_REWARD_LINE);
            int length = 0;
            while (length < SLOT_TEXT_SIZE - 1 && pattern[length]) {
                line[length] = pattern[length];
                length++;
            }
            line[length] = 0;

            char number[12];
            u3x_ms_itoa(number, static_cast<int32_t>(static_cast<uint32_t>(def[0x1c / 4]) + 2));
            u3x_ms_replace(line, SLOT_TEXT_SIZE, tag_a, number);
            u3x_ms_replace(line, SLOT_TEXT_SIZE, tag_b, text_zero);
            u3x_ms_replace(line, SLOT_TEXT_SIZE, tag_text, u3x_ms_text(base, rd<uint32_t>(booster, 8)));

            uint32_t icon[3];
            u3x_ms_icon(base, rd<uint32_t>(booster, 0x28), icon);
            // the level is read again for the queue
            const int32_t level = static_cast<int32_t>(static_cast<uint32_t>(def[0x1c / 4]) + 1);
            if (u3x_ms_hud(base, user, line, MESSAGE_REWARD, level, icon)) ms[MS_REWARD_LINES] += 1;
        }
        if (!has_bit) return;
    }

    *done_byte |= done_mask;
    ms[MS_MARKED] += 1;
}
