// G57 part B: wall emblems (spec G57_emblem_compositor.md, section 4.6).
// Once per frame the emblem texture of the leading team's player (or of the local player) is written into the
// render tables of every map object whose art name carries the mark "-multi-emblem-".

extern "C" int u3h_emb_is_record(uint64_t p);                        // G56: 1 = one of the 24 emblem records
extern "C" uint8_t *u3h_emb_team_player(uintptr_t b, int32_t team);  // G58: net player record of a team's emblem player
extern "C" uint32_t *u3h_emb_wall_diag(void);                        // C part: the four wall counters ew[0..3]

namespace g57b {

U3X_RO const char name_mark[] = "-multi-emblem-";   // 14 characters
U3X_RO const char board_mark[] = "boa";             // straight after the mark: a board

constexpr int MARK_LENGTH = 14;
constexpr int MARK_SEARCH_END = 40;      // start positions tried: 0..39
constexpr int WALK_LIMIT = 16384;        // objects looked at per root
constexpr int TABLE_LIMIT = 8;           // render table pointers an object holds

// the switch and diagnostic words ec[]
U3X_FN uint32_t *ec_words() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0x4450); }

// the function pointer in an object's virtual table
U3X_FN uintptr_t virtual_slot(const uint8_t *object, uintptr_t slot) {
    return rd<uint64_t>(reinterpret_cast<const void *>(rd<uint64_t>(object, 0)), slot);
}

// Position just behind the mark in an art name, or null when the name does not carry it.
U3X_FN const char *after_mark(const char *name) {
    for (int start = 0; start < MARK_SEARCH_END && name[start]; start++) {
        int k = 0;
        while (k < MARK_LENGTH && name[start + k] == name_mark[k]) k++;
        if (k == MARK_LENGTH) return name + start + MARK_LENGTH;
    }
    return nullptr;
}

U3X_FN bool is_board(const char *rest) {
    for (int k = 0; k < 3; k++)
        if (rest[k] != board_mark[k]) return false;
    return true;
}

// The player whose emblem is shown: the leading team's in a match, else (or when there is none) the local one.
U3X_FN const uint8_t *shown_player(uintptr_t b) {
    const uint8_t *player = nullptr;
    if (at<int32_t>(b + 0x17d1868) == 2) {
        const int32_t team = game_call<int32_t, uint8_t *>(b + 0x737eb0, reinterpret_cast<uint8_t *>(b + 0x17cb060));
        player = u3h_emb_team_player(b, team);
    }
    if (!player) player = at<const uint8_t *>(b + 0x17cb3d0);
    if (player && !ptr_ok(player)) player = nullptr;   // dropped, no second choice
    return player;
}

// One object of the walk.
U3X_FN void wall_object(uint8_t *object, const uint8_t *player, uint64_t default_record, uint32_t *ew) {
    if ((rd<uint16_t>(object, 0x40) & 5) != 1) return;
    const uint32_t type = rd<uint32_t>(object, 0x30);
    if (type != 0x2db62a42u && type != 0xb95beddbu) return;
    ew[3] += 0x10000;   // a candidate

    const uint8_t *art = game_call<uint8_t *, uint8_t *>(virtual_slot(object, 0xe8), object);
    if (!art || !ptr_ok(art)) return;
    const char *name = rd<const char *>(art, 0x48);
    if (!name) return;
    const char *rest = after_mark(name);
    if (!rest) return;
    const bool board = is_board(rest);

    uint32_t *ec = ec_words();
    uint64_t record = player ? rd<uint64_t>(player, board ? 0x7e8 : 0x7f0) : 0;
    if (!record || !u3h_emb_is_record(record)) {
        record = default_record;
        ec[148]++;
    } else {
        ec[147]++;
    }
    if (player) {
        ec[151] = rd<uint32_t>(player, 0x980);
        ec[152] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(player));
    }

    // the chosen record is checked again before its texture is read
    uint64_t texture = 0;
    if (record && u3h_emb_is_record(record)) texture = rd<uint64_t>(reinterpret_cast<const void *>(record), 0);
    if (!texture) {
        ew[3]++;
        return;
    }

    int32_t tables = game_call<int32_t, uint8_t *>(virtual_slot(object, 0x2c0), object);
    if (tables > TABLE_LIMIT) tables = TABLE_LIMIT;
    bool written = false;
    for (int32_t i = 0; i < tables; i++) {
        uint8_t *table = rd<uint8_t *>(object, 0x240 + 8 * static_cast<uintptr_t>(i));
        if (!table) continue;
        *reinterpret_cast<uint64_t *>(table + 8) = texture;
        written = true;
    }
    ew[written ? 1 : 2]++;
}

}  // namespace g57b

extern "C" U3X_FN void u3x_wall_emblems(uintptr_t b) {
    if (g57b::ec_words()[0] & 0x4000u) return;   // emblem port off
    uint32_t *ew = u3h_emb_wall_diag();
    ew[0]++;

    const uint8_t *player = g57b::shown_player(b);
    const uint64_t default_record = at<uint64_t>(b + 0x17c3440 + 0x78);

    for (uintptr_t root_at = b + 0x1b37610 + 0xd0; root_at <= b + 0x1b37610 + 0xd8; root_at += 8) {
        uint8_t *root = at<uint8_t *>(root_at);
        if (!ptr_ok(root)) continue;
        uint8_t *object = root;
        for (int n = 0; n < g57b::WALK_LIMIT; n++) {
            object = game_call<uint8_t *, uint8_t *, uint8_t *>(b + 0x11a4580, object, root);
            if (!object) break;
            g57b::wall_object(object, player, default_record, ew);
        }
    }
}
