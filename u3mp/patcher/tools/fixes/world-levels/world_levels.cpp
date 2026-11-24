// Recipe step world-levels: register every world level a net.bin map needs.     u3fix world-levels --project P [--apply]
//
// Each net.bin map (table 0xdec97a4, PS4 entries 0x98) names its world level at +0x70 (PS3 1.19 entry 0x58, +0x40).
// The world level's pak (<world>-ingame12.pak) holds the REGION_TREE that streams the map's levels in and out. Some of
// those levels are not in the map's own level list (+0x58): competitive London Underground's station ldn-train-multi
// is only loaded by world-london-mp's region tree (rgn-ldn-world-1) when the train arrives; Airstrip's take-off-sound /
// air-multi-sound only by world-yemen-mp's (rgn-yemen-multi-mid-1). A world the registry does not name never loads, so
// after the train the station had no geometry and no collision and every respawn fell into the void.
//
// Rule: for every world level named by any net.bin map of overlay-r3, the registry of overlay-r3 (pak23_ps4.txt) must
// have the PS3 block   level-name <w> / level-id <w> / package <w>-ingame   and the pak must be in an overlay (the
// plan.tsv row of convert-levels-*). A missing block is added after the last world-* block (else at the end); a world
// whose pak is in no overlay and not in the retail game is reported and the step fails (its plan.tsv row is missing).
// Every level name a world pak's region tree names is checked against the registry (reported, not fixed).
// Dry run by default; --apply writes (pak23_ps4.txt backed up once to work/backup-world-levels/). Idempotent: 'already
// applied' when every world level is registered.
//
// Diagnosis, the world-london-mp plan row and the block insertion (level_name, split_lines, backup / idempotent flow)
// come from contributor PR 16 (hard-coded to world-london-mp there); the net.bin derivation is this version's.
#include "dc.hpp"
#include "u3fix.hpp"

#include <cstdio>

namespace u3 {
namespace {

const uint32_t MAPS = 0xdec97a4;

bool space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

// second word of a line whose first word is 'level-name', else ""
std::string level_name(const std::string &ln) {
    size_t i = 0;
    while (i < ln.size() && space(ln[i])) i++;
    const std::string key = "level-name";
    if (ln.compare(i, key.size(), key) != 0) return "";
    i += key.size();
    if (i >= ln.size() || !space(ln[i])) return "";
    while (i < ln.size() && space(ln[i])) i++;
    size_t j = i;
    while (j < ln.size() && !space(ln[j])) j++;
    return ln.substr(i, j - i);
}

std::vector<std::string> split_lines(const std::string &text, const std::string &nl) {
    std::vector<std::string> v;
    size_t pos = 0;
    while (true) {
        size_t e = text.find(nl, pos);
        v.push_back(text.substr(pos, e == std::string::npos ? std::string::npos : e - pos));
        if (e == std::string::npos) break;
        pos = e + nl.size();
    }
    return v;
}

// the world levels of the net.bin maps, in map order, each once
std::vector<std::string> map_worlds(const Dc &c) {
    auto it = c.bysid.find(MAPS);
    if (it == c.bysid.end()) throw Stop("net.bin: no map table");
    const uint64_t o = it->second.second, es = c.ps == 4 ? 0x58 : 0x98, wo = c.ps == 4 ? 0x40 : 0x70;
    const uint32_t n = c.u32(o);
    const uint64_t ar = c.ptr(o + uint64_t(c.ps));
    std::vector<std::string> w;
    for (uint32_t i = 0; i < n; i++) {
        const uint64_t f = ar + es * i + wo;
        if (!c.isptr(f)) continue;
        const std::string s = c.str_or_empty(c.ptr(f));
        if (!s.empty() && std::find(w.begin(), w.end(), s) == w.end()) w.push_back(s);
    }
    return w;
}

// the level-like strings of a pak (lower-case words with '-'), for the region-tree check
std::set<std::string> pak_words(const Bytes &d) {
    std::set<std::string> out;
    std::string cur;
    for (uint8_t ch : d) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-') { cur += char(ch); continue; }
        if (ch == 0 && cur.size() > 3 && cur.find('-') != std::string::npos && cur[0] != '-' && cur.find("--") == std::string::npos) out.insert(cur);
        cur.clear();
    }
    return out;
}

}  // namespace

int world_levels(const std::vector<std::string> &argv) {
    Args a(argv, {"--apply"}, {"--project", "--ps4"});
    const std::string project = a.need("--project");
    const fs::path work = upath(project + "/work");
    const fs::path dir = work / "overlay-r3" / "u3data" / "build" / "main";
    const fs::path reg = dir / "pak23_ps4.txt", net = dir / "dc1" / "ps4" / "net.bin";
    std::error_code ec;
    if (!fs::is_regular_file(reg, ec) || !fs::is_regular_file(net, ec)) {
        std::printf("overlay-r3 pak23_ps4.txt / net.bin not found: nothing to do yet\n");
        return 0;
    }
    // the pak of a level: the highest overlay that has it, else the retail game (--ps4)
    auto find_pak = [&](const std::string &file) -> fs::path {
        for (const char *ov : {"overlay-r3", "overlay-r2", "overlay-port", "overlay-mp"}) {
            const fs::path p = work / ov / "u3data" / "build" / "main" / "pak23" / "ps4" / upath(file);
            if (fs::is_regular_file(p, ec)) return p;
        }
        if (a.val.count("--ps4")) {
            const fs::path p = upath(a.get("--ps4", "")) / "u3data" / "build" / "main" / "pak23" / "ps4" / upath(file);
            if (fs::is_regular_file(p, ec)) return p;
        }
        return {};
    };
    const Bytes b = read_file(reg);
    const std::string text(b.begin(), b.end());
    const std::string nl = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::vector<std::string> lines = split_lines(text, nl);
    std::set<std::string> registered;
    for (const auto &ln : lines)
        if (const std::string n = level_name(ln); !n.empty()) registered.insert(n);

    const std::vector<std::string> worlds = map_worlds(Dc(net));
    std::vector<std::string> added;
    int missing = 0;
    for (const std::string &w : worlds) {
        const fs::path pak = find_pak(w + "-ingame12.pak");
        if (pak.empty()) {
            std::printf("ERROR %s: a net.bin map needs it, but %s-ingame12.pak is in no overlay (plan.tsv row missing)\n", w.c_str(), w.c_str());
            missing++;
            continue;
        }
        // the region tree's levels must be registered too (a report: their paks come from the level rows)
        std::vector<std::string> unreg;
        for (const std::string &s : pak_words(read_file(pak)))
            if (!registered.count(s) && s != w && s.rfind("rgn-", 0) != 0 && s.rfind("regionshape-", 0) != 0 && s.rfind("regiongroup-", 0) != 0)
                unreg.push_back(s);
        for (const std::string &s : unreg) std::printf("note %s: names '%s', which is not a registered level\n", w.c_str(), s.c_str());
        if (registered.count(w)) continue;
        size_t at = lines.size();
        while (at > 0 && lines[at - 1].empty()) at--;   // keep a trailing newline after the block
        for (size_t i = 0; i < lines.size(); i++) {     // after the last world-* block
            if (level_name(lines[i]).rfind("world-", 0) != 0) continue;
            size_t j = i + 1;
            while (j < lines.size() && level_name(lines[j]).empty()) j++;
            if (j < lines.size()) at = j;
        }
        const std::vector<std::string> block = {"level-name " + w, "level-id " + w, "package " + w + "-ingame"};
        lines.insert(lines.begin() + std::ptrdiff_t(at), block.begin(), block.end());
        registered.insert(w);
        added.push_back(w);
    }
    if (added.empty()) {
        std::printf(missing ? "%d world level(s) without a pak\n" : "already applied: every world level of net.bin is registered\n", missing);
        return missing ? 1 : 0;
    }
    for (const auto &l : added) std::printf("overlay-r3 pak23_ps4.txt: + %s\n", l.c_str());
    if (!a.has("--apply")) {
        std::printf("dry run: nothing changed\n");
        return missing ? 1 : 0;
    }
    std::string out;
    for (size_t i = 0; i < lines.size(); i++) out += (i ? nl : "") + lines[i];
    const fs::path bk = work / "backup-world-levels";
    fs::create_directories(bk);
    copy_once(reg, bk / "overlay-r3-pak23_ps4.txt");
    write_file_replace(reg, Bytes(out.begin(), out.end()));
    std::printf("applied: %zu world level(s) registered\n", added.size());
    return missing ? 1 : 0;
}

}  // namespace u3
