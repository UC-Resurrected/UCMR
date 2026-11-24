// S04 part A: film data helpers (clean-room, written from spec S04_film_leftovers.md section 4 "Part A").

// ---- functions of the C part, of other groups and of part B (declared only)
extern "C" uint8_t *u3h_ext(void);
extern "C" uint8_t *u3h_cin_state(uintptr_t base);
extern "C" char *u3h_cin_path(char *out, int size, const char *dir, const char *name);
extern "C" void u3h_cin_scan(uintptr_t base, void *s, uint8_t *st);
extern "C" void u3h_cin_flags(uint8_t *st);
extern "C" void u3h_rp_assert(uintptr_t base);
extern "C" uint32_t u3x_bits_get(const uint32_t *words, uint32_t pos, uint32_t n);
extern "C" void u3x_bits_put(uint32_t *words, uint32_t pos, uint32_t value, uint32_t n);
extern "C" int u3x_k23_rep(uintptr_t base, uint8_t *pad, uint32_t m);
extern "C" void u3x_k23_action(uintptr_t base, uint32_t id);
extern "C" int u3x_k29_eok(const void *p);
extern "C" void u3x_k23_update(uintptr_t base, uint8_t *el);
extern "C" int u3x_cinema_config_push(uintptr_t base, const uint64_t *r);

namespace s04a {

// film list entry
constexpr uintptr_t ENTRY_SIZE = 0x1e90;
constexpr uintptr_t ENTRY_KEPT_END = 0x201;     // bytes 0x000..0x200 belong to the entry
constexpr uintptr_t ENTRY_REST = 0x208;         // bytes 0x208..0x1e8f belong to the entry
constexpr uintptr_t ENTRY_TIME = 0x208;         // s64 key
constexpr uintptr_t ENTRY_NAME = 0x100;

// film state (writer)
constexpr uintptr_t IMG_WRITER = 0x17c3ba8;
constexpr uintptr_t ST_VERSION = 0xa0;
constexpr uintptr_t ST_DIR = 0xb9;
constexpr uintptr_t ST_HEADER = 0x140;
constexpr uintptr_t ST_SELECTED = 0x1e48;
constexpr uintptr_t ST_LIST = 0x1e50;

// game data
constexpr uintptr_t IMG_NET = 0x17d5c58;
constexpr uintptr_t IMG_NET_GAME_INDEX = 0x17d167c;
constexpr uintptr_t IMG_CUSTOM_RULES = 0x17d1a98;
constexpr uintptr_t IMG_RENDER_VARS = 0x16e2fb8;
constexpr uintptr_t IMG_HANDLE_TABLE = 0x16e2f30;

// ExtState
constexpr uintptr_t EXT_UNLINK = 0x3220;
constexpr uintptr_t EXT_TIME = 0x4060;
constexpr int EB_DELETED = 19;

constexpr uint32_t PATH_SIZE = 0x180;
constexpr uint32_t F_STEP_60 = 0x3c888889u;     // 1/60 s
constexpr uint32_t FRAME_MARK = 0x7fcccccc;

template <typename T> [[gnu::always_inline]] inline void wr(void *p, uintptr_t off, T v) {
    *reinterpret_cast<T *>(static_cast<uint8_t *>(p) + off) = v;
}

// byte copy, ascending; the empty asm keeps the compiler from turning it into a memcpy call
[[gnu::always_inline]] inline void copy_bytes(uint8_t *to, const uint8_t *from, uintptr_t n) {
    for (uintptr_t i = 0; i < n; ++i) {
        to[i] = from[i];
        __asm__ volatile("" ::: "memory");
    }
}

[[gnu::always_inline]] inline int64_t entry_key(const uint8_t *e) { return rd<int64_t>(e, ENTRY_TIME); }

// common tail of stream read / write: advance, then check the capacity
[[gnu::always_inline]] inline void stream_advance(uintptr_t base, uint8_t *bs, uint32_t pos, uint32_t n) {
    const uint32_t end = pos + n;
    wr<uint32_t>(bs, 8, end);
    if (rd<uint32_t>(bs, 0xc) < end) u3h_rp_assert(base);
}

}  // namespace s04a

// A1: copy a film list entry; the slot-bound bytes 0x201..0x207 of `to` are left alone
extern "C" U3X_FN void u3x_film_entry_copy(uint8_t *to, const uint8_t *from) {
    using namespace s04a;
    copy_bytes(to, from, ENTRY_KEPT_END);
    copy_bytes(to + ENTRY_REST, from + ENTRY_REST, ENTRY_SIZE - ENTRY_REST);
}

// A2: stable sort, newest first, by straight insertion through `spare`
extern "C" U3X_FN void u3x_sort_films(uint8_t *list, int count, uint8_t *spare) {
    using namespace s04a;
    for (int k = 1; k < count; ++k) {
        uint8_t *slot = list + static_cast<uintptr_t>(k) * ENTRY_SIZE;
        if (!(entry_key(slot - ENTRY_SIZE) < entry_key(slot))) continue;
        u3x_film_entry_copy(spare, slot);
        int j = k;
        while (j > 0 && entry_key(list + static_cast<uintptr_t>(j - 1) * ENTRY_SIZE) < entry_key(spare)) {
            u3x_film_entry_copy(list + static_cast<uintptr_t>(j) * ENTRY_SIZE,
                                list + static_cast<uintptr_t>(j - 1) * ENTRY_SIZE);
            --j;
        }
        u3x_film_entry_copy(list + static_cast<uintptr_t>(j) * ENTRY_SIZE, spare);
    }
}

// A3: delete the selected local film file, step the selection back, rescan
extern "C" U3X_FN void u3x_film_delete(uintptr_t base, void *s) {
    using namespace s04a;
    uint8_t *st = u3h_cin_state(base);
    uint8_t *list = rd<uint8_t *>(st, ST_LIST);
    if (!list) return;
    // the index is unsigned for the path (no range check) ...
    const uint32_t sel = rd<uint32_t>(st, ST_SELECTED);
    char path[PATH_SIZE];
    u3h_cin_path(path, PATH_SIZE, reinterpret_cast<const char *>(st + ST_DIR),
                 reinterpret_cast<const char *>(list + static_cast<uint64_t>(sel) * ENTRY_SIZE + ENTRY_NAME));
    uint8_t *ext = u3h_ext();
    rd<int (*)(const char *)>(ext, EXT_UNLINK)(path);   // result ignored
    // ... and signed for the step back
    const int32_t cur = rd<int32_t>(st, ST_SELECTED);
    if (cur != 0) wr<int32_t>(st, ST_SELECTED, cur - 1);
    u3h_cin_scan(base, s, st);
    u3h_cin_flags(st);
    ext_eb()[EB_DELETED] += 1;
}

// A4: fill the record header fields the recording needs
extern "C" U3X_FN void u3x_film_record_header(uintptr_t base, uint8_t *st) {
    using namespace s04a;
    uint8_t *h = st + ST_HEADER;
    const uintptr_t net = base + IMG_NET;
    wr<uint32_t>(h, 0, rd<uint32_t>(st, ST_VERSION));
    wr<uint32_t>(h, 4, (at<int32_t>(net + 0x14) != 0 || at<int32_t>(net + 0x10) != 0) ? 1u : 0u);
    wr<uint32_t>(h, 8, static_cast<uint32_t>(at<uint16_t>(base + IMG_NET_GAME_INDEX)));
    const auto now = rd<int64_t (*)(int64_t *)>(u3h_ext(), EXT_TIME);
    if (now) wr<int64_t>(h, 0x20, now(nullptr));
    copy_bytes(h + 0x28, reinterpret_cast<const uint8_t *>(base + IMG_CUSTOM_RULES), 24);
    wr<uint32_t>(h, 0x1dc, at<int32_t>(net + 0x40) != 0 ? 2u : 0u);
}

// A5: restart the playback clock at time t
extern "C" U3X_FN void u3x_clock_reset(uint8_t *clock, uint32_t game_time, int32_t t) {
    using namespace s04a;
    wr<uint8_t>(clock, 0, 0);
    wr<uint32_t>(clock, 0x4, F_ONE);
    wr<uint32_t>(clock, 0x8, game_time);
    wr<int32_t>(clock, 0xc, t);
    wr<uint32_t>(clock, 0x10, 0);
    wr<uint32_t>(clock, 0x14, F_STEP_60);
    wr<uint32_t>(clock, 0x18, 0);
    wr<uint16_t>(clock, 0x1c, 0);
    wr<uint32_t>(clock, 0x20, 0);
    wr<uint32_t>(clock, 0x24, 0);
    wr<int32_t>(clock, 0x28, t);
    wr<uint32_t>(clock, 0x2c, 0);
    wr<uint64_t>(clock, 0x30, 0);
    wr<uint32_t>(clock, 0x38, 0);
}

// A6: take the film render defaults from the game's current render variables, then keep a copy of them
extern "C" U3X_FN void u3x_render_defaults(uintptr_t base) {
    using namespace s04a;
    uint8_t *w = reinterpret_cast<uint8_t *>(base + IMG_WRITER);
    const uint8_t *r = at<const uint8_t *>(base + IMG_RENDER_VARS);
    auto take = [&](uintptr_t to, uintptr_t from) { wr<uint32_t>(w, to, rd<uint32_t>(r, from)); };
    auto put = [&](uintptr_t to, uint32_t v) { wr<uint32_t>(w, to, v); };
    take(0x1fa0, 0x0);
    take(0x1fa4, 0x4);
    take(0x1fa8, 0x8);
    take(0x1ea8, 0xc);
    put(0x1eac, 0);
    put(0x1eb0, 0);
    put(0x1eb4, 0);
    take(0x1e94, 0x898);   // saturation
    take(0x1eb8, 0x880);   // tint multiply
    take(0x1ebc, 0x884);
    take(0x1ec0, 0x888);
    put(0x1e98, 0);
    take(0x1f94, 0x230);
    take(0x1f98, 0x234);
    take(0x1f9c, 0x238);
    put(0x1ec4, F_ONE);
    put(0x1ea4, 0);
    put(0x1e9c, 0);
    put(0x1ea0, 0);
    take(0x1ec8, 0xba4);
    take(0x1ecc, 0x4c);
    take(0x1ed0, 0x8c);
    take(0x1ed4, 0xcc);
    copy_bytes(w + 0x1f14, w + 0x1e94, 0x80);   // the kept copy
}

// A7: read n bits; the read happens before the capacity check
extern "C" U3X_FN uint32_t u3x_stream_read(uintptr_t base, uint8_t *bs, uint32_t n) {
    using namespace s04a;
    const uint32_t pos = rd<uint32_t>(bs, 8);
    const uint32_t v = u3x_bits_get(rd<const uint32_t *>(bs, 0), pos, n);
    stream_advance(base, bs, pos, n);
    return v;
}

// A8: write n bits; the write happens before the capacity check
extern "C" U3X_FN void u3x_stream_write(uintptr_t base, uint8_t *bs, uint32_t value, uint32_t n) {
    using namespace s04a;
    const uint32_t pos = rd<uint32_t>(bs, 8);
    u3x_bits_put(rd<uint32_t *>(bs, 0), pos, value, n);
    stream_advance(base, bs, pos, n);
}

// A9: move a bookmark into another slot; the buffer copy uses the TARGET's length
extern "C" U3X_FN void u3x_bookmark_move(uint8_t *d, const uint8_t *s) {
    using namespace s04a;
    wr<uint32_t>(d, 0x18, rd<uint32_t>(s, 0x18));
    wr<uint32_t>(d, 0x1c, rd<uint32_t>(s, 0x1c));
    wr<uint64_t>(d, 0x28, rd<uint64_t>(s, 0x28));
    wr<uint32_t>(d, 0x8, rd<uint32_t>(s, 0x8));
    wr<uint32_t>(d, 0xc, rd<uint32_t>(s, 0xc));
    const uint32_t n = rd<uint32_t>(d, 0x10);
    copy_bytes(rd<uint8_t *>(d, 0x20), rd<const uint8_t *>(s, 0x20), n);
}

// A10: a fresh bound frame with no parent and the given local location
extern "C" U3X_FN void u3x_frame_make(uint8_t *bf, const uint8_t *local) {
    using namespace s04a;
    for (uintptr_t i = 0; i < 0x60; ++i) {
        bf[i] = 0;
        __asm__ volatile("" ::: "memory");
    }
    wr<uint32_t>(bf, 0, FRAME_MARK);
    wr<uint32_t>(bf, 4, FRAME_MARK);
    wr<uint32_t>(bf, 0x3c, F_ONE);
    copy_bytes(bf + 0x40, local, 0x20);
}

// A11: refresh the parent location from the bound body while its handle still points at it
extern "C" U3X_FN void u3x_frame_parent(uintptr_t base, uint8_t *bf) {
    using namespace s04a;
    const uint8_t *handle = rd<const uint8_t *>(bf, 8);
    const uint32_t id = rd<uint32_t>(bf, 0x10);
    if (!handle || id == 0) return;
    const uint8_t *body = rd<const uint8_t *>(handle, 0);
    if (!body) return;
    const uint32_t body_id = rd<uint32_t>(body, 0x50);
    const bool same = body_id == id || (id <= 0xfffe && (body_id & 0xffff) == id);
    // full 32-bit slot number against the 16-bit slot field
    const uint64_t diff = reinterpret_cast<uint64_t>(handle) - at<uint64_t>(base + IMG_HANDLE_TABLE);
    const uint32_t slot = static_cast<uint32_t>(diff >> 3);
    if (same && static_cast<uint32_t>(rd<uint16_t>(body, 0x54)) == slot) copy_bytes(bf + 0x20, body, 0x20);
}
