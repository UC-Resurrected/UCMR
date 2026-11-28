// importer/ps3values.cpp: the PS3-only values of the Uncharted 3 multiplayer helper, taken from the player's own PS3 1.19
// files (recipe step ps3-values: u3import --ps3-values ...).
//
// A few things the helper needs exist only in the PS3 game: names of packages and actors, a HUD position, the stat
// ids of the PS3 1.19 code and the room cookies PS3 players use. No release of this project carries them. This step
// reads them from the player's own files (or the kept copies under <data>/ps3-kept, which the recipe's input folders
// include), checks each against the SHA-256 of the bytes it expects, and writes them to
// <recipe>/work/ps3-values/u3-ps3-values.bin. The launcher then puts that file into the game's /data
// (<user dir>\data\u3-ps3-values.bin, launcher/native/core.cpp u3_ps3_values, before every game start), where the
// helper reads it once at start. This file holds only file names, record layouts, offsets and SHA-256 values, never
// the values.
//
// File format (little endian): "U3PV", u32 version 1, then one record per value kind found, in kind order:
//   u32 kind, u32 count, u32 byte length, data (count NUL-terminated texts, or count u32)
// Kinds: 1 taunt names, 3 civilian package names, 4 multi-all actors, 5 weapon panel origin (PS3 pair: x, y float
// patterns), 6 stat class ids (3 inverse, 5 always-up, 4 lower-is-better, in that order), 7 the second flag stat id,
// 8 room cookies (2 u32: the PS3 game's cookie of its match session, then of its party session; the helper sends
// them for its own match / party session, so a PS3 player who joins a PC player's room gets a cookie it can use).
// 9 the net game table (per PC net game index: the PS3 index of the same net game in the player's net18.bin, u16, 0xffff
// none; u16 0; the u32 bits of that PS3 record's LoadLevel float, which the helper sends a PS3 player).
// 10 the level packs (per PS3 net game of net18.bin: the map packs its level needs, one byte), 13 the map packs this
// import has (u32 pack bits); the helper's content gates (cg) read both.
// (Kind 2, the content pack numbers, is not written here.) A source that is missing or not as expected leaves its record
// out; the step then exits 3, so the recipe runs it again at the next import.
#include "importer.hpp"
#include "psarc.hpp"   // dctools: Psarc, Ps3Index, Dc
#include "recipe.hpp"  // tools_dir, local_net18_remap_text
#include "u3fix.hpp"   // dctools: first_with (the DLC folders)

#include <array>
#include <cstdio>
#include <fstream>

namespace u3imp {

namespace {

enum : uint32_t {
    TAUNTS = 1, CIVILIANS = 3, MULTI_ALL = 4, E01_PS3 = 5, STAT_CLASSES = 6, FLAG2 = 7, ROOM_COOKIES = 8, NET_GAMES = 9, LEVEL_PACKS = 10,
    MAP_PACKS = 13
};
const size_t RECORDS = 10;   // the kinds this step writes

struct Record {
    explicit Record(uint32_t k) : kind(k) {}
    uint32_t kind, count = 0;
    Bytes data;
};

std::string hex_of(const Bytes &b) { return u3::sha256_hex(b); }

void add_text(Record &r, const std::string &s) {
    r.data.insert(r.data.end(), s.begin(), s.end());
    r.data.push_back(0);
    r.count++;
}
void add_u32(Record &r, uint32_t v) {
    for (int i = 0; i < 4; i++) r.data.push_back(uint8_t(v >> (8 * i)));
    r.count++;
}

// ------------------------------------------------------------------------------------------------ taunt names
// dlc-patch18.psarc build/main/bin/dc1/net.bin (PS3 DC, big endian, 4-byte pointers), symbol 0xb2b6e512 (the taunt
// table): u32 row count at +0, pointer to the rows at +4; a row is 12 bytes, +0 = pointer to the row's animation name.
// The record holds every row's name in row order (row 0 included).
const std::string TAUNTS_SHA = "30fb5043196c76b8c5d20ae14d6ddb36909cfddf6614743ea5dd65181fd0a44a";

std::optional<Record> taunts(u3::Ps3Index &idx) {
    const auto b = idx.read("dc1/ps4/net.bin", "patch18");
    if (!b) return std::nullopt;
    const u3::Dc dc(*b, "net.bin");
    const auto it = dc.bysid.find(0xb2b6e512u);
    if (!dc.be || it == dc.bysid.end()) return std::nullopt;
    const uint64_t table = it->second.second, rows = dc.ptr(table + 4), n = dc.u32(table);
    Record r{TAUNTS};
    for (uint64_t i = 0; i < n && i < 256; i++) {
        const auto name = dc.cstr(dc.ptr(rows + 12 * i));
        if (!name) return std::nullopt;
        add_text(r, *name);
    }
    return hex_of(r.data) == TAUNTS_SHA ? std::optional(r) : std::nullopt;
}

// ------------------------------------------------------------------------------------------------ multi-all actors
// dlc-patch18.psarc build/main/pak23.txt: the "actor" lines of the level group that starts with the line
// "level-name multi-all" (up to the next "level-name" line). The record takes them by position in that group: the
// weapon preview set first, then the rest; positions 0 (handled by the helper itself), 33, 47 and 48 are not taken.
const std::string MULTI_ALL_SHA = "3faa3feca6b4f93983665d064523a24858776146e424a8672ecab8ce54e989cd";
const int MULTI_ALL_ACTORS = 49;   // actors in the group
const std::array<int, 45> MULTI_ALL_TAKE = {25, 26, 28, 29, 30, 31, 34, 35, 36, 37, 17, 20, 1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11,
                                            12, 13, 14, 15, 16, 18, 19, 21, 22, 23, 24, 27, 32, 38, 39, 40, 41, 42, 43, 44, 45, 46};

std::optional<Record> multi_all(const std::string &update) {
    if (update.empty()) return std::nullopt;
    const fs::path arc = P(update) / "dlc-patch18.psarc";
    std::error_code ec;
    if (!fs::is_regular_file(arc, ec)) return std::nullopt;
    u3::Psarc ps(arc);
    std::string text;
    for (size_t i = 0; i < ps.names.size(); i++)
        if (ps.names[i] == "build/main/pak23.txt" || ps.names[i] == "/build/main/pak23.txt") {
            const Bytes b = ps.read(i + 1);
            text.assign(b.begin(), b.end());
        }
    std::vector<std::string> actors;
    bool in = false;
    for (std::string line : split(text, '\n')) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (starts(line, "level-name ")) {
            if (in) break;
            in = line == "level-name multi-all";
        } else if (in && starts(line, "actor ")) actors.push_back(line.substr(6));
    }
    if ((int)actors.size() != MULTI_ALL_ACTORS) return std::nullopt;
    Record r{MULTI_ALL};
    for (int at : MULTI_ALL_TAKE) add_text(r, actors[at]);
    return hex_of(r.data) == MULTI_ALL_SHA ? std::optional(r) : std::nullopt;
}

// ------------------------------------------------------------------------------------------------ weapon panel origin
// build/main/bin/dc1/hud-settings.bin of dlc-patch18 / dlc-patch13 / goty-patch / the disc, accepted when the whole module
// has one of these SHA-256 values: symbol 0x9353ed16 (hud settings), pointer at +0x24 to the weapon panel block, whose
// first two words are the origin x, y (float, big endian).
const std::array<std::string, 2> HUD_SHA = {"1a3e8723ed26de2a08cae95337422ac364d79d64f4fce702d5b53c68a4968642",
                                            "a367db58865a399d0caf289deba2d3c075cca40fdb8ca70808811245a18e2684"};

std::optional<Record> e01_origin(u3::Ps3Index &idx) {
    for (const char *layer : {"patch18", "patch13", "goty-patch", "disc"}) {
        const auto b = idx.read("dc1/ps4/hud-settings.bin", layer);
        if (!b || std::find(HUD_SHA.begin(), HUD_SHA.end(), hex_of(*b)) == HUD_SHA.end()) continue;
        const u3::Dc dc(*b, "hud-settings.bin");
        const uint64_t block = dc.ptr(dc.bysid.at(0x9353ed16u).second + 0x24);
        Record r{E01_PS3};
        add_u32(r, dc.u32(block));
        add_u32(r, dc.u32(block + 4));
        return r;
    }
    return std::nullopt;
}

// ------------------------------------------------------------------------------------------------ the 1.19 EBOOT.elf
// File offsets in the decrypted PS3 1.19 EBOOT.elf (file offset = virtual address - 0x10000 in both load segments).
// Civilian packages: the net anim package table (6 rows x 8 pointers) at 0x10e8100 and the text block at 0xdacae0
// that its cells (3, 2), (4, 0), (5, 0) point into, checked together (0xc0 + 0x40 bytes); the three texts are at
// 0xdacae0, 0xdacaf0 and 0xdacb08.
const std::string CIV_SHA = "f49a5bd77b2312a024654fcb842e8278eea97bf63db9d8ef613bf09d211cbb68";
// Stat ids: the stat line function's id tests, 0xc0 bytes at 0x1f7444. Each id is built by two instructions: lis + ori
// (high half, low half) or xoris + cmpwi (high half, then the low half as a signed immediate, so the high half is
// inverted when the low half's top bit is set). Offsets of the first instruction from 0x1f7444, in record order.
const std::string STAT_SHA = "fbb94f23c4810af91d087344f5ccbd351d717d8e1765327d5e3c4aa1639422a5";
struct IdAt {
    uint32_t at;
    bool xoris;
};
const std::array<IdAt, 12> STAT_IDS = {{{0x00, false}, {0x3c, true}, {0x80, true},                                    // inverse
                                        {0x18, false}, {0x8c, false}, {0xb0, false}, {0x68, false}, {0xa4, true},    // always up
                                        {0x30, true}, {0x4c, true}, {0x58, true}, {0x98, true}}};                    // lower is better
const IdAt FLAG2_ID = {0x68, false};   // the flag stat id the PS4 game does not hold
// Room cookies: the PS3 game sends the address of its session object as the cookie of a room request, and the room
// server gives the creator's cookie to everyone who joins that room. The match and the party session objects are
// static (the same address on every PS3 running 1.19); each address is a big-endian u32 at these file offsets, checked
// by the SHA-256 of its 4 bytes. Match first, then party (the record's order).
struct CookieAt {
    uint32_t at;
    const char *sha;
};
const std::array<CookieAt, 2> COOKIES = {{{0x112cff8, "62c6a2872dd1392284c7ae6c750f76bb0e0ad8a6d112c920ec66016dbd0f06d7"},     // match
                                          {0x112cfe4, "ccecca1a410c13b4c9a99c2d67c17d789f8f7f336847b3f1a410b0fb0e6b6d9a"}}};   // party

Bytes read_at(std::ifstream &f, uint64_t at, size_t n) {
    Bytes b(n);
    f.clear();
    f.seekg(std::streamoff(at));
    f.read(reinterpret_cast<char *>(b.data()), std::streamsize(n));
    if (size_t(f.gcount()) != n) b.clear();
    return b;
}
uint32_t be32(const Bytes &b, size_t o) { return uint32_t(b[o]) << 24 | uint32_t(b[o + 1]) << 16 | uint32_t(b[o + 2]) << 8 | b[o + 3]; }
uint32_t id_from(const Bytes &code, IdAt w) {
    uint32_t hi = be32(code, w.at) & 0xffff;
    const uint32_t lo = be32(code, w.at + 4) & 0xffff;
    if (w.xoris && (lo & 0x8000)) hi ^= 0xffff;
    return hi << 16 | lo;
}

void from_eboot(const std::string &elf, std::vector<Record> &out) {
    std::ifstream f(P(elf), std::ios::binary);
    if (elf.empty() || !f) return;
    Bytes civ = read_at(f, 0x10e8100, 0xc0);
    const Bytes texts = read_at(f, 0xdacae0, 0x40);
    civ.insert(civ.end(), texts.begin(), texts.end());
    if (texts.size() == 0x40 && civ.size() == 0x100 && hex_of(civ) == CIV_SHA) {
        Record r{CIVILIANS};
        for (size_t at : {0x00, 0x10, 0x28}) add_text(r, std::string(reinterpret_cast<const char *>(&texts[at])));
        out.push_back(r);
    }
    const Bytes code = read_at(f, 0x1f7444, 0xc0);
    if (code.size() == 0xc0 && hex_of(code) == STAT_SHA) {
        Record r{STAT_CLASSES}, g{FLAG2};
        for (const IdAt &w : STAT_IDS) add_u32(r, id_from(code, w));
        add_u32(g, id_from(code, FLAG2_ID));
        out.push_back(r);
        out.push_back(g);
    }
    Record c{ROOM_COOKIES};
    for (const CookieAt &k : COOKIES) {
        const Bytes b = read_at(f, k.at, 4);
        if (b.size() != 4 || hex_of(b) != k.sha) return;
        add_u32(c, be32(b, 0));
    }
    out.push_back(c);
}

// ------------------------------------------------------------------------------------------------ net game table (9)
// The PS3 1.19 game takes a match's net game by its index in the player's own net18.bin (the PS3 game data of the
// online server, RPCS3's BCES01670DATA2 USRDIR: DC, big endian, 4-byte pointers), and LoadLevel's group by that record.
// The helper needs, per PC net game index, the PS3 index of the same net game and that record's float, read here.
//   net18.bin: symbol 0x3b9a067d (the net game table) -> {u32 count, pointer to the records}; records of 0xe4 bytes:
//   +0 name id, +0xc game type, +0x20 the float the PS3 LoadLevel sends (float A).
//   PC net.bin (the import's, after netgame-order; little endian, 8-byte pointers): the same symbol -> {u32 count,
//   pointer at +8}; records of 0xd0 bytes, +0 name id, +0xc game type.
// Pairing: the PS3 record at the same index when its name id and game type agree (netgame-order builds the PC table in
// net18's order); else the k-th PS3 record with that name id and game type for the k-th PC record with them (the two
// name ids that occur twice); none = 0xffff. The net18.bin is used when the import accepts it: its SHA-256 is listed in
// tools/dcconv/net18-versions.json (accepted or remap) or a remap was made for it on this PC (net18-remap.json).
// Record: count = the PC net games, then per PC index {u16 PS3 index, u16 0, u32 float A bits}.
const uint32_t NET_GAMES_SID = 0x3b9a067du;
const uint32_t PS3_RECORD = 0xe4, PC_RECORD = 0xd0, PS3_FLOAT_A = 0x20, GAME_TYPE = 0xc;

struct NetGame {
    uint32_t name, type, a;
};

std::vector<NetGame> net_games(const u3::Dc &dc, uint32_t size) {
    std::vector<NetGame> out;
    const auto it = dc.bysid.find(NET_GAMES_SID);
    if (it == dc.bysid.end()) return out;
    const uint64_t table = it->second.second, n = dc.u32(table), recs = dc.ptr(table + uint64_t(dc.ps));
    if (!n || n > 4096 || recs + n * size > dc.d.size()) return out;
    for (uint64_t i = 0; i < n; i++) {
        const uint64_t r = recs + i * size;
        out.push_back({dc.u32(r), dc.u32(r + GAME_TYPE), size == PS3_RECORD ? dc.u32(r + PS3_FLOAT_A) : 0});
    }
    return out;
}

bool net18_accepted(const fs::path &usrdir, const std::string &sha) {
    if (auto j = try_json(tools_dir() / "dcconv" / "net18-versions.json")) {
        for (const char *k : {"accepted", "remap"})
            if (const Value *a = j->get(k))
                for (const auto &f : a->fields)
                    if (f.first == sha) return true;
    }
    return local_net18_remap_text(usrdir, sha).has_value();
}

// the pairing (exposed for the test tool: net18 and PC tables as read above)
std::vector<std::pair<uint16_t, uint32_t>> pair_net_games(const std::vector<NetGame> &ps3, const std::vector<NetGame> &pc) {
    std::vector<std::pair<uint16_t, uint32_t>> out;
    for (size_t i = 0; i < pc.size(); i++) {
        size_t j = ps3.size();
        if (i < ps3.size() && ps3[i].name == pc[i].name && ps3[i].type == pc[i].type) {
            j = i;
        } else {
            size_t k = 0;   // the PC record's place among PC records with its name id and game type
            for (size_t q = 0; q < i; q++) k += pc[q].name == pc[i].name && pc[q].type == pc[i].type;
            for (size_t q = 0; q < ps3.size(); q++)
                if (ps3[q].name == pc[i].name && ps3[q].type == pc[i].type && k-- == 0) {
                    j = q;
                    break;
                }
        }
        out.push_back(j < ps3.size() && j < 0xffff ? std::pair<uint16_t, uint32_t>{uint16_t(j), ps3[j].a}
                                                   : std::pair<uint16_t, uint32_t>{0xffff, 0});
    }
    return out;
}

std::optional<Record> net_game_table(const std::string &data2, const std::string &pc_net) {
    if (data2.empty() || pc_net.empty()) return std::nullopt;
    const fs::path n18 = P(data2) / "net18.bin";
    std::error_code ec;
    if (!fs::is_regular_file(n18, ec) || !fs::is_regular_file(P(pc_net), ec)) return std::nullopt;
    const Bytes b = read_bytes(n18);
    if (!net18_accepted(P(data2), hex_of(b))) {
        log("ps3-values: net18.bin is not a version the import accepts (no kind 9)");
        return std::nullopt;
    }
    const u3::Dc ps3dc(b, "net18.bin"), pcdc(read_bytes(P(pc_net)), "net.bin");
    if (!ps3dc.be || pcdc.be) return std::nullopt;
    const std::vector<NetGame> ps3 = net_games(ps3dc, PS3_RECORD), pc = net_games(pcdc, PC_RECORD);
    if (ps3.empty() || pc.empty()) return std::nullopt;
    Record r{NET_GAMES};
    size_t unpaired = 0;
    for (const auto &[j, a] : pair_net_games(ps3, pc)) {
        add_u32(r, j);   // u16 index, u16 0
        r.data.push_back(uint8_t(a));
        r.data.push_back(uint8_t(a >> 8));
        r.data.push_back(uint8_t(a >> 16));
        r.data.push_back(uint8_t(a >> 24));
        unpaired += j == 0xffff;
    }
    r.count = uint32_t(pc.size());
    log(fmt("ps3-values: net game table: %zu PC net games, %zu PS3 net games, %zu PC ones without a PS3 net game", pc.size(),
            ps3.size(), unpaired));
    return r;
}

// ------------------------------------------------------------------------------------------------ level packs (10)
// Per PS3 net game of the player's net18.bin (its order): the map packs its level needs. The net game record's level id
// (+4) is looked up in the level table (symbol 0x0dec97a4: {u32 count, pointer to the records}; records of 0x58 bytes,
// +0 level id, +0x14 the pack mask); one byte per net game, the mask's low 8 bits (0 = a base map, or no level found).
// The helper's content gates read it (u3x_fixes.cpp cg: R1, the vote, the Custom Game start, the late join).
const uint32_t LEVELS_SID = 0x0dec97a4u, LEVEL_RECORD = 0x58, LEVEL_MASK = 0x14, NG_LEVEL = 4;

std::optional<Record> level_packs(const std::string &data2) {
    if (data2.empty()) return std::nullopt;
    const fs::path n18 = P(data2) / "net18.bin";
    std::error_code ec;
    if (!fs::is_regular_file(n18, ec)) return std::nullopt;
    const Bytes b = read_bytes(n18);
    if (!net18_accepted(P(data2), hex_of(b))) return std::nullopt;
    const u3::Dc dc(b, "net18.bin");
    if (!dc.be) return std::nullopt;
    const auto ng = dc.bysid.find(NET_GAMES_SID), lv = dc.bysid.find(LEVELS_SID);
    if (ng == dc.bysid.end() || lv == dc.bysid.end()) return std::nullopt;
    const uint64_t nt = ng->second.second, nn = dc.u32(nt), nrec = dc.ptr(nt + uint64_t(dc.ps));
    const uint64_t lt = lv->second.second, ln = dc.u32(lt), lrec = dc.ptr(lt + uint64_t(dc.ps));
    if (!nn || nn > 512 || nrec + nn * PS3_RECORD > dc.d.size() || ln > 4096 || lrec + ln * LEVEL_RECORD > dc.d.size()) return std::nullopt;
    Record r{LEVEL_PACKS};
    size_t with = 0;
    for (uint64_t i = 0; i < nn; i++) {
        const uint32_t level = dc.u32(nrec + i * PS3_RECORD + NG_LEVEL);
        uint8_t mask = 0;
        for (uint64_t k = 0; k < ln; k++)
            if (dc.u32(lrec + k * LEVEL_RECORD) == level) {
                mask = uint8_t(dc.u32(lrec + k * LEVEL_RECORD + LEVEL_MASK));
                break;
            }
        r.data.push_back(mask);
        with += mask != 0;
    }
    r.count = uint32_t(nn);
    log(fmt("ps3-values: level packs: %llu PS3 net games, %zu of them on map pack levels", (unsigned long long)nn, with));
    return r;
}

// ------------------------------------------------------------------------------------------------ map packs (13)
// The map packs this import has, as the helper's pack bits (bit n-1 = map pack n; bit 4 = the content only the Game of
// the Year disc's goty-dlc.psarc has; bits 5..7 are not archives and stay set): from the recipe's PS3_DLCMAPS (the DLC
// folders, '|' between them) and PS3_DISC (a goty-dlc.psarc there has all of them). One u32.
std::optional<Record> map_packs() {
    const char *dl = std::getenv("RECIPE_PS3_DLCMAPS"), *disc = std::getenv("RECIPE_PS3_DISC");
    std::error_code ec;
    uint32_t mask = 0xe0;
    if (disc && *disc && fs::is_regular_file(P(std::string(disc)) / "goty-dlc.psarc", ec)) mask = 0xff;
    else
        for (int n = 1; n <= 4; n++)
            if (dl && !u3::first_with(dl, "dlc-map" + std::to_string(n) + ".psarc").empty()) mask |= 1u << (n - 1);
    Record r{MAP_PACKS};
    add_u32(r, mask);
    log(fmt("ps3-values: map packs: mask 0x%02x", mask));
    return r;
}

Bytes file_bytes(std::vector<Record> recs) {
    std::sort(recs.begin(), recs.end(), [](const Record &a, const Record &b) { return a.kind < b.kind; });
    Bytes o = {'U', '3', 'P', 'V'};
    auto u32 = [&](uint32_t v) {
        for (int i = 0; i < 4; i++) o.push_back(uint8_t(v >> (8 * i)));
    };
    u32(1);
    for (const Record &r : recs) {
        u32(r.kind);
        u32(r.count);
        u32(uint32_t(r.data.size()));
        o.insert(o.end(), r.data.begin(), r.data.end());
    }
    return o;
}

}  // namespace

// u3import --ps3-values --out <file> [--disc <USRDIR>] [--update <USRDIR>] [--data2 <USRDIR>] [--elf <EBOOT.elf>]
// [--pc-net <the import's net.bin>]
// (--elf defaults to RECIPE_PS3_ELF). Exit 0 all records written, 3 some missing (written without them), 2 bad use.
int ps3values_main(const std::vector<std::string> &v) {
    std::string out, disc, update, data2, elf, pc_net;
    if (const char *e = std::getenv("RECIPE_PS3_ELF")) elf = e;
    for (size_t i = 1; i + 1 < v.size(); i += 2) {
        const std::string &k = v[i];
        if (k == "--out") out = v[i + 1];
        else if (k == "--disc") disc = v[i + 1];
        else if (k == "--update") update = v[i + 1];
        else if (k == "--data2") data2 = v[i + 1];
        else if (k == "--elf") elf = v[i + 1];
        else if (k == "--pc-net") pc_net = v[i + 1];
        else throw u3::Stop("unrecognized argument: " + k);
    }
    if (out.empty()) throw u3::Stop("--out is required");
    std::vector<Record> recs;
    auto guarded = [&](const char *what, const std::function<std::optional<Record>()> &fn) {
        try {
            if (auto r = fn()) return recs.push_back(*r);
        } catch (const std::exception &) {
        }
        log(std::string("ps3-values: ") + what + " not found");
    };
    {
        std::optional<u3::Ps3Index> idx;
        try {
            idx.emplace(disc, update, data2);
        } catch (const std::exception &e) {
            log(std::string("ps3-values: the PS3 archives could not be read: ") + e.what());
        }
        if (idx) {
            guarded("taunt names (dlc-patch18 net.bin)", [&] { return taunts(*idx); });
            guarded("weapon panel origin (hud-settings.bin)", [&] { return e01_origin(*idx); });
        }
    }
    guarded("multi-all actors (dlc-patch18 pak23.txt)", [&] { return multi_all(update); });
    guarded("net game table (DATA2 net18.bin and the import's net.bin)", [&] { return net_game_table(data2, pc_net); });
    guarded("level packs (DATA2 net18.bin)", [&] { return level_packs(data2); });
    guarded("map packs (the recipe's DLC folders)", [] { return map_packs(); });
    const size_t before = recs.size();
    try {
        from_eboot(elf, recs);
    } catch (const std::exception &) {
    }
    if (recs.size() - before != 4)
        log("ps3-values: civilian package names / stat ids / room cookies (the decrypted 1.19 EBOOT.elf) not all found");
    const Bytes data = file_bytes(recs);
    const fs::path p = P(out);
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    if (!(fs::is_regular_file(p, ec) && read_bytes(p) == data)) {
        write_bytes(P(out + ".tmp"), data);
        fs::rename(P(out + ".tmp"), p);
    }
    log(fmt("ps3-values: %zu of %zu records written to %s", recs.size(), RECORDS, S(p).c_str()));
    return recs.size() == RECORDS ? 0 : 3;
}

}  // namespace u3imp
