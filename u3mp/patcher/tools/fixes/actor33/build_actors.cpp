// Recipe step convert-actors-r3: the converted actor paks (actor33/ps4) of overlay-port / -r2 / -r3 / -mp and
// overlay-mp's net.pak, rebuilt from the player's PS3 + PS4 files with the current converter.
//   u3fix actors --project P --job J --ps4 D --ps3-disc D --ps3-update D --ps3-data2 D
//                [--apply] [--overlays port,r2,r3,mp] [--only name,...] [--allow-equivalent] [--jobs N]
//
// jobs.tsv: overlay, name, sha256, status, method, toolset (ignored: the current converter always), PS3 source, retail
// template (a PS3 cut-out map on a template without a cut-out material: actor::transparency_template picks another),
// mode, texture cap (only for textures a retail texture dictionary streams), env (json, PS4ACTOR_*), post. A file
// already at its sha256 is left alone; a 'base' row
// never replaces an existing file (a later step owns it); other statuses need --allow-equivalent. A build whose sha256
// differs from an identical / base row is reported (DIFF) and not installed. Replaced files are backed up once to
// work/backup-actor33/. Lab rows (no Lab maps on PC) and rows hidden by a higher overlay are not built.
// Methods, all C++: ps4actor and rename (dctools/ps4actor.cpp), anim (anim.cpp), wpnmods and convert_net (ps4mats.cpp);
// own-skeleton props with collision need the Havok tables of step actors-hk-tables (TMP/hk). PS3 sources and the
// animstream extract come from the player's archives (dctools/psarc.cpp).
#include "proc.hpp"
#include "psarc.hpp"
#include "anim.hpp"
#include "hkconvert.hpp"
#include "ps4actor.hpp"
#include "ps4mats.hpp"
#include "ps4stage.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace u3 {
namespace {

std::string a_here, a_ps4, a_disc, a_update, a_data2;
fs::path a_tmp;
std::mutex a_extract_lock;

std::string ret() { return a_ps4 + "/u3data/build/main/actor33/ps4"; }
// the retail texture dictionaries' table: the textures it streams are the only ones that lose top mips to the cap
fs::path texdict() { return upath(a_ps4 + "/u3data/build/main/texturedict1/ps4/TextureDictionaries.csv"); }

// PS3 sources: source key -> archive
std::map<std::string, std::string> ps3_archives() {
    std::map<std::string, std::string> m = {{"disc", a_disc + "/build/main/actor33.psarc"}, {"goty-patch", a_disc + "/goty-patch.psarc"},
                                            {"goty-dlc", a_disc + "/goty-dlc.psarc"}, {"dlc-patch13", a_update + "/dlc-patch13.psarc"},
                                            {"dlc-patch18", a_update + "/dlc-patch18.psarc"}, {"data2-parts", a_data2 + "/build/main/actor-parts-7.psarc"}};
    for (auto it = m.begin(); it != m.end();) it = fs::exists(upath(it->second)) ? std::next(it) : m.erase(it);
    return m;
}

struct OpenArchive {
    std::unique_ptr<Psarc> a;
    std::map<std::string, size_t> by_name;   // basename -> entry index (a later one wins)
};
std::map<std::string, OpenArchive> a_open;

// PS3 source pak -> TMP/ps3/<src>/<name>
fs::path extract(const std::string &src, const std::string &name) {
    std::string sd = src;
    std::replace(sd.begin(), sd.end(), ':', '_');
    const fs::path d = a_tmp / "ps3" / upath(sd), f = d / upath(name);
    std::lock_guard<std::mutex> lock(a_extract_lock);
    if (fs::exists(f)) return f;
    if (src == "labsrc") throw Stop("no Lab maps on PC: Lab sources are not extracted");
    std::string p;
    if (src.rfind("pak23:", 0) == 0) p = a_disc + "/build/main/pak23.psarc";
    else {
        static const auto arcs = ps3_archives();
        auto it = arcs.find(src);
        if (it == arcs.end()) throw Stop("KeyError: '" + src + "'");
        p = it->second;
    }
    OpenArchive &o = a_open[p];
    if (!o.a) {
        o.a = std::make_unique<Psarc>(upath(p));
        for (size_t i = 0; i < o.a->names.size(); i++) {
            const std::string &n = o.a->names[i];
            o.by_name[n.substr(n.find_last_of('/') == std::string::npos ? 0 : n.find_last_of('/') + 1)] = i;
        }
    }
    auto e = o.by_name.find(name);
    if (e == o.by_name.end()) throw Stop("KeyError: '" + name + "'");
    fs::create_directories(d);
    Bytes b = o.a->read(e->second + 1);
    write_file_replace(f, b);
    provide_read_cache(f, std::move(b));
    return f;
}

fs::path stm_dir() {   // the PS3 disc's animstream4.psarc, extracted
    const fs::path d = a_tmp / "stm";
    std::lock_guard<std::mutex> lock(a_extract_lock);
    if (fs::exists(d / ".ready")) return d;
    Psarc a(upath(a_disc + "/build/main/animstream4.psarc"));
    fs::create_directories(d);
    for (size_t i = 0; i < a.names.size(); i++) {
        const std::string &n = a.names[i];
        write_file_replace(d / upath(n.substr(n.find_last_of('/') == std::string::npos ? 0 : n.find_last_of('/') + 1)), a.read(i + 1));
    }
    write_text(d / ".ready", "ok");
    return d;
}

fs::path template_pak(const std::string &t, bool solo) {
    const fs::path tp = upath(ret() + "/" + t + ".pak");
    if (!solo) {
        prefetch_read(tp);
        return tp;
    }
    const fs::path d = a_tmp / "tsolo" / upath(t);   // ps4actor reads the retail material GUIDs of the template's folder
    std::lock_guard<std::mutex> lock(a_extract_lock);
    if (!fs::exists(d / upath(t + ".pak"))) {
        fs::create_directories(d);
        fs::copy_file(tp, d / upath(t + ".pak"), fs::copy_options::overwrite_existing);
    }
    prefetch_read(d / upath(t + ".pak"));
    return d / upath(t + ".pak");
}

// a real copy, never a hard link: later steps install and restore with copy2 (copy_file), which writes into the
// existing file, so a hard-linked backup would change with it
void backup_file(const fs::path &from, const fs::path &to, const Bytes *data = nullptr) {
    if (fs::exists(to)) return;
    fs::create_directories(to.parent_path());
    if (data) write_file_replace(to, *data);
    else copy2(from, to);
}

// mode -> ps4actor.convert keyword arguments and PS4ACTOR_* variables
bool mode_of(const std::string &m, json::Value &kw, Env &env) {
    struct Mode {
        const char *name;
        int sf;
        bool by_file, match, graft, own;
        const char *spread, *notmpl;
    };
    static const Mode MODES[] = {
        {"mp", 2, false, false, false, false, nullptr, nullptr}, {"mp-sf3", 3, false, false, false, false, nullptr, nullptr},
        {"mp-sf0", 0, false, false, false, false, nullptr, nullptr}, {"kit", 2, true, true, false, false, nullptr, nullptr},
        {"kit-nomatch", 2, true, false, false, false, nullptr, nullptr}, {"kit-spread", 2, true, true, false, false, "1", nullptr},
        {"kit-notmpl", 2, true, true, false, false, nullptr, "1"}, {"kit-spread-notmpl", 2, true, true, false, false, "1", "1"},
        {"graft", 3, true, true, true, false, nullptr, nullptr}, {"graft-spread", 3, true, true, true, false, "1", nullptr},
        {"graft-notmpl", 3, true, true, true, false, nullptr, "1"}, {"graft-spread-notmpl", 3, true, true, true, false, "1", "1"},
        {"own", 3, true, true, false, true, nullptr, nullptr}, {"own-spread", 3, true, true, false, true, "1", nullptr},
        {"own-notmpl", 3, true, true, false, true, nullptr, "1"}, {"own-spread-notmpl", 3, true, true, false, true, "1", "1"}};
    for (const Mode &x : MODES) {
        if (m != x.name) continue;
        kw = json::Value::object();
        kw.set("stream_flags", json::Value::integer(x.sf));
        if (x.by_file) kw.set("mat_by_file", json::Value::boolean(true));
        if (x.match) kw.set("match_streams", json::Value::boolean(true));
        if (x.graft) kw.set("graft", json::Value::boolean(true));
        if (x.own) kw.set("own_skel", json::Value::boolean(true));
        if (x.spread) env["PS4ACTOR_SPREAD_MATS"] = x.spread;
        if (x.notmpl) env["PS4ACTOR_NO_TMPL_TEX"] = x.notmpl;
        return true;
    }
    return false;
}

using Job = std::map<std::string, std::string>;

std::vector<std::string> post_list(const std::string &post) {
    std::vector<std::string> v;
    const std::string first = post.substr(0, post.find('|'));
    size_t a = 0;
    for (size_t k = 0; k <= first.size(); k++)
        if (k == first.size() || first[k] == '+') {
            if (k > a) v.push_back(first.substr(a, k - a));
            a = k + 1;
        }
    return v;
}

Bytes build(const Job &r) {
    const std::string &m = r.at("method");
    if (m == "wpnmods") {   // template: '<retail pak>|<retail sibling skin>|<skin,...>'
        const std::string &t = r.at("template");
        const size_t b1 = t.find('|'), b2 = t.find('|', b1 + 1);
        if (b1 == std::string::npos || b2 == std::string::npos) throw Stop("ValueError: wpnmods template needs 3 fields");
        std::vector<std::string> skins;
        const std::string sk = t.substr(b2 + 1);
        size_t a = 0;
        for (size_t k = 0; k <= sk.size(); k++)
            if (k == sk.size() || sk[k] == ',') { skins.push_back(sk.substr(a, k - a)); a = k + 1; }
        prefetch_read(upath(ret()) / upath(r.at("name")));
        for (const std::string &sk : skins) prefetch_read(upath(ret()) / upath(sk + ".pak"));
        Bytes d = mats::wpnmods(upath(ret()) / upath(r.at("name")), extract(r.at("source"), r.at("name")), t.substr(b1 + 1, b2 - b1 - 1), skins,
                                std::stoi(r.at("cap")), 2, texdict());
        if (r.at("post").find("cb3fix") != std::string::npos) d = actor::cb3fix(d);
        return d;
    }
    if (m == "convert_net") {
        prefetch_read(upath(a_ps4 + "/u3data/build/main/pak23/ps4/menu.pak"));
        return mats::convert_net(extract("pak23:", "net.pak"), upath(a_ps4 + "/u3data/build/main/pak23/ps4/menu.pak"));
    }
    if (m == "am") {
        // a character part on a retail material template (actor::Options::am, the template's own materials chosen by
        // actor::am_choice): mode 'retail-mats', or 'tint' for tintable head-wear (instance tint slots; 'tint-all' also
        // takes OVERLAY / UV_3 templates). Source 'newest': the newest PS3 archive that has the pak.
        const std::string &md = r.at("mode");
        if (md != "retail-mats" && md != "tint" && md != "tint-all") throw Stop("KeyError: am mode '" + md + "'");
        const std::string stem = r.at("name").substr(0, r.at("name").size() - 4);
        fs::path p3;
        for (const char *src : {"data2-parts", "dlc-patch18", "dlc-patch13", "disc"}) {
            try {
                p3 = extract(src, r.at("name"));
                break;
            } catch (const Stop &) {
            }
        }
        if (p3.empty()) throw Stop("no PS3 source for " + stem);
        const fs::path tpl0 = upath(ret()) / upath(r.at("template") + ".pak");
        prefetch_read(tpl0);
        const fs::path tpl = actor::transparency_template(p3, tpl0, upath(ret()), false);
        actor::Kw kw;
        kw.max_tex = std::stoi(r.at("cap"));
        actor::Options o;
        o.texdict = texdict();
        o.am = true;
        o.mat_choice = actor::am_choice(p3, tpl, md != "retail-mats", md == "tint");
        return actor::cb3fix(stage::cubefix_pak(actor::convert(p3, tpl, kw, o), p3));
    }
    if (m != "ps4actor" && m != "anim" && m != "rename") throw Stop("method " + m + " is not supported");
    const std::string &source = r.at("source");
    const size_t slash = source.find('/');
    const std::string src = source.substr(0, slash), other = slash == std::string::npos ? "" : source.substr(slash + 1);
    const fs::path ps3 = source != "-" ? extract(src, other.empty() ? r.at("name") : other) : fs::path();
    Bytes d;
    if (m == "anim") {   // dctools/anim.cpp
        std::vector<fs::path> dirs;
        if (r.at("mode").find("stm") != std::string::npos) dirs.push_back(stm_dir());
        d = anim::convert_pak(ps3, dirs);
    } else if (m == "rename") {   // a retail PS4 actor under the MP name
        prefetch_read(upath(ret()) / upath(r.at("template") + ".pak"));
        d = actor::rename(upath(ret()) / upath(r.at("template") + ".pak"), r.at("template"), r.at("name").substr(0, r.at("name").size() - 4));
    } else {   // dctools/ps4actor.cpp
        actor::Kw kw;
        actor::Options o;
        json::Value kwj;
        Env env;
        if (!mode_of(r.at("mode"), kwj, env)) throw Stop("KeyError: '" + r.at("mode") + "'");
        kw.stream_flags = int(kwj.get("stream_flags")->i);
        kw.mat_by_file = kwj.get("mat_by_file") != nullptr;
        kw.match_streams = kwj.get("match_streams") != nullptr;
        kw.graft = kwj.get("graft") != nullptr;
        kw.own_skel = kwj.get("own_skel") != nullptr;
        // (the cap only applies to textures a retail dictionary streams: o.texdict)
        kw.max_tex = std::stoi(r.at("cap"));
        o.texdict = texdict();
        const std::string &es = r.at("env");
        if (es != "" && es != "-" && es != "{}") {
            json::Value e;
            if (!json::parse(es, e)) throw Stop("bad env json for " + r.at("name"));
            for (const auto &[k, v] : e.fields) env[k] = env_text(v);
        }
        for (const auto &[k, val] : env) {
            if (k == "PS4ACTOR_DONORS") {   // retail donor paks by name
                size_t a = 0;
                for (size_t q = 0; q <= val.size(); q++)
                    if (q == val.size() || val[q] == ':') {
                        if (q > a) {
                            o.donors.push_back(upath(ret()) / upath(val.substr(a, q - a) + ".pak"));
                            prefetch_read(o.donors.back());
                        }
                        a = q + 1;
                    }
            } else if (k == "PS4ACTOR_SKEL3") {   // '<source>/<name>.pak': the PS3 pak whose skeleton a LOD pak uses
                const size_t s = val.find('/');
                o.skel3 = extract(val.substr(0, s), val.substr(s + 1));
                prefetch_read(*o.skel3);
            } else if (k == "PS4ACTOR_SB_EXACT") o.sb_exact = val == "1";
            else if (k == "PS4ACTOR_SB_DROP") o.sb_drop = val == "1";
            else if (k == "PS4ACTOR_FILL_STREAMS") o.fill_streams = val == "1";
            else if (k == "PS4ACTOR_SPREAD_MATS") o.spread_mats = val == "1";
            else if (k == "PS4ACTOR_NO_TMPL_TEX") o.no_tmpl_tex = val == "1";
            else if (k == "PS4ACTOR_CB3_MIN") o.cb3_min = std::stoi(val);
            else if (k == "PS4ACTOR_DROP_MAT") o.drop_mat = val;
            else if (k == "_rename") kw.rename = val;
            else if (k == "_skel") {
                kw.skel_tmpl = upath(ret()) / upath(val + ".pak");
                prefetch_read(*kw.skel_tmpl);
            }
            else throw Stop("unknown actor setting " + k);
        }
        // a PS3 material that cuts out or blends on a template without such a material: the retail actor on the same
        // skeleton that has one (actor::transparency_template)
        const fs::path tp = actor::transparency_template(ps3, upath(ret()) / upath(r.at("template") + ".pak"), upath(ret()), kw.own_skel, kw.skel_tmpl);
        d = actor::convert(ps3, template_pak(utf8(tp.stem()), r.at("post").find("|solo") != std::string::npos), kw, o);
    }
    // post passes: cubefix (PS3 cube maps on retail-copied descs), cb3fix (prototype scope-3 cbuffer capacity),
    // flatfix (the generated flat-color texture sRGB), cubetype (retail cube descs type 2)
    for (const std::string &p : post_list(r.at("post"))) {
        if (p == "cubefix") d = stage::cubefix_pak(d, ps3);
        else if (p == "cb3fix") d = actor::cb3fix(d);
        else if (p == "flatfix" || p == "cubetype") d = actor::vdesc_fix(d, p);
        else throw Stop("unknown post pass " + p);
    }
    return d;
}

std::set<std::pair<std::string, std::string>> shadowed_set() {
    std::set<std::pair<std::string, std::string>> s;
    const char *f = std::getenv("RECIPE_SHADOWED");
    if (!f || !*f || !fs::exists(upath(f))) return s;
    std::string t = text_file(upath(f));
    size_t a = 0;
    for (size_t k = 0; k <= t.size(); k++) {
        if (k < t.size() && t[k] != '\n') continue;
        std::string l = t.substr(a, k - a);
        a = k + 1;
        const size_t tab = l.find('\t');
        if (tab != std::string::npos) s.insert({l.substr(0, tab), l.substr(tab + 1)});
    }
    return s;
}

// the PS3 inputs' signature (paths, sizes and times of the .psarc files and net18.bin, three levels deep)
void sig_walk(const fs::path &top, const fs::path &dp, std::vector<std::string> &sig) {
    std::vector<std::string> files;
    std::vector<fs::path> dirs;
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(dp, ec)) (e.is_directory() ? (void)dirs.push_back(e.path()) : (void)files.push_back(utf8(e.path().filename())));
    std::sort(files.begin(), files.end());
    for (const std::string &n : files) {
        if (!(n.size() > 6 && n.compare(n.size() - 6, 6, ".psarc") == 0) && n != "net18.bin") continue;
#ifdef _WIN32
        struct _stat64 st;
        if (_wstat64((dp / upath(n)).wstring().c_str(), &st)) continue;
#else
        struct stat st;
        if (::stat((dp / upath(n)).c_str(), &st)) continue;
#endif
        sig.push_back(fmt("%s %lld %lld", win_path(utf8(fs::relative(dp / upath(n), top))).c_str(), (long long)st.st_size, (long long)st.st_mtime));
    }
    const std::string rel = utf8(fs::relative(dp, top));
    if (std::count(rel.begin(), rel.end(), '/') >= 3) return;
    for (const fs::path &d : dirs) sig_walk(top, d, sig);
}

// the inputs of actors; the PS3 extracts are dropped when the PS3 inputs changed. Returns the thread count.
int setup(const Args &a) {
    const std::string project = a.need("--project");
    a_here = a.need("--job"); a_ps4 = a.need("--ps4");
    a_disc = a.get("--ps3-disc", ""); a_update = a.get("--ps3-update", ""); a_data2 = a.get("--ps3-data2", "");
    a_tmp = upath(project) / "scratch" / "actor33-work";
    std::vector<std::string> sig;
    for (const std::string &f : {a_disc, a_update, a_data2}) {
        sig.push_back(f);
        if (!f.empty() && fs::is_directory(upath(f))) sig_walk(upath(f), upath(f), sig);
    }
    std::string s;
    for (size_t i = 0; i < sig.size(); i++) s += (i ? "\n" : "") + sig[i];
    const std::string h = sha256_hex(Bytes(s.begin(), s.end()));
    const fs::path stamp = a_tmp / ".ps3-inputs";
    if (!fs::exists(stamp) || py_rstrip(text_file(stamp)) != h) {
        std::error_code ec;
        fs::remove_all(a_tmp / "ps3", ec);
        fs::remove_all(a_tmp / "labsrc", ec);
        fs::create_directories(a_tmp);
        write_text(stamp, h);
    }
    // Havok reflection tables (step actors-hk-tables keeps them in TMP/hk): own-skeleton props with collision need them
    if (fs::exists(a_tmp / "hk" / "hk2010_ps3.json") && fs::exists(a_tmp / "hk" / "hk2011_ps4.json"))
        hk::load_tables(a_tmp / "hk" / "hk2010_ps3.json", a_tmp / "hk" / "hk2011_ps4.json");
    const char *rj = std::getenv("RECIPE_JOBS");
    const char *rc = std::getenv("RECIPE_CACHE_MB");
    const size_t cache_mb = a.val.count("--cache-mb") ? size_t(std::stoul(a.get("--cache-mb", "0")))
                                                      : (rc && *rc ? size_t(std::stoul(rc)) : size_t(2048));
    set_read_cache_mb(cache_mb);
    if (cache_mb) std::printf("read cache: %zu MB\n", cache_mb);
    return a.val.count("--jobs") ? std::stoi(a.get("--jobs", "1")) : (rj && *rj ? std::atoi(rj) : int(std::max(1u, std::thread::hardware_concurrency())));
}

}  // namespace

int actors(const std::vector<std::string> &argv) {
    const Args a(argv, {"--apply", "--allow-equivalent"},
                 {"--project", "--job", "--ps4", "--ps3-disc", "--ps3-update", "--ps3-data2", "--overlays", "--only", "--jobs", "--cache-mb"});
    const int jobs = setup(a);
    const fs::path work = upath(a.need("--project")) / "work";
    static const std::map<std::string, std::string> OVL = {{"port", "overlay-port"}, {"r2", "overlay-r2"}, {"r3", "overlay-r3"}, {"mp", "overlay-mp"}};
    std::set<std::string> ovs, only;
    {
        const std::string o = a.get("--overlays", "port,r2,r3,mp");
        size_t b = 0;
        for (size_t k = 0; k <= o.size(); k++)
            if (k == o.size() || o[k] == ',') { ovs.insert(OVL.at(o.substr(b, k - b))); b = k + 1; }
        const std::string on = a.get("--only", "");
        b = 0;
        for (size_t k = 0; k <= on.size(); k++)
            if (k == on.size() || on[k] == ',') {
                std::string x = on.substr(b, k - b);
                if (!x.empty()) only.insert(x.size() > 4 && x.compare(x.size() - 4, 4, ".pak") == 0 ? x : x + ".pak");
                b = k + 1;
            }
    }
    static const char *const COLS[] = {"overlay", "name", "sha256", "status", "method", "toolset", "source", "template", "mode", "cap", "env", "post"};
    std::vector<Job> rows;
    {
        const std::string t = text_file(upath(a_here + "/jobs.tsv"));
        size_t b = 0;
        for (size_t k = 0; k <= t.size(); k++) {
            if (k < t.size() && t[k] != '\n') continue;
            const std::string l = t.substr(b, k - b);
            b = k + 1;
            if (l.empty() || l[0] == '#' || py_split(l).empty()) continue;
            Job r;
            size_t c = 0, s = 0;
            for (size_t j = 0; j <= l.size() && c < 12; j++)
                if (j == l.size() || l[j] == '\t') { r[COLS[c++]] = l.substr(s, j - s); s = j + 1; }
            for (; c < 12; c++) r[COLS[c]] = "";
            if (ovs.count(r["overlay"]) && (only.empty() || only.count(r["name"]))) rows.push_back(r);
        }
    }
    size_t n0 = rows.size();
    rows.erase(std::remove_if(rows.begin(), rows.end(), [](const Job &r) { return r.at("source") == "labsrc"; }), rows.end());
    if (n0 != rows.size()) std::printf("%zu Lab jobs skipped (no Lab maps on PC)\n", n0 - rows.size());
    const auto hidden = shadowed_set();
    if (!hidden.empty()) {
        n0 = rows.size();
        rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const Job &r) {
                       return hidden.count({r.at("overlay"), "u3data/build/main/" + std::string(r.at("method") == "convert_net" ? "pak23" : "actor33") + "/ps4/" + r.at("name")}) != 0;
                   }), rows.end());
        if (n0 != rows.size()) std::printf("%zu jobs skipped (hidden by a higher overlay, the game never loads them)\n", n0 - rows.size());
    }
    std::fflush(stdout);
    const auto t0 = std::chrono::steady_clock::now();
    int same = 0, installed = 0, built_diff = 0, fail = 0, skipped = 0;
    struct Todo {
        Job r;
        fs::path dst;
    };
    std::vector<Todo> todo;
    for (const Job &r : rows) {
        const fs::path dst = work / upath(r.at("overlay")) / "u3data" / "build" / "main" / (r.at("method") == "convert_net" ? "pak23" : "actor33") / "ps4" / upath(r.at("name"));
        if (fs::exists(dst) && sha_file(dst) == r.at("sha256")) { same++; continue; }
        if (r.at("status") == "base" && fs::exists(dst)) { same++; continue; }   // a later recipe step owns this file now
        if (r.at("status") != "identical" && r.at("status") != "base" && !a.has("--allow-equivalent")) {
            skipped++;
            std::printf("skip (%s) %s %s\n", r.at("status").c_str(), r.at("overlay").c_str(), r.at("name").c_str());
            continue;
        }
        todo.push_back({r, dst});
    }
    std::vector<std::string> err(todo.size());
    std::vector<Bytes> early(todo.size());
    std::vector<std::string> early_hash(todo.size());
    // wpnmods / convert_net (their own processes, shared temp files) and the animstream extract first, here
    bool stm = false;
    for (const Todo &t : todo)
        if (t.r.at("method") == "anim" && t.r.at("mode").find("stm") != std::string::npos) stm = true;
    if (stm) stm_dir();
    for (size_t i = 0; i < todo.size(); i++) {
        const std::string &m = todo[i].r.at("method");
        if (m != "wpnmods" && m != "convert_net") continue;
        try {
            early[i] = build(todo[i].r);
            early_hash[i] = sha256_hex(early[i]);
        } catch (const std::exception &e) {
            err[i] = std::string(e.what()).substr(0, 200);
        }
    }
    const size_t batch = std::max<size_t>(8, size_t(std::max(1, jobs)) + 4);
    for (size_t start = 0; start < todo.size(); start += batch) {
        const size_t end = std::min(todo.size(), start + batch);
        std::set<std::string> tpls;
        for (size_t i = start; i < end; i++) {
            const Job &r = todo[i].r;
            const std::string &m = r.at("method");
            if ((m == "ps4actor" || m == "rename") && r.at("template") != "-") tpls.insert(r.at("template"));
        }
        for (const std::string &t : tpls) prefetch_read(upath(ret()) / upath(t + ".pak"));
        std::vector<Bytes> bout(end - start);
        std::vector<std::string> bhash(end - start);
        std::atomic<size_t> next{start};
        std::vector<std::thread> pool;
        for (int w = 0; w < std::max(1, std::min<int>(jobs, int(end - start))); w++)
            pool.emplace_back([&] {
                for (size_t i; (i = next++) < end;) {
                    const std::string &m = todo[i].r.at("method");
                    if (m == "wpnmods" || m == "convert_net") continue;
                    try {
                        bout[i - start] = build(todo[i].r);
                        bhash[i - start] = sha256_hex(bout[i - start]);
                    } catch (const std::exception &e) {
                        err[i] = std::string(e.what()).substr(0, 200);
                    }
                }
            });
        for (auto &t : pool) t.join();
        for (size_t i = start; i < end; i++) {   // installed in jobs.tsv order
            const Job &r = todo[i].r;
            const bool pre = r.at("method") == "wpnmods" || r.at("method") == "convert_net";
            const Bytes &b = pre ? early[i] : bout[i - start];
            const std::string &h = pre ? early_hash[i] : bhash[i - start];
            if (!err[i].empty() || b.empty()) {
                fail++;
                std::printf("FAIL %s %s %s\n", r.at("overlay").c_str(), r.at("name").c_str(), err[i].c_str());
                continue;
            }
            if (h != r.at("sha256") && (r.at("status") == "identical" || r.at("status") == "base")) {
                built_diff++;
                std::printf("DIFF %s %s %s != %s\n", r.at("overlay").c_str(), r.at("name").c_str(), h.substr(0, 16).c_str(), r.at("sha256").substr(0, 16).c_str());
                continue;
            }
            if (!a.has("--apply")) {
                std::printf("would install %s %s\n", r.at("overlay").c_str(), r.at("name").c_str());
                continue;
            }
            const fs::path &dst = todo[i].dst;
            fs::create_directories(dst.parent_path());
            if (fs::exists(dst)) {
                const fs::path bk = work / "backup-actor33" / upath(r.at("overlay")) / upath(r.at("name"));
                backup_file(dst, bk);
            }
            write_file_replace(dst, b);
            installed++;
        }
        std::vector<Bytes>().swap(bout);
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("actor33: same %d, installed %d, built_diff %d, fail %d, skipped %d, %.0f s%s\n", same, installed, built_diff, fail, skipped, secs,
                a.has("--apply") ? "" : " (dry run)");
    return fail || built_diff ? 1 : 0;
}

}  // namespace u3
