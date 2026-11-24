// dctools/normal_blend.cpp: see normal_blend.hpp. The PS3 side: the main-pass fragment program of each material
// (disassembled by ps3fp) says where its blend factor comes from and which texture units it reads.
#include "normal_blend.hpp"
#include "ps3bg.hpp"
#include "ps3fp.hpp"
#include "stageplan.hpp"
#include "texcodec.hpp"
#include "u3fix.hpp"

#include <cstring>
#include <regex>

namespace u3 {
namespace {

using Mat4 = nblend::Material;
const std::string INV = "BLEND_FACTOR_INVERT_BLEND_MAP";
const uint32_t CAP = 256;

bool has(const std::vector<std::string> &v, const std::string &x) { return std::find(v.begin(), v.end(), x) != v.end(); }
bool has(const std::string &s, const std::string &x) { return s.find(x) != std::string::npos; }
bool starts(const std::string &s, const std::string &x) { return s.rfind(x, 0) == 0; }
std::string before(const std::string &s, char c) { return s.substr(0, s.find(c)); }
std::string last_part(const std::string &s) { return s.substr(s.rfind('/') == std::string::npos ? 0 : s.rfind('/') + 1); }
std::string pstr_need(const Pak &q, const ORef &r) {   // q.pstr(*q.resolve(...)): a null pointer stops (Python's TypeError)
    if (!r) throw Stop("TypeError: no pointer");
    return q.pstr(r->first, r->second);
}

// ---- PS3 program facts (p3prog.py) --------------------------------------------------------------------------------
struct Ins {
    std::string op, d;
    std::vector<std::string> a;
};
std::optional<Ins> parse(std::string l) {
    for (size_t k; (k = l.find("  (end)")) != std::string::npos;) l.erase(k, 7);
    static const std::regex rx(R"(^(\w+) (\S+), (.*)$)");
    std::smatch m;
    if (!std::regex_search(l, m, rx)) return std::nullopt;
    Ins t{m[1], m[2], {}};
    const std::string rest = m[3];
    // split on ', ' that is not inside a {...} constant
    size_t s = 0;
    for (size_t k = 0; k + 1 < rest.size(); k++) {
        if (rest[k] != ',' || rest[k + 1] != ' ') continue;
        bool inside = false;
        for (size_t j = k + 2; j < rest.size(); j++) {
            if (rest[j] == '{') break;
            if (rest[j] == '}') {
                inside = true;
                break;
            }
        }
        if (inside) continue;
        t.a.push_back(rest.substr(s, k - s));
        s = k + 2;
        k++;
    }
    t.a.push_back(rest.substr(s));
    for (auto &x : t.a) {   // strip()
        size_t b = 0;
        while (b < x.size() && py_space((unsigned char)x[b])) b++;
        x = py_rstrip(x.substr(b));
    }
    return t;
}
std::string lstrip_set(std::string x, const std::string &set) {
    size_t b = 0;
    while (b < x.size() && set.find(x[b]) != std::string::npos) b++;
    return x.substr(b);
}
std::string rstrip_set(std::string x, const std::string &set) {
    while (!x.empty() && set.find(x.back()) != std::string::npos) x.pop_back();
    return x;
}
std::string split_at(const std::string &x, char c, size_t i) {   // x.split(c)[i]
    size_t s = 0;
    for (size_t k = 0; k < i; k++) {
        s = x.find(c, s);
        if (s == std::string::npos) throw Stop("IndexError: list index out of range");
        s++;
    }
    const size_t e = x.find(c, s);
    return x.substr(s, e == std::string::npos ? std::string::npos : e - s);
}
std::string base(const std::string &x) { return rstrip_set(split_at(lstrip_set(x, "-|"), '.', 0), "|"); }
std::string swz(const std::string &x0) {
    const std::string x = rstrip_set(lstrip_set(x0, "-|"), "|");
    return x.find('.') != std::string::npos ? split_at(x, '.', 1) : "xyzw";
}
std::pair<std::string, std::string> wr(const std::string &d) {
    return {split_at(d, '.', 0), d.find('.') != std::string::npos ? split_at(d, '.', 1) : "xyzw"};
}
int tex_unit(const std::string &a) {   // int(a[3:])
    const std::string s = a.size() > 3 ? a.substr(3) : "";
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos) throw Stop("ValueError: invalid literal for int() with base 10: '" + s + "'");
    return std::stoi(s);
}
struct Src {   // ('tex', unit, chan, coord) or (op, None, None, None) or ('?',)
    std::string kind;
    std::optional<int> unit;
    std::string chan, coord;
};
std::optional<Src> trace(const std::vector<std::optional<Ins>> &P, size_t i, const std::string &reg, char c, int depth = 0) {
    for (size_t j = i; j-- > 0;) {
        if (!P[j]) continue;
        const Ins &t = *P[j];
        const auto [rn, mk] = wr(t.d);
        if (rn != reg || mk.find(c) == std::string::npos) continue;
        if (starts(t.op, "TEX")) return Src{"tex", tex_unit(t.a.back()), std::string(1, c), t.a.at(0)};
        if (starts(t.op, "MOV") && depth < 6 && !t.a.empty() && !starts(t.a[0], "{") && !starts(t.a[0], "f[")) {
            const size_t k = mk.find(c);
            const std::string s = swz(t.a[0]);
            char s2;
            if (s.size() == mk.size() || (s.size() == 4 && mk.size() == 4)) s2 = s.at(k);
            else if (s == "xyzw") s2 = c;
            else s2 = s.at(std::min(std::string("xyzw").find(c), s.size() - 1));
            return trace(P, j, base(t.a[0]), s2, depth + 1);
        }
        return Src{before(t.op, '_'), std::nullopt, "", ""};
    }
    return std::nullopt;
}
std::optional<Src> blend_source(const std::vector<std::string> &L) {
    std::vector<std::optional<Ins>> P;
    for (const auto &l : L) P.push_back(parse(l));
    std::optional<std::pair<std::string, char>> k2, rc;
    auto traced = [&](size_t i, const std::string &x) -> std::optional<Src> {
        const std::string s = swz(x);
        auto r = trace(P, i, base(x), s.at(0));
        return r ? r : Src{"?", std::nullopt, "", ""};
    };
    for (size_t i = 0; i < P.size(); i++) {
        if (!P[i]) continue;
        const Ins &t = *P[i];
        if (starts(t.op, "MAD") && has(L[i], "0.0100021")) {
            k2 = std::make_pair(wr(t.d).first, wr(t.d).second.at(0));
            rc.reset();
            continue;
        }
        if (k2 && starts(t.op, "DIV") && t.a.size() >= 2 && base(t.a[1]) == k2->first) return traced(i, t.a[0]);
        if (k2 && starts(t.op, "RCP") && base(t.a.at(0)) == k2->first) {
            rc = std::make_pair(wr(t.d).first, wr(t.d).second.at(0));
            continue;
        }
        if (rc && (starts(t.op, "MUL") || starts(t.op, "MAD"))) {
            const size_t n2 = std::min<size_t>(2, t.a.size());
            bool any = false;
            for (size_t k = 0; k < n2; k++) any = any || base(t.a[k]) == rc->first;
            if (!any) continue;
            std::vector<std::string> o;
            for (size_t k = 0; k < n2; k++)
                if (base(t.a[k]) != rc->first) o.push_back(t.a[k]);
            if (starts(t.op, "MAD") && starts(t.a.at(0), "-") && t.a.at(1) == t.a.at(2)) o = {t.a[0]};
            if (!o.empty()) return traced(i, o[0]);
        }
    }
    return std::nullopt;
}
std::map<int, std::vector<std::pair<std::string, std::string>>> tex_use(const std::vector<std::string> &L) {
    std::map<int, std::vector<std::pair<std::string, std::string>>> out;
    for (const auto &l : L) {
        const auto t = parse(l);
        if (t && (starts(t->op, "TEX") || starts(t->op, "TXP"))) out[tex_unit(t->a.back())].push_back({t->a.at(0), wr(t->d).second});
    }
    return out;
}
struct Facts {
    bool zproof = false;
    std::vector<std::pair<int, std::string>> units;   // in unit order
    std::optional<Src> src;
    std::map<int, std::vector<std::pair<std::string, std::string>>> use;
    bg::Params params;
};
std::map<std::string, Facts> facts(const Pak &p) {
    std::map<std::string, Facts> out;
    const auto mts = p.items_of("MATERIAL_TABLE_1");
    if (mts.empty()) return out;
    const uint32_t pg = mts[0].page;
    const uint64_t b = mts[0].data_off();
    const uint32_t nmat = p.pu32(pg, b + 0xc), ntex = p.pu32(pg, b + 4);
    const ORef ma = bg::res(p, pg, b + 0x20), th = bg::res(p, pg, b + 0x18), nm = bg::res(p, pg, b + 0x30), sr = bg::res(p, pg, b + 0x1c);
    const auto PAR = bg::all_material_params(p);
    std::map<uint32_t, std::vector<std::string>> cache;
    auto tn = [&](const ORef &rr) -> std::optional<std::string> {
        if (rr && th && rr->first == th->first && th->second <= rr->second && rr->second < th->second + 4ull * ntex) {
            const std::string s = pstr_need(p, bg::res(p, bg::need(nm).first, bg::need(nm).second + 4 * ((rr->second - th->second) / 4)));
            std::vector<std::string> parts;
            size_t a = 0;
            for (size_t k; (k = s.find('/', a)) != std::string::npos; a = k + 1) parts.push_back(s.substr(a, k - a));
            parts.push_back(s.substr(a));
            if (parts.size() < 2) throw Stop("IndexError: list index out of range");
            return parts[parts.size() - 2];
        }
        return std::nullopt;
    };
    for (uint32_t i = 0; i < nmat; i++) {
        const Ref m = bg::need(p, bg::need(ma).first, bg::need(ma).second + 4ull * i);
        const std::string name = pstr_need(p, bg::res(p, m.first, m.second));
        std::optional<std::tuple<std::vector<std::string>, Ref, uint32_t>> best;
        for (const bg::WalkNode &w : bg::walk_passes(p, m)) {
            if (w.k > 2) continue;
            const uint32_t pid = bg::be16(p, p.pbase(w.node.first) + w.node.second);
            const ORef t = bg::res(p, bg::need(sr).first, bg::need(sr).second + 0x10ull * pid + 12);
            if (!t) continue;
            const uint64_t B = p.pbase(t->first) + t->second;
            if (!cache.count(pid)) {
                try {
                    const uint32_t size = be32(p.d, B), off = be32(p.d, B + 16);
                    const uint64_t s = std::min<uint64_t>(B + off, p.d.size()), e = std::min<uint64_t>(B + uint64_t(off) + size, p.d.size());
                    cache[pid] = fp::dis(Bytes(p.d.begin() + std::ptrdiff_t(s), p.d.begin() + std::ptrdiff_t(std::max(s, e))));
                } catch (const std::exception &) {
                    cache[pid] = {};
                }
            }
            if (w.e == 0 && (!best || cache[pid].size() > std::get<0>(*best).size())) best = std::make_tuple(cache[pid], w.node, pid);
        }
        if (!best) continue;
        const auto &[L, node, pid] = *best;
        Facts f;
        if (const ORef bb = bg::res(p, node.first, node.second + 0x10)) {
            if (auto x = tn(bg::res(p, bb->first, bb->second + 0x10))) f.units.push_back({0, *x});
            for (int u = 1; u < 12; u++)
                if (auto x = tn(bg::res(p, bb->first, bb->second + 0x40 + 0x10ull * uint64_t(u)))) f.units.push_back({u, *x});
        }
        const auto U = tex_use(L);
        const auto sc = blend_source(L);
        bool zp = false;
        if (sc && sc->kind == "tex" && sc->chan == "z") {
            auto ui = U.find(*sc->unit);
            if (ui != U.end())
                for (const auto &[c, mk] : ui->second)
                    if (has(mk, "x") && has(mk, "y") && !has(mk, "w")) zp = true;
            bool dp = false;
            for (const auto &l : L) dp = dp || starts(l, "DP2") || starts(l, "DP3");
            zp = zp && dp;
        }
        f.zproof = zp, f.src = sc, f.use = U;
        auto pi = PAR.find(name);
        if (pi != PAR.end()) f.params = pi->second;
        out[name] = std::move(f);
    }
    return out;
}

// ---- rows (detect.py) and plan ------------------------------------------------------------------------------------
struct Row {
    std::string pak, mat;
    bool vc_only, blendmap, shared;
    std::optional<std::tuple<int, std::optional<std::string>, std::string, std::string>> src;   // unit, texture, chan, coord
    bg::Params ps3p;
    std::vector<std::tuple<int, std::string, std::optional<std::string>>> dropped;            // unit, texture, role
};
struct Job {
    std::string cls, mat, nrm, ovl;
    const Row *row;
};

std::vector<Row> rows_of(const std::string &pak, const std::vector<Mat4> &mats, const std::map<std::string, Facts> &F) {
    static const std::regex Z("missingtexture|indirect|lightmap", std::regex::icase);
    std::vector<Row> out;
    for (const Mat4 &m : mats) {
        const std::string key = m.base();
        const size_t c = key.rfind(':');
        const std::string pn = (c == std::string::npos ? "" : key.substr(0, c)) + "/" + (c == std::string::npos ? key : key.substr(c + 1));
        auto gi = F.find(pn);
        if (gi == F.end() || m.shader != "prime-shader.fx") continue;
        const Facts &g = gi->second;
        std::set<std::string> bn;
        for (const auto &b : m.binds) bn.insert(last_part(b.second));
        Row r;
        r.pak = pak, r.mat = key, r.ps3p = g.params;
        r.vc_only = has(m.feats, "BLEND_FACTOR_USE_VERTEX_COLOR_ONLY");
        r.blendmap = has(m.feats, "BLEND_FACTOR_USE_BLEND_MAP_AND_VERTEX_COLOR");
        r.shared = has(m.feats, "BLEND_FACTOR_USE_SHARED_TEXTURE_1_B");
        auto unit = [&](int u) -> std::optional<std::string> {
            for (const auto &[k, s] : g.units)
                if (k == u) return s;
            return std::nullopt;
        };
        if (g.src && g.src->kind == "tex") r.src = std::make_tuple(*g.src->unit, unit(*g.src->unit), g.src->chan, g.src->coord);
        for (const auto &[u, s] : g.units) {
            if (std::regex_search(s, Z) || bn.count(s)) continue;
            r.dropped.push_back({u, s, stage::tex_role_final("x/" + s)});
        }
        out.push_back(std::move(r));
    }
    return out;
}

// the materials that get a blend map from a normal map's blue channel: the PS3 program blends by the z channel of a
// texture (not the shared-texture blend), the normal is that texture when it is a normal map, else the material's
// Normal1, and the converted BlendMap is a flat generated mask. (A vertex-colour-only material is not one of them.)
std::vector<Job> plan(const std::vector<Row> &R, const std::vector<Mat4> &mats) {
    std::vector<Job> out;
    for (const Row &r : R) {
        const Mat4 *m = nullptr;
        for (const Mat4 &x : mats)
            if (x.base() == r.mat) {
                m = &x;
                break;
            }
        if (!m) throw Stop("StopIteration: " + r.mat);
        if (!r.src || std::get<2>(*r.src) != "z" || r.shared || r.vc_only) continue;
        std::optional<std::string> nrm;
        const auto &s1 = std::get<1>(*r.src);
        if (s1 && !s1->empty() && has(*s1, "normal")) nrm = *s1;
        if (!nrm)
            for (const auto &b : m->binds)
                if (has(b.first, "Normal1")) {
                    nrm = last_part(b.second);
                    break;
                }
        std::optional<std::string> blend;
        for (const auto &b : m->binds)
            if (has(b.first, "BlendMap")) {
                blend = b.second;
                break;
            }
        if (nrm && !nrm->empty() && blend && !blend->empty() && has(*blend, "generated/u3mp/flat")) out.push_back({"Zflat", r.mat, *nrm, "", &r});
    }
    return out;
}


// ---- textures -------------------------------------------------------------------------------------------------------
// DXT1 / DXT5 colour blocks -> DXT1 blocks whose colour is the grey of the blue channel (as retail '-blend' maps)
Bytes blueblocks(const Bytes &lin, int bpb) {
    const size_t n = lin.size() / size_t(bpb);
    Bytes out(n * 8);
    auto g = [](uint32_t c) { return (c & 31) << 11 | (((c & 31) << 1) | ((c & 31) >> 4)) << 5 | (c & 31); };
    for (size_t k = 0; k < n; k++) {
        const size_t o = k * size_t(bpb) + (bpb == 16 ? 8 : 0);
        const uint32_t c0 = uint32_t(lin[o] | lin[o + 1] << 8), c1 = uint32_t(lin[o + 2] | lin[o + 3] << 8);
        uint32_t ix = rd32le(&lin[o + 4]);
        uint32_t a = g(c0), b = g(c1);
        if (c0 > c1 || bpb == 16) {   // 4-colour block: a > b
            if (a < b) std::swap(a, b), ix ^= 0x55555555;
            else if (a == b) {
                if (a == 0) a = 1;
                else b = a - 1;
                ix = 0;
            }
        } else if (a > b) {   // 3-colour block: a <= b
            std::swap(a, b);
            uint32_t ni = 0;
            static const uint32_t MAP[4] = {1, 0, 2, 3};
            for (int q = 0; q < 16; q++) ni |= MAP[(ix >> (2 * q)) & 3] << (2 * q);
            ix = ni;
        }
        out[k * 8] = uint8_t(a), out[k * 8 + 1] = uint8_t(a >> 8), out[k * 8 + 2] = uint8_t(b), out[k * 8 + 3] = uint8_t(b >> 8);
        wr32le(&out[k * 8 + 4], ix);
    }
    return out;
}
// BC1 value-wise inversion (v -> 1 - v): 4-colour blocks exactly (endpoints NOT-ed and swapped, indices ^ 1), 3-colour
// blocks re-fitted as 4-colour blocks of the inverted colours (transparent black becomes white)
Bytes bc1_invert(const Bytes &blob) {
    Bytes out = blob;
    auto d565 = [](uint32_t c) { return std::array<int, 3>{int((c >> 11) & 31) * 255 / 31, int((c >> 5) & 63) * 255 / 63, int(c & 31) * 255 / 31}; };
    auto e565 = [](int r, int g, int b) { return uint32_t((r * 31 + 127) / 255) << 11 | uint32_t((g * 63 + 127) / 255) << 5 | uint32_t((b * 31 + 127) / 255); };
    for (size_t o = 0; o + 7 < blob.size(); o += 8) {
        const uint32_t c0 = uint32_t(blob[o] | blob[o + 1] << 8), c1 = uint32_t(blob[o + 2] | blob[o + 3] << 8), ix = rd32le(&blob[o + 4]);
        auto put = [&](uint32_t a, uint32_t b, uint32_t i) {
            out[o] = uint8_t(a), out[o + 1] = uint8_t(a >> 8), out[o + 2] = uint8_t(b), out[o + 3] = uint8_t(b >> 8);
            wr32le(&out[o + 4], i);
        };
        if (c0 > c1) {
            put(c1 ^ 0xffff, c0 ^ 0xffff, ix ^ 0x55555555);
            continue;
        }
        const auto p0 = d565(c0), p1 = d565(c1);
        std::array<std::array<int, 3>, 4> pal = {p0, p1, {(p0[0] + p1[0]) / 2, (p0[1] + p1[1]) / 2, (p0[2] + p1[2]) / 2}, {0, 0, 0}};
        std::array<std::array<int, 3>, 16> px;
        for (int k = 0; k < 16; k++) {
            const auto &c = pal[(ix >> (2 * k)) & 3];
            px[size_t(k)] = {255 - c[0], 255 - c[1], 255 - c[2]};
        }
        std::array<int, 3> lo = px[0], hi = px[0];
        for (const auto &c : px)
            for (int j = 0; j < 3; j++) lo[size_t(j)] = std::min(lo[size_t(j)], c[size_t(j)]), hi[size_t(j)] = std::max(hi[size_t(j)], c[size_t(j)]);
        uint32_t e0 = e565(hi[0], hi[1], hi[2]), e1 = e565(lo[0], lo[1], lo[2]);
        if (e0 == e1) {
            put(e0, e1, 0);
            continue;
        }
        if (e0 < e1) std::swap(e0, e1);
        const auto q0 = d565(e0), q1 = d565(e1);
        std::array<std::array<int, 3>, 4> q = {q0, q1, std::array<int, 3>{(2 * q0[0] + q1[0]) / 3, (2 * q0[1] + q1[1]) / 3, (2 * q0[2] + q1[2]) / 3},
                                               std::array<int, 3>{(q0[0] + 2 * q1[0]) / 3, (q0[1] + 2 * q1[1]) / 3, (q0[2] + 2 * q1[2]) / 3}};
        uint32_t ni = 0;
        for (int k = 0; k < 16; k++) {
            int best = 0, bd = -1;
            for (int i = 0; i < 4; i++) {
                int dd = 0;
                for (int j = 0; j < 3; j++) dd += (px[size_t(k)][size_t(j)] - q[size_t(i)][size_t(j)]) * (px[size_t(k)][size_t(j)] - q[size_t(i)][size_t(j)]);
                if (bd < 0 || dd < bd) bd = dd, best = i;
            }
            ni |= uint32_t(best) << (2 * k);
        }
        put(e0, e1, ni);
    }
    return out;
}

const PakTexture *ps3_tex(const std::vector<PakTexture> &T, const std::string &base_) {
    for (const PakTexture &t : T) {
        const std::string &s = t.path;
        const size_t e = s.rfind('/');
        if (e == std::string::npos) continue;
        const size_t b = s.rfind('/', e - 1);
        if (s.substr(b == std::string::npos ? 0 : b + 1, e - (b == std::string::npos ? 0 : b + 1)) == base_) return &t;
    }
    return nullptr;
}
struct Chain {
    Bytes lin;
    uint32_t w, h, m;
    std::string kind;
    int bpb;
};
Chain chain(const Pak &p, const PakTexture &t) {   // the mips from the first one within 256 pixels
    const auto f = tc::ps3_format(t);
    if (!f || !f->compressed || (f->kind != "DXT1" && f->kind != "DXT5")) throw Stop("ValueError: format " + (f ? f->kind : std::string("None")));
    Bytes d = p.vram_bytes(t.vramOffset, t.vramSize);
    uint32_t w = t.width, h = t.height, m = t.mips;
    size_t s = 0;
    while (std::max(w, h) > CAP && m > 1) s += tc::ps3_linear_size(w, h, 1, f->bpb), w = std::max(1u, w >> 1), h = std::max(1u, h >> 1), m--;
    const size_t e = std::min<size_t>(d.size(), s + tc::ps3_linear_size(w, h, m, f->bpb));
    return {Bytes(d.begin() + std::ptrdiff_t(std::min(s, d.size())), d.begin() + std::ptrdiff_t(std::max(e, std::min(s, d.size())))), w, h, m, f->kind, f->bpb};
}

}  // namespace

std::vector<nblend::Blend> nblend::normal_blue_blends(const Pak &ps3, const std::vector<Material> &mats) {
    std::vector<Job> jobs;
    std::vector<Row> rows;
    try {
        if (mats.empty()) return {};
        const std::map<std::string, Facts> F = facts(ps3);
        rows = rows_of("", mats, F);
        for (const Row &r : rows)
            for (Job j : plan(std::vector<Row>{r}, mats)) {
                j.row = &r;
                jobs.push_back(j);
            }
    } catch (const std::exception &) {   // PS3 programs or materials this cannot read: no blends
        return {};
    }
    const auto T3 = ps3.textures();
    std::vector<Blend> out;
    try {
        for (const Job &r : jobs) {
            size_t mi = mats.size();
            for (size_t k = 0; k < mats.size(); k++)
                if (mats[k].base() == r.mat) mi = k;   // the last material of that name
            if (mi == mats.size()) continue;
            const bool invert = has(mats[mi].feats, INV);
            try {
                const PakTexture *t3 = ps3_tex(T3, r.nrm);
                if (!t3) throw Stop("ValueError: no PS3 normal " + r.nrm);
                const Chain c = chain(ps3, *t3);
                Bytes gb = blueblocks(c.lin, c.bpb);
                if (invert) gb = bc1_invert(gb);
                out.push_back({mi, "art/generated/u3mp/zblend/" + r.nrm.substr(0, r.nrm.size() - 4) + (invert ? "-zblendinv.tga" : "-zblend.tga"),
                               tc::ps3_to_ps4_texels(gb, c.w, c.h, c.m, 8), c.w, c.h, c.m});
            } catch (const Stop &ex) {   // this material is left as converted
                if (!starts(ex.what(), "ValueError: ")) throw;
            }
        }
    } catch (const std::exception &) {   // the level is left as converted
        return {};
    }
    return out;
}

}  // namespace u3
