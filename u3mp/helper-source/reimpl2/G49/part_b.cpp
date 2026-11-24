// G49 part B: the lookups of the animation state packer (spec G49_anim_packer.md, sections 2.2, 3.7, 4.8 to 4.13).

// other groups and the C part
extern "C" int u3x_bf_handle_valid(uintptr_t base, const uint8_t *bf);
extern "C" uint32_t u3x_sid(uintptr_t base, const char *s);
extern "C" uint32_t u3h_crc_sid(const char *name);
extern "C" uint8_t *u3h_find_art_group_name(uintptr_t base, const char *name);
extern "C" uint32_t u3h_layer_range_first(void);

// this part
extern "C" uint8_t *u3x_bind_root(uintptr_t base, uint8_t *obj);
extern "C" uint64_t u3x_ev_query(uintptr_t base, uint32_t sid, uint8_t *obj);
extern "C" int32_t u3x_tab_find(uint8_t *t, uint32_t key);
extern "C" uint8_t *u3x_art_tab(uint8_t *g);
extern "C" int u3x_anim_lookup(uintptr_t base, uint32_t *tab, int32_t *idx, uint32_t id, uint8_t *anim, uint8_t **tabs);
extern "C" uint8_t *u3x_stream_root(uintptr_t base, uint8_t *x, uint32_t *phase);
extern "C" int u3x_layer_replicated(uintptr_t base, uint32_t id);

namespace g49b {

// game functions and data (image offsets)
constexpr uintptr_t GAME_SEND_EVENT = 0x1197380;
constexpr uintptr_t GAME_BIND_PARENT = 0x10c5010;
constexpr uintptr_t GAME_STREAMED_ANIM = 0xfd62a0;
constexpr uintptr_t STREAM_COUNT = 0x1d46088;
constexpr uintptr_t STREAM_RECORDS = 0x1d46090;
constexpr int32_t STREAM_RECORDS_MAX = 50;

// the layers that are never sent: single layers by name (each name ends with 0, the list with an empty name)
U3X_RO const char unsent_layer_names[] =
    "Hands\0Hair\0facial-base\0hands\0breathing-layer\0net-anim-layer\0ragdoll\0ragdoll-snapshot\0";
// the fire animation layers: this name's id and the four ids after it
U3X_RO const char fire_layer_name[] = "FireAnim";
constexpr uint32_t FIRE_LAYERS = 5;
// four more layer ids are immediates in the game's own code, at these image offsets
constexpr uintptr_t LAYER_ID_WORD_FIRST = 0x52f018;
constexpr uintptr_t LAYER_ID_WORD_STEP = 0x80;
constexpr int LAYER_ID_WORDS = 4;
// the family of consecutive layer ids whose first id the C part gives
constexpr uint32_t RANGE_LAYERS = 15;

// a u32 at any address (the image words are not aligned)
[[gnu::always_inline]] inline uint32_t u32_unaligned(uintptr_t address) {
    const uint8_t *p = reinterpret_cast<const uint8_t *>(address);
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}

// the body a bound frame hangs on, or null when its handle is stale
[[gnu::always_inline]] inline uint8_t *frame_body(uintptr_t base, const uint8_t *frame) {
    if (!u3x_bf_handle_valid(base, frame)) return nullptr;
    return rd<uint8_t *>(rd<uint8_t *>(frame, 8), 0);
}

// index of an animation id in an art group (which may be null), or -1
[[gnu::always_inline]] inline int32_t find_in_group(uint8_t *group, uint32_t id) {
    uint8_t *table = u3x_art_tab(group);
    return table ? u3x_tab_find(table, id) : -1;
}

}  // namespace g49b

// 4.8: the body the object's chain of bindings hangs on
extern "C" U3X_FN uint8_t *u3x_bind_root(uintptr_t base, uint8_t *obj) {
    // the object's own answer first (before any test of the object)
    const uintptr_t vtable = rd<uintptr_t>(obj, 0);
    uint8_t *body = game_call<uint8_t *>(rd<uintptr_t>(reinterpret_cast<const void *>(vtable), 0x248), obj, uint32_t{0});
    uint8_t *o = obj;
    for (int n = 0; n < 64; ++n) {
        if (rd<uint16_t>(o, 0x128) & 0x800) return body;   // a binding root: the body found so far
        body = g49b::frame_body(base, o + 0xb0);
        o = game_call<uint8_t *>(base + g49b::GAME_BIND_PARENT, o);
        if (!o) break;
    }
    return nullptr;
}

// 4.9: sends event `sid` to `obj`, gives the first word of the answer
extern "C" U3X_FN uint64_t u3x_ev_query(uintptr_t base, uint32_t sid, uint8_t *obj) {
    alignas(16) uint64_t result[4];
    uint64_t args[4][3];
    for (int i = 0; i < 4; ++i) {
        result[i] = 0;
        for (int k = 0; k < 3; ++k) args[i][k] = 0;
    }
    game_call<void>(base + g49b::GAME_SEND_EVENT, static_cast<void *>(result), static_cast<void *>(nullptr), sid, obj,
                    static_cast<void *>(args[0]), static_cast<void *>(args[1]), static_cast<void *>(args[2]),
                    static_cast<void *>(args[3]));
    return result[0];
}

// 4.10: bisection in a table of 16-byte entries sorted by their first u32
extern "C" U3X_FN int32_t u3x_tab_find(uint8_t *t, uint32_t key) {
    const uint32_t count = rd<uint32_t>(t, 0);
    const uint8_t *entries = rd<const uint8_t *>(t, 8);
    if (!entries || !count) return -1;
    int64_t low = 0, high = static_cast<int64_t>(count) - 1;
    while (low <= high) {
        const int64_t mid = (low + high) / 2;
        const uint32_t probe = rd<uint32_t>(entries, static_cast<uintptr_t>(mid) * 16);
        if (probe == key) return static_cast<int32_t>(mid);
        if (probe > key) high = mid - 1;
        else low = mid + 1;
    }
    return -1;
}

// 4.10: the animation table of an art group
extern "C" U3X_FN uint8_t *u3x_art_tab(uint8_t *g) {
    if (!g) return nullptr;
    uint8_t *block = rd<uint8_t *>(g, 0x30);
    return block ? rd<uint8_t *>(block, 0x10) : nullptr;
}

// 4.11: where animation `id` is: in one of the actor's 8 art groups, or in the loaded group the animation names
extern "C" U3X_FN int u3x_anim_lookup(uintptr_t base, uint32_t *tab, int32_t *idx, uint32_t id, uint8_t *anim, uint8_t **tabs) {
    for (uint32_t i = 0; i < 8; ++i) {
        uint8_t *group = tabs[i];
        if (!group) continue;
        // the group name pointer is read again for every group, just before it is used
        if (anim && rd<uint32_t>(group, 0x18) != u3x_sid(base, rd<const char *>(anim, 0x38))) continue;
        const int32_t k = g49b::find_in_group(group, id);
        if (k != -1) {
            *idx = k;
            *tab = i;
            return 1;
        }
    }
    if (!anim) return 0;
    const char *name = rd<const char *>(anim, 0x38);   // read again for the fallback
    if (!name) return 0;
    const int32_t k = g49b::find_in_group(u3h_find_art_group_name(base, name), id);
    if (k == -1) return 0;
    *idx = k;
    *tab = u3x_sid(base, name);   // not one of the actor's groups: the group's id instead of a number
    return 1;
}

// 4.12: the streamed animation that chunk `x` is a piece of
extern "C" U3X_FN uint8_t *u3x_stream_root(uintptr_t base, uint8_t *x, uint32_t *phase) {
    int32_t records = at<int32_t>(base + g49b::STREAM_COUNT);
    if (records > g49b::STREAM_RECORDS_MAX) records = g49b::STREAM_RECORDS_MAX;
    for (int32_t r = 0; r < records; ++r) {
        const uint8_t *record = at<const uint8_t *>(base + g49b::STREAM_RECORDS + static_cast<uintptr_t>(r) * 8);
        if (!record) continue;
        const uint8_t *definition = rd<const uint8_t *>(record, 0x30);
        const uint8_t *lists_holder = rd<const uint8_t *>(record, 0x48);
        if (!definition || !lists_holder) continue;
        const uint32_t *counts = rd<const uint32_t *>(definition, 0);
        const uint32_t *const *ids = rd<const uint32_t *const *>(definition, 8);
        const uint32_t *const *slots = rd<const uint32_t *const *>(definition, 0x10);
        const uint32_t *last_channel = rd<const uint32_t *>(definition, 0x20);
        uint8_t *const *const *const *slot_channels = rd<uint8_t *const *const *const *>(lists_holder, 0);
        if (!counts || !ids || !slots || !last_channel || !slot_channels) continue;

        const uint32_t units = rd<uint32_t>(record, 0);
        for (uint32_t u = 0; u < units; ++u) {
            for (uint32_t k = 0; k < counts[u]; ++k) {
                const uint32_t slot = slots[u][k];
                uint8_t *const *const *channels = slot_channels[slot];
                if (!channels) continue;
                for (uint64_t c = 0; c <= last_channel[slot]; ++c) {
                    uint8_t *const *row = channels[c];   // one chunk per unit
                    if (!row || row[u] != x) continue;
                    *phase = rd<uint32_t>(record, 0x20);
                    return game_call<uint8_t *>(base + g49b::GAME_STREAMED_ANIM, rd<uint32_t>(x, 0x40),
                                                rd<uint32_t>(rd<const uint8_t *>(x, 0x28), 4), ids[u][k]);
                }
            }
        }
    }
    return nullptr;
}

// 4.13: 0 for the animation layers that are never sent, 1 for every other layer
extern "C" U3X_FN int u3x_layer_replicated(uintptr_t base, uint32_t id) {
    // single layers by name
    for (const char *name = g49b::unsent_layer_names; *name;) {
        if (id == u3h_crc_sid(name)) return 0;
        while (*name) ++name;
        ++name;
    }
    // single layers the game's code names by id
    for (int i = 0; i < g49b::LAYER_ID_WORDS; ++i)
        if (id == g49b::u32_unaligned(base + g49b::LAYER_ID_WORD_FIRST + static_cast<uintptr_t>(i) * g49b::LAYER_ID_WORD_STEP))
            return 0;
    // the two families of consecutive ids
    if (id - u3h_crc_sid(g49b::fire_layer_name) < g49b::FIRE_LAYERS) return 0;
    // first <= id <= first + 14 without wrap around zero (id - first is taken only when id >= first)
    const uint32_t first = u3h_layer_range_first();
    if (first <= id && id - first <= g49b::RANGE_LAYERS - 1) return 0;
    return 1;
}
