// S08 part D: UI material default texture (packet level u3x_k40_deftex, executor level u3x_k25_fill).
// Written from spec S08_structure_walks.md, part D. No float, no game value table.

namespace s08d {

// k40 cache: four slots (material, handle, texture object) and eight diagnostic words; zero at load, never cleared
struct alignas(16) Handle { uint8_t b[32]; };
struct alignas(16) TexObj { uint32_t type; uint32_t pad; uint64_t tex; };
struct K40State {
    Handle h[4];
    TexObj w[4];
    uint64_t mat[4];
    uint32_t d[8];
};
U3X_BSS K40State k40;

// the material's technique-0 texture collection (16 bytes), or null when the material's collection list is unusable
U3X_FN const uint8_t *tech0(const uint8_t *mat) {
    const int32_t nc = rd<int32_t>(mat, 0x18);
    if (nc <= 0 || nc > 64) return nullptr;
    const uint8_t *cols = rd<const uint8_t *>(mat, 0x20);
    if (!ptr_ok(cols)) return nullptr;
    for (int32_t i = 0; i < nc; ++i) {
        const uint8_t *c = cols + i * 16;
        if (rd<uint32_t>(c, 0) == 0) return c;
    }
    return nullptr;
}

// texture of a texture entry {p0, p1}: p0 when non-zero, else p1
U3X_FN uint64_t entry_tex(const uint8_t *e) {
    const uint64_t p0 = rd<uint64_t>(e, 0);
    return p0 ? p0 : rd<uint64_t>(e, 8);
}

// the game's resource writer can read t without faulting
U3X_FN bool writer_safe(uint64_t t) {
    if (t < 0x10000u || t >= (1ull << 47)) return false;
    const uint8_t *p = reinterpret_cast<const uint8_t *>(t);
    return (rd<uint32_t>(p, 0) & 3) != 0 && rd<uint64_t>(p, 8) != 0;
}

// u32 at p + off, or 0xffffffff when p fails ptr_ok
U3X_FN uint32_t word_or_ff(uint64_t p, uintptr_t off) {
    const uint8_t *q = reinterpret_cast<const uint8_t *>(p);
    return ptr_ok(q) ? rd<uint32_t>(q, off) : 0xffffffffu;
}

}  // namespace s08d

extern "C" U3X_FN uint64_t u3x_k40_tex(const uint8_t *mat) {
    const uint8_t *c = s08d::tech0(mat);
    if (!c) return 0;
    const uint8_t *list = rd<const uint8_t *>(c, 8);
    if (rd<uint32_t>(c, 4) < 2 || !ptr_ok(list)) return 0;
    const uint8_t *e = rd<const uint8_t *>(list, 8);
    if (!ptr_ok(e)) return 0;
    const uint64_t t = s08d::entry_tex(e);
    return ptr_ok(reinterpret_cast<const void *>(t)) ? t : 0;
}

extern "C" U3X_FN int u3x_k40_deftex(uint64_t *r) {
    s08d::K40State &s = s08d::k40;
    s.d[0] += 1;
    if (ext_eb()[1] & 0x40) return 0;

    const uint8_t *ctx = reinterpret_cast<const uint8_t *>(r[9]);
    const uint64_t mat = r[12];
    uint8_t *fp = reinterpret_cast<uint8_t *>(r[14]);
    if (!ptr_ok(ctx) || !ptr_ok(reinterpret_cast<const void *>(mat)) || !ptr_ok(fp)) return 0;
    if (rd<uint64_t>(ctx, 0xc8) != mat) return 0;

    uint64_t *second = reinterpret_cast<uint64_t *>(fp - 0x70);
    if (*second) { s.d[6] += 1; return 0; }

    int k = -1;
    for (int i = 0; i < 4; ++i)
        if (s.mat[i] == mat) { k = i; break; }
    if (k < 0) {
        const uint64_t t = u3x_k40_tex(reinterpret_cast<const uint8_t *>(mat));
        if (!t) { s.d[3] += 1; return 0; }
        k = 3;   // all used: slot 3 is overwritten
        for (int i = 0; i < 4; ++i)
            if (!s.mat[i]) { k = i; break; }
        s.w[k].type = 2;
        s.w[k].pad = 0;
        s.w[k].tex = t;
        *reinterpret_cast<uint64_t *>(s.h[k].b + 0) = reinterpret_cast<uint64_t>(&s.w[k]);
        s.h[k].b[0x10] = 1;
        s.mat[k] = mat;
        s.d[2] += 1;
        const uint8_t *tp = reinterpret_cast<const uint8_t *>(t);
        s.d[4] = rd<uint32_t>(tp, 0);
        s.d[5] = rd<uint32_t>(tp, 0x20);
    }
    *second = reinterpret_cast<uint64_t>(&s.h[k]);
    s.d[1] += 1;
    return 0;
}

extern "C" U3X_FN int u3x_k25_fill(uint64_t *r) {
    uint64_t *a = reinterpret_cast<uint64_t *>(reinterpret_cast<uint8_t *>(r) + 0x78 + 0xe0);
    if (r[13] != reinterpret_cast<uint64_t>(a)) return 0;
    uint32_t *eb = ext_eb();
    eb[216] += 1;

    const uint8_t *pk = reinterpret_cast<const uint8_t *>(r[2]);

    // 1. drop entries the resource writer would fault on
    for (uint32_t k = 0; k < 3; ++k) {
        if (!a[k] || s08d::writer_safe(a[k])) continue;
        eb[237] += 1;
        eb[238] = s08d::word_or_ff(a[k], 0);
        eb[239] = s08d::word_or_ff(a[k], 8);
        uint32_t v = k;
        if (ptr_ok(pk)) {
            const uint8_t *m = rd<const uint8_t *>(pk, 0);
            if (ptr_ok(m)) v += (rd<uint32_t>(m, 0) & 0xff) << 8;
        }
        eb[240] = v;
        if (ptr_ok(pk)) {
            const uint8_t *h = rd<const uint8_t *>(pk, 8 + 8 * k);
            if (ptr_ok(h)) {
                eb[241] = rd<uint32_t>(h, 0x20);
                eb[242] = rd<uint32_t>(h, 0x28);
            }
        }
        a[k] = 0;
    }

    if (a[1] && a[2]) return 0;

    // 2. the material's technique-0 texture list
    if (!ptr_ok(pk)) { eb[218] += 1; return 0; }
    const uint8_t *mat = rd<const uint8_t *>(pk, 0);
    if (!ptr_ok(mat)) { eb[218] += 1; return 0; }
    eb[219] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mat));
    const uint8_t *c = s08d::tech0(mat);
    if (!c) { eb[218] += 1; return 0; }
    const uint32_t nt = rd<uint32_t>(c, 4);
    const uint8_t *list = rd<const uint8_t *>(c, 8);
    if (nt == 0 || !ptr_ok(list)) { eb[218] += 1; return 0; }

    eb[214] = nt;
    eb[215] = (a[1] != 0 ? 1u : 0u) + 2u * (a[2] != 0 ? 1u : 0u);

    // 3. fill the missing entries 1 and 2 with the material's defaults (entry 0 is never filled)
    for (uint32_t k = 1; k <= 2 && k < nt; ++k) {
        if (a[k]) continue;
        const uint8_t *e = rd<const uint8_t *>(list, 8 * k);
        if (!ptr_ok(e)) { eb[209] += 1; continue; }
        const uint64_t p0 = rd<uint64_t>(e, 0);
        const uint64_t p1 = rd<uint64_t>(e, 8);
        const uint64_t t = p0 ? p0 : p1;
        const uint8_t *tp = reinterpret_cast<const uint8_t *>(t);
        if (!s08d::writer_safe(t)) {
            eb[5] += 1;
            eb[6] = nt | (k << 16);
            eb[13] = static_cast<uint32_t>(p0);
            eb[14] = static_cast<uint32_t>(p1);
            eb[15] = static_cast<uint32_t>(t);
            if (ptr_ok(tp)) {
                eb[7] = rd<uint32_t>(tp, 0);
                eb[8] = rd<uint32_t>(tp, 4);
                eb[9] = rd<uint32_t>(tp, 8);
                eb[11] = rd<uint32_t>(tp, 0xc);
                eb[12] = rd<uint32_t>(tp, 0x20);
            }
            continue;
        }
        if ((rd<uint32_t>(tp, 0) & 3) == 2) {
            uint8_t *m = rd<uint8_t *>(tp, 8);
            if (ptr_ok(m)) *reinterpret_cast<uint32_t *>(m + 4) &= 0xc3ffffffu;
        }
        a[k] = t;
        eb[217] += 1;
    }
    return 0;
}
