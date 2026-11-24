// Recipe step audio-defs: overlay-r3 dc1/ps4/audio.bin = the PS4 Collection's audio.bin completed with the audio
// definitions that only the player's PS3 Uncharted 3 1.19 data has, plus the audio size word of overlay-r3 modules.bin.
//   u3fix audio-defs --project P --ps4 D --ps3-disc D --ps3-update D [--ps3-data2 D] [--apply] [--verify]
//
// Why: the PS4 audio.bin is the PS3 disc version's (same 15 478 symbols). The 1.19 update and the DLC maps added sound
// definitions, ambient environments and environment links that the map regions name: a sound region (region field A =
// {2, f18aaf1b, definition}) plays a sound definition, and the region tags 36cca78a / e601ddf1 / 12b1cfdb name an
// environment. Without them the region is silent: no waterfall in the Cave map, no rain or sea in Fort (and co-op Fort),
// and the same for Gompa, Graveyard, London, Oasis, Ruins, Shambhala, Temple, Train Wreck, Village, Warzone, Yemen.
// General rule: every symbol of the newest PS3 audio.bin (data2 > dlc-patch18 > dlc-patch13 > goty-patch > goty-dlc >
// disc) that the PS4 module lacks is added in the PS4 layout. Layouts (checked on every disc / PS4 pair, --verify):
//   ee0bff2f sound definition   value {u32 sid, ptr record} -> {u32 sid, u32 0, u64 ptr}; the record has a type header
//                                (the word before it): 57945495 short: 5 words: 2 x u32, 3 x u16 pair (each pair's
//                                halves swap places as a little-endian u32); f9e725b3 long: those 5, a pointer (PS3 +0x14,
//                                PS4 +0x18, 8-aligned; always 0 in the records added), 3 u32 (PS4 +0x20), one 4-byte
//                                field kept as bytes (PS3 +0x24, PS4 +0x2c), two u8 and a u16 (PS3 +0x28, PS4 +0x30) and 10 u16
//                                pairs (PS3 +0x2c, PS4 +0x34).
//                                PS4 type headers: short [type] with the record at 4 mod 8, long [0][type], 8-aligned.
//   9940c6c4 environment         16 u32 -> 19 u32: three zero words inserted after word 6
//   454bba11 environment link    5 u32, unchanged
//   8a21e5fa reverb preset       not added: PS4 re-authored every reverb preset (two more fields, other values), so a
//                                PS3 preset has no exact PS4 form; an environment naming a missing preset keeps the name
//                                (the PS4 module already has environments naming presets it does not define).
// The module is rebuilt: the symbol table stays sorted by sid, the old objects move by the table growth (pointers and
// the relocation bitmap follow), the added objects go after them. Dry run by default; --apply backs up once to
// work/backup-audio-defs. Idempotent: an overlay audio.bin equal to the rebuilt one is left alone.
#include "psarc.hpp"
#include "u3fix.hpp"

#include <cstdio>

namespace u3 {
namespace {

constexpr uint32_t T_SOUND = 0xee0bff2f, T_ENV = 0x9940c6c4, T_LINK = 0x454bba11, T_REVERB = 0x8a21e5fa;
constexpr uint32_t H_SHORT = 0x57945495, H_LONG = 0xf9e725b3;

uint32_t halves(uint32_t v) { return v >> 16 | v << 16; }   // a big-endian u16 pair as the little-endian u32 of the PS4 pair
uint32_t bytes(uint32_t v) { return __builtin_bswap32(v); }  // a 4-byte field copied byte by byte
uint32_t two_bytes_pair(uint32_t v) { return (v >> 24 & 0xff) | (v >> 16 & 0xff) << 8 | (v & 0xffff) << 16; }   // 2 x u8, then a u16

// a converted object: words, the offsets (in the object) of 8-byte pointer slots to another object, alignment, and an
// optional type header written right before it
struct Obj {
    std::vector<uint32_t> w;
    std::vector<std::pair<size_t, size_t>> ptrs;   // (byte offset in w, index of the target object)
    size_t align = 8, phase = 0;                   // start % align == phase
    std::vector<uint32_t> header;                  // words right before the object
};

// one PS3 symbol in the PS4 layout: objects[0] is the symbol's value; nullopt for a type this step does not add
std::optional<std::vector<Obj>> convert(const Dc &a, const Sym &s, std::string &why) {
    auto w = [&](uint64_t o) { return a.u32(o); };
    if (s.type == T_LINK) {
        Obj v;
        for (int i = 0; i < 5; i++) v.w.push_back(w(s.off + 4 * i));
        return std::vector<Obj>{v};
    }
    if (s.type == T_ENV) {
        Obj v;
        for (int i = 0; i < 16; i++) {
            v.w.push_back(w(s.off + 4 * i));
            if (i == 6) v.w.insert(v.w.end(), {0, 0, 0});
        }
        return std::vector<Obj>{v};
    }
    if (s.type == T_SOUND) {
        if (a.isptr(s.off) || !a.isptr(s.off + 4)) { why = "value layout"; return std::nullopt; }
        const uint64_t r = a.ptr(s.off + 4);
        const uint32_t hdr = w(r - 4);
        Obj v, rec;
        v.w = {w(s.off), 0, 0, 0};
        v.ptrs.push_back({8, 1});
        auto pairs = [&](int from, int to) { for (int i = from; i <= to; i++) rec.w.push_back(halves(w(r + 4 * i))); };
        rec.w = {w(r), w(r + 4)};
        pairs(2, 4);
        if (hdr == H_SHORT) {
            for (uint64_t o = r; o < r + 0x14; o += 4)
                if (a.isptr(o)) { why = "pointer in a short record"; return std::nullopt; }
            rec.align = 8, rec.phase = 4, rec.header = {H_SHORT};
        } else if (hdr == H_LONG) {
            for (uint64_t o = r; o < r + 0x54; o += 4)
                if (a.isptr(o) || (o == r + 0x14 && w(o))) { why = "pointer in a long record"; return std::nullopt; }
            rec.w.insert(rec.w.end(), {0, 0, 0});   // pad, then the (null) 8-byte pointer at +0x18
            for (int i = 6; i <= 8; i++) rec.w.push_back(w(r + 4 * i));
            rec.w.push_back(bytes(w(r + 0x24)));
            rec.w.push_back(two_bytes_pair(w(r + 0x28)));
            pairs(11, 20);
            rec.align = 8, rec.phase = 0, rec.header = {0, H_LONG};
        } else {
            why = fmt("record type header %08x", hdr);
            return std::nullopt;
        }
        return std::vector<Obj>{v, rec};
    }
    why = fmt("symbol type %08x", s.type);
    return std::nullopt;
}

// the bytes of a converted symbol's objects laid out from `base` (pointer targets as offsets), for --verify
Bytes words_le(const std::vector<uint32_t> &w) {
    Bytes b(w.size() * 4);
    for (size_t i = 0; i < w.size(); i++) wr32le(&b[4 * i], w[i]);
    return b;
}

// the PS4 module rebuilt with `add` (sid, type, objects) inserted into the symbol table
Bytes rebuild(const Dc &b, const std::vector<std::tuple<uint32_t, uint32_t, std::vector<Obj>>> &add) {
    const uint64_t n = b.syms.size(), tab_end = 0x18 + 16 * n, delta = 16 * add.size();
    for (size_t i = 0; i < n; i++)
        if (b.u32(0x18 + 16 * i) != b.syms[i].sid || !b.isptr(0x18 + 16 * i + 8)) throw Stop("audio.bin: unexpected symbol table");
    if (b.syms.empty() || b.syms[0].off != tab_end) throw Stop("audio.bin: the data does not start after the symbol table");
    Bytes d(b.d.begin(), b.d.begin() + std::ptrdiff_t(b.reloc));   // header + table + objects, without the bitmap
    d.insert(d.begin() + std::ptrdiff_t(tab_end), size_t(delta), 0);
    std::set<uint64_t> ptrs;
    for (uint64_t p : b.ptrs) {
        if (p >= b.reloc) continue;
        const uint64_t t = b.ptr(p);
        if (t && t < tab_end) throw Stop(fmt("audio.bin: pointer at %llx into the header", (unsigned long long)p));
        const uint64_t np = p < tab_end ? p : p + delta;
        if (t) wr64le(&d[np], t + delta);
        ptrs.insert(np);
    }
    // the added objects after the old ones
    std::vector<std::pair<uint32_t, uint32_t>> ids;   // sid, type
    std::vector<uint64_t> value_at;
    for (const auto &[sid, type, objs] : add) {
        std::vector<uint64_t> at(objs.size());
        for (size_t k = 0; k < objs.size(); k++) {
            const Obj &o = objs[k];
            uint64_t start = d.size() + 4 * o.header.size();
            while (start % o.align != o.phase) start += 4;
            d.resize(start - 4 * o.header.size(), 0);
            for (uint32_t h : o.header) { d.resize(d.size() + 4); wr32le(&d[d.size() - 4], h); }
            at[k] = d.size();
            const Bytes wb = words_le(o.w);
            d.insert(d.end(), wb.begin(), wb.end());
        }
        for (size_t k = 0; k < objs.size(); k++)
            for (const auto &[off, tgt] : objs[k].ptrs) {
                if ((at[k] + off) % 8) throw Stop("internal: unaligned pointer slot");
                wr64le(&d[at[k] + off], at[tgt]);
                ptrs.insert(at[k] + off);
            }
        ids.push_back({sid, type});
        value_at.push_back(at[0]);
    }
    // the symbol table, sorted by sid
    std::vector<std::tuple<uint32_t, uint32_t, uint64_t>> tab;
    for (const Sym &s : b.syms) tab.push_back({s.sid, s.type, s.off + delta});
    for (size_t i = 0; i < ids.size(); i++) tab.push_back({ids[i].first, ids[i].second, value_at[i]});
    std::sort(tab.begin(), tab.end());
    for (size_t i = 0; i + 1 < tab.size(); i++)
        if (std::get<0>(tab[i]) == std::get<0>(tab[i + 1])) throw Stop("internal: duplicate symbol");
    for (size_t i = 0; i < tab.size(); i++) {
        const uint64_t e = 0x18 + 16 * i;
        wr32le(&d[e], std::get<0>(tab[i]));
        wr32le(&d[e + 4], std::get<1>(tab[i]));
        wr64le(&d[e + 8], std::get<2>(tab[i]));
        ptrs.insert(e + 8);
    }
    wr32le(&d[0x14], uint32_t(tab.size()));
    // the relocation bitmap (DcEdit's format)
    while (d.size() % 16) d.push_back(0);
    const uint64_t r = d.size(), nbytes = (r + 63) / 64;
    Bytes bm(nbytes, 0);
    for (uint64_t p : ptrs) {
        if (p % 8 || p >= r) throw Stop(fmt("internal: pointer slot %llx", (unsigned long long)p));
        bm[p / 64] |= uint8_t(1u << (p / 8 % 8));
    }
    wr64le(&d[8], r);
    d.resize(d.size() + 4);
    wr32le(&d[d.size() - 4], uint32_t(nbytes));
    d.insert(d.end(), bm.begin(), bm.end());
    return d;
}

// --verify: every disc symbol of the added types converted and compared with the PS4 module's own object bytes
int verify(const Dc &a, const Dc &b) {
    std::map<uint32_t, std::array<int, 3>> st;   // type -> exact, different, not converted
    int shown = 0;
    for (const Sym &s : a.syms) {
        if (s.type != T_SOUND && s.type != T_ENV && s.type != T_LINK) continue;
        auto it = b.bysid.find(s.sid);
        if (it == b.bysid.end()) continue;
        std::string why;
        const auto objs = convert(a, s, why);
        auto &c = st[s.type];
        if (!objs) { c[2]++; continue; }
        // the PS4 objects: the value, and for a sound definition its record (through the value's pointer)
        std::vector<uint64_t> at{it->second.second};
        if (s.type == T_SOUND) at.push_back(b.ptr(at[0] + 8));
        bool same = true;
        std::string where;
        for (size_t k = 0; k < objs->size() && same; k++) {
            const Obj &o = (*objs)[k];
            if (at[k] % o.align != o.phase) same = false, where = "alignment";
            for (size_t h = 0; h < o.header.size() && same; h++)
                if (b.u32(at[k] - 4 * (o.header.size() - h)) != o.header[h]) same = false, where = "type header";
            for (size_t i = 0; i < o.w.size() && same; i++) {
                bool slot = false;
                for (const auto &[off, t] : o.ptrs) slot |= (4 * i == off || 4 * i == off + 4);
                if (slot) continue;
                if (b.u32(at[k] + 4 * i) != o.w[i]) same = false, where = fmt("object %zu word %zu: %08x, PS4 %08x", k, i, o.w[i], b.u32(at[k] + 4 * i));
            }
        }
        c[same ? 0 : 1]++;
        if (!same && shown++ < 12) std::printf("  differs %08x (type %08x): %s\n", s.sid, s.type, where.c_str());
    }
    for (const auto &[t, c] : st) std::printf("type %08x: %d exact, %d different, %d not converted\n", t, c[0], c[1], c[2]);
    return 0;
}

}  // namespace

int audio_defs(const std::vector<std::string> &argv) {
    Args A(argv, {"--apply", "--verify"}, {"--project", "--ps4", "--ps3-disc", "--ps3-update", "--ps3-data2"});
    const std::string ps4 = A.need("--ps4");
    Ps3Index idx(A.need("--ps3-disc"), A.get("--ps3-update", ""), A.get("--ps3-data2", ""));
    const fs::path retail = upath(ps4 + "/u3data/build/main/dc1/ps4/audio.bin");
    if (!fs::exists(retail)) throw Stop("PS4 audio.bin not found: " + utf8(retail));
    const Dc b(retail);
    if (A.has("--verify")) {
        const auto disc = idx.read("dc1/ps4/audio.bin", "disc");
        if (!disc) throw Stop("the PS3 disc has no audio.bin");
        return verify(Dc(*disc, "disc audio.bin"), b);
    }
    const std::string project = A.need("--project");
    const auto newest = idx.read("dc1/ps4/audio.bin");
    if (!newest) throw Stop("the PS3 data has no audio.bin");
    const Dc a(*newest, "PS3 audio.bin");
    std::vector<std::tuple<uint32_t, uint32_t, std::vector<Obj>>> add;
    std::map<std::string, int> skipped;
    std::map<uint32_t, int> added;
    for (const Sym &s : a.syms) {
        if (b.bysid.count(s.sid)) continue;
        if (s.type == T_REVERB) { skipped["reverb preset (no exact PS4 form)"]++; continue; }
        std::string why;
        auto objs = convert(a, s, why);
        if (!objs) { skipped[why]++; continue; }
        add.push_back({s.sid, s.type, std::move(*objs)});
        added[s.type]++;
    }
    std::string sum;
    for (const auto &[t, n] : added) sum += fmt("%s%d of type %08x", sum.empty() ? "" : ", ", n, t);
    std::printf("PS3 audio.bin %zu symbols, PS4 %zu; adding %zu (%s)\n", a.syms.size(), b.syms.size(), add.size(), sum.empty() ? "none" : sum.c_str());
    for (const auto &[w, n] : skipped) std::printf("  not added: %d (%s)\n", n, w.c_str());
    if (rebuild(b, {}) != b.d) throw Stop("internal: the PS4 audio.bin does not rebuild to itself (unknown layout)");
    const Bytes out = rebuild(b, add);
    {   // the rebuilt module must parse, keep every old symbol's bytes and hold the added ones
        const Dc c(out, "rebuilt audio.bin");
        if (c.syms.size() != b.syms.size() + add.size()) throw Stop("internal: symbol count");
        for (const Sym &s : b.syms) {
            auto it = c.bysid.find(s.sid);
            if (it == c.bysid.end() || it->second.first != s.type || (!b.isptr(s.off) && c.u32(it->second.second) != b.u32(s.off))) throw Stop(fmt("internal: symbol %08x lost", s.sid));
        }
        if (c.ptrs.size() != b.ptrs.size() + add.size() + std::count_if(add.begin(), add.end(), [](const auto &x) { return std::get<1>(x) == T_SOUND; }))
            throw Stop("internal: pointer count");
    }
    const std::string dir = project + "/work/overlay-r3/u3data/build/main/dc1/ps4/";
    const fs::path dst = upath(dir + "audio.bin"), modules = upath(dir + "modules.bin");
    if (!fs::exists(modules)) throw Stop("overlay-r3 modules.bin not found (the DC conversion steps make it): " + utf8(modules));
    Bytes mod = read_file(modules);
    const uint64_t slot = module_size_slot(Dc(mod, "modules.bin"), "audio");
    const uint64_t old = rd64le(&mod[slot]);
    const uint64_t cur = fs::exists(dst) ? fs::file_size(dst) : fs::file_size(retail);
    if (fs::exists(dst) && read_file(dst) == out && old == out.size()) {
        std::printf("already applied: audio.bin %zu bytes, modules.bin audio size %llu\n", out.size(), (unsigned long long)old);
        return 0;
    }
    // (a modules.bin an earlier step rebuilt on an update carries the retail size while the overlay's audio.bin is
    // already the completed one: the word is set again)
    if (old != cur && old != out.size() && old != fs::file_size(retail))
        throw Stop(fmt("modules.bin: audio size word %llu is neither the current audio.bin size %llu nor the new %zu", (unsigned long long)old,
                       (unsigned long long)cur, out.size()));
    wr64le(&mod[slot], out.size());
    if (!A.has("--apply")) {
        std::printf("dry run: audio.bin %llu -> %zu bytes, modules.bin audio size %llu -> %zu; nothing written\n", (unsigned long long)cur, out.size(),
                    (unsigned long long)old, out.size());
        return 0;
    }
    const fs::path backup = upath(project + "/work/backup-audio-defs/");
    fs::create_directories(backup);
    if (fs::exists(dst)) copy_once(dst, backup / "audio.bin");
    copy_once(modules, backup / "modules.bin");
    write_file_replace(dst, out);
    write_file_replace(modules, mod);
    std::printf("applied: audio.bin %llu -> %zu bytes (sha256 %s), modules.bin audio size %llu -> %zu\n", (unsigned long long)cur, out.size(),
                sha256_hex(out).c_str(), (unsigned long long)old, out.size());
    return 0;
}

}  // namespace u3
