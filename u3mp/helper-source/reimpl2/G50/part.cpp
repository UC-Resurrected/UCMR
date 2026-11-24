// G50: anim state decoder and its entry points (written from spec/G50_anim_decoder.md).

extern "C" {
uint8_t *u3h_ext(void);
void u3h_decode(void *visitor, uint8_t *snapshot, uint8_t *generator, int32_t a4, int32_t a5);
int32_t u3h_pick_snapshot(uintptr_t base, uint8_t *generator, uint32_t *blend);
uint64_t u3h_call_f32(uintptr_t fn, uint32_t f0, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
uint32_t u3h_anim_pack(uintptr_t base, uint8_t *msg, uint8_t *stream, uint8_t *actor, uint8_t **tabs);
uint32_t *u3h_repl_error(void);
void u3h_repl_loop(uintptr_t base, uint8_t *actor, uint8_t **tabs, uint8_t *msg, uint32_t bits);
void u3x_bits_init(void *reader, uint8_t *buf, uint32_t nbytes);
uint32_t u3x_bits_read(void *reader, uint32_t n);
void u3x_bits_frame(uintptr_t base, void *reader, uint8_t *frame);
void u3x_bits_vector(uintptr_t base, void *reader, uint8_t *vec);
void u3x_frame_fresh(uint8_t *frame);
uint8_t *u3x_art_group_sid(uintptr_t base, uint32_t sid);
uint8_t *u3x_anim_by_index(uint8_t *group, int32_t index);
uint8_t *u3x_anim_by_sid(uint8_t *group, uint32_t sid);
uint8_t *u3x_stream_record(uintptr_t base, uint32_t id);
uint32_t u3x_v_frames(const uint8_t *anim);
void u3x_v_begin(void *visitor);
void u3x_v_blend(void *visitor, uint32_t weight, uint32_t a, uint32_t b, uint32_t into, uint32_t mode);
void u3x_v_mark(void *visitor, uint32_t phase, uint8_t *anim, uint32_t depth, int32_t a7);
void u3x_v_sub0(void *visitor, uint32_t x);
void u3x_v_sub2(void *visitor);
void u3x_v_sub4(void *visitor);
void u3x_v_push(void *visitor, uint32_t phase, uint8_t *anim);
void u3x_v_pose(void *visitor, uint32_t frame, uint8_t *anim, uint32_t index);
void u3x_v_bind(void *visitor, uint32_t index);
void u3x_v_sample(void *visitor, uint32_t phase, uint8_t *anim, uint32_t depth);

void u3x_decode_anim(void *visitor, uint8_t *bits, uint32_t nbytes, uint32_t flags, uint8_t **tabs, int32_t a4,
                     int32_t a5, int32_t a6, int32_t a7);
void u3x_apply_decode(uintptr_t base, const uint64_t *regs);
int u3x_blended_locator(uintptr_t base, uint64_t *regs);
void u3x_npc_bits(uintptr_t base, const uint64_t *regs);
void u3x_v_error(void *visitor, int code);
void u3x_v_anim(void *visitor, uint32_t phase, uint8_t *anim, uint32_t depth);
void u3x_v_sub3(void *visitor, uint32_t depth);
}

namespace g50 {

// the shared visitor record (spec 3.2)
struct alignas(16) Visitor {
    uint8_t frame[0x60];
    uint8_t locators[4][0x30];   // each: scale, rotation, translation (16 bytes each)
    uint64_t base;
    uint8_t *entries;
    uint8_t *commands;
    int32_t instance_base;
    int32_t depth_base;
    uint32_t attach_id;
    uint32_t key;
    uint32_t channel;
    uint8_t abort;
    uint8_t active;
    int32_t kind;
    int32_t error;
};
static_assert(sizeof(Visitor) == 0x160);
static_assert(__builtin_offsetof(Visitor, base) == 0x120);
static_assert(__builtin_offsetof(Visitor, channel) == 0x148);
static_assert(__builtin_offsetof(Visitor, abort) == 0x14c);
static_assert(__builtin_offsetof(Visitor, kind) == 0x150);
static_assert(__builtin_offsetof(Visitor, error) == 0x154);

// G51's bit reader record: only its position and error words are read here
struct alignas(8) Reader {
    uint8_t raw[0x20];
    uint32_t position() const { return rd<uint32_t>(raw, 8); }
    bool failed() const { return rd<uint32_t>(raw, 0x18) != 0; }
};

// the bit stream block the packer writes through
struct alignas(16) PackBlock {
    uint8_t *buffer;
    uint32_t position;
    uint32_t capacity;
};

struct alignas(16) Locator {
    uint8_t b[0x30];   // position, rotation, 16 zero bytes
};

constexpr uint32_t SNAPSHOT_SIZE = 0x3f0, SNAPSHOT_COUNT = 6, SNAPSHOTS_AT = 0xb0;
constexpr uint32_t ENTRY_AREA_AT = 0x10, ENTRY_AREA_SIZE = 0x214;
constexpr uint32_t TABS_AT = 0x68;
constexpr uint32_t DEFAULT_ANIM = 0xfff;

inline uint32_t *ec() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0x4450); }

// a w-bit quantised value as a float pattern: q * (1 / (2^w - 1)), the constant rounded to float first
inline uint32_t dequantise(uint32_t q, uint32_t width) {
    const uint32_t step = width == 4 ? 0x3d888889u : width == 8 ? 0x3b808081u : 0x37800080u;
    return f_mul(f_from_int(static_cast<int32_t>(q)), step);
}

// frame of a phase: phase * (frames - 1), the subtraction first
inline uint32_t frame_of_phase(uint32_t phase, const uint8_t *anim) {
    return f_mul(phase, f_sub(f_from_int(static_cast<int32_t>(u3x_v_frames(anim))), F_ONE));
}

// a type field: 3 bits, 7 = replaced by the 32 bits that follow
inline uint32_t read_type(Reader *r) {
    const uint32_t t = u3x_bits_read(r, 3);
    return t == 7 ? u3x_bits_read(r, 32) : t;
}

// a command code; the sub code changes only with code 3
inline uint32_t read_command(Reader *r, uint32_t *sub) {
    const uint32_t code = u3x_bits_read(r, 2);
    if (code == 3) *sub = u3x_bits_read(r, 3);
    return code;
}

inline void zero_bytes(uint8_t *p, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) p[i] = 0;
}

// the visitor's first locator as {position, rotation, zero}
inline void take_first_locator(const Visitor *v, Locator *out) {
    const uint8_t *rotation = v->locators[0] + 0x10, *translation = v->locators[0] + 0x20;
    for (uint32_t i = 0; i < 0x10; ++i) {
        out->b[i] = translation[i];
        out->b[0x10 + i] = rotation[i];
        out->b[0x20 + i] = 0;
    }
}

inline void start_visitor(Visitor *v, uintptr_t base, int32_t kind, uint8_t *entries, uint32_t channel) {
    v->base = base;
    v->kind = kind;
    v->error = 0;
    v->entries = entries;
    v->commands = nullptr;
    v->channel = channel;
}

}  // namespace g50

extern "C" U3X_FN void u3x_v_error(void *visitor, int code) {
    g50::Visitor *v = static_cast<g50::Visitor *>(visitor);
    if (v->error) return;   // only the first error counts
    v->error = code;
    if (code == 12) g50::ec()[104] += 1;
    else g50::ec()[82] = static_cast<uint32_t>(code);
}

extern "C" U3X_FN void u3x_v_anim(void *visitor, uint32_t phase, uint8_t *anim, uint32_t depth) {
    g50::Visitor *v = static_cast<g50::Visitor *>(visitor);
    if (v->kind == 0) {
        u3x_v_push(v, phase, anim);
    } else if (v->kind == 1) {
        if (depth + static_cast<uint32_t>(v->depth_base) > 6) { u3x_v_error(v, 6); return; }
        const uint32_t index = depth + static_cast<uint32_t>(v->instance_base);
        if (!anim) u3x_v_bind(v, index);
        else u3x_v_pose(v, g50::frame_of_phase(phase, anim), anim, index);
    } else {
        u3x_v_sample(v, phase, anim, depth);
    }
}

extern "C" U3X_FN void u3x_v_sub3(void *visitor, uint32_t depth) {
    g50::Visitor *v = static_cast<g50::Visitor *>(visitor);
    if (v->kind != 1) return;
    if (depth + static_cast<uint32_t>(v->depth_base) > 6) u3x_v_error(v, 6);
    else u3x_v_bind(v, depth + static_cast<uint32_t>(v->instance_base));
}

extern "C" U3X_FN void u3x_decode_anim(void *visitor, uint8_t *bits, uint32_t nbytes, uint32_t flags, uint8_t **tabs,
                                       int32_t a4, int32_t a5, int32_t a6, int32_t a7) {
    using namespace g50;
    Visitor *v = static_cast<Visitor *>(visitor);
    const uintptr_t base = v->base;   // image base as it is before u3x_v_begin
    u3x_v_begin(v);
    v->instance_base = a4;
    v->depth_base = a5;
    if (!nbytes) { u3x_v_error(v, 12); return; }

    // ---- header (no error test: failed reads give 0 and the walk goes on)
    Reader reader;
    Reader *r = &reader;
    u3x_bits_init(r, bits, nbytes);
    const bool wide = u3x_bits_read(r, 1) != 0;
    v->attach_id = u3x_bits_read(r, 1) ? u3x_bits_read(r, 13) : 0;
    u3x_bits_frame(base, r, v->frame);
    if (flags & 1) {   // camera block: read and dropped
        alignas(16) uint8_t camera[0x60];
        alignas(16) uint8_t aim[0x10];
        u3x_frame_fresh(camera);
        u3x_bits_frame(base, r, camera);
        u3x_bits_read(r, 8);
        u3x_bits_vector(base, r, aim);
    }
    const uint32_t count = u3x_bits_read(r, 5);
    const bool direct = u3x_bits_read(r, 1) != 0;
    uint32_t ids[31], types[31];
    for (uint32_t i = 0; i < count; ++i) ids[i] = u3x_bits_read(r, wide ? 32 : 12);
    if (u3x_bits_read(r, 1)) {
        const uint32_t t = read_type(r);
        for (uint32_t i = 0; i < count; ++i) types[i] = t;
    } else {
        for (uint32_t i = 0; i < count; ++i) types[i] = read_type(r);
    }
    const uint32_t index_bits = count <= 2 ? 1 : count <= 4 ? 2 : count <= 8 ? 3 : count <= 16 ? 4 : 5;

    // ---- commands
    uint32_t sub = 6;
    uint32_t code = read_command(r, &sub);
    uint32_t entry = 0, depth = 0;
    while (!v->abort && sub != 1) {
        if (reader.failed()) { u3x_v_error(v, 1); break; }
        if (v->error) break;

        const bool marker_anim = code == 3 && sub == 5;
        if (code == 2 || marker_anim) {
            if (!direct) entry = u3x_bits_read(r, index_bits);
            if (entry >= count) { u3x_v_error(v, 11); break; }
            const uint32_t type = types[entry], id = ids[entry];
            uint8_t *group = type < 7 ? tabs[type] : u3x_art_group_sid(base, type);
            if (!group) { u3x_v_error(v, 3); break; }
            uint8_t *anim;
            if (id == DEFAULT_ANIM) anim = tabs[0] ? u3x_anim_by_index(tabs[0], a6) : nullptr;
            else if (wide) anim = u3x_anim_by_sid(group, id);
            else anim = u3x_anim_by_index(group, static_cast<int32_t>(id));
            if (!anim || !rd<uint8_t *>(anim, 0x28)) { u3x_v_error(v, 4); break; }

            uint32_t width = 16;
            if (!wide) {
                const uint32_t frames = u3x_v_frames(anim);
                width = frames <= 8 ? 4 : frames <= 64 ? 8 : 16;
            }
            uint32_t phase = dequantise(u3x_bits_read(r, width), width);

            if (rd<uint32_t>(anim, 0x24) & 4) {   // streamed: the game's record gives phase and chunk
                const uint32_t anim_id = rd<uint32_t>(anim, 0x18);
                const uint8_t *record = u3x_stream_record(base, anim_id);
                if (record) {
                    phase = rd<uint32_t>(record, 0x20);
                    anim = game_call<uint8_t *>(base + 0xfd4c60, anim, anim_id, &phase);
                } else {
                    phase = 0;
                    anim = nullptr;
                }
            }
            if (reader.failed()) { u3x_v_error(v, 1); break; }
            entry += 1;
            if (marker_anim) u3x_v_mark(v, phase, anim, depth, a7);
            else u3x_v_anim(v, phase, anim, depth);
            depth += 1;
        } else if (code < 2) {   // blend: code 0 with a weight, code 1 with weight 1.0
            if (depth < 2) { u3x_v_error(v, 2); break; }
            const uint32_t low = depth - 2, high = depth - 1;
            const bool swapped = u3x_bits_read(r, 1) != 0;
            const uint32_t a = swapped ? high : low, b = swapped ? low : high;
            const uint32_t into = u3x_bits_read(r, 1) ? a : b;
            if (into == low) depth -= 1;
            const uint32_t wire_mode = u3x_bits_read(r, 2);
            const uint32_t mode = wire_mode == 1 ? 2 : wire_mode == 2 ? 6 : 1;
            const uint32_t weight = code == 0 ? dequantise(u3x_bits_read(r, 8), 8) : F_ONE;
            if (reader.failed()) { u3x_v_error(v, 1); break; }
            u3x_v_blend(v, weight, a, b, into, mode);
        } else if (sub == 0) {   // reference to an earlier slot (depth 0 wraps and never fails)
            const uint32_t back = u3x_bits_read(r, 2);
            if (depth - 1 < back) { u3x_v_error(v, 5); break; }
            u3x_v_sub0(v, depth - back - 1);
        } else if (sub == 2) {
            u3x_v_sub2(v);
        } else if (sub == 3) {
            u3x_v_sub3(v, depth);
            depth += 1;
        } else if (sub == 4) {
            depth -= 1;
            u3x_v_sub4(v);
        }
        code = read_command(r, &sub);
    }

    if (reader.failed() && !v->error) u3x_v_error(v, 1);
    ec()[101] = reader.position();
}

extern "C" U3X_FN void u3x_apply_decode(uintptr_t base, const uint64_t *regs) {
    using namespace g50;
    if (ec()[0] & 0x200) return;
    uint8_t *generator = reinterpret_cast<uint8_t *>(regs[4]);
    const uint64_t offset = regs[3];
    if (!generator || offset >= SNAPSHOT_COUNT * SNAPSHOT_SIZE) return;
    uint8_t *snapshot = generator + SNAPSHOTS_AT + offset;
    ec()[80] += 1;

    Visitor v;
    start_visitor(&v, base, 0, snapshot + ENTRY_AREA_AT, 0);
    u3h_decode(&v, snapshot, generator, 0, 0);
    if (v.error) {
        if (v.error != 12) ec()[81] += 1;
        zero_bytes(snapshot + ENTRY_AREA_AT, ENTRY_AREA_SIZE);
    }
    ec()[83] = rd<uint32_t>(snapshot, 0x220);
}

extern "C" U3X_FN int u3x_blended_locator(uintptr_t base, uint64_t *regs) {
    using namespace g50;
    if (ec()[0] & 0x200) return 0;
    uint8_t *out = reinterpret_cast<uint8_t *>(regs[4]);
    uint8_t *generator = reinterpret_cast<uint8_t *>(regs[5]);
    const uint32_t channel = static_cast<uint32_t>(regs[6]);
    if (!out || !generator) return 0;
    ec()[86] += 1;

    uint32_t blend;
    const int32_t index = u3h_pick_snapshot(base, generator, &blend);
    if (index == -1) {   // nothing to blend: zero position, identity rotation
        uint32_t *words = reinterpret_cast<uint32_t *>(out);
        for (int i = 0; i < 7; ++i) words[i] = 0;
        words[7] = F_ONE;
        regs[8] = reinterpret_cast<uint64_t>(out);
        return 1;
    }
    uint8_t *first = generator + SNAPSHOTS_AT + static_cast<int64_t>(index) * static_cast<int64_t>(SNAPSHOT_SIZE);
    uint8_t *second = first + SNAPSHOT_SIZE;

    Visitor v;
    Locator a, b, result;
    start_visitor(&v, base, 2, nullptr, channel);
    u3h_decode(&v, first, generator, 0, 0);
    take_first_locator(&v, &a);
    if (!v.error) u3h_decode(&v, second, generator, 1, 1);
    if (v.error) {
        ec()[87] += 1;
        if (v.error != 12) ec()[81] += 1;
        return 0;
    }
    take_first_locator(&v, &b);
    u3h_call_f32(base + 0xfd3c60, blend, reinterpret_cast<uint64_t>(&result), reinterpret_cast<uint64_t>(&a),
                 reinterpret_cast<uint64_t>(&b), 0);
    for (uint32_t i = 0; i < 0x20; ++i) out[i] = result.b[i];
    regs[8] = reinterpret_cast<uint64_t>(out);
    return 1;
}

extern "C" U3X_FN void u3x_npc_bits(uintptr_t base, const uint64_t *regs) {
    using namespace g50;
    uint8_t *message = reinterpret_cast<uint8_t *>(regs[4]);
    uint8_t *npc = reinterpret_cast<uint8_t *>(regs[0]);
    uint8_t *holder = reinterpret_cast<uint8_t *>(regs[13]);
    if (!message) return;
    uint32_t bits = 0;
    if (!(ec()[0] & 0x800) && npc && holder) {
        uint8_t **tabs = reinterpret_cast<uint8_t **>(holder + TABS_AT);
        ec()[90] += 1;
        zero_bytes(message + 0x18, 300);
        PackBlock block;
        block.buffer = message + 0x18;
        block.position = 0;
        block.capacity = 300 * 8;
        *u3h_repl_error() = 0;
        bits = u3h_anim_pack(base, message, reinterpret_cast<uint8_t *>(&block), npc, tabs);
        if (*u3h_repl_error()) {
            ec()[92] += 1;
            bits = 0;
        }
        ec()[91] = bits;
        u3h_repl_loop(base, npc, tabs, message, bits);
    }
    *reinterpret_cast<uint32_t *>(message + 0x144) = bits;   // the word the replaced instruction stored
}
