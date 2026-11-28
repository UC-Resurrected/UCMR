// u2mp menus: the multiplayer menu queries the PS4 1.02 build left empty or fed with PS3-only data.
//
// The menu pages (menu.bin) are the PS3 ones, but behind several of their controls the query
// functions are bare `ret` stubs, and others read live data only the PS3 game had (server data,
// PSN). This file gives those controls content: the profile panel's name, the party status line,
// the PS3 playlists of the lobby, the friends list (RPCN online, the party on LAN), the players
// met, the join game list and the voice volume slider. It also traces a few Profile page queries
// for diagnostics, and gives the help bar's Prev / Next entries shoulder button pictures again.
//
// A query reaches us either through the query table (we swap the entry's function pointer and may
// still call the game's function) or through a jump written over a stripped `ret` stub.
//
// Reimplemented from the behaviour description u2mp/docs/spec/menus.md.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "frontend3.h"
#include "host_memory.h"
#include "online.h"
#include "progression.h"
#include "session_api.h"
#include "u2mp_internal.h"
#include "voice.h"

// Every float operation is rounded on its own, as the description requires.
#pragma clang fp contract(off)

namespace u2mp {
namespace {

// ---- guest memory and calls ----------------------------------------------------------------------

template <typename T> T get(const uint8_t *p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
template <typename T> void put(uint8_t *p, T v) { std::memcpy(p, &v, sizeof v); }

uint8_t *abs_at(uint64_t offset) { return image.base + offset; }

// Game locations used here (image offsets of the 1.02 build).
constexpr uint64_t kQueryTable = 0x105e370;
constexpr int kQueryEntries = 132;
constexpr uint64_t kEntrySize = 0x18;
constexpr uint64_t kStartMatchQuery = 0x655c40;
constexpr uint64_t kNetInfo = 0x104f1c8;
constexpr uint64_t kNgm = 0x113b760;
constexpr uint64_t kNpd = 0x11474f8;
constexpr uint64_t kTextDb = 0x1066660;
constexpr uint64_t kNetSm = 0x14ce97c;
constexpr uint64_t kTextFind = 0xcddc00;
constexpr uint64_t kDcLookup = 0xa15e70;
constexpr uint64_t kSetNetGame = 0x63dc30;

constexpr uint32_t kDcNetGames = 0x3b9a067d;
constexpr uint32_t kDcGameTypes = 0xf985c7b6;
constexpr uint32_t kTextPartyCount = 0xba8db526;
constexpr uint32_t kTextPartyLeader = 0xac182308;
constexpr uint32_t kMachinimaName = 0x5614c769;

using QueryFn = void(U2MP_GUEST *)(uint8_t *q);

const char *game_text(uint32_t id) {
    auto find = reinterpret_cast<const char *(U2MP_GUEST *)(void *, uint32_t)>(abs_at(kTextFind));
    const char *text = find(abs_at(kTextDb), id);
    return text ? text : "";
}

void *dc_lookup(uint32_t id) {
    return reinterpret_cast<void *(U2MP_GUEST *)(uint32_t)>(abs_at(kDcLookup))(id);
}

// A DC table as the game lays it out: {s32 count; s32 pad; u8 *records}.
struct DcTable {
    int32_t count = 0;
    uint8_t *records = nullptr;
};
DcTable read_table(const void *symbol) {
    const auto *p = static_cast<const uint8_t *>(symbol);
    return {get<int32_t>(p), get<uint8_t *>(p + 8)};
}

// ---- query object ----------------------------------------------------------------------------------

uint32_t op_of(const uint8_t *q) { return get<uint32_t>(q); }
void set_text(uint8_t *q, const char *text) { put<uint64_t>(q + 0x40, reinterpret_cast<uint64_t>(text)); }

const char kNothing[] = "";

// Copies `text` into a 96-byte buffer, cutting it to 95 bytes.
void fill96(char (&buffer)[96], const std::string &text) {
    const size_t n = text.size() < 95 ? text.size() : 95;
    std::memcpy(buffer, text.data(), n);
    buffer[n] = 0;
}

// The list behaviour most controls share: `count` rows, a remembered current index, and the row
// text produced only for a valid row of column 0.
template <typename RowText> void plain_list(uint8_t *q, int32_t count, int32_t &cur, RowText row_text) {
    switch (op_of(q)) {
    case 2: {
        const int32_t r = get<int32_t>(q + 4);
        if (get<uint32_t>(q + 0x54) == 0 && r >= 0 && r < count)
            set_text(q, row_text(r));
        else
            set_text(q, kNothing);
        break;
    }
    case 4: put<int32_t>(q + 4, count == 0 ? -1 : (cur < count ? cur : count - 1)); break;
    case 5: put<int32_t>(q + 4, count - 1); break;
    case 7: put<int32_t>(q + 4, 1); break;
    case 8:
        if (count <= 0) cur = -1;
        else if (cur < 0) cur = 0;
        else if (cur >= count) cur = count - 1;
        break;
    case 9: cur = get<int32_t>(q + 4); break;
    default: break;
    }
}

// Game text `id` with its first "[A]" replaced by `value`.
void count_text(char (&buffer)[96], uint32_t id, int value) {
    std::string text = game_text(id);
    const size_t at = text.find("[A]");
    if (at != std::string::npos) text.replace(at, 3, std::to_string(value));
    fill96(buffer, text);
}

// ---- wrapped table entries --------------------------------------------------------------------------

uint64_t original_profile = 0, original_status = 0, original_playlist = 0, original_met = 0, original_volume = 0;

void call(uint64_t fn, uint8_t *q) { reinterpret_cast<QueryFn>(fn)(q); }

// cond 133: the profile panel shows the online id the PS3 got from PSN; give it our player name.
U2MP_GUEST void query_profile(uint8_t *q) {
    uint8_t *id = abs_at(kNetInfo + 0x298);
    std::memset(id, 0, 17);
    const std::string name = session::local_name();
    std::memcpy(id, name.data(), name.size() < 16 ? name.size() : 16);
    call(original_profile, q);
}

// cond 49: the status line counts the PS3 live data; let it count the party for this one call.
U2MP_GUEST void query_status(uint8_t *q) {
    uint8_t *field = abs_at(kNetInfo + 0x10);
    const int32_t saved = get<int32_t>(field);
    put<int32_t>(field, session::member_count());
    call(original_status, q);
    put<int32_t>(field, saved);
}

// ---- playlists (cond 55) ------------------------------------------------------------------------------

// One playlist record as the game reads it through ngm+0x20a0 (0x6c bytes used).
struct alignas(16) Playlist {
    uint8_t bytes[0x70];
    uint32_t id() const { return get<uint32_t>(bytes); }
    uint32_t picture() const { return get<uint32_t>(bytes + 0x10); }
    int32_t first_type() const { return get<int32_t>(bytes + 0x40); }
    int32_t type_count() const { return get<int32_t>(bytes + 0x68); }
    int32_t type(int i) const { return get<int32_t>(bytes + 0x48 + 4 * i); }
    int8_t ps3_index() const { return int8_t(bytes[0x6c]); }
    int32_t max_players() const { return get<int32_t>(bytes + 0x18); }
    int32_t max_party() const { return get<int32_t>(bytes + 0x1c); }
};

// The game types of co-op (4, 8, 9, 12).
bool coop_game_type(int32_t t) { return t == 4 || t == 8 || t == 9 || t == 12; }

constexpr int kMaxPlaylists = 32;
Playlist playlists[kMaxPlaylists];
int built = 0;           // records of the last successful build (0: none yet)
uint32_t applied = 0;    // the playlist id whose net game is already set

void make_record(Playlist &p, uint32_t id, uint32_t name, uint32_t desc, uint32_t picture, const int32_t *types,
                 int n, int8_t ps3_index = -1) {
    std::memset(p.bytes, 0, sizeof p.bytes);
    put<uint32_t>(p.bytes + 0x00, id);
    put<uint32_t>(p.bytes + 0x04, name);
    put<uint32_t>(p.bytes + 0x08, desc);
    put<uint32_t>(p.bytes + 0x10, picture);
    // The PS3 *playlists* records' limits: +0x18 the maximum player count (the size of a matchmaking room, PS3
    // 0x49a958), +0x1c the maximum party size (Start Match): 10 / 5 for competitive playlists, 3 / 3 for co-op ones.
    bool coop = n > 0;
    for (int i = 0; i < n; ++i) coop = coop && coop_game_type(types[i]);
    put<int32_t>(p.bytes + 0x18, coop ? 3 : 10);
    put<int32_t>(p.bytes + 0x1c, coop ? 3 : 5);
    put<int32_t>(p.bytes + 0x34, -1);
    put<int32_t>(p.bytes + 0x40, types[0]);
    put<uint32_t>(p.bytes + 0x44, picture);
    for (int i = 0; i < n; ++i) put<int32_t>(p.bytes + 0x48 + 4 * i, types[i]);
    put<int32_t>(p.bytes + 0x68, n);
    put<uint8_t>(p.bytes + 0x6c, uint8_t(ps3_index));
}

// The PS3 playlists, in the order of the PS3 list (the GOTY server data's playlist table: The Lab, Deathmatch,
// Elimination, Plunder, Objectives, Ranked, All Competitive Modes, Co-op Objective, Co-op Arena), then the two co-op
// playlists of ours the PS3 list does not have. Name and description are text ids, pictures are names under
// general/net/playlists/, types are the game types' name text ids.
struct PlaylistDef {
    const char *key;
    uint32_t name;
    uint32_t desc;   // 0: the first game type's description
    const char *pictures[3];
    uint32_t types[8];
    int8_t ps3_index;   // index in the PS3 playlist list (9, 10: after it), also when a playlist before is skipped
};

constexpr uint32_t kDeathmatch = 0xa30341e7, kTeamDeathmatch = 0x8dfdede9, kPlunder = 0x3d9470a5,
                   kTurf = 0x0544e6f6, kKoh = 0x13f76fcc, kObjA = 0x71ede2dc, kObjB = 0xf625a024,
                   kElimination = 0x3b8de400, kCoopObjective = 0xc072e0a6, kSurvival = 0x2959eca4,
                   kGoldRush = 0xe9995f4c, kCoopArena3 = 0xf0a21209;

const PlaylistDef kDefs[] = {
    {"the-lab", sid("u2mp/playlist/the-lab"), sid("u2mp/playlist/the-lab/desc"), {"the-lab", "default"},
     {kDeathmatch, kTeamDeathmatch}, 0},
    {"deathmatch", kDeathmatch, 0xfc8dd900, {"deathmatch"}, {kDeathmatch, kTeamDeathmatch, kElimination}, 1},
    {"elimination", kElimination, sid("u2mp/playlist/elimination/desc"), {"elimination"}, {kElimination}, 2},
    {"plunder", kPlunder, 0x32cc1acb, {"plunder"}, {kPlunder}, 3},
    {"objectives", 0x4bf30e73, 0x675ef552, {"objective"}, {kPlunder, kTurf, kKoh, kObjA, kObjB}, 4},
    {"ranked", 0x47b24464, 0x57fc82aa, {"mlg"},
     {kDeathmatch, kTeamDeathmatch, kPlunder, kTurf, kKoh, kObjA, kObjB, kElimination}, 5},
    {"all-competitive", sid("u2mp/playlist/all-competitive"), sid("u2mp/playlist/all-competitive/desc"),
     {"grab-bag"}, {kDeathmatch, kTeamDeathmatch, kPlunder, kTurf, kKoh, kObjA, kObjB, kElimination}, 6},
    // The co-op playlists list net games of these game types on PS3 (Co-op Objective: the "Co-op" type; Co-op Arena:
    // Survival, Gold Rush and the third arena type, f0a21209); their names are not game type names.
    {"coop-objective", 0x5136a578, 0xc35e16e8, {"coop"}, {kCoopObjective}, 7},
    {"coop-arena", 0x2ee192ab, 0xe1ae2796, {"coop", "survival", "default"}, {kSurvival, kGoldRush, kCoopArena3}, 8},
    {"gold-rush", 0x00248bb5, 0, {"default", "survival", "coop"}, {0x00248bb5, 0xe9995f4c}, 9},
    {"survival", 0x2959eca4, 0, {"survival", "default", "coop"}, {0x2959eca4, 0xb81a7c18}, 10},
};

uint32_t picture_id(const char *name) { return sid(("general/net/playlists/" + std::string(name)).c_str()); }

void publish_playlists() {
    uint8_t *ngm = abs_at(kNgm);
    for (int i = 0; i < built; ++i) put<uint64_t>(ngm + 0x20a0 + 8 * i, reinterpret_cast<uint64_t>(&playlists[i]));
    put<uint32_t>(ngm + 0x21a0, uint32_t(built));
}

// The picture of a fallback playlist, chosen by its game type's name.
uint32_t fallback_picture(uint32_t name) {
    switch (name) {
    case kDeathmatch: case kTeamDeathmatch: case kElimination: return picture_id("deathmatch");
    case kPlunder: case kTurf: case kKoh: case 0x8f86cb5f: case 0x4bf30e73: return picture_id("objective");
    case 0x2ee192ab: case 0x5136a578: case 0x00248bb5: case 0xe9995f4c: return picture_id("coop");
    case 0x2959eca4: case 0xb81a7c18: return picture_id("survival");
    default: return picture_id("default");
    }
}

void build_playlists() {
    uint8_t *ngm = abs_at(kNgm);
    if (get<uint32_t>(ngm + 0x21a0) != 0) return;
    if (built > 0) {   // the game cleared the list: hand it the same records again
        publish_playlists();
        return;
    }
    void *games_sym = dc_lookup(kDcNetGames);
    void *types_sym = dc_lookup(kDcGameTypes);
    if (!games_sym || !types_sym) return;
    const DcTable games = read_table(games_sym), types = read_table(types_sym);
    if (games.count <= 0 || types.count <= 0 || !games.records || !types.records) return;

    auto game_type = [&](int i) { return get<int32_t>(games.records + 0x90 * i + 0x0c); };
    auto type_record = [&](int t) { return types.records + 0x44 * t; };

    // A game type counts only when one of its net games can be played (DLC maps imported).
    std::vector<bool> playable(size_t(types.count), false);
    for (int i = 0; i < games.count; ++i) {
        const int32_t t = game_type(i);
        if (t >= 0 && t < types.count && dlc_net_game_available(i)) playable[size_t(t)] = true;
    }

    for (const PlaylistDef &def : kDefs) {
        if (built >= kMaxPlaylists) break;
        int32_t found[8];
        int n = 0;
        for (int w = 0; w < 8 && def.types[w] != 0; ++w)
            for (int32_t t = 0; t < types.count; ++t) {
                if (!playable[size_t(t)] || get<uint32_t>(type_record(t)) != def.types[w]) continue;
                bool known = false;
                for (int k = 0; k < n; ++k) known = known || found[k] == t;
                if (!known && n < 8) found[n++] = t;
            }
        if (n == 0) continue;

        // Prefer a picture no earlier playlist of this pass shows.
        uint32_t picture = picture_id(def.pictures[0]);
        for (const char *name : def.pictures) {
            if (!name) break;
            const uint32_t candidate = picture_id(name);
            bool taken = false;
            for (int k = 0; k < built; ++k) taken = taken || playlists[k].picture() == candidate;
            if (!taken) {
                picture = candidate;
                break;
            }
        }
        const uint32_t desc = def.desc ? def.desc : get<uint32_t>(type_record(found[0]) + 4);
        make_record(playlists[built++], sid(("u2mp/playlist/" + std::string(def.key)).c_str()), def.name, desc,
                    picture, found, n, def.ps3_index);
        log("menus: playlist %s -> picture %08x", def.key, picture);
    }
    if (built > 0) {
        publish_playlists();
        log("menus: %d PS3 playlists", built);
        return;
    }

    // No PS3 playlist fits these game types: one playlist per playable game type instead.
    for (int i = 0; i < games.count && built < kMaxPlaylists; ++i) {
        const int32_t t = game_type(i);
        if (t < 0 || t >= types.count || !dlc_net_game_available(i)) continue;
        bool have = false;
        for (int k = 0; k < built; ++k) have = have || playlists[k].first_type() == t;
        if (have) continue;
        const uint8_t *rec = type_record(t);
        const uint32_t name = get<uint32_t>(rec);
        if (name == kMachinimaName) continue;
        uint32_t id = get<uint32_t>(rec + 8);
        bool clash = id == 0;
        for (int k = 0; k < built; ++k) clash = clash || playlists[k].id() == id;
        if (clash) id = 0x55320000u + uint32_t(t);
        make_record(playlists[built++], id, name, get<uint32_t>(rec + 4), fallback_picture(name), &t, 1);
    }
    // Deathmatch playlists first, each group keeping its order.
    const uint32_t dm = picture_id("deathmatch");
    Playlist sorted[kMaxPlaylists];
    int s = 0;
    for (int k = 0; k < built; ++k)
        if (playlists[k].picture() == dm) sorted[s++] = playlists[k];
    for (int k = 0; k < built; ++k)
        if (playlists[k].picture() != dm) sorted[s++] = playlists[k];
    for (int k = 0; k < built; ++k) playlists[k] = sorted[k];
    publish_playlists();
    log("menus: %d playlists from %d net games", built, games.count);
}

// Host in matchmaking: make the current net game one of the chosen playlist's game types.
void apply_playlist() {
    if (!session::is_host() || get<int32_t>(abs_at(kNetInfo + 0x2c4)) != 1) return;
    const uint32_t id = get<uint32_t>(abs_at(kNpd + 0x104));
    if (id == applied) return;
    const Playlist *chosen = nullptr;
    for (int k = 0; k < built; ++k)
        if (playlists[k].id() == id) chosen = &playlists[k];
    if (!chosen) return;
    void *games_sym = dc_lookup(kDcNetGames);
    if (!games_sym) return;
    const DcTable games = read_table(games_sym);
    if (!games.records) return;
    auto game_type = [&](int i) { return get<int32_t>(games.records + 0x90 * i + 0x0c); };
    const int32_t type = chosen->first_type();

    const int16_t current = get<int16_t>(abs_at(kNgm + 0x1ca0));
    if (current >= 0 && current < games.count && game_type(current) == type && dlc_net_game_available(current)) {
        applied = id;
        return;
    }
    for (int i = 0; i < games.count; ++i) {
        if (game_type(i) != type || !dlc_net_game_available(i)) continue;
        // The index travels as a sign-extended s16 in esi.
        reinterpret_cast<void(U2MP_GUEST *)(void *, int32_t, const void *, int32_t)>(abs_at(kSetNetGame))(
            abs_at(kNgm), int32_t(int16_t(i)), nullptr, 10);
        applied = id;
        log("menus: playlist %08x -> net game %d", id, i);
        return;
    }
}

// cond 55: the lobby's PLAYLIST control, fed with our playlist records.
U2MP_GUEST void query_playlist(uint8_t *q) {
    build_playlists();
    if (get<int16_t>(abs_at(kNgm + 0x1ca0)) < 0) applied = 0;   // left Multiplayer: apply again next time
    call(original_playlist, q);

    // op 3: the game's answer stays (image type 0x25 with the playlist id, as PS3): menuimages.cpp draws the
    // picture in PS3's box and the game type icons under it.
    if (op_of(q) == 3) return;
    if (op_of(q) != 4 || built == 0) return;
    if (get<int32_t>(q + 4) < 0) {   // the saved choice is not one of ours: take the first playlist
        put<uint32_t>(abs_at(kNpd + 0x104), playlists[0].id());
        put<int32_t>(q + 4, 0);
    }
    apply_playlist();
}

// ---- players met (cond 39) ----------------------------------------------------------------------------

std::vector<std::string> met;
int32_t met_cur = 0;

U2MP_GUEST void query_players_met(uint8_t *q) {
    if (op_of(q) == 8 || met.empty()) met = progression_players_met();
    plain_list(q, int32_t(met.size()), met_cur, [](int32_t r) { return met[size_t(r)].c_str(); });
}

// ---- voice volume (cond 104) --------------------------------------------------------------------------

U2MP_GUEST void query_volume(uint8_t *q) {
    const uint32_t op = op_of(q);
    const bool in_mp = get<int32_t>(abs_at(kNetSm)) > 1;
    if (op == 4 && in_mp) {
        // Slider steps 0..20: the scaled volume truncated to s32 the way x86 does it (a value out of
        // s32 range or NaN becomes INT32_MIN), then clamped; so huge volumes and +inf give 0.
        const float v = voice::volume() * 20.0f + 0.5f;
        int32_t step = (v >= -2147483648.0f && v < 2147483648.0f) ? int32_t(v) : INT32_MIN;
        if (step < 0) step = 0;
        if (step > 20) step = 20;
        put<int32_t>(q + 4, step);
    } else if (op == 9 && in_mp) {
        voice::set_volume(float(get<int32_t>(q + 4)) * 0.05f);
    } else {
        call(original_volume, q);
    }
}

// ---- friends (cond 36), friend card (cond 38) and join list (cond 3): stripped queries ----------------------

char title_text[96];
char member_text[16][96];
int32_t friends_cur = 0;
std::string friends_sel;   // the friend (or request) of the cursor row: the cursor follows it (PS3 by friend id)
int32_t join_cur = 0;

// Texts of the PS3 friends list and friend card (spec parity-b-menus.md 0, 3, 4, 11.1, 11.5).
constexpr uint32_t kTextTitle = 0xfd2de693;   // the game's title: "this friend plays this game"
constexpr uint32_t kTextSinglePlayer = 0x77f62775, kTextAnotherGame = 0x953825d0, kTextInLobby = 0x84041d79,
                   kTextCinema = 0xea3bb70e, kTextPlayingMatch = 0xcf292b57, kTextMatchEnds = 0x93ac6446,
                   kTextLessThanMinute = 0x20ae4825, kTextHour = 0x669b8ea8, kTextHours = 0x5879aefb,
                   kTextMinute = 0x9d59ec8a, kTextMinutes = 0x38e8ba93;
constexpr uint32_t kDcLevels = 0x0dec97a4;
constexpr uint32_t kElementStatusDot = 0xf5a294f3, kElementCardMap = 0x0a071025, kElementCardType = 0x57135906;
constexpr char kBreak[] = "\\";   // the game's line break

std::string replaced(std::string text, const char *token, const std::string &value) {
    const size_t at = text.find(token);
    if (at != std::string::npos) text.replace(at, std::strlen(token), value);
    return text;
}
std::string headline() { return std::string("|@H|") + game_text(kTextTitle) + "|@N|"; }

// Column 3 of a friend row (PS3 0x4c1fe0).
std::string friend_status(const online::FriendView &v) {
    if (!v.multiplayer) {
        if (v.this_game) return headline() + kBreak + game_text(kTextSinglePlayer);
        return v.online ? game_text(kTextAnotherGame) : "";
    }
    std::string t = headline();
    if (!v.details) return t;   // a version 1 presence: in this game's multiplayer, nothing more known
    t += kBreak;
    if (v.party >= 2) t += replaced(game_text(kTextPartyCount), "[A]", std::to_string(v.party)) + kBreak;
    if (v.activity == 1) t += game_text(kTextInLobby);
    else if (v.activity == 2) t += game_text(kTextCinema);
    else if (v.activity == 3) t += std::string(game_text(kTextPlayingMatch)) + kBreak;
    return t;
}

// The friend's net game record (*net-games*), NULL when unknown.
const uint8_t *net_game_record(int index) {
    void *sym = dc_lookup(kDcNetGames);
    if (!sym || index < 0) return nullptr;
    const DcTable games = read_table(sym);
    return games.records && index < games.count ? games.records + 0x90 * index : nullptr;
}
const uint8_t *game_type_record(int32_t type) {
    void *sym = dc_lookup(kDcGameTypes);
    if (!sym || type < 0) return nullptr;
    const DcTable types = read_table(sym);
    return types.records && type < types.count ? types.records + 0x44 * type : nullptr;
}
std::string map_name(uint32_t level_id) {
    void *sym = dc_lookup(kDcLevels);
    if (!sym) return {};
    const DcTable levels = read_table(sym);
    for (int i = 0; levels.records && i < levels.count; ++i)
        if (get<uint32_t>(levels.records + 0x38 * i) == level_id) return game_text(get<uint32_t>(levels.records + 0x38 * i + 4));
    return {};
}

// [TIME] (PS3 0x929df8 = PS4 0xcddfe0): whole hours modulo 24 and minutes modulo 60, no seconds.
std::string time_text(uint64_t seconds) {
    const unsigned h = unsigned(seconds / 3600 % 24), m = unsigned(seconds / 60 % 60);
    char b[192];
    if (!h && !m) return game_text(kTextLessThanMinute);
    if (!m) std::snprintf(b, sizeof b, "%d %s", int(h), game_text(h == 1 ? kTextHour : kTextHours));
    else if (!h) std::snprintf(b, sizeof b, "%d %s", int(m), game_text(m == 1 ? kTextMinute : kTextMinutes));
    else
        std::snprintf(b, sizeof b, "%d %s %d %s", int(h), game_text(h == 1 ? kTextHour : kTextHours), int(m),
                      game_text(m == 1 ? kTextMinute : kTextMinutes));
    return b;
}

// Card row 1 in a match (PS3 0x4c1a08): "- <map> / <type>", co-op type 4 adds the difficulty, the competitive
// types the variation and "- Match ends in [TIME]". The difficulty and variation texts are not known to this
// port (no spec of their ids / record fields): those parts are left out.
std::string match_line(const online::FriendView &v) {
    const uint8_t *game = net_game_record(v.net_game);
    if (!game) return game_text(kTextPlayingMatch);
    const int32_t type = get<int32_t>(game + 0x0c);
    const uint8_t *type_rec = game_type_record(type);
    std::string t = "- " + map_name(get<uint32_t>(game + 4)) + " / " + (type_rec ? game_text(get<uint32_t>(type_rec)) : "");
    if (type == 4 || type == 8 || type == 9 || type == 12 || !v.match_end) return t;
    const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::seconds>(
                                      std::chrono::system_clock::now().time_since_epoch()).count());
    const uint64_t left = std::min<uint64_t>(v.match_end > now ? v.match_end - now : 0, 216000);
    return t + kBreak + "- " + replaced(game_text(kTextMatchEnds), "[TIME]", time_text(left));
}

std::vector<std::string> card_lines(const online::FriendView &v) {
    std::vector<std::string> lines{v.npid};
    if (v.multiplayer || v.this_game) lines.push_back(headline());
    else lines.push_back(v.online ? game_text(kTextAnotherGame) : "");
    std::string second;
    if (v.multiplayer && v.details && v.activity == 1) second = game_text(kTextInLobby);
    else if (v.multiplayer && v.details && v.activity == 2) second = game_text(kTextCinema);
    else if (v.multiplayer && v.details && v.activity == 3) second = match_line(v);
    lines.push_back(second);
    return lines;
}

constexpr int kFriendRows = 1 + 250 + 64;   // row 0 + the friend rows online.cpp keeps
char friend_cells[kFriendRows][2][256];     // column 0 and column 3 per row
char card_cells[3][256];

const char *keep(char (&cell)[256], const std::string &text) {
    const size_t n = std::min<size_t>(text.size(), sizeof cell - 1);
    std::memcpy(cell, text.data(), n);
    cell[n] = 0;
    return cell;
}

// The row of the stored friend now (rows move as the list is sorted), else the stored row.
int32_t friends_row(int32_t count) {
    if (!friends_sel.empty()) {
        const int i = online::friend_index(friends_sel);
        if (i >= 0) friends_cur = i;
    }
    if (count <= 0) return -1;
    return friends_cur < 0 ? 0 : friends_cur >= count ? count - 1 : friends_cur;
}
void store_friend_row(int32_t row, int n) {
    friends_cur = row;
    friends_sel = row >= 0 && row < n ? online::friend_name(row) : std::string();
}

// Online: the friend rows in the PS3 layout (name in column 0, the status block in column 3, status dot and row
// icon), then the friend requests; PC additions kept: the title with the online count, the request rows, one hint
// row while there are no friends. PS3 has no "Browse Online Games" row in this list (owner 2026-11-27): removed.
void query_friends_online(uint8_t *q) {
    const int n = online::friend_count();
    const int32_t count = n == 0 ? 1 : std::max(n, 0);
    const int32_t row = get<int32_t>(q + 4);
    switch (op_of(q)) {
    case 1: {
        int on = 0;
        for (int i = 0; i < n; ++i)
            if (online::friend_online(i)) ++on;
        std::snprintf(title_text, sizeof title_text, "Friends (%d of %d online)", on, n);
        set_text(q, title_text);
        break;
    }
    case 2: {
        const uint32_t column = get<uint32_t>(q + 0x54);
        if (row < 0 || row >= count || row >= kFriendRows || (column != 0 && column != 3)) {
            set_text(q, kNothing);
            break;
        }
        const int c = column == 0 ? 0 : 1;
        online::FriendView v;
        if (n == 0) set_text(q, column == 0 ? "No RPCN friends yet. Add friends in the launcher." : kNothing);
        else if (online::friend_view(row, v)) set_text(q, keep(friend_cells[row][c], column == 0 ? v.npid : friend_status(v)));
        else set_text(q, keep(friend_cells[row][c], column == 0 ? online::friend_name(row) : online::request_state(row)));
        break;
    }
    case 3: {
        online::FriendView v;
        if (row < 0 || !online::friend_view(row, v)) break;
        const uint32_t element = get<uint32_t>(q + 0x50);
        auto image = [q](uint32_t type, uint32_t value) {
            put<uint32_t>(q + 8, type);
            put<uint64_t>(q + 0x10, 0);
            put<uint32_t>(q + 0x18, value);
            put<uint64_t>(q + 0x1c, 0);
            put<int32_t>(q + 0x24, -1);
            put<uint32_t>(q + 0x28, 0);
            put<int32_t>(q + 0x2c, -1);
            put<float>(q + 0x30, 1.0f);
        };
        if (element == kElementStatusDot) image(0x20, v.party_member ? 2 : v.online ? 1 : 0);
        else if (element == 0 && v.multiplayer) image(0x21, uint32_t(std::max(v.mode, 1)));
        else if (element == 0 && v.this_game) image(0x21, 0);
        break;   // the avatar (0x2138a9b2): RPCN has none
    }
    case 4: put<int32_t>(q + 4, friends_row(count)); break;
    case 5: put<int32_t>(q + 4, count - 1); break;
    case 7: put<int32_t>(q + 4, 1); break;
    case 8:
        online::refresh_friend_list();
        store_friend_row(friends_row(count), n);
        break;
    case 9: store_friend_row(row, n); break;
    default: break;
    }
}

U2MP_GUEST void query_friends(uint8_t *q) {
    if (online::active()) {
        static int traced = 0;
        if (traced < 96 && op_of(q) != 1) {
            log("friends query: op %u row %d cur %d rows %d", op_of(q), get<int32_t>(q + 4), friends_cur,
                online::friend_count() == 0 ? 1 : std::max(online::friend_count(), 0));
            ++traced;
        }
        query_friends_online(q);
        return;
    }
    // LAN: the list shows the party.
    const int count = session::member_count();
    if (op_of(q) == 1) {
        count_text(title_text, kTextPartyCount, count);
        set_text(q, title_text);
        return;
    }
    plain_list(q, count, friends_cur, [](int32_t r) -> const char * {
        if (r >= 16) return kNothing;
        uint32_t id = 0;
        std::string name;
        if (!session::member_at(r, id, name)) return kNothing;
        if (id == 1) name += std::string(" - ") + game_text(kTextPartyLeader);
        fill96(member_text[r], name);
        return member_text[r];
    });
}

// cond 38: the PAGE_FRIENDACTIONS header, for the friend the list stored (PS3 0x4c1a08). No PC page opens it;
// View Profile shows the same lines (friend_card_lines).
U2MP_GUEST void query_friend_card(uint8_t *q) {
    const int i = friends_sel.empty() ? -1 : online::friend_index(friends_sel);
    online::FriendView v;
    if (!online::active() || i < 0 || !online::friend_view(i, v)) return;
    const std::vector<std::string> lines = card_lines(v);
    switch (op_of(q)) {
    case 1: set_text(q, keep(card_cells[0], lines[0])); break;
    case 2: {
        const int32_t row = get<int32_t>(q + 4);
        if (row == 0 || row == 1) set_text(q, keep(card_cells[1 + row], lines[size_t(1 + row)]));
        break;
    }
    case 3: {
        const uint8_t *game = v.multiplayer && v.details ? net_game_record(v.net_game) : nullptr;
        if (!game) break;
        const uint32_t element = get<uint32_t>(q + 0x50);
        uint32_t type = 0, value = 0, icon = 0;
        if (element == kElementCardMap) {
            type = 0x22;
            value = get<uint32_t>(game + 4);   // the level id: menuimages.cpp 0x22 finds the map's picture
        } else if (element == kElementCardType) {
            const uint8_t *type_rec = game_type_record(get<int32_t>(game + 0x0c));
            if (!type_rec || !get<uint32_t>(type_rec + 8)) break;
            type = 3;
            icon = get<uint32_t>(type_rec + 8);
        } else {
            break;   // the avatar (0xb81e2263): RPCN has none
        }
        put<uint32_t>(q + 8, type);
        put<uint64_t>(q + 0x10, 0);
        put<uint32_t>(q + 0x18, value);
        put<uint32_t>(q + 0x1c, icon);
        put<uint32_t>(q + 0x20, icon ? 1u : 0u);
        put<int32_t>(q + 0x24, -1);
        put<uint32_t>(q + 0x28, 0);
        put<int32_t>(q + 0x2c, -1);
        put<float>(q + 0x30, 1.0f);
        break;
    }
    default: break;
    }
}

U2MP_GUEST void query_join_list(uint8_t *q) {
    const int rows = online::join_rows();
    if (rows >= 0) {
        static int traced = 0;
        if (traced < 96) {
            log("join query: op %u row %d cur %d rows %d", op_of(q), get<int32_t>(q + 4), join_cur, rows);
            ++traced;
        }
        if (op_of(q) == 1)
            set_text(q, online::join_title());
        else
            plain_list(q, rows, join_cur, [](int32_t r) { return online::join_row(r); });
        return;
    }
    if (op_of(q) == 1)
        set_text(q, kNothing);
    else
        plain_list(q, 0, join_cur, [](int32_t) { return kNothing; });
}

// ---- booster slots (cond 77, 78) -------------------------------------------------------------------------
// The booster list query answers op 7 (row enabled) with 0 while the lobby's net game is a co-op type and the Profile
// is open over the custom game lobby; PS3 does the same (MENUS.md 3.3). The owner wants the boosters selectable
// there too (2026-11-27), so op 7 always answers 1 here; everything else is the game's query.
uint64_t original_boosters = 0;
U2MP_GUEST void query_boosters(uint8_t *q) {
    call(original_boosters, q);
    if (op_of(q) == 7) put<int32_t>(q + 4, 1);
}

// ---- Golden Guns (cond 108) --------------------------------------------------------------------------------
// The Profile's option rows 108 / 109 / 110 share one query (0x65c610, its table argument the row's bit: 0x10 Golden
// Guns). A row shows only when its bit is in the licence word [0x104f4b0 + 0x78] (or a region bit): on PS3 the
// golden-weapons licence gave 0x10 (net.bin's licence table, dlc.cpp), on PS4 nothing sets it, so the row had no
// label and stayed hidden. The setting itself is NetPlayerData +0x13c (saved with the career), and the PS4 spawn code
// still gives the golden weapon sets of net-patch9 when it is 1 (0x663494). The query runs with the bit added and the
// word put back after.
constexpr uint64_t kLicenceWord = 0x104f4b0 + 0x78;
constexpr uint32_t kGoldenGunsBit = 0x10;
uint64_t original_golden_guns = 0;
U2MP_GUEST void query_golden_guns(uint8_t *q) {
    uint8_t *licences = abs_at(kLicenceWord);
    const uint32_t kept = get<uint32_t>(licences);
    put<uint32_t>(licences, kept | kGoldenGunsBit);
    call(original_golden_guns, q);
    put<uint32_t>(licences, kept);
}

// ---- Matchmaking: Default / DLC (cond 135) -----------------------------------------------------------------
// The Profile's Other Settings row between Taunt and Golden Guns (query 0x65a790, PS3 0x4c09a8): label "Matchmaking",
// values "Default" (0) and "DLC" (1), the setting NetPlayerData +0x1b0 bit 0 (PS3 data block +0x16c), saved with the
// career. The game gives the row its label only while the owned-DLC word [0x1115a30 + 0xa0] is not 0; on PS3 the map
// pack entitlements set it, on PS4 nothing does, so the row stayed hidden and left the gap. The query runs with the
// imported map packs' bits (dlc_packs, bits 1 and 2) in that word and the word put back after; no pack imported: the
// row stays hidden as on a PS3 without map packs. What the value does: session.cpp mm_pack_mask.
constexpr uint64_t kOwnedDlcWord = 0x1115a30 + 0xa0;
uint64_t original_matchmaking = 0;
U2MP_GUEST void query_matchmaking(uint8_t *q) {
    uint8_t *owned = abs_at(kOwnedDlcWord);
    const uint32_t kept = get<uint32_t>(owned);
    put<uint32_t>(owned, kept | (dlc_packs() & 0x6u));
    call(original_matchmaking, q);
    put<uint32_t>(owned, kept);
}

// ---- diagnostic trace of the Profile page queries ---------------------------------------------------------

constexpr uint32_t kTracedConds[] = {105, 77, 78, 134, 40, 41, 80, 82, 135, 108, 109, 110};
constexpr int kTraced = int(sizeof kTracedConds / sizeof kTracedConds[0]);
uint64_t traced_original[kTraced];
std::atomic<int> traced_calls[kTraced];

template <int N> U2MP_GUEST void tracer(uint8_t *q) {
    const uint32_t op = get<uint32_t>(q), row = get<uint32_t>(q + 4);
    call(traced_original[N], q);
    if (traced_calls[N].fetch_add(1) < 12)
        log("profile query cond %u: op %u row %u -> +4 %d +8 %u +0x40 %p +0x54 %u", kTracedConds[N], op, row,
            get<int32_t>(q + 4), get<uint32_t>(q + 8), reinterpret_cast<void *>(get<uint64_t>(q + 0x40)),
            get<uint32_t>(q + 0x54));
}

template <size_t... N> constexpr auto make_tracers(std::index_sequence<N...>) {
    return std::array<QueryFn, sizeof...(N)>{&tracer<int(N)>...};
}

// ---- help bar shoulder icons (MENUS.md 3.8.1) ------------------------------------------------------------
// Four help-bar records (0x18 bytes, read-only data: u64 +0 token string, u32 +8 menu action, ...) carry the tokens
// "|L1-NOSWAP| " / "|R1-NOSWAP| ", which the PS4 text printer no longer knows, so no picture was drawn. On PS3 they
// always draw the L1 / R1 picture, whatever the controller setting. The PS4 printer has no such token: its "|L1|" /
// "|R1|" draw the L2 / R2 pictures while the shoulder setting is 0 and its "|L2|" / "|R2|" then draw L1 / R1. So the
// records get the token that draws L1 (action 0xb, previous) or R1 (0xa, next) under the current setting.

constexpr uint64_t kShoulderSetting = 0x1059640 + 0x4888;    // u32: 0 = the printer swaps L1 / L2 and R1 / R2
constexpr uint8_t kActionPrev = 0xb;   // the other records' action is 0xa (next)
// The Multiplayer pages' Store entry: PS3 and PS4 both use text 90354b88, which reads "Store" in the PS3 text banks and
// "Rewards" in the PS4 ones. The record gets our text "Store" (texts.cpp, every language), as PS3 shows it.
constexpr uint64_t kStoreRecord = 0x10417a0;
constexpr uint32_t kStoreText = sid("u2mp/help/store");

struct HelpRecord {
    uint64_t offset;
    uint8_t tail[16];   // bytes +8..+0x17: action, text id, callback id, shown
};
const HelpRecord kHelpRecords[] = {
    {0x1041710, {0x0b, 0, 0, 0, 0xfb, 0xfd, 0xa2, 0x5b, 0x25, 0x35, 0x68, 0xd1, 1, 0, 0, 0}},   // Leaderboard Prev
    {0x10416f8, {0x0a, 0, 0, 0, 0x31, 0xa0, 0x94, 0x89, 0xef, 0x68, 0x5e, 0x03, 1, 0, 0, 0}},   // Leaderboard Next
    {0x1041758, {0x0b, 0, 0, 0, 0x9c, 0x52, 0x42, 0x18, 0xe2, 0x70, 0x29, 0x6b, 1, 0, 0, 0}},
    {0x10417a0, {0x0a, 0, 0, 0, 0x88, 0x4b, 0x35, 0x90, 0x72, 0xcc, 0x0e, 0x13, 1, 0, 0, 0}},   // Multiplayer Store
};
const char kTokenL1NoSwap[] = "|L1-NOSWAP| ";   // at 0xe8957b
const char kTokenR1NoSwap[] = "|R1-NOSWAP| ";   // at 0xe8956e
const char kIconL1[] = "|L1| ";
const char kIconR1[] = "|R1| ";
const char kIconL2[] = "|L2| ";
const char kIconR2[] = "|R2| ";

bool help_bar_ok = false;
int help_bar_logged = -1;   // the shoulder setting state last logged (-1: none yet)

void check_help_bar() {
    help_bar_ok = false;
    bool same = image.matches(0xe8957b, reinterpret_cast<const unsigned char *>(kTokenL1NoSwap), sizeof kTokenL1NoSwap) &&
                image.matches(0xe8956e, reinterpret_cast<const unsigned char *>(kTokenR1NoSwap), sizeof kTokenR1NoSwap);
    for (const HelpRecord &r : kHelpRecords) same = same && image.matches(r.offset + 8, r.tail, sizeof r.tail);
    if (!same) {
        log("menus: help bar shoulder records differ, Prev / Next icons left alone");
        return;
    }
    help_bar_ok = true;
}

// ---- co-op difficulty: Hard from the start (owner request 2026-11-28) --------------------------------------------
//
// The co-op difficulty lists (Profile, cond 80 query 0x65a1e0; custom lobby, cond 65 query 0x6576d0) end at
// max(highest difficulty won + 1, 2, floor), so a new career sees Easy and Normal until a co-op win on Normal (1 Easy,
// 2 Normal, 3 Hard, 4 Crushing; the highest win is NetPlayerData +0x120, saved with the career). The floor is a u32 at
// 0x104f4b0 + 0x74 that nothing in the game writes (0); only those two queries read it. It becomes 3 here, so every
// player has Hard from the start; Crushing still needs a co-op win on Hard. The career itself is not changed.
// UNCHARTED_U2MP_COOP_HARD=0 leaves it as shipped.
constexpr uint64_t kCoopDifficultyFloor = 0x104f4b0 + 0x74;
constexpr uint32_t kCoopFloorHard = 3;
const unsigned char kFloorReader[] = {0x48, 0x8d, 0x05, 0, 0, 0, 0, 0x8b, 0x40, 0x74};   // lea rax, [floor base]; mov eax, [rax+0x74]

void unlock_coop_hard() {
    const char *env = std::getenv("UNCHARTED_U2MP_COOP_HARD");
    if (env && env[0] == '0') return;
    // both readers: the lea's displacement points at 0x104f4b0 from its own end
    bool same = kCoopDifficultyFloor + 4 <= image.size;
    for (const uint64_t at : {uint64_t(0x65a2d5), uint64_t(0x657817)}) {
        unsigned char want[sizeof kFloorReader];
        std::memcpy(want, kFloorReader, sizeof want);
        const int32_t disp = int32_t(int64_t(0x104f4b0) - int64_t(at + 7));
        std::memcpy(want + 3, &disp, 4);
        same = same && image.matches(at, want, sizeof want);
    }
    const uint32_t floor = same ? get<uint32_t>(abs_at(kCoopDifficultyFloor)) : 0;
    if (!same || (floor != 0 && floor != kCoopFloorHard)) {
        log("menus: co-op difficulty floor not as expected, Hard stays locked until a Normal win");
        return;
    }
    put<uint32_t>(abs_at(kCoopDifficultyFloor), kCoopFloorHard);
    log("menus: co-op difficulty: Easy, Normal and Hard from the start (Crushing after a win on Hard)");
}

// ---- install ----------------------------------------------------------------------------------------

uint8_t *entry(int i) { return abs_at(kQueryTable + kEntrySize * uint64_t(i)); }

// The first entry of `cond` before the table's end marker (fn 0), or NULL.
uint8_t *find_entry(uint32_t cond) {
    for (int i = 0; i < kQueryEntries; ++i) {
        uint8_t *e = entry(i);
        if (get<uint64_t>(e + 8) == 0) return nullptr;
        if (get<uint32_t>(e) == cond) return e;
    }
    return nullptr;
}

}  // namespace

std::vector<std::string> friend_card_lines(int i) {
    online::FriendView v;
    if (!online::friend_view(i, v)) return {};
    return card_lines(v);
}

int32_t playlist_pick_type() {
    const uint32_t id = get<uint32_t>(abs_at(kNpd + 0x104));
    for (int k = 0; k < built; ++k) {
        const Playlist &p = playlists[k];
        if (p.id() != id || p.type_count() <= 0) continue;
        const unsigned r = unsigned(std::rand());
        return p.type(int(r % unsigned(p.type_count())));
    }
    return -1;
}

bool playlist_game_types(std::vector<int32_t> &types) {
    types.clear();
    const uint32_t id = get<uint32_t>(abs_at(kNpd + 0x104));
    for (int k = 0; k < built; ++k) {
        const Playlist &p = playlists[k];
        if (p.id() != id) continue;
        for (int i = 0; i < p.type_count(); ++i) types.push_back(p.type(i));
    }
    return !types.empty();
}

// PS3 kept a maximum party size in each playlist record (+0x1c), checked by Start Match (parity-d-flow1.md 2.2),
// and the maximum player count (+0x18, PS3 0x49a958: 0 when the id is not found).
int32_t playlist_max_party(uint32_t id) {
    for (int k = 0; k < built; ++k)
        if (playlists[k].id() == id) return playlists[k].max_party();
    return 0;
}

int32_t playlist_max_players(uint32_t id) {
    for (int k = 0; k < built; ++k)
        if (playlists[k].id() == id) return playlists[k].max_players();
    return 0;
}

int playlist_type_icons(uint32_t id, uint32_t *icons, int max) {
    const Playlist *p = nullptr;
    for (int k = 0; k < built && !p; ++k)
        if (playlists[k].id() == id) p = &playlists[k];
    if (!p || max <= 0) return 0;
    void *sym = dc_lookup(kDcGameTypes);
    if (!sym) return 0;
    const DcTable types = read_table(sym);
    if (types.count <= 0 || !types.records) return 0;
    int n = 0;
    for (int32_t t = 0; t < types.count && n < max; ++t) {   // game type order, each type once
        bool listed = false;
        for (int i = 0; i < p->type_count() && i < 8; ++i) listed = listed || p->type(i) == t;
        if (listed) icons[n++] = get<uint32_t>(types.records + 0x44 * t + 8);
    }
    return n;
}

int32_t playlist_index(uint32_t id) {
    for (int k = 0; k < built; ++k) {
        if (playlists[k].id() != id) continue;
        const int8_t ps3 = playlists[k].ps3_index();
        return ps3 >= 0 ? ps3 : k;
    }
    return -1;
}

void menus_help_bar_frame() {
    if (!help_bar_ok) return;
    const bool swapped = get<uint32_t>(abs_at(kShoulderSetting)) == 0;   // the printer draws |L2| as L1 then
    bool written = false;
    for (const HelpRecord &r : kHelpRecords) {
        const char *token = r.tail[0] == kActionPrev ? (swapped ? kIconL2 : kIconL1) : (swapped ? kIconR2 : kIconR1);
        const uint64_t wanted = reinterpret_cast<uint64_t>(token);
        uint8_t *p = abs_at(r.offset);
        const uint32_t text = r.offset == kStoreRecord ? kStoreText : get<uint32_t>(p + 0xc);
        if (get<uint64_t>(p) == wanted && get<uint32_t>(p + 0xc) == text) continue;
        const HostCodeWrite writable(p, 0x10);   // read-only data: writable for the write, then put back
        if (!writable.ok()) {
            log("menus: help bar record 0x%llx cannot be made writable (%lu), Prev / Next icons left alone",
                (unsigned long long)r.offset, writable.error());
            help_bar_ok = false;
            return;
        }
        put<uint64_t>(p, wanted);
        put<uint32_t>(p + 0xc, text);
        written = true;
    }
    if (!written || int(swapped) == help_bar_logged) return;
    help_bar_logged = int(swapped);
    log("menus: help bar shoulder icons L1 / R1 (shoulder setting %u: tokens %s)",
        get<uint32_t>(abs_at(kShoulderSetting)), swapped ? "|L2| / |R2|" : "|L1| / |R1|");
}

void install_menus(int &installed) {
    check_help_bar();
    unlock_coop_hard();
    if (kQueryTable + kQueryEntries * kEntrySize > image.size || get<uint32_t>(entry(0)) != 50 ||
        get<uint64_t>(entry(0) + 8) != reinterpret_cast<uint64_t>(abs_at(kStartMatchQuery))) {
        log("menus: query table 0x%llx differs, menu queries not installed", (unsigned long long)kQueryTable);
        return;
    }
    if (!image.executable(kTextFind, 16) || !image.executable(kSetNetGame, 16) || !image.executable(kDcLookup, 16)) {
        log("menus: guest helpers not where expected, menu queries not installed");
        return;
    }

    struct Wrap {
        uint32_t cond;
        uint64_t expected;
        const char *name;
        QueryFn host;
        uint64_t *original;
    };
    const Wrap wraps[] = {
        {133, 0x65e890, "profile panel", query_profile, &original_profile},
        {49, 0x659a90, "party status line", query_status, &original_status},
        {55, 0x6562d0, "PLAYLIST", query_playlist, &original_playlist},
        {39, 0x65bcd0, "LIST_PLAYERSMET", query_players_met, &original_met},
        {104, 0x65f3f0, "voice volume slider", query_volume, &original_volume},
        {77, 0x659bd0, "LIST_BOOSTERS0", query_boosters, &original_boosters},
        {78, 0x659bd0, "LIST_BOOSTERS1", query_boosters, &original_boosters},
        {108, 0x65c610, "Golden Guns", query_golden_guns, &original_golden_guns},
        {135, 0x65a790, "Matchmaking", query_matchmaking, &original_matchmaking},
    };
    for (const Wrap &w : wraps) {
        uint8_t *e = find_entry(w.cond);
        if (!e || get<uint64_t>(e + 8) != reinterpret_cast<uint64_t>(abs_at(w.expected)) ||
            !image.executable(w.expected, 1)) {
            log("%s (cond %u): table entry is not 0x%llx, left alone", w.name, w.cond,
                (unsigned long long)w.expected);
            continue;
        }
        *w.original = get<uint64_t>(e + 8);
        put<uint64_t>(e + 8, reinterpret_cast<uint64_t>(w.host));
        log("%s (cond %u): 0x%llx wrapped", w.name, w.cond, (unsigned long long)w.expected);
        installed += 1;
    }

    // Trace: every entry of the table counts here, the end marker does not stop the search.
    static constexpr auto tracers = make_tracers(std::make_index_sequence<kTraced>());
    for (int n = 0; n < kTraced; ++n)
        for (int i = 0; i < kQueryEntries; ++i) {
            uint8_t *e = entry(i);
            if (get<uint64_t>(e + 8) == 0 || get<uint32_t>(e) != kTracedConds[n]) continue;
            traced_original[n] = get<uint64_t>(e + 8);
            put<uint64_t>(e + 8, reinterpret_cast<uint64_t>(tracers[size_t(n)]));
            break;
        }

    struct Stripped {
        uint32_t cond;
        uint64_t address;
        const char *name;
        QueryFn host;
    };
    const Stripped stripped[] = {
        {36, 0x65bbb0, "LIST_FRIENDS query", query_friends},
        {3, 0x659590, "join game list query", query_join_list},
        {38, 0x65bbc0, "friend card query", query_friend_card},
    };
    for (const Stripped &s : stripped) {
        uint8_t *e = find_entry(s.cond);
        if (!e || get<uint64_t>(e + 8) != reinterpret_cast<uint64_t>(abs_at(s.address)) || !is_ret_stub(s.address)) {
            log("%s at 0x%llx: not the stripped query this port expects, left alone", s.name,
                (unsigned long long)s.address);
            continue;
        }
        if (replace_function(s.address, 5, reinterpret_cast<void *>(s.host), s.name)) installed += 1;
    }
}

}  // namespace u2mp
