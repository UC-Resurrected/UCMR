// G49 part A: the bit writer and the maths of the animation state packer (spec G49_anim_packer.md, 2.1, 4.1 to 4.7).
// Floats are 32-bit patterns; every float step is one single-precision operation (f_* helpers).

// ---- other groups and the C part
extern "C" int u3x_bf_handle_valid(uintptr_t base, const uint8_t *bf);   // G42
extern "C" uint32_t *u3h_repl_error(void);                              // the overflow word

// ---- this part
extern "C" void u3x_bsw(uint8_t *bs, uint32_t val, uint32_t n);
extern "C" uint32_t u3x_fabs(uint32_t f);
extern "C" uint32_t u3x_ppc_ctiwz(uint32_t f);
extern "C" void u3x_bsw_vec(uint8_t *bs, const uint32_t *v);
extern "C" void u3x_bsw_frame(uintptr_t base, uint8_t *bs, uint8_t *f);
extern "C" void u3x_q_mul(uint32_t *o, const uint32_t *a, const uint32_t *b);
extern "C" void u3x_q_rot(uint32_t *o, const uint32_t *q, const uint32_t *v);
extern "C" void u3x_loc_mul(uint32_t *o, const uint32_t *a, const uint32_t *b);
extern "C" void u3x_loc_rel(uint32_t *o, const uint32_t *a, const uint32_t *b);
extern "C" void u3x_bf_world(uintptr_t base, uint8_t *f, uint32_t *o);

namespace g49a {

constexpr uint32_t SIGN = 0x80000000u;
constexpr uint32_t F_255 = 0x437f0000u;   // limit of x and z
constexpr uint32_t F_127 = 0x42fe0000u;   // limit of y
constexpr uint32_t F_512 = 0x44000000u;   // scale of x and z
constexpr uint32_t F_256 = 0x43800000u;   // scale of y

constexpr uintptr_t GAME_FRAME_WRITER = 0x7befc0;
constexpr uint32_t FRAME_ROOM = 512;      // free bits the game's frame writer must find

// bit stream fields
constexpr uintptr_t BS_WORDS = 0, BS_POS = 8, BS_CAP = 0xc;
// bound frame fields
constexpr uintptr_t BF_HANDLE = 8, BF_PARENT = 0x20, BF_LOCAL = 0x40;

struct Quat { uint32_t x, y, z, w; };

// quaternion product a * b
U3X_FN void product(Quat *o, const Quat *a, const Quat *b) {
    const Quat p = *a, q = *b;
    o->x = f_add(f_add(f_mul(p.w, q.x), f_mul(p.x, q.w)), f_sub(f_mul(p.y, q.z), f_mul(p.z, q.y)));
    o->y = f_add(f_add(f_mul(p.w, q.y), f_mul(p.y, q.w)), f_sub(f_mul(p.z, q.x), f_mul(p.x, q.z)));
    o->z = f_add(f_add(f_mul(p.w, q.z), f_mul(p.z, q.w)), f_sub(f_mul(p.x, q.y), f_mul(p.y, q.x)));
    o->w = f_sub(f_sub(f_mul(p.w, q.w), f_mul(p.x, q.x)), f_add(f_mul(p.y, q.y), f_mul(p.z, q.z)));
}

// conjugate: the sign bits of x, y and z flipped
U3X_FN void conjugate(Quat *o, const Quat *q) {
    const Quat s = *q;
    o->x = s.x ^ SIGN;
    o->y = s.y ^ SIGN;
    o->z = s.z ^ SIGN;
    o->w = s.w;
}

// the vector (v0, v1, v2) rotated by q: (q * V) * conjugate(q); the result's w is whatever the product gives
U3X_FN void rotate(Quat *o, const Quat *q, uint32_t v0, uint32_t v1, uint32_t v2) {
    const Quat rot = *q;
    const Quat vec = {v0, v1, v2, 0};
    Quat half, back;
    product(&half, &rot, &vec);
    conjugate(&back, &rot);
    product(o, &half, &back);
}

U3X_FN void load_quat(Quat *o, const uint32_t *p) {
    o->x = p[0];
    o->y = p[1];
    o->z = p[2];
    o->w = p[3];
}

// stores a locator: position, a zero pad word, rotation
U3X_FN void store_locator(uint32_t *o, uint32_t x, uint32_t y, uint32_t z, const Quat *rot) {
    const Quat r = *rot;
    o[0] = x;
    o[1] = y;
    o[2] = z;
    o[3] = 0;
    o[4] = r.x;
    o[5] = r.y;
    o[6] = r.z;
    o[7] = r.w;
}

}  // namespace g49a

// 4.1: appends the low n bits of val, most significant bit first; what follows the field in its word(s) becomes 0
extern "C" U3X_FN void u3x_bsw(uint8_t *bs, uint32_t val, uint32_t n) {
    const uint32_t pos = rd<uint32_t>(bs, g49a::BS_POS);
    if (pos + n > rd<uint32_t>(bs, g49a::BS_CAP)) {   // 32-bit sum, wraps
        *u3h_repl_error() = 1;
        return;
    }
    uint32_t *word = rd<uint32_t *>(bs, g49a::BS_WORDS) + pos / 32;
    const uint32_t used = pos % 32;                 // bits of this word that are already written
    const uint32_t room = 32 - used;                // bits of this word from the position on
    const uint32_t kept = used ? word[0] & ~(0xffffffffu >> used) : 0;
    const uint32_t field = n >= 32 ? val : val & ((1u << n) - 1);
    if (n == 0) {
        word[0] = kept;
    } else if (n <= room) {
        word[0] = kept | (field << (room - n));
    } else {
        const uint32_t rest = n - room;             // bits that go into the next word
        word[0] = kept | (field >> rest);
        word[1] = field << (32 - rest);
    }
    *reinterpret_cast<uint32_t *>(bs + g49a::BS_POS) = pos + n;
}

// 4.2
extern "C" U3X_FN uint32_t u3x_fabs(uint32_t f) {
    return f & ~g49a::SIGN;
}

// 4.2: float to signed integer, truncating, with the PowerPC results for values that do not fit
extern "C" U3X_FN uint32_t u3x_ppc_ctiwz(uint32_t f) {
    const uint32_t exponent = (f >> 23) & 0xff;
    const uint32_t fraction = f & 0x7fffffu;
    if (exponent == 0xff && fraction) return 0x80000000u;                  // not a number
    if (exponent >= 0x9e) return (f & g49a::SIGN) ? 0x80000000u : 0x7fffffffu;   // 2^31 or more
    return static_cast<uint32_t>(f_to_int(f));
}

// 4.3: a vector, as three fixed point numbers when it is small, else as three float patterns
extern "C" U3X_FN void u3x_bsw_vec(uint8_t *bs, const uint32_t *v) {
    const uint32_t x = v[0], y = v[1], z = v[2];
    const bool small = f_less(u3x_fabs(x), g49a::F_255) && f_less(u3x_fabs(y), g49a::F_127) &&
                       f_less(u3x_fabs(z), g49a::F_255);
    if (small) {
        u3x_bsw(bs, 0, 1);
        u3x_bsw(bs, u3x_ppc_ctiwz(f_mul(x, g49a::F_512)), 18);
        u3x_bsw(bs, u3x_ppc_ctiwz(f_mul(y, g49a::F_256)), 17);
        u3x_bsw(bs, u3x_ppc_ctiwz(f_mul(z, g49a::F_512)), 18);
    } else {
        u3x_bsw(bs, 1, 1);
        u3x_bsw(bs, x, 32);
        u3x_bsw(bs, y, 32);
        u3x_bsw(bs, z, 32);
    }
}

// 4.4: a bound frame through the game's own writer, when 512 bits are free
extern "C" U3X_FN void u3x_bsw_frame(uintptr_t base, uint8_t *bs, uint8_t *f) {
    uint32_t *overflow = u3h_repl_error();
    if (*overflow) return;
    if (rd<uint32_t>(bs, g49a::BS_POS) + g49a::FRAME_ROOM > rd<uint32_t>(bs, g49a::BS_CAP)) {   // 32-bit sum, wraps
        *overflow = 1;
        return;
    }
    game_call<void>(base + g49a::GAME_FRAME_WRITER, bs, f);
}

// 4.5
extern "C" U3X_FN void u3x_q_mul(uint32_t *o, const uint32_t *a, const uint32_t *b) {
    g49a::Quat qa, qb, r;
    g49a::load_quat(&qa, a);
    g49a::load_quat(&qb, b);
    g49a::product(&r, &qa, &qb);
    o[0] = r.x;
    o[1] = r.y;
    o[2] = r.z;
    o[3] = r.w;
}

// 4.5: the rotated vector, fourth word 0
extern "C" U3X_FN void u3x_q_rot(uint32_t *o, const uint32_t *q, const uint32_t *v) {
    g49a::Quat rot, r;
    g49a::load_quat(&rot, q);
    g49a::rotate(&r, &rot, v[0], v[1], v[2]);
    o[0] = r.x;
    o[1] = r.y;
    o[2] = r.z;
    o[3] = 0;
}

// 4.6: locator a, then b inside a
extern "C" U3X_FN void u3x_loc_mul(uint32_t *o, const uint32_t *a, const uint32_t *b) {
    const uint32_t ax = a[0], ay = a[1], az = a[2];
    const uint32_t bx = b[0], by = b[1], bz = b[2];
    g49a::Quat a_rot, b_rot, moved, rot;
    g49a::load_quat(&a_rot, a + 4);
    g49a::load_quat(&b_rot, b + 4);
    g49a::rotate(&moved, &a_rot, bx, by, bz);
    g49a::product(&rot, &a_rot, &b_rot);
    g49a::store_locator(o, f_add(ax, moved.x), f_add(ay, moved.y), f_add(az, moved.z), &rot);
}

// 4.6: locator b as seen from a
extern "C" U3X_FN void u3x_loc_rel(uint32_t *o, const uint32_t *a, const uint32_t *b) {
    const uint32_t dx = f_sub(b[0], a[0]), dy = f_sub(b[1], a[1]), dz = f_sub(b[2], a[2]);
    g49a::Quat a_rot, b_rot, inverse, pos, rot;
    g49a::load_quat(&a_rot, a + 4);
    g49a::load_quat(&b_rot, b + 4);
    g49a::conjugate(&inverse, &a_rot);
    g49a::rotate(&pos, &inverse, dx, dy, dz);
    g49a::product(&rot, &inverse, &b_rot);
    g49a::store_locator(o, pos.x, pos.y, pos.z, &rot);
}

// 4.7: world locator of a bound frame: its body's locator (or the last known parent locator), then its local one
extern "C" U3X_FN void u3x_bf_world(uintptr_t base, uint8_t *f, uint32_t *o) {
    const uint8_t *parent = f + g49a::BF_PARENT;
    if (u3x_bf_handle_valid(base, f)) {
        const uint8_t *body = rd<const uint8_t *>(rd<const uint8_t *>(f, g49a::BF_HANDLE), 0);
        if (body) parent = body;
    }
    u3x_loc_mul(o, reinterpret_cast<const uint32_t *>(parent), reinterpret_cast<const uint32_t *>(f + g49a::BF_LOCAL));
}
