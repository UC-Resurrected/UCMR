// Recipe step dc-recipes: the overlay DC files convert-dc1 does not reproduce (the merged modules net.bin, menu.bin,
// modules.bin, medals, looks, scenes, vox*, weapon-*, net-always-loaded(.dci), ss-coop-adv-fort, the 3 dc2 part
// modules, overlay-r2 / overlay-port net / medals / modules / render-settings, and the 80 conversions convert-dc1 does
// not reproduce, with their .dci) rebuilt byte for byte from the player's PS4 and PS3 files. Replaces
// apply_recipes.py and dcrecipe.py apply.
//   u3fix dc-recipes --project P --dcconv D --ps4 D --ps3-disc D --ps3-update D --ps3-data2 D [--apply] [--only TEXT]
// D (tools/dcconv): dc-recipes.json.gz, one provenance recipe per file: copy ops (source, byte transform r raw / w
// 32-bit swap / h 16-bit halves swap / o script opcodes renumbered, offset, length), zero runs, a few literal bytes and
// the pointer slots (the relocation bitmap is recomputed from them); the sources are checked by sha256 and every
// rebuilt file must have the recorded sha256. net18-versions.json: other accepted versions of the server's net18.bin
// (their newer values land in the same copied slots, so only the size is checked), and for versions with another layout
// the offset runs that rebuild the reference layout from them ("remap"). schema-oldconv.json: the opcode map.
// Dry run by default; files already at the recorded hash are left alone.
#include "inflate.h"
#include "json.h"
#include "proc.hpp"
#include "psarc.hpp"
#include "u3fix.hpp"

#include <array>
namespace u3 {
namespace {

Bytes xform(const Bytes &buf, char x, const std::map<int, int> &opmap) {
    if (x == 'r') return buf;
    Bytes out;
    if (x == 'o') {
        out = buf;
        for (size_t i = 0; i + 3 < out.size(); i += 4)
            if (auto it = opmap.find(out[i]); it != opmap.end()) out[i] = uint8_t(it->second);
        return out;
    }
    out.assign(buf.size(), 0);
    const size_t n = buf.size() - buf.size() % 4;
    for (size_t i = 0; i < n; i += 4) {
        if (x == 'w') out[i] = buf[i + 3], out[i + 1] = buf[i + 2], out[i + 2] = buf[i + 1], out[i + 3] = buf[i];
        else if (x == 'h') out[i] = buf[i + 1], out[i + 1] = buf[i], out[i + 2] = buf[i + 3], out[i + 3] = buf[i + 2];
    }
    std::copy(buf.begin() + std::ptrdiff_t(n), buf.end(), out.begin() + std::ptrdiff_t(n));
    return out;
}

struct Fail : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Converter rule for the multiplayer menu.bin (overlay-r3), 28 Nov 2026: the Custom Game SETTINGS and ADVANCED
// SETTINGS rows open a drop-down box, as on PS3. Both games' menu data give these rows control kind 6, but PS3's page
// builder makes kind 6 and kind 13 the same drop-down box, while the PS4 game draws kind 6 as Bluepoint's inline
// spinner ("<>", left / right). Kind 13 is still the drop-down box on PS4 (Friendly Fire already uses it), so every
// kind-6 row of those two pages becomes kind 13; nothing else in the record changes (the box shows +0x60 = 5 rows).
// A row that is not one of the known option rows stops the step instead of being changed. Returns the rows changed.
const char *MENU_KEY = "overlay-r3/dc1/ps4/menu.bin";
int option_boxes(Bytes &menu) {
    constexpr uint32_t PAGES = 0x74ffceeb, PAGE = 0x80, RECORD = 0x68, SPINNER = 6, BOX = 13;
    constexpr uint32_t BOX_PAGES[] = {0x86799eee /* PAGE_CUSTOMRULES */, 0xd7a415a2 /* PAGE_CUSTOMRULES2 */};
    static const std::set<uint32_t> OPTION_ROWS = {   // label ids
        0x86da827f, 0x7f532e93, 0x07774a0d, 0x9b7aa1a9, 0x59f9b620, 0x3ddcb691, 0x8d95d2ba, 0x463a4618, 0x9c4e8022,
        0xfcac898d,                                                                         // SETTINGS (Variation: PS4)
        0x0e5d6fcf, 0xf3ea88d6, 0xceed2298, 0xf185a544, 0x34380570, 0xaf04feeb, 0x13d4e325, 0x72ce8aea};   // ADVANCED
    const Dc c(menu, "menu.bin");
    const auto table = c.bysid.find(PAGES);
    if (table == c.bysid.end()) throw Fail("menu.bin: no page table");
    const uint64_t t = table->second.second, pages = c.ptr(t + 8);
    const uint32_t n = c.u32(t);
    int changed = 0, found = 0;
    for (uint32_t k = 0; k < n; k++) {
        const uint64_t page = pages + uint64_t(k) * PAGE;
        const uint32_t id = c.u32(page + 8);
        if (std::find(std::begin(BOX_PAGES), std::end(BOX_PAGES), id) == std::end(BOX_PAGES)) continue;
        found++;
        const uint64_t list = c.ptr(page + 0x48), rows = c.u32(list), first = c.ptr(list + 8);
        for (uint64_t r = 0; r < rows; r++) {
            const uint64_t rec = first + r * RECORD;
            const uint32_t kind = c.u32(rec + 0x10), label = c.u32(rec + 0x28);
            const int16_t shown = int16_t(c.u32(rec + 0x60) & 0xffff), arg = int16_t(c.u32(rec + 0x5c) >> 16);
            if (kind == BOX && OPTION_ROWS.count(label)) continue;
            if (kind != SPINNER || !OPTION_ROWS.count(label) || c.u32(rec + 0x14) != 0 || shown != 5 || arg != 0)
                throw Fail(fmt("menu.bin: page %08x row %llu (label %08x, kind %u) is not a known option row", id,
                               (unsigned long long)r, label, kind));
            wr32le(&menu[rec + 0x10], BOX);
            changed++;
        }
    }
    if (found != 2) throw Fail(fmt("menu.bin: %d of the 2 Custom Game settings pages found", found));
    return changed;
}

// rebuild a recipe's file; accepted: other accepted versions (sha256) per source name
Bytes apply(const json::Value &rec, std::map<std::string, std::optional<Bytes>> &sources, const std::map<std::string, std::set<std::string>> &accepted,
            const std::map<int, int> &opmap) {
    bool other = false;
    const json::Value *srcs = rec.get("sources");
    if (!srcs) throw Stop("dc recipe without sources");
    for (const auto &[n, h] : srcs->fields) {
        auto it = sources.find(n);
        if (it == sources.end()) throw Fail("missing source " + n);
        if (!it->second) throw Fail("TypeError: a bytes-like object is required, not 'NoneType'");
        const std::string s = sha256_hex(*it->second);
        if (s != h.s) {
            auto v = accepted.find(n);
            if (v == accepted.end() || !v->second.count(s)) throw Fail("source " + n + " differs from the one the recipe was made from");
            other = true;
        }
    }
    std::map<std::pair<std::string, char>, Bytes> cache;
    Bytes out;
    for (const json::Value &op : rec.get("ops")->items) {
        const std::string &k = op.items.at(0).s;
        if (k == "z") out.resize(out.size() + size_t(op.items.at(1).i), 0);
        else if (k == "l") {
            const std::string &h = op.items.at(1).s;
            for (size_t i = 0; i + 1 < h.size(); i += 2) out.push_back(uint8_t(std::stoul(h.substr(i, 2), nullptr, 16)));
        } else {
            const std::string &name = op.items.at(1).s;
            const char x = op.items.at(2).s.empty() ? 'r' : op.items.at(2).s[0];
            const int64_t j = op.items.at(3).i, n = op.items.at(4).i;
            auto c = cache.find({name, x});
            if (c == cache.end()) c = cache.emplace(std::make_pair(name, x), xform(*sources.at(name), x, opmap)).first;
            const Bytes &b = c->second;
            const int64_t a = std::clamp<int64_t>(j, 0, int64_t(b.size())), e = std::clamp<int64_t>(j + n, a, int64_t(b.size()));
            out.insert(out.end(), b.begin() + a, b.begin() + e);
        }
    }
    const json::Value *slots = rec.get("slots");
    for (const json::Value &s : slots->items) {
        const int64_t o = s.items.at(0).i;
        const uint64_t v = uint64_t(s.items.at(1).i);
        if (o < 0 || o + 8 > int64_t(out.size())) throw Stop("struct.error: pack_into out of range");
        for (int k = 0; k < 8; k++) out[size_t(o + k)] = uint8_t(v >> (8 * k));
    }
    if (const json::Value *dc = rec.get("dc"); dc && dc->truthy()) {
        const int64_t reloc = rec.get("body_end")->i, nb = (reloc + 63) / 64;
        Bytes bm(size_t(nb), 0);
        for (const json::Value &s : slots->items) {
            const int64_t w = s.items.at(0).i / 8;
            bm.at(size_t(w / 8)) |= uint8_t(1 << (w % 8));
        }
        for (int k = 0; k < 4; k++) out.push_back(uint8_t(uint32_t(nb) >> (8 * k)));
        out.insert(out.end(), bm.begin(), bm.end());
    }
    if (int64_t(out.size()) != rec.get("size")->i || (!other && sha256_hex(out) != rec.get("sha256")->s)) throw Fail("rebuilt file differs from the recipe target");
    return out;
}

}  // namespace

int dc_recipes(const std::vector<std::string> &argv) {
    const Args a(argv, {"--apply"}, {"--project", "--dcconv", "--ps4", "--ps3-disc", "--ps3-update", "--ps3-data2", "--only"});
    const std::string project = a.need("--project"), main4 = a.need("--ps4") + "/u3data/build/main/";
    const fs::path dd = upath(a.need("--dcconv"));
    json::Value R;
    {
        const Bytes gz = read_file(dd / "dc-recipes.json.gz");
        if (!json::parse(inflate::gunzip(std::string(gz.begin(), gz.end())), R)) throw Stop("dc-recipes.json.gz is not JSON");
    }
    std::map<int, int> opmap;
    {
        json::Value s;
        if (!json::parse(text_file(dd / "schema-oldconv.json"), s)) throw Stop("schema-oldconv.json is not JSON");
        if (const json::Value *o = s.get("opmap"))
            for (const auto &[k, v] : o->fields) opmap[std::stoi(k)] = int(v.i);
    }
    // other accepted versions of the server's net18.bin (hashes only), and for newer versions whose layout differs
    // (the server added or removed entries) the offset runs that put their bytes back in the reference layout: the
    // recipes then read the reference layout with the server's newer values (net18-versions.json "remap": per sha256
    // the reference size, runs [reference offset, offset in that version, length] and header words [offset, value])
    const std::string data2_net = "ps3/data2:dc1/ps4/net.bin";
    std::map<std::string, std::set<std::string>> accepted;
    struct Remap {
        uint64_t size = 0;
        std::vector<std::array<uint64_t, 3>> runs;
        std::vector<std::pair<uint64_t, uint32_t>> words;
    };
    std::map<std::string, Remap> remaps;
    if (fs::exists(dd / "net18-versions.json")) {
        json::Value v;
        if (!json::parse(text_file(dd / "net18-versions.json"), v)) throw Stop("net18-versions.json is not JSON");
        if (const json::Value *acc = v.get("accepted")) {
            auto &s = accepted[data2_net];
            for (const auto &[h, x] : acc->fields) s.insert(h);
            for (const json::Value &h : acc->items) s.insert(h.s);
        }
        if (const json::Value *rm = v.get("remap"))
            for (const auto &[h, r] : rm->fields) {
                Remap m;
                m.size = uint64_t(r.get("size")->i);
                for (const json::Value &x : r.get("runs")->items)
                    m.runs.push_back({uint64_t(x.items.at(0).i), uint64_t(x.items.at(1).i), uint64_t(x.items.at(2).i)});
                for (const json::Value &x : r.get("words")->items) m.words.push_back({uint64_t(x.items.at(0).i), uint32_t(x.items.at(1).i)});
                remaps[h] = std::move(m);
            }
    }
    // a newer layout the import adapted on this PC (u3import, net18adapt.cpp): net18-remap.json next to that net18.bin,
    // in the same form, for exactly that file (its sha256)
    if (const std::string d2 = a.get("--ps3-data2", ""); !d2.empty() && fs::exists(upath(d2) / "net18-remap.json")) {
        json::Value r;
        if (json::parse(text_file(upath(d2) / "net18-remap.json"), r) && r.get("sha256") && r.get("size") && r.get("runs") &&
            r.get("words")) {
            Remap m;
            m.size = uint64_t(r.get("size")->i);
            for (const json::Value &x : r.get("runs")->items)
                m.runs.push_back({uint64_t(x.items.at(0).i), uint64_t(x.items.at(1).i), uint64_t(x.items.at(2).i)});
            for (const json::Value &x : r.get("words")->items) m.words.push_back({uint64_t(x.items.at(0).i), uint32_t(x.items.at(1).i)});
            remaps.emplace(r.get("sha256")->s, std::move(m));   // a release's own entry for the same file wins
        }
    }
    // a newer layout of the server's net18.bin rebuilt in the reference layout (unmapped bytes stay zero: entries the
    // server removed)
    auto to_reference_layout = [&](const Bytes &d) -> std::optional<Bytes> {
        auto f = remaps.find(sha256_hex(d));
        if (f == remaps.end()) return std::nullopt;
        const Remap &m = f->second;
        Bytes out(m.size, 0);
        for (const auto &r : m.runs) {
            if (r[0] + r[2] > out.size() || r[1] + r[2] > d.size()) throw Stop("net18-versions.json: a remap run is out of range");
            std::copy(d.begin() + std::ptrdiff_t(r[1]), d.begin() + std::ptrdiff_t(r[1] + r[2]), out.begin() + std::ptrdiff_t(r[0]));
        }
        for (const auto &[o, w] : m.words) {
            if (o + 4 > out.size()) throw Stop("net18-versions.json: a remap word is out of range");
            for (int k = 0; k < 4; k++) out[o + k] = uint8_t(w >> (24 - 8 * k));
        }
        return out;
    };
    Ps3Index idx(a.get("--ps3-disc", ""), a.get("--ps3-update", ""), a.get("--ps3-data2", ""));
    std::map<std::string, std::optional<Bytes>> cache;
    auto src = [&](const std::string &name) -> std::optional<Bytes> {
        auto it = cache.find(name);
        if (it != cache.end()) return it->second;
        const size_t c = name.find(':');
        const std::string kind = name.substr(0, c), rel = name.substr(c + 1);
        std::optional<Bytes> d;
        if (kind == "ps4") {
            if (!fs::exists(upath(main4 + rel))) throw Fail("[Errno 2] No such file or directory: '" + main4 + rel + "'");
            d = read_file(upath(main4 + rel));
        } else
            d = idx.read(rel, kind.substr(kind.find('/') + 1));
        if (d && name == data2_net)
            if (std::optional<Bytes> r = to_reference_layout(*d)) {
                std::printf("the server's net18.bin %s has a newer layout: rebuilt in the reference layout\n", sha256_hex(*d).substr(0, 16).c_str());
                accepted[data2_net].insert(sha256_hex(*r));
                d = std::move(r);
            }
        cache[name] = d;
        return d;
    };
    std::vector<std::pair<std::string, int>> tot;
    auto count = [&](const std::string &k) {
        for (auto &x : tot)
            if (x.first == k) { x.second++; return; }
        tot.push_back({k, 1});
    };
    std::vector<const std::pair<std::string, json::Value> *> recs;
    for (const auto &f : R.fields) recs.push_back(&f);
    std::stable_sort(recs.begin(), recs.end(), [](const auto *x, const auto *y) { return x->first < y->first; });
    const std::string only = a.get("--only", "");
    for (const auto *kv : recs) {
        const std::string &key = kv->first;
        const json::Value &rec = kv->second;
        if (!only.empty() && key.find(only) == std::string::npos) continue;
        if (key.find("/dc1/ps4/render-settings/") != std::string::npos) {   // convert-dc1 makes these exactly (dcconv render-settings rule)
            count("render-settings (convert-dc1)");
            continue;
        }
        const size_t sl = key.find('/');
        const fs::path dst = upath(project + "/work/" + key.substr(0, sl) + "/u3data/build/main/" + key.substr(sl + 1));
        const std::string want = rec.get("sha256")->s;
        const bool menu = key == MENU_KEY;   // the recipe's bytes plus the option-box rule: compared with dst below
        if (!menu && fs::exists(dst) && sha_file(dst) == want) {
            count("already");
            continue;
        }
        Bytes out;
        bool reference = false;
        try {
            std::map<std::string, std::optional<Bytes>> s;
            for (const auto &[n, h] : rec.get("sources")->fields) s[n] = src(n);
            out = apply(rec, s, accepted, opmap);
            reference = sha256_hex(out) == want;
            if (menu) std::printf("menu.bin: %d Custom Game option rows open a drop-down box\n", option_boxes(out));
        } catch (const Fail &ex) {
            count("failed");
            std::printf("FAILED %s %s\n", key.c_str(), ex.what());
            continue;
        } catch (const Stop &ex) {
            count("failed");
            std::printf("FAILED %s %s\n", key.c_str(), ex.what());
            continue;
        }
        if (menu && fs::exists(dst) && read_file(dst) == out) {
            count("already");
            continue;
        }
        if (!reference) {   // built from another accepted net18.bin: the reference structure with the server's newer values
            if (fs::exists(dst) && read_file(dst) == out) {
                count("already");
                continue;
            }
            count("rebuilt (newer server net18)");
            std::printf("rebuilt from the newer server net18.bin: %s\n", key.c_str());
        } else
            count("rebuilt");
        if (a.has("--apply")) {
            fs::create_directories(dst.parent_path());
            write_file_replace(dst, out);
        }
    }
    std::string d = "{";
    int failed = 0;
    for (size_t i = 0; i < tot.size(); i++) {
        d += (i ? ", '" : "'") + tot[i].first + "': " + std::to_string(tot[i].second);
        if (tot[i].first == "failed") failed = tot[i].second;
    }
    std::printf("dc-recipes: %s} %s\n", d.c_str(), a.has("--apply") ? "" : "(dry run, nothing written)");
    return failed ? 1 : 0;
}

}  // namespace u3
