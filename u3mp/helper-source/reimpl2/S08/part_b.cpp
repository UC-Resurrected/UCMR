// S08 part B: solo co-op / store-unlock switches and the church-key diagnostic (spec S08_structure_walks.md, Part B).

// ---- called from other parts (prototypes as the spec gives them)
extern "C" uint8_t *u3h_nmh(uintptr_t base);                          // custom-game menu table (C part)
extern "C" uint32_t u3h_crc_sid(const char *name);                    // string id (C part)
extern "C" int u3x_rp_isa(uintptr_t base, uint8_t *obj, uint32_t hash);  // class test (film helpers)

// ---- this part's exports
extern "C" void u3x_solo_patch_tick(uintptr_t base, const uint8_t *s, Log log);
extern "C" void u3x_mk02_tick(uintptr_t base, Log log);
extern "C" uint64_t u3x_mk03_process(uintptr_t base, uint32_t name);
extern "C" uint32_t u3x_mk03_edge(uintptr_t base, uint64_t p, uint32_t *owner, uint32_t *edge, uint32_t *handle,
                                  uint32_t *up);

namespace s08b {

// game image offsets
constexpr uintptr_t DEBUG_OPTIONS = 0x17d7a50 + 0x7d;  // u8, PS3 keeps it 1
constexpr uintptr_t STORE_FLAG = 0x17b2a94;            // u8, store "all items unlocked"
constexpr uintptr_t MP_ACTIVE = 0x17d5c61;             // u8, non-zero while a net game runs
constexpr uintptr_t LOCAL_NET_PLAYER = 0x17cb3d0;      // pointer to record T
constexpr uintptr_t PROCESS_TABLE = 0x1b37610;
constexpr uintptr_t EDGE_OWNER_FN = 0x1047c90;         // uint64_t fn(void *Rf)
constexpr uintptr_t EDGE_TEST_FN = 0x1047d60;          // uint64_t fn(void *Rf, const void *dir, const void *limit)

// switch bits at s + 0x3d78
constexpr uint32_t SW_SOLO_COOP = 0x80000;
constexpr uint32_t SW_STORE_UNLOCK = 0x100000;
constexpr uint32_t SW_NO_MAP = 0x200000;

constexpr uint32_t LINE_LIMIT = 120;
constexpr uint32_t ARM_FRAMES = 240;

// names whose string ids are used
U3X_RO const char name_drake[] = {'c','o','o','p','-','d','r','a','k','e',0};
U3X_RO const char name_chloe[] = {'c','o','o','p','-','c','h','l','o','e',0};
U3X_RO const char name_sully[] = {'c','o','o','p','-','s','u','l','l','i','v','a','n',0};
U3X_RO const char name_player[] = {'P','l','a','y','e','r',0};

// edge test vectors (32-bit float patterns): dir (0, 1, 0, 0), limits 0.707 and -2.0
U3X_RO alignas(16) const uint32_t vec_dir[4] = {0, 0x3f800000u, 0, 0};
U3X_RO alignas(16) const uint32_t vec_l1[4] = {0x3f34fdf4u, 0x3f34fdf4u, 0x3f34fdf4u, 0x3f34fdf4u};
U3X_RO alignas(16) const uint32_t vec_l2[4] = {0xc0000000u, 0xc0000000u, 0xc0000000u, 0xc0000000u};

// log texts
U3X_RO const char t_l1a[] = {'U','3','M','P',' ','m','k','0','2',':',' ','l','o','c','a','l',' ','p','l','a','y','e','r',
                             ' ','n','a','m','e',' ',0};
U3X_RO const char t_l1b[] = {' ','c','o','o','p','-','i','d','x',' ',0};
U3X_RO const char t_l1c[] = {' ','t','e','a','m',' ',0};
U3X_RO const char t_l2a[] = {'U','3','M','P',' ','m','k','0','3',' ','f',0};
U3X_RO const char t_l2st[] = {' ','s','t',' ',0};
U3X_RO const char t_l2obj[] = {' ','o','b','j',' ',0};
U3X_RO const char t_l2e[] = {' ','e',' ',0};
U3X_RO const char t_l2h[] = {' ','h',' ',0};
U3X_RO const char t_l2up[] = {' ','u','p',' ',0};
U3X_RO const char t_l2coop[] = {' ','c','o','o','p',' ',0};
U3X_RO const char t_minus[] = {'-',0};

// the diagnostic's own state (zero at load)
struct Diag {
    uint32_t lines;                     // lines printed N
    uint32_t name, index, team;         // remembered local player name id, co-op index, team
    uint32_t owner, edge, handle, up;   // remembered record K
    uint32_t digits, state, stamp;
};
U3X_BSS Diag diag;

// sc[0..7] = u32 words at X + 0x2ff0
inline uint32_t *sc() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0x2ff0); }

// the "sg test": above 0x100000, below 0x7fffffffffff, multiple of 8
[[gnu::always_inline]] inline bool sg_ok(uint64_t v) { return v > 0x100000u && v < 0x7fffffffffffull && !(v & 7); }

// a handle (slot pointer + id) resolved to its object, or 0
U3X_FN uint64_t resolve(uint64_t slot, uint32_t id) {
    if (!sg_ok(slot) || id == 0) return 0;
    const uint64_t obj = rd<uint64_t>(reinterpret_cast<const void *>(slot), 0);
    if (!sg_ok(obj)) return 0;
    if (rd<uint32_t>(reinterpret_cast<const void *>(obj), 0x7c) != id) return 0;
    return obj;
}

// 32-bit FNV-1a over the four bytes of v, low byte first
[[gnu::always_inline]] inline uint32_t fnv1a(uint32_t v) {
    uint32_t h = 0x811c9dc5u;
    for (int i = 0; i < 4; ++i) {
        h ^= (v >> (8 * i)) & 0xff;
        h *= 0x01000193u;
    }
    return h;
}

[[gnu::always_inline]] inline void dec(Line &l, uint32_t v) {   // signed decimal
    if (static_cast<int32_t>(v) < 0) {
        l.s(t_minus);
        v = 0u - v;
    }
    l.d(v);
}

[[gnu::always_inline]] inline void digit(Line &l, uint32_t v) {
    if (l.n < 190) l.b[l.n++] = static_cast<char>('0' + v);
}

// one co-op hero's digit (0..4), see spec B.4.4 step 5
U3X_FN uint32_t hero_digit(uintptr_t base, const char *name, uint32_t player_class, uint64_t p, uint32_t res,
                           uint32_t eo) {
    const uint64_t q = u3x_mk03_process(base, u3h_crc_sid(name));
    if (q == 0) return 0;
    if (u3x_rp_isa(base, reinterpret_cast<uint8_t *>(q), player_class) == 0) return 1;
    uint32_t r = res;
    if (q != p) {
        uint32_t o, e, h, u;
        r = u3x_mk03_edge(base, q, &o, &e, &h, &u);
    }
    if (r == 0) return 2;
    if (eo != 0 && r == eo) return 3;
    return 4;
}

}  // namespace s08b

extern "C" U3X_FN void u3x_solo_patch_tick(uintptr_t base, const uint8_t *s, Log log) {
    using namespace s08b;
    uint32_t *w = sc();
    const uint32_t m = rd<uint32_t>(s, 0x3d78);
    if (m & SW_SOLO_COOP) {
        w[0] += 1;
        u3x_mk02_tick(base, log);
        if (at<uint8_t>(base + DEBUG_OPTIONS) == 0) {
            at<uint8_t>(base + DEBUG_OPTIONS) = 1;
            w[6] += 1;
        }
        if (!(m & SW_NO_MAP)) {
            uint8_t *t = u3h_nmh(base);
            if (w[7] == 0) {
                w[7] = 1;   // set even when the table is already built: the preset is tried once per load
                if (t[0] == 0) {
                    for (int i = 0; i < 11; ++i) {
                        t[0xc + i * 0x308] = 1;   // mode count of game i
                        w[2] += 1;
                    }
                }
            }
        }
    }
    if ((m & SW_STORE_UNLOCK) && at<uint8_t>(base + STORE_FLAG) == 0) {
        w[5] = 0;   // the value seen
        at<uint8_t>(base + STORE_FLAG) = 1;
        w[4] += 1;
    }
}

extern "C" U3X_FN uint64_t u3x_mk03_process(uintptr_t base, uint32_t name) {
    using namespace s08b;
    const uintptr_t tab = base + PROCESS_TABLE;
    const int32_t c = rd<int32_t>(reinterpret_cast<const void *>(tab), 0x5c);
    const uint64_t buckets = rd<uint64_t>(reinterpret_cast<const void *>(tab), 0x20);
    if (c <= 0 || c > 0x100000 || !sg_ok(buckets)) return 0;
    const uint64_t *bucket = reinterpret_cast<const uint64_t *>(buckets);
    const uint64_t end = bucket[c];
    uint64_t e = bucket[fnv1a(name) % static_cast<uint32_t>(c)];
    for (int i = 0; i < 256 && sg_ok(e) && e != end; ++i) {
        const void *ent = reinterpret_cast<const void *>(e);
        if (rd<uint32_t>(ent, 0x10) == name) return resolve(rd<uint64_t>(ent, 0x18), rd<uint32_t>(ent, 0x20));
        e = rd<uint64_t>(ent, 0);
    }
    return 0;
}

extern "C" U3X_FN uint32_t u3x_mk03_edge(uintptr_t base, uint64_t p, uint32_t *owner, uint32_t *edge, uint32_t *handle,
                                         uint32_t *up) {
    using namespace s08b;
    *owner = 0;
    *edge = 0;
    *handle = 0;
    *up = 0;
    const uint64_t e = rd<uint64_t>(reinterpret_cast<const void *>(p), 0x538);
    if (!sg_ok(e)) return 0;
    void *rf = reinterpret_cast<void *>(e + 0x128);
    *edge = static_cast<uint32_t>(rd<uint64_t>(rf, 0));
    *handle = rd<uint32_t>(rf, 0x10);
    const uint64_t o = game_call<uint64_t>(base + EDGE_OWNER_FN, rf);
    if (!sg_ok(o)) return 0;
    *owner = rd<uint32_t>(reinterpret_cast<const void *>(o), 0x98);
    const bool u = (game_call<uint64_t>(base + EDGE_TEST_FN, rf, static_cast<const void *>(vec_dir),
                                        static_cast<const void *>(vec_l1)) & 0xff) != 0;
    const bool f = (game_call<uint64_t>(base + EDGE_TEST_FN, rf, static_cast<const void *>(vec_dir),
                                        static_cast<const void *>(vec_l2)) & 0xff) != 0;
    *up = 2u * u + f;
    return u ? *owner : 0;
}

extern "C" U3X_FN void u3x_mk02_tick(uintptr_t base, Log log) {
    using namespace s08b;
    Diag &k = diag;
    if (k.lines >= LINE_LIMIT || at<uint8_t>(base + MP_ACTIVE) == 0) return;
    const uint64_t t = at<uint64_t>(base + LOCAL_NET_PLAYER);
    if (!sg_ok(t)) return;
    const void *rec = reinterpret_cast<const void *>(t);
    const uint64_t p = resolve(rd<uint64_t>(rec, 0x10), rd<uint32_t>(rec, 0x18));
    if (p == 0) return;
    const void *pp = reinterpret_cast<const void *>(p);

    // 1: name, co-op index and team, printed on change
    const uint32_t nm = rd<uint32_t>(pp, 0x98);
    const uint32_t ci = rd<uint32_t>(rec, 0x42c);
    const uint32_t tm = rd<uint32_t>(rec, 0x21c);
    if (nm != k.name || ci != k.index || tm != k.team) {
        k.name = nm;
        k.index = ci;
        k.team = tm;
        Line l;
        l.s(t_l1a).x(nm, 8).s(t_l1b);
        dec(l, ci);
        l.s(t_l1c);
        dec(l, tm);
        log(base, l.z());
        k.lines += 1;
    }

    // 2: frame, state name, edge test of the local player and the three heroes
    const uint32_t fr = sc()[0];
    uint32_t st = 0;
    const uint64_t mach = rd<uint64_t>(pp, 0x80);
    if (sg_ok(mach)) {
        const uint64_t cur = rd<uint64_t>(reinterpret_cast<const void *>(mach), 8);
        if (sg_ok(cur)) st = rd<uint32_t>(reinterpret_cast<const void *>(cur), 0x18);
    }
    uint32_t eo, ed, eh, uf;
    const uint32_t res = u3x_mk03_edge(base, p, &eo, &ed, &eh, &uf);
    const uint32_t player = u3h_crc_sid(name_player);
    const uint32_t d0 = hero_digit(base, name_drake, player, p, res, eo);
    const uint32_t d1 = hero_digit(base, name_chloe, player, p, res, eo);
    const uint32_t d2 = hero_digit(base, name_sully, player, p, res, eo);
    const uint32_t rc = d0 * 100 + d1 * 10 + d2;

    if (eo != 0) k.stamp = fr;
    const bool armed = eo != 0 || (k.stamp != 0 && fr - k.stamp < ARM_FRAMES);   // stamp 0 = never
    const bool changed = eo != k.owner || ed != k.edge || eh != k.handle || uf != k.up || st != k.state;
    const bool digits_changed = rc != k.digits;
    k.owner = eo;
    k.edge = ed;
    k.handle = eh;
    k.up = uf;
    k.state = st;
    k.digits = rc;
    if (!(digits_changed || (armed && changed))) return;

    Line l;
    l.s(t_l2a);
    dec(l, fr);
    l.s(t_l2st).x(st, 8).s(t_l2obj).x(eo, 8).s(t_l2e).x(ed, 8).s(t_l2h).x(eh, 8).s(t_l2up);
    digit(l, (uf >> 1) & 1);
    digit(l, uf & 1);
    l.s(t_l2coop);
    digit(l, d0);
    digit(l, d1);
    digit(l, d2);
    log(base, l.z());
    k.lines += 1;
}
