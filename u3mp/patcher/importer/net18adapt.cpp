// importer/net18adapt.cpp: a newer layout of the server's net18.bin, adapted on the player's PC.
//
// The DC recipes copy bytes out of the server's net18.bin at fixed offsets of one layout (the reference layout). When
// the community server's game data changes its layout, tools/dcconv/net18-versions.json needs a "remap" for it: the
// runs that put the new file's bytes back in the reference layout. Until a release adds it, the import would stop.
// Here the importer makes that remap itself, from a net18.bin version it already knows and that is on this PC (the
// previous download, or the game data of the player's RPCS3): the two files' objects (the module's symbol values and
// every pointer target; an object ends where the next one starts) are matched in file order by content (pointer values
// ignored, so objects that only moved still match), objects that changed are paired by size, the rest word by word,
// and strings are found by their bytes. Those runs (known version -> new) are chained with the known version's own
// remap (reference -> known version). Words the rebuild must keep as they were (the module header, array counts the
// new version changed, words other files' recipes read) keep the known version's value in the reference layout.
// The result is used only when every byte the recipes read from this file is accounted for (the safety check);
// otherwise the import stops as before and asks for the version to be reported. It is written next to the new
// net18.bin as net18-remap.json, where the import's input check and the dc-recipes step read it.
#include "importer.hpp"
#include "inflate.h"   // launcher/native/inflate.h
#include "recipe.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <set>

namespace u3imp {

namespace {

bool is_file(const fs::path &p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

using Run = std::array<uint64_t, 3>;   // reference offset, offset in the other file, length

struct Remap {
    uint64_t size = 0;
    std::vector<Run> runs;
    std::vector<std::pair<uint64_t, uint32_t>> words;   // reference offset, big-endian word written over the runs
};

std::optional<Remap> remap_from(const Value &r) {
    const Value *size = r.get("size"), *runs = r.get("runs"), *words = r.get("words");
    if (!size || !runs || !words) return std::nullopt;
    Remap m;
    m.size = uint64_t(size->i);
    for (const Value &x : runs->items)
        if (x.items.size() == 3) m.runs.push_back({uint64_t(x.items[0].i), uint64_t(x.items[1].i), uint64_t(x.items[2].i)});
    for (const Value &x : words->items)
        if (x.items.size() == 2) m.words.push_back({uint64_t(x.items[0].i), uint32_t(x.items[1].i)});
    return m;
}

Bytes rebuild(const Bytes &d, const Remap &m) {
    Bytes out(m.size, 0);
    for (const Run &r : m.runs)
        if (r[0] + r[2] <= out.size() && r[1] + r[2] <= d.size())
            std::memcpy(out.data() + r[0], d.data() + r[1], size_t(r[2]));
    for (const auto &[o, w] : m.words)
        if (o + 4 <= out.size())
            for (int k = 0; k < 4; k++) out[o + k] = uint8_t(w >> (24 - 8 * k));
    return out;
}

uint32_t be32(const Bytes &b, uint64_t o) {
    return uint32_t(b[o]) << 24 | uint32_t(b[o + 1]) << 16 | uint32_t(b[o + 2]) << 8 | uint32_t(b[o + 3]);
}

// --------------------------------------------------------------------------------------- matching (old file -> new)
struct Obj {
    uint64_t off, size, key;
};

std::vector<Obj> objects(const u3::Dc &c) {
    std::set<uint64_t> starts;
    for (const u3::Sym &s : c.syms) starts.insert(s.off);
    for (uint64_t p : c.ptrs) starts.insert(c.ptr(p));
    std::vector<uint64_t> st;
    for (uint64_t s : starts) if (s < c.reloc) st.push_back(s);
    std::vector<Obj> v;
    for (size_t i = 0; i < st.size(); i++) {
        const uint64_t end = i + 1 < st.size() ? st[i + 1] : c.reloc;
        uint64_t h = 1469598103934665603ull ^ (end - st[i]);
        for (uint64_t o = st[i]; o + 4 <= end; o += 4) {
            const uint32_t w = c.isptr(o) ? 0xfffffffeu : c.u32(o);
            h = (h ^ w) * 1099511628211ull;
        }
        v.push_back({st[i], end - st[i], h});
    }
    return v;
}

// pairs (i, j) of a longest common subsequence of a and b (Myers' O(ND) diff, with the path rebuilt)
std::vector<std::pair<size_t, size_t>> lcs(const std::vector<uint64_t> &a, const std::vector<uint64_t> &b) {
    const long n = long(a.size()), m = long(b.size()), max = n + m;
    std::vector<long> v(size_t(2 * max + 2), 0);
    std::vector<std::vector<long>> trace;
    long d_end = -1;
    for (long d = 0; d <= max && d_end < 0; d++) {
        trace.push_back(v);
        for (long k = -d; k <= d; k += 2) {
            long x = (k == -d || (k != d && v[size_t(k - 1 + max)] < v[size_t(k + 1 + max)])) ? v[size_t(k + 1 + max)]
                                                                                              : v[size_t(k - 1 + max)] + 1;
            long y = x - k;
            while (x < n && y < m && a[size_t(x)] == b[size_t(y)]) x++, y++;
            v[size_t(k + max)] = x;
            if (x >= n && y >= m) { d_end = d; break; }
        }
    }
    std::vector<std::pair<size_t, size_t>> out;
    long x = n, y = m;
    for (long d = d_end; d > 0; d--) {
        const std::vector<long> &pv = trace[size_t(d)];
        const long k = x - y;
        const long pk = (k == -d || (k != d && pv[size_t(k - 1 + max)] < pv[size_t(k + 1 + max)])) ? k + 1 : k - 1;
        const long px = pv[size_t(pk + max)], py = px - pk;
        while (x > px && y > py) out.push_back({size_t(--x), size_t(--y)});
        x = px, y = py;
    }
    while (x > 0 && y > 0) out.push_back({size_t(--x), size_t(--y)});
    std::reverse(out.begin(), out.end());
    return out;
}

// runs (old offset, new offset, length) that place the old file's bytes in the new one
std::vector<Run> match(const u3::Dc &a, const u3::Dc &b) {
    const std::vector<Obj> oa = objects(a), ob = objects(b);
    std::vector<uint64_t> ka, kb;
    for (const Obj &o : oa) ka.push_back(o.key);
    for (const Obj &o : ob) kb.push_back(o.key);
    std::vector<std::pair<size_t, size_t>> pairs = lcs(ka, kb);
    std::vector<Run> raw;
    auto words = [](const u3::Dc &c, const std::vector<Obj> &o, const std::vector<size_t> &idx, std::vector<uint64_t> &off,
                    std::vector<uint64_t> &key) {
        for (size_t i : idx)
            for (uint64_t w = o[i].off; w + 4 <= o[i].off + o[i].size; w += 4) {
                off.push_back(w);
                key.push_back(c.isptr(w) ? 0x1fffffffeull : c.u32(w));
            }
    };
    size_t pi = 0, pj = 0;
    pairs.push_back({oa.size(), ob.size()});
    for (const auto &[i, j] : pairs) {
        // the objects of the gap before this matched pair: equal sizes pair in order (an object whose values changed;
        // never the header object at offset 0, whose symbol table shifts with new symbols), the rest word by word
        std::vector<size_t> la, lb;
        std::vector<bool> used(j - pj, false);
        size_t q = 0;
        for (size_t x = pi; x < i; x++) {
            bool hit = false;
            for (size_t r = q; r < j - pj; r++) {
                const size_t y = pj + r;
                if (oa[x].off != 0 && ob[y].off != 0 && oa[x].size == ob[y].size) {
                    raw.push_back({oa[x].off, ob[y].off, oa[x].size});
                    used[r] = true;
                    q = r + 1;
                    hit = true;
                    break;
                }
            }
            if (!hit) la.push_back(x);
        }
        for (size_t r = 0; r < j - pj; r++) if (!used[r]) lb.push_back(pj + r);
        if (!la.empty() && !lb.empty()) {
            std::vector<uint64_t> wa, wb, kwa, kwb;
            words(a, oa, la, wa, kwa);
            words(b, ob, lb, wb, kwb);
            std::vector<std::pair<size_t, size_t>> wp = lcs(kwa, kwb);
            size_t qi = 0, qj = 0;
            wp.push_back({wa.size(), wb.size()});
            for (const auto &[x, y] : wp) {
                if (x - qi == y - qj)   // changed words between two matched ones: paired in order when the counts agree
                    for (size_t t = 0; t < x - qi; t++) raw.push_back({wa[qi + t], wb[qj + t], 4});
                if (x < wa.size()) raw.push_back({wa[x], wb[y], 4});
                qi = x + 1, qj = y + 1;
            }
        }
        if (i < oa.size()) raw.push_back({oa[i].off, ob[j].off, oa[i].size});
        pi = i + 1, pj = j + 1;
    }
    // bytes no run covers yet (strings are packed byte by byte): each string, else the stretch, found exactly in the
    // new file, nearest to where the runs before it put it
    std::vector<uint8_t> cov(a.d.size(), 0);
    for (const Run &r : raw) std::fill(cov.begin() + long(r[0]), cov.begin() + long(r[0] + r[2]), 1);
    std::sort(raw.begin(), raw.end());
    std::vector<Run> more;
    std::vector<std::pair<uint64_t, uint64_t>> pieces;
    const uint64_t body0 = oa.size() > 1 ? oa[1].off : 0;
    auto printable = [](uint8_t c) { return c >= 0x20 && c < 0x7f; };
    for (uint64_t x = body0; x < a.reloc;) {
        if (cov[x]) { x++; continue; }
        uint64_t e = x;
        while (e < a.reloc && !cov[e]) e++;
        uint64_t s = x;
        while (s > body0 && printable(a.d[s - 1])) s--;
        for (uint64_t p = s; p < e;) {
            uint64_t q = p;
            while (q < a.reloc && printable(a.d[q])) q++;
            if (q - p >= 3 && q < a.reloc && a.d[q] == 0) {
                pieces.push_back({p, q + 1 - p});
                p = q + 1;
            } else {
                uint64_t r = p;
                while (r < e && !printable(a.d[r])) r++;
                if (r == p) r = q > p ? q : p + 1;
                pieces.push_back({p, r - p});
                p = r;
            }
        }
        x = e;
    }
    for (const auto &[off, len] : pieces) {
        if (len < 4) continue;
        const uint8_t *p = a.d.data() + off;
        auto it = std::upper_bound(raw.begin(), raw.end(), Run{off, ~0ull, ~0ull});
        const int64_t delta = it == raw.begin() ? 0 : int64_t((it - 1)->at(1)) - int64_t((it - 1)->at(0));
        const int64_t want = int64_t(off) + delta;
        int64_t best = -1;
        for (uint64_t s = 0; s + len <= b.reloc; s++) {
            if (b.d[s] != p[0] || std::memcmp(b.d.data() + s, p, len) != 0) continue;
            if (best < 0 || std::llabs(int64_t(s) - want) < std::llabs(best - want)) best = int64_t(s);
        }
        if (best < 0) continue;
        for (uint64_t x = 0; x < len; x++) {
            if (cov[off + x]) continue;
            uint64_t e = x;
            while (e < len && !cov[off + e]) e++;
            more.push_back({off + x, uint64_t(best) + x, e - x});
            std::fill(cov.begin() + long(off + x), cov.begin() + long(off + e), 1);
            x = e;
        }
    }
    raw.insert(raw.end(), more.begin(), more.end());
    std::sort(raw.begin(), raw.end());
    std::vector<Run> runs;
    for (const Run &r : raw) {
        if (!runs.empty() && runs.back()[0] + runs.back()[2] == r[0] && runs.back()[1] + runs.back()[2] == r[1])
            runs.back()[2] += r[2];
        else
            runs.push_back(r);
    }
    return runs;
}

// runs reference -> b from runs reference -> a (sorted by a offset after the copy) and a -> b
std::vector<Run> chain(std::vector<Run> ra, const std::vector<Run> &ab) {
    std::sort(ra.begin(), ra.end(), [](const Run &x, const Run &y) { return x[1] < y[1]; });
    std::vector<Run> out;
    size_t j = 0;
    for (const Run &r : ra) {   // r: reference r[0] <- a r[1], r[2] bytes
        const uint64_t a0 = r[1], a1 = r[1] + r[2];
        while (j < ab.size() && ab[j][0] + ab[j][2] <= a0) j++;
        for (size_t k = j; k < ab.size() && ab[k][0] < a1; k++) {
            const uint64_t s = std::max(a0, ab[k][0]), e = std::min(a1, ab[k][0] + ab[k][2]);
            if (s < e) out.push_back({r[0] + (s - a0), ab[k][1] + (s - ab[k][0]), e - s});
        }
    }
    std::sort(out.begin(), out.end());
    std::vector<Run> merged;
    for (const Run &r : out) {
        if (!merged.empty() && merged.back()[0] + merged.back()[2] == r[0] && merged.back()[1] + merged.back()[2] == r[1])
            merged.back()[2] += r[2];
        else
            merged.push_back(r);
    }
    return merged;
}

struct Versions {
    std::set<std::string> accepted;
    std::map<std::string, Remap> remaps;
};

Versions shipped_versions() {
    Versions v;
    if (auto j = try_json(tools_dir() / "dcconv" / "net18-versions.json")) {
        if (const Value *a = j->get("accepted")) for (const auto &f : a->fields) v.accepted.insert(f.first);
        if (const Value *r = j->get("remap"))
            for (const auto &[h, x] : r->fields)
                if (auto m = remap_from(x)) v.remaps[h] = *m;
    }
    return v;
}

// the copy ops of the DC recipes that read the server's net18.bin: (target is net.bin itself, offset, length)
struct Read {
    bool net;
    uint64_t off, len;
};

std::vector<Read> recipe_reads() {
    std::vector<Read> out;
    const std::string gz = read_text(tools_dir() / "dcconv" / "dc-recipes.json.gz");
    Value R;
    if (!json::parse(inflate::gunzip(gz), R)) return out;
    for (const auto &[file, rec] : R.fields) {
        const bool net = file.size() >= 7 && file.compare(file.size() - 7, 7, "net.bin") == 0;
        const Value *ops = rec.get("ops");
        if (!ops) continue;
        for (const Value &op : ops->items)
            if (op.items.size() >= 5 && op.items[0].s == "c" && op.items[1].s == "ps3/data2:dc1/ps4/net.bin")
                out.push_back({net, uint64_t(op.items[3].i), uint64_t(op.items[4].i)});
    }
    return out;
}

std::string json_remap(const std::string &sha, const std::string &from, const Remap &m) {
    std::string o = "{\"sha256\":\"" + sha + "\",\"from\":\"" + from + "\",\"size\":" + std::to_string(m.size) + ",\"runs\":[";
    for (size_t k = 0; k < m.runs.size(); k++)
        o += (k ? "," : "") + std::string("[") + std::to_string(m.runs[k][0]) + "," + std::to_string(m.runs[k][1]) + "," +
             std::to_string(m.runs[k][2]) + "]";
    o += "],\"words\":[";
    for (size_t k = 0; k < m.words.size(); k++)
        o += (k ? "," : "") + std::string("[") + std::to_string(m.words[k].first) + "," + std::to_string(m.words[k].second) + "]";
    return o + "]}\n";
}

}  // namespace

// the remap made on this PC for the net18.bin in `usrdir` (net18-remap.json next to it, for exactly that file)
std::optional<std::string> local_net18_remap_text(const fs::path &usrdir, const std::string &sha) {
    const fs::path f = usrdir / "net18-remap.json";
    auto j = try_json(f);
    if (!j) return std::nullopt;
    const Value *s = j->get("sha256");
    if (!s || s->s != sha || !remap_from(*j)) return std::nullopt;
    return read_text(f);
}

// Makes <new_usrdir>/net18-remap.json for the net18.bin there from one of the known versions in `bases` (net18.bin
// files on this PC). Returns "" when it was made, else why not.
std::string adapt_net18(const fs::path &new_usrdir, const std::vector<fs::path> &bases) {
    const fs::path nf = new_usrdir / "net18.bin";
    if (!is_file(nf)) return "no net18.bin to adapt";
    const Bytes nb = u3::read_file(nf);
    const std::string nsha = sha256_hex(nb);
    const Versions known = shipped_versions();
    if (known.accepted.count(nsha)) return "";   // nothing to do
    std::vector<Read> reads = recipe_reads();
    if (reads.empty()) return "the DC recipes could not be read";
    std::string why = "no known net18.bin version on this PC to adapt from";
    for (const fs::path &base : bases) {
        if (!is_file(base)) continue;
        const Bytes kb = u3::read_file(base);
        const std::string ksha = sha256_hex(kb);
        if (ksha == nsha) continue;
        // the known version's own way into the reference layout
        Remap rk;
        if (auto it = known.remaps.find(ksha); it != known.remaps.end()) {
            rk = it->second;
        } else if (known.accepted.count(ksha)) {
            rk.size = kb.size();   // a reference-layout version: one run over the whole file
            rk.runs.push_back({0, 0, kb.size()});
        } else if (auto t = local_net18_remap_text(base.parent_path(), ksha)) {
            Value j;
            if (!json::parse(*t, j)) continue;
            rk = *remap_from(j);
        } else {
            continue;
        }
        try {
            const u3::Dc a(kb, S(base)), b(nb, S(nf));
            Remap out;
            out.size = rk.size;
            out.runs = chain(rk.runs, match(a, b));
            const Bytes RK = rebuild(kb, rk);
            Bytes RB = rebuild(nb, {out.size, out.runs, {}});
            std::map<uint64_t, uint32_t> lit;
            for (const auto &[o, w] : rk.words) lit[o] = w;
            for (uint64_t w = 0; w < 0x18 && w + 4 <= RK.size(); w += 4) lit[w] = be32(RK, w);   // module header
            for (uint64_t w = 539000 & ~3ull; w < 539016 && w + 4 <= RK.size(); w += 4) lit[w] = be32(RK, w);   // a recipe reads it
            // array counts (a small number right before a pointer slot): the rebuild keeps the known version's arrays
            std::map<uint64_t, uint64_t> k2r;   // known-version word offset -> reference offset
            for (const Run &r : rk.runs)
                for (uint64_t x = 0; x + 4 <= r[2]; x += 4)
                    if ((r[1] + x) % 4 == 0) k2r[r[1] + x] = r[0] + x;
            for (uint64_t p : a.ptrs) {
                if (p < 4 || p >= a.reloc || a.isptr(p - 4)) continue;
                auto it = k2r.find(p - 4);
                if (it == k2r.end() || it->second + 4 > RB.size()) continue;
                const uint32_t old = be32(RK, it->second), now = be32(RB, it->second);
                if (old != now && old < 0x10000 && now < 0x10000) lit[it->second] = old;
            }
            for (const auto &[o, w] : lit)
                if (o + 4 <= RB.size()) for (int k = 0; k < 4; k++) RB[o + k] = uint8_t(w >> (24 - 8 * k));
            // words other files' recipes read keep the known version's value (only net.bin takes the server's values)
            std::vector<uint8_t> covered(RB.size(), 0);
            for (const Run &r : out.runs) std::fill(covered.begin() + long(r[0]), covered.begin() + long(r[0] + r[2]), 1);
            for (const Read &r : reads)
                if (!r.net)
                    for (uint64_t x = r.off; x < r.off + r.len && x < RB.size(); x++)
                        if (RB[x] != RK[x]) {
                            const uint64_t w = x & ~3ull;
                            lit[w] = be32(RK, w);
                            for (int k = 0; k < 4; k++) RB[w + k] = RK[w + k];
                        }
            for (const auto &[o, w] : lit)
                if (o + 4 <= covered.size()) std::fill(covered.begin() + long(o), covered.begin() + long(o + 4), 1);
            // the safety check: every byte the recipes read from this file is placed by a run or kept as a word
            uint64_t missing = 0, total = 0;
            for (const Read &r : reads)
                for (uint64_t x = r.off; x < r.off + r.len; x++) {
                    total++;
                    if (x >= covered.size() || !covered[x]) { if (missing < 40 && std::getenv("U3_NET18_DEBUG")) log("unplaced " + std::to_string(x) + (r.net ? " net" : " other")); missing++; }
                }
            // a few bytes may have no place: entries the server removed (they read 0, as with the hand-made remaps of
            // 2026-11-20 and 2026-11-23, which leave the same 17 bytes unplaced); more than that is not adapted
            if (missing > 64) {
                why = "adapting from " + ksha.substr(0, 12) + " left " + std::to_string(missing) + " of " + std::to_string(total) +
                      " bytes the recipes read without a place in the new file";
                continue;
            }
            for (const auto &[o, w] : lit) out.words.push_back({o, w});
            write_text(new_usrdir / "net18-remap.json", json_remap(nsha, ksha, out));
            log("game data: the server's net18.bin " + nsha.substr(0, 12) + " has a layout this release does not know; adapted on this PC from " +
                ksha.substr(0, 12) + " (" + std::to_string(out.runs.size()) + " runs, " + std::to_string(out.words.size()) +
                " kept words; " + std::to_string(total - missing) + " of " + std::to_string(total) + " bytes the recipes read placed)");
            return "";
        } catch (const std::exception &e) {
            why = "adapting from " + ksha.substr(0, 12) + " failed: " + e.what();
        }
    }
    return why;
}

}  // namespace u3imp
