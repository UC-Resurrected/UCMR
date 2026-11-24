// wall-emblem/make_emblem.cpp: the wall emblems of the Uncharted 3 multiplayer levels, part of the level conversion
// (interface and overview: wall_emblem.hpp). Replaces the former recipe steps wall-emblem, wall-emblem-actors and
// emblem-cap3 (and before them mkemblem_v2.py / mkemblem.py, prep_p3.py, apply_air_emblem.py, install_emblem_actors.py).
//
// A patch = one PS3 instance of an emblem material group, reduced to the triangles that carry the emblem
// (bg::emblem_triangles; the stage converter keeps the others in the level), moved to world space by the instance
// matrix (normals / tangents by the matrix scaled to determinant 1) and clipped along uv1 = 0 and 1 (pieces outside
// get uv1 (0, 0), so nothing relies on the overlay sampler's wrap mode). Its actor pak is built on the retail
// loading-menu-cloth effects (its first two materials, the cloth's per-instance overlay) and the 1-joint
// NdSimpleObject skeleton of cargo-plane-int-moving-multi: a single prototype, positions centred, a shared map with a
// flat normal and blue 166, white overlay and transparency, the cbuffer capacity 3, and the wall's look baked into two
// textures (the wall material's two colour layers with tint and brightness, blended by the PS3 vertex blend value and
// the blend map): Color1Map in the patch's uv0 box, Overlay4Map (the cloth's layer 4 base) in the emblem square.
#include "ndpak.hpp"
#include "ps3bg.hpp"
#include "ps4actor.hpp"
#include "ps4ingame.hpp"
#include "ps4writer.hpp"
#include "psarc.hpp"
#include "texcodec.hpp"
#include "u3fix.hpp"
#include "wall_emblem.hpp"

#include <array>
#include <cmath>
#include <cstring>

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace u3 {

namespace {

using namespace u3::actor;
using BlobP = std::shared_ptr<Blob>;
using ItemP = std::shared_ptr<PakWItem>;

// ---- the PS3 patches -------------------------------------------------------------------------------------------
// vertex data of a patch, float64 as decoded; uv0 / uv1 2 columns, nrm 3, tan 4
struct Verts {
    std::vector<std::array<double, 3>> pos, nrm;
    std::vector<std::array<double, 2>> uv0, uv1;
    std::vector<std::array<double, 4>> tan;
    std::vector<std::array<double, 1>> blend;   // the PS3 vertex blend value (attribute 0x15 first channel, -1..1; 1 without)
    size_t size() const { return pos.size(); }
    void push_from(const Verts &s, size_t i) {
        pos.push_back(s.pos[i]), nrm.push_back(s.nrm[i]), uv0.push_back(s.uv0[i]), uv1.push_back(s.uv1[i]), tan.push_back(s.tan[i]); blend.push_back(s.blend[i]);
    }
};
using Tri = std::array<uint32_t, 3>;

struct Patch {
    size_t proto, inst;
    std::string mat;
    Verts v;
    std::vector<Tri> tris;
    size_t nout = 0;
    bool blackboard = false;
};

// a*b+c rounded once (the BLAS kernels the reference used multiply-add in one step)
double fma_(double a, double b, double c) { return std::fma(a, b, c); }

// |det| of a 3x3 matrix as numpy gives it: LU with partial pivoting, then exp(sum of log |pivot|)
double abs_det3(double a[3][3]) {
    double logdet = 0;
    for (int c = 0; c < 3; c++) {
        int p = c;
        for (int r = c + 1; r < 3; r++)
            if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
        if (p != c) std::swap(a[p], a[c]);
        if (a[c][c] == 0) return 0;
        const double inv = 1.0 / a[c][c];
        for (int r = c + 1; r < 3; r++) {
            a[r][c] *= inv;
            for (int k = c + 1; k < 3; k++) a[r][k] = fma_(-a[r][c], a[c][k], a[r][k]);
        }
        logdet += std::log(std::fabs(a[c][c]));
    }
    return std::exp(logdet);
}

// a PS3 background block (bggeom.decode_block without instance data): positions, normals, tangents, uv0, uv1, triangles
struct BgBlock {
    uint32_t nv = 0;
    std::map<int, Mat> a;   // by attribute id, the last of an id wins
    std::vector<Tri> tris;
};
BgBlock decode_bg_block(const Pak &p, const Ref &blk) {
    const auto [pg, bo] = blk;
    BgBlock r;
    const Ref cnt = bg::need(p, pg, bo + 0x24);
    r.nv = p.pu32(cnt.first, cnt.second);
    const Ref f = bg::need(p, pg, bo + 0x28), s = bg::need(p, pg, bo);
    const std::vector<bg::Attr> attrs = bg::read_format(p, f.first, f.second);
    for (size_t ai = 0; ai < attrs.size(); ai++) {
        const bg::Attr &at = attrs[ai];
        if (at.id == 0x4c || at.id == 0x4e) continue;   // per-instance data
        const ORef rr = bg::res(p, s.first, s.second + 8 * ai);
        int sw = 0;
        for (int c = 0; c < at.n(); c++) sw += at.w[c];
        if (sw && !rr) throw Stop("ValueError: missing stream");
        r.a[at.id] = bg::decode_attr(p, f.first, f.second, at, rr ? rr->first : 0, rr ? rr->second : 0, r.nv);
    }
    const Ref ii = bg::need(p, pg, bo + 4);
    const uint64_t ib = p.pbase(ii.first) + ii.second;
    const uint32_t ntris = bg::be16(p, ib), isz = bg::be16(p, ib + 4);
    const Ref idat = bg::need(p, ii.first, ii.second + 8);
    const uint64_t bi = p.pbase(idat.first) + idat.second;
    if (bi + isz > p.d.size()) throw Stop("struct.error: index data past the end");
    r.tris = bg::decode_indices(p.d.data() + bi, isz, ntris);
    return r;
}
const Mat &attr(const BgBlock &b, int id, const char *name) {
    auto it = b.a.find(id);
    if (it == b.a.end()) throw Stop(std::string("KeyError: '") + name + "'");
    return it->second;
}

// split on uv1 = 0 / 1; pieces outside get their own vertices with uv1 (0, 0)
void clip(Verts &V, std::vector<Tri> &tris, size_t &nout) {
    auto lerp = [&](uint32_t a, uint32_t b, double t) {
        auto mix = [&](auto &vec) {
            auto x = vec[a], y = vec[b];
            for (size_t k = 0; k < x.size(); k++) {
                const double p = x[k] * (1 - t), q = y[k] * t;
                x[k] = p + q;
            }
            vec.push_back(x);
        };
        mix(V.pos), mix(V.uv0), mix(V.uv1), mix(V.nrm), mix(V.tan), mix(V.blend);
        return uint32_t(V.size() - 1);
    };
    std::vector<std::pair<Tri, bool>> cur;
    for (const Tri &t : tris) cur.push_back({t, true});
    const struct {
        int ax;
        double lim;
        int keep;
    } planes[] = {{0, 0.0, 1}, {0, 1.0, -1}, {1, 0.0, 1}, {1, 1.0, -1}};
    for (const auto &pl : planes) {
        std::vector<std::pair<Tri, bool>> nxt;
        for (const auto &[t, ins] : cur) {
            if (!ins) {
                nxt.push_back({t, false});
                continue;
            }
            double d[3];
            for (int j = 0; j < 3; j++) d[j] = pl.keep * (V.uv1[t[size_t(j)]][size_t(pl.ax)] - pl.lim);
            if (std::min({d[0], d[1], d[2]}) >= -1e-5) {
                nxt.push_back({t, true});
                continue;
            }
            if (std::max({d[0], d[1], d[2]}) <= 1e-5) {
                nxt.push_back({t, false});
                continue;
            }
            std::vector<uint32_t> pi, po;
            for (int j = 0; j < 3; j++) {
                const uint32_t a = t[size_t(j)], b = t[size_t((j + 1) % 3)];
                const double da = d[j], db = d[(j + 1) % 3];
                (da >= 0 ? pi : po).push_back(a);
                if ((da >= 0) != (db >= 0)) {
                    const uint32_t m = lerp(a, b, da / (da - db));
                    pi.push_back(m), po.push_back(m);
                }
            }
            for (int side = 0; side < 2; side++) {
                const auto &L = side == 0 ? pi : po;
                for (size_t k = 1; k + 1 < L.size(); k++) nxt.push_back({Tri{L[0], L[k], L[k + 1]}, side == 0});
            }
        }
        cur = std::move(nxt);
    }
    tris.clear();
    nout = 0;
    for (const auto &[t, ins] : cur) {
        if (ins) {
            tris.push_back(t);
            continue;
        }
        Tri nt;
        for (int j = 0; j < 3; j++) {
            V.push_from(V, t[size_t(j)]);
            V.uv1.back() = {0.0, 0.0};
            nt[size_t(j)] = uint32_t(V.size() - 1);
        }
        tris.push_back(nt);
        nout++;
    }
}

std::vector<Patch> patches(const Pak &P3) {
    std::vector<Patch> res;
    const std::vector<bg::Proto> col = bg::collection(P3);
    for (size_t pi = 0; pi < col.size(); pi++) {
        const bg::Proto &c = col[pi];
        for (const bg::Group &g : c.groups) {
            if (g.mat.find("emblem") == std::string::npos) continue;
            for (size_t ii = 0; ii < c.insts.size(); ii++) {
                const bg::Instance I = bg::instance(P3, bg::need(c.insts[ii]));
                double M[4][3], R[3][3], Rd[3][3];
                for (int r = 0; r < 4; r++)
                    for (int k = 0; k < 3; k++) M[r][k] = I.mat[r * 4 + k];
                for (int r = 0; r < 3; r++)
                    for (int k = 0; k < 3; k++) R[r][k] = Rd[r][k] = I.mat[r * 4 + k];
                const double cb = std::cbrt(abs_det3(Rd));
                double Rn[3][3];
                for (int r = 0; r < 3; r++)
                    for (int k = 0; k < 3; k++) Rn[r][k] = R[r][k] / cb;
                Patch pt;
                pt.proto = pi, pt.inst = ii, pt.mat = g.mat, pt.blackboard = g.mat.find("blackboard") != std::string::npos;
                uint32_t base = 0;
                for (uint32_t k = g.first; k < g.first + g.count; k++) {
                    const BgBlock d = decode_bg_block(P3, c.blocks.at(k));
                    const Mat &pos = attr(d, 1, "pos"), &nrm = attr(d, 2, "normal"), &tan = attr(d, 3, "tangent");
                    // the emblem UVs: the second set (0x12); a block without it carries them as 0x14 (Shambhala / Highrise plaster walls)
                    const Mat &uv0 = attr(d, 0x11, "uv0"), &uv1 = d.a.count(0x12) ? attr(d, 0x12, "uv1") : attr(d, 0x14, "uv1");
                    const std::vector<bool> em = bg::emblem_triangles(P3, c.blocks.at(k));   // the emblem area only (the stage converter keeps the rest)
                    for (uint32_t i = 0; i < d.nv; i++) {
                        const double ph[4] = {pos.at(i, 0), pos.at(i, 1), pos.at(i, 2), 1.0};
                        std::array<double, 3> w{};
                        for (size_t j = 0; j < 3; j++) {   // [x y z 1] @ M, the 4th column unused
                            double acc = ph[0] * M[0][j];
                            for (int q = 1; q < 4; q++) acc = fma_(ph[q], M[q][j], acc);
                            w[j] = acc;
                        }
                        auto rot = [&](const Mat &m) {
                            std::array<double, 3> o{};
                            for (size_t j = 0; j < 3; j++) {
                                double acc = m.at(i, 0) * Rn[0][j];
                                for (int q = 1; q < 3; q++) acc = fma_(m.at(i, size_t(q)), Rn[q][j], acc);
                                o[j] = acc;
                            }
                            return o;
                        };
                        pt.v.pos.push_back(w);
                        pt.v.nrm.push_back(rot(nrm));
                        const auto t3 = rot(tan);
                        pt.v.tan.push_back({t3[0], t3[1], t3[2], tan.cols > 3 ? tan.at(i, 3) : 1.0});
                        pt.v.uv0.push_back({uv0.at(i, 0), uv0.at(i, 1)});
                        pt.v.uv1.push_back({uv1.at(i, 0), uv1.at(i, 1)});
                        pt.v.blend.push_back({d.a.count(0x15) ? d.a.at(0x15).at(i, 0) : 1.0});
                    }
                    for (size_t t = 0; t < d.tris.size(); t++)
                        if (t < em.size() && em[t]) pt.tris.push_back({d.tris[t][0] + base, d.tris[t][1] + base, d.tris[t][2] + base});
                    base += d.nv;
                }
                if (pt.tris.empty()) continue;   // no triangle of this instance carries the emblem
                {   // only the vertices of the emblem triangles, in their first-use order
                    std::vector<int64_t> to(pt.v.size(), -1);
                    Verts v2;
                    for (Tri &t : pt.tris)
                        for (uint32_t &x : t) {
                            if (to[x] < 0) {
                                to[x] = int64_t(v2.size());
                                v2.push_from(pt.v, x);
                            }
                            x = uint32_t(to[x]);
                        }
                    pt.v = std::move(v2);
                }
                clip(pt.v, pt.tris, pt.nout);
                res.push_back(std::move(pt));
            }
        }
    }
    return res;
}

// ---- the actor pak -------------------------------------------------------------------------------------------
struct Tex {
    uint32_t hash = 0, fmt = 0;
    std::string shrt, full;
    ItemP desc, name;
};
using TexP = std::shared_ptr<Tex>;

std::string bindfull(const Tex &e) { return e.full.size() >= 4 && e.full.compare(e.full.size() - 4, 4, ".ndb") == 0 ? e.full.substr(0, e.full.size() - 4) : e.full; }

struct Inputs {
    Pak T, SK, MAP;
    std::vector<TMat> tm;                     // loading-menu-cloth's materials
    std::map<std::string, TMat> mapm;         // air-multi's materials by name before ','
    std::map<std::string, PakTexture> maptx;  // air-multi's textures by path
    uint32_t cb3 = 8, cb4 = 1;
};

struct Built {
    std::string art, color;
    Bytes pak;
    std::array<double, 3> center;
    size_t verts, tris, nout;
};

// ---- the wall's look baked into the actor's own textures ---------------------------------------------------------
// The actor cannot use the wall's material (only the cloth effect draws a per-instance emblem), so the wall's albedo
// at the patch is baked: the wall material's two colour layers (PS4 converted colour maps, each with its tint and
// brightness), blended by the PS3 vertex blend value (attribute 0x15: -1 = the second layer, 1 = the first) moved by
// the blend map. A layer: a decoded colour map (or none) and its multiplier.
struct BakeLayer {
    std::optional<tc::Img> img;
    std::array<double, 3> mul = {1, 1, 1};
};
// sample an image at (u, v) with wrap, bilinear
std::array<double, 3> sample(const tc::Img &im, double u, double v) {
    const double x = (u - std::floor(u)) * im.w - 0.5, y = (v - std::floor(v)) * im.h - 0.5;
    const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    const double fx = x - x0, fy = y - y0;
    std::array<double, 3> o{};
    for (int dy = 0; dy < 2; dy++)
        for (int dx = 0; dx < 2; dx++) {
            const uint32_t xx = uint32_t(((x0 + dx) % int(im.w) + int(im.w)) % int(im.w)), yy = uint32_t(((y0 + dy) % int(im.h) + int(im.h)) % int(im.h));
            const double wgt = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);
            const uint8_t *p = im.at(xx, yy);
            for (int c = 0; c < 3; c++) o[size_t(c)] += wgt * p[c];
        }
    return o;
}
// rasterize the patch's triangles into a w x h image in the coordinates crd(i) (0..1); per texel the wall albedo at
// the interpolated uv0 and blend value; empty texels are filled from their neighbours
tc::Img bake(const Patch &pt, const std::vector<Tri> &tris, const std::function<std::array<double, 2>(uint32_t)> &crd, uint32_t w, uint32_t h,
             const BakeLayer &l1, const BakeLayer &l2, const std::optional<tc::Img> &blendmap) {
    tc::Img im;
    im.w = w, im.h = h;
    im.px.assign(size_t(w) * h * 4, 0);
    std::vector<uint8_t> set(size_t(w) * h, 0);
    for (const Tri &t : tris) {
        std::array<double, 2> q[3];
        for (int k = 0; k < 3; k++) q[k] = crd(t[size_t(k)]), q[k][0] *= w, q[k][1] *= h;
        const double den = (q[1][1] - q[2][1]) * (q[0][0] - q[2][0]) + (q[2][0] - q[1][0]) * (q[0][1] - q[2][1]);
        if (std::fabs(den) < 1e-12) continue;
        const int x0 = std::max(0, int(std::floor(std::min({q[0][0], q[1][0], q[2][0]})))), x1 = std::min(int(w) - 1, int(std::ceil(std::max({q[0][0], q[1][0], q[2][0]}))));
        const int y0 = std::max(0, int(std::floor(std::min({q[0][1], q[1][1], q[2][1]})))), y1 = std::min(int(h) - 1, int(std::ceil(std::max({q[0][1], q[1][1], q[2][1]}))));
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                const double px = x + 0.5, py = y + 0.5;
                const double a = ((q[1][1] - q[2][1]) * (px - q[2][0]) + (q[2][0] - q[1][0]) * (py - q[2][1])) / den;
                const double b = ((q[2][1] - q[0][1]) * (px - q[2][0]) + (q[0][0] - q[2][0]) * (py - q[2][1])) / den;
                const double c = 1 - a - b;
                const double e = -1e-3;
                if (a < e || b < e || c < e) continue;
                const double bw[3] = {a, b, c};
                double u = 0, v = 0, bl = 0;
                for (int k = 0; k < 3; k++) {
                    u += bw[k] * pt.v.uv0[t[size_t(k)]][0], v += bw[k] * pt.v.uv0[t[size_t(k)]][1];
                    bl += bw[k] * pt.v.blend[t[size_t(k)]][0];
                }
                double f = std::clamp((bl + 1) / 2, 0.0, 1.0);   // weight of the first layer
                if (blendmap) f = std::clamp(f + (sample(*blendmap, u, v)[0] / 255.0 - 0.5) * 0.5, 0.0, 1.0);
                std::array<double, 3> c1{}, c2{};
                if (l1.img) c1 = sample(*l1.img, u, v);
                c2 = l2.img ? sample(*l2.img, u, v) : c1;
                if (!l2.img) f = 1;
                uint8_t *o = im.at(uint32_t(x), uint32_t(y));
                for (int ch = 0; ch < 3; ch++) {
                    const double x1c = c1[size_t(ch)] * l1.mul[size_t(ch)], x2c = c2[size_t(ch)] * l2.mul[size_t(ch)];
                    o[ch] = uint8_t(std::clamp(f * x1c + (1 - f) * x2c, 0.0, 255.0) + 0.5);
                }
                o[3] = 255;
                set[size_t(y) * w + size_t(x)] = 1;
            }
    }
    // fill the empty texels from their set neighbours (a few rings), then the rest with the mean
    double mean[3] = {0, 0, 0};
    size_t n = 0;
    for (size_t i = 0; i < set.size(); i++)
        if (set[i]) {
            for (int c = 0; c < 3; c++) mean[c] += im.px[i * 4 + size_t(c)];
            n++;
        }
    for (int c = 0; c < 3; c++) mean[c] = n ? mean[c] / double(n) : 128;
    for (int ring = 0; ring < 8; ring++) {
        std::vector<uint8_t> nset = set;
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                if (set[size_t(y) * w + x]) continue;
                double acc[3] = {0, 0, 0};
                int k = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        const int xx = int(x) + dx, yy = int(y) + dy;
                        if (xx < 0 || yy < 0 || xx >= int(w) || yy >= int(h) || !set[size_t(yy) * w + size_t(xx)]) continue;
                        const uint8_t *p = im.at(uint32_t(xx), uint32_t(yy));
                        for (int c = 0; c < 3; c++) acc[c] += p[c];
                        k++;
                    }
                if (!k) continue;
                uint8_t *o = im.at(x, y);
                for (int c = 0; c < 3; c++) o[c] = uint8_t(acc[c] / k + 0.5);
                o[3] = 255;
                nset[size_t(y) * w + x] = 1;
            }
        set = std::move(nset);
    }
    for (size_t i = 0; i < set.size(); i++)
        if (!set[i]) {
            for (int c = 0; c < 3; c++) im.px[i * 4 + size_t(c)] = uint8_t(mean[c] + 0.5);
            im.px[i * 4 + 3] = 255;
        }
    return im;
}
// PS3-linear BC3 blocks of the image and its box-filtered mips down to 1 x 1, and the mip count
std::pair<Bytes, uint32_t> bc3_mips(tc::Img im) {
    Bytes out;
    uint32_t mips = 0;
    while (true) {
        const Bytes b = tc::encode_bc3(im);
        out.insert(out.end(), b.begin(), b.end());
        mips++;
        if (im.w == 1 && im.h == 1) break;
        tc::Img nx;
        nx.w = std::max(1u, im.w / 2), nx.h = std::max(1u, im.h / 2);
        nx.px.assign(size_t(nx.w) * nx.h * 4, 0);
        for (uint32_t y = 0; y < nx.h; y++)
            for (uint32_t x = 0; x < nx.w; x++)
                for (int c = 0; c < 4; c++) {
                    unsigned s = 0, k = 0;
                    for (uint32_t dy = 0; dy < 2; dy++)
                        for (uint32_t dx = 0; dx < 2; dx++) {
                            const uint32_t xx = std::min(im.w - 1, 2 * x + dx), yy = std::min(im.h - 1, 2 * y + dy);
                            s += im.at(xx, yy)[c], k++;
                        }
                    nx.at(x, y)[c] = uint8_t((s + k / 2) / k);
                }
        im = std::move(nx);
    }
    return {out, mips};
}
uint32_t pow2_in(double x, uint32_t lo, uint32_t hi) {
    uint32_t p = lo;
    while (p < hi && p < x) p *= 2;
    return p;
}

Built build_actor(const Inputs &in, const std::string &art, const Patch &pt) {
    PakWriter w;
    auto [span, sit] = extract_effect_span(in.T);
    AutoPages p0(w, std::max<int64_t>(1, (int64_t(sit[0].off) - 0x14) / 16));
    span->modulo = uint64_t(1) << 40;
    span->phase = sit[0].off;
    p0.span(span);
    Bytes vram;
    std::vector<TexP> tex;
    auto add = [&](const std::string &shrt, uint32_t h, uint32_t fmt, uint32_t tw, uint32_t th, uint32_t mips, const Bytes &tiled, uint32_t flags = 3,
                   std::optional<uint32_t> unk20 = std::nullopt) {
        while (vram.size() % 0x100) vram.push_back(0);
        const uint64_t off = vram.size();
        vram.insert(vram.end(), tiled.begin(), tiled.end());
        auto nb = std::make_shared<Blob>("texname");
        nb->cstr(shrt);
        auto e = std::make_shared<Tex>();
        e->name = p0.item("RAW_DATA", nb);
        e->full = cook_prefix() + shrt;
        auto db = std::make_shared<Blob>("vramdesc");
        db->u64(0).u64(off).u32(uint32_t(tiled.size())).u32(h);
        db->u32(1).u32(fmt).u32(unk20 ? *unk20 : (fmt == 0x48 || fmt == 0x4b || fmt == 0x4e) ? 0x010f0000 : 0x01000000);
        db->u32(mips).u32(tw).u32(th).u32(0).u32(flags);
        db->ptr(db, 0x40);
        db->cstr(e->full);
        e->hash = h, e->shrt = shrt, e->fmt = fmt;
        e->desc = p0.item("VRAM_DESC", db);
        tex.push_back(e);
        return e;
    };
    auto flat = [&](const std::string &tag, const uint32_t rgb[3], uint32_t fmt = 0x47) {
        const auto [lin, m] = bc1_flat(64, 64, rgb);
        const Bytes ti = tc::ps3_to_ps4_texels(lin, 64, 64, m, 8);
        const std::string sh = u3::fmt("art/generated/u3mp/emblem-%s.tga/%016X.ndb", tag.c_str(), sid(tag));
        return add(sh, sid(sh), fmt, 64, 64, m, ti);
    };
    const size_t sl = pt.mat.rfind('/');
    const std::string key = pt.mat.substr(0, sl) + ":" + pt.mat.substr(sl + 1);
    auto mmi = in.mapm.find(key);
    if (mmi == in.mapm.end()) throw Stop("KeyError: '" + key + "' (air-multi.pak material)");
    const TMat &mm = mmi->second;
    auto f32 = [](const Bytes &d, size_t i) {
        float f;
        std::memcpy(&f, &d.at(4 * i), 4);
        return double(f);
    };
    std::map<std::string, std::array<double, 3>> pv3;
    std::map<std::string, double> pv;
    for (const auto &p : mm.params) {
        if (p.cnt == 3) pv3[p.name] = {f32(p.vals, 0), f32(p.vals, 1), f32(p.vals, 2)};
        pv[p.name] = f32(p.vals, 0);
    }
    const TMat::Bind *cb = nullptr;
    for (const auto &b : mm.binds)
        if (b.pname == "g_tNdFetchColor1Map") {
            cb = &b;
            break;
        }
    if (!cb) throw Stop("IndexError: no colour map bound to " + key);
    auto ti = in.maptx.find(cb->full);
    if (ti == in.maptx.end()) ti = in.maptx.find(cb->full + ".ndb");
    if (ti == in.maptx.end()) throw Stop("TypeError: colour map " + cb->full + " not in air-multi.pak");
    const PakTexture &t = ti->second;
    const std::string full = t.path;
    const size_t pk = full.find(cook_prefix());
    const std::string shrt = pk != std::string::npos ? full.substr(pk + cook_prefix().size()) : full;
    const Bytes tvram = in.MAP.vram_bytes(t.vramOffset, t.vramSize);
    TexP ecol;   // the bake below (the wall colour map itself is not carried)
    const uint32_t c_shared[3] = {128, 128, 166}, c_white[3] = {255, 255, 255};
    TexP eshared = flat("shared-flat-b166", c_shared);
    TexP ewhite = flat("white", c_white);
    // layer 4 has no colour map: its base (Overlay4Map, on uv1) gets the wall's mean tone
    std::string kind;
    int bpb;
    switch (t.format) {
    case 0x47: case 0x48: kind = "BC1", bpb = 8; break;
    case 0x4a: case 0x4b: kind = "BC2", bpb = 16; break;
    case 0x4d: case 0x4e: kind = "BC3", bpb = 16; break;
    default: throw Stop(u3::fmt("KeyError: colour map format %u", t.format));
    }
    const tc::Img img = tc::decode(tc::ps4_mip0_linear(tvram, t.width, t.height, bpb), t.width, t.height, kind);
    double sum[3] = {0, 0, 0};
    const size_t npx = size_t(img.w) * img.h;
    for (size_t i = 0; i < npx; i++)
        for (int c = 0; c < 3; c++) sum[c] += img.px[i * 4 + size_t(c)];
    const std::array<double, 3> one = {1, 1, 1};
    const std::array<double, 3> tint = pv3.count("g_colorTint1") ? pv3["g_colorTint1"] : pv3.count("g_colorTint2") ? pv3["g_colorTint2"] : one;
    uint32_t tone[3];
    for (int c = 0; c < 3; c++) {
        const double mean = sum[c] / double(npx) / 255.0;
        const double x = mean * std::pow(tint[size_t(c)], 1 / 2.2) * 255;
        tone[c] = uint32_t(int(std::min(std::max(x, 0.0), 255.0)));
    }
    (void)tone;

    // the wall's look baked: layer 1 / 2 colour maps of the wall material with their tint and brightness, blended by the
    // PS3 vertex blend value and the blend map. Color1Map = the bake in the patch's uv0 box (the actor's uv0 is
    // written as that box's 0..1), Overlay4Map (the base of the cloth's layer 4, on uv1) = the bake in the emblem
    // square: both layers of the cloth then show the same wall.
    auto decode_bind = [&](const char *pname) -> std::optional<tc::Img> {
        for (const auto &bd : mm.binds) {
            if (bd.pname != pname) continue;
            auto it = in.maptx.find(bd.full);
            if (it == in.maptx.end()) it = in.maptx.find(bd.full + ".ndb");
            if (it == in.maptx.end()) return std::nullopt;
            const PakTexture &x = it->second;
            std::string kd;
            int bb;
            switch (x.format) {
            case 0x47: case 0x48: kd = "BC1", bb = 8; break;
            case 0x4a: case 0x4b: kd = "BC2", bb = 16; break;
            case 0x4d: case 0x4e: kd = "BC3", bb = 16; break;
            default: return std::nullopt;
            }
            return tc::decode(tc::ps4_mip0_linear(in.MAP.vram_bytes(x.vramOffset, x.vramSize), x.width, x.height, bb), x.width, x.height, kd);
        }
        return std::nullopt;
    };
    auto layer_mul = [&](int k) {
        const std::string tk = "g_colorTint" + std::to_string(k), bk = "g_colorBrightness" + std::to_string(k);
        const std::array<double, 3> tn = pv3.count(tk) ? pv3[tk] : one;
        const double br = pv.count(bk) ? pv[bk] : 1.0;
        std::array<double, 3> m{};
        for (int c = 0; c < 3; c++) m[size_t(c)] = std::pow(std::max(0.0, tn[size_t(c)] * br), 1 / 2.2);
        return m;
    };
    BakeLayer l1, l2;
    l1.img = img, l1.mul = layer_mul(1);
    l2.img = decode_bind("g_tNdFetchColor2Map"), l2.mul = layer_mul(2);
    const std::optional<tc::Img> bmap = decode_bind("g_tNdFetchBlendMap");
    double umin = 1e30, umax = -1e30, vmin = 1e30, vmax = -1e30;
    for (const auto &u : pt.v.uv0) umin = std::min(umin, u[0]), umax = std::max(umax, u[0]), vmin = std::min(vmin, u[1]), vmax = std::max(vmax, u[1]);
    const double du = std::max(umax - umin, 1e-6), dv = std::max(vmax - vmin, 1e-6);
    const uint32_t bw = pow2_in(du * img.w, 16, 256), bh = pow2_in(dv * img.h, 16, 256);
    const tc::Img bakeA = bake(pt, pt.tris, [&](uint32_t i) { return std::array<double, 2>{(pt.v.uv0[i][0] - umin) / du, (pt.v.uv0[i][1] - vmin) / dv}; }, bw, bh,
                               l1, l2, bmap);
    std::vector<Tri> inside;   // the pieces in the emblem square (the clip gives the others uv1 (0, 0))
    for (const Tri &t : pt.tris) {
        bool out = true;
        for (uint32_t k : t) out = out && pt.v.uv1[k][0] == 0.0 && pt.v.uv1[k][1] == 0.0;
        if (!out) inside.push_back(t);
    }
    const tc::Img bakeB = bake(pt, inside, [&](uint32_t i) { return std::array<double, 2>{pt.v.uv1[i][0], pt.v.uv1[i][1]}; }, 128, 128, l1, l2, bmap);
    auto add_bake = [&](const std::string &tag, const tc::Img &im) {
        const auto [lin, mips] = bc3_mips(im);
        const Bytes tiled = tc::ps3_to_ps4_texels(lin, im.w, im.h, mips, 16);
        const std::string sh = u3::fmt("art/generated/u3mp/%s-%s.tga/%016X.ndb", art.c_str(), tag.c_str(), sid(art + "-" + tag));
        return add(sh, sid(sh), 0x4e, im.w, im.h, mips, tiled);
    };
    ecol = add_bake("wall", bakeA);
    TexP etone = add_bake("wall-emblem-square", bakeB);

    // two materials: the cloth's first (the emblem) with the wall's tint, brightness and specular, and its second
    const std::vector<TMat> &tm = in.tm;
    if (tm.size() < 2) throw Stop("IndexError: loading-menu-cloth has fewer than 2 materials");
    auto pack4 = [](double a, double b, double c, double d) {
        Bytes o(16);
        const float f[4] = {float(a), float(b), float(c), float(d)};
        std::memcpy(o.data(), f, 16);
        return o;
    };
    auto get = [&](const std::string &k, double def) { return pv.count(k) ? pv[k] : def; };
    auto ov = [&](const std::string &nm, const Bytes &vals) -> Bytes {
        auto starts = [&](const char *s) { return nm.rfind(s, 0) == 0; };
        if (nm == "g_ambientOcclusionTint" || starts("g_facingTintIn") || starts("g_facingTintOut")) return pack4(1, 1, 1, 0);
        if (nm == "g_colorTint1") return pack4(1, 1, 1, 0);         // tint and brightness are in the bake
        if (nm == "g_colorBrightness1") return pack4(1, 0, 0, 0);
        if (nm == "g_normalStrength1" || nm == "g_specularMetalness" || nm == "g_uvOffsetU2" || nm == "g_uvOffsetV2") return pack4(0, 0, 0, 0);
        if (nm == "g_specularBrightness") return pack4(get("g_specularBrightness", 0.0), 0, 0, 0);
        if (nm == "g_specularExponentMax") return pack4(get("g_specularExponentMax", 10.0), 0, 0, 0);
        if (starts("g_uvScale")) return pack4(1, 0, 0, 0);
        return vals;
    };
    const std::map<std::string, TexP> b0 = {{"g_tNdFetchSharedTexture1Map", eshared}, {"g_tNdFetchColor1Map", ecol}, {"g_tNdFetchOverlay1Map", ewhite},
                                            {"g_tNdFetchOverlay4Map", etone},      {"g_tNdFetchTransparencyMap", ewhite}};
    const std::string hx = md5_hex(Bytes(art.begin(), art.end()));
    const std::string guid = hx.substr(0, 8) + "-" + hx.substr(8, 4) + "-" + hx.substr(12, 4) + "-" + hx.substr(16, 4) + "-" + hx.substr(20, 12);
    struct Rec {
        std::string name;
        const TMat *tm;
        std::vector<std::tuple<std::string, uint32_t, Bytes>> params;
        std::vector<std::pair<std::string, TexP>> binds;
    };
    std::vector<Rec> recs(2);
    recs[0].name = "objects/levels/multiplayer/" + art + ":default," + guid + "," + tm[0].shader;
    recs[0].tm = &tm[0];
    for (const auto &p : tm[0].params) recs[0].params.push_back({p.name, p.cnt, ov(p.name, p.vals)});
    for (const auto &b : tm[0].binds) {
        auto it = b0.find(b.pname);
        if (it == b0.end()) throw Stop("KeyError: '" + b.pname + "'");
        recs[0].binds.push_back({b.pname, it->second});
    }
    recs[1].name = tm[1].name;
    recs[1].tm = &tm[1];
    for (const auto &p : tm[1].params) recs[1].params.push_back({p.name, p.cnt, p.vals});
    for (const auto &b : tm[1].binds) recs[1].binds.push_back({b.pname, b.pname.find("Color") != std::string::npos ? ewhite : eshared});

    auto ttab = p0.item("TEXTURE_TABLE");
    std::vector<TexP> order = tex;
    std::stable_sort(order.begin(), order.end(), [](const TexP &a, const TexP &b) { return a->hash < b->hash; });
    auto nb = std::make_shared<Blob>("ttnames");
    for (const auto &e : order) nb->ptr(e->name->data);
    auto nit = p0.item("RAW_DATA", nb);
    auto hb = std::make_shared<Blob>("tthash");
    for (const auto &e : order) hb->u32(e->hash);
    auto hit = p0.item("RAW_DATA", hb);
    BlobP tb = ttab->data;
    tb->u64(order.size()).ptr(hit->data).ptr(nit->data);
    tb->ptr(tb, 0x20);
    for (const auto &e : order) tb->ptr(e->desc->data);
    auto strs = std::make_shared<Blob>("geostrings");
    std::map<std::string, size_t> sp;
    auto S = [&](const std::string &s) {
        auto it = sp.find(s);
        if (it != sp.end()) return it->second;
        return sp[s] = strs->cstr(s);
    };
    auto raw = [&](const Bytes &d) {
        auto b = std::make_shared<Blob>("raw");
        b->raw(d);
        return p0.item("RAW_DATA", b)->data;
    };
    const Verts &v = pt.v;
    const size_t nv = v.size();
    std::array<double, 3> c{};
    for (int k = 0; k < 3; k++) {
        double lo = v.pos[0][size_t(k)], hi = lo;
        for (const auto &p : v.pos) lo = std::min(lo, p[size_t(k)]), hi = std::max(hi, p[size_t(k)]);
        c[size_t(k)] = (lo + hi) / 2;
    }
    std::vector<std::array<float, 3>> pos(nv);
    float rad = 0;
    for (size_t i = 0; i < nv; i++) {
        for (int k = 0; k < 3; k++) pos[i][size_t(k)] = float(v.pos[i][size_t(k)] - c[size_t(k)]);
        const float sx = pos[i][0] * pos[i][0], sy = pos[i][1] * pos[i][1], sz = pos[i][2] * pos[i][2];
        const float n2 = (sx + sy) + sz;
        rad = std::max(rad, std::sqrt(n2));
    }
    auto fmtb = std::make_shared<Blob>("vtxfmt");
    std::map<int, size_t> fo;
    const std::vector<std::pair<int, std::array<uint8_t, 4>>> fmts = {{0, {0, 0, 12, 6}},    {1, {1, 0, 4, 0x22}}, {12, {12, 0, 4, 0x22}},
                                                                      {2, {2, 0, 8, 0x0a}}, {3, {3, 0, 8, 0x0a}}, {4, {4, 0, 8, 0x0a}}};
    for (const auto &[s_, f_] : fmts) {
        fo[s_] = fmtb->size();
        fmtb->raw(Bytes(f_.begin(), f_.end()));
    }
    p0.item("RAW_DATA", fmtb);
    auto h16 = [](Bytes &o, double x) { const uint16_t h = f16b(x); o.push_back(uint8_t(h)), o.push_back(uint8_t(h >> 8)); };
    Bytes sp0, s1, s12, s2, s3, s4;
    for (size_t i = 0; i < nv; i++) {
        const uint8_t *pb = reinterpret_cast<const uint8_t *>(pos[i].data());
        sp0.insert(sp0.end(), pb, pb + 12);
        h16(s1, (v.uv0[i][0] - umin) / du), h16(s1, (v.uv0[i][1] - vmin) / dv);   // uv0 = the bake's box
        for (int k = 0; k < 2; k++) h16(s12, v.uv1[i][size_t(k)]);
        for (int k = 0; k < 3; k++) h16(s2, v.nrm[i][size_t(k)]);
        h16(s2, 1.0);
        for (int k = 0; k < 4; k++) h16(s3, v.tan[i][size_t(k)]), h16(s4, 1.0);
    }
    const std::vector<std::tuple<int, uint32_t, const Bytes *>> streams = {{0, 12, &sp0}, {1, 4, &s1}, {12, 4, &s12}, {2, 8, &s2}, {3, 8, &s3}, {4, 8, &s4}};
    auto st = std::make_shared<Blob>("streams");
    for (const auto &[s_, sd, d] : streams) {
        auto db = raw(*d);
        st->u32((sd << 16) | 1).u32(uint32_t(d->size())).ptr(fmtb, int64_t(fo[s_])).ptr(db);
    }
    auto sti = p0.item("RAW_DATA", st);
    Bytes ibd, cnts, offs, js, ws;
    for (const Tri &t : pt.tris)
        for (uint32_t x : t) ibd.push_back(uint8_t(x)), ibd.push_back(uint8_t(x >> 8));
    auto ib = raw(ibd);
    const uint32_t ni = uint32_t(pt.tris.size() * 3);
    for (size_t i = 0; i < nv; i++) {
        cnts.push_back(1), cnts.push_back(0);
        offs.push_back(uint8_t(i)), offs.push_back(uint8_t(i >> 8));
        js.push_back(0), js.push_back(0);
        const float one_f = 1.0f;
        const uint8_t *ob = reinterpret_cast<const uint8_t *>(&one_f);
        ws.insert(ws.end(), ob, ob + 4);
    }
    auto bc = raw(cnts), bo = raw(offs), bj = raw(js), bwt = raw(ws);
    auto skb = std::make_shared<Blob>("skin");
    skb->u32(0).u32(uint32_t(nv)).ptr(bc).ptr(bo).ptr(bj).ptr(bwt);
    auto ski = p0.item("RAW_DATA", skb);
    const Pak &SK = in.SK;
    const auto jhs = SK.items_of("JOINT_HIERARCHY"), fgs = SK.items_of("FOREGROUND_SECTION_2");
    if (jhs.empty() || fgs.empty()) throw Stop("IndexError: cargo-plane-int-moving-multi.pak without its skeleton");
    const PakItem &jh = jhs[0], &fgS = fgs[0];
    const uint32_t skel = SK.pu32(fgS.page, fgS.data_off() + 8), hier = SK.pu32(fgS.page, fgS.data_off() + 12);
    const Ref jr = bg::need(SK, fgS.page, fgS.data_off() + 0x10);
    const PakItem jit = SK.item_at(jr.first, uint32_t(jr.second - 0x20));
    const uint64_t jb = SK.pbase(jr.first) + jr.second;
    const Bytes jnames(SK.d.begin() + std::ptrdiff_t(jb), SK.d.begin() + std::ptrdiff_t(std::min<uint64_t>(jb + jit.size, SK.d.size())));
    auto subs = std::make_shared<Blob>("submeshes");
    const std::string sname = art + ".Prototype0.Submesh0";
    subs->u64(0).ptr(strs, int64_t(S(sname))).u32(1).u32(0).u32(0).u32(0).u32(uint32_t(nv)).u32(ni).u32(uint32_t(streams.size())).u32(0).ptr(sti->data).u64(0).ptr(ib);
    subs->ptr(strs, int64_t(S(recs[0].name))).i32(-1).u32(0).u32(1).u32(0).ptr(ski->data).raw(Bytes(0x20, 0));
    p0.item("RAW_DATA", subs);
    const std::string pname = "Prototype0." + art;
    auto fl = std::make_shared<Blob>("pf");
    fl->raw(Bytes{1, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0});
    auto sn = std::make_shared<Blob>("sn");
    sn->ptr(strs, int64_t(S(sname))).i32(-1).u32(0);
    auto pb = std::make_shared<Blob>("prototypes");
    pb->u32(3).u32(1).u32(0).u32(0x5791a421).ptr(strs, int64_t(S(pname))).ptr(sn).ptr(fl).u64(0).u32(in.cb3).u32(in.cb4).raw(Bytes(0x78 - 0x38, 0));
    auto pl = std::make_shared<Blob>("pl");
    pl->ptr(pb);
    for (const auto &b : {pb, fl, sn, pl}) p0.item("RAW_DATA", b);
    std::vector<TexP> gt;
    for (const Rec &m : recs)
        for (const auto &[n, e] : m.binds)
            if (std::find(gt.begin(), gt.end(), e) == gt.end()) gt.push_back(e);
    auto texb = std::make_shared<Blob>("gt");
    for (const auto &e : gt) texb->ptr(strs, int64_t(S(bindfull(*e)))).u32(e->hash).u32(0x200).u32(0x101).i32(-1).u32(0).u32(4).u64(0);
    auto fe = std::make_shared<Blob>("f"), pa = std::make_shared<Blob>("p"), va = std::make_shared<Blob>("v"), bi = std::make_shared<Blob>("b"),
         ma = std::make_shared<Blob>("m");
    for (const Rec &m : recs) {
        const TMat &tmm = *m.tm;
        const size_t fo_ = fe->size();
        for (const auto &f : tmm.feats) fe->ptr(strs, int64_t(S(f)));
        const size_t po = pa->size();
        for (const auto &[n_, c_, vals] : m.params) {
            const size_t vo = va->size();
            va->raw(vals);
            pa->ptr(strs, int64_t(S(n_))).ptr(va, int64_t(vo)).u32(c_).u32(0);
        }
        const size_t bo_ = bi->size();
        for (const auto &[pn, e] : m.binds) {
            const std::string sh = e->shrt.size() >= 4 && e->shrt.compare(e->shrt.size() - 4, 4, ".ndb") == 0 ? e->shrt.substr(0, e->shrt.rfind('/')) : e->shrt;
            bi->ptr(strs, int64_t(S(pn))).ptr(strs, int64_t(S(sh))).ptr(strs, int64_t(S(bindfull(*e)))).i32(-1).u32(0).u64(0);
        }
        ma->ptr(strs, int64_t(S(m.name))).ptr(strs, int64_t(S(tmm.shader))).ptr(fe, int64_t(fo_)).ptr(strs, int64_t(S(tmm.hash))).ptr(pa, int64_t(po)).ptr(bi, int64_t(bo_));
        ma->u32(uint32_t(tmm.feats.size())).u32(uint32_t(m.params.size())).u32(uint32_t(m.binds.size())).u32(tmm.x3).u32(tmm.tail).u32(0);
    }
    for (const auto &b : {fe, pa, va, bi, ma, texb}) p0.item("RAW_DATA", b);
    p0.item("RAW_DATA", strs);
    auto gb = std::make_shared<Blob>("geometry");
    gb->u32(0x10).u32(1).u64(1).ptr(subs).u64(1).ptr(pl).u64(0).ptr(nullptr).u64(gt.size()).ptr(texb).u64(2).ptr(ma).u64(0).u64(0);
    auto geo = p0.item("GEOMETRY_1", gb);
    auto jn = raw(jnames);
    auto fb = std::make_shared<Blob>("fg");
    fb->ptr(fb, 0x1c).u32(skel).u32(hier).ptr(jn);
    fb->u32(0);   // 0.0f
    fb->cstr(pname);
    auto fg = p0.item("FOREGROUND_SECTION_2", fb, art);
    auto ji = p0.item("JOINT_HIERARCHY", copy_item_blob(SK, jh), jh.name);
    auto tv = std::make_shared<Blob>("t");
    {
        const float f[4] = {0, 0, 0, rad};
        Bytes b(16);
        std::memcpy(b.data(), f, 16);
        tv->raw(b);
    }
    auto tv_ = p0.item("TAG_VEC4", tv, "vis_sphere");
    auto tiB = std::make_shared<Blob>("t");
    tiB->i32(0);
    auto ti_ = p0.item("TAG_INT", tiB, "vis_joint_index");
    const uint32_t keys[2] = {sid(jh.name), sid("geo." + art)};
    const int nb_ = 7, n = 2;
    auto ab = std::make_shared<Blob>("artgroup");
    ab->u32(n).u32(1).ptr(ab, 0x50).ptr(ab, 0x30).u64(0).u64(0).u32(PAD32).u32(PAD32);
    const int64_t tbl = 0x50 + 16 * n, x = tbl + 16 * nb_;
    ab->u64(0).u32(1).u32(0x12345).ptr(ab, x).u32(PAD32).u32(PAD32);
    ab->ptr(ji->data, -0x20).u64(0);
    ab->u64(0).u64(0);
    std::optional<std::pair<uint32_t, int>> bk[nb_];
    for (int k = 0; k < 2; k++) {
        int i = int(keys[k] % nb_);
        while (bk[i]) i = (i + 1) % nb_;
        bk[i] = std::make_pair(keys[k], k);
    }
    for (const auto &e : bk) {
        if (!e) ab->u32(0).u32(PAD32).u64(0);
        else ab->u32(e->first).u32(PAD32).ptr(ab, 0x50 + 16 * e->second);
    }
    ab->u32(0).u32(PAD32).u64(0).u64(0).u32(0).u32(nb_).ptr(ab, tbl);
    auto ag = p0.item("ARTGROUP_DATA", ab, art);
    auto lp = std::make_shared<Blob>("lp");
    lp->u64(0);
    auto lpi = p0.item("LEVEL_PATS_1", lp);
    auto pkb = std::make_shared<Blob>("pk");
    pkb->ptr(pkb, 0x20).u32(0).u32(1).ptr(pkb, int64_t(0x20 + ((art.size() + 1 + 15) / 16) * 16)).u32(PAD32).u32(PAD32);
    pkb->cstr(art);
    pkb->pad(16, 0xaa);
    pkb->u32(sid(art)).u32(PAD32).ptr(ag->data, -0x20);
    auto pki = p0.item("PACKAGE_ITEM_LOOKUP", pkb);
    Page &p1 = w.page();
    std::vector<ItemP> descs;
    for (const auto &e : tex) descs.push_back(e->desc);
    make_login_and_vram_page(w, p1, {nullptr, nullptr, nullptr}, descs);
    w.vram = vram;
    auto span_item = std::make_shared<PakWItem>();   // the EFFECT_TABLE inside the copied span
    span_item->type = "EFFECT_TABLE";
    span_item->span_ref = span;
    for (const PakItem &i : sit)
        if (i.type == "EFFECT_TABLE") {
            span_item->span_pos = i.off - sit[0].off;
            break;
        }
    w.login_targets = {lpi, ag, span_item, ttab, geo, ji, fg, pki, tv_, ti_};
    Bytes lb(8 + 8 * w.login_targets.size(), 0);
    lb[0] = uint8_t(w.login_targets.size());
    w.login_item->data->b = lb;
    Built r;
    r.art = art;
    r.pak = w.build();
    r.center = c;
    r.verts = nv, r.tris = pt.tris.size(), r.nout = pt.nout;
    const std::string dir = shrt.substr(0, shrt.rfind('/'));
    r.color = dir.substr(dir.rfind('/') + 1);
    return r;
}

// Airstrip's actor names (kept: its paks and old installs use them)
const std::vector<std::pair<std::string, std::string>> SHORT = {{"ground-concrete", "ground"},      {"metal-diamond", "diamond"}, {"corrugated-main-blue", "corr-blue"},
                                                                {"corrugated-main", "corr"},        {"control-tower", "tower"},   {"stucco-brown", "stucco"},
                                                                {"blackboard", "board"}};
const std::string AIR = "air-multi";

// the actor names of a level's patches: Airstrip air-multi-emblem-<wall>[-<instance>], the others
// '<level>-emblem-<n>' / '<level>-emblem-board-<n>' (the helper's wall walk takes '-multi-emblem-' and 'boa' after it)
std::vector<std::string> art_names(const std::string &level, const std::vector<Patch> &pts) {
    std::vector<std::string> out;
    int n = 0, nb = 0;
    for (const Patch &pt : pts) {
        if (level == AIR) {
            std::string s;
            for (const auto &[k, v] : SHORT)
                if (pt.mat.find(k) != std::string::npos) {
                    s = v;
                    break;
                }
            if (s.empty()) throw Stop("IndexError: no short name for " + pt.mat);
            out.push_back("air-multi-emblem-" + s + (pt.blackboard ? "-" + std::to_string(pt.inst) : ""));
        } else
            out.push_back(level + "-emblem-" + (pt.blackboard ? "board-" + std::to_string(nb++) : std::to_string(n++)));
    }
    return out;
}

// the centre of a patch's box (where its actor is placed; build_actor centres the positions on it)
std::array<double, 3> patch_center(const Patch &pt) {
    std::array<double, 3> c{};
    for (int k = 0; k < 3; k++) {
        double lo = pt.v.pos[0][size_t(k)], hi = lo;
        for (const auto &p : pt.v.pos) lo = std::min(lo, p[size_t(k)]), hi = std::max(hi, p[size_t(k)]);
        c[size_t(k)] = (lo + hi) / 2;
    }
    return c;
}

}  // namespace

namespace wallemblem {

bool emblem_level(const std::string &level) { return level.size() > 6 && level.compare(level.size() - 6, 6, "-multi") == 0; }

std::vector<ExtraSpawner> spawners(const Pak &p3stage, const std::string &level) {
    const std::vector<Patch> pts = patches(p3stage);
    const std::vector<std::string> names = art_names(level, pts);
    std::vector<ExtraSpawner> out;
    for (size_t i = 0; i < pts.size(); i++) {
        const auto c = patch_center(pts[i]);
        out.push_back({names[i] + "-spawner", names[i], {c[0], c[1], c[2]}});
    }
    return out;
}

std::vector<std::pair<std::string, Bytes>> actors(const Pak &p3stage, const std::string &level, const Pak &converted, const fs::path &retail_actor33) {
    const std::vector<Patch> pts = patches(p3stage);
    if (pts.empty()) return {};
    const std::vector<std::string> names = art_names(level, pts);
    Inputs in{Pak(retail_actor33 / "loading-menu-cloth.pak"), Pak(retail_actor33 / "cargo-plane-int-moving-multi.pak"), converted, {}, {}, {}};
    in.tm = tmpl_materials(in.T);
    // the per-instance cbuffer capacity the game needs (crashcheck G6: one feature set + 2), written directly
    in.cb3 = 3, in.cb4 = 1;
    for (const PakItem &i : in.T.items)
        if (i.type == "RAW_DATA" && i.size >= 0x38) {
            const uint64_t o = in.T.pbase(i.page) + i.data_off();
            if (rd32le(in.T.d, o) == 3 && rd32le(in.T.d, o + 12) == 0x5791a421) in.cb4 = std::max(in.cb4, rd32le(in.T.d, o + 0x34));
        }
    for (const PakTexture &t : in.MAP.textures()) in.maptx[t.path] = t;
    for (const TMat &m : tmpl_materials(in.MAP)) in.mapm[m.name.substr(0, m.name.find(','))] = m;
    std::vector<std::pair<std::string, Bytes>> out;
    for (size_t i = 0; i < pts.size(); i++) out.push_back({names[i], build_actor(in, names[i], pts[i]).pak});
    return out;
}

}  // namespace wallemblem

}  // namespace u3
