// G52: anim decoder visitors (written from spec G52_anim_visitors.md).
// The functions of the visitor for its three kinds (0 state, 1 commands, 2 locator) and the command buffer writers.

extern "C" {
// other groups / C part (not written here)
void u3x_v_error(void *visitor, int code);
void u3x_v_bind(void *visitor, uint32_t index);
void u3x_v_copy(void *visitor, uint32_t a, uint32_t b);
uint32_t u3x_v_frames(const uint8_t *anim);
void u3x_frame_fresh(uint8_t *frame);
uint64_t u3x_call_float_arg(uintptr_t fn, uint32_t f0, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
uint8_t *u3h_stream_root(uintptr_t base, uint8_t *anim, uint32_t *phase);

// this group
uint32_t *u3x_v_command(void *visitor, uint16_t type, uint16_t words);
void u3x_v_max(void *visitor, uint32_t index);
void u3x_v_pose(void *visitor, uint32_t frame, uint8_t *anim, uint32_t index);
void u3x_v_blend_cmd(void *visitor, uint32_t weight, uint32_t a, uint32_t b, uint32_t into, uint32_t mode);
void u3x_v_sub(void *visitor, uint32_t index);
void u3x_v_ice(void *visitor, uint16_t type, uint32_t index, uint64_t p1, uint64_t p2, uint64_t p3);
void u3x_v_begin(void *visitor);
void u3x_v_push(void *visitor, uint32_t phase, uint8_t *anim);
void u3x_v_sample(void *visitor, uint32_t phase, uint8_t *anim, uint32_t depth);
void u3x_v_blend(void *visitor, uint32_t weight, uint32_t a, uint32_t b, uint32_t into, uint32_t mode);
void u3x_v_sub0(void *visitor, uint32_t x);
void u3x_v_sub4(void *visitor);
void u3x_v_mark(void *visitor, uint32_t phase, uint8_t *anim, uint32_t depth, int32_t a7);
void u3x_frame_identity(uint8_t *frame);
}

namespace g52 {

// visitor record fields (G50 3.2)
constexpr uintptr_t V_LOCATORS = 0x60, V_BASE = 0x120, V_ENTRIES = 0x128, V_COMMANDS = 0x130, V_INSTANCE_BASE = 0x138,
                    V_DEPTH_BASE = 0x13c, V_KEY = 0x144, V_CHANNEL = 0x148, V_ABORT = 0x14c, V_ACTIVE = 0x14d,
                    V_KIND = 0x150, V_ERROR = 0x154;
constexpr uint32_t LOCATOR_SIZE = 0x30, LOCATOR_COUNT = 4;

// command buffer of the game (3.2)
constexpr uintptr_t CB_WORDS = 0, CB_CAPACITY = 8, CB_POSITION = 0xc, CB_HIGHEST = 0x10;

// snapshot entry area (3.3)
constexpr uint32_t ENTRY_SIZE = 0x18, ENTRY_LIMIT = 22;
constexpr uintptr_t ENTRIES_COUNT = 0x210;

enum : int { ERR_NO_ANIM = 4, ERR_DEPTH = 6, ERR_FULL = 7, ERR_SLOT = 8, ERR_ICE_ALIGN = 10 };

template <typename T> [[gnu::always_inline]] inline T &field(void *p, uintptr_t off) {
    return *reinterpret_cast<T *>(static_cast<uint8_t *>(p) + off);
}
[[gnu::always_inline]] inline uint8_t *locator(void *visitor, uint32_t slot) {
    return static_cast<uint8_t *>(visitor) + V_LOCATORS + static_cast<uintptr_t>(slot) * LOCATOR_SIZE;
}
[[gnu::always_inline]] inline uint32_t *diag(uint32_t n) {   // ec[n]
    return reinterpret_cast<uint32_t *>(u3h_ext() + 0x4450) + n;
}
// a pointer in a command: two consecutive words
[[gnu::always_inline]] inline void put_pointer(uint32_t *words, uint64_t value) {
    words[0] = static_cast<uint32_t>(value);
    words[1] = static_cast<uint32_t>(value >> 32);
}

// scale 1, rotation (0, 0, 0, 1), translation 0
U3X_FN void identity_locator(uint8_t *loc) {
    uint32_t *w = reinterpret_cast<uint32_t *>(loc);
    for (uint32_t i = 0; i < LOCATOR_SIZE / 4; ++i) w[i] = (i < 4 || i == 7) ? F_ONE : 0u;
}

// the request of the channel sampler of the game
struct alignas(16) SampleRequest {
    uint64_t zero0;
    uint8_t *anim;
    uint32_t channel;
    uint32_t phase;
    uint64_t zero18;
    uint64_t zero20;
};

}  // namespace g52

// ---- command buffer -------------------------------------------------------------------------------------------

// room for one command: writes its type and length, result = its first word (null: earlier error, or full)
extern "C" U3X_FN uint32_t *u3x_v_command(void *visitor, uint16_t type, uint16_t words) {
    using namespace g52;
    if (field<int32_t>(visitor, V_ERROR) != 0) return nullptr;
    uint8_t *buffer = field<uint8_t *>(visitor, V_COMMANDS);
    const uint32_t position = rd<uint32_t>(buffer, CB_POSITION);
    if (position + static_cast<uint32_t>(words) > rd<uint32_t>(buffer, CB_CAPACITY)) {
        u3x_v_error(visitor, ERR_FULL);
        return nullptr;
    }
    uint32_t *command = rd<uint32_t *>(buffer, CB_WORDS) + position;
    uint16_t *header = reinterpret_cast<uint16_t *>(command);
    header[0] = type;
    header[1] = words;
    field<uint32_t>(buffer, CB_POSITION) = position + words;
    return command;
}

// raises the highest instance index of the buffer
extern "C" U3X_FN void u3x_v_max(void *visitor, uint32_t index) {
    using namespace g52;
    uint8_t *buffer = field<uint8_t *>(visitor, V_COMMANDS);
    if (index > rd<uint32_t>(buffer, CB_HIGHEST)) field<uint32_t>(buffer, CB_HIGHEST) = index;
}

extern "C" U3X_FN void u3x_v_pose(void *visitor, uint32_t frame, uint8_t *anim, uint32_t index) {
    uint32_t *c = u3x_v_command(visitor, 8, 5);
    if (!c) return;
    g52::put_pointer(c + 1, reinterpret_cast<uint64_t>(anim));
    c[3] = index;
    c[4] = frame;
    u3x_v_max(visitor, index);
}

extern "C" U3X_FN void u3x_v_blend_cmd(void *visitor, uint32_t weight, uint32_t a, uint32_t b, uint32_t into, uint32_t mode) {
    uint32_t *c = u3x_v_command(visitor, 9, 6);
    if (!c) return;
    c[1] = a;
    c[2] = b;
    c[3] = into;
    c[4] = mode;
    c[5] = weight;
    u3x_v_max(visitor, a);
    u3x_v_max(visitor, b);
    u3x_v_max(visitor, into);
}

extern "C" U3X_FN void u3x_v_sub(void *visitor, uint32_t index) {
    uint32_t *c = u3x_v_command(visitor, 0xa, 2);
    if (!c) return;
    c[1] = index;
    u3x_v_max(visitor, index);
}

extern "C" U3X_FN void u3x_v_ice(void *visitor, uint16_t type, uint32_t index, uint64_t p1, uint64_t p2, uint64_t p3) {
    if ((p1 | p2 | p3) & 15) {   // a pointer that is not 16-byte aligned
        u3x_v_error(visitor, g52::ERR_ICE_ALIGN);
        return;
    }
    uint32_t *c = u3x_v_command(visitor, type, 8);
    if (!c) return;
    c[1] = index;
    g52::put_pointer(c + 2, p1);
    g52::put_pointer(c + 4, p2);
    g52::put_pointer(c + 6, p3);
    u3x_v_max(visitor, index);
}

// ---- identity bound frame -------------------------------------------------------------------------------------

extern "C" U3X_FN void u3x_frame_identity(uint8_t *frame) {
    uint32_t *w = reinterpret_cast<uint32_t *>(frame);
    for (uint32_t i = 0; i < 0x60 / 4; ++i) w[i] = 0;
    w[0] = 0x7fccccccu;          // "not bound" marker words
    w[1] = 0x7fccccccu;
    w[0x3c / 4] = F_ONE;         // parent rotation w
    w[0x5c / 4] = F_ONE;         // local rotation w
}

// ---- visitor functions ----------------------------------------------------------------------------------------

// start of a decode
extern "C" U3X_FN void u3x_v_begin(void *visitor) {
    using namespace g52;
    u3x_frame_fresh(static_cast<uint8_t *>(visitor));
    field<uint8_t>(visitor, V_ABORT) = 0;
    const int32_t kind = field<int32_t>(visitor, V_KIND);
    if (kind == 0) {
        field<uint32_t>(visitor, V_KEY) = 0;
    } else if (kind == 2) {
        for (uint32_t i = 0; i < LOCATOR_COUNT; ++i) identity_locator(locator(visitor, i));
        field<uint8_t>(visitor, V_ACTIVE) = 0;
    }
}

// state visitor: one entry of the snapshot list for an anim that has effects
extern "C" U3X_FN void u3x_v_push(void *visitor, uint32_t phase, uint8_t *anim) {
    using namespace g52;
    field<uint32_t>(visitor, V_KEY) = 0;
    if (!anim) return;
    const uint8_t *effects = rd<const uint8_t *>(anim, 0x48);
    if (!effects || rd<uint32_t>(effects, 4) == 0 || !rd<const uint8_t *>(anim, 0x28)) return;
    field<uint32_t>(visitor, V_KEY) = rd<uint32_t>(effects, 0);

    // a chunk of a streaming animation is stored as that streaming animation
    uint32_t root_phase = 0;   // reported, not used
    uint8_t *root = u3h_stream_root(field<uintptr_t>(visitor, V_BASE), anim, &root_phase);
    uint8_t *stored = root ? root : anim;

    uint8_t *entries = field<uint8_t *>(visitor, V_ENTRIES);
    const uint32_t count = rd<uint32_t>(entries, ENTRIES_COUNT);
    if (count >= ENTRY_LIMIT) {   // list full: the key stays set
        *diag(102) += 1;
        return;
    }
    uint8_t *entry = entries + count * ENTRY_SIZE;
    field<uint32_t>(entries, ENTRIES_COUNT) = count + 1;   // count first, then the fields
    field<uint32_t>(entry, 0) = field<uint32_t>(visitor, V_KEY);
    field<uint8_t *>(entry, 8) = stored;
    field<uint32_t>(entry, 0x10) = phase;
    field<uint8_t>(entry, 0x14) = 1;   // set
    field<uint8_t>(entry, 0x15) = 0;   // referenced again
}

// locator visitor: sample the channel of the anim into locator number depth
extern "C" U3X_FN void u3x_v_sample(void *visitor, uint32_t phase, uint8_t *anim, uint32_t depth) {
    using namespace g52;
    if (!field<uint8_t>(visitor, V_ACTIVE)) return;
    if (depth >= LOCATOR_COUNT) { u3x_v_error(visitor, ERR_SLOT); return; }
    if (!anim) { u3x_v_error(visitor, ERR_NO_ANIM); return; }
    SampleRequest request;
    request.zero0 = 0;
    request.anim = anim;
    request.channel = field<uint32_t>(visitor, V_CHANNEL);
    request.phase = phase;
    request.zero18 = 0;
    request.zero20 = 0;
    uint8_t *out = locator(visitor, depth);
    const uint64_t ok = game_call<uint64_t, uint64_t, SampleRequest *, uint8_t *>(
        field<uintptr_t>(visitor, V_BASE) + 0xfbaca0, 0, &request, out);
    if ((ok & 0xff) == 0) identity_locator(out);
}

extern "C" U3X_FN void u3x_v_blend(void *visitor, uint32_t weight, uint32_t a, uint32_t b, uint32_t into, uint32_t mode) {
    using namespace g52;
    const int32_t kind = field<int32_t>(visitor, V_KIND);
    if (kind == 1) {
        const uint32_t ib = field<uint32_t>(visitor, V_INSTANCE_BASE);
        u3x_v_blend_cmd(visitor, weight, a + ib, b + ib, into + ib, mode);
    } else if (kind == 2 && field<uint8_t>(visitor, V_ACTIVE)) {
        if (a >= LOCATOR_COUNT || b >= LOCATOR_COUNT || into >= LOCATOR_COUNT) {
            u3x_v_error(visitor, ERR_SLOT);
            return;
        }
        u3x_call_float_arg(field<uintptr_t>(visitor, V_BASE) + 0xfd3c60, weight,
                           reinterpret_cast<uint64_t>(locator(visitor, into)),
                           reinterpret_cast<uint64_t>(locator(visitor, a)),
                           reinterpret_cast<uint64_t>(locator(visitor, b)), 0);
    }
}

// "back" reference
extern "C" U3X_FN void u3x_v_sub0(void *visitor, uint32_t x) {
    using namespace g52;
    const int32_t kind = field<int32_t>(visitor, V_KIND);
    if (kind == 0) {
        const uint32_t key = field<uint32_t>(visitor, V_KEY);
        if (!key) return;
        uint8_t *entries = field<uint8_t *>(visitor, V_ENTRIES);
        // the count is signed here: a negative count means no search
        const int32_t count = rd<int32_t>(entries, ENTRIES_COUNT);
        for (int32_t i = count > 0 ? count : 0; i-- > 0;) {   // newest first
            uint8_t *entry = entries + static_cast<uintptr_t>(i) * ENTRY_SIZE;
            if (rd<uint32_t>(entry, 0) == key) {
                field<uint8_t>(entry, 0x15) = 1;
                return;
            }
        }
    } else if (kind == 1) {
        u3x_v_sub(visitor, x + field<uint32_t>(visitor, V_INSTANCE_BASE));
    }
}

// "copy down": slot ib + 1 to slot ib
extern "C" U3X_FN void u3x_v_sub4(void *visitor) {
    using namespace g52;
    const int32_t kind = field<int32_t>(visitor, V_KIND);
    const uint32_t ib = field<uint32_t>(visitor, V_INSTANCE_BASE);
    if (kind == 1) {
        u3x_v_copy(visitor, ib + 1, ib);
    } else if (kind == 2 && field<uint8_t>(visitor, V_ACTIVE)) {
        if (ib + 1 >= LOCATOR_COUNT) {
            u3x_v_error(visitor, ERR_SLOT);
            return;
        }
        const uint32_t *from = reinterpret_cast<const uint32_t *>(locator(visitor, ib + 1));
        uint32_t *to = reinterpret_cast<uint32_t *>(locator(visitor, ib));
        for (uint32_t i = 0; i < LOCATOR_SIZE / 4; ++i) to[i] = from[i];
    }
}

// marker anim
extern "C" U3X_FN void u3x_v_mark(void *visitor, uint32_t phase, uint8_t *anim, uint32_t depth, int32_t a7) {
    using namespace g52;
    const int32_t kind = field<int32_t>(visitor, V_KIND);
    if (kind == 0) {
        u3x_v_push(visitor, phase, anim);
        return;
    }
    if (kind != 1) {
        u3x_v_sample(visitor, phase, anim, depth);
        return;
    }
    if (depth + field<uint32_t>(visitor, V_DEPTH_BASE) > 6) { u3x_v_error(visitor, ERR_DEPTH); return; }
    if (!anim) { u3x_v_error(visitor, ERR_NO_ANIM); return; }

    // retarget the pose of the anim at the frame of the phase: phase * (frames - 1)
    uint64_t out1 = 0, out2 = 0;
    const uintptr_t base = field<uintptr_t>(visitor, V_BASE);   // read before the frames call
    const uint32_t frames = u3x_v_frames(anim);
    const uint32_t frame = f_mul(phase, f_sub(f_from_int(static_cast<int32_t>(frames)), F_ONE));
    const uint64_t ok = u3x_call_float_arg(base + 0xdc4240, frame,
                                           reinterpret_cast<uint64_t>(anim), static_cast<uint32_t>(a7),
                                           reinterpret_cast<uint64_t>(&out1), reinterpret_cast<uint64_t>(&out2));
    const uint32_t index = depth + field<uint32_t>(visitor, V_INSTANCE_BASE);
    if (ok & 0xff) {
        *diag(103) += 1;
        u3x_v_ice(visitor, 0xc, index, out1, 0, out2);
    } else {
        u3x_v_bind(visitor, index);
    }
}
