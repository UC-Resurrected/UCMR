// Three overlay-r3 DC edits of net.bin (and the module size table), replacing elim_pic.py, classic.py and
// skins_net_names.py of dctools:
//   u3fix elim-pic        --project P --ps3-data2 D
//       the Elimination playlist picture: net.bin playlist 10 (table 0xf985c7b6, entries 0x58) gets the picture string
//       'net-playlists/gametype-elimination' in its empty picture field; modules.bin's net.bin size word follows. Works
//       from the copies in work/backup-elim-pic (made once); 'already applied' when the picture is set.
//   u3fix classic         --project P --ps3-data2 D [NET] [--dry-run]
//       Classic netgames (final form of 2026-11-01 03:33): every netgame +0x68 = +0x6c (the kickback / weapon-mod flag
//       the helper reads), the 23 type-0x13 netgames loadout +0x2c 0x8cf8e1cc -> 0xd932420c, settings +0x64
//       0x63e35446 -> 0x1ad223b4, +0x6c 1 -> 0 (boosters on). Same size. Backup once to work/backup-classic. Nothing
//       to do when the table is in PS3 1.19 order (netgame-order-119) or has no old-form Classic netgame.
//   u3fix skins-net-names --project P [--apply]
//       weapon skins: every weapon-mod art path 'objects/weapons/<w>/<w>-mod/<name>' of net.bin (tables 0xe77d8fba /
//       0xa94f33c0, entries 0x28, fields +0x18 / +0x20) and weapon-art.bin (0x61d589d9 variants +0x58 / +0x60) is
//       repointed to the full 'path:name,guid,shader' name the converted weapon actor paks of overlay-r3 carry (the
//       first in sort order); the modules.bin size words follow. Backup once to work/backup-skins.
//   u3fix booster-table   --project P --net18 F [--apply]
//       the booster tables in online PS3 1.19 form (DATA2 net18.bin): records, ids, order, buddy table, id renames
//       (see booster_table below). Appended; modules.bin size word follows. Backup once to work/backup-booster-table.
#include "dc.hpp"
#include "proc.hpp"
#include "u3fix.hpp"

namespace u3 {
namespace {

const uint32_t NG = 0x3b9a067d, PL_TABLE = 0xf985c7b6;
const uint64_t NG3 = 0xe4, NG4 = 0xd0;   // netgame entry size PS3 1.19 / PS4

std::string dir_of(const std::string &project) { return project + "/work/overlay-r3/u3data/build/main/dc1/ps4/"; }
uint64_t u64le(const Bytes &d, uint64_t o) {
    if (o + 8 > d.size()) throw Stop("struct.error: unpack_from out of range");
    return rd64le(&d[o]);
}
void put64(Bytes &d, uint64_t o, uint64_t v) {
    if (o + 8 > d.size()) throw Stop("struct.error: pack_into out of range");
    wr64le(&d[o], v);
}
const std::pair<uint32_t, uint64_t> &sym_of(const Dc &c, uint32_t s) {
    auto it = c.bysid.find(s);
    if (it == c.bysid.end()) throw Stop(u3::fmt("KeyError: %u", s));
    return it->second;
}
// (count, array) of a table symbol (count u32, then a pointer)
std::pair<uint32_t, uint64_t> table(const Dc &c, uint32_t s) {
    const uint64_t o = sym_of(c, s).second;
    return {c.u32(o), c.ptr(o + uint64_t(c.ps))};
}
// is the net.bin netgame table in PS3 1.19 order (netgame-order-119)? exact with net18.bin, else > 256 netgames
bool is_ps3_order(const Dc &c, const std::string &net18) {
    const auto [n, ar] = table(c, NG);
    if (!net18.empty() && fs::is_regular_file(upath(net18))) {
        const Dc b(upath(net18));
        const auto [n3, ar3] = table(b, NG);
        if (n != n3) return false;
        for (uint32_t i = 0; i < n; i++)
            for (uint64_t k : {0, 4, 0xc})
                if (c.u32(ar + i * NG4 + k) != b.u32(ar3 + i * NG3 + k)) return false;
        return true;
    }
    return n > 256;
}

}  // namespace

bool net_is_ps3_order(const Dc &c, const std::string &net18) { return is_ps3_order(c, net18); }

int elim_pic(const std::vector<std::string> &argv) {
    const Args a(argv, {}, {"--project", "--ps3-data2"});
    const std::string R = a.need("--project") + "/work/", D = dir_of(a.need("--project")), BK = R + "backup-elim-pic/";
    fs::create_directories(upath(BK));
    for (const char *f : {"net.bin", "modules.bin"})
        if (!fs::exists(upath(BK + f))) copy2(upath(D + f), upath(BK + f));
    const Dc b(upath(a.get("--ps3-data2", "") + "/net18.bin"));
    {
        const Dc c(upath(D + "net.bin"));
        const uint64_t e = c.ptr(sym_of(c, PL_TABLE).second + 8) + 10 * 0x58;
        for (uint64_t k = 0; k < 0x58; k += 8)
            if (c.isptr(e + k) && c.cstr(c.ptr(e + k)) == std::optional<std::string>("net-playlists/gametype-elimination")) {
                std::printf("already applied: elimination picture is set\n");
                return 0;
            }
    }
    {   // the PS3 playlist stride and picture field (a report)
        const uint64_t o = sym_of(b, PL_TABLE).second, ar = b.ptr(o + 4);
        for (uint64_t st = 0x40; st < 0x60; st += 4) {
            std::vector<uint64_t> ok, ok0;
            for (uint64_t k = 0; k < st; k += 4) {
                if (b.isptr(ar + 10 * st + k) && b.str_or_empty(b.ptr(ar + 10 * st + k)).rfind("net-playlists", 0) == 0) ok.push_back(k);
                if (b.isptr(ar + k) && b.str_or_empty(b.ptr(ar + k)).rfind("net-playlists", 0) == 0) ok0.push_back(k);
            }
            if (!ok.empty() && ok == ok0) {
                std::string f;
                for (uint64_t k : ok) f += (f.empty() ? "'" : ", '") + u3::fmt("%#llx", (unsigned long long)k) + "'";
                std::printf("ps3 stride %#llx field [%s] %s\n", (unsigned long long)st, f.c_str(), b.str_or_empty(b.ptr(ar + 10 * st + ok[0])).c_str());
            }
        }
    }
    const Dc an(upath(BK + "net.bin"));
    const uint64_t ar = an.ptr(sym_of(an, PL_TABLE).second + 8), e = ar + 10 * 0x58;
    std::vector<uint64_t> fields;
    for (uint64_t k = 0; k < 0x58; k += 8)
        if (an.isptr(e + k)) fields.push_back(k);
    std::string fs_;
    for (uint64_t k : fields) fs_ += (fs_.empty() ? "'" : ", '") + u3::fmt("%#llx", (unsigned long long)k) + "'";
    std::printf("ps4 ptr fields [%s] %#x\n", fs_.c_str(), an.u32(e));
    std::optional<uint64_t> k;
    for (uint64_t x : fields)
        if (an.isptr(ar + x) && an.str_or_empty(an.ptr(ar + x)).rfind("net-playlists", 0) == 0) { k = x; break; }
    if (!k) throw Stop("IndexError: list index out of range");
    if (!an.str_or_empty(an.ptr(e + *k)).empty()) throw Stop("AssertionError: the picture field is not empty");
    DcEdit E(upath(BK + "net.bin"));
    E.putptr(e + *k, E.add_string("net-playlists/gametype-elimination"));
    const Bytes net = E.build();
    write_file_replace(upath(D + "net.bin"), net);
    Bytes m = read_file(upath(BK + "modules.bin"));
    if (u64le(m, 192352) != fs::file_size(upath(BK + "net.bin"))) throw Stop("AssertionError: modules.bin net.bin size word");
    put64(m, 192352, net.size());
    write_file_replace(upath(D + "modules.bin"), m);
    const Dc c(upath(D + "net.bin"));
    std::printf("%s %zu %llu\n", c.str_or_empty(c.ptr(c.ptr(sym_of(c, PL_TABLE).second + 8) + 10 * 0x58 + *k)).c_str(), net.size(), (unsigned long long)u64le(m, 192352));
    return 0;
}

int classic(const std::vector<std::string> &argv) {
    std::vector<std::string> rest;
    std::string net;
    for (const std::string &x : argv) {
        if (net.empty() && rest.empty() && !x.empty() && x.rfind("--", 0) != 0) net = x;   // sys.argv[1] when not a flag
        else rest.push_back(x);
    }
    const Args a(rest, {"--dry-run"}, {"--project", "--ps3-data2"});
    const std::string BK = a.need("--project") + "/work/backup-classic/";
    if (net.empty()) net = dir_of(a.need("--project")) + "net.bin";
    const std::string net18 = a.get("--ps3-data2", "").empty() ? "" : a.get("--ps3-data2", "") + "/net18.bin";
    const Dc c0(upath(net));
    if (is_ps3_order(c0, net18)) {
        std::printf("already applied: the netgame table is in PS3 1.19 order (netgame-order-119), Classic values are PS3's\n");
        return 0;
    }
    const uint64_t q = sym_of(c0, NG).second, ar = c0.ptr(q + 8);
    const uint32_t N = c0.u32(q);
    bool any_old = false;
    for (uint32_t i = 0; i < N; i++) any_old = any_old || (c0.u32(ar + i * 0xd0 + 0xc) == 0x13 && c0.u32(ar + i * 0xd0 + 0x2c) == 0x8cf8e1cc);
    if (!any_old) {
        std::printf("already applied: no Classic netgame in the old form\n");
        return 0;
    }
    DcEdit e(upath(net));
    int n = 0;
    for (uint32_t i = 0; i < N; i++) {
        const uint64_t g = ar + i * 0xd0;
        e.put32(g + 0x68, c0.u32(g + 0x6c));   // the kickback / mod flag
        if (c0.u32(g + 0xc) != 0x13) continue;
        if (!(c0.u32(g + 0x2c) == 0x8cf8e1cc && c0.u32(g + 0x64) == 0x63e35446 && c0.u32(g + 0x6c) == 1)) throw Stop("AssertionError: a Classic netgame not in the old form");
        e.put32(g + 0x2c, 0xd932420c);
        e.put32(g + 0x64, 0x1ad223b4);
        e.put32(g + 0x6c, 0);   // boosters on
        n++;
    }
    if (n != 23) throw Stop(u3::fmt("AssertionError: %d", n));
    const Bytes out = e.build();
    if (out.size() != e.orig.size()) throw Stop("AssertionError: size changed");
    if (a.has("--dry-run")) {
        std::printf("dry run: %d Classic netgames would change\n", n);
        return 0;
    }
    fs::create_directories(upath(BK));
    if (!fs::exists(upath(BK + "net.bin"))) copy2(upath(net), upath(BK + "net.bin"));
    write_file_replace(upath(net), out);
    const Dc c(upath(net));
    const uint64_t q2 = sym_of(c, NG).second, ar2 = c.ptr(q2 + 8);
    int now = 0;
    for (uint32_t i = 0; i < c.u32(q2); i++) now += c.u32(ar2 + i * 0xd0 + 0xc) == 0x13 && c.u32(ar2 + i * 0xd0 + 0x2c) == 0xd932420c;
    std::printf("%d %zu %d\n", n, out.size(), now);
    return 0;
}

int skins_net_names(const std::vector<std::string> &argv) {
    const Args a(argv, {"--apply"}, {"--project"});
    const std::string W = a.need("--project") + "/work/overlay-r3/u3data/build/main/", D = W + "dc1/ps4/", B = a.need("--project") + "/work/backup-skins/";
    // full material names (path:name,guid,shader) of the converted weapon actor paks
    std::map<std::string, std::set<std::string>> full;
    if (fs::is_directory(upath(W + "actor33/ps4")))
        for (const auto &ent : fs::directory_iterator(upath(W + "actor33/ps4"))) {
            const std::string fn = utf8(ent.path().filename());
            if (fn.size() < 4 || fn.compare(fn.size() - 4, 4, ".pak")) continue;
            const Bytes d = read_file(ent.path());
            static const std::string PRE = "objects/weapons/";
            auto c1 = [](uint8_t c) { return std::isalnum(c) || c == '_' || c == '.' || c == '/' || c == '-'; };
            auto c2 = [](uint8_t c) { return std::isalnum(c) || c == '_' || c == '.' || c == '-'; };
            size_t i = 0;
            while (true) {   // re.findall(rb'objects/weapons/[a-zA-Z0-9_./-]+:[a-zA-Z0-9_.-]+(?:,[^\0]*)?\0')
                auto it = std::search(d.begin() + std::ptrdiff_t(i), d.end(), PRE.begin(), PRE.end());
                if (it == d.end()) break;
                const size_t s = size_t(it - d.begin());
                size_t p = s + PRE.size();
                const size_t r1 = p;
                while (p < d.size() && c1(d[p])) p++;
                bool ok = p > r1 && p < d.size() && d[p] == ':';
                size_t end = 0;
                if (ok) {
                    const size_t r2 = ++p;
                    while (p < d.size() && c2(d[p])) p++;
                    ok = p > r2;
                    if (ok && p < d.size() && d[p] == ',') {
                        size_t z = p;
                        while (z < d.size() && d[z]) z++;
                        if (z < d.size()) end = z;
                        else ok = false;
                    } else if (ok && p < d.size() && d[p] == 0)
                        end = p;
                    else
                        ok = false;
                }
                if (!ok) {
                    i = s + 1;
                    continue;
                }
                const std::string x(d.begin() + std::ptrdiff_t(s), d.begin() + std::ptrdiff_t(end));
                full[x.substr(0, x.find(','))].insert(x);
                i = end + 1;
            }
        }
    {
        std::vector<std::string> multi;
        for (const auto &[k, v] : full)
            if (v.size() > 1) multi.push_back(k);
        if (!multi.empty()) std::printf("warning: several full names for %zu art paths (the first in sort order is used)\n", multi.size());
    }
    auto net_slots = [](const Dc &c) {
        std::vector<uint64_t> r;
        for (uint32_t sy : {0xe77d8fbau, 0xa94f33c0u}) {
            const uint64_t o = sym_of(c, sy).second, ar = c.ptr(o + 8);
            for (uint32_t i = 0; i < c.u32(o); i++) r.push_back(ar + i * 0x28ull + 0x18), r.push_back(ar + i * 0x28ull + 0x20);
        }
        return r;
    };
    auto art_slots = [](const Dc &c) {
        std::vector<uint64_t> r;
        const uint64_t o = sym_of(c, 0x61d589d9).second, vs = c.ptr(o + 16);
        for (uint32_t i = 0; i < c.u32(o); i++) {
            const uint64_t v = c.ptr(vs + i * 8ull);
            if (v) r.push_back(v + 0x58), r.push_back(v + 0x60);
        }
        return r;
    };
    {
        const Dc cn(upath(D + "net.bin"));
        for (uint64_t q : net_slots(cn))
            if (cn.ptr(q) && cn.str_or_empty(cn.ptr(q)).find(',') != std::string::npos) {
                std::printf("already applied: net.bin weapon-mod art names are full names\n");
                return 0;
            }
    }
    if (full.empty()) {
        std::fprintf(stderr, "no converted weapon actor paks with full material names in %sactor33/ps4\n", W.c_str());
        return 1;
    }
    std::vector<std::pair<std::string, Bytes>> outs;
    for (const auto &[fn, slots] : std::vector<std::pair<std::string, std::function<std::vector<uint64_t>(const Dc &)>>>{{"net.bin", net_slots}, {"weapon-art.bin", art_slots}}) {
        const Dc c(upath(D + fn));
        DcEdit e(upath(D + fn));
        int n = 0, done = 0;
        std::vector<std::string> miss;
        std::map<std::string, uint64_t> cache;
        for (uint64_t q : slots(c)) {
            const uint64_t p = c.ptr(q);
            if (!p) continue;
            const auto s = c.cstr(p);
            if (!s || s->empty() || s->rfind("objects/weapons/", 0) != 0) continue;
            if (s->find(',') != std::string::npos) {
                done++;
                continue;
            }
            std::string k = *s;
            if (k.find(':') == std::string::npos) {
                const size_t sl = k.rfind('/');
                if (sl != std::string::npos) k[sl] = ':';
            }
            auto f = full.find(k);
            if (f != full.end()) {
                const std::string &fname = *f->second.begin();
                if (!cache.count(fname)) cache[fname] = e.add_string(fname);
                e.putptr(q, cache[fname]);
                n++;
            } else
                miss.push_back(k);
        }
        Bytes out = e.build();
        std::string ms;
        for (size_t i = 0; i < std::min<size_t>(8, miss.size()); i++) ms += (ms.empty() ? "'" : ", '") + miss[i] + "'";
        std::printf("%s repointed %d already full %d miss [%s] %llu -> %zu\n", fn.c_str(), n, done, ms.c_str(), (unsigned long long)fs::file_size(upath(D + fn)), out.size());
        outs.push_back({fn, std::move(out)});
    }
    Bytes m = read_file(upath(D + "modules.bin"));
    for (const auto &[fn, out] : outs) {   // the module size words
        const uint64_t old = fs::file_size(upath(D + fn));
        std::vector<size_t> hits;
        for (size_t i = 0; i + 8 <= m.size(); i++)
            if (rd64le(&m[i]) == old) hits.push_back(i);
        if (hits.size() != 1) throw Stop("AssertionError: " + fn + " size word found " + std::to_string(hits.size()) + " times in modules.bin");
        put64(m, hits[0], out.size());
    }
    if (!a.has("--apply")) {
        std::printf("dry run, nothing written\n");
        return 0;
    }
    fs::create_directories(upath(B));
    for (const char *f : {"net.bin", "weapon-art.bin", "modules.bin"})
        if (!fs::exists(upath(B + f))) copy2(upath(D + f), upath(B + f));
    for (const auto &[fn, out] : outs) write_file_replace(upath(D + fn), out);
    write_file_replace(upath(D + "modules.bin"), m);
    std::printf("written\n");
    return 0;
}

// ---- booster-table: the booster tables of the online PS3 1.19 game (DATA2 net18.bin) ----------------------------
// The booster menus list the definitions of DC table 0x916e03b0 in table order, keeping those whose slot (+0xc) is
// the slot being edited and whose mode mask (+0x30: 1 competitive, 2 co-op) has the loadout's bit (game function
// 0x7596a0). The converted net.bin carries the PS4 retail table (= the PS3 disc table). An online PS3 1.19 game loads
// the whole net module from net18.bin, whose boosters differ: 57 instead of 58 (Deposit gone, Sugar Buddy replaced
// by Scavenger), 36 competitive and handicap boosters under new ids, other slots, order, flags and tier values.
//   * the booster ids old -> new come from the unlock table 0xe2e8998e: its booster entries (category 3) have the
//     same item number in both files; the sid (+8) of every category-3 entry of ours is renamed to net18's.
//   * table 0x916e03b0 becomes net18's: a new record array (PS4 records 0x38 bytes: +0, id +4, name +8, slot +0xc,
//     tiers +0x10, tier pointer +0x18, cost +0x20, +0x24, icon +0x28, flag +0x2c, modes +0x30), new tier arrays
//     (0x10 bytes: text, count, pointer) and new (stat id, value) pair arrays, all appended.
//   * table 0xcdbd1c65 (records of 4 words, the last one a booster id) becomes net18's (12 records instead of 7).
//   * table 0x4360c988 (per-item challenge lists; records PS4 0x20 bytes / PS3 0x18: id, name, entry count, two
//     words, pointer to the entries of 0x4c bytes, plain words) becomes net18's: its booster entries carry the new
//     ids and net18's numbers.
// Ids that are not booster ids (the tier stat ids, other tables that keep the old ids in net18 too) are not touched,
// and neither are the other modules: PS3 1.19 online replaces only the net module. Append-only (DcEdit); the
// modules.bin size word of 'net' follows. Idempotent; backup once to work/backup-booster-table.
int booster_table(const std::vector<std::string> &argv) {
    const Args a(argv, {"--apply"}, {"--project", "--net18"});
    const uint32_t BT = 0x916e03b0, UT = 0xe2e8998e, BU = 0xcdbd1c65, RT = 0x4360c988;
    const std::string D = dir_of(a.need("--project")), BK = a.need("--project") + "/work/backup-booster-table/";
    const Dc b(upath(a.need("--net18")));
    const Dc c(upath(D + "net.bin"));
    if (!b.be) throw Stop("--net18 is not a PS3 DC");
    using KP = std::pair<uint64_t, uint64_t>;
    // booster record fields, PS4 offset <- PS3 offset (the tier pointer is PS4 +0x18, PS3 +0x14)
    const KP F[] = {{0, 0}, {4, 4}, {8, 8}, {0xc, 0xc}, {0x10, 0x10}, {0x20, 0x18}, {0x24, 0x1c}, {0x28, 0x20}, {0x2c, 0x24}, {0x30, 0x28}};
    // the id map from the unlock tables (category 3, same item number)
    std::map<uint32_t, uint32_t> remap;   // old -> new, only changed ids
    {
        std::map<uint32_t, uint32_t> item_new;
        const auto [n3, a3] = table(b, UT);
        for (uint32_t i = 0; i < n3; i++)
            if (b.u32(a3 + i * 0x1c + 4) == 3) item_new[b.u32(a3 + i * 0x1c)] = b.u32(a3 + i * 0x1c + 8);
        const auto [n4, a4] = table(c, UT);
        for (uint32_t i = 0; i < n4; i++) {
            const uint64_t e = a4 + i * 0x28;
            if (c.u32(e + 4) != 3) continue;
            auto it = item_new.find(c.u32(e));
            if (it == item_new.end()) throw Stop(u3::fmt("booster unlock item %u is not in net18.bin", c.u32(e)));
            if (it->second != c.u32(e + 8)) remap[c.u32(e + 8)] = it->second;
        }
    }
    const auto [n18, ar18] = table(b, BT);
    const auto [nb18, ab18] = table(b, BU);
    // the in-place renames: (offset, new value)
    std::vector<std::pair<uint64_t, uint32_t>> put;
    {
        const auto [n4, a4] = table(c, UT);
        for (uint32_t i = 0; i < n4; i++) {
            const uint64_t e = a4 + i * 0x28;
            if (c.u32(e + 4) == 3 && remap.count(c.u32(e + 8))) put.push_back({e + 8, remap[c.u32(e + 8)]});
        }
    }
    const auto [nr18, ar18r] = table(b, RT);
    for (uint32_t i = 0; i < nr18; i++) {   // net18's challenge entries must be plain words
        const uint64_t r3 = ar18r + i * 0x18;
        if (!b.isptr(r3 + 0x14)) throw Stop(u3::fmt("net18 table 4360c988 record %u: no entry pointer", i));
        for (uint64_t k = 0; k < b.u32(r3 + 8) * 0x4cull; k += 4)
            if (b.isptr(b.ptr(r3 + 0x14) + k)) throw Stop(u3::fmt("net18 table 4360c988 record %u: a pointer in its entries", i));
    }
    // the booster and buddy tables equal net18's, field by field (tiers and stat pairs included)
    auto same_tables = [&](const Dc &x) {
        const auto [n, ar] = table(x, BT);
        if (n != n18) return false;
        for (uint32_t i = 0; i < n; i++) {
            const uint64_t r = ar + i * 0x38, r3 = ar18 + i * 0x2c;
            for (const auto &[k4, k3] : F)
                if (x.u32(r + k4) != b.u32(r3 + k3)) return false;
            const uint64_t t4 = x.ptr(r + 0x18), t3 = b.ptr(r3 + 0x14);
            for (uint32_t t = 0; t < x.u32(r + 0x10); t++) {
                const uint32_t cnt = b.u32(t3 + t * 0xc + 4);
                if (x.u32(t4 + t * 0x10) != b.u32(t3 + t * 0xc) || x.u32(t4 + t * 0x10 + 4) != cnt) return false;
                for (uint32_t k = 0; k < 2 * cnt; k++)
                    if (x.u32(x.ptr(t4 + t * 0x10 + 8) + 4ull * k) != b.u32(b.ptr(t3 + t * 0xc + 8) + 4ull * k)) return false;
            }
        }
        const auto [nb, ab] = table(x, BU);
        if (nb != nb18) return false;
        for (uint64_t k = 0; k < nb * 0x10ull; k += 4)
            if (x.u32(ab + k) != b.u32(ab18 + k)) return false;
        const auto [nr, arr] = table(x, RT);
        if (nr != nr18) return false;
        for (uint32_t i = 0; i < nr; i++) {
            const uint64_t r = arr + i * 0x20, r3 = ar18r + i * 0x18;
            for (uint64_t k = 0; k < 0x14; k += 4)
                if (x.u32(r + k) != b.u32(r3 + k)) return false;
            for (uint64_t k = 0; k < b.u32(r3 + 8) * 0x4cull; k += 4)
                if (x.u32(x.ptr(r + 0x18) + k) != b.u32(b.ptr(r3 + 0x14) + k)) return false;
        }
        return true;
    };
    if (put.empty() && same_tables(c)) {
        std::printf("already applied: the booster tables are net18's\n");
        return 0;
    }
    std::printf("booster tables from net18: %u boosters (was %u), %u buddy records (was %u), %u challenge lists, %zu ids renamed, %zu unlock sids renamed\n", n18,
                table(c, BT).first, nb18, table(c, BU).first, nr18, remap.size(), put.size());
    for (const auto &[o, n] : remap) std::printf("  %08x -> %08x\n", o, n);
    if (!a.has("--apply")) {
        std::printf("dry run: nothing written\n");
        return 0;
    }
    DcEdit e(upath(D + "net.bin"));
    for (const auto &[o, v] : put) e.put32(o, v);
    // the booster records, their tiers and stat pairs
    const uint64_t recs = e.alloc(n18 * 0x38ull, 16);
    for (uint32_t i = 0; i < n18; i++) {
        const uint64_t r = recs + i * 0x38ull, r3 = ar18 + i * 0x2c;
        for (const auto &[k4, k3] : F) e.raw32(r + k4, b.u32(r3 + k3));
        const uint32_t nt = b.u32(r3 + 0x10);
        const uint64_t t3 = b.ptr(r3 + 0x14);
        const uint64_t tiers = e.alloc(std::max<uint32_t>(nt, 1) * 0x10ull, 16);
        e.putptr(r + 0x18, tiers);
        for (uint32_t t = 0; t < nt; t++) {
            const uint32_t cnt = b.u32(t3 + t * 0xc + 4);
            e.raw32(tiers + t * 0x10, b.u32(t3 + t * 0xc));
            e.raw32(tiers + t * 0x10 + 4, cnt);
            const uint64_t pairs = e.alloc(std::max<uint32_t>(cnt, 1) * 8ull, 16);
            for (uint32_t k = 0; k < 2 * cnt; k++) e.raw32(pairs + 4ull * k, b.u32(b.ptr(t3 + t * 0xc + 8) + 4ull * k));
            e.putptr(tiers + t * 0x10 + 8, pairs);
        }
    }
    const uint64_t buddies = e.alloc(nb18 * 0x10ull, 16);
    for (uint64_t k = 0; k < nb18 * 0x10ull; k += 4) e.raw32(buddies + k, b.u32(ab18 + k));
    const uint64_t chal = e.alloc(nr18 * 0x20ull, 16);   // the challenge lists: records, then each record's entries
    for (uint32_t i = 0; i < nr18; i++) {
        const uint64_t r = chal + i * 0x20ull, r3 = ar18r + i * 0x18;
        for (uint64_t k = 0; k < 0x14; k += 4) e.raw32(r + k, b.u32(r3 + k));
        const uint64_t n = b.u32(r3 + 8) * 0x4cull, ent = e.alloc(std::max<uint64_t>(n, 4), 16);
        for (uint64_t k = 0; k < n; k += 4) e.raw32(ent + k, b.u32(b.ptr(r3 + 0x14) + k));
        e.putptr(r + 0x18, ent);
    }
    e.put32(sym_of(c, RT).second, nr18);
    e.putptr(sym_of(c, RT).second + 8, chal);
    e.put32(sym_of(c, BT).second, n18);
    e.putptr(sym_of(c, BT).second + 8, recs);
    e.put32(sym_of(c, BU).second, nb18);
    e.putptr(sym_of(c, BU).second + 8, buddies);
    const Bytes out = e.build();
    Bytes m = read_file(upath(D + "modules.bin"));
    const uint64_t slot = module_size_slot(Dc(Bytes(m), "modules.bin"), "net");
    if (u64le(m, slot) != e.orig.size()) throw Stop("AssertionError: modules.bin net.bin size word");
    put64(m, slot, out.size());
    fs::create_directories(upath(BK));
    for (const char *f : {"net.bin", "modules.bin"})
        if (!fs::exists(upath(BK + f))) copy2(upath(D + f), upath(BK + f));
    write_file_replace(upath(D + "net.bin"), out);
    write_file_replace(upath(D + "modules.bin"), m);
    const Dc v(upath(D + "net.bin"));
    if (!same_tables(v)) throw Stop("AssertionError: the written booster tables differ from net18");
    std::printf("written: net.bin %zu bytes (backup in %s)\n", out.size(), BK.c_str());
    return 0;
}

}  // namespace u3
