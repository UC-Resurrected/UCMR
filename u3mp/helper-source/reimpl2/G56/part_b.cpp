// G56 part B: emblem temp memory, texture files, data handles and texture records.
// Written from spec G56_emblem_raster.md (sections 2.2, 3.1, 3.2, 3.5 to 3.9, 4.5 to 4.13).

// ---- stays as it is (same file), other parts of the helper
extern "C" void u3x_emb_push(uintptr_t b);
extern "C" void u3x_emb_free(uintptr_t b, void *p);
extern "C" uint8_t *u3x_emb_cache(uintptr_t b, const char *name);

// ---- C part
extern "C" void *u3h_emb_fallback(void);
extern "C" void *u3h_emb_retire_queue(void);
extern "C" const uint8_t *u3h_emb_zero(void);
extern "C" uint8_t *u3h_emb_records(uint8_t **flag);

// ---- delivered here
extern "C" void u3x_emb_pop(uintptr_t b);
extern "C" void *u3x_emb_alloc(uintptr_t b, uint64_t n);
extern "C" uint8_t *u3x_emb_file(uintptr_t b, const char *name, uint64_t *size);
extern "C" uint8_t *u3x_emb_h(uintptr_t b, void *s, uintptr_t handle);
extern "C" void u3x_emb_bind1(uintptr_t b, void *s, uintptr_t handle, uint32_t sid);
extern "C" void u3x_emb_dcbind(uintptr_t b, void *s);
extern "C" int u3x_emb_isrec(uint64_t p);
extern "C" void u3x_emb_release(uintptr_t b, uint64_t *tex, uint64_t *map);
extern "C" void u3x_emb_retire(uintptr_t b, uint64_t *tex, uint64_t *map);
extern "C" void u3x_emb_retick(uintptr_t b);
extern "C" void u3x_emb_rfb(uintptr_t b, void *rec);
extern "C" void *u3x_emb_ralloc(uint32_t size);
extern "C" void u3x_emb_rfree(uintptr_t b, void *rec);
extern "C" int u3x_emb_texcreate(uintptr_t b, void *rec, const uint8_t *pix, uint32_t size);
extern "C" void u3x_emb_fb(uintptr_t b);
extern "C" int u3x_emb_dds(uintptr_t b, const char *name, void *dds);

namespace g56b {

// emblem record (spec 3.2)
struct Record {
    uint64_t tex;
    uint64_t map;
    uint8_t loaded;
    uint8_t pad[3];
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t size;
    uint16_t pin;
    uint16_t used;
    uint8_t key[40];
};
static_assert(sizeof(Record) == 80);

// retire queue entry (spec 3.5)
struct Parked {
    uint64_t tex;
    uint64_t map;
    uint32_t age;
    uint32_t unused;
};
static_assert(sizeof(Parked) == 0x18);

// data handle in the game image (spec 3.6)
struct Handle {
    uint8_t *value;
    uint32_t version;
    uint32_t id;
    uint8_t *module;
};
static_assert(sizeof(Handle) == 0x18);

// decoded-texture block (spec 3.3)
struct Decoded {
    uint8_t *file;
    uint32_t w;
    uint32_t h;
    uint32_t last;
    uint32_t bs;
    uint32_t off[16];
};
static_assert(sizeof(Decoded) == 88);

constexpr uint32_t RECORDS = 24;
constexpr uint32_t PARKED = 16;
constexpr uint32_t FORMAT_BGRA = 0x57;
constexpr uintptr_t ALLOC_TEXT = 0x147d6bb;       // text the allocator calls take
constexpr uintptr_t NO_MODULE = 0x1b3af40;        // "find module" answer for "none"
constexpr uintptr_t HANDLE_LAYER = 0x1b67e60;     // + 0x18 per layer
constexpr uintptr_t HANDLE_PALETTE = 0x1b67ec0;
constexpr uintptr_t HANDLE_SCALE = 0x1b67ee0;     // + 0x18 per layer

U3X_RO const char path_build[] = "/build/";
U3X_RO const char path_vtex[] = "/vtex1/ps4/";
U3X_RO const char path_dds[] = ".dds";

using FindModule = uint8_t *(*)(uint32_t id, int one);
using Allocate = void *(*)(void *object, uint64_t bytes, uint64_t alignment, const char *file, uint32_t line,
                           const char *function);

[[gnu::always_inline]] inline uint32_t &ec(uint32_t i) {   // diagnostic words ec[]
    return reinterpret_cast<uint32_t *>(u3h_ext() + 0x4450)[i];
}
[[gnu::always_inline]] inline Record *fallback() { return static_cast<Record *>(u3h_emb_fallback()); }
[[gnu::always_inline]] inline Record *record_table() {
    uint8_t *flag = nullptr;
    return reinterpret_cast<Record *>(u3h_emb_records(&flag));
}
[[gnu::always_inline]] inline Handle *handle_at(uintptr_t b, uintptr_t handle) {
    return reinterpret_cast<Handle *>(b + handle);
}
[[gnu::always_inline]] inline uint32_t u32_bytes(const uint8_t *p) {   // unaligned little-endian u32
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}

// the thread's allocator stack and its top entry (spec 3.8); null when there is none
[[gnu::always_inline]] inline uint8_t *alloc_stack(uintptr_t b) { return game_call<uint8_t *>(b + 0xda0d60); }

// the "find module" function of the state, with "none" turned into null
U3X_FN uint8_t *find_module(uintptr_t b, void *s, uint32_t id) {
    const FindModule find = rd<FindModule>(s, 0x348);
    if (!find) return nullptr;
    uint8_t *m = find(id, 1);
    if (reinterpret_cast<uintptr_t>(m) == b + NO_MODULE) m = nullptr;
    return m;
}

// appends text to the path while it is shorter than 0x1f8 characters
U3X_FN uint32_t path_add(char *path, uint32_t n, const char *text) {
    while (*text && n < 0x1f8) path[n++] = *text++;
    return n;
}

// one data id out of the game's own bind function: the immediate at image offset `where`, taken only when the
// instruction bytes around it are the expected ones
U3X_FN bool game_id(uintptr_t b, uintptr_t where, uint32_t *id) {
    const uint8_t *p = reinterpret_cast<const uint8_t *>(b + where);
    if (p[-1] != 0xbf || p[4] != 0xbe || p[5] != 0x01 || p[6] != 0 || p[7] != 0 || p[8] != 0) return false;
    *id = u32_bytes(p);
    return true;
}

// bad file: give it back, count it
U3X_FN int dds_reject(uintptr_t b, Decoded *d) {
    u3x_emb_free(b, d->file);
    d->file = nullptr;
    ec(123) += 1;
    return 0;
}

}  // namespace g56b

// ---- 4.5 temp memory

extern "C" U3X_FN void u3x_emb_pop(uintptr_t b) {
    uint8_t *st = g56b::alloc_stack(b);
    if (!st) return;
    const uint32_t count = rd<uint32_t>(st, 0x730);
    if (!count) return;
    uint64_t *entry = reinterpret_cast<uint64_t *>(st + 0x28 * static_cast<uintptr_t>(count - 1));
    for (int i = 0; i < 5; i++) entry[i] = 0;
    *reinterpret_cast<uint32_t *>(st + 0x730) = count - 1;
}

extern "C" U3X_FN void *u3x_emb_alloc(uintptr_t b, uint64_t n) {
    u3x_emb_push(b);
    void *result = nullptr;
    uint8_t *st = g56b::alloc_stack(b);
    if (st) {
        const uint32_t count = rd<uint32_t>(st, 0x730);
        if (count) {
            void *object = rd<void *>(st, 0x28 * static_cast<uintptr_t>(count - 1));
            if (object) {
                const g56b::Allocate allocate = rd<g56b::Allocate>(rd<void *>(object, 0), 0x48);
                const char *text = reinterpret_cast<const char *>(b + g56b::ALLOC_TEXT);
                result = allocate(object, n, 0x10, text, 0, text);
            }
        }
    }
    u3x_emb_pop(b);
    return result;
}

// ---- 4.6 one texture file, whole

extern "C" U3X_FN uint8_t *u3x_emb_file(uintptr_t b, const char *name, uint64_t *size) {
    const uint8_t *paths = at<const uint8_t *>(b + 0x1b24b80);
    if (!paths || !name) return nullptr;

    char path[0x200];
    uint32_t n = 0;
    n = g56b::path_add(path, n, reinterpret_cast<const char *>(paths + 0x408));
    n = g56b::path_add(path, n, g56b::path_build);
    n = g56b::path_add(path, n, reinterpret_cast<const char *>(paths + 0x479c));
    n = g56b::path_add(path, n, g56b::path_vtex);
    n = g56b::path_add(path, n, name);
    for (const char *t = g56b::path_dds; *t; t++) path[n++] = *t;
    path[n] = 0;

    uint64_t loaded[2] = {0, 0};   // data pointer, byte count
    u3x_emb_push(b);
    const uint64_t answer = game_call<uint64_t, uint64_t *, const char *>(b + 0xf42fa0, loaded, path);
    u3x_emb_pop(b);
    if (!(answer & 0xff) || !loaded[0]) return nullptr;
    *size = loaded[1];
    return reinterpret_cast<uint8_t *>(loaded[0]);
}

// ---- 4.7 to 4.9 data handles

extern "C" U3X_FN uint8_t *u3x_emb_h(uintptr_t b, void *s, uintptr_t handle) {
    g56b::Handle *h = g56b::handle_at(b, handle);
    const uint32_t id = h->id;
    if (!id) return nullptr;

    uint8_t *m = h->module;
    if (!m) {
        if (!rd<g56b::FindModule>(s, 0x348)) return nullptr;
        m = g56b::find_module(b, s, id);
        h->module = m;
        if (!m) return nullptr;
    } else if (h->version == rd<uint32_t>(m, 0x58)) {
        return h->value;
    }

    const uint8_t *table = rd<const uint8_t *>(m, 0x48);
    if (!table) return nullptr;
    int64_t low = 0, high = static_cast<int64_t>(rd<int32_t>(table, 0x30)) - 1;
    while (low <= high) {
        const int64_t middle = (low + high) / 2;
        const uint8_t *entry = rd<const uint8_t *>(table, 0x38) + 0x10 * middle;
        const uint32_t found = rd<uint32_t>(entry, 0);
        if (found == id) {
            h->value = rd<uint8_t *>(entry, 8);
            h->version = rd<uint32_t>(m, 0x58);
            return h->value;
        }
        if (found < id) low = middle + 1;
        else high = middle - 1;
    }
    return nullptr;
}

extern "C" U3X_FN void u3x_emb_bind1(uintptr_t b, void *s, uintptr_t handle, uint32_t sid) {
    g56b::Handle *h = g56b::handle_at(b, handle);
    uint8_t *m = g56b::find_module(b, s, sid);
    h->module = m;
    h->value = nullptr;
    h->version = 0xffffffffu;
    h->id = sid;
    if (m && rd<const uint8_t *>(m, 0x48)) u3x_emb_h(b, s, handle);
}

extern "C" U3X_FN void u3x_emb_dcbind(uintptr_t b, void *s) {
    // the seven ids, from the game's own bind function; a different build binds nothing
    uint32_t layer[3], scale[3], palette;
    if (!g56b::game_id(b, 0x66b756, &layer[0]) || !g56b::game_id(b, 0x66b808, &layer[1]) ||
        !g56b::game_id(b, 0x66b8aa, &layer[2]) || !g56b::game_id(b, 0x66b976, &scale[0]) ||
        !g56b::game_id(b, 0x66ba1a, &scale[1]) || !g56b::game_id(b, 0x66bac8, &scale[2]) ||
        !g56b::game_id(b, 0x66bb89, &palette))
        return;

    // layers 0..2, then scales 0..2; number 3 of each is a copy of number 2
    for (int set = 0; set < 2; set++) {
        const uintptr_t first = set ? g56b::HANDLE_SCALE : g56b::HANDLE_LAYER;
        const uint32_t *ids = set ? scale : layer;
        for (uintptr_t n = 0; n < 3; n++) u3x_emb_bind1(b, s, first + 0x18 * n, ids[n]);
        const g56b::Handle *from = g56b::handle_at(b, first + 0x18 * 2);
        g56b::Handle *to = g56b::handle_at(b, first + 0x18 * 3);
        to->value = from->value;
        to->version = from->version;
        to->id = from->id;
        to->module = from->module;
    }
    u3x_emb_bind1(b, s, g56b::HANDLE_PALETTE, palette);
}

// ---- 4.10 record test, release, retire

extern "C" U3X_FN int u3x_emb_isrec(uint64_t p) {
    const uint64_t table = reinterpret_cast<uint64_t>(g56b::record_table());
    if (p < table || p >= table + g56b::RECORDS * sizeof(g56b::Record)) return 0;
    return (p - table) % sizeof(g56b::Record) == 0;
}

extern "C" U3X_FN void u3x_emb_release(uintptr_t b, uint64_t *tex, uint64_t *map) {
    const uint32_t frame = *at<const uint32_t *>(b + 0x1c1f308);
    if (*tex && *tex != g56b::fallback()->tex) game_call<void, uint64_t *>(b + 0xe73490, tex);
    if (*map) game_call<void, uintptr_t, uint64_t, uint32_t>(b + 0xe4e8f0, b + 0x18c8960, *map, frame);
    *tex = 0;
    *map = 0;
}

extern "C" U3X_FN void u3x_emb_retire(uintptr_t b, uint64_t *tex, uint64_t *map) {
    if (*tex == g56b::fallback()->tex && !*map) {   // the shared fallback is never released
        *tex = 0;
        return;
    }
    if (!*tex && !*map) return;
    g56b::Parked *queue = static_cast<g56b::Parked *>(u3h_emb_retire_queue());
    for (uint32_t i = 0; i < g56b::PARKED; i++) {
        g56b::Parked &e = queue[i];
        if (e.tex || e.map) continue;
        e.tex = *tex;
        e.map = *map;
        e.age = 0;
        *tex = 0;
        *map = 0;
        return;
    }
    u3x_emb_release(b, tex, map);
}

extern "C" U3X_FN void u3x_emb_retick(uintptr_t b) {
    g56b::Parked *queue = static_cast<g56b::Parked *>(u3h_emb_retire_queue());
    for (uint32_t i = 0; i < g56b::PARKED; i++) {
        g56b::Parked &e = queue[i];
        if (!e.tex && !e.map) continue;
        e.age += 1;
        if (e.age >= 3) u3x_emb_release(b, &e.tex, &e.map);
    }
}

// ---- 4.11 records

extern "C" U3X_FN void u3x_emb_rfb(uintptr_t b, void *rec) {
    g56b::Record *r = static_cast<g56b::Record *>(rec);
    uint64_t old_tex = r->tex, old_map = r->map;
    const uint64_t shared = g56b::fallback()->tex;
    r->tex = shared;
    r->map = 0;
    r->loaded = shared ? 1 : 0;
    r->format = g56b::FORMAT_BGRA;
    if (old_map) u3x_emb_retire(b, &old_tex, &old_map);
}

extern "C" U3X_FN void *u3x_emb_ralloc(uint32_t size) {
    g56b::Record *table = g56b::record_table();
    for (uint32_t i = 0; i < g56b::RECORDS; i++) {
        g56b::Record *r = &table[i];
        if (r->used) continue;
        uint64_t *words = reinterpret_cast<uint64_t *>(r);
        for (int k = 0; k < 10; k++) words[k] = 0;
        const uint64_t shared = g56b::fallback()->tex;
        r->used = 1;
        r->size = r->width = r->height = size;
        r->tex = shared;
        r->loaded = shared ? 1 : 0;
        r->format = g56b::FORMAT_BGRA;
        g56b::ec(116) += 1;
        return r;
    }
    g56b::ec(122) = 1;
    return nullptr;
}

extern "C" U3X_FN void u3x_emb_rfree(uintptr_t b, void *rec) {
    g56b::Record *r = static_cast<g56b::Record *>(rec);
    if (!r || !u3x_emb_isrec(reinterpret_cast<uint64_t>(r)) || !r->used) return;
    u3x_emb_rfb(b, r);
    r->used = 0;
    r->pin = 0;
    g56b::ec(117) += 1;
}

// ---- 4.12 texture of a record, the fallback

extern "C" U3X_FN int u3x_emb_texcreate(uintptr_t b, void *rec, const uint8_t *pix, uint32_t size) {
    const uint64_t wide = size;
    int32_t context = 0xe;
    uint64_t description[2] = {wide | wide << 16 | 1ull << 32 | 1ull << 40 | 0x57ull << 48, 0x0000000800010001ull};
    const uint32_t row_bytes = size * 4, all_bytes = size * size * 4;
    uint64_t source[2] = {reinterpret_cast<uint64_t>(pix), row_bytes | static_cast<uint64_t>(all_bytes) << 32};
    uint64_t extra = 0;

    uint64_t map = game_call<uint64_t, int32_t *, uint64_t *, uint64_t *, uint64_t *>(b + 0xe76cb0, &context,
                                                                                    description, source, &extra);
    if (!map) {
        g56b::ec(122) = 2;
        return 0;
    }
    const uint64_t tex = game_call<uint64_t, uint64_t, uint32_t>(b + 0xe77c50, map, g56b::FORMAT_BGRA);
    if (!tex) {
        uint64_t none = 0;
        u3x_emb_release(b, &none, &map);
        g56b::ec(122) = 3;
        return 0;
    }

    g56b::Record *r = static_cast<g56b::Record *>(rec);
    uint64_t old_tex = r->tex, old_map = r->map;
    r->tex = tex;
    r->map = map;
    r->width = r->height = size;
    r->format = g56b::FORMAT_BGRA;
    r->loaded = 1;
    g56b::ec(115) += 1;
    if (old_map) u3x_emb_retire(b, &old_tex, &old_map);
    return 1;
}

extern "C" U3X_FN void u3x_emb_fb(uintptr_t b) {
    g56b::Record *shared = g56b::fallback();
    if (shared->tex || !at<uint64_t>(b + 0x1cf5330)) return;   // made already, or no graphics device yet

    uint64_t fresh[10];
    for (int k = 0; k < 10; k++) fresh[k] = 0;
    if (!u3x_emb_texcreate(b, fresh, u3h_emb_zero(), 4)) return;

    uint64_t *words = reinterpret_cast<uint64_t *>(shared);
    for (int k = 0; k < 10; k++) words[k] = fresh[k];
    shared->map = 0;   // so nothing ever releases it
    g56b::ec(158) = static_cast<uint32_t>(shared->tex);
    g56b::ec(159) = static_cast<uint32_t>(shared->tex >> 32);
}

// ---- 4.13 a DXT1 / DXT5 texture file and where its levels are

extern "C" U3X_FN int u3x_emb_dds(uintptr_t b, const char *name, void *dds) {
    g56b::Decoded *d = static_cast<g56b::Decoded *>(dds);
    uint64_t n = 0;
    d->file = u3x_emb_file(b, name, &n);
    if (!d->file) {
        g56b::ec(123) += 1;
        return 0;
    }
    if (n < 128 || rd<uint32_t>(d->file, 0) != 0x20534444u) return g56b::dds_reject(b, d);
    const uint32_t code = rd<uint32_t>(d->file, 84);
    const bool dxt1 = code == 0x31545844u;
    if (!dxt1 && code != 0x35545844u) return g56b::dds_reject(b, d);

    d->h = rd<uint32_t>(d->file, 12);
    d->w = rd<uint32_t>(d->file, 16);
    uint32_t levels = rd<uint32_t>(d->file, 28);
    if (levels == 0) levels = 1;
    if (levels > 16) levels = 16;
    d->bs = dxt1 ? 8 : 16;

    uint64_t position = 128;
    for (uint32_t l = 0; l < levels; l++) {
        d->off[l] = static_cast<uint32_t>(position);
        uint32_t lw = d->w >> l, lh = d->h >> l;
        if (lw < 1) lw = 1;
        if (lh < 1) lh = 1;
        const uint32_t across = (lw + 3) / 4, down = (lh + 3) / 4;
        position += static_cast<uint64_t>(across) * down * d->bs;
    }
    if (position > n || d->w == 0 || d->h == 0) return g56b::dds_reject(b, d);

    d->last = levels - 1;
    return 1;
}
