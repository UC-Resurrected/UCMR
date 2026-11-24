// G49 part C: the animation state packer (spec G49_anim_packer.md, 2.3, 3.8, 4.14 to 4.16).

extern "C" {
// parts A and B of this group
void u3x_bsw(uint8_t *bs, uint32_t val, uint32_t n);
uint32_t u3x_ppc_ctiwz(uint32_t f);
void u3x_bsw_vec(uint8_t *bs, const uint32_t *v);
void u3x_bsw_frame(uintptr_t base, uint8_t *bs, uint8_t *f);
void u3x_loc_rel(uint32_t *o, const uint32_t *a, const uint32_t *b);
void u3x_bf_world(uintptr_t base, uint8_t *f, uint32_t *o);
uint8_t *u3x_bind_root(uintptr_t base, uint8_t *obj);
uint64_t u3x_ev_query(uintptr_t base, uint32_t sid, uint8_t *obj);
int32_t u3x_tab_find(uint8_t *t, uint32_t key);
uint8_t *u3x_art_tab(uint8_t *g);
int u3x_anim_lookup(uintptr_t base, uint32_t *tab, int32_t *idx, uint32_t id, uint8_t *anim, uint8_t **tabs);
uint8_t *u3x_stream_root(uintptr_t base, uint8_t *x, uint32_t *phase);
int u3x_layer_replicated(uintptr_t base, uint32_t id);
// other groups and the C part
uint8_t *u3x_np_by_id(uintptr_t base, uint32_t id);
uint8_t *u3h_repl_frame(int i);
uint8_t *u3h_repl_loc(void);
uint8_t *u3h_handle_obj(uint8_t *h);
uint8_t *u3h_cam_mgr(uintptr_t base, int32_t screen);
uint8_t *u3h_anim_cmd_buffer(void);
void u3h_loop_src119(uint8_t *buf, uint32_t n);

uint32_t u3x_anim_pack_body(uintptr_t base, uint8_t *msg, uint8_t *bs, uint8_t *actor, uint8_t **tabs, int32_t param5,
                            int32_t param6, uint8_t **buf);
}

namespace g49c {

constexpr uintptr_t IMG_PACKING = 0x16e79f0;       // u8: a state is being packed
constexpr uintptr_t GAME_REBIND = 0x1196230;       // (bound frame, body or null)
constexpr uintptr_t GAME_CMD_LIST = 0xfbe270;      // (animation controller, output block)
constexpr uint32_t CMD_CAPACITY = 0x2000;          // words

constexpr uint32_t EV_IS_CARRIED = 0x660778e0u;
constexpr uint32_t EV_CARRY_PLAYER = 0xfbd8cf95u;
constexpr uint32_t QUERY_AIM = 0x3a83fc57u;
constexpr uint32_t LAYER_BASE = 0x7339d08cu;
constexpr uint32_t ANIM_BINDPOSE = 0x1a1c52d3u;
constexpr uint32_t ANIM_TENSE_IDLE = 0x86dfee10u;

constexpr uint32_t F_15 = 0x41700000u;
constexpr uint32_t F_255 = 0x437f0000u;
constexpr uint32_t F_65535 = 0x477fff00u;
constexpr uint32_t FRAME_NAN = 0x7fccccccu;

constexpr uint32_t MAX_NOTES = 64;    // table entries and uses
constexpr int32_t MAX_LAYERS = 36;    // layers with a command total

enum : uint32_t {
    K_LAYER_BEGIN = 3, K_LAYER_END = 4, K_ANIM = 8, K_BLEND = 9, K_BLEND_PREV = 0xa, K_SNAPSHOT = 0xc,
    K_EMPTY_POSE = 0xd, K_LAYER_MARK = 0x19, K_COUNTED = 0x1a, K_ANIM_MIRROR = 0x1b
};

// a command field: not aligned
template <typename T> [[gnu::always_inline]] inline T field(const uint8_t *cmd, uintptr_t off) {
    struct [[gnu::packed]] Raw { T v; };
    return reinterpret_cast<const Raw *>(cmd + off)->v;
}

[[gnu::always_inline]] inline uint32_t *ec() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0x4450); }

// what the first pass finds out
struct Notes {
    uint32_t index[MAX_NOTES];     // table: index in the art group's animation table
    uint32_t group[MAX_NOTES];     // table: art group code
    uint32_t use[MAX_NOTES];       // entry number of each animation use, in order
    uint32_t entries;              // E
    uint32_t uses;                 // R
    int32_t total[MAX_LAYERS];     // counted commands per layer
};

// where a pass stands among the layers
struct Layers {
    int32_t number;     // L
    int32_t count;      // C
    bool muted;
};

// the list being read
struct List {
    const uint8_t *words;
    uint32_t n;       // words the game wrote
    uint32_t at;      // word offset of the next command
};

// next command, or null at the end of what is read (word count, empty command, layer end)
U3X_FN const uint8_t *next_command(List *l) {
    if (l->at >= l->n) return nullptr;
    const uint8_t *cmd = l->words + static_cast<uintptr_t>(l->at) * 4;
    const uint32_t length = field<uint16_t>(cmd, 2);
    if (length == 0 || field<uint16_t>(cmd, 0) == K_LAYER_END) {
        l->at = l->n;     // stops for good
        return nullptr;
    }
    l->at += length;
    return cmd;
}

U3X_FN void enter_layer(uintptr_t base, Layers *s, uint32_t id) {
    s->count = 0;
    s->number += 1;
    s->muted = !u3x_layer_replicated(base, id);
}

U3X_FN uint32_t frame_count(const uint8_t *anim) { return rd<uint16_t>(rd<const uint8_t *>(anim, 0x28), 0x22); }

// 4.15: the actor's bound frame, relative to the carrying player's character when attached
U3X_FN void pack_actor_frame(uintptr_t base, uint8_t *bs, uint8_t *actor) {
    uint8_t *frame = u3h_repl_frame(0);
    for (uint32_t i = 0; i < 0x60; i++) frame[i] = actor[0xb0 + i];

    uint32_t carrier = 0;
    if (u3x_ev_query(base, EV_IS_CARRIED, actor) & 0xff)
        carrier = static_cast<uint32_t>(u3x_ev_query(base, EV_CARRY_PLAYER, actor));

    if (!carrier) {
        u3x_bsw(bs, 0, 1);
    } else {
        u3x_bsw(bs, 1, 1);
        u3x_bsw(bs, carrier, 13);
        uint8_t *player = u3x_np_by_id(base, carrier);
        uint8_t *character = player ? u3h_handle_obj(player + 0x10) : nullptr;
        if (!character) {
            ec()[67] += 1;
        } else {
            uint32_t theirs[8], ours[8], rel[8];
            u3x_bf_world(base, character + 0xb0, theirs);
            u3x_bf_world(base, frame, ours);
            u3x_loc_rel(rel, theirs, ours);
            game_call<void>(base + GAME_REBIND, frame, static_cast<uint8_t *>(nullptr));
            uint32_t *local = reinterpret_cast<uint32_t *>(frame + 0x40);
            for (int i = 0; i < 8; i++) local[i] = rel[i];
        }
    }
    u3x_bsw_frame(base, bs, frame);
}

// 4.15: camera frame, zoom and aim point of a player; false when there is no camera manager
U3X_FN bool pack_camera(uintptr_t base, uint8_t *bs, uint8_t *actor) {
    const int32_t screen = rd<int32_t>(actor, 0x1c68);
    uint8_t *cam = (screen == 0 || screen == 1) ? u3h_cam_mgr(base, screen) : nullptr;
    if (!cam) {
        ec()[68] += 1;
        return false;
    }

    uint8_t *frame = u3h_repl_frame(1);
    for (uint32_t i = 0; i < 0x60; i++) frame[i] = 0;
    uint32_t *fw = reinterpret_cast<uint32_t *>(frame);
    fw[0] = FRAME_NAN;
    fw[1] = FRAME_NAN;
    fw[0x3c / 4] = F_ONE;
    for (int i = 0; i < 8; i++) fw[0x40 / 4 + i] = rd<uint32_t>(cam, 0x4c0 + i * 4);
    if (uint8_t *root = u3x_bind_root(base, actor)) game_call<void>(base + GAME_REBIND, frame, root);
    u3x_bsw_frame(base, bs, frame);

    const uint8_t *zoom = rd<const uint8_t *>(cam, 0xc0);
    u3x_bsw(bs, zoom ? u3x_ppc_ctiwz(rd<uint32_t>(zoom, 0x30)) : 0, 8);

    uint8_t *aim = u3h_repl_loc();
    for (uint32_t i = 0; i < 0x40; i++) aim[i] = 0;
    const uintptr_t aim_fn = rd<uintptr_t>(rd<const uint8_t *>(actor, 0), 0x170);
    game_call<void>(aim_fn, aim, actor, QUERY_AIM);
    u3x_bsw_vec(bs, reinterpret_cast<const uint32_t *>(aim));
    return true;
}

// notes one use of the pair (index, art group code)
U3X_FN void note(Notes *t, uint32_t index, uint32_t group) {
    uint32_t e = 0;
    while (e < t->entries && !(t->index[e] == index && t->group[e] == group)) e++;
    if (e == t->entries) {
        if (t->entries < MAX_NOTES) {
            t->index[e] = index;
            t->group[e] = group;
            t->entries += 1;
        } else {
            e = MAX_NOTES;
        }
    }
    if (t->uses < MAX_NOTES) t->use[t->uses++] = e;
}

U3X_FN bool not_found(uint32_t id) {
    uint32_t *e = ec();
    e[75] += 1;
    e[76] = id;
    return false;
}

// 4.16 first pass: which animations are used; false when one is not found
U3X_FN bool collect(uintptr_t base, List list, uint8_t **tabs, int32_t param5, int32_t param6, Notes *t) {
    Layers s;
    s.number = -1;
    s.count = 0;
    s.muted = false;
    while (const uint8_t *cmd = next_command(&list)) {
        const uint32_t kind = field<uint16_t>(cmd, 0);
        const bool dropped = s.muted || s.count >= param6;
        if (kind == K_LAYER_MARK) {
            enter_layer(base, &s, field<uint32_t>(cmd, 4));
        } else if (kind == K_COUNTED) {
            s.count += 1;
            if (s.number >= 0 && s.number < MAX_LAYERS) t->total[s.number] += 1;
        } else if ((kind == K_ANIM || kind == K_ANIM_MIRROR) && !dropped) {
            uint8_t *anim = field<uint8_t *>(cmd, 4);
            uint32_t unused_phase = 0;
            uint8_t *whole = u3x_stream_root(base, anim, &unused_phase);
            if (!whole) whole = anim;
            const uint32_t id = rd<uint32_t>(whole, 0x18);
            uint32_t group = 0;
            int32_t index = -1;
            if (!u3x_anim_lookup(base, &group, &index, id, whole, tabs) || index == -1 ||
                (static_cast<uint32_t>(index) & ~0xfffu))
                return not_found(id);
            note(t, static_cast<uint32_t>(index), group);
        } else if (kind == K_SNAPSHOT && !dropped) {
            const uint32_t pose = param5 ? ANIM_TENSE_IDLE : ANIM_BINDPOSE;
            uint8_t *table = u3x_art_tab(tabs[0]);
            const int32_t index = table ? u3x_tab_find(table, pose) : -1;
            if (index == -1) return not_found(pose);
            note(t, static_cast<uint32_t>(index), 0);
        }
    }
    return true;
}

U3X_FN void write_group_code(uint8_t *bs, uint32_t code) {
    if (code < 7) {
        u3x_bsw(bs, code, 3);
    } else {
        u3x_bsw(bs, 7, 3);
        u3x_bsw(bs, code, 32);
    }
}

// 3.8 item 5: the animation table
U3X_FN void write_table(uint8_t *bs, const Notes *t) {
    const uint32_t e = t->entries;
    u3x_bsw(bs, e, 5);
    u3x_bsw(bs, e == t->uses ? 1 : 0, 1);
    for (uint32_t i = 0; i < e; i++) u3x_bsw(bs, t->index[i], 12);

    bool same = true;
    for (uint32_t i = 1; i < e; i++)
        if (t->group[i] != t->group[0]) same = false;
    u3x_bsw(bs, same ? 1 : 0, 1);
    if (same) {
        write_group_code(bs, e ? t->group[0] : 0);
    } else {
        for (uint32_t i = 0; i < e; i++) write_group_code(bs, t->group[i]);
    }
}

// state of the second pass besides the layers
struct Out {
    uintptr_t base;
    uint8_t *bs;
    const Notes *t;
    int32_t last_slot;      // LS
    uint32_t use;           // U
    int32_t depth;          // D
    uint32_t prev_layer;    // PL
};

U3X_FN void depth_check(const Out *o) {
    if (o->depth > 5) ec()[69] += 1;
}

// which table entry the current use means (left out when every use has its own entry)
U3X_FN void write_entry(const Out *o) {
    const uint32_t e = o->t->entries;
    if (e == o->t->uses) return;
    const uint32_t entry = o->use < MAX_NOTES ? o->t->use[o->use] : 0;
    if (entry >= e) ec()[69] += 1;
    u3x_bsw(o->bs, entry, e <= 2 ? 1 : e <= 4 ? 2 : e <= 8 ? 3 : e <= 16 ? 4 : 5);
}

// the phase of an animation command, in 4, 8 or 16 bits by the animation's frame count
U3X_FN void write_phase(Out *o, const uint8_t *cmd) {
    uint8_t *anim = field<uint8_t *>(cmd, 4);
    uint32_t phase = field<uint32_t>(cmd, 0x10);
    uint32_t stream_phase = 0;
    uint8_t *whole = u3x_stream_root(o->base, anim, &stream_phase);
    if (whole)
        phase = f_mul(stream_phase, f_sub(f_from_int(static_cast<int32_t>(frame_count(whole))), F_ONE));
    else
        whole = anim;

    const uint32_t frames = frame_count(whole);
    const uint32_t bits = frames < 9 ? 4 : frames > 64 ? 16 : 8;
    const uint32_t scale = frames < 9 ? F_15 : frames > 64 ? F_65535 : F_255;
    const uint32_t q =
        u3x_ppc_ctiwz(f_div(f_mul(phase, scale), f_sub(f_from_int(static_cast<int32_t>(frames)), F_ONE)));
    o->depth += 1;
    u3x_bsw(o->bs, q, bits);
    o->use += 1;
}

U3X_FN void write_blend(Out *o, const uint8_t *cmd) {
    const uint32_t a = field<uint32_t>(cmd, 4), b = field<uint32_t>(cmd, 8), result = field<uint32_t>(cmd, 0xc);
    const uint32_t mode = field<uint32_t>(cmd, 0x10), weight = field<uint32_t>(cmd, 0x14);
    const bool full = weight == F_ONE;

    u3x_bsw(o->bs, full ? 1 : 0, 2);
    if (b < a) u3x_bsw(o->bs, 1, 1);
    else if (a < b) u3x_bsw(o->bs, 0, 1);
    else ec()[69] += 1;

    if (result == a) {
        u3x_bsw(o->bs, 1, 1);
        if (a < b) o->depth -= 1;
    } else if (result == b) {
        u3x_bsw(o->bs, 0, 1);
        if (b < a) o->depth -= 1;
    } else {
        ec()[69] += 1;
    }

    o->last_slot = static_cast<int32_t>(result);
    depth_check(o);
    u3x_bsw(o->bs, mode == 2 ? 1 : mode == 6 ? 2 : 0, 2);
    if (!full) u3x_bsw(o->bs, u3x_ppc_ctiwz(f_mul(weight, F_255)), 8);
}

// 4.16 second pass: the commands and the end code
U3X_FN void write_commands(uintptr_t base, uint8_t *bs, List list, int32_t param6, const Notes *t) {
    Layers s;
    s.number = -1;
    s.count = 0;
    s.muted = false;
    Out o = {base, bs, t, 0, 0, 0, 0};
    while (const uint8_t *cmd = next_command(&list)) {
        const uint32_t kind = field<uint16_t>(cmd, 0);
        const bool over = s.count >= param6;
        const bool dropped = s.muted || over;
        if (kind == K_LAYER_MARK) {
            const uint32_t id = field<uint32_t>(cmd, 4);
            if (o.prev_layer == LAYER_BASE || id == LAYER_BASE) {
                u3x_bsw(bs, 3, 2);
                u3x_bsw(bs, 2, 3);
            }
            enter_layer(base, &s, id);
            o.prev_layer = id;
        } else if (kind == K_COUNTED) {
            s.count += 1;
        } else if (kind == K_BLEND) {
            const bool last_of_layer =
                s.number >= 0 && s.number < MAX_LAYERS && s.count == t->total[s.number];
            if (!s.muted && o.depth >= 2 && (!over || last_of_layer)) write_blend(&o, cmd);
        } else if (dropped) {
            // nothing of a muted layer or beyond the layer's limit
        } else if (kind == K_ANIM || kind == K_ANIM_MIRROR) {
            depth_check(&o);
            o.last_slot = field<int32_t>(cmd, 0xc);
            if (kind == K_ANIM) {
                u3x_bsw(bs, 2, 2);
            } else {
                u3x_bsw(bs, 3, 2);
                u3x_bsw(bs, 5, 3);
            }
            write_entry(&o);
            write_phase(&o, cmd);
        } else if (kind == K_SNAPSHOT) {
            depth_check(&o);
            o.last_slot = field<int32_t>(cmd, 4);
            u3x_bsw(bs, 2, 2);
            write_entry(&o);
            o.depth += 1;
            u3x_bsw(bs, 0, 4);
            o.use += 1;
        } else if (kind == K_EMPTY_POSE) {
            depth_check(&o);
            o.last_slot = field<int32_t>(cmd, 4);
            u3x_bsw(bs, 3, 2);
            u3x_bsw(bs, 3, 3);
            o.depth += 1;
        } else if (kind == K_BLEND_PREV) {
            depth_check(&o);
            u3x_bsw(bs, 3, 2);
            u3x_bsw(bs, 0, 3);
            const uint32_t back = static_cast<uint32_t>(o.last_slot) - field<uint32_t>(cmd, 4);
            if (back & ~3u) ec()[69] += 1;
            u3x_bsw(bs, back, 2);
        }
    }
    u3x_bsw(bs, 3, 2);
    u3x_bsw(bs, 1, 3);
}

// 4.14 steps 3 to 14 (the "packing" byte is set around this)
U3X_FN uint32_t pack(uintptr_t base, uint8_t *msg, uint8_t *bs, uint8_t *actor, uint8_t **tabs, int32_t param5,
                     int32_t param6, uint8_t **buf) {
    uint8_t *controller = rd<uint8_t *>(actor, 0x138);
    if (!controller) return 0;
    uint8_t *words = u3h_anim_cmd_buffer();
    if (!words) {
        ec()[74] += 1;
        return 0;
    }
    *buf = words;

    struct alignas(16) { uint8_t *buffer; uint32_t capacity, written; uint64_t rest[2]; } block;
    block.buffer = words;
    block.capacity = CMD_CAPACITY;
    block.written = 0;
    block.rest[0] = 0;
    block.rest[1] = 0;
    game_call<void>(base + GAME_CMD_LIST, controller, reinterpret_cast<uint8_t *>(&block));
    const uint32_t n = block.written < CMD_CAPACITY ? block.written : CMD_CAPACITY;
    if (ec()[0] & 0x1000) u3h_loop_src119(words, n);

    u3x_bsw(bs, 0, 1);     // animation indexes are 12 bits wide
    pack_actor_frame(base, bs, actor);
    if ((rd<uint32_t>(msg, 0x148) & 1) && !pack_camera(base, bs, actor)) return 0;

    Notes notes;
    for (uint32_t i = 0; i < MAX_NOTES; i++) notes.index[i] = notes.group[i] = notes.use[i] = 0;
    for (int32_t i = 0; i < MAX_LAYERS; i++) notes.total[i] = 0;
    notes.entries = 0;
    notes.uses = 0;
    const List list = {words, n, 0};
    if (!collect(base, list, tabs, param5, param6, &notes)) return 0;
    if (notes.uses > 63 || notes.entries >= 32) {
        ec()[70] += 1;
        return 0;
    }
    write_table(bs, &notes);
    write_commands(base, bs, list, param6, &notes);
    return rd<uint32_t>(bs, 8);
}

}  // namespace g49c

extern "C" U3X_FN uint32_t u3x_anim_pack_body(uintptr_t base, uint8_t *msg, uint8_t *bs, uint8_t *actor, uint8_t **tabs,
                                              int32_t param5, int32_t param6, uint8_t **buf) {
    if (!actor || !tabs) {
        g49c::ec()[66] += 1;
        return 0;
    }
    at<uint8_t>(base + g49c::IMG_PACKING) = 1;
    const uint32_t position = g49c::pack(base, msg, bs, actor, tabs, param5, param6, buf);
    at<uint8_t>(base + g49c::IMG_PACKING) = 0;
    return position;
}
