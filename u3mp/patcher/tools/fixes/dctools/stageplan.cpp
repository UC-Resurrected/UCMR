// dctools/stageplan.cpp: see stageplan.hpp.
#include "stageplan.hpp"

#include "inflate.h"   // launcher/native/inflate.h: gunzip, crc32
#include "proc.hpp"
#include "ps3fp.hpp"
#include "texcodec.hpp"
#include "u3fix.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <regex>
#include <set>
#include <unordered_map>

namespace u3::stage {

// ------------------------------------------------------------------------------------------------ names and roles
namespace {

bool is_hex16(const std::string &s, size_t a) {
    if (a + 16 > s.size()) return false;
    for (size_t i = a; i < a + 16; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'A' && s[i] <= 'F'))) return false;
    return true;
}

bool ends_with(const std::string &s, const std::string &e) { return s.size() >= e.size() && s.compare(s.size() - e.size(), e.size(), e) == 0; }
bool starts_with(const std::string &s, const std::string &e) { return s.compare(0, e.size(), e) == 0; }
bool has(const std::string &s, const char *x) { return s.find(x) != std::string::npos; }

std::string lower(std::string s) {
    for (char &c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}
// a material named as a shadow caster (shadow blocker, canopy shadow card): retail draws such a material black or into the
// shadow maps only (COLOR_1_BLACK / GLOBAL_SHADOW_PROXY, or a colourless tinted canopy card), so a shadow template is only
// for a PS3 shadow caster, and a PS3 shadow caster only takes a shadow template. A leaf canopy planned on the retail
// cha-beech-leaves-canopy-shadow card (no colour map, white tint) drew the Borneo co-op trees white; the Sanctuary void
// planes (cave-shadowblocker, black colour + lightmap) were planned on a lit floor
bool shadow_only_name(const std::string &s) {
    const std::string l = lower(s);
    return has(l, "shadowblocker") || has(l, "shadow-blocker") || has(l, "canopy-shadow");
}

std::vector<std::string> split(const std::string &s, char c) {
    std::vector<std::string> v;
    size_t a = 0;
    for (size_t i = 0;; i++)
        if (i == s.size() || s[i] == c) {
            v.push_back(s.substr(a, i - a));
            if (i == s.size()) break;
            a = i + 1;
        }
    return v;
}

bool rx(const std::string &s, const std::regex &r) { return std::regex_search(s, r); }

}  // namespace

std::string short_of(const std::string &full) {
    // re.search(r'texture[12]/(.*?)/[0-9A-F]{16}(\.ndb)?$'): the first 'texture1/' or 'texture2/' that the anchored
    // tail '/<16 hex>' or '/<16 hex>.ndb' follows
    size_t tail = 0;
    if (full.size() >= 21 && ends_with(full, ".ndb") && full[full.size() - 21] == '/' && is_hex16(full, full.size() - 20)) tail = 21;
    else if (full.size() >= 17 && full[full.size() - 17] == '/' && is_hex16(full, full.size() - 16)) tail = 17;
    if (!tail) return full;
    for (size_t i = full.find("texture"); i != std::string::npos; i = full.find("texture", i + 1)) {
        if (i + 9 > full.size() || (full[i + 7] != '1' && full[i + 7] != '2') || full[i + 8] != '/') continue;
        if (i + 9 + tail > full.size()) break;
        return full.substr(i + 9, full.size() - tail - (i + 9));
    }
    return full;
}

std::optional<std::string> tex_role(const std::string &shrt) {
    static const std::vector<std::pair<std::string, std::regex>> ROLE_RE = {
        {"normal", std::regex(R"(-normals?\b|-normal-|normal\.)")}, {"detail", std::regex("-detail")}, {"spec", std::regex("-spec")},
        {"mask", std::regex("-mask")}, {"blend", std::regex("-blend")}, {"trans", std::regex(R"(-trans|-alpha|-opacity)")},
        {"env", std::regex("refle|env")}, {"color", std::regex(R"(-color|-diffuse|-col\b)")}};
    const std::string s = lower(shrt);
    if (has(s, "lightmap") || has(s, "missingtexture") || has(s, ".atgi/") || has(s, "indirect.rgbmax")) return std::nullopt;
    for (const auto &[role, r] : ROLE_RE)
        if (rx(s, r)) return role;
    return std::string("color");
}

std::optional<std::string> tex_role_final(const std::string &shrt) {
    static const std::vector<std::pair<std::string, std::regex>> ROLE_FINAL_RE = {
        {"normal", std::regex(R"(-normals?(?=[-._]))")}, {"detail", std::regex(R"(-detail(?=[-._]))")}, {"spec", std::regex(R"(-spec(?=[-._]))")},
        {"mask", std::regex(R"(-mask(?=[-._]))")}, {"blend", std::regex(R"(-blend(?=[-._]))")},
        {"trans", std::regex(R"(-(trans\w*|alpha|opacity)(?=[-._]))")}, {"env", std::regex(R"((-env(?=[-._])|refle))")},
        {"color", std::regex(R"(-(color|diffuse|col)(?=[-._]))")}};
    static const std::regex COLOR_END(R"(-(color|diffuse|col)\.[a-z]+$)");
    if (!tex_role(shrt)) return std::nullopt;
    std::string b = lower(shrt);
    if (const size_t k = b.rfind('/'); k != std::string::npos) b = b.substr(k + 1);
    std::string role = "color";
    for (const auto &[r, re] : ROLE_FINAL_RE)
        if (rx(b, re)) {
            role = r;
            break;
        }
    if ((role == "blend" || role == "detail" || role == "env" || role == "mask") && rx(b, COLOR_END)) role = "color";
    return role;
}

std::optional<std::string> param_role(const std::string &p) {
    static const std::pair<const char *, const char *> PARAM_ROLE[] = {{"Normal", "normal"}, {"Detail", "detail"}, {"Mask", "mask"},
                                                                       {"Blend", "blend"}, {"Transparency", "trans"}, {"Environment", "env"},
                                                                       {"Color", "color"}, {"Spec", "spec"}, {"LightMap", "lightmap"}};
    for (const auto &[k, r] : PARAM_ROLE)
        if (has(p, k)) return std::string(r);
    return std::nullopt;
}

std::string ps4key(const std::string &n) {
    const size_t k = n.rfind('/');
    if (k == std::string::npos) {
        if (g_u2) return n;   // U2: facility-multi has one material group with an empty name, planned like any other
        throw Stop("ValueError: not enough values to unpack (expected 2, got 1)");
    }
    return n.substr(0, k) + ":" + n.substr(k + 1);
}

std::vector<std::pair<std::string, std::string>> overlay_pairs(const bg::Material3 &m, const std::vector<bg::Texture3> &texs) {
    std::vector<std::pair<std::string, std::string>> out;
    // (only an opaque material: a blended one's -trans map is its transparency, as the Ruins helicopter window's
    // ruins-window-break-color + -trans, whose colour map was taken for an overlay layer and left a flat grey colour slot)
    if (m.alpha_test != 0 || m.blend == 1) return out;
    static const std::regex HT(R"(/[0-9A-F]{16}\.ndb(\.error)?$)"), MASK(R"(^(.*)-(trans\w*|alpha|opacity)$)"), COL(R"(^(.*)-(color|diffuse|col)$)");
    auto stem = [](const std::string &s) {   // the file name without its extension, lower case
        std::string b = lower(s.substr(s.rfind('/') == std::string::npos ? 0 : s.rfind('/') + 1));
        return b.substr(0, b.rfind('.'));
    };
    std::vector<std::string> sh;
    for (uint32_t i : m.textures) sh.push_back(std::regex_replace(short_of(texs.at(i).name), HT, ""));
    for (const std::string &s : sh) {
        std::smatch mm;
        const std::string b = stem(s);
        if (tex_role_final(s) != "trans" || !std::regex_match(b, mm, MASK)) continue;
        const std::string key = mm[1].str();
        for (const std::string &t : sh) {
            if (t == s || tex_role_final(t) != "color") continue;
            std::string c = stem(t);
            std::smatch cm;
            if (std::regex_match(c, cm, COL)) c = cm[1].str();
            if (c == key) {
                out.push_back({t, s});
                break;
            }
        }
    }
    return out;
}

// ------------------------------------------------------------------------------------------------ survey (mkres)
namespace {

// one JSON value as json.dumps writes it (', ' / ': ' separators, non-ASCII as \uXXXX of the latin-1 text)
void jstr(std::string &o, const std::string &latin1) { json::quote(o, bg::utf8_of(latin1)); }

std::string gzip_stored(const std::string &data) {   // a gzip member with stored deflate blocks
    std::string o("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\xff", 10);
    size_t i = 0;
    do {
        const size_t n = std::min<size_t>(65535, data.size() - i);
        o += char(i + n == data.size() ? 1 : 0);
        o += char(n & 0xff);
        o += char(n >> 8);
        o += char(~n & 0xff);
        o += char((~n >> 8) & 0xff);
        o.append(data, i, n);
        i += n;
    } while (i < data.size());
    const uint32_t crc = inflate::crc32(0, reinterpret_cast<const unsigned char *>(data.data()), data.size()), sz = uint32_t(data.size());
    for (int k = 0; k < 4; k++) o += char(crc >> (8 * k));
    for (int k = 0; k < 4; k++) o += char(sz >> (8 * k));
    return o;
}

std::string survey_line(const Pak &q, const std::string &fn) {
    std::string o = "{\"pak\": ";
    jstr(o, fn);
    o += ", \"size\": " + std::to_string(q.d.size()) + ", \"nfx\": " + std::to_string(q.items_of("EFFECT").size()) + ", \"mats\": [";
    bool firstm = true;
    for (const PakItem &g : q.items_of("GEOMETRY_1")) {
        const uint32_t pg = g.page;
        const uint64_t b = g.data_off();
        const uint32_t flags = q.pu32(pg, b + 4);
        const uint64_t n = q.u64(q.pbase(pg) + b + 0x48);
        const ORef r = bg::res(q, pg, b + 0x50);
        if (!r) continue;
        const auto [mp, mo] = *r;
        for (uint64_t i = 0; i < n; i++) {
            const uint64_t o_ = mo + i * 0x48;
            auto S = [&](uint64_t off, std::string &out) {
                const ORef s = bg::res(q, mp, o_ + off);
                if (s) jstr(out, q.pstr(s->first, s->second));
                else out += "null";
            };
            const uint64_t mb = q.pbase(mp) + o_;
            const uint32_t nf = rd32le(q.d, mb + 0x30), nb = rd32le(q.d, mb + 0x38);
            rd32le(q.d, mb + 0x3c);
            std::string m = "{\"n\": ";
            S(0, m);
            m += ", \"s\": ";
            S(8, m);
            m += ", \"h\": ";
            S(0x18, m);
            m += ", \"f\": [";
            const ORef fr = bg::res(q, mp, o_ + 0x10);
            for (uint32_t k = 0; k < (fr ? nf : 0); k++) {
                const Ref s = bg::need(q, fr->first, fr->second + 8 * k);
                if (k) m += ", ";
                jstr(m, q.pstr(s.first, s.second));
            }
            m += "], \"b\": [";
            const ORef br = bg::res(q, mp, o_ + 0x28);
            for (uint32_t k = 0; k < (br ? nb : 0); k++) {
                const uint64_t bo = br->second + 0x28 * k;
                const Ref a = bg::need(q, br->first, bo), c = bg::need(q, br->first, bo + 8);
                if (k) m += ", ";
                m += "[";
                jstr(m, q.pstr(a.first, a.second));
                m += ", ";
                jstr(m, q.pstr(c.first, c.second));
                m += "]";
            }
            m += "], \"fl\": " + std::to_string(flags) + "}";
            if (!firstm) o += ", ";
            o += m;
            firstm = false;
        }
    }
    return o + "]}";
}

}  // namespace

void make_res(const fs::path &out, const fs::path &pak_dir) {
    std::vector<std::string> names;
    for (const auto &e : fs::directory_iterator(pak_dir)) {
        const std::string fn = utf8(e.path().filename());
        if (ends_with(fn, ".pak") && !ends_with(fn, g_u2 ? "-ingame10.pak" : "-ingame12.pak")) names.push_back(fn);   // level data paks: no materials
    }
    std::sort(names.begin(), names.end());
    std::string mats, tex;
    size_t read = 0;
    for (const std::string &fn : names) {
        try {
            const Pak q(pak_dir / upath(fn));
            mats += survey_line(q, fn) + "\n";
            read++;
            for (const PakTexture &x : q.textures())
                tex += fn + "\t" + bg::utf8_of(x.path) + "\t" + std::to_string(x.width) + "\t" + std::to_string(x.height) + "\t" + std::to_string(x.format) + "\t" +
                       std::to_string(x.mips) + "\t" + std::to_string(x.vramOffset) + "\t" + std::to_string(x.vramSize) + "\t0\t0\t0\r\n";
        } catch (const std::exception &ex) {
            std::string l = "{\"pak\": ";
            jstr(l, fn);
            l += ", \"err\": ";
            jstr(l, ex.what());
            mats += l + "}\n";
        }
    }
    // a survey without one readable retail pak has no templates: every converted level would fail with "no fallback
    // material", and a written survey would be reused by every later import
    if (!read) throw Stop("the PS4 game's level paks could not be read (" + utf8(pak_dir) + "): no material templates");
    fs::create_directories(out);
    const std::string gz = gzip_stored(mats);
    write_file_replace(out / "ps4mats.jsonl.gz", Bytes(gz.begin(), gz.end()));
    write_file_replace(out / "ps4tex.tsv", Bytes(tex.begin(), tex.end()));
}

// ------------------------------------------------------------------------------------------------ the survey, loaded
namespace {

std::string latin1_of(const std::string &u) {   // the UTF-8 text of the JSON survey back to the paks' latin-1 bytes
    std::string o;
    for (size_t i = 0; i < u.size(); i++) {
        const unsigned char c = (unsigned char)u[i];
        if (c >= 0xc2 && c <= 0xc3 && i + 1 < u.size()) {
            o += char(((c & 3) << 6) | ((unsigned char)u[i + 1] & 0x3f));
            i++;
        } else
            o += char(c);
    }
    return o;
}

struct SMat {
    std::string n, s, h;
    std::vector<std::string> f;
    std::vector<std::pair<std::string, std::string>> b;
    // derived (the fallback scoring)
    bool prime = false, water = false;
    bool u2bg = false;   // U2 fallback candidate: a background material (GEOMETRY_1 flag bit 0 clear), not hair / eye / skin
    unsigned r4 = 0, c4 = 0;   // role / class bits
    bool tmus1a = false, wind = false, leaf = false;
    size_t fsid = 0;   // feature set id
    // an overlay layer the template binds a colour and a control (mask) texture for: g_tNdFetchOverlay<d>Map and
    // g_tNdFetchOverlayControl<d>Map (0 = none); ovset2: it reads both from the second vertex UV set (PS4 counts sets
    // from 0: COLOR_d_OVERLAY_USE_UV2, COLOR_d_OVERLAY_CONTROL_TEXTURE_USE_UV2 and UV_2_USE_SET_1); ovvmask: the
    // overlay mask is scaled by the vertex colour (COLOR_d_VERTEX_OVERLAY_MASK); soft: BLEND_FACTOR_SOFTNESS
    int ovl = 0;
    bool ovset2 = false, ovvmask = false, soft = false;
    bool two = false;     // GLOBAL_2SIDED: the template draws both faces
    bool proxy = false;   // GLOBAL_SHADOW_PROXY: the template draws into the shadow maps only
    bool sun = false;     // DIFFUSE_LIGHT_ADD_SUNLIGHT: the template is lit by the sun
    unsigned uvs = 0;     // bit d: UV_d_APPLY_SCALE (the template scales layer d's UVs by g_uvScale<d>)
};
struct SEnt {
    std::string pak;
    uint64_t size;
    const SMat *m;
};
// the kind of surface a material path names, from its whole words (split at / - _ : .): water, sky, foliage, glass,
// cloth, decal, fx, prop, surface (walls / ground / rock), metal, wood, or '' (no kind word). The planner keeps a PS3
// material on a template of its own kind: desert-village's mud2diagonal-bricks wall was planned on a wooden crate,
// Borneo's grass on cargo-plane wires, its willow leaves on wires, its tree bark on a pub ceiling
std::string surface_kind(const std::string &path) {
    static const std::vector<std::pair<std::string, std::set<std::string>>> C = {
        {"water", {"water", "waterfall", "waterfalls", "ocean", "sea", "river", "puddle", "h2o", "foam", "falla", "pool"}},
        {"sky", {"sky", "skies", "cloud", "clouds"}},
        {"wood", {"bark", "log", "logs", "trunk", "trunks", "roots", "stump"}},   // tree trunks are solid wood, not cards
        {"foliage", {"foliage", "plant", "plants", "tree", "trees", "leaf", "leafs", "leaves", "grass", "vine", "vines", "ivy", "bush", "bushes", "fern", "ferns", "palm",
                     "moss", "shrub", "shrubs", "flower", "flowers", "branch", "branches", "willow", "banyan", "hedge", "weeds", "reeds"}},
        {"glass", {"glass", "window", "windows"}},
        {"cloth", {"fabric", "fabrics", "cloth", "tarp", "tarps", "curtain", "curtains", "canvas", "banner", "banners", "flag", "flags", "rug", "carpet", "awning", "tent",
                   "rope", "ropes", "cable", "cables", "wires", "wire", "net", "laundry"}},
        {"decal", {"decal", "decals", "graffiti", "poster", "posters", "sign", "signs", "grime", "stain", "stains", "leak", "leaks", "splat", "blood", "text", "names"}},
        {"fx", {"fx", "lightbeam", "godray", "glow", "flare", "smoke", "fire", "flame", "haze", "filament"}},
        {"prop", {"objects", "props", "prop", "crate", "crates", "container", "containers", "barrel", "barrels", "box", "boxes", "furniture", "chair", "table", "vehicles",
                  "vehicle", "truck", "pallet", "bottle", "bottles", "lamp", "basket", "generator", "car", "cars", "flatbed", "boxcar", "boat",
                  "tugboat", "tug", "galleon", "helicopter", "fj40", "raft", "pump"}},
        {"surface", {"wall", "walls", "ground", "floor", "floors", "roof", "roofs", "terrain", "rock", "rocks", "cliff", "stone", "stones", "brick", "bricks", "plaster",
                     "stucco", "concrete", "asphalt", "dirt", "mud", "sand", "road", "street", "streets", "tile", "tiles", "cobble", "stair", "stairs", "column",
                     "pillar", "arch", "ceiling", "building", "buildings", "brickwork", "stonework", "deck", "cement", "gravel", "cave", "tnl",
                     "gate", "gates", "door", "doors", "entry", "marble", "statue", "statues", "trim", "trimming", "trimmings", "molding",
                     "panels", "rubble", "mountain", "snow", "ice", "seawall", "cliffwall", "rockwall", "minaret", "tower", "fountain",
                     "basement", "basment", "hull"}},
        {"metal", {"metal", "steel", "iron", "rust", "pipe", "pipes", "rail", "rails", "fence", "grate", "grating", "corrugated", "handrail", "crane"}},
        {"wood", {"wood", "plank", "planks", "beam", "beams", "lumber", "timber"}}};
    std::vector<std::string> w;
    std::string cur;
    for (char ch : lower(path) + "/") {
        if (ch == '/' || ch == '-' || ch == '_' || ch == ':' || ch == '.' || ch == ' ') {
            if (!cur.empty()) w.push_back(cur);
            cur.clear();
        } else
            cur += ch;
    }
    for (int pass = 0; pass < 2; pass++)   // the last words (the material's own name) first, then its directories
        for (const auto &[c, ws] : C)
            for (size_t i = 0; i < w.size(); i++)
                if ((pass == 1 || i + 6 >= w.size()) && ws.count(w[i])) return c;
    return "";
}
// two kinds a template may not cross (the walls / ground, metal, wood, props and decals share materials freely)
bool kinds_clash(const std::string &a, const std::string &b) {
    if (a.empty() || b.empty() || a == b) return false;
    // (a prime-shader material named water is a wet floor / stream sheet with a water layer: any solid surface may carry it)
    if (a == "water" || b == "water") return false;
    static const std::set<std::string> SOLID = {"surface", "metal", "wood", "prop", "decal"};
    if (SOLID.count(a) && SOLID.count(b)) return false;
    if ((a == "cloth" && b == "prop") || (a == "prop" && b == "cloth") || (a == "fx" && b == "decal") || (a == "decal" && b == "fx")) return false;
    return true;
}

struct SKey {
    std::string k, kdir, kind;
    std::vector<std::string> kc;
    bool shadowblocker;
    std::vector<SEnt> ents;
};
// a feature set's facts for the v2 / v2.3 penalties
struct FeatSet {
    std::vector<std::string> feats;   // unique
    std::vector<int> bad;             // first BAD_FEATS weight per feature (0 = none)
    std::vector<bool> dist, incand, addenv;
    unsigned layers = 0;
    bool blend12 = false, normalmap = false, anydist = false, anyinc = false, blendfactor = false, uv2 = false, uv34 = false, vcol = false;
    bool border = false;
    bool specular = false, envspec = false;   // FINAL_COLOR_ADD_SPECULAR_LIGHT / SPECULAR_LIGHT_ADD_ENVIRONMENT
    bool tmap = false, talpha = false;   // the alpha from a transparency map (its own slot or shared texture 1); talpha: from the A channel of shared texture 1
    bool normal2 = false;   // a second normal map: layer 2's own (NORMAL_2_USE_NORMAL_MAP) or a detail normal
};
struct Survey {
    std::vector<SKey> keys;
    std::map<std::string, size_t> key_at;
    std::vector<std::unique_ptr<SMat>> mats;
    std::map<std::string, uint64_t> paksize;
    std::vector<FeatSet> fsets;
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> tex;   // short -> [(pak, full)]
};

const unsigned R_NORMAL = 1, R_DETAIL = 2, R_SPEC = 4, R_MASK = 8, R_BLEND = 16, R_TRANS = 32, R_ENV = 64, R_COLOR = 128;
unsigned role_bit(const std::string &r) {
    static const std::map<std::string, unsigned> m = {{"normal", R_NORMAL}, {"detail", R_DETAIL}, {"spec", R_SPEC}, {"mask", R_MASK},
                                                      {"blend", R_BLEND},   {"trans", R_TRANS},   {"env", R_ENV},   {"color", R_COLOR}};
    auto it = m.find(r);
    return it == m.end() ? 0 : it->second;
}
const unsigned C_CUT = 1, C_BLEND = 2, C_LEAF = 4;
int popc(unsigned v) { return __builtin_popcount(v); }

bool layer_of(const std::string &x, int &d) {   // re.match(r'COLOR_\d_', x) -> int(x.split('_')[1])
    if (x.size() >= 8 && starts_with(x, "COLOR_") && x[6] >= '0' && x[6] <= '9' && x[7] == '_') {
        d = x[6] - '0';
        return true;
    }
    return false;
}

FeatSet feat_set(const std::vector<std::string> &uniq) {
    // BAD_FEATS: (alternatives, weight); '_BLACK$' is the only anchored one
    static const std::vector<std::pair<std::vector<const char *>, int>> BAD = {
        {{"\x01_BLACK"}, 25}, {{"MASK_COLOR_MAP"}, 20}, {{"FACING_TINT"}, 12}, {{"BORDER_TINT"}, 12}, {{"_DSR_", "DIFFUSE_SUBSURFACE"}, 10},
        {{"DISTORTION"}, 15}, {{"CAUSTICS"}, 15}, {{"VERTEX_OVERLAY", "VERTEX_MASK", "SLOPE"}, 12}, {{"INSTANCE_TINT", "INSTANCE_OFFSET"}, 12},
        {{"TINT_WITH_MASK"}, 12}, {{"APPLY_OVERLAY_TEXTURE", "OVERLAY_CONTROL", "OVERLAY_MASK"}, 12},
        {{"TIME_SCROLL", "TILT_SCROLL", "UV_FLOW"}, 15}, {{"OFFSET_MAPPING"}, 6}, {{"PICKUP_SHIMMER"}, 30}, {{"SHADOW_PROXY"}, 30},
        {{"CLOTH", "SUBSURF"}, 10}, {{"RIPPLES"}, 15}, {{"PARTIALLY_CONVERTED"}, 3}, {{"GLASS_FRESNEL"}, 10}, {{"TRANSLUCEN"}, 6},
        {{"EARTH_CURVATURE", "SPHERIZE"}, 20}};
    FeatSet fs;
    fs.feats = uniq;
    for (const std::string &x : uniq) {
        int w = 0;
        for (const auto &[alts, wt] : BAD) {
            bool hit = false;
            for (const char *a : alts) hit = hit || (a[0] == '\x01' ? ends_with(x, a + 1) : has(x, a));
            if (hit) {
                w = wt;
                break;
            }
        }
        fs.bad.push_back(w);
        fs.dist.push_back(has(x, "DISTORTION"));
        fs.incand.push_back(has(x, "INCANDESCENCE"));
        fs.addenv.push_back(has(x, "ADD_ENVIRONMENT"));
        fs.specular = fs.specular || x == "FINAL_COLOR_ADD_SPECULAR_LIGHT";
        fs.envspec = fs.envspec || x == "SPECULAR_LIGHT_ADD_ENVIRONMENT";
        int d;
        if (layer_of(x, d)) fs.layers |= 1u << d;
        fs.blend12 = fs.blend12 || starts_with(x, "COLOR_1_BLEND_COLOR_2");
        fs.normalmap = fs.normalmap || has(x, "NORMAL_1_USE_NORMAL_MAP") || has(x, "NORMAL_MAP_USE_SHARED");
        fs.anydist = fs.anydist || has(x, "DISTORTION");
        fs.anyinc = fs.anyinc || has(x, "INCANDESCENCE");
        fs.blendfactor = fs.blendfactor || starts_with(x, "BLEND_FACTOR");
        // the vertex UV sets the template reads (PS4 counts them from 0): a layer's USE_UV<n> picks UV channel n, which is
        // vertex set 0 unless UV_<n>_USE_SET_<s> says otherwise. Set 1 needs the PS3 second UV set (attribute 0x12); set
        // 2 the stage never writes. (Retail twins agree: 492 of the single-set PS3 materials' twins read channel 2 or 4,
        // e.g. a layer 2 tiled by UV_2_APPLY_SCALE with COLOR_2_COLOR_MAP_USE_UV2, from set 0; 111 of 112 twins reading set
        // 1 have the PS3 second set.) Reading USE_UV2 / UV3 / UV4 as 'second / third set' kept every layer-tiling
        // template off the single-set PS3 materials.
        fs.uv2 = fs.uv2 || has(x, "_USE_SET_1");
        fs.uv34 = fs.uv34 || has(x, "_USE_SET_2");
        fs.vcol = fs.vcol || has(x, "VERTEX_COLOR") || has(x, "VERTEX_AMBIENT") || has(x, "VERTEX_OVERLAY") || has(x, "VERTEX_MASK") || has(x, "_VERTEX_");
        fs.border = fs.border || has(x, "BORDER_TINT");
        fs.tmap = fs.tmap || x == "TRANSPARENCY_USE_TRANSPARENCY_MAP" || starts_with(x, "TRANSPARENCY_MAP_USE_SHARED_TEXTURE_1_");
        fs.talpha = fs.talpha || x == "TRANSPARENCY_MAP_USE_SHARED_TEXTURE_1_A";
        fs.normal2 = fs.normal2 || x == "NORMAL_2_USE_NORMAL_MAP" || (starts_with(x, "NORMAL_") && has(x, "_DETAIL_MAP"));
    }
    return fs;
}

std::mutex g_survey_lock;
std::map<std::string, std::shared_ptr<Survey>> g_surveys;

std::shared_ptr<Survey> load_survey(const fs::path &res) {
    std::lock_guard<std::mutex> lock(g_survey_lock);
    const std::string key = utf8(res);
    if (auto it = g_surveys.find(key); it != g_surveys.end()) return it->second;
    auto S = std::make_shared<Survey>();
    const Bytes gz = read_file(res / "ps4mats.jsonl.gz");
    const std::string text = inflate::gunzip(std::string(gz.begin(), gz.end()));
    std::map<std::vector<std::string>, size_t> fset_at;
    size_t a = 0;
    while (a < text.size()) {
        size_t e = text.find('\n', a);
        if (e == std::string::npos) e = text.size();
        const std::string line = text.substr(a, e - a);
        a = e + 1;
        if (line.empty()) continue;
        json::Value d;
        if (!json::parse(line, d)) throw Stop("ps4mats.jsonl.gz: a line that is not JSON");
        const json::Value *mats = d.get("mats");
        if (!mats) continue;
        const std::string pak = latin1_of(d.get("pak")->s);
        const json::Value *sz = d.get("size");
        const uint64_t size = sz ? uint64_t(sz->i) : 0;
        for (const json::Value &m : mats->items) {
            auto sm = std::make_unique<SMat>();
            auto str = [&](const char *k) { const json::Value *v = m.get(k); return v && v->kind == json::Value::String ? latin1_of(v->s) : std::string(); };
            sm->n = str("n");
            sm->s = str("s");
            sm->h = str("h");
            if (const json::Value *f = m.get("f"))
                for (const json::Value &x : f->items) sm->f.push_back(latin1_of(x.s));
            if (const json::Value *b = m.get("b"))
                for (const json::Value &x : b->items) sm->b.push_back({latin1_of(x.items.at(0).s), latin1_of(x.items.at(1).s)});
            sm->prime = sm->s == "prime-shader.fx";
            sm->water = sm->s == "water.fx";
            const json::Value *fl = m.get("fl");
            sm->u2bg = !(fl && (fl->i & 1)) && sm->s != "hair.fx" && sm->s != "eye.fx" && sm->s != "skin.fx";
            for (const auto &bd : sm->b)
                if (auto r = param_role(bd.first); r && *r != "lightmap" && *r != "env") sm->r4 |= role_bit(*r);
            std::set<std::string> fset(sm->f.begin(), sm->f.end());
            if (fset.count("TRANSPARENCY_CUTOFF")) sm->c4 |= C_CUT;
            if (fset.count("TRANSPARENCY_ALPHA_BLEND")) sm->c4 |= C_BLEND;
            if (fset.count("GLOBAL_LEAF")) sm->c4 |= C_LEAF;
            sm->leaf = fset.count("GLOBAL_LEAF") != 0;
            sm->two = fset.count("GLOBAL_2SIDED") != 0;
            sm->proxy = fset.count("GLOBAL_SHADOW_PROXY") != 0;
            sm->sun = fset.count("DIFFUSE_LIGHT_ADD_SUNLIGHT") != 0;
            // (layer 1 reads UV channel 1; layer d > 1 reads channel d only with COLOR_d_COLOR_MAP_USE_UVd, as retail's
            // twins of PS3 layers with their own g_uvScale<d> do)
            for (int d = 1; d <= 4; d++) {
                const std::string ds = std::to_string(d);
                // (the converter writes the PS3 g_uvScale<d> into the template's g_uvScale<d>: it scales layer d only when layer
                // d reads channel d; Fort's cream plaster (g_uvScale1 2) landed on a layer 1 read through channel 3)
                bool other = false;
                for (int e = 2; e <= 4; e++) other = other || (e != d && fset.count("COLOR_" + ds + "_COLOR_MAP_USE_UV" + std::to_string(e)));
                if (fset.count("UV_" + ds + "_APPLY_SCALE") && !other && (d == 1 || fset.count("COLOR_" + ds + "_COLOR_MAP_USE_UV" + ds))) sm->uvs |= 1u << d;
            }
            sm->tmus1a = fset.count("TRANSPARENCY_MAP_USE_SHARED_TEXTURE_1_A") != 0;
            for (const std::string &x : fset)
                for (const char *w : {"GLOBAL_PIVOT_WIND", "BP_TREE_ANIM", "ND_TREE_ANIM"}) sm->wind = sm->wind || has(x, w);
            // (only a template whose one overlay is a diffuse layer 1..3: a second overlay or a spec / mask layer overlay
            // would keep the donor's own overlay texture)
            int novl = 0;
            for (const std::string &x : fset) novl += int(starts_with(x, "COLOR_") && ends_with(x, "_APPLY_OVERLAY_TEXTURE"));
            for (int d = 1; d <= 3 && novl == 1 && !sm->ovl; d++) {
                const std::string ov = "g_tNdFetchOverlay" + std::to_string(d) + "Map", oc = "g_tNdFetchOverlayControl" + std::to_string(d) + "Map";
                bool bo = false, bc = false;
                for (const auto &bd : sm->b) {
                    bo = bo || bd.first == ov;
                    bc = bc || bd.first == oc;
                }
                if (!bo || !bc) continue;
                sm->ovl = d;
                const std::string c = "COLOR_" + std::to_string(d) + "_OVERLAY_";
                sm->ovset2 = fset.count(c + "USE_UV2") && fset.count(c + "CONTROL_TEXTURE_USE_UV2") && fset.count("UV_2_USE_SET_1");
                sm->ovvmask = fset.count("COLOR_" + std::to_string(d) + "_VERTEX_OVERLAY_MASK") != 0;
            }
            sm->soft = fset.count("BLEND_FACTOR_SOFTNESS") != 0;
            std::vector<std::string> uniq(fset.begin(), fset.end());
            auto fi = fset_at.find(uniq);
            if (fi == fset_at.end()) {
                fi = fset_at.emplace(uniq, S->fsets.size()).first;
                S->fsets.push_back(feat_set(uniq));
            }
            sm->fsid = fi->second;
            const std::string k = split(sm->n, ',')[0];
            auto ki = S->key_at.find(k);
            if (ki == S->key_at.end()) {
                ki = S->key_at.emplace(k, S->keys.size()).first;
                SKey sk;
                sk.k = k;
                sk.kdir = split(k, ':')[0];
                sk.kc = split(sk.kdir, '/');
                sk.kind = surface_kind(k);
                sk.shadowblocker = shadow_only_name(k);
                S->keys.push_back(std::move(sk));
            }
            S->keys[ki->second].ents.push_back({pak, size, sm.get()});
            S->paksize[pak] = size;
            S->mats.push_back(std::move(sm));
        }
    }
    const Bytes tsv = read_file(res / "ps4tex.tsv");
    const std::string t(tsv.begin(), tsv.end());
    a = 0;
    while (a < t.size()) {
        size_t e = t.find('\n', a);
        if (e == std::string::npos) e = t.size();
        std::string line = t.substr(a, e - a);
        a = e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::vector<std::string> f = split(line, '\t');
        if (f.size() < 11) continue;
        const std::string full = latin1_of(f[1]);
        S->tex[short_of(full)].push_back({latin1_of(f[0]), full});
    }
    g_surveys[key] = S;
    return S;
}

// ------------------------------------------------------------------------------------------------ PS3 side of the plan
std::set<uint32_t> ps3_alpha_textures(const Pak &p) {   // PS3 DXT1 textures with > 5 % punch-through blocks in the top mip
    std::set<uint32_t> out;
    for (const PakTexture &t : p.textures()) {
        const auto f = tc::ps3_format(t);
        if (!f || !f->compressed) continue;
        const uint64_t bw = (t.width + 3) / 4, bh = (t.height + 3) / 4;
        const Bytes d = p.vram_bytes(t.vramOffset, bw * bh * f->bpb);
        if (f->kind != "DXT1") continue;
        if (d.size() % 8) throw Stop("ValueError: cannot reshape array");
        const size_t n = d.size() / 8;
        size_t hit = 0;
        for (size_t i = 0; i < n; i++) {
            const uint8_t *b = &d[i * 8];
            const uint32_t c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
            const uint32_t idx = uint32_t(b[4]) | uint32_t(b[5]) << 8 | uint32_t(b[6]) << 16 | uint32_t(b[7]) << 24;
            bool has3 = false;
            for (int k = 0; k < 16; k++) has3 = has3 || ((idx >> (2 * k)) & 3) == 3;
            if (c0 <= c1 && has3) hit++;
        }
        if (n && double(hit) / double(n) > 0.05) out.insert(t.hash);
    }
    return out;
}

// the mean |normal y| of a PS3 material's background vertices (attribute 2), -1 without normals: a waterfall sheet stands
// (well under 0.5), a river, pool or ocean surface lies (near 1)
double ps3_material_upness(const Pak &p, const std::vector<bg::Proto> &C, const std::string &mat) {
    double sum = 0;
    size_t n = 0;
    for (const bg::Proto &c : C)
        for (size_t k = 0; k < c.blocks.size(); k++) {
            if (!c.mats[k] || *c.mats[k] != mat) continue;
            try {
                const auto [pg, bo] = c.blocks[k];
                const Ref cnt = bg::need(p, pg, bo + 0x24), f = bg::need(p, pg, bo + 0x28), s = bg::need(p, pg, bo);
                const uint32_t nv = p.pu32(cnt.first, cnt.second);
                const std::vector<bg::Attr> attrs = bg::read_format(p, f.first, f.second);
                const std::map<int, int> prec = bg::block_prec(attrs);
                for (size_t ai = 0; ai < attrs.size(); ai++) {
                    if (attrs[ai].id != 2 || attrs[ai].n() < 2) continue;
                    const ORef r = bg::res(p, s.first, s.second + 8 * ai);
                    if (!r) continue;
                    auto pi = prec.find(2);
                    const Mat v = bg::decode_attr(p, f.first, f.second, attrs[ai], r->first, r->second, nv, pi == prec.end() ? -1 : pi->second);
                    for (size_t i = 0; i < v.rows; i++) {
                        sum += std::fabs(v.at(i, 1));
                        n++;
                    }
                    break;
                }
            } catch (const std::exception &) {
            }
        }
    return n ? sum / double(n) : -1.0;
}

std::map<std::string, std::set<int>> ps3_material_attrs(const Pak &p) {
    std::map<std::string, std::set<int>> out;
    std::vector<bg::Proto> C;
    try {
        C = bg::collection(p);
    } catch (const std::exception &) {
        return out;
    }
    for (const bg::Proto &c : C)
        for (size_t k = 0; k < c.blocks.size(); k++) {
            const std::string m = c.mats[k] ? *c.mats[k] : std::string("\x01None");
            auto &s = out[m];
            try {
                const Ref f = bg::need(p, c.blocks[k].first, c.blocks[k].second + 0x28);
                for (const bg::Attr &a : bg::read_format(p, f.first, f.second)) s.insert(a.id);
            } catch (const std::exception &) {
            }
        }
    return out;
}

bool colp(const std::string &k, int &d) {   // re.match(r'g_color(Tint|Brightness)(\d)$', k)
    for (const char *pre : {"g_colorTint", "g_colorBrightness"}) {
        const size_t n = std::strlen(pre);
        if (k.size() == n + 1 && starts_with(k, pre) && k[n] >= '0' && k[n] <= '9') {
            d = k[n] - '0';
            return true;
        }
    }
    return false;
}
bool uvp(const std::string &k, int &d) {   // re.match(r'g_uvScale(\d)$', k)
    if (k.size() == 10 && starts_with(k, "g_uvScale") && k[9] >= '0' && k[9] <= '9') {
        d = k[9] - '0';
        return true;
    }
    return false;
}

// the per-material facts of the fallback scoring
struct M3 {
    std::string n, low, fdir, top;
    std::vector<std::string> ncomp;
    unsigned r3 = 0, c3 = 0;
    std::set<int> attrs3;
    bool inc3 = false, dis3 = false, emissive = false, two3 = false, nrm3 = false;
    bool rstate = false;   // U3: the render class comes from the PS3 render state (alpha test / blend bits)
    std::vector<std::string> scr3;
    std::vector<std::string> strong;   // ps3_strong, the keys
    size_t ns3 = 0;                    // PS3 constants minus the generic ones
    bool ov = false;                   // the PS3 material has an overlay layer (overlay_pairs)
    bool soft3 = false;                // the PS3 material sets g_blendSoftness_blendOffset_linearBlendOffset
    bool softblend3 = false;           // ... and its program blends the layers softly (see soft_blend_score)
    bool spec3 = false, env3 = false;  // the PS3 program adds specular light (g_specularBrightness > 0); and an env map
                                       // reflection (an env texture and g_specularEnvironmentStrength > 0)
    bool fres3 = false;                // the PS3 material sets g_fresnel* constants (the PS3 glass programs)
    bool nrm2 = false;                 // the PS3 material has two normal maps of its own (one per layer)
    int two_sided = -1;                // the PS3 pass state draws both faces (1), culls (0), unknown (-1)
    unsigned uvfar = 0;                // bit d: g_uvScale<d> at least 1.5x away from 1 (a tiling only a UV_d scale carries)
    std::unordered_map<size_t, int> cache;   // feature set id -> v2 + v2.3 score
};

// U3: a PS3 layer blend with a softness (the program divides the blend-map term by 2s + 0.01, s the material's
// g_blendSoftness: a soft ramp between the layers) on a template without BLEND_FACTOR_SOFTNESS draws the layer edge as a
// hard step: the blend map shows as black-and-white blotches (Facility's frozen ship hull and wall / floor wear). Retail
// keeps the softness on 78% of its blend-map + vertex-colour templates; a hard-edge PS3 program (fp::hard_blend) is
// not asked for one (ps4stage sets its softness to 0)
int soft_blend_score(const M3 &m, const SMat &sm) { return m.softblend3 && !sm.soft ? -14 : 0; }
// U3: an alpha-tested or blending PS3 material whose alpha is its own '-trans' map, on a template without a
// transparency-map slot (it cuts by the colour map's alpha, and the PS3 colour maps of such materials are opaque BC1):
// the cut-out parts draw solid (Facility's gate lattice, wooden lattices and grates)
int trans_map_score(const M3 &m, const SMat &sm, const FeatSet &f) {
    if (g_u2 || !(m.r3 & R_TRANS) || !(m.c3 & (C_CUT | C_BLEND))) return 0;
    return (sm.r4 & R_TRANS) || f.tmap ? 0 : -30;
}

int v2_v23(const M3 &m, const FeatSet &f) {
    int pen = 0;
    const bool blend3 = (m.r3 & R_BLEND) || m.two3;
    if (m.two3 && !f.blend12) pen -= 45;
    if (m.nrm3 && !f.normalmap) pen -= 15;
    // U3: a PS3 material with two normal maps (one per blended layer) on a template with one normal slot loses the second
    // (ps4stage binds the PS3 normals to Normal1, Normal2 / a detail normal in order)
    if (!g_u2 && m.nrm2 && m.scr3.empty() && !f.normal2) pen -= 15;   // (a moving material keeps its scroll first)
    for (const std::string &need : m.scr3) {
        bool any = false;
        for (const std::string &x : f.feats) any = any || starts_with(x, need);
        if (!any) pen -= 15;
    }
    if (!m.scr3.empty()) {   // a moving PS3 surface (scroll / flow) on a template that moves none of it would stand still
        bool carried = false;
        for (const std::string &need : m.scr3)   // a template reading UV set 3 or 4 (the stage writes two) cannot move it right
            for (const std::string &x : f.feats) carried = carried || (!f.uv34 && starts_with(x, need));
        if (!carried) pen -= 40;
    }
    if (m.dis3 && !f.anydist) pen -= 15;
    if (m.inc3 && !f.anyinc) pen -= 15;
    // the PS3 program's specular term: a PS3 material that sets g_specularBrightness above 0 draws specular light (sun,
    // ambient spec, and with g_specularEnvironmentStrength above 0 and an env texture the env map reflection). A template
    // without FINAL_COLOR_ADD_SPECULAR_LIGHT drops it and with it the PS3 constants that set it: Museum lost it on 117 of
    // 211 materials (copper roofs drawn as their orange base instead of the teal-reflecting metal, brass lanterns dark,
    // marble without its sheen).
    if (m.spec3 && !f.specular) pen -= 30;
    if (m.spec3 && m.env3 && (m.r3 & R_ENV) && !f.envspec) pen -= 10;
    unsigned allowed = blend3 ? 6u : 2u;   // {1, 2} / {1}
    // U3: layers 4..6 are the specular colour layers: retail puts its '-spec-mask' maps in their mask slots (Mask4 6275 of
    // the 9986 retail spec bindings, Mask5 1133, Color4 1009, Mask6 549). For a PS3 material with a spec map, which
    // ps4stage binds to a template mask slot, they are not foreign: penalised, its spec map was dropped (Facility: 7 of its
    // 176 materials kept a spec-mask layer, now 53)
    if (!g_u2 && (m.r3 & R_SPEC) && m.scr3.empty()) allowed |= 0x70u;   // (a moving material keeps its scroll first)
    if (m.ov)   // a PS3 overlay layer: the template's overlay layers and features are needed, not foreign
        for (const std::string &x : f.feats) {
            int d;
            if (layer_of(x, d) && ends_with(x, "_APPLY_OVERLAY_TEXTURE")) allowed |= 1u << d;
        }
    pen -= 10 * popc(f.layers & ~allowed);
    if (blend3 && !(f.layers & 4)) pen -= 10;
    if (!blend3 && f.blendfactor) pen -= 10;
    // a blending PS3 glass program (g_fresnel* constants: alpha = a transparency texture channel plus the fresnel term) on
    // a template that takes its alpha from the colour map alpha draws opaque where that is solid (Museum's dark-grey
    // display-case glass drew as a flat grey wall): a template with a transparency map slot, which gets the PS3 channel.
    // Retail's glass-fresnel program (FINAL_COLOR_GLASS_FRESNEL) stays foreign: on these PS3 constants it drew the Museum
    // cases solid black in the owner's test (2026-11-16)
    if (m.fres3 && (m.c3 & C_BLEND) && (!f.tmap || f.talpha)) pen -= 30;
    for (size_t i = 0; i < f.feats.size(); i++) {
        if (f.bad[i]) {
            bool skip = (m.dis3 && f.dist[i]) || (m.ov && has(f.feats[i], "OVERLAY"));
            for (const std::string &need : m.scr3) skip = skip || starts_with(f.feats[i], need);
            if (!skip) pen -= f.bad[i];
        }
        if (f.incand[i] && !m.emissive) pen -= 15;
        // (U3: retail adds an environment reflection exactly where the PS3 material has an env map: 4429 of 4454
        // exact twins without one have none. A wet-metal template on Fort's cream plaster drew blue-white mirror slabs)
        if (f.addenv[i] && !(m.r3 & R_ENV)) pen -= g_u2 ? 4 : 20;
    }
    if (!m.attrs3.empty()) {
        if (f.uv2 && !m.attrs3.count(0x12)) pen -= 40;
        if (f.uv34) pen -= 40;
        if (f.vcol && !m.attrs3.count(0x15)) pen -= 40;
    }
    // v2.3: strong PS3 constants the template cannot carry
    if (!m.strong.empty()) {
        int sc = 0;
        unsigned proven = 0;
        bool border_st = false;
        for (const std::string &k : m.strong) {
            int d;
            std::string need;
            if (colp(k, d)) {
                need = "COLOR_" + std::to_string(d) + "_APPLY_COLOR_TINT";
                proven |= 1u << d;
            } else if (uvp(k, d))
                need = "UV_" + std::to_string(d) + "_APPLY_SCALE";
            else
                need = "BORDER_TINT";
            if (k == "g_diffuseColorBorderTint") border_st = true;
            bool carried = false;
            for (const std::string &x : f.feats) carried = carried || has(x, need.c_str());
            if (!carried) sc -= k == "g_diffuseColorBorderTint" ? 6 : 12;
        }
        sc += 10 * popc(f.layers & ~6u & proven);
        if (border_st && f.border) sc += 12;
        pen += sc;
    }
    return pen;
}

// waterscore.py: a retail water.fx template against a PS3 water material: constant names (packed names split),
// texture roles (diffuse colour, diffuse mask, foam, env cube, shadow map), a water program
std::set<std::string> split_names(const std::vector<std::string> &names) {
    static const std::regex PART("^[a-z][A-Z0-9]");
    std::set<std::string> out;
    for (const std::string &k : names) {
        std::vector<std::string> parts;
        if (starts_with(k, "g_")) parts = split(k.substr(2), '_');
        bool all = parts.size() > 1;
        for (const std::string &s : parts) all = all && std::regex_search(s, PART);
        if (all)
            for (const std::string &s : parts) out.insert("g_" + s);
        else
            out.insert(k);
    }
    return out;
}

// U3: a PS3 water program reflects the material's own env cube (it samples the '-env-reflection' .dds with the reflected
// view vector and adds it to the refraction); a water.fx template without an env-cube slot (g_tNdFetchEnvCubeMap)
// drops that reflection, and the surface draws as its dark refraction colour (Museum's fountain basin: flat black).
// Such a PS3 material takes a template with the slot first (ps4stage binds the PS3 cube to it)
bool water_env_ok(const std::vector<std::string> &texnames, const std::set<std::string> &tbinds) {
    if (g_u2) return true;
    bool env3 = false;
    for (const std::string &n0 : texnames) {
        const std::string n = lower(n0);
        env3 = env3 || (ends_with(n, ".dds") && has(n, "env"));
    }
    return !env3 || tbinds.count("g_tNdFetchEnvCubeMap") != 0;
}
// a PS3 water material with a diffuse colour layer (the algae of Oasis' and Dig's pools: a '<stem>-color' map with its
// '<stem>-mask') keeps a template with g_tDiffuseColorMap before the env cube (no retail water template has both; the
// colour layer is the larger visible part). Other '-color' maps in a water material's list (Syria's puzzle-room water
// lists wall colours it does not draw) are no diffuse layer
bool water_diffuse_ok(const std::vector<std::string> &texnames, const std::set<std::string> &tbinds) {
    std::set<std::string> col, msk;
    for (const std::string &n0 : texnames) {
        const std::string n = lower(n0);
        if (has(n, "foam")) continue;
        if (const size_t k = n.rfind("-color."); k != std::string::npos) col.insert(n.substr(0, k));
        if (const size_t k = n.rfind("-mask."); k != std::string::npos) msk.insert(n.substr(0, k));
    }
    bool d3 = false;
    for (const std::string &s : col) d3 = d3 || msk.count(s);
    return d3 == (tbinds.count("g_tDiffuseColorMap") != 0);
}
// U3: the class of a WATER_DATA surface kind: 4 / 5 mesh surfaces, 7 / 8 grid surfaces, 9 displacement. A template
// retail draws on another kind of the same class fits the surface (Museum's fountain basins are kind 4, and the two
// retail kind-4 templates have no env cube: a kind-5 mesh template carries it); a grid program on a mesh does not
int water_kind_class(int64_t kind) { return kind == 4 || kind == 5 ? 1 : kind == 7 || kind == 8 ? 2 : kind == 9 ? 3 : 0; }
bool water_class_ok(int64_t kind3, const json::Value *tkind) {
    return !g_u2 && tkind && water_kind_class(kind3) != 0 && water_kind_class(kind3) == water_kind_class(tkind->i);
}

int water_score(const std::vector<std::string> &p3names, const std::vector<std::string> &texnames, const std::vector<std::string> &tparams,
                const std::set<std::string> &tbinds, const std::vector<std::string> &tfeats) {
    const std::set<std::string> P = split_names(p3names), T = split_names(tparams);
    int both = 0;
    for (const std::string &x : P) both += int(T.count(x));
    int s = 2 * both - int(T.size() - both) - int(P.size() - both);
    static const std::regex DIFF(R"(-(color|diffuse|col)\.)"), DMASK(R"(-(mask|trans\w*|alpha)\.)");
    bool ti[5] = {false, false, false, false, false};   // diff, dmask, foam, env, shadow
    for (const std::string &n0 : texnames) {
        if (has(n0, "missingtexture")) continue;
        const std::string n = lower(n0);
        ti[2] = ti[2] || has(n, "foam");
        ti[0] = ti[0] || (std::regex_search(n, DIFF) && !has(n, "foam"));
        ti[1] = ti[1] || (std::regex_search(n, DMASK) && !has(n, "foam") && !has(n, "default-mask") && !has(n, "shadow"));
        ti[3] = ti[3] || ends_with(n, ".dds");
        ti[4] = ti[4] || has(n, "shadow");
    }
    const std::set<std::string> f(tfeats.begin(), tfeats.end());
    const bool tt[5] = {tbinds.count("g_tDiffuseColorMap") != 0, tbinds.count("g_tNdFetchDiffuseMaskMap") != 0, f.count("ND_FOAM") != 0,
                        f.count("ND_SPECULAR_ENVMAP") != 0, f.count("ND_WATER_TEXTURE_SHADOWS") != 0};
    const int wt[5] = {8, 5, 4, 3, 2};
    for (int k = 0; k < 5; k++) s += ti[k] == tt[k] ? wt[k] : -wt[k];
    if (!f.count("ND_WATER_REFRACTIONS")) s -= 100;   // not a water surface program
    return s;
}

// a PS3 material of another shader family than prime-shader: most of its constants are 'g_f' / 'g_c' / 'g_v' + upper
// case (the water, waterfall and old surface shaders) or have no 'g_' at all (the noodler shaders: 'TextureA_ScrollRateV')
bool other_family(const bg::Params &ps) {
    size_t n = 0, other = 0;
    for (const auto &[k, v] : ps) {
        if (k == "_Default_Maya_Color_") continue;
        n++;
        const bool hung = k.size() > 3 && k.compare(0, 2, "g_") == 0 && (k[2] == 'f' || k[2] == 'c' || k[2] == 'v') && std::isupper((unsigned char)k[3]);
        if (hung || k.compare(0, 2, "g_") != 0) other++;
    }
    return other >= 3 && 2 * other >= n;
}

// the constant names of the retail materials of one pak that it draws on geometry (GEOMETRY_1 materials: name ->
// params), read once per pak. A material only a water surface uses (WATER_DATA: its submesh is the one-vertex dummy) is
// left out: its program (the water pass's h2o.fx / water.fx) does not draw background geometry, e.g. the retail
// cha-forest-sink-hole-waterfalls h2o.fx; the h2o.fx materials retail draws on geometry carry the lighting /
// transparency features of the geometry pass.
std::mutex g_rparams_lock;
std::map<std::string, std::shared_ptr<std::map<std::string, std::vector<std::string>>>> g_rparams;
std::shared_ptr<std::map<std::string, std::vector<std::string>>> retail_params(const fs::path &pak_dir, const std::string &pak) {
    {
        std::lock_guard<std::mutex> l(g_rparams_lock);
        if (auto it = g_rparams.find(pak); it != g_rparams.end()) return it->second;
    }
    auto out = std::make_shared<std::map<std::string, std::vector<std::string>>>();
    try {
        const Pak q(pak_dir / upath(pak));
        const auto gs = q.items_of("GEOMETRY_1");
        if (!gs.empty()) {
            const uint32_t pg = gs[0].page;
            const uint64_t b = gs[0].data_off();
            auto S = [&](uint32_t a, uint64_t o) {
                const ORef r = bg::res(q, a, o);
                return r ? q.pstr(r->first, r->second) : std::string();
            };
            std::set<std::string> drawn;   // the materials of submeshes with more than one vertex
            const uint64_t nsub = q.u64(q.pbase(pg) + b + 8);
            if (const ORef sr = bg::res(q, pg, b + 0x10))
                for (uint64_t i = 0; i < nsub; i++)
                    if (q.pu32(sr->first, sr->second + 0x88 * i + 0x20) > 1) drawn.insert(S(sr->first, sr->second + 0x88 * i + 0x48));
            const uint64_t nmat = q.u64(q.pbase(pg) + b + 0x48);
            if (const ORef mr = bg::res(q, pg, b + 0x50))
                for (uint64_t i = 0; i < nmat; i++) {
                    const uint64_t mo = mr->second + 0x48 * i;
                    if (!drawn.count(S(mr->first, mo))) continue;
                    std::vector<std::string> ps;
                    const uint32_t npar = q.pu32(mr->first, mo + 0x34);
                    if (const ORef pr = bg::res(q, mr->first, mo + 0x20))
                        for (uint32_t k = 0; k < npar; k++) ps.push_back(S(pr->first, pr->second + 0x18 * k));
                    (*out)[S(mr->first, mo)] = ps;
                }
        }
    } catch (const std::exception &) {
    }
    std::lock_guard<std::mutex> l(g_rparams_lock);
    g_rparams[pak] = out;
    return out;
}
}  // namespace

const PlanMat *Plan::get(const std::string &n) const {
    for (const auto &e : mats)
        if (e.first == n) return &e.second;
    return nullptr;
}

Plan make_plan(const fs::path &ps3path, const fs::path &res, const fs::path &water_templates, const std::vector<std::string> &also_exclude, const fs::path &pak_dir,
               const std::map<std::string, std::string> &pins) {
    std::optional<json::Value> wtpl;
    // (no "reed": retail draws its reed mats, thatch and reed plants with non-leaf templates; a GLOBAL_LEAF template
    // sways, and Desert Village's still PS3 thatch moved on one)
    static const std::regex FOLIAGE("plant|leaf|leaves|vine|card(?!board)|grass|moss|brom|bush|fern|ivy|palm-frond|frond|shrub|weed|"
                                    "flower|hedge|branch|twig|canopy|willow|banyan");
    static const std::regex GROUNDISH("floor|tile|ground|terrain|road|path|stone|rock|brick|wall|concrete|dirt|sand|mud|plaster|"
                                      "pavement|cobble|street|roof|step|stair|trim|wood-plank|planks|metal|marble|tarmac|gravel");
    static const std::regex BLENDNAME(R"(glass|window|-trans\b)");
    const Pak p(ps3path);
    Plan plan;
    std::string stage = utf8(ps3path.filename());
    stage = stage.substr(0, stage.size() - 4);
    plan.stage = stage;
    std::set<std::string> exclude(also_exclude.begin(), also_exclude.end());   // and the stage itself
    exclude.insert(stage + ".pak");
    const std::shared_ptr<Survey> S = load_survey(res);
    const std::vector<bg::Proto> C = bg::collection(p);
    std::vector<std::string> names;
    auto add_name = [&](const std::string &n) {
        if (std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
    };
    for (const bg::Proto &c : C)
        for (const bg::Group &g : c.groups) add_name(g.mat);
    const std::vector<std::string> wm = bg::water_materials(p);
    const std::set<std::string> water(wm.begin(), wm.end());
    // the kind of each PS3 water surface (object word 5: 4 / 5 mesh surfaces, 7 / 8 grid surfaces, 9 displacement), per
    // material; PS4 keeps it, and the retail water renderer draws a water.fx material only on objects of one kind
    std::map<std::string, uint32_t> wkind;
    for (const auto &[o, m] : bg::water_objects(p))
        if (m && !m->empty() && !wkind.count(*m)) wkind[*m] = p.pu32(o.first, o.second + 20);
    for (const std::string &n : wm) add_name(n);
    std::vector<bg::Material3> mats3;
    std::vector<bg::Texture3> texs3;
    bg::read_materials(p, mats3, texs3);
    std::map<std::string, const bg::Material3 *> m3;
    for (const bg::Material3 &m : mats3) m3[m.name] = &m;
    const std::map<std::string, bg::Params> P3 = bg::all_material_params(p);
    const std::map<std::string, std::set<int>> A3 = ps3_material_attrs(p);
    auto p3_of = [&](const std::string &n) {
        auto it = P3.find(n);
        return it == P3.end() ? bg::Params() : it->second;
    };
    auto roles3 = [&](const std::string &n) {
        unsigned r = 0;
        auto it = m3.find(n);
        if (it == m3.end()) return r;
        for (uint32_t i : it->second->textures)
            if (auto x = tex_role(texs3.at(i).name)) r |= role_bit(*x);
        return r;
    };
    // texture twin sets: short paths without env maps (.dds), lightmaps, default / missing textures
    auto twin_tex_ignored = [](const std::string &s) {
        return s.empty() || has(s, "missingtexture") || starts_with(s, "art/general/textures/default/") || starts_with(s, "lightmaps/") ||
               ends_with(s, ".dds") || has(s, ".geom.atgi/");
    };
    auto twin_param_ignored = [](const std::string &prm) { return has(prm, "LightMap") || has(prm, "Environment") || has(prm, "EnvCube"); };
    auto twin_textures3 = [&](const std::string &n) {
        std::set<std::string> s;
        auto it = m3.find(n);
        if (it == m3.end()) return s;
        static const std::regex HT(R"(/[0-9A-F]{16}\.ndb(\.error)?$)");
        for (uint32_t i : it->second->textures) {
            const std::string x = std::regex_replace(short_of(texs3.at(i).name), HT, "");
            if (!twin_tex_ignored(x)) s.insert(x);
        }
        return s;
    };
    std::map<std::string, int> used;
    std::map<std::string, PlanMat> out;
    std::vector<std::string> order;
    auto put = [&](const std::string &n, PlanMat e) {
        order.push_back(n);
        out[n] = std::move(e);
    };
    auto plan_mat = [&](const std::string &n, const SEnt &c, const std::string &key, bool exact) {
        PlanMat e;
        e.key = key;
        e.pak = c.pak;
        e.exact = exact;
        e.name = c.m->n;
        e.binds = c.m->b;
        e.ps3params = p3_of(n);
        return e;
    };
    // exact first (greedy: prefer paks that cover many materials)
    std::map<std::string, std::vector<SEnt>> cand;
    std::map<std::string, int> cover;
    for (const std::string &n : names) {
        std::vector<SEnt> cs, all;
        if (auto ki = S->key_at.find(ps4key(n)); ki != S->key_at.end())
            for (const SEnt &c : S->keys[ki->second].ents)
                if (!exclude.count(c.pak)) {
                    all.push_back(c);
                    if (c.m->prime) cs.push_back(c);
                }
        cand[n] = cs.empty() ? all : cs;
        std::set<std::string> pks;
        for (const SEnt &c : cand[n]) pks.insert(c.pak);
        for (const std::string &pk : pks) cover[pk]++;
    }
    for (const std::string &n : names) {
        const auto &cs = cand[n];
        if (cs.empty()) continue;
        const SEnt *best = nullptr;
        for (const SEnt &c : cs)
            if (!best || std::make_pair(cover[c.pak], -int64_t(c.size)) > std::make_pair(cover[best->pak], -int64_t(best->size))) best = &c;
        put(n, plan_mat(n, *best, ps4key(n), true));
        used[best->pak]++;
    }
    // fallbacks
    const std::set<uint32_t> alpha_tex = ps3_alpha_textures(p);
    fp::Ps3Programs progs3(p);   // the PS3 programs (overlay layers on UV set 2)
    for (const std::string &n : names) {
        if (out.count(n)) continue;
        if (water.count(n)) {
            // the closest water.fx material: common directory, same variant, smaller pak
            struct WB {
                int common;
                bool same;
                int64_t negsz;
                const SKey *k;
                const SEnt *c;
            };
            std::optional<WB> wbest;
            const std::vector<std::string> nc = [&] { auto v = split(n, '/'); v.pop_back(); return v; }();
            const std::string nv = n.substr(n.rfind('/') + 1);
            if (!water_templates.empty() && fs::exists(water_templates)) {
                // the sky converter: score every retail water.fx template against the PS3 material (waterscore.py)
                if (!wtpl) wtpl = read_json(water_templates);
                std::vector<std::string> tx3;
                if (auto mi3 = m3.find(n); mi3 != m3.end())
                    for (uint32_t i : mi3->second->textures) {
                        if (i >= texs3.size()) continue;
                        static const std::regex HT(R"(/[0-9A-F]{16}\.ndb.*$)");
                        const std::string s = std::regex_replace(texs3[i].name, HT, "");
                        tx3.push_back(s.substr(s.rfind('/') == std::string::npos ? 0 : s.rfind('/') + 1));
                    }
                std::vector<std::pair<std::string, const SEnt *>> cands;   // template key -> its smallest retail pak
                for (const SKey &k : S->keys)
                    for (const SEnt &c : k.ents) {
                        if (exclude.count(c.pak) || !c.m->water || !wtpl->get(bg::utf8_of(k.k))) continue;
                        auto ci = std::find_if(cands.begin(), cands.end(), [&](const auto &x) { return x.first == k.k; });
                        if (ci == cands.end()) cands.push_back({k.k, &c});
                        else if (c.size < ci->second->size) ci->second = &c;
                    }
                std::map<std::string, uint64_t> psz;
                for (const auto &[k, c] : cands) psz[c->pak] = c->size;
                if (!cands.empty()) {
                    std::vector<std::string> p3names;
                    for (const auto &[k, v] : p3_of(n)) p3names.push_back(k);
                    const std::string *bestk = nullptr;
                    std::tuple<bool, bool, bool, bool, int, int, int, int64_t, std::string> bestkey;
                    const auto wk = wkind.find(n);
                    std::set<std::string> tb(tx3.begin(), tx3.end());
                    for (const auto &[k, c] : cands) {
                        const json::Value &t = *wtpl->get(bg::utf8_of(k));
                        std::vector<std::string> tparams, tfeats;
                        std::set<std::string> tbinds, tbn;
                        if (const json::Value *v = t.get("params"))
                            for (const json::Value &x : v->items) tparams.push_back(latin1_of(x.s));
                        if (const json::Value *v = t.get("feats"))
                            for (const json::Value &x : v->items) tfeats.push_back(latin1_of(x.s));
                        if (const json::Value *v = t.get("binds"))
                            for (const json::Value &x : v->items) {
                                tbinds.insert(latin1_of(x.items.at(0).s));
                                const std::string s1 = latin1_of(x.items.at(1).s);
                                tbn.insert(s1.substr(s1.rfind('/') == std::string::npos ? 0 : s1.rfind('/') + 1));
                            }
                        const int s = water_score(p3names, tx3, tparams, tbinds, tfeats);
                        int tov = 0;
                        for (const std::string &x : tb) tov += int(tbn.count(x));
                        const std::vector<std::string> kc = split(split(k, ':')[0], '/');
                        int common = 0;
                        for (size_t i = 0; i < std::min(nc.size(), kc.size()) && nc[i] == kc[i]; i++) common++;
                        auto pi = psz.find(c->pak);
                        // first a template that retail draws on a water surface of the PS3 surface's kind: a grid
                        // program (cruise-ship-ocean-end, ship-water-plane: kind 7) on a mesh surface (kind 5) drew Fort's
                        // and Museum's open sea wrong
                        const json::Value *tk = t.get("kind");
                        const bool same_kind = wk == wkind.end() || !tk || tk->i == int64_t(wk->second);
                        const bool kind_ok = same_kind || (wk != wkind.end() && water_class_ok(int64_t(wk->second), tk));
                        const auto key = std::make_tuple(kind_ok, g_u2 || water_diffuse_ok(tx3, tbinds), water_env_ok(tx3, tbinds), same_kind, s, tov,
                                                         common, -int64_t(pi == psz.end() ? 0 : pi->second), k);
                        if (!bestk || key > bestkey) {
                            bestk = &k;
                            bestkey = key;
                        }
                    }
                    const SEnt *c = std::find_if(cands.begin(), cands.end(), [&](const auto &x) { return x.first == *bestk; })->second;
                    const SKey *sk = &S->keys[S->key_at.at(*bestk)];
                    wbest = WB{0, false, 0, sk, c};
                }
            }
            if (!wbest)
                for (const SKey &k : S->keys)
                for (const SEnt &c : k.ents) {
                    if (exclude.count(c.pak) || !c.m->water) continue;
                    int common = 0;
                    for (size_t i = 0; i < std::min(nc.size(), k.kc.size()) && nc[i] == k.kc[i]; i++) common++;
                    const size_t kp = k.k.rfind(':');
                    const bool same = nv == (kp == std::string::npos ? k.k : k.k.substr(kp + 1));
                    const WB w{common, same, -int64_t(c.size), &k, &c};
                    if (!wbest || std::make_tuple(w.common, w.same, w.negsz) > std::make_tuple(wbest->common, wbest->same, wbest->negsz)) wbest = w;
                }
            if (wbest) {
                PlanMat e = plan_mat(n, *wbest->c, wbest->k->k, false);
                e.has_roles = true;
                e.cls = {"water"};
                put(n, e);
                used[wbest->c->pak]++;
                continue;
            }
        }
        {
            // texture twin: a retail prime-shader material that binds exactly the PS3 material's own textures (same short
            // paths, at least two; env maps, lightmaps, defaults and missing textures aside) is the same authored material
            // under another name (dig-coop-waterfall-01 = san-cistern's san-plaza-waterfalls-01). It is the template and
            // its bindings keep those textures (stage_convert); the PS3 constants still overwrite its own. It does not count
            // as a used pak, so the other materials' choices stay as they were.
            const std::set<std::string> t3 = twin_textures3(n);
            const SEnt *tw = nullptr;
            const SKey *twk = nullptr;
            // (U3: a twin that culls otherwise than the PS3 pass state is no twin: Desert Village's two-sided thatch
            // des-reeds-messy2alpha-001 twins the one-sided retail des-vil-reeds2-2trans and vanished from below)
            int two3 = -1;
            if (auto mi3 = m3.find(n); mi3 != m3.end() && !g_u2) two3 = mi3->second->two_sided;
            // (a pinned material keeps its pinned key as twin whatever it culls: stage_convert writes the PS3 cull state)
            const auto pin = pins.find(n);
            if (t3.size() >= 2)
                for (const SKey &k : S->keys) {
                    if (k.shadowblocker && !shadow_only_name(n)) continue;
                    if (pin != pins.end() && k.k != pin->second) continue;
                    for (const SEnt &c : k.ents) {
                        if (exclude.count(c.pak) || !c.m->prime) continue;
                        if (pin == pins.end() && two3 >= 0 && (two3 == 1) != c.m->two) continue;
                        std::set<std::string> t4;
                        for (const auto &bd : c.m->b)
                            if (!twin_param_ignored(bd.first) && !twin_tex_ignored(bd.second)) t4.insert(bd.second);
                        if (t4 != t3) continue;
                        if (!tw || std::make_pair(used[c.pak] > 0, -int64_t(c.size)) > std::make_pair(used[tw->pak] > 0, -int64_t(tw->size))) {
                            tw = &c;
                            twk = &k;
                        }
                    }
                }
            if (tw) {
                PlanMat e = plan_mat(n, *tw, twk->k, false);
                e.twin = true;
                e.has_roles = true;
                const unsigned r3 = roles3(n);
                for (const char *r : {"blend", "color", "detail", "env", "mask", "normal", "spec", "trans"})
                    if (r3 & role_bit(r)) e.roles.push_back(r);
                e.cls = {"twin"};
                put(n, e);
                continue;
            }
        }
        // U3: a PS3 water program (bump + refraction / flow constants) on a standing sheet of background geometry is a
        // waterfall: retail draws its waterfalls with the prime-shader ND_WATERFALL templates (cha-waterfalls-standard,
        // san-cistern / san-courtyard water-fall-01), never with h2o.fx / water.fx, whose programs draw a lying water surface
        // (on the Sanctuary stone-head waterfall the h2o.fx ocean template left a thin dark line). Such a material skips the
        // other-family rule and takes the flowing-water prime pass below
        bool standing_water = false;
        if (!g_u2 && !water.count(n)) {
            bool bump = false, flow = false;
            for (const auto &[k, v] : p3_of(n)) {
                bump = bump || k == "g_fBumpScale";
                flow = flow || starts_with(k, "g_fRefraction") || starts_with(k, "g_fFlowTime") || starts_with(k, "g_fTimeScroll");
            }
            if (bump && flow) {
                const double up = ps3_material_upness(p, C, n);
                standing_water = up >= 0 && up < 0.5;
            }
        }
        if (!g_u2 && !pak_dir.empty() && !water.count(n) && !standing_water && !pins.count(n) && other_family(p3_of(n))) {
            // a PS3 material of another shader family (water / h2o meshes, waterfall sheets, noodler waterfall foam): the
            // retail material of a non-prime shader whose constants (packed names split) cover it best, the textures it
            // shares with the PS3 material breaking ties. Its own program keeps the PS3 look and motion; a prime-shader
            // fallback makes the surface still and flat.
            std::vector<std::string> p3names;
            for (const auto &[k, v] : p3_of(n))
                if (k != "_Default_Maya_Color_") p3names.push_back(k);
            const std::set<std::string> P = split_names(p3names);
            std::set<std::string> t3;
            if (auto mi3 = m3.find(n); mi3 != m3.end())
                for (uint32_t i : mi3->second->textures)
                    if (i < texs3.size()) {
                        static const std::regex HT(R"(/[0-9A-F]{16}\.ndb(\.error)?$)");
                        t3.insert(std::regex_replace(short_of(texs3[i].name), HT, ""));
                    }
            const SEnt *fb = nullptr;
            const SKey *fk = nullptr;
            std::tuple<int, int, int, int64_t> fbest{};
            int blend3 = -1;   // the PS3 render state: 1 blends, 0 does not, -1 unknown
            if (auto mi3 = m3.find(n); mi3 != m3.end() && mi3->second->alpha_test >= 0) blend3 = mi3->second->blend;
            for (const SKey &k : S->keys)
                for (const SEnt &c : k.ents) {
                    const std::string &sh = c.m->s;
                    if (exclude.count(c.pak) || c.m->prime || starts_with(sh, "u2parts") || starts_with(sh, "u3parts") || starts_with(sh, "hud/") ||
                        sh == "lensflare.fx" || sh == "sky-clouds.fx")
                        continue;
                    const auto rp = retail_params(pak_dir, c.pak);
                    auto ti = rp->find(c.m->n);
                    if (ti == rp->end()) continue;
                    const std::set<std::string> T = split_names(ti->second);
                    // the fresnel / specular / diffuse terms every surface shader has are no evidence of the family
                    static const std::regex GENERIC(R"(^g_(fFresnel|fSpecular|cDiffuse|fDiffuse))");
                    int both = 0, own3 = 0, own_both = 0;
                    for (const std::string &x : P) {
                        const bool hit = T.count(x) != 0, gen = std::regex_search(x, GENERIC);
                        both += int(hit);
                        own3 += int(!gen);
                        own_both += int(hit && !gen);
                    }
                    if (own_both < 3 || 3 * size_t(both) < 2 * P.size() || 3 * own_both < 2 * own3) continue;   // two thirds of the PS3 constants at least
                    const int s = 2 * both - int(T.size() - both) - int(P.size() - both);
                    int tov = 0;
                    for (const auto &bd : c.m->b) tov += int(t3.count(bd.second));
                    // (the PS3 render state first: a PS3 sheet that blends takes a blending template (Borneo / Cave waterfall foam
                    // drew opaque on a non-blending waterfall template); then the constants and shared textures)
                    // (the non-prime shaders blend by ND_ALPHA_BLEND)
                    const bool blend4 = (c.m->c4 & C_BLEND) || std::find(c.m->f.begin(), c.m->f.end(), "ND_ALPHA_BLEND") != c.m->f.end();
                    const int cls_ok = blend3 < 0 || (blend3 == 1) == blend4;
                    const auto key = std::make_tuple(cls_ok, s + 4 * tov, tov, -int64_t(c.size));
                    if (!fb || key > fbest) {
                        fb = &c;
                        fk = &k;
                        fbest = key;
                    }
                }
            if (fb) {
                PlanMat e = plan_mat(n, *fb, fk->k, false);
                e.has_roles = true;
                e.cls = {"shader"};
                put(n, e);
                used[fb->pak]++;
                continue;
            }
        }
        M3 m;
        m.n = n;
        m.low = lower(n);
        m.r3 = roles3(n);
        const auto mi = m3.find(n);
        bool col_alpha = false;
        if (mi != m3.end() && mi->second->alpha_test >= 0) {
            // U3: the PS3 material's own render state says whether it alpha-tests (texture names do not: an opaque wall's
            // overlay mask is named -alpha too); an overlay layer's mask is no transparency role
            col_alpha = mi->second->alpha_test == 1;
            // (an overlay layer: the PS3 program reads its colour and mask with UV set 2, as ps4stage requires)
            const auto ovp = fp::uv_set2_reads(progs3, n) >= 2 ? overlay_pairs(*mi->second, texs3)
                                                               : std::vector<std::pair<std::string, std::string>>{};
            m.ov = !ovp.empty();
            bool other_trans = false;
            for (uint32_t i : mi->second->textures) {
                const std::string s = short_of(texs3.at(i).name);
                if (tex_role(s) != "trans") continue;
                bool paired = false;
                for (const auto &pr : ovp) paired = paired || s.compare(0, pr.second.size(), pr.second) == 0;
                other_trans = other_trans || !paired;
            }
            if (m.ov && !other_trans) m.r3 &= ~R_TRANS;
        } else if (mi != m3.end())
            for (uint32_t i : mi->second->textures) {
                const bg::Texture3 &t = texs3.at(i);
                const auto r = tex_role(short_of(t.name));
                if (r == "trans" || (r == "color" && alpha_tex.count(t.hash))) col_alpha = true;
            }
        bool two_cols = false;
        if (mi != m3.end() && !g_u2) {
            // U3: two colour maps of their own (not an overlay layer's colour, not a height map) are two blended layers, also
            // when no second-layer constant says so: desert-village's mud2diagonal-bricks blends a mud and a brick colour
            // by a height map and the vertex colour; taken as one layer it was planned on a single-layer crate material
            std::set<std::string> cols;
            const auto ovp = fp::uv_set2_reads(progs3, n) >= 2 ? overlay_pairs(*mi->second, texs3)   // london2 r2: overlay layers only where the PS3 program reads them with UV set 2
                                                               : std::vector<std::pair<std::string, std::string>>{};
            static const std::regex HT(R"(/[0-9A-F]{16}\.ndb(\.error)?$)");
            for (uint32_t i : mi->second->textures) {
                const std::string s = std::regex_replace(short_of(texs3.at(i).name), HT, "");
                if (tex_role_final(s) != "color" || has(lower(s), "-height")) continue;
                bool ov = false;
                for (const auto &pr : ovp) ov = ov || pr.first == s;
                if (!ov) cols.insert(s);
            }
            two_cols = cols.size() >= 2;
        }
        const size_t sl = m.low.rfind('/');
        if (rx(m.low, FOLIAGE) && !rx(sl == std::string::npos ? m.low : m.low.substr(sl + 1), GROUNDISH)) m.c3 |= C_LEAF;
        // U3: the PS3 render state is the render class (as for the alpha test above): a blending first pass is blend, else
        // an alpha test is cut, else opaque. The name rule (glass / window) made opaque PS3 windows, window frames and
        // 'fakewindow' wall carvings alpha-blended (see-through) on PS4; retail never blends an exact twin PS3 draws opaque
        m.rstate = mi != m3.end() && mi->second->alpha_test >= 0 && mi->second->blend >= 0;
        if (m.rstate) {
            if (mi->second->blend == 1) m.c3 |= C_BLEND;
            else if (col_alpha) m.c3 |= C_CUT;
        } else if (rx(m.low, BLENDNAME))
            m.c3 |= C_BLEND;
        else if (col_alpha)
            m.c3 |= C_CUT;
        if (mi != m3.end() && !g_u2) m.two_sided = mi->second->two_sided;
        m.fdir = n.substr(0, n.rfind('/'));
        {
            const auto v = split(n, '/');
            m.top = v[0] + (v.size() > 1 ? "/" + v[1] : "");
            m.ncomp.assign(v.begin(), v.end() - 1);
        }
        if (auto a = A3.find(n); a != A3.end()) m.attrs3 = a->second;
        const bg::Params p3 = p3_of(n);
        {   // ps3_needs, ps3_two_layers, ps3_strong
            static const std::regex SCROLL(R"(^g_timescroll[uv](\d)$)"), FLOW("flowtime|flowuv|uvflow");
            std::string ks;
            for (const auto &[k, v] : p3) {
                const std::string kl = lower(k);
                ks += (ks.empty() ? "" : " ") + kl;
                std::smatch sm;
                std::string need;
                // a scroll layer is needed only when the PS3 material moves it (a speed of 0 is a still layer)
                if (std::regex_search(kl, sm, SCROLL)) {
                    if (!v.empty() && std::fabs(v[0]) > 1e-6) need = "UV_" + sm[1].str() + "_APPLY_TIME_SCROLL";
                } else if ((kl == "g_ftimescrollx" || kl == "g_ftimescrolly") && !v.empty() && std::fabs(v[0]) > 1e-6)
                    need = "UV_1_APPLY_TIME_SCROLL";   // the old surface shaders scroll their one UV set
                else if (rx(kl, FLOW)) need = "UV_FLOW";
                if (!need.empty() && std::find(m.scr3.begin(), m.scr3.end(), need) == m.scr3.end()) m.scr3.push_back(need);
                if (k == "g_colorTint2" || k == "g_colorBrightness2" || k == "g_blendSoftness_blendOffset_linearBlendOffset") m.two3 = true;
                if (k == "g_blendSoftness_blendOffset_linearBlendOffset") m.soft3 = true;
                if (k == "g_blendSoftness_blendOffset_linearBlendOffset" && !g_u2) m.softblend3 = !fp::hard_blend(progs3, n);
                int d;
                if (colp(k, d)) {
                    bool strong = false;
                    if (starts_with(k, "g_colorTint"))
                        for (size_t j = 0; j < std::min<size_t>(3, v.size()); j++) strong = strong || std::fabs(v[j] - 1) > 0.02;
                    else
                        strong = std::fabs(v.at(0) - 1) > 0.02;
                    if (strong) m.strong.push_back(k);
                } else if (uvp(k, d) && std::fabs(v.at(0) - 1) > 0.02)
                    m.strong.push_back(k);
                else if (k == "g_diffuseColorBorderTint")
                    m.strong.push_back(k);
                if (k != "g_specularBrightness" && k != "g_specularExponentMax") m.ns3++;
                if (k == "g_specularBrightness" && !v.empty() && v[0] > 1e-4) m.spec3 = true;
                if (k == "g_specularEnvironmentStrength" && !v.empty() && v[0] > 1e-4) m.env3 = true;
            }
            m.inc3 = has(ks, "incand");
            m.fres3 = has(ks, "g_fresnel");
            m.dis3 = has(ks, "distort");
            static const std::regex EMISSIVE("lamp|light|fire|glow|neon|screen|incand|emissive|sky");
            m.emissive = m.inc3 || rx(m.low, EMISSIVE);
            m.nrm3 = (m.r3 & R_NORMAL) != 0;
            if (m.nrm3 && mi != m3.end() && !g_u2) {
                static const std::regex HT(R"(/[0-9A-F]{16}\.ndb(\.error)?$)");
                std::set<std::string> nrms;
                for (uint32_t i : mi->second->textures) {
                    const std::string s = std::regex_replace(short_of(texs3.at(i).name), HT, "");
                    if (tex_role_final(s) == "normal") nrms.insert(s);
                }
                m.nrm2 = nrms.size() >= 2;
            }
            // (a moving material keeps its scroll first: a second colour layer is not asked of the scrolling templates)
            if (two_cols && m.scr3.empty()) m.two3 = true;
            if (!g_u2)   // U3: the far tilings of the layers the PS3 material draws (layer 1; layer 2 of a two-layer one)
                for (const auto &[k, v] : p3) {
                    int d;
                    if (!uvp(k, d) || v.empty() || d < 1 || d > 4 || (d >= 2 && !m.two3)) continue;
                    const double a = std::fabs(v[0]);
                    if (a >= 1.5 || a <= 1.0 / 1.5) m.uvfar |= 1u << d;
                }
        }
        struct Best {
            int64_t score2, negsz;
            const SKey *k;
            const SEnt *c;
        };
        std::optional<Best> best;
        // a PS3 water program on background geometry (bump + refraction / flow / time scroll constants, not a WATER_DATA
        // surface: waterfalls, river sheets): only the retail flowing-water prime templates (ND_WATER_FLOW) first, then the
        // whole pool (PS4 has water.fx on WATER_DATA surfaces only)
        bool waterprog = false;
        {
            bool bump = false, flow = false;
            for (const auto &[k, v] : p3) {
                bump = bump || k == "g_fBumpScale";
                flow = flow || starts_with(k, "g_fRefraction") || starts_with(k, "g_fFlowTime") || starts_with(k, "g_fTimeScroll");
            }
            waterprog = bump && flow;
            // U3: a PS3 water program refracts the scene behind it through its bump map: a refracting waterfall template
            // (TRANSPARENCY_DISTORTION) keeps it clear; a '-no-refract' one drew Borneo co-op's clear falling-water sheet
            // as an opaque dark sheet across the gorge
            if (waterprog && !g_u2) m.dis3 = true;
        }
        const std::string mkind = surface_kind(n);
        // U3: a moving PS3 water surface (a material of the water kind or a caustics sheet with scroll / flow constants) is
        // planned on a template that moves the same UV set first (pass 1), whatever else it lacks: a still river or caustic
        // sheet looks broken, a missing layer or a render-class mismatch on it much less (the Highrise / Shambhala wide
        // rivers, Fort co-op cistern caustics); pass 2 takes any
        auto carries = [&](const FeatSet &f) {
            for (const std::string &need : m.scr3)
                for (const std::string &x : f.feats)
                    if (!f.uv34 && starts_with(x, need)) return true;
            return false;
        };
        const bool scroll_first = !g_u2 && !m.scr3.empty() && (mkind == "water" || has(m.low, "caustic"));
        for (int pass = waterprog ? 0 : scroll_first ? 1 : 2; pass < 3 && !best; pass++)
        for (const SKey &k : S->keys) {
            if (auto pin = pins.find(n); pin != pins.end() && k.k != pin->second) continue;   // a pinned material: its pinned key only
            if (!g_u2 && k.shadowblocker != shadow_only_name(n)) continue;   // U3: shadow casters on shadow templates only
            if (g_u2 && k.shadowblocker && !has(n, "shadowblocker")) continue;
            if (!g_u2 && has(k.k, "ik-only") != has(n, "ik-only")) continue;   // IK-only blockers stay their own kind
            int common = 0;
            for (size_t i = 0; i < std::min(m.ncomp.size(), k.kc.size()) && m.ncomp[i] == k.kc[i]; i++) common++;
            int dirsc = 0;
            if (k.kdir == m.fdir) dirsc = 8;
            else if (starts_with(k.k, m.top)) dirsc = 1 + std::max(0, common - 2);
            if (!g_u2) {
                // U3: a template of the PS3 material's own folder or own kind of surface; one of a clashing kind (a wall on
                // a crate, grass on wires) is a last resort
                if (k.kdir != m.fdir && k.kc.size() == m.ncomp.size() && size_t(common) + 1 == m.ncomp.size()) dirsc += 6;
                if (!mkind.empty() && mkind == k.kind) dirsc += 10;
                else if (kinds_clash(mkind, k.kind)) dirsc -= 20;
            }
            for (const SEnt &c : k.ents) {
                // U3: prime-shader materials only; U2 PS4 level materials mostly keep their own shaders
                if (exclude.count(c.pak) || !(g_u2 ? c.m->u2bg : c.m->prime)) continue;
                if (pass == 0 && std::find(c.m->f.begin(), c.m->f.end(), "ND_WATER_FLOW") == c.m->f.end()) continue;
                if (pass == 1 && (!scroll_first || !carries(S->fsets[c.m->fsid]))) continue;
                const SMat &sm = *c.m;
                // (U3: a PS3 spec map counts as the mask it becomes on PS4: ps4stage binds it to a template mask slot)
                const unsigned r3 = !g_u2 && (m.r3 & R_SPEC) ? (m.r3 & ~R_SPEC) | R_MASK : m.r3;
                int score = popc(r3 & sm.r4) * 2 - popc(sm.r4 & ~r3) - popc(r3 & ~sm.r4);
                // U3: a PS3 '-blend' map is the factor its program blends the two layers by: a template without a blend-map
                // slot blends by the vertex colour or a colour channel instead and the PS3 pattern is lost (Facility's
                // concrete floors)
                if (!g_u2 && (r3 & R_BLEND) && m.scr3.empty() && !(sm.r4 & R_BLEND)) score -= 12;
                // (a render class the PS3 state proves outweighs every other term: a template that blends an opaque or
                // alpha-tested surface draws it see-through, one that does not alpha-test an alpha-tested one draws its cut
                // parts as solid cards. A cutoff template on an opaque PS3 surface stays a mild mismatch: the opaque PS3
                // colour maps carry no alpha to cut, and such templates are often the only ones that scroll a water layer.
                // A blending PS3 material may alpha-test too: retail blends its twins with or without a cutoff)
                if (m.rstate && (m.c3 & C_BLEND)) score += (sm.c4 & C_CUT) ? 0 : 12;
                else score += ((m.c3 & C_CUT) != 0) == ((sm.c4 & C_CUT) != 0) ? 12 : m.rstate && (m.c3 & C_CUT) ? -60 : -12;
                score += ((m.c3 & C_BLEND) != 0) == ((sm.c4 & C_BLEND) != 0) ? 8 : m.rstate ? -60 : -8;
                score += ((m.c3 & C_LEAF) != 0) == ((sm.c4 & C_LEAF) != 0) ? 4 : -2;
                if ((m.c3 & C_CUT) && !(m.r3 & R_TRANS) && sm.tmus1a) score += 6;
                score += dirsc;
                if (((m.r3 & R_TRANS) != 0) != ((sm.r4 & R_TRANS) != 0)) score -= 5;
                score -= 3 * int(m.ns3);   // no feature rule file: the template's predicted params are empty
                if (!g_u2) {
                    // U3: the PS3 pass state's face culling: a two-sided PS3 surface (reed mats, foliage, cloth) on a
                    // one-sided template vanishes from behind (Desert Village's reed roof seen from below), a one-sided one
                    // on a two-sided template shows its back faces
                    if (m.two_sided >= 0 && (m.two_sided == 1) != sm.two) score -= 25;
                    // a PS3 colour texture on a template without a colour map draws as the flat template tint (white)
                    if ((m.r3 & R_COLOR) && !(sm.r4 & R_COLOR)) score -= 40;
                    // a PS3 layer tiled far from 1x (g_uvScale<d> 0.5 / 3 / 8 / 10 ...) on a template that cannot scale
                    // that layer stretches it (Sanctuary's moss layer at 1/10 of its tiling: a blurry green pillar)
                    score -= 30 * popc(m.uvfar & ~sm.uvs);
                    // a PS3 shadow blocker draws (black) where the PS3 instance shows it; the instance flags hide the rest
                    if (sm.proxy) score -= 20;
                    // a lit PS3 surface on a template the sun does not light (interior templates) darkens outdoors
                    if (!sm.sun && !m.emissive) score -= 12;
                }
                if (sm.wind) score -= 40;
                if (sm.leaf && !(m.c3 & C_LEAF)) score -= 30;
                // a PS3 overlay layer (colour + its mask, opaque, read with UV set 2): a template with an overlay colour +
                // control slot pair on UV set 2 carries it; one laid out on another set would draw it in the wrong place
                // (ps4stage turns it off), so a template without an overlay does better. Closer to the PS3 program: the
                // overlay on layer 3, after the layer 1 / 2 blend, for a two-layer PS3 material (the PS3 twins of retail
                // COLOR_3 overlays, des-mud-stucco2crumble-003 and ug-lib-block-01, blend first, then overlay); the blend
                // softness when the PS3 material has one; the mask scaled by the vertex colour (COLOR_d_VERTEX_OVERLAY_MASK:
                // the PS3 programs multiply it by vertex green ^ 2.2 when a vertex colour exists)
                if (m.ov && sm.ovl && sm.ovset2)
                    score += 60 + (m.two3 && sm.ovl == 3 ? 30 : 0) + (m.soft3 == sm.soft ? 10 : 0) + (m.attrs3.count(0x15) && sm.ovvmask ? 10 : 0);
                else if (m.ov)
                    score -= sm.ovl ? 60 : 20;   // without a usable overlay the PS3 layer is lost
                auto ci = m.cache.find(sm.fsid);
                if (ci == m.cache.end()) ci = m.cache.emplace(sm.fsid, v2_v23(m, S->fsets[sm.fsid])).first;
                score += ci->second;
                score += soft_blend_score(m, sm);
                score += trans_map_score(m, sm, S->fsets[sm.fsid]);
                score -= 2 * popc(sm.r4 & ~r3);
                const int64_t score2 = int64_t(score) * 2 + (used[c.pak] > 0 ? 1 : 0);
                if (!best || std::make_pair(score2, -int64_t(c.size)) > std::make_pair(best->score2, best->negsz)) best = Best{score2, -int64_t(c.size), &k, &c};
            }
        }
        if (!best) {
            // why no template was left: the survey in memory and what each test turned away (a player's import failed
            // here on every map with a survey equal to a working one, 2026-11-22)
            size_t keys = 0, pinned = 0, shadow = 0, ik = 0, ents = 0, excluded = 0, not_prime = 0;
            const auto pin = pins.find(n);
            for (const SKey &k : S->keys) {
                keys++;
                if (pin != pins.end() && k.k != pin->second) { pinned++; continue; }
                if (!g_u2 && k.shadowblocker != shadow_only_name(n)) { shadow++; continue; }
                if (!g_u2 && has(k.k, "ik-only") != has(n, "ik-only")) { ik++; continue; }
                for (const SEnt &c : k.ents) {
                    ents++;
                    if (exclude.count(c.pak)) excluded++;
                    else if (!(g_u2 ? c.m->u2bg : c.m->prime)) not_prime++;
                }
            }
            throw Stop(fmt("no fallback material for %s (survey %zu materials, %zu keys; turned away: pinned %zu, shadow %zu, "
                           "ik %zu; entries %zu, excluded %zu, not prime %zu; shadow-only name %d, pinned to %s, u2 %d)",
                           n.c_str(), S->mats.size(), keys, pinned, shadow, ik, ents, excluded, not_prime,
                           int(shadow_only_name(n)), pin == pins.end() ? "-" : pin->second.c_str(), int(g_u2)));
        }
        PlanMat e = plan_mat(n, *best->c, best->k->k, false);
        e.has_roles = true;
        for (const char *r : {"blend", "color", "detail", "env", "mask", "normal", "spec", "trans"})
            if (m.r3 & role_bit(r)) e.roles.push_back(r);
        for (const char *c : {"blend", "cut", "leaf"})
            if (m.c3 & (std::string(c) == "blend" ? C_BLEND : std::string(c) == "cut" ? C_CUT : C_LEAF)) e.cls.push_back(c);
        put(n, e);
        used[best->c->pak]++;
    }
    // textures: every PS3 texture and every texture bound by a chosen PS4 material
    std::vector<std::string> shorts;
    for (const PakTexture &t : p.textures()) shorts.push_back(short_of(t.path));
    for (const std::string &n : order)
        for (const auto &b : out[n].binds) shorts.push_back(b.second);
    std::set<std::string> want_done;
    std::set<std::pair<std::string, std::string>> want;
    for (const std::string &s : shorts) {
        if (want_done.count(s)) continue;
        auto ti = S->tex.find(s);
        if (ti == S->tex.end()) continue;
        const std::pair<std::string, std::string> *best = nullptr;
        std::pair<int, int64_t> bk{};
        for (const auto &x : ti->second) {
            auto ps = S->paksize.find(x.first);
            const std::pair<int, int64_t> k{used[x.first] > 0 ? 1 : 0, -int64_t(ps == S->paksize.end() ? (1ull << 40) : ps->second)};
            if (!best || k > bk) {
                best = &x;
                bk = k;
            }
        }
        want_done.insert(s);
        want.insert(*best);
    }
    std::set<std::pair<std::string, std::string>> rm;
    for (const std::string &n : order) rm.insert({out[n].pak, out[n].key});
    plan.req_mats.assign(rm.begin(), rm.end());
    plan.req_tex.assign(want.begin(), want.end());
    for (const std::string &n : order) plan.mats.push_back({n, out[n]});
    return plan;
}

std::string plan_json(const Plan &pl) {
    using json::Value;
    auto text = [](const std::string &s) { return Value::text(bg::utf8_of(s)); };
    auto arr = []() { Value v; v.kind = Value::Array; return v; };
    Value root = Value::object();
    root.set("stage", text(pl.stage));
    Value mats = Value::object();
    for (const auto &[n, e] : pl.mats) {
        Value m = Value::object();
        m.set("key", text(e.key));
        m.set("pak", text(e.pak));
        m.set("exact", Value::boolean(e.exact));
        if (e.twin) m.set("twin", Value::boolean(true));
        if (!e.has_bindmap) {   // the sky converter's re-pointed entries drop name, binds, roles and cls
            m.set("name", text(e.name));
            Value b = arr();
            for (const auto &x : e.binds) {
                Value pr = arr();
                pr.items.push_back(text(x.first));
                pr.items.push_back(text(x.second));
                b.items.push_back(pr);
            }
            m.set("binds", b);
        }
        if (e.has_roles && !e.has_bindmap) {
            Value r = arr(), c = arr();
            for (const auto &x : e.roles) r.items.push_back(text(x));
            for (const auto &x : e.cls) c.items.push_back(text(x));
            m.set("roles", r);
            m.set("cls", c);
        }
        Value pp = Value::object();
        for (const auto &[k, v] : e.ps3params) {
            Value a = arr();
            for (double x : v) {
                Value f;
                f.kind = Value::Float;
                f.f = x;
                a.items.push_back(f);
            }
            pp.set(bg::utf8_of(k), a);
        }
        m.set("ps3params", pp);
        if (e.has_bindmap) {
            Value bm = Value::object();
            for (const auto &[k, v] : e.bindmap) bm.set(bg::utf8_of(k), text(v));
            m.set("bindmap", bm);
        }
        mats.fields.push_back({bg::utf8_of(n), m});
    }
    root.set("mats", mats);
    Value req = Value::object();
    for (const auto *lst : {&pl.req_mats, &pl.req_tex}) {
        Value a = arr();
        for (const auto &x : *lst) {
            Value pr = arr();
            pr.items.push_back(text(x.first));
            pr.items.push_back(text(x.second));
            a.items.push_back(pr);
        }
        req.set(lst == &pl.req_mats ? "mats" : "tex", a);
    }
    root.set("requests", req);
    return json_dump_indent1(root);
}

// ------------------------------------------------------------------------------------------------ fxextract
namespace {

struct XPak {
    std::unique_ptr<Pak> q;
    std::map<uint32_t, std::vector<const PakItem *>> idx;   // page -> items by offset
    bool has_tex = false;
    std::map<std::string, PakTexture> tex;
    std::map<std::string, Bytes> gtex;
    bool has_tex2 = false;
    std::map<std::string, PakTexture> tex2;
};

const PakItem *container(const XPak &x, uint32_t pg, uint64_t off) {
    auto it = x.idx.find(pg);
    if (it == x.idx.end()) return nullptr;
    const auto &L = it->second;
    auto u = std::upper_bound(L.begin(), L.end(), off, [](uint64_t o, const PakItem *i) { return o < i->off; });
    if (u == L.begin()) return nullptr;
    const PakItem *c = *(u - 1);
    return c->off <= off && off < c->data_off() + c->size ? c : nullptr;
}

std::string S_(const Pak &q, uint32_t pg, uint64_t off) {
    const ORef r = bg::res(q, pg, off);
    if (!r) throw Stop("fxextract: a null string pointer");
    return q.pstr(r->first, r->second);
}

Bytes slice(const Pak &q, uint64_t a, uint64_t n) {
    const uint64_t s = std::min<uint64_t>(a, q.d.size()), e = std::min<uint64_t>(a + n, q.d.size());
    return Bytes(q.d.begin() + s, q.d.begin() + std::max(s, e));
}

std::vector<std::string> techs(const Pak &q) {
    const auto ets = q.items_of("EFFECT_TABLE");
    if (ets.empty()) throw Stop("IndexError: no EFFECT_TABLE");
    const Ref t = bg::need(q, ets[0].page, ets[0].data_off() + 0x18);
    const uint32_t n = q.pu32(t.first, t.second);
    std::vector<std::string> out;
    for (uint32_t i = 0; i < n; i++) out.push_back(S_(q, t.first, t.second + 8 + 8 * i));
    return out;
}

std::vector<FxItem> closure(const XPak &x, const PakItem &eit) {
    const Pak &q = *x.q;
    std::vector<const PakItem *> seq = {&eit};
    std::map<std::pair<uint32_t, uint32_t>, size_t> ids = {{{eit.page, eit.off}, 0}};
    std::vector<FxItem> out;
    for (size_t k = 0; k < seq.size(); k++) {
        const PakItem &it = *seq[k];
        FxItem f;
        for (uint64_t oo = it.data_off(); oo < uint64_t(it.data_off()) + it.size; oo += 8) {
            const ORef r = bg::res(q, it.page, oo);
            if (!r) continue;
            const PakItem *c = container(x, r->first, r->second);
            if (!c) throw Stop("ValueError: loose pointer");
            auto ii = ids.find({c->page, c->off});
            if (ii == ids.end()) {
                ii = ids.emplace(std::make_pair(c->page, c->off), seq.size()).first;
                seq.push_back(c);
            }
            f.ptrs.push_back({oo - it.data_off(), ii->second, int64_t(r->second) - int64_t(c->data_off())});
        }
        f.type = it.type;
        f.name = it.name;
        f.off = it.data_off();
        f.page = it.page;
        f.data = slice(q, q.pbase(it.page) + it.data_off(), it.size);
        out.push_back(std::move(f));
    }
    return out;
}

// The PS4 texture dictionaries (u3data/build/main/texturedict1/ps4/*.pak) hold the full-size texels of the textures the
// retail game streams. A retail pak keeps only a small resident copy of such a texture (64x64 and its mips, VRAM_DESC
// +0x20 bit 0x100) and the game streams the rest in; a converted level does not stream, so a texture taken from a retail
// level pak drew as that 64x64 copy (Desert: 139 of its 143 textures). A dictionary pak is an ordinary pak: its header
// and VRAM descriptors come first, its texels end the file; only the descriptors are read here, the texels of a texture
// when it is used.
struct DictTex {
    fs::path file;
    uint64_t off = 0, size = 0;
    std::string path;
    Bytes desc;   // the VRAM_DESC data (0x40 bytes)
};
struct Dicts {
    std::map<std::string, DictTex> by_path;
    std::map<std::string, const DictTex *> by_short;
};
const Dicts &texture_dicts(const fs::path &dir) {
    static std::mutex lock;
    static std::map<std::string, Dicts> all;
    std::lock_guard<std::mutex> g(lock);
    const std::string k = utf8(dir);
    if (auto it = all.find(k); it != all.end()) return it->second;
    Dicts &out = all[k];
    std::error_code ec;
    std::vector<fs::path> files;
    for (const auto &e : fs::directory_iterator(dir, ec))
        if (e.path().extension() == ".pak") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const fs::path &f : files) {
        try {
            const uint64_t fsize = fs::file_size(f);
            std::ifstream in(f, std::ios::binary);
            auto rd = [&](uint64_t a, uint64_t n) {
                Bytes b(n);
                in.seekg(std::streamoff(a));
                in.read(reinterpret_cast<char *>(b.data()), std::streamsize(n));
                if (!in) throw Stop("cannot read " + utf8(f));
                return b;
            };
            const Bytes h = rd(0, 0x2c);
            const uint32_t npages = rd32le(&h[0x10]), ptab = rd32le(&h[0x14]), vram = rd32le(&h[0x20]);
            const Bytes pt = rd(ptab, uint64_t(npages) * 12);
            uint64_t end = rd32le(&h[4]);
            for (uint32_t i = 0; i < npages; i++) end = std::max<uint64_t>(end, uint64_t(rd32le(&pt[i * 12])) + rd32le(&pt[i * 12 + 4]));
            if (end > fsize || vram > fsize) continue;
            const Pak q(rd(0, end), utf8(f));
            for (const PakTexture &t : q.textures()) {
                DictTex d;
                d.file = f;
                d.off = fsize - vram + t.vramOffset;
                d.size = t.vramSize;
                d.path = t.path;
                d.desc = slice(q, q.pbase(t.page) + t.off + 0x20, 0x40);
                if (d.desc.size() == 0x40 && d.off + d.size <= fsize) out.by_path[t.path] = std::move(d);
            }
        } catch (const std::exception &) {   // an unreadable dictionary: its textures stay the resident copies
        }
    }
    for (const auto &[p, d] : out.by_path) out.by_short.emplace(short_of(p), &d);   // (the first of a short path)
    return out;
}

// a dictionary texture with its top mips above max_w x max_h left out (mips are stored top first, each padded to 8x8
// blocks); a 2D texture of a known format only
std::optional<DictTexture> dict_read(const DictTex &x, uint32_t max_w, uint32_t max_h) {
    Bytes d = x.desc;
    auto w = [&](size_t o) { return rd32le(&d[o]); };
    if (w(0x18) != 1) return std::nullopt;
    auto fi = tc::DXGI.find(w(0x1c));
    if (fi == tc::DXGI.end()) return std::nullopt;
    const bool comp = fi->second.first.rfind("BC", 0) == 0;
    const uint32_t mips = w(0x24), tw = w(0x28), th = w(0x2c);
    uint32_t n = 0;
    while (n + 1 < mips && ((tw >> n) > max_w || (th >> n) > max_h)) n++;
    const uint64_t cut = n ? tc::ps4_tiled_size(tw, th, n, fi->second.second, comp) : 0;
    if (cut >= x.size) return std::nullopt;
    DictTexture out;
    out.path = x.path;
    out.texels.resize(x.size - cut);
    std::ifstream in(x.file, std::ios::binary);
    in.seekg(std::streamoff(x.off + cut));
    in.read(reinterpret_cast<char *>(out.texels.data()), std::streamsize(out.texels.size()));
    if (!in) return std::nullopt;
    auto put = [&](size_t o, uint32_t v) { std::memcpy(&d[o], &v, 4); };
    put(0x24, mips - n);
    put(0x28, std::max(1u, tw >> n));
    put(0x2c, std::max(1u, th >> n));
    out.desc = std::move(d);
    return out;
}

}  // namespace

bool dict_texture(const fs::path &pak_dir, const std::string &path, uint32_t max_w, uint32_t max_h, Bytes &desc, Bytes &texels) {
    if (desc.size() < 0x30) return false;
    const Dicts &D = texture_dicts(pak_dir.parent_path().parent_path() / "texturedict1" / "ps4");
    auto it = D.by_path.find(path);
    if (it == D.by_path.end() || rd32le(&it->second.desc[0x1c]) != rd32le(&desc[0x1c])) return false;   // same format
    auto t = dict_read(it->second, max_w, max_h);
    if (!t || uint64_t(rd32le(&t->desc[0x28])) * rd32le(&t->desc[0x2c]) <= uint64_t(rd32le(&desc[0x28])) * rd32le(&desc[0x2c]))
        return false;   // not larger than the resident copy
    desc = std::move(t->desc);
    texels = std::move(t->texels);
    return true;
}

std::optional<DictTexture> dict_texture_short(const fs::path &dict_dir, const std::string &shrt, uint32_t max_w, uint32_t max_h) {
    const Dicts &D = texture_dicts(dict_dir);
    auto it = D.by_short.find(shrt);
    if (it == D.by_short.end()) return std::nullopt;
    return dict_read(*it->second, max_w, max_h);
}

namespace {


std::optional<MatRec> material(XPak &x, const std::string &key) {
    const Pak &q = *x.q;
    const auto gs = q.items_of("GEOMETRY_1");
    if (gs.empty()) throw Stop("IndexError: no GEOMETRY_1");
    const uint32_t pg = gs[0].page;
    const uint64_t b = gs[0].data_off();
    const uint64_t n = q.u64(q.pbase(pg) + b + 0x48);
    const Ref mr = bg::need(q, pg, b + 0x50);
    for (uint64_t i = 0; i < n; i++) {
        const uint32_t mp = mr.first;
        const uint64_t o = mr.second + i * 0x48;
        const std::string name = S_(q, mp, o);
        if (split(name, ',')[0] != key) continue;
        const uint64_t mb = q.pbase(mp) + o;
        const uint32_t nf = rd32le(q.d, mb + 0x30), npar = rd32le(q.d, mb + 0x34), nb = rd32le(q.d, mb + 0x38);
        MatRec rec;
        rec.name = name;
        rec.shader = S_(q, mp, o + 8);
        rec.hash = S_(q, mp, o + 0x18);
        rec.x3 = rd32le(q.d, mb + 0x3c);
        rec.tail[0] = rd32le(q.d, mb + 0x40);
        rec.tail[1] = rd32le(q.d, mb + 0x44);
        if (const ORef fr = bg::res(q, mp, o + 0x10))
            for (uint32_t k = 0; k < nf; k++) {
                const Ref s = bg::need(q, fr->first, fr->second + 8 * k);
                rec.feats.push_back(q.pstr(s.first, s.second));
            }
        if (const ORef pr = bg::res(q, mp, o + 0x20))
            for (uint32_t k = 0; k < npar; k++) {
                const uint64_t po = pr->second + 0x18 * k;
                const Ref vr = bg::need(q, pr->first, po + 8);
                const uint32_t cnt = rd32le(q.d, q.pbase(pr->first) + po + 0x10), xx = rd32le(q.d, q.pbase(pr->first) + po + 0x14);
                rec.params.push_back({S_(q, pr->first, po), cnt, xx, slice(q, q.pbase(vr.first) + vr.second, 4ull * std::max<uint32_t>(1, cnt))});
            }
        if (const ORef br = bg::res(q, mp, o + 0x28))
            for (uint32_t k = 0; k < nb; k++) {
                const uint64_t bo = br->second + 0x28 * k;
                rec.binds.push_back({S_(q, br->first, bo), S_(q, br->first, bo + 8), S_(q, br->first, bo + 0x10)});
            }
        // the effect
        const auto ets = q.items_of("EFFECT_TABLE");
        if (ets.empty()) throw Stop("IndexError: no EFFECT_TABLE");
        const uint32_t ne = q.pu32(ets[0].page, ets[0].data_off());
        const Ref arr = bg::need(q, ets[0].page, ets[0].data_off() + 8);
        const uint64_t h = std::stoull(rec.hash, nullptr, 16);
        for (uint32_t k = 0; k < ne; k++) {
            const uint64_t eo = arr.second + 0x18ull * k;
            if (q.u64(q.pbase(arr.first) + eo + 0x10) != h) continue;
            const Ref ep = bg::need(q, arr.first, eo);
            const PakItem *eit = container(x, ep.first, ep.second);
            if (!eit) throw Stop("AttributeError: effect outside an item");
            rec.has_fx = true;
            rec.fx_idx = q.pu32(arr.first, eo + 8);
            rec.fx_x = q.pu32(arr.first, eo + 12);
            rec.fx_eoff = int64_t(ep.second) - int64_t(eit->data_off());
            rec.fx = closure(x, *eit);
            break;
        }
        // textures bound by the material: VRAM_DESC (+ resident texels) and the GEOMETRY_1 texture entry
        if (!x.has_tex) {
            x.has_tex = true;
            for (const PakTexture &t : q.textures()) x.tex[t.path] = t;
            const uint64_t nt = q.u64(q.pbase(pg) + b + 0x38);
            if (const ORef tr = bg::res(q, pg, b + 0x40))
                for (uint64_t k = 0; k < nt; k++) {
                    const uint64_t to = tr->second + 0x28 * k;
                    x.gtex[S_(q, tr->first, to)] = slice(q, q.pbase(tr->first) + to + 8, 0x20);
                }
        }
        for (const MatBind &bnd : rec.binds) {
            const std::string &full = bnd.full;
            auto ti = x.tex.find(full + ".ndb");
            if (ti == x.tex.end()) ti = x.tex.find(full);
            MatTex e;
            if (auto g = x.gtex.find(full); g != x.gtex.end()) e.gtex = g->second;
            if (ti != x.tex.end() && !has(bnd.param, "LightMap") && ti->second.vramSize <= 0x80000) {
                const PakTexture &t = ti->second;
                e.has_desc = true;
                e.path = t.path;
                e.desc = slice(q, q.pbase(t.page) + t.off + 0x20, 0x40);
                e.texels = q.vram_bytes(t.vramOffset, t.vramSize);
            }
            auto ei = std::find_if(rec.textures.begin(), rec.textures.end(), [&](const auto &p) { return p.first == full; });
            if (ei == rec.textures.end()) rec.textures.push_back({full, e});
            else ei->second = e;
        }
        return rec;
    }
    return std::nullopt;
}

}  // namespace

Bundle extract(const fs::path &D, const Plan &plan) {
    std::set<std::string> tp;
    for (const auto &[pk, key] : plan.req_mats) tp.insert(pk);
    return extract_req(D, plan.req_mats, plan.req_tex, std::vector<std::string>(tp.begin(), tp.end()));
}

std::vector<std::string> pak_techs(const fs::path &pak) { return techs(Pak(pak)); }

Bundle extract_req(const fs::path &D, const std::vector<std::pair<std::string, std::string>> &req_mats,
                   const std::vector<std::pair<std::string, std::string>> &req_tex, const std::vector<std::string> &req_techs) {
    Bundle out;
    std::map<std::string, XPak> cache;
    auto P = [&](const std::string &fn) -> XPak & {
        if (!cache.count(fn)) {
            if (cache.size() > 6) cache.clear();
            XPak x;
            x.q = std::make_unique<Pak>(D / upath(fn));
            for (const PakItem &it : x.q->items) x.idx[it.page].push_back(&it);
            for (auto &[pg, L] : x.idx) std::stable_sort(L.begin(), L.end(), [](const PakItem *a, const PakItem *b) { return a->off < b->off; });
            cache.emplace(fn, std::move(x));
        }
        return cache.at(fn);
    };
    for (const std::string &fn : req_techs) out.techs[fn] = techs(*P(fn).q);
    for (const auto &[fn, key] : req_mats) {
        XPak &x = P(fn);
        std::optional<MatRec> r = material(x, key);
        if (!r) throw Stop("bundle errors: no material " + key + " in " + fn);
        out.mats.push_back({{fn, key}, std::move(*r)});
        if (!out.techs.count(fn)) out.techs[fn] = techs(*x.q);
    }
    for (const auto &[fn, path] : req_tex) {
        try {
            XPak &x = P(fn);
            if (!x.has_tex2) {
                x.has_tex2 = true;
                for (const PakTexture &c : x.q->textures()) x.tex2[c.path] = c;   // the last of a path wins (a dict)
            }
            auto ti = x.tex2.find(path);
            if (ti == x.tex2.end()) continue;
            const PakTexture tt = ti->second;
            BundleTex b;
            b.pak = fn;
            b.path = path;
            b.desc = slice(*x.q, x.q->pbase(tt.page) + tt.off + 0x20, 0x40);
            b.texels = x.q->vram_bytes(tt.vramOffset, tt.vramSize);
            out.tex.push_back(std::move(b));
        } catch (const std::exception &) {
        }
    }
    return out;
}

std::vector<BundleTex> default_lightmaps(const fs::path &pak_dir) {
    static const char *PATHS[] = {
        "d:/dev/CP9/Data/U3/CookData/ND/build/texture2/art/general/textures/default/default-lightmap0.tga/121EDD38E3116400.ndb",
        "d:/dev/CP9/Data/U3/CookData/ND/build/texture2/art/general/textures/default/default-lightmap1.tga/B1C390E18A2F0DF2.ndb"};
    const Pak q(pak_dir / "grave-02-hollowhulk-front-low.pak");
    std::vector<BundleTex> out;
    const std::vector<PakTexture> ts = q.textures();
    for (const char *path : PATHS) {
        const PakTexture *t = nullptr;
        for (const PakTexture &c : ts)
            if (c.path == path) t = &c;
        if (!t) throw Stop(std::string("default lightmaps not found in grave-02-hollowhulk-front-low.pak: ") + path);
        BundleTex b;
        b.pak = "fort-multi-stage.pak";
        b.path = path;
        b.desc = slice(q, q.pbase(t->page) + t->off + 0x20, 0x40);   // the converter keeps desc[:0x38] and rewrites +0..+0x14
        b.texels = q.vram_bytes(t->vramOffset, t->vramSize);
        out.push_back(std::move(b));
    }
    return out;
}

}  // namespace u3::stage
