// u2mp session: the party and match session layer of the Uncharted 2 multiplayer port.
//
// The PS4 build of the game lost the PS3 multiplayer bootstrap, the party and match sessions, the
// lobby roster queries, Start Match, Leave Multiplayer and the per-frame network update. This file
// stands in for all of them on PC: it owns the module's one transport (one PC hosts, the others join
// it by LAN address or over the online link), keeps the party member list and copies it into the
// game's session objects and player trackers, replaces the five stripped menu functions, drives the
// network once per game frame (from a menu page handler and from the wrapped match state updates),
// owns the net clock the game reads, and logs diagnostics along the match start path.
//
// Reimplemented from the behaviour description u2mp/docs/spec/session.md.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "cinema.h"
#include "frontend3.h"
#include "frontend6.h"
#include "gamenet.h"
#include "net.h"
#include "online.h"
#include "peer_rules.h"
#include "progression.h"
#include "roster_wire.h"
#include "session_api.h"
#include "start_rules.h"
#include "u2mp_internal.h"
#include "grenades.h"
#include "unlocks.h"
#include "voice.h"

extern "C" int runtime_file_translate(const char *guest, char *out, size_t size);

namespace u2mp::session {
void install(int &installed);
}

namespace u2mp {
namespace {

// ---- game memory ------------------------------------------------------------------------------------
// Image offsets of the objects and values this file reads or writes.
constexpr uint64_t kNetSm = 0x14ce97c;
constexpr uint64_t kPartySession = 0x105efb8;
constexpr uint64_t kMatchSession = 0x1065b08;
constexpr uint64_t kNetInfo = 0x104f1c8;
constexpr uint64_t kNgm = 0x113b760;
constexpr uint64_t kNpd = 0x11474f8;
constexpr uint64_t kPackageTable = 0x1130390;
constexpr uint64_t kNetImageTable = 0x1130078;
constexpr uint64_t kGameFrame = 0x11162c4;
constexpr uint64_t kGameClock = 0x1116180 + 0x204;   // 300 ticks per second, from boot
constexpr uint64_t kNetClockPtr = 0x113a040;
constexpr uint64_t kAbortByte = 0x113a270;
constexpr uint64_t kMatchOver = 0x104f1c0;
constexpr uint64_t kMatchOverCompanion = 0x14ce978;
constexpr uint64_t kIdleTimer = 0x14cec0c;
constexpr uint64_t kCustomChoiceCount = 0x113ddb0;
constexpr uint64_t kLevelListHead = 0x1534140;
constexpr uint64_t kAbortSlot = 0x103fa70;
constexpr uint64_t kAbortOriginal = 0x5e9f10;

// Guest functions.
constexpr uint64_t kPopPage = 0x5cc6f0;
constexpr uint64_t kClearNetPlayers = 0x63db90;
constexpr uint64_t kClearTracker = 0x662a30;
constexpr uint64_t kCreateEvent = 0x6180f0;
constexpr uint64_t kMessageApply = 0x62dc50;
constexpr uint64_t kSendEventAll = 0x644ab0;
constexpr uint64_t kRequestPackage = 0x56ad90;
constexpr uint64_t kPackagesLoaded = 0x56ce50;
constexpr uint64_t kLoaderBusy = 0xa18010;
constexpr uint64_t kLookup = 0xa15e70;
constexpr uint64_t kPresentPlayers = 0x63ffb0;
constexpr uint64_t kSetNetGame = 0x63dc30;
constexpr uint64_t kMenuReset = 0x5cd1e0;
constexpr uint64_t kTeamColor = 0x64aeb0;
constexpr uint64_t kOwnerChanged = 0x5e9f30;

// Ids and page ids.
constexpr uint32_t kNetGamesId = 0x3b9a067d;
constexpr uint32_t kLevelsId = 0x0dec97a4;
constexpr uint32_t kRankTableId = 0xc85e199d;
constexpr uint32_t kPageMultiplayer = 0x53cbb5f2;
constexpr uint32_t kPageJoining = 0x1af4bdd2;
constexpr uint32_t kPageLeavingGame = 0xb808d03c;
constexpr uint32_t kPageHostCreate = 0x471007a1;
constexpr uint32_t kPageLobby = 0xb99a3235;
constexpr uint32_t kMessageBoxPage = 0x719e9fec;
constexpr uint64_t kHostCreateHandler = 0x6093f0;
constexpr uint32_t kWaitText = 0x69deab50;
constexpr uint32_t kNoNetworkText = 0x88a082af;   // BUTTON_MULTIPLAYER: no network (PS3 bootstrap 0x438bc8)
// Start Match message boxes (PS3 0x43aab0): body / second text.
constexpr uint32_t kPartyTooBig = 0x58b7e078, kPartyTooBigInfo = 0x3a586923;
constexpr uint32_t kNoMapPack1 = 0x0e39fdf3, kNoMapPack2 = 0x2341ea7b, kMemberNoMapPack = 0x58d7fbb3,
                   kCustomInfo = 0x13cabc15;
constexpr uint32_t kTooManyPlayers = 0x4a936499, kTooFewPlayers = 0xb90de2b1, kTooFewInfo = 0xcca3ca1f;
constexpr uint32_t kOneTeam = 0x101c7f10, kOneTeamInfo = 0x5350146e;
constexpr uint32_t kNetPackage = 0x4240ef2e;
constexpr uint32_t kNetU2mpPackage = 0x5e516de8;
constexpr uint32_t kIconTalking = 0xffa98ec5;
constexpr uint32_t kIconMuted = 0x7a5f5bf1;
constexpr uint16_t kDefaultPort = 27210;
constexpr uint64_t kCtx = 0x55324d50000001ull;   // any non-zero session context; this one is ours

static_assert(sid("PAGE_MULTIPLAYER_JOINING") == kPageJoining, "page id");
static_assert(sid("net") == kNetPackage && sid("net-u2mp") == kNetU2mpPackage, "package ids");

// Session message types (the byte after the layer tag). kTags (11) is newer than the others: the host sends it
// right after every kRoster ({slot, tag length, tag bytes} per member); older peers ignore an unknown type, so
// the roster format itself stays as old peers read it.
enum : uint8_t { kInfo = 1, kRoster = 2, kMode = 3, kFollow = 4, kKick = 5, kTeams = 6, kIds = 8, kClock = 9,
                  kLobby = 10, kTags = 11,
                  // PS3 parity group E (spec parity-e-flow2.md): older peers ignore these types.
                  kClockSync = 12,    // {0, u64 client us} request / {1, u64 echo, u32 host ticks} reply (2.3)
                  kPartyUp = 13,      // {0, u64 party, u8 size} pressed (0x11a) / {1, u64 room} join (0x119) / {2} back (0x118) (2.4)
                  kReserve = 14,      // {0, u64 room, u8 n, n x {u8 len, npid}} request (0x100) / {1, u8 ok} reply (0x101) (2.6)
                  kLeaderLeave = 15 };   // {} the custom-game leader leaves: leave with it (0x117) (2.5)

// ---- raw memory helpers -----------------------------------------------------------------------------
template <typename T> T get(const uint8_t *p) { T v; std::memcpy(&v, p, sizeof v); return v; }
template <typename T> void put(uint8_t *p, T v) { std::memcpy(p, &v, sizeof v); }
uint8_t *g(uint64_t offset) { return image.base + offset; }
uint8_t *ptr_at(const uint8_t *p) { return reinterpret_cast<uint8_t *>(get<uint64_t>(p)); }
uint64_t addr_of(const void *p) { return reinterpret_cast<uint64_t>(p); }
int32_t net_sm() { return get<int32_t>(g(kNetSm)); }
void set_net_sm(int32_t v) { put<int32_t>(g(kNetSm), v); }
uint8_t *menu() { return g(addr::menu_manager); }
uint8_t *ngm() { return g(kNgm); }
int16_t current_game() { return get<int16_t>(ngm() + 0x1ca0); }

template <typename Fn> Fn guest(uint64_t offset) { return reinterpret_cast<Fn>(image.base + offset); }

uint8_t *lookup(uint32_t id) {
    return guest<uint8_t *(U2MP_GUEST *)(uint32_t)>(kLookup)(id);
}

// A script table {s32 count; pad; records}: the record pointer, or NULL if the table is unusable.
struct Table {
    int32_t count = 0;
    uint8_t *records = nullptr;
};
Table table_of(uint8_t *t) {
    Table out;
    if (!t) return out;
    out.count = get<int32_t>(t);
    out.records = ptr_at(t + 8);
    return out;
}

bool coop_type(int32_t type) { return type == 4 || type == 8 || type == 9 || type == 12; }

// ---- module state -----------------------------------------------------------------------------------
constexpr int kSlots = int(net::kMaxPeers) + 1;

struct Member {
    bool used = false;
    uint32_t net_id = 0;
    std::string name;
    std::string hero;
    std::string villain;
    uint8_t rank = 0;
    uint8_t team = 0;
    uint32_t import_notice = 0;
    std::string tag = {};   // the clan tag the member published (kInfo / kTags), "" none; ours is read live
    uint8_t packs = 0xff;   // host: the map packs the member has (dlc_packs bits, kInfo); 0xff = not told (older PC)
};

Member members[kSlots];
net::Transport transport;
bool active = false;
bool hosting = false;
uint32_t my_id = 0;
std::string my_name = "Drake";
uint32_t host_id = 1;
int32_t sent_mode = -1;
int32_t last_sm = 0;
uint32_t frames = 0;
bool kicked = false;
bool host_lost = false;
bool roster_synced = false;
bool multiplayer_page_seen = false;
double leaving_since = 0.0;
bool leaving_seen = false;
int leaving_cleanup = 0;
uint8_t sent_rank = 0xff;
uint8_t sent_team = 0xff;
std::string sent_tag;   // the clan tag our last kInfo (client) or kTags (host) carried
double next_rate_log = 0;
double last_clock = -1;
bool rehost_pending = false;
bool client_leave_requested = false;
bool lobby_return = false;
bool lobby_returning = false;
void *online_state = nullptr;
double lobby_grace = 0.0;
std::map<uint32_t, std::string> npids;   // net id -> NpId (ordered, so the smallest id is found first)
// Host: online ids kicked in this hosted session (Kick From Party, griefer kick) -> refused Hellos since. Lives
// as long as the transport (close_transport clears it), like PS3's block on the room.
std::map<std::string, unsigned> blocked;
int closed_frames = 0;
uint8_t *clock_object = nullptr;
int last_state = -1;
constexpr int kPlayingState = 3;
int64_t next_online_log = 0;
char row_text[kSlots][64];

void clear_members() {
    for (Member &m : members) m = Member();
}

uint32_t slot_net_id(int slot) { return slot == 0 ? host_id : uint32_t(slot) + 1; }

// The party / lobby row order (PS3 0x4bc728): the party host (slot 0) first, then, in slot order, the
// members on the host's team, then the others. Returns the slot of row `row`, or -1.
int slot_of_row(int row) {
    if (row < 0) return -1;
    const uint8_t host_team = members[0].used ? members[0].team : 0;
    int order[kSlots];
    int n = 0;
    if (members[0].used) order[n++] = 0;
    for (int pass = 0; pass < 2; ++pass)
        for (int s = 1; s < kSlots; ++s)
            if (members[s].used && (members[s].team == host_team) == (pass == 0)) order[n++] = s;
    return row < n ? order[row] : -1;
}

std::string cut(const std::string &s, size_t n) { return s.size() > n ? s.substr(0, n) : s; }

// Our clan tag as checked and stored by clantag.cpp (NetPlayerData +0x1a8, 8 bytes NUL-padded, <= 4 chars).
std::string local_tag() {
    const char *t = reinterpret_cast<const char *>(g(kNpd + 0x1a8));
    return cut(std::string(t, strnlen(t, 8)), 4);
}

// A member's name as the rows show it: "[TAG] name" with a clan tag, else the name (PS3 "[%s] %s").
std::string shown_name(const Member &m) {
    const std::string tag = m.net_id == my_id ? local_tag() : m.tag;
    return tag.empty() ? m.name : "[" + tag + "] " + m.name;
}

// A game text by id (the text hook answers our own texts first); "" when there is none.
const char *game_text(uint32_t id) {
    const char *t = guest<const char *(U2MP_GUEST *)(void *, uint32_t)>(0xcddc00)(g(0x1066660), id);
    return t ? t : "";
}

// ---- sessions and trackers --------------------------------------------------------------------------
void set_session(uint64_t session, uint64_t context, uint32_t me, uint32_t host) {
    uint8_t *s = g(session);
    put<uint64_t>(s + 0x10, context);
    put<uint32_t>(s + 0xb00, me);
    put<uint32_t>(s + 0xb04, host);
}

void publish(uint64_t session) {
    // The game's session holds only 10 member records; an 11th member never reaches it.
    uint8_t *records = g(session) + 0x228;
    int written = 0;
    for (int s = 0; s < kSlots && written < 10; ++s) {
        if (!members[s].used) continue;
        uint8_t *r = records + written * 0xd0;
        r[9] = 1;
        put<uint32_t>(r + 0x10, members[s].net_id);
        put<uint32_t>(r + 0x18, members[s].net_id);
        ++written;
    }
    for (int i = written; i < 10; ++i) {
        uint8_t *r = records + i * 0xd0;
        r[9] = 0;
        put<uint32_t>(r + 0x10, 0);
        put<uint32_t>(r + 0x18, 0);
    }
}

void clear_session(uint64_t session) {
    set_session(session, 0, 0, 0);
    uint8_t *records = g(session) + 0x228;
    for (int i = 0; i < 10; ++i) {
        records[i * 0xd0 + 9] = 0;
        put<uint32_t>(records + i * 0xd0 + 0x10, 0);
    }
}

void refresh_sessions() {
    publish(kPartySession);
    if (get<uint64_t>(g(kMatchSession) + 0x10) != 0) publish(kMatchSession);
}

void add_tracker(uint32_t net_id, const std::string &name, bool local) {
    uint8_t *base = ngm() + 0x10;
    int index = -1;
    for (int i = 0; i < 10 && index < 0; ++i) {
        uint8_t *t = base + i * 0x2c0;
        if (t[0x20] != 0 && get<uint32_t>(t + 0x120) == net_id) index = i;
    }
    for (int i = 0; i < 10 && index < 0; ++i)
        if (base[i * 0x2c0 + 0x20] == 0) index = i;
    if (index < 0) {
        log("no free player slot for %s", name.c_str());
        return;
    }
    uint8_t *t = base + index * 0x2c0;
    guest<void(U2MP_GUEST *)(void *)>(kClearTracker)(t);
    const std::string shown = name.empty() ? std::string("Player") : cut(name, 63);
    std::memcpy(t + 0x20, shown.c_str(), shown.size() + 1);
    std::memcpy(t + 0xa0, shown.c_str(), shown.size() + 1);  // message-list name
    put<uint32_t>(t + 0x120, net_id);
    // The scoreboard/team lists read the money rank here; the tracker constructor never clears it.
    const uint32_t rank = session::member_rank(net_id);
    put<uint32_t>(t + 0x128, rank);
    put<uint32_t>(t + 0x130, rank);
    put<uint32_t>(t + 0x238, 0);
    put<int32_t>(t + 0x23c, local ? -1 : int32_t(net_id));
    // The player-setup event matches slots by NpId: with every one empty, all of its entries land on
    // the first slot (the client lost its roster this way).
    char npid[0x24] = {};
    std::snprintf(npid, sizeof npid, "lan-player-%u", net_id);
    std::memcpy(t + 0x240, npid, sizeof npid);
    // The checkpoint start gives spawner N to slot N; the index also picks the co-op character.
    put<int32_t>(t + 0x198, net_id >= 1 && net_id <= 3 ? int32_t(net_id) - 1 : 0);
    t[0x26c] = 0;
    t[0x275] = 1;
    if (!local) t[0x148] = 1;  // ready: EstablishConnection waits for this on a peer
    t[0x27c] = 1;
    if (local) put<uint64_t>(ngm(), addr_of(t));
    if (net_id == host_id) put<uint64_t>(ngm() + 8, addr_of(t));
    log("player %s: net id %u, slot %d%s", name.c_str(), net_id, index, local ? " (local)" : "");
}

void fill_trackers() {
    for (int s = 0; s < kSlots; ++s)
        if (members[s].used) add_tracker(members[s].net_id, members[s].name, members[s].net_id == my_id);
}

// ---- net events -------------------------------------------------------------------------------------
using EventDtor = void(U2MP_GUEST *)(void *);
using EventApply = void(U2MP_GUEST *)(void *, void *);

uint8_t *create_event(int32_t type) {
    return guest<uint8_t *(U2MP_GUEST *)(int32_t)>(kCreateEvent)(type);
}

void release_event(uint8_t *ev) {
    // Events are reference counted; the last reference runs the deleting destructor.
    if (--ev[0x0a] == 0) reinterpret_cast<EventDtor>(get<uint64_t>(ptr_at(ev) + 0x08))(ev);
}

void player_left(uint32_t net_id) {
    if (net_id == 0) return;
    uint8_t *ev = create_event(79);
    if (!ev) return;
    put<uint32_t>(ev + 0xc, net_id);
    guest<void(U2MP_GUEST *)(void *, void *, uint32_t)>(kSendEventAll)(ngm(), ev, 0xffffffffu);
    reinterpret_cast<EventApply>(get<uint64_t>(ptr_at(ev) + 0x20))(ev, nullptr);
    release_event(ev);
}

// The game's party mailbox: a NetEventMessage applied as if it came from `sender`.
std::vector<uint8_t> mailbox_payload;
void mailbox_post(uint32_t code, uint32_t sender, const uint8_t *data, size_t size) {
    uint8_t *ev = create_event(46);
    if (!ev) return;
    mailbox_payload.assign(data, data + size);
    alignas(16) uint8_t connection[0x200] = {};
    put<uint32_t>(connection + 0x160, sender);
    put<uint32_t>(ev + 0xc, code);
    put<uint64_t>(ev + 0x10, addr_of(mailbox_payload.data()));
    put<uint32_t>(ev + 0x18, uint32_t(size));
    guest<void(U2MP_GUEST *)(void *, void *)>(kMessageApply)(ev, connection);
    // The payload is ours: the event must not point at it once we let go.
    put<uint64_t>(ev + 0x10, 0);
    put<uint32_t>(ev + 0x18, 0);
    release_event(ev);
}

// ---- local player facts -----------------------------------------------------------------------------
int local_rank() {
    uint8_t *t = lookup(kRankTableId);
    if (!t) return 0;
    int32_t count = get<int32_t>(t + 0x50);
    uint8_t *thresholds = ptr_at(t + 0x58);
    if (count <= 1 || !thresholds) return 0;
    int32_t money = get<int32_t>(g(kNpd) + 0xd8);
    int32_t last = count - 1;
    for (int32_t i = 0; i < last; ++i) {
        int32_t lo = get<int32_t>(thresholds + 4 * i), hi = get<int32_t>(thresholds + 4 * (i + 1));
        if (lo <= money && money < hi) return i < 255 ? i : 255;
    }
    return last < 255 ? last : 255;
}

int local_team() {
    int32_t team = get<int32_t>(g(kNpd) + 0x114);
    return team == 1 || team == 2 ? team : 0;
}

// ---- menu ---------------------------------------------------------------------------------------------
uint32_t top_page_id() {
    int32_t top = get<int32_t>(menu() + 0x1010);
    if (top < 0 || top > 0x40) return 0;
    uint8_t *page = ptr_at(menu() + 0x9d0 + 8 * top);
    return page ? get<uint32_t>(page + 0x10) : 0;
}

bool multiplayer_page_open() {
    int32_t top = get<int32_t>(menu() + 0x1010);
    if (top < 0 || top >= 200) return true;
    for (int32_t i = 0; i <= top; ++i) {
        uint8_t *page = ptr_at(menu() + 0x9d0 + 8 * i);
        if (page && get<uint32_t>(page + 0x10) == kPageMultiplayer) return true;
    }
    return false;
}

void finish_leaving_game() {
    // The LEAVING_GAME page waits for "left the room", which nothing on PC sets otherwise.
    uint8_t *match = g(kMatchSession);
    if (top_page_id() == kPageLeavingGame) {
        if (match[0xc8] == 0) log("leaving the match: LEAVING_GAME may close (match +0xc8 = 1)");
        put<int32_t>(g(kNetInfo) + 0x34, 0);
        match[0xc8] = 1;
    }
}

void return_from_leaving() {
    uint8_t *m = menu();
    bool popped = false;
    for (int i = 0; i < 5; ++i) {
        const int32_t top = get<int32_t>(m + 0x1010);
        uint8_t *page = top >= 0 && top <= 0x40 ? ptr_at(m + 0x9d0 + 8 * uint64_t(top)) : nullptr;
        if (!page) break;
        const uint32_t id = get<uint32_t>(page + 0x10);
        if (id != kMessageBoxPage && id != kPageLeavingGame) break;
        log("leaving the match: popping page %08x", id);
        guest<void(U2MP_GUEST *)(void *)>(kPopPage)(menu());
        popped = true;
    }
    leaving_cleanup = 300;
    *g(kMatchOver) = 0;
    if (popped) guest<void(U2MP_GUEST *)(void *)>(kMenuReset)(menu());
    if (lobby_returning) {
        set_net_sm(17);
        show_page(kPageLobby);
    } else {
        set_net_sm(18);
        show_page(kPageMultiplayer);
    }
    log("leaving the match: back to %s", lobby_returning ? "the lobby" : "the Multiplayer page");
}

void cleanup_leaving_page() {
    if (hosting || leaving_cleanup <= 0) return;
    --leaving_cleanup;
    const uint32_t id = top_page_id();
    if (id != kPageLeavingGame && id != kMessageBoxPage) return;
    log("leaving the match: closing page %08x", id);
    guest<void(U2MP_GUEST *)(void *)>(kPopPage)(menu());
    show_page(lobby_returning ? kPageLobby : kPageMultiplayer);
}

void leaving_step() {
    if (hosting || !leaving_seen) return;
    const int32_t s = net_sm();
    if (s == 35) {
        if (leaving_since == 0.0) leaving_since = gamenet::clock();
        if (gamenet::clock() - leaving_since < 3.0) return;
    } else if (s != 3 && s != 18) {
        leaving_seen = false;
        leaving_since = 0.0;
        return;
    }
    leaving_seen = false;
    leaving_since = 0.0;
    return_from_leaving();
}

// ---- packages -----------------------------------------------------------------------------------------
uint8_t *find_package(uint32_t id) {
    uint8_t *table = g(kPackageTable);
    uint8_t *buckets = ptr_at(table + 0x10);
    int32_t count = get<int32_t>(table + 0x4c);
    if (!buckets || count <= 0) return nullptr;
    uint32_t h = 0x811c9dc5u;
    for (int i = 0; i < 4; ++i) {
        h ^= (id >> (8 * i)) & 0xff;
        h *= 0x01000193u;
    }
    uint8_t *node = ptr_at(buckets + 8 * (h % uint32_t(count)));
    while (node && get<uint32_t>(node + 0x10) != id) node = ptr_at(node);
    if (!node || node == ptr_at(buckets + 8 * uint64_t(count))) return nullptr;
    return ptr_at(node + 0x18);
}

uint8_t *request_package_internal(const char *name) {
    uint8_t *pkg = guest<uint8_t *(U2MP_GUEST *)(void *, const char *, int32_t)>(kRequestPackage)(
        g(kPackageTable), name, 0);
    if (pkg) pkg[0x18] |= 0x40;   // what the game's own requests set
    return pkg;
}

uint8_t *keep_net_package() {
    if (const char *extra = extra_net_package()) request_package_internal(extra);
    return request_package_internal("net");
}

// The weapons and animation packages the PS3 game loaded on entering Multiplayer (PS3 0xe155d0).
constexpr const char *kPreloadNames[] = {
    "beretta-pistol", "beretta-pistol-magazine", "colt-defender-pistol", "colt-defender-magazine",
    "desert-eagle", "desert-eagle-magazine", "dragunov-magazine", "dragunov-rifle", "fn-fal-clip",
    "fn-fal-rifle", "m4-rifle", "m4-rifle-magazine", "shotgun", "spas12-shotgun", "machine-gun",
    "machine-gun-clip", "rpg", "anim-proto-multiplayer", "anim-proto-shield", "anim-proto-carry-objects",
    "m32-grenade-launcher"};

void preload_packages() {
    const char *env = std::getenv("UNCHARTED_U2MP_PRELOAD");
    if (env && std::strcmp(env, "0") == 0) return;
    int requested = 0, present = 0, missing = 0;
    for (const char *name : kPreloadNames) {
        if (uint8_t *pkg = find_package(sid(name))) {
            pkg[0x18] |= 0x40;   // kept for Multiplayer again (the teardown cleared it last time)
            ++present;
        } else if (request_package_internal(name)) {
            ++requested;
        } else {
            ++missing;
        }
    }
    log("package preload (PS3 0xe155d0): %d requested, %d already present, %d not created", requested, present,
        missing);
}

// Multiplayer teardown (PS3 0x4386d0): the "net" packages and the preload list lose the "keep for multiplayer"
// flag our requests set (package +0x18 bit 0x40); absent ones are skipped. The game's own release of a package
// request on PS4 is not known (spec parity-d-flow1.md question 6): the flag is cleared, nothing is called.
// The level manager's packages in use: the bits set in its slot bitmap (+0x80..+0x97; free = 180 - this).
int packages_in_use() {
    const uint8_t *bits = g(kPackageTable) + 0x80;
    int n = 0;
    for (int i = 0; i < 24; ++i) n += __builtin_popcount(bits[i]);
    return n;
}

// Diagnostics for spec parity-d-flow1.md 2.3: a few seconds after the teardown cleared the keep bits, one log line says
// how many of those packages the level manager dropped. Runs from a page handler of its own (the session's handler
// is gone after the teardown); it is taken out of the table when Multiplayer registers its handler again.
struct ReleaseCheck {
    std::vector<uint32_t> ids;
    int used_before = 0;
    std::chrono::steady_clock::time_point due{};
    bool pending = false;
};
ReleaseCheck release_check;
constexpr auto kReleaseCheckDelay = std::chrono::seconds(5);

bool add_page_handler(void *handler);
U2MP_GUEST void release_check_handler(uint32_t *event) {
    if (!event || event[0] != 6 || !release_check.pending) return;
    if (std::chrono::steady_clock::now() < release_check.due) return;
    release_check.pending = false;
    int gone = 0, loaded = 0, other = 0;
    for (uint32_t id : release_check.ids) {
        const uint8_t *pkg = find_package(id);
        if (!pkg) ++gone;
        else if (get<int32_t>(pkg + 0x30) == 7) ++loaded;
        else ++other;
    }
    log("leaving multiplayer, %lld s later: of %zu released package(s) %d gone, %d still loaded, %d in another state; "
        "level manager packages in use %d -> %d",
        (long long)std::chrono::duration_cast<std::chrono::seconds>(kReleaseCheckDelay).count(), release_check.ids.size(),
        gone, loaded, other, release_check.used_before, packages_in_use());
}

void release_packages() {
    int released = 0;
    release_check.ids.clear();
    auto release = [&](uint32_t id) {
        uint8_t *pkg = find_package(id);
        if (!pkg || !(pkg[0x18] & 0x40)) return;
        pkg[0x18] &= uint8_t(~0x40);
        release_check.ids.push_back(id);
        ++released;
    };
    release(kNetPackage);
    if (const char *extra = extra_net_package()) release(sid(extra));
    for (const char *name : kPreloadNames) release(sid(name));
    log("leaving multiplayer: %d package(s) no longer kept for Multiplayer", released);
    if (released) {
        release_check.used_before = packages_in_use();
        release_check.due = std::chrono::steady_clock::now() + kReleaseCheckDelay;
        release_check.pending = true;
        add_page_handler(reinterpret_cast<void *>(&release_check_handler));
    }
}

void request_actor_package(const char *actor) {
    if (!actor || !*actor || std::strcmp(actor, "hero-default") == 0) return;
    guest<void(U2MP_GUEST *)(void *, const char *, uint32_t)>(0x56d170)(g(0x1130390), actor, 0);
    log("skins: actor '%s' requested from the level manager", actor);
}

void request_match_skin_packages() {
    std::vector<std::string> asked;
    auto ask = [&](const char *actor) {
        if (!actor || !*actor) return;
        for (const std::string &s : asked)
            if (s == actor) return;
        asked.emplace_back(actor);
        request_actor_package(actor);
    };
    // Co-op: every player plays the net game's character of their rank, not their own skin (unlocks.md 5.8a).
    if (unlocks_coop_character(0)) {
        for (int rank = 0; rank < 3; ++rank) ask(unlocks_coop_character(rank));
        return;
    }
    ask(unlocks_local_skin());
    if (std::strcmp(unlocks_local_villain() ? unlocks_local_villain() : "", unlocks_local_skin() ? unlocks_local_skin() : "") != 0)
        ask(unlocks_local_villain());
    for (const Member &m : members)
        if (m.used) {
            ask(m.hero.c_str());
            if (m.villain != m.hero) ask(m.villain.c_str());
        }
}

U2MP_GUEST uint64_t actor_request_hook() {
    unlocks_apply_slot_skins();
    request_match_skin_packages();
    return get<uint64_t>(g(0x104ac00));
}

// ---- messages -----------------------------------------------------------------------------------------
void send_raw(int slot, const std::vector<uint8_t> &body, int except) {
    std::vector<uint8_t> msg;
    msg.reserve(body.size() + 1);
    msg.push_back(gamenet::kSessionMessage);
    msg.insert(msg.end(), body.begin(), body.end());
    if (slot >= 0) {
        transport.send(uint8_t(slot), true, msg.data(), msg.size());
    } else if (except < 0) {
        transport.broadcast(true, msg.data(), msg.size());
    } else {
        for (int s = 1; s <= int(net::kMaxPeers); ++s)
            if (members[s].used && s != except) transport.send(uint8_t(s), true, msg.data(), msg.size());
    }
}

void send_info() {
    std::string name = cut(my_name, 31);
    std::vector<uint8_t> b{kInfo, uint8_t(name.size())};
    b.insert(b.end(), name.begin(), name.end());
    sent_rank = uint8_t(local_rank());
    sent_team = uint8_t(local_team());
    b.push_back(sent_rank);
    b.push_back(sent_team);
    uint32_t fp = unlocks_skin_fingerprint();
    for (int i = 0; i < 4; ++i) b.push_back(uint8_t(fp >> (8 * i)));
    const char *hero = unlocks_local_skin();
    const char *villain = unlocks_local_villain();
    auto push_skin = [&](const char *s) {
        std::string v = cut(s ? s : "", 47);
        b.push_back(uint8_t(v.size()));
        b.insert(b.end(), v.begin(), v.end());
    };
    push_skin(hero);
    push_skin(villain);
    sent_tag = local_tag();
    push_skin(sent_tag.c_str());   // the clan tag: one more length-prefixed string (older hosts stop before it)
    b.push_back(uint8_t(dlc_packs()));   // the map packs we have, for the host's Start Match check (PS3 member data)
    send_raw(0, b, -1);
}

void broadcast_roster() {
    std::vector<uint8_t> b{kRoster};
    for (int s = 0; s < kSlots; ++s) {
        if (!members[s].used) continue;
        roster_wire::put(b, {uint8_t(s), members[s].rank, cut(members[s].name, 31), cut(members[s].hero, 47),
                             cut(members[s].villain, 47)});
    }
    send_raw(-1, b, -1);
    std::vector<uint8_t> tags{kTags};
    for (int s = 0; s < kSlots; ++s) {
        if (!members[s].used) continue;
        const std::string tag = cut(members[s].net_id == my_id ? local_tag() : members[s].tag, 4);
        if (members[s].net_id == my_id) sent_tag = tag;
        tags.push_back(uint8_t(s));
        tags.push_back(uint8_t(tag.size()));
        tags.insert(tags.end(), tag.begin(), tag.end());
    }
    send_raw(-1, tags, -1);
    if (!online::active()) return;
    // Online, clients also need everyone's NpId to map room members to net ids.
    npids.clear();
    std::vector<uint8_t> ids{kIds};
    for (int s = 0; s < kSlots; ++s) {
        if (!members[s].used) continue;
        std::string npid;
        net::Address a;
        if (s == 0) npid = online::my_npid_text();
        else if (transport.peer_address(uint8_t(s), a)) npid = online::npid_at(a);
        if (npid.empty()) continue;
        uint32_t id = members[s].net_id;
        npids[id] = npid;
        std::string shown = cut(npid, 16);
        ids.push_back(uint8_t(id));
        ids.push_back(uint8_t(shown.size()));
        ids.insert(ids.end(), shown.begin(), shown.end());
    }
    send_raw(-1, ids, -1);
}

void broadcast_teams() {
    std::vector<uint8_t> b{kTeams};
    for (int s = 0; s < kSlots; ++s) {
        if (!members[s].used) continue;
        b.push_back(uint8_t(s));
        b.push_back(members[s].team);
    }
    send_raw(-1, b, -1);
}

void sync_local_skins() {
    const char *hero = unlocks_local_skin();
    const char *villain = unlocks_local_villain();
    // Compared as sent (cut to 47), so a long package name echoed back by the roster is no change.
    const std::string h = cut(hero ? hero : "", 47), v = cut(villain ? villain : "", 47);
    for (Member &m : members) {
        if (!m.used || m.net_id != my_id) continue;
        if (m.hero == h && m.villain == v) return;
        m.hero = h;
        m.villain = v;
        if (hosting) broadcast_roster();
        else send_info();
        return;
    }
}

void broadcast_mode(int32_t mode) {
    std::vector<uint8_t> b{kMode};
    for (int i = 0; i < 4; ++i) b.push_back(uint8_t(uint32_t(mode) >> (8 * i)));
    send_raw(-1, b, -1);
}

std::vector<uint8_t> follow_body() {
    std::vector<uint8_t> b{kFollow};
    if (uint64_t room = online::match_room())
        for (int i = 0; i < 8; ++i) b.push_back(uint8_t(room >> (8 * i)));
    return b;
}

void on_info(uint8_t from, const uint8_t *d, size_t size) {
    if (!hosting || from < 1 || from > 10 || size < 2) return;
    Member &m = members[from];
    size_t len = d[1];
    m.name.assign(reinterpret_cast<const char *>(d + 2), len < size - 2 ? len : size - 2);
    if (size >= 3 + len) m.rank = d[2 + len];
    if (size >= 4 + len) m.team = d[3 + len];
    if (size >= 8 + len) {
        uint32_t theirs = get<uint32_t>(d + 4 + len);
        uint32_t mine = unlocks_skin_fingerprint();
        if (theirs != 0 && mine != 0 && theirs != mine) {
            if (m.import_notice != theirs) {
                m.import_notice = theirs;
                log("party: %s's skin table differs (fingerprint %08x, ours %08x): a different import; skins may show "
                    "as others",
                    m.name.c_str(), theirs, mine);
                std::string notice = m.name +
                                     " has a different Uncharted 2 multiplayer import. Their character skins may show "
                                     "differently. Both players should run the importer again.";
                show_notice(notice.c_str());
            }
        } else {
            m.import_notice = 0;
        }
    }
    size_t at = 8 + len;
    if (at < size) {
        const uint8_t n = d[at];
        if (at + 1 + n <= size) {
            m.hero.assign(reinterpret_cast<const char *>(d + at + 1), n);
            at += 1 + n;
        } else {
            at = size;
        }
    }
    if (at < size) {
        const uint8_t n = d[at];
        if (at + 1 + n <= size) {
            m.villain.assign(reinterpret_cast<const char *>(d + at + 1), n);
            at += 1 + n;
        } else {
            at = size;
        }
    }
    // The clan tag (newer clients; an older one sends none and shows untagged).
    m.tag.clear();
    if (at < size && at + 1 + d[at] <= size) {
        m.tag = cut(std::string(reinterpret_cast<const char *>(d + at + 1), d[at]), 4);
        at += 1 + d[at];
    } else {
        at = size;
    }
    // The member's map packs (newer clients; an older one is not checked).
    m.packs = at < size ? d[at] : 0xff;
    refresh_sessions();
    broadcast_roster();
    broadcast_teams();
}

void on_roster(const uint8_t *d, size_t size) {
    if (hosting) return;
    // A roster carries no teams; those come with Teams messages and must survive a new roster.
    uint8_t teams[kSlots];
    for (int s = 0; s < kSlots; ++s) teams[s] = members[s].team;
    clear_members();
    for (const roster_wire::Entry &e : roster_wire::read(d, size, 1, uint8_t(kSlots - 1))) {
        Member &m = members[e.slot];
        m.used = true;
        m.net_id = slot_net_id(e.slot);
        m.name = e.name;
        if (m.name.empty() || m.name == "@") {
            const std::string room_name = online::room_member_name(m.net_id);
            if (!room_name.empty()) {
                m.name = room_name;
                log("party: roster name for net id %u was empty or @; using %s", m.net_id, m.name.c_str());
            }
        }
        m.rank = e.rank;
        m.team = teams[e.slot];
        m.hero = e.hero;
        m.villain = e.villain;
    }
    roster_synced = false;
    int used = 0;
    for (const Member &m : members) {
        if (!m.used) continue;
        ++used;
        if (m.net_id == my_id) roster_synced = true;
    }
    if (!roster_synced && my_id != 0 && transport.local_slot() < kSlots) {
        // Our skins went out with our info already; empty ones here would read as a change and resend it.
        const char *hero = unlocks_local_skin(), *villain = unlocks_local_villain();
        members[transport.local_slot()] = Member{true, my_id, my_name, cut(hero ? hero : "", 47),
                                                 cut(villain ? villain : "", 47), uint8_t(local_rank()),
                                                 uint8_t(local_team())};
        roster_synced = true;
        ++used;
    }
    log("party: roster received (%d member(s), local slot %u)", used, unsigned(transport.local_slot()));
    refresh_sessions();
}

void on_teams(const uint8_t *d, size_t size) {
    if (hosting) return;
    for (size_t at = 1; size - at >= 2; at += 2)
        if (d[at] <= 10) members[d[at]].team = d[at + 1];
}

// kTags from the host: {slot, length, tag} per member, right after its kRoster.
void on_tags(const uint8_t *d, size_t size) {
    if (hosting) return;
    for (size_t at = 1; size - at >= 2;) {
        const uint8_t slot = d[at], len = d[at + 1];
        if (at + 2 + len > size) break;
        if (slot < kSlots) members[slot].tag = cut(std::string(reinterpret_cast<const char *>(d + at + 2), len), 4);
        at += 2 + len;
    }
}

void on_follow(const uint8_t *d, size_t size) {
    if (hosting) return;
    log("following the host into the match");
    // Online custom game: the leader posts 0x10e once the whole party is in its room (online_party_in).
    const bool wait = size >= 9 && online::follow_match_room(get<uint64_t>(d + 1));
    netinit_match();
    g(kNetInfo)[0x08] = 0;
    guest<void(U2MP_GUEST *)(void *)>(kClearNetPlayers)(ngm());
    set_session(kMatchSession, kCtx, my_id, 1);
    publish(kMatchSession);
    uint8_t *match = g(kMatchSession);
    put<int32_t>(match + 0xbc, 0);
    match[0xc8] = 0;
    put<uint64_t>(match + 0xc0, kCtx);
    match[0xb8] = 1;
    fill_trackers();
    request_match_skin_packages();
    if (top_page_id() != kPageJoining) show_page(kPageJoining);   // a join from the list shows it at the selection
    set_net_sm(12);
    if (wait) {
        log("party: waiting in JOINING until the leader says the whole party is in");
        return;
    }
    const uint8_t joined = 1;
    mailbox_post(0x10e, 1, &joined, 1);
}

void on_ids(const uint8_t *d, size_t size) {
    if (hosting) return;
    npids.clear();
    for (size_t at = 1; size - at >= 2;) {
        uint8_t id = d[at], len = d[at + 1];
        if (at + 2 + len > size) break;
        npids[id] = std::string(reinterpret_cast<const char *>(d + at + 2), len);
        at += 2 + len;
    }
}

// ---- PS3 parity group E (spec parity-e-flow2.md) ---------------------------------------------------------
void put_u64(std::vector<uint8_t> &b, uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(uint8_t(v >> (8 * i)));
}
int64_t steady_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
uint32_t own_ticks();
std::string npid_of_net_id(uint32_t net_id) {
    if (net_id == my_id) return online::my_npid_text();
    const auto it = npids.find(net_id);
    return it == npids.end() ? std::string() : it->second;
}
const char *member_label(uint32_t net_id) {
    const int s = net_id == host_id ? 0 : int(net_id) - 1;
    return s >= 0 && s < kSlots && members[s].used ? members[s].name.c_str() : "?";
}

// 2.3 Net clock synchronisation (PS3 NetConnectionUpdate 0x43cdb8 / NpNetUpdate 0x43bdd0). A client asks the host
// for its clock at random 20..120 ms intervals until it has 8 samples; each answer gives the round trip and the
// host's clock corrected by half of it minus ours. The 4 samples with the smallest round trip are averaged and
// our game clock (the clock StartGame and the round timers read) moves by that offset. Once per connection.
struct ClockSample {
    int64_t rtt_us;
    double offset_ticks;
};
std::vector<ClockSample> clock_samples;
bool clock_synced = false;
int64_t next_clock_request = 0;
uint32_t clock_rng = 0x2545f491u;

void clock_sync_reset(const char *why) {
    if (!clock_samples.empty() || clock_synced) log("net clock: synchronising again (%s)", why);
    clock_samples.clear();
    clock_synced = false;
    next_clock_request = 0;
}

void clock_sync_frame() {
    if (hosting || clock_synced || my_id == 0 || !transport.connected()) return;
    const int64_t t = steady_us();
    if (t < next_clock_request) return;
    clock_rng = clock_rng * 1103515245u + 12345u;
    next_clock_request = t + 20000 + int64_t((clock_rng >> 8) % 100001u);
    std::vector<uint8_t> b{kClockSync, 0};
    put_u64(b, uint64_t(t));
    send_raw(0, b, -1);
}

void on_clock_sync(uint8_t from, const uint8_t *d, size_t size) {
    if (size < 2) return;
    if (d[1] == 0) {   // host: answer at once with the request's own time and our clock
        if (!hosting || size < 10) return;
        std::vector<uint8_t> b{kClockSync, 1};
        b.insert(b.end(), d + 2, d + 10);
        const uint32_t ticks = own_ticks();
        for (int i = 0; i < 4; ++i) b.push_back(uint8_t(ticks >> (8 * i)));
        send_raw(from, b, -1);
        return;
    }
    if (d[1] != 1 || hosting || from != 0 || size < 14 || clock_synced) return;
    const int64_t sent = int64_t(get<uint64_t>(d + 2));
    const int64_t rtt = steady_us() - sent;
    if (rtt < 0 || rtt > 5000000) return;   // not one of ours (or from before a restart)
    const uint32_t host_ticks = get<uint32_t>(d + 10);
    const double half = double(rtt) * 0.5 * 300.0 / 1e6;
    clock_samples.push_back({rtt, double(int32_t(host_ticks - own_ticks())) + half});
    if (clock_samples.size() < 8) return;
    std::sort(clock_samples.begin(), clock_samples.end(),
              [](const ClockSample &a, const ClockSample &b) { return a.rtt_us < b.rtt_us; });
    double sum = 0;
    for (int i = 0; i < 4; ++i) sum += clock_samples[size_t(i)].offset_ticks;
    const int32_t offset = int32_t(std::lround(sum / 4.0));
    uint8_t *clock = g(kGameClock);
    put<uint32_t>(clock, get<uint32_t>(clock) + uint32_t(offset));
    clock_synced = true;
    log("net clock: 8 samples, best 4 round trips %.1f..%.1f ms: clock moved by %+d ticks to the host's",
        double(clock_samples[0].rtt_us) / 1000.0, double(clock_samples[3].rtt_us) / 1000.0, int(offset));
}

// 2.6 Slot manager (PS3 0x4d987c..0x4da3f8, room handler 0x4da278). The host of an online match room keeps one
// reservation per player: NpId, party id, expiry. A joining player asks for its whole party (0x100) once its
// transport reaches the host; the host reserves all of them or none and answers (0x101). A newcomer without a
// reservation only gets in while a slot is free besides the live reservations.
struct Reservation {
    std::string npid;
    uint32_t party = 0;
    double expiry = 0;
};
constexpr double kReserveSeconds = 10.0;
std::vector<Reservation> reservations;
uint64_t slots_room = 0;
int slot_capacity = 0;
uint32_t next_party_id = 1;
unsigned slot_refusals = 0;

std::vector<std::string> present_npids() {
    std::vector<std::string> out;
    if (!online::my_npid_text().empty()) out.push_back(online::my_npid_text());
    for (int s = 1; s < kSlots; ++s) {
        net::Address a;
        if (members[s].used && transport.peer_address(uint8_t(s), a)) {
            const std::string n = online::npid_at(a);
            if (!n.empty()) out.push_back(n);
        }
    }
    return out;
}
bool contains(const std::vector<std::string> &v, const std::string &s) { return std::find(v.begin(), v.end(), s) != v.end(); }
Reservation *reservation_of(const std::string &npid) {
    for (Reservation &r : reservations)
        if (r.npid == npid) return &r;
    return nullptr;
}

// Free slots for `newcomers`: the capacity minus the reservations minus the players present without one. Expired
// reservations of players that never arrived are dropped first (PS3 "Timeout User").
int free_slots(const std::vector<std::string> &newcomers, double now) {
    const std::vector<std::string> present = present_npids();
    for (size_t i = 0; i < reservations.size();) {
        if (reservations[i].expiry <= now && !contains(present, reservations[i].npid)) {
            log("slot manager: Timeout User %s (party %u)", reservations[i].npid.c_str(), reservations[i].party);
            reservations.erase(reservations.begin() + std::ptrdiff_t(i));
        } else {
            ++i;
        }
    }
    int unreserved = 0;
    for (const std::string &n : present) unreserved += !reservation_of(n) && !contains(newcomers, n);
    return slot_capacity - int(reservations.size()) - unreserved;
}

bool reserve_party(const std::vector<std::string> &party, double now) {
    int needed = 0;
    for (const std::string &n : party) needed += !reservation_of(n);
    if (needed == 0) {   // a member that followed its leader asks for itself again: only the hold is renewed
        for (const std::string &n : party) reservation_of(n)->expiry = now + kReserveSeconds;
        return true;
    }
    const int free = free_slots(party, now);
    if (needed > free) {
        log("slot manager: failed to reserve %d slot(s) for a party of %zu (%d free)", needed, party.size(), free);
        return false;
    }
    const uint32_t id = next_party_id++;
    for (const std::string &n : party) {
        if (Reservation *r = reservation_of(n)) {
            log("slot manager: updating user %s: party %u", n.c_str(), id);
            r->party = id;
            r->expiry = now + kReserveSeconds;
        } else {
            log("slot manager: adding user %s: party %u", n.c_str(), id);
            reservations.push_back({n, id, now + kReserveSeconds});
        }
    }
    return true;
}

void release_reservation(const std::string &npid) {
    for (size_t i = 0; i < reservations.size(); ++i)
        if (reservations[i].npid == npid) {
            log("slot manager: removing user %s (party %u)", npid.c_str(), reservations[i].party);
            reservations.erase(reservations.begin() + std::ptrdiff_t(i));
            return;
        }
}

// Host, per frame: a new match room of ours starts a new table with the room's capacity and our party as one group.
void slots_frame() {
    const uint64_t room = hosting && online::active() ? online::match_room() : 0;
    if (room == slots_room) return;
    slots_room = room;
    reservations.clear();
    slot_refusals = 0;
    if (!room) return;
    slot_capacity = online::mm_slots() > 0 ? online::mm_slots() : 10;
    log("slot manager: room %llx, %d slots", (unsigned long long)room, slot_capacity);
    reserve_party(present_npids(), gamenet::clock());
}

std::string member_npid(const Member &m);

// AssignTeams' per-slot values (teams.cpp; PS3 0x4a2490 read them from the slot table and the room members): each
// tracker slot's party id from the host's reservations (0 for a player without one, and on LAN, where there is no
// slot table) and its member's level (rank index + 1; 0 for an empty slot).
void teams_frame() {
    static uint64_t shown[10] = {};
    std::string changed;
    uint8_t *base = ngm() + 0x10;
    for (int k = 0; k < 10; ++k) {
        const uint8_t *t = base + uint64_t(k) * 0x2c0;
        uint32_t party = 0, level = 0;
        if (t[0x20]) {
            const uint32_t id = get<uint32_t>(t + 0x120);
            for (const Member &m : members) {
                if (!m.used || m.net_id != id) continue;
                level = uint32_t(m.rank) + 1;
                if (const Reservation *r = reservation_of(member_npid(m))) party = r->party;
                break;
            }
        }
        teams_set_slot(k, party, level);
        const uint64_t now = uint64_t(party) << 32 | level;
        if (now != shown[k]) {
            shown[k] = now;
            char b[48];
            std::snprintf(b, sizeof b, " slot %d: party %u level %u;", k, party, level);
            changed += b;
        }
    }
    if (!changed.empty()) log("team balance values:%s", changed.c_str());
}

bool slots_admit(const std::string &npid) {
    if (!slots_room || npid.empty()) return true;
    const double now = gamenet::clock();
    if (reservation_of(npid) || free_slots({npid}, now) >= 1) return true;
    if (slot_refusals++ % 20 == 0) log("slot manager: %s refused: every slot is taken or reserved", npid.c_str());
    return false;
}

void on_reserve(uint8_t from, const uint8_t *d, size_t size) {
    if (size < 2) return;
    if (d[1] == 1) {   // client: the host's answer (0x101)
        if (hosting || from != 0 || size < 3) return;
        if (d[2]) {
            log("slot manager: the host reserved our party's slots");
        } else {
            log("slot manager: the host has no slots for our party: the join fails");
            online::reservation_refused();
        }
        return;
    }
    if (d[1] != 0 || !hosting || size < 11) return;
    const uint64_t room = get<uint64_t>(d + 2);
    std::vector<std::string> party;
    size_t at = 11;
    for (uint8_t i = 0; i < d[10] && at < size; ++i) {
        const uint8_t len = d[at];
        if (at + 1 + len > size) break;
        party.emplace_back(reinterpret_cast<const char *>(d + at + 1), len);
        at += 1 + size_t(len);
    }
    if (!slots_room || room != slots_room || party.empty()) {
        log("slot manager: request for room %llx ignored (ours %llx)", (unsigned long long)room, (unsigned long long)slots_room);
        return;
    }
    const bool ok = reserve_party(party, gamenet::clock());
    send_raw(from, {kReserve, 1, uint8_t(ok ? 1 : 0)}, -1);
}

// Client: reached the owner of a match room we joined: ask for our party (or for ourselves alone).
void request_reservation() {
    const uint64_t room = online::active() ? online::match_room() : 0;
    if (!room) return;
    std::vector<std::string> party = online::take_led_party();
    const std::string me = online::my_npid_text();
    if (!me.empty() && !contains(party, me)) party.insert(party.begin(), me);
    if (party.empty()) return;
    std::vector<uint8_t> b{kReserve, 0};
    put_u64(b, room);
    b.push_back(uint8_t(std::min<size_t>(party.size(), 10)));
    for (size_t i = 0; i < party.size() && i < 10; ++i) {
        const std::string n = cut(party[i], 32);
        b.push_back(uint8_t(n.size()));
        b.insert(b.end(), n.begin(), n.end());
    }
    send_raw(0, b, -1);
    log("slot manager: asking the host of room %llx for %zu slot(s)", (unsigned long long)room, party.size());
}

// 2.4 Post-match Party Up (PS3 popup handler 0x43df00; PS4 page 0x471007a1 over POSTGAME, handler 0x5ec4e0 = ret).
// Pressing Party Up opens the wait popup: the press goes to the match host (0x11a). The host groups the players who
// pressed 4 s after the first press: a party stays whole, parties merge while the group stays below 6 players, the
// first party of a group leads it. The others of a group join the leader's party room (0x119); everyone else goes
// back (0x118). Nobody waits longer than 15 s.
constexpr uint32_t kPagePostgame = 0x6b7378ea;   // PAGE_MULTIPLAYER_POSTGAME
struct PartyUpPress {
    uint32_t net_id;
    uint64_t party;
    uint8_t size;
};
struct {
    bool running = false;     // our wait popup is up and has no answer yet
    bool answered = false;    // answered (or given up) for the popup that is still up
    double give_up_at = 0;
    std::vector<PartyUpPress> presses;   // host
    double group_at = 0;                 // host: 4 s after the first press
} party_up;

bool page_in_stack(uint32_t id) {
    const int32_t top = get<int32_t>(menu() + 0x1010);
    for (int32_t i = 0; i <= top && i <= 0x40; ++i) {
        uint8_t *page = ptr_at(menu() + 0x9d0 + 8 * uint64_t(i));
        if (page && get<uint32_t>(page + 0x10) == id) return true;
    }
    return false;
}

void party_up_note(uint32_t net_id, uint64_t party, uint8_t size) {
    for (const PartyUpPress &p : party_up.presses)
        if (p.net_id == net_id) return;
    if (party_up.presses.empty()) party_up.group_at = gamenet::clock() + 4.0;
    party_up.presses.push_back({net_id, party, std::max<uint8_t>(size, 1)});
    log("party up: %s pressed Party Up (party %llx of %u)", member_label(net_id), (unsigned long long)party, unsigned(size));
}

void party_up_answer(bool join, uint64_t room) {
    if (!party_up.running) {
        log("party up: an answer without the wait popup: ignored");
        return;
    }
    party_up.running = false;
    party_up.answered = true;
    const bool joining = join && room && online::party_up_join(room);
    log("party up: %s", joining ? "joining the new party's room (0x119)" : "back to matchmaking (0x118)");
    // Leave the match room: in a stranger's matchmaking room back to our own party (a member to its leader).
    if (!joining && online::active() && !hosting && !online::host_in_party())
        online::mm_host_gone("Party Up: back to our party", false);
    // POSTGAME's own 0x118 consumer: pops, NET_SM 19, PAGE_MULTIPLAYER_MATCHMAKING.
    const uint64_t handle = get<uint64_t>(g(kMatchSession) + 0x10);
    mailbox_post(0x118, 1, reinterpret_cast<const uint8_t *>(&handle), 8);
}

void send_party_up_answer(const PartyUpPress &p, bool join, uint64_t room) {
    if (p.net_id == my_id) {
        party_up_answer(join, room);
        return;
    }
    std::vector<uint8_t> b{kPartyUp, uint8_t(join ? 1 : 2)};
    if (join) put_u64(b, room);
    send_raw(int(p.net_id) - 1, b, -1);
}

void party_up_group() {
    std::vector<uint64_t> parties;   // in the order of their first press
    for (const PartyUpPress &p : party_up.presses)
        if (std::find(parties.begin(), parties.end(), p.party) == parties.end()) parties.push_back(p.party);
    auto size_of = [](uint64_t party) {
        int pressed = 0, told = 0;
        for (const PartyUpPress &p : party_up.presses)
            if (p.party == party) {
                ++pressed;
                told = std::max(told, int(p.size));
            }
        return std::max(pressed, told);
    };
    std::vector<std::vector<uint64_t>> groups;
    int group_size = 0;
    for (uint64_t party : parties) {
        const int n = size_of(party);
        if (groups.empty() || group_size + n >= 6) {
            groups.push_back({});
            group_size = 0;
        }
        groups.back().push_back(party);
        group_size += n;
    }
    for (size_t gi = 0; gi < groups.size(); ++gi) {
        const std::vector<uint64_t> &grp = groups[gi];
        const uint64_t lead = grp.front();
        int players = 0;
        for (uint64_t party : grp) players += size_of(party);
        log("party up: group %zu: %zu part%s, %d player(s), led by party %llx", gi + 1, grp.size(),
            grp.size() == 1 ? "y" : "ies", players, (unsigned long long)lead);
        for (const PartyUpPress &p : party_up.presses) {
            if (std::find(grp.begin(), grp.end(), p.party) == grp.end()) continue;
            const bool join = grp.size() > 1 && p.party != lead && lead != 0;
            send_party_up_answer(p, join, lead);
        }
    }
    party_up.presses.clear();
    party_up.group_at = 0;
}

// Per frame (page handler and frame work).
void party_up_frame() {
    const double now = gamenet::clock();
    const bool open = active && top_page_id() == kPageHostCreate && page_in_stack(kPagePostgame);
    if (!open) {
        if (party_up.running) log("party up: the wait popup closed");
        party_up.running = party_up.answered = false;
    } else if (!party_up.running && !party_up.answered) {
        party_up.running = true;
        party_up.give_up_at = now + 15.0;
        const uint64_t party = online::active() ? online::party_room_id() : 1;
        const int size = online::active() ? online::party_size() : std::max(1, session::member_count());
        log("party up: Party Up pressed (party %llx of %d): the match host groups, giving up in 15 s",
            (unsigned long long)party, size);
        if (hosting) {
            party_up_note(my_id, party, uint8_t(std::clamp(size, 1, 10)));
        } else {
            std::vector<uint8_t> b{kPartyUp, 0};
            put_u64(b, party);
            b.push_back(uint8_t(std::clamp(size, 1, 10)));
            send_raw(0, b, -1);
        }
    }
    if (party_up.running && now >= party_up.give_up_at) {
        log("party up: no group after 15 s: giving up");
        party_up_answer(false, 0);
    }
    if (hosting && !party_up.presses.empty() && now >= party_up.group_at) party_up_group();
}

void on_party_up(uint8_t from, const uint8_t *d, size_t size) {
    if (size < 2) return;
    if (d[1] == 0) {
        if (hosting && from >= 1 && from < kSlots && members[from].used && size >= 11)
            party_up_note(members[from].net_id, get<uint64_t>(d + 2), d[10]);
        return;
    }
    if (hosting || from != 0) return;
    if (d[1] == 1 && size >= 10) party_up_answer(true, get<uint64_t>(d + 2));
    else if (d[1] == 2) party_up_answer(false, 0);
}

// 2.9 Players Met (PS3 Playing::Enter 0x444660 -> 0x43bcbc): every match and round start hands the remote players
// to progression_note_met, which skips friends and players already listed (parity-g-friends.md G1).
void note_players_met() {
    const uint8_t *local = ptr_at(ngm());
    std::vector<std::string> names;
    for (int i = 0; i < 10; ++i) {
        const uint8_t *t = ngm() + 0x10 + 0x2c0 * uint64_t(i);
        if (t == local || t[0x20] == 0 || t[0x26c] != 0) continue;
        const char *name = reinterpret_cast<const char *>(t + 0x20);
        names.emplace_back(name, strnlen(name, 64));
    }
    if (!names.empty()) progression_note_met(names);
}

// 2.5 Voluntary leave (PS3 Leave::Enter 0x445608): the custom-game leader asks its party to leave with it (0x117).
bool leader_asked_leave = false;   // set by 0x117: this player leaves with its leader and stays in its party
bool own_party_after_leave = false;

void on_leader_leave(uint8_t from) {
    if (hosting || from != 0) return;
    log("party: the party leader leaves the game: leaving with it (0x117)");
    leader_asked_leave = true;
    // Playing::Update's own 0x117 consumer compares the room handle with netInfo +0x60.
    const uint64_t room = get<uint64_t>(g(kNetInfo) + 0x60);
    mailbox_post(0x117, 1, reinterpret_cast<const uint8_t *>(&room), 8);
}

void on_session_message(uint8_t from, const uint8_t *d, size_t size) {
    if (size == 0) return;
    uint8_t type = d[0];
    if (type >= session::kFrontendMessages) {
        matchmaking::on_message(from, d, size);
        return;
    }
    switch (type) {
    case kInfo: on_info(from, d, size); break;
    case kRoster: on_roster(d, size); break;
    case kTeams: on_teams(d, size); break;
    case kTags: on_tags(d, size); break;
    case kMode:
        if (!hosting && size >= 5) mailbox_post(0x110, 1, d + 1, 4);
        break;
    case kLobby:
        if (!hosting) {
            lobby_return = true;
            lobby_returning = true;
        }
        break;
    case kKick:
        if (!hosting && from == 0) {
            log("party: the host kicked us");
            g(kNetInfo)[0x1c] = 1;
            kicked = true;   // the transport closes on the next frame, outside the receive loop
        }
        break;
    case kFollow: on_follow(d, size); break;
    case kIds: on_ids(d, size); break;
    case kClock:
        if (!hosting && size >= 5 && net_sm() >= 12) {
            const uint32_t theirs = get<uint32_t>(d + 1);
            uint8_t *clock = g(kGameClock);
            const int64_t diff = int64_t(theirs) - int64_t(get<uint32_t>(clock));
            if (diff > 150 || diff < -150) {
                put<uint32_t>(clock, theirs);
                log("game clock set to the host's (%+d ticks)", int(diff));
                clock_sync_reset("coarse correction");   // the latency correction follows again
            }
        }
        break;
    case kClockSync: on_clock_sync(from, d, size); break;
    case kPartyUp: on_party_up(from, d, size); break;
    case kReserve: on_reserve(from, d, size); break;
    case kLeaderLeave: on_leader_leave(from); break;
    case session::kOnlineMessage: online::on_message(from, d + 1, size - 1); break;
    default: break;
    }
}

void on_transport_message(uint8_t slot, const uint8_t *data, size_t size) {
    if (size == 0) return;
    switch (data[0]) {
    case gamenet::kGamePacket: gamenet::receive(slot, data + 1, size - 1); break;
    case gamenet::kSessionMessage: on_session_message(slot, data + 1, size - 1); break;
    case gamenet::kVoice: voice::receive(slot, data + 1, size - 1); break;
    default: break;
    }
}

void on_peer(uint8_t slot, bool joined) {
    if (hosting) {
        if (slot < kSlots) {
            Member &m = members[slot];
            if (joined) {
                uint32_t id = slot_net_id(slot);
                m = Member{true, id, "Player " + std::to_string(slot + 1), "", "", 0, 0};
                gamenet::attach(slot, id, true);
                log("party: peer in slot %u joined", unsigned(slot));
                if (last_state == kPlayingState)
                    log("late join: peer slot %u arrived during Playing; no catch-up path yet", unsigned(slot));
                if (online::active()) {
                    online::on_host_reached(id);
                    // Someone who arrives while the match lobby is up must be told to follow too.
                    if (g(kMatchSession)[0x99] != 0 && net_sm() == 16) {
                        log("online: slot %u joined while the lobby is up: sending the follow", unsigned(slot));
                        send_raw(slot, follow_body(), -1);
                    }
                    matchmaking::on_peer_joined(slot);
                }
            } else {
                log("party: %s left", m.name.c_str());
                if (slots_room) {   // slot manager, member left (PS3 match listener slot 3)
                    const std::string npid = npid_of_net_id(m.net_id);
                    if (!npid.empty()) release_reservation(npid);
                }
                player_left(m.net_id);
                m = Member();
                gamenet::detach(slot);
            }
        }
        refresh_sessions();
        broadcast_roster();
        return;
    }
    if (slot != 0) return;
    if (joined) {
        my_id = slot_net_id(transport.local_slot());
        set_session(kPartySession, kCtx, my_id, host_id);
        gamenet::attach(0, host_id, true);
        if (transport.local_slot() < kSlots) {
            members[transport.local_slot()] = Member{true, my_id, my_name, "", "", uint8_t(local_rank()), uint8_t(local_team())};
        }
        if (!members[0].used) members[0] = Member{true, host_id, "Host", "", "", 0, 0};
        roster_synced = false;
        refresh_sessions();
        send_info();
        log("party: joined the host as net id %u", my_id);
        online::on_host_reached(my_id);
        clock_sync_reset("joined a host");
        request_reservation();
        if (rehost_pending) {
            // A match migration finishes once the new host is reached: the round restarts under it.
            rehost_pending = false;
            set_session(kMatchSession, kCtx, my_id, host_id);
            publish(kMatchSession);
            guest<void(U2MP_GUEST *)(void *)>(kOwnerChanged)(nullptr);
            log("match migration: reached the new host (net id %u); the round restarts under it", host_id);
        }
        return;
    }
    if (online::host_change_expected()) {
        log("online: the old host closed the link; following the leader to its new room");
        gamenet::detach(0);
    } else if (online::on_host_lost()) {
        log("party: lost the host of the matchmaking room; matchmaking searches again");
        gamenet::detach(0);
    } else {
        log("party: lost the host");
        if (last_state == kPlayingState) {
            log("party: ending the match after losing the host");
            uint32_t end_arg = 0;
            guest<void(U2MP_GUEST *)(void *, int, void *)>(0x64a7c0)(ngm(), 3, &end_arg);
        }
        gamenet::detach(0);
        g(kNetInfo)[0x1c] = 1;
        host_lost = true;
    }
}

// ---- transport start ----------------------------------------------------------------------------------
// Host: a Hello from a blocked (kicked) online id gets no slot, the same way a full party turns it away.
bool admit_peer(const net::Address &from) {
    if (blocked.empty() && !slots_room) return true;
    const std::string npid = online::npid_at(from);
    const auto it = npid.empty() ? blocked.end() : blocked.find(npid);
    if (it == blocked.end()) return slots_admit(npid);
    if (it->second++ % 20 == 0)   // Hellos repeat every 0.5 s: one line per 10 s
        log("party: refused a join from %s (kicked from this session; %u tries)", npid.c_str(), it->second);
    return false;
}

void close_transport() {
    transport.close();
    blocked.clear();
    reservations.clear();
    slots_room = 0;
    clock_sync_reset("transport closed");
}

bool start_transport() {
    const char *env_name = std::getenv("UNCHARTED_U2MP_NAME");
    if (env_name && *env_name) my_name = cut(env_name, 31);
    uint16_t port = kDefaultPort;
    if (const char *p = std::getenv("UNCHARTED_U2MP_PORT")) {
        int v = std::atoi(p);
        if (v > 0) port = uint16_t(v);
    }
    transport.on_peer = on_peer;
    transport.on_message = on_transport_message;
    transport.admit = admit_peer;
    transport.on_timeout = [](uint8_t slot) {   // PS3 NpNetUpdate's silent-peer drop (spec parity-e-flow2.md 2.2)
        const char *name = slot < kSlots && members[slot].used ? members[slot].name.c_str() : "?";
        log("*************** %s has left : timed out (nothing heard for %.0f s)", name, net::kTimeoutSeconds);
    };
    blocked.clear();
    gamenet::set_transport(&transport);
    voice::set_transport(&transport);
    clear_members();

    if (net::Link *link = online::start()) {
        if (!transport.host_link(link)) return false;
        hosting = true;
        my_id = 1;
        host_id = 1;
        members[0] = Member{true, 1, my_name, "", "", uint8_t(local_rank()), 0};
        log("online: hosting a party on the online link as %s", my_name.c_str());
        return true;
    }
    const char *join = std::getenv("UNCHARTED_U2MP_JOIN");
    if (join && *join) {
        net::Address address;
        if (!net::Address::parse(join, port, address) || !transport.join(address)) {
            log("cannot join %s", join);
            return false;
        }
        hosting = false;
        my_id = 0;
        log("joining the party at %s", address.text().c_str());
        return true;
    }
    if (!transport.host(port)) {
        log("cannot listen on UDP port %u", unsigned(port));
        return false;
    }
    hosting = true;
    my_id = 1;
    members[0] = Member{true, 1, my_name, "", "", uint8_t(local_rank()), 0};
    log("hosting a party on UDP port %u", unsigned(port));
    return true;
}

// ---- page handler -------------------------------------------------------------------------------------
U2MP_GUEST void page_handler(uint32_t *event);

uint8_t *handler_entry(uint32_t i) { return menu() + 0x24f8 + uint64_t(i) * 0x18; }

// Adds `fn` (a U2MP_GUEST page handler) for every page; false when the table is full. Nothing when it is there already.
bool add_page_handler(void *handler) {
    uint32_t count = get<uint32_t>(menu() + 0x24f0);
    uint64_t fn = addr_of(handler);
    for (uint32_t i = 0; i < count; ++i)
        if (get<uint64_t>(handler_entry(i) + 8) == fn) return true;
    if (count >= 32) return false;
    uint8_t *e = handler_entry(count);
    put<uint32_t>(e, 0);   // page 0: called for every page
    put<uint64_t>(e + 8, fn);
    put<uint64_t>(e + 0x10, 0);
    put<uint32_t>(menu() + 0x24f0, count + 1);
    return true;
}

void remove_page_handler(void *handler) {
    uint32_t count = get<uint32_t>(menu() + 0x24f0);
    uint64_t fn = addr_of(handler);
    for (uint32_t i = 0; i < count; ++i) {
        if (get<uint64_t>(handler_entry(i) + 8) != fn) continue;
        for (uint32_t j = i + 1; j < count; ++j) std::memcpy(handler_entry(j - 1), handler_entry(j), 0x18);
        put<uint32_t>(menu() + 0x24f0, count - 1);
        return;
    }
}

void register_handler() {
    remove_page_handler(reinterpret_cast<void *>(&release_check_handler));   // the last teardown's check
    release_check.pending = false;
    const uint32_t before = get<uint32_t>(menu() + 0x24f0);
    if (!add_page_handler(reinterpret_cast<void *>(&page_handler))) {
        log("menu page handler table full: no per-frame updates in menus");
        return;
    }
    const uint32_t count = get<uint32_t>(menu() + 0x24f0);
    if (count != before) log("per-frame page handler registered (%u handlers)", count);
}

void unregister_handler() { remove_page_handler(reinterpret_cast<void *>(&page_handler)); }

// ---- teardown -----------------------------------------------------------------------------------------
void teardown(bool pop, bool leaving = true) {
    log("leaving multiplayer");
    unregister_handler();
    for (int s = 0; s < kSlots; ++s) gamenet::detach(uint8_t(s));
    voice::stop();
    close_transport();
    online::on_transport_closed("Multiplayer left");
    active = false;
    clear_session(kPartySession);
    clear_session(kMatchSession);
    put<int32_t>(g(kNetInfo) + 0x2c4, 0);
    guest<void(U2MP_GUEST *)(void *)>(kClearNetPlayers)(ngm());
    progression_publish();
    set_net_sm(0);
    clear_members();
    // PS3 0x4386d0's releases: the kept packages, then the net game level preload (PS3 0x4a12e8(ngm, 0, 1, 0)),
    // with the net game still current as on PS3; our own "no net game" comes after them.
    // A cinema transfer has nothing to abort on PC: online::send_cinema hands the whole file over in one call.
    if (leaving) {
        release_packages();
        frontend6::preload_net_game(false, true, false);
    }
    put<int16_t>(ngm() + 0x1ca0, -1);
    if (pop) guest<void(U2MP_GUEST *)(void *)>(kPopPage)(menu());
}

// After a member quit a match on its own (PS3 Leave::Enter: it leaves the party too): it leads a party of its own,
// on the online link or a UDP port (never UNCHARTED_U2MP_JOIN: that would join the old host again).
void host_own_party() {
    bool ok = false;
    if (net::Link *link = online::start()) {
        ok = transport.host_link(link);
    } else {
        uint16_t port = kDefaultPort;
        if (const char *p = std::getenv("UNCHARTED_U2MP_PORT")) {
            const int v = std::atoi(p);
            if (v > 0) port = uint16_t(v);
        }
        ok = transport.host(port) || transport.host(0);
    }
    if (!ok) {
        log("party: no transport for a party of our own: Multiplayer starts a new one");
        return;
    }
    hosting = active = true;
    my_id = host_id = 1;
    members[0] = Member{true, 1, my_name, "", "", uint8_t(local_rank()), uint8_t(local_team())};
    set_session(kPartySession, kCtx, 1, 1);
    publish(kPartySession);
    sent_mode = -1;
    register_handler();
    log("party: left the party with the match (PS3 Leave::Enter): leading a party of our own");
}

// ---- net clock ----------------------------------------------------------------------------------------
// The original net clock object's +0xc is the game clock (0x1116180+0x204, 300 ticks per second);
// snapshot times are unwrapped against it, so it must not be a clock of our own.
uint32_t own_ticks() { return get<uint32_t>(g(kGameClock)); }

void update_net_clock() {
    uint8_t *slot = g(kNetClockPtr);
    if (!clock_object) {
        if (get<uint64_t>(slot) != 0) return;   // the game's own (or someone else's) clock: leave it
        auto *object = static_cast<uint8_t *>(netheap_alloc(0x40, 16));
        if (!object) return;
        std::memset(object, 0, 0x40);
        clock_object = object;
        put<uint64_t>(slot, addr_of(object));
        log("net clock created (300 ticks per second)");
    }
    if (get<uint64_t>(slot) == addr_of(clock_object)) put<int32_t>(clock_object + 0xc, int32_t(own_ticks()));
}

// ---- per-frame work -----------------------------------------------------------------------------------
void skip_net_fade() {
    // Net images fade in from the package's login frame; in menus that reads as a 1 s flicker.
    int32_t now = get<int32_t>(g(kGameFrame));
    for (uint32_t id : {kNetPackage, kNetU2mpPackage}) {
        uint8_t *pkg = find_package(id);
        if (!pkg || get<int32_t>(pkg + 0x30) != 7) continue;
        int32_t age = int32_t(uint32_t(now) - uint32_t(get<int32_t>(pkg + 0x924)));
        if (age < 300) put<int32_t>(pkg + 0x924, int32_t(uint32_t(now) - 300u));
    }
}

void check_net_package() {
    static int32_t logged_state = -100;
    static bool logged_table_null = false;
    static uint32_t logged_count = 0xffffffffu;
    int32_t sm = net_sm();
    bool menus = sm != 0;   // the net images (medals, HUD) must stay requested in a match too
    uint8_t *pkg = menus ? keep_net_package() : find_package(kNetPackage);
    int32_t state = pkg ? get<int32_t>(pkg + 0x30) : -1;
    uint8_t *table = ptr_at(g(kNetImageTable));
    uint32_t count = table ? get<uint16_t>(table + 2) : 0;
    bool table_null = table == nullptr;
    if (state != logged_state || table_null != logged_table_null || count != logged_count)
        log("net package: %s, state %d, flags 0x%x; net image table %s, %u images", pkg ? "present" : "missing",
            state, pkg ? unsigned(get<uint16_t>(pkg + 0x18)) : 0u, table ? "set" : "NULL", count);
    logged_state = state;
    logged_table_null = table_null;
    logged_count = count;
}

void sync_tracker_teams();
void sync_stats_teams();
bool cooperative_match();

void frame_work() {
    // scan_type_vtables();   // off for now: the type-vtable scan and dump are a debugging aid (PR 21)
    if (active || clock_object) update_net_clock();
    online::frame();
    if (!active) return;
    if (!hosting && !roster_synced && frames % 60 == 0) send_info();
    if (hosting && (net_sm() == 16 || net_sm() == 18) && frames % 120 == 0) broadcast_roster();
    sync_local_skins();
    unlocks_apply_slot_skins();
    double now = gamenet::clock();
    if (now == last_clock) return;   // the page handler and the state wrappers share one frame
    last_clock = now;

    transport.pump(now);
    gamenet::frame(now);
    grenades::frame(now);
    if (lobby_return) {
        lobby_return = false;
        if (active) {
            loading_hide("host returned to the lobby");
            lobby_returning = true;
            lobby_grace = now + 5.0;
            uint32_t end_arg = 0;
            uint8_t *ctx = online_state ? ptr_at(reinterpret_cast<uint8_t *>(online_state) + 8) : nullptr;
            if (ngm()) guest<void(U2MP_GUEST *)(void *, int, void *)>(0x64a7c0)(ngm(), 3, &end_arg);
            if (ctx) {
                log("party: host return end flag: abort %u over %u gate %d ctx %p", unsigned(*g(kAbortByte)),
                    unsigned(*g(kMatchOver)), int(get<int32_t>(image.base + 0x1065b08 + 0xc8) != 0), ctx);
                ctx[0x152] = 1;
            }
            log("party: the host returned to the lobby; ending the match but keeping the room");
        }
    }
    if (client_leave_requested) {
        client_leave_requested = false;
        if (active) {
            log("party: leaving the match room and returning to the Multiplayer menu");
            teardown(false, false);   // still in Multiplayer: the packages stay kept
            set_net_sm(3);
            show_page(kPageMultiplayer);
            if (own_party_after_leave) host_own_party();
            own_party_after_leave = false;
        }
    }
    if (last_state == kPlayingState && frames % 60 == 0) tier2_patch_weapon_damage();
    hud_weapon_tick();
    mute_players_frame();
    voice::frame();
    clock_sync_frame();
    slots_frame();
    teams_frame();
    party_up_frame();
    transport.pump(now);

    if (kicked || host_lost) {
        const char *why = kicked ? "kicked" : "host lost";
        log("party: closing the transport (%s); Multiplayer starts a new one", why);
        kicked = host_lost = false;
        for (int s = 0; s < kSlots; ++s) gamenet::detach(uint8_t(s));
        voice::stop();
        close_transport();
        online::on_transport_closed(why);
        active = false;
        clear_members();
        return;
    }

    ++frames;
    skip_net_fade();
    finish_leaving_game();
    if (frames % 60 == 1) check_net_package();
    if (now >= next_rate_log) {
        if (next_rate_log > 0) log("frame hook: %u calls in 10 s", frames);
        frames = 0;
        next_rate_log = now + 10;
    }

    if (transport.connected()) sync_tracker_teams();
    if (frames % 30 == 0) sync_stats_teams();
    if (transport.connected()) {
        const int32_t mode = get<int32_t>(g(kNetInfo) + 0x2c4);
        const bool coop = cooperative_match();
        int32_t team;
        if (coop) {
            team = hosting ? local_team() : int32_t(members[0].team);   // slot 0 is the host (3.3)
            if (team != 1 && team != 2) team = 1;
        } else {
            team = int32_t(transport.local_slot() & 1) + 1;
        }
        if (get<int32_t>(g(kNpd) + 0x114) != team) {
            put<int32_t>(g(kNpd) + 0x114, team);
            log("party: team preference %d for slot %u (%s, mode %d)", int(team),
                unsigned(transport.local_slot()), coop ? "co-op: host team" : "auto-balance", int(mode));
        }
        if (hosting && coop) {
            bool changed = false;
            for (int s = 0; s < kSlots; ++s)
                if (members[s].used && members[s].team != team) {
                    members[s].team = uint8_t(team);
                    changed = true;
                }
            if (changed) broadcast_teams();
        }
    }
    if (frames % 120 == 7) {
        int r = local_rank();
        int t = local_team();
        const bool tag_changed = local_tag() != sent_tag;
        if (hosting && members[0].used && (members[0].rank != r || tag_changed)) {
            members[0].rank = uint8_t(r);
            broadcast_roster();
        } else if (!hosting && my_id != 0 && (r != sent_rank || t != sent_team || tag_changed)) {
            send_info();
        }
        if (hosting && members[0].used && members[0].team != t) {
            members[0].team = uint8_t(t);
            broadcast_teams();
        }
    }

    if (hosting) {
        int32_t mode = get<int32_t>(g(kNetInfo) + 0x2c4);
        if (mode != sent_mode) {
            sent_mode = mode;
            broadcast_mode(mode);
        }
        static double next_clock_send = 0;
        if (now >= next_clock_send) {
            next_clock_send = now + 1.0;
            std::vector<uint8_t> b{kClock};
            const uint32_t ticks = own_ticks();
            for (int i = 0; i < 4; ++i) b.push_back(uint8_t(ticks >> (8 * i)));
            send_raw(-1, b, -1);
        }
    }

    int32_t s = net_sm();
    if (s != last_sm) {
        log("net state %d -> %d", last_sm, s);
        if (s == 16) netinit_dump("state 16");
        if (s == 3) netinit_dump("state 3");
        // Back from a left match to the lobby: the host create page must wait for a new room.
        if (hosting && last_sm == 35 && (s == 3 || s == 18)) {
            if (s != 3) set_net_sm(3);
            g(kMatchSession)[0x99] = 0;
            for (int i = 0; i < 4; ++i) {
                const int32_t top = get<int32_t>(menu() + 0x1010);
                uint8_t *page = top >= 0 && top <= 0x40 ? ptr_at(menu() + 0x9d0 + 8 * top) : nullptr;
                if (!page || get<uint32_t>(page + 0x10) != kMessageBoxPage) break;
                guest<void(U2MP_GUEST *)(void *)>(kPopPage)(menu());
            }
            guest<void(U2MP_GUEST *)(void *)>(kPopPage)(menu());
            guest<void(U2MP_GUEST *)(void *)>(kMenuReset)(menu());
            show_page(kPageLobby);
            uint8_t *wait = ptr_at(menu() + 0x1828);
            if (wait) {
                uint32_t old_text = get<uint32_t>(wait + 8);
                put<uint32_t>(wait + 8, 0);
                log("host leave: wait text %08x cleared; object %p vtable %p", old_text, wait,
                    reinterpret_cast<void *>(uintptr_t(get<uint64_t>(wait))));
            }
        }
        if (!hosting && s == 26) lobby_returning = false;
        if (!hosting && lobby_returning && (s == 3 || s == 18)) {
            set_net_sm(17);
            show_page(kPageLobby);
        }
        last_sm = s;
    }
    if (!hosting && leaving_seen && (s == 3 || s == 18)) {
        leaving_seen = false;
        leaving_since = 0.0;
        return_from_leaving();
    }
    if (s == 35) {
        if (!hosting) leaving_seen = true;
        if (leaving_since == 0.0) leaving_since = now;
        if (hosting && now - leaving_since > 3.0) {
            leaving_since = 0.0;
            log("leaving the match: no client acknowledgement after 3 s; returning to the lobby");
            set_net_sm(3);
            show_page(kPageHostCreate);
        } else if (!hosting && now - leaving_since > 3.0) {
            leaving_since = 0.0;
            leaving_seen = false;
            lobby_returning = false;
            log("leaving the match: no host acknowledgement after 3 s; back to the multiplayer page");
            set_net_sm(18);
            show_page(kPageMultiplayer);
        }
    } else {
        leaving_since = 0.0;
    }

    static bool logged_load_level = false;
    if (!logged_load_level && g(kMatchSession)[0xb54]) {
        logged_load_level = true;
        log("online: NetEventLoadLevel applied (match+0xb54), %s", hosting ? "host" : "client");
    }
}

// 2.11 PS3 drew the game data version ("1.09.078") at the top right of the multiplayer menus; the owner does not
// want it on PC (2026-11-27), so nothing draws it.

U2MP_GUEST void page_handler(uint32_t *event) {
    if (!event || event[0] != 6) return;   // 6: per-frame update
    menus_help_bar_frame();   // help bar L1 / R1 pictures (MENUS.md 3.8.1)
    finish_leaving_game();
    leaving_step();
    cleanup_leaving_page();
    const bool page_open = multiplayer_page_open();
    if (page_open) multiplayer_page_seen = true;
    if (page_open) ticker_refresh();   // PS3 0x447248: the stats ticker's next line (ticker.cpp)
    if (active && net_sm() == 18 && !page_open) {
        if (multiplayer_page_seen) {
            // The player backed out of the party menus without Leave Multiplayer (PS3 tore down here too).
            if (++closed_frames >= 30) {
                closed_frames = 0;
                log("PAGE_MULTIPLAYER closed without Leave Multiplayer: multiplayer teardown (PS3 0x4386d0)");
                teardown(false);
                return;
            }
        } else if (++closed_frames >= 30) {
            // The first entry can beat the page's own load; keep asking until it is really up.
            closed_frames = 0;
            log("PAGE_MULTIPLAYER is not up yet; showing it again");
            show_page(kPageMultiplayer);
        }
    } else {
        closed_frames = 0;
    }
    frame_work();
}

// ---- match state wrappers -----------------------------------------------------------------------------
struct StateSlot {
    uint64_t slot;
    uint64_t original;
};
constexpr StateSlot kStates[10] = {
    {0x100ac58, 0x5f2ea0},   // EstablishConnection
    {0x100ac18, 0x5f2dc0},   // LoadLevels
    {0x100abd8, 0x5f26d0},   // WaitReady
    {0x100ab98, 0x5f14d0},   // Playing
    {0x100ab58, 0x5f1390},   // PreResetting
    {0x100ab18, 0x5f1000},   // Resetting
    {0x100aa98, 0x5f0ec0},   // ResettingReady
    {0x100aa58, 0x5f0b30},   // ShowResults
    {0x100aa18, 0x5f0a90},   // ShowRoundResults
    {0x100a9d8, 0x5eff30},   // Leave
};
enum { kEstablish = 0, kLoadLevels = 1, kWaitReady = 2, kPlaying = 3, kPreResetting = 4, kResetting = 5,
       kResettingReady = 6, kShowResults = 7, kShowRoundResults = 8, kLeave = 9 };
using UpdateFn = uint64_t(U2MP_GUEST *)(void *);
UpdateFn originals[10];

void leave_reason(const char *when) {
    uint8_t *host = ptr_at(ngm() + 8);
    log("%s: abort byte %u, host tracker %s, host excluded %u", when, unsigned(*g(kAbortByte)),
        host ? "set" : "NULL", host ? unsigned(host[0x26c]) : 0u);
    int32_t players = guest<int32_t(U2MP_GUEST *)(void *)>(kPresentPlayers)(ngm());
    uint8_t *npd = g(kNpd);
    log("%s: leave flag %u, error box kind %d state %d, idle %.1f s, players %d, room %d", when,
        unsigned(*g(kMatchOver)), get<int32_t>(npd + 0x44), get<int32_t>(npd + 0x6c0),
        double(get<float>(g(kIdleTimer))), players, net_sm());
}

void log_wait_list(int list, bool skip_flagged) {
    uint8_t *table = g(kPackageTable);
    uint64_t at = list == 0 ? 0x5a98 : 0x5aa8;
    uint32_t count = get<uint32_t>(table + at);
    uint8_t *entries = ptr_at(table + at + 8);
    if (!entries) return;
    if (count > 256) count = 256;
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t *e = entries + i * 0x10;
        uint32_t id = get<uint32_t>(e + 8), flags = get<uint32_t>(e + 0xc);
        if (skip_flagged && (flags & 1)) continue;
        uint8_t *pkg = find_package(id);
        if (pkg && get<int32_t>(pkg + 0x30) == 7 && (get<uint16_t>(pkg + 0x18) & 0x80)) continue;
        log("load levels: list %d package %08x (flags %x): %s, state %d, package flags 0x%x", list, id, flags,
            pkg ? "present" : "missing", pkg ? get<int32_t>(pkg + 0x30) : -1,
            pkg ? unsigned(get<uint16_t>(pkg + 0x18)) : 0u);
    }
}

bool level_loader_busy = false;

void load_levels_diagnostics(uint8_t *state) {
    leave_reason("load levels");
    uint32_t task_level = 0;
    if (uint8_t *task = ptr_at(state + 8))
        if (uint8_t *a = ptr_at(task + 0x80))
            if (uint8_t *b = ptr_at(a + 0x88)) task_level = get<uint32_t>(b + 0x28);
    bool packages = guest<bool(U2MP_GUEST *)(void *)>(kPackagesLoaded)(g(kPackageTable));
    bool busy = guest<bool(U2MP_GUEST *)()>(kLoaderBusy)();
    level_loader_busy = busy;
    uint32_t second = 0;
    Table games = table_of(lookup(kNetGamesId));
    int16_t game = current_game();
    if (games.records && game >= 0 && game < games.count) second = get<uint32_t>(games.records + game * 0x90 + 0x78);

    char text[512];
    size_t used = 0;
    text[0] = 0;
    uint8_t *head = g(kLevelListHead);
    uint8_t *node = ptr_at(head);
    for (int n = 0; n < 24 && node && node != head; ++n, node = ptr_at(node)) {
        if (used >= 496) break;
        int w = std::snprintf(text + used, sizeof text - used, " %08x:%d", get<uint32_t>(node + 0x98),
                              get<int32_t>(node + 0x10));
        if (w > 0) used += size_t(w) < sizeof text - used ? size_t(w) : sizeof text - used - 1;
    }
    log("load levels: packages loaded %d, busy %d, task level %08x, second level %08x; levels%s", int(packages),
        int(busy), task_level, second, text);
    if (!packages) {
        log_wait_list(0, true);
        log_wait_list(1, false);
    }
}

void missing_level_package() {
    static uint32_t waited_id = 0;
    static std::chrono::steady_clock::time_point waited_since;
    static bool file_present = false;
    static uint32_t last_told = 0;   // the id of the last 20 s log (0 none)
    static uint32_t requested_id = 0;

    Table games = table_of(lookup(kNetGamesId));
    Table levels = table_of(lookup(kLevelsId));
    int16_t game = current_game();
    if (!games.records || !levels.records || game < 0 || game >= games.count) return;
    uint32_t level = get<uint32_t>(games.records + game * 0x90 + 4);
    const char *name = nullptr;
    bool found = false;
    for (int32_t i = 0; i < levels.count; ++i) {
        uint8_t *r = levels.records + i * 0x38;
        if (get<uint32_t>(r) == level) {
            found = true;
            name = reinterpret_cast<const char *>(ptr_at(r + 0x18));
        }
    }
    if (!found || !name) return;
    size_t len = std::strlen(name);
    if (len < 4 || std::strcmp(name + len - 4, "-pak") != 0) return;
    uint32_t id = sid(name);
    if (find_package(id)) return;

    auto now = std::chrono::steady_clock::now();
    if (id != waited_id) {
        waited_id = id;
        waited_since = now;
        requested_id = 0;
        std::string guest_path = std::string("/app0/u2data/build/main/pak22/ps4/") + name + "-ingame10.pak";
        char host[1024];
        file_present = false;
        if (runtime_file_translate(guest_path.c_str(), host, sizeof host) == 0) {
            if (FILE *f = std::fopen(host, "rb")) {
                file_present = true;
                std::fclose(f);
            }
        }
    }
    if (file_present) {
        if (level_loader_busy) {
            if (requested_id != id) {
                requested_id = id;
                log("load levels: requesting %s while the loader is busy", name);
                request_package_internal(name);
            }
            return;
        }
        if (now - waited_since < std::chrono::seconds(20)) return;
        if (id != last_told) {
            last_told = id;
            log("load levels: %s has a file but no package after 20 s (not registered?): not waited for", name);
        }
    }
    // The loader would wait forever for a package nothing can produce: mark its wait entry skipped.
    uint8_t *table = g(kPackageTable);
    uint32_t count = get<uint32_t>(table + 0x5a98);
    uint8_t *entries = ptr_at(table + 0x5aa0);
    if (!entries) return;
    if (count > 256) count = 256;
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t *e = entries + i * 0x10;
        uint32_t flags = get<uint32_t>(e + 0xc);
        if (get<uint32_t>(e + 8) != id || (flags & 1)) continue;
        put<uint32_t>(e + 0xc, flags | 1);
        log("load levels: %s is defined nowhere (no registry entry, no file): not waited for", name);
    }
}

int64_t steady_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// WaitReady leaves once the match task's ready byte is set and the net clock reaches its deadline; the
// countdown starts when no used, not excluded tracker other than the local one is left. Show what it reads.
void wait_ready_diagnostics(uint8_t *state) {
    static int64_t next = 0;
    const int64_t ms = steady_ms();
    if (ms < next) return;
    next = ms + 2000;
    uint8_t *task = ptr_at(state + 8);
    uint8_t *clock_object = ptr_at(g(kNetClockPtr));
    uint8_t *base = ngm();
    uint8_t *local = ptr_at(base);
    log("wait ready: all ready %u, deadline %d, timer %d, local tracker 0x%llx", task ? unsigned(task[0x151]) : 0u,
        task ? get<int32_t>(task + 0x154) : 0, clock_object ? get<int32_t>(clock_object + 0xc) : 0,
        (unsigned long long)reinterpret_cast<uint64_t>(local));
    for (int i = 0; i < 10; ++i) {
        uint8_t *t = base + 0x10 + 0x2c0 * i;
        if (!t[0x20]) continue;
        log("wait ready: slot %d net id %u%s, %02x %02x", i, get<uint32_t>(t + 0x120), t == local ? " (local)" : "",
            unsigned(t[0x26c]), unsigned(t[0x274]));
    }
}

bool cooperative_match() {
    if (!active) return false;
    Table games = table_of(lookup(kNetGamesId));
    int16_t game = current_game();
    return games.records && game >= 0 && game < games.count &&
           coop_type(get<int32_t>(games.records + game * 0x90 + 0x0c));
}

using TaskMethod = void(U2MP_GUEST *)(void *);
void task_transition(void *task, uint64_t slot) {
    if (!task) return;
    auto *vt = reinterpret_cast<uint8_t *>(get<uint64_t>(static_cast<uint8_t *>(task)));
    if (!vt || vt < image.base || vt >= image.base + image.size) return;
    uint64_t fn = get<uint64_t>(vt + slot);
    if (fn) reinterpret_cast<TaskMethod>(fn)(task);
}

// The Collection has no village-2-hub-2a-towers-coop.pak, so the checkpoint reset's level list can
// never empty. Keep the game's reset, but leave the dead wait for ResettingReady after it stops shrinking.
void reset_watch(int index, void *state, int64_t ms) {
    static int reset_state = -1;
    static int64_t since = 0, progress = 0, next_log = 0, started_at = 0;
    static uint32_t fewest = ~0u;
    static bool forced = false, marked = false;
    if (index == kPreResetting || index == kResetting || index == kResettingReady) {
        if (reset_state != index) {
            reset_state = index;
            since = progress = ms;
            next_log = 0;
            fewest = ~0u;
            forced = marked = false;
            started_at = 0;
        }
    } else {
        reset_state = -1;
        return;
    }
    uint8_t *task = ptr_at(static_cast<uint8_t *>(state) + 8);
    if (!task || forced) return;
    if (index == kResettingReady) {
        uint8_t started = task[0x151];
        uint32_t start = get<uint32_t>(task + 0x154);
        uint8_t *clock_object = ptr_at(g(kNetClockPtr));
        uint32_t clock = clock_object ? get<uint32_t>(clock_object + 0xc) : 0;
        if (started) {
            if (!started_at) started_at = ms;
            if (start > clock + 0x384 + 60) {
                uint32_t at = clock + 0x384;
                put<uint32_t>(task + 0x154, at);
                log("reset: start %u is %u ticks away (other clock): now %u", start, start - clock, at);
            }
            if (ms - started_at >= 17000) {
                forced = true;
                task[0x151] = 0;
                log("reset: start time %u never came (clock %u): going on to Playing", start, clock);
                task_transition(task, 0x168);
            }
        } else {
            started_at = 0;
            if (hosting && !marked && ms - since >= 8000) {
                marked = true;
                uint8_t *base = ngm() + 0x10;
                uint8_t *local = ptr_at(ngm());
                int count = 0;
                for (int i = 0; i < 10; ++i) {
                    uint8_t *slot = base + 0x2c0 * i;
                    if (!slot[0x20] || slot[0x26c] || slot[0x274] || slot == local) continue;
                    slot[0x274] = 1;
                    ++count;
                }
                log("reset: host marked %d remote slot(s) ready after 8 s", count);
            }
        }
        return;
    }
    if (index == kPreResetting) {
        uint8_t *clock = ptr_at(task + 0x70), *entered = ptr_at(task + 0x50);
        uint32_t tick = clock ? get<uint32_t>(clock + 0xc) : 0;
        uint32_t at = entered ? get<uint32_t>(entered + 0x28) : 0;
        if (ms - since >= 7000 && int32_t(tick - at) < 0x12d) {
            forced = true;
            log("reset: PreResetting stuck after %lld ms: going on to Resetting", (long long)(ms - since));
            task_transition(task, 0x178);
        }
        return;
    }
    struct LevelsLeft {
        uint32_t count;
        uint32_t pad;
        const char *names[33];
    } left{};
    guest<void(U2MP_GUEST *)(void *, void *)>(0x56c420)(g(0x1130390), &left);
    if (left.count < fewest) {
        fewest = left.count;
        progress = ms;
    }
    if (ms >= next_log) {
        next_log = ms + 1000;
        char text[384];
        size_t used = 0;
        text[0] = 0;
        for (uint32_t i = 0; i < left.count && i < 32; ++i) {
            int n = std::snprintf(text + used, sizeof text - used, " %s", left.names[i] ? left.names[i] : "?");
            if (n < 0) break;
            used += size_t(n) < sizeof text - used ? size_t(n) : sizeof text - used - 1;
        }
        log("reset: %lld ms, %u level(s) still to load:%s", (long long)(ms - since), left.count, text);
    }
    if (left.count && ms - progress >= 4000) {
        forced = true;
        log("reset: level list stopped shrinking with %u left: going on to ResettingReady", left.count);
        task_transition(task, 0x180);
    }
}

void mark_others_done() {
    uint8_t *base = ngm() + 0x10;
    uint8_t *local = ptr_at(ngm());
    int marked = 0;
    for (int i = 0; i < 10; ++i) {
        uint8_t *slot = base + 0x2c0 * i;
        if (!slot[0x20] || slot[0x26c] || slot == local) continue;
        slot[0x26d] = 1;
        ++marked;
    }
    log("round results: %d other player slot(s) marked done", marked);
}

bool results_hold(int index, int64_t ms) {
    static int held_index = -1;
    static int64_t since = 0, last = 0;
    if (index != held_index || ms - last > 1000) {
        held_index = index;
        since = last = ms;
        return true;
    }
    last = ms;
    if (ms - since < 10000) return true;
    static int64_t released_at = 0;
    if (released_at != since) {
        released_at = since;
        log("round results: held 10 s (state 0x%llx)", (unsigned long long)kStates[index].original);
        if (hosting) mark_others_done();
    }
    return false;
}

void sync_stats_teams() {
    uint8_t *stats = image.base + 0x1147bd0;
    const int32_t count = get<int32_t>(stats + 8);
    for (int32_t i = 0; i < count && i < 10; ++i) {
        uint8_t *entry = stats + 0x38ec + uint64_t(i) * 0x80;
        const int32_t id = get<int32_t>(entry);
        for (int s = 0; s < 10; ++s) {
            uint8_t *slot = ngm() + 0x10 + 0x2c0 * s;
            if (!slot[0x20] || get<uint32_t>(slot + 0x120) != uint32_t(id)) continue;
            const uint8_t team = slot[0x134];
            if (entry[7] != team) entry[7] = team;
            break;
        }
    }
}

void sync_tracker_teams() {
    uint8_t *base = ngm() + 0x10;
    for (int s = 0; s < kSlots; ++s) {
        const uint8_t team = members[s].team;
        if (!members[s].used || (team != 1 && team != 2)) continue;
        uint8_t *slot = nullptr;
        for (int i = 0; i < 10 && !slot; ++i) {
            uint8_t *t = base + 0x2c0 * i;
            if (t[0x20] && get<uint32_t>(t + 0x120) == members[s].net_id) slot = t;
        }
        if (!slot) continue;
        const int32_t worn = int32_t(team) - 1;
        if (slot[0x157] != team) slot[0x157] = team;
        if (get<int32_t>(slot + 0x134) != worn) {
            put<int32_t>(slot + 0x134, worn);
            log("party: slot %d net id %u team %u synced", int((slot - base) / 0x2c0), members[s].net_id,
                unsigned(team));
        }
    }
}

uint64_t state_update(int index, void *state) {
    online_state = state;
    int64_t ms = steady_ms();
    bool changed = last_state != index;
    last_state = index;
    if (changed || ms >= next_online_log) {
        next_online_log = ms + 10000;
        log("online state: update 0x%llx (vtable slot 0x%llx)", (unsigned long long)kStates[index].original,
            (unsigned long long)(kStates[index].slot - 0x18));
        if (index == kLoadLevels) load_levels_diagnostics(static_cast<uint8_t *>(state));
        if (index == kWaitReady) wait_ready_diagnostics(static_cast<uint8_t *>(state));
        if (index == kLeave) {
            leave_reason("leave");
            // PS3 Leave::Enter's voluntary path: the player chose Leave Game (the leave byte is set).
            const bool voluntary = *g(kMatchOver) != 0 && changed;
            if (active) {
                if (hosting) {
                    if (voluntary && get<int32_t>(g(kNetInfo) + 0x2c4) == 2) {
                        log("party: the party leader leaves the custom game: the party leaves with it (0x117)");
                        send_raw(-1, {kLeaderLeave}, -1);
                    }
                    send_raw(-1, {kLobby}, -1);
                    log("party: host left the match; telling the clients to return to the lobby");
                } else if (!lobby_returning && gamenet::clock() > lobby_grace) {
                    client_leave_requested = true;
                    // A member that quits on its own also leaves its party (the leader in matchmaking too: the
                    // party room hands the party on); one that leaves because its leader asked stays with it.
                    if (changed) own_party_after_leave = voluntary && !leader_asked_leave;
                }
            }
            if (changed) leader_asked_leave = false;
        }
    }
    if (active) reset_watch(index, state, ms);
    if (index == kLoadLevels) missing_level_package();
    if (index == kEstablish) {
        if (changed) {
            clock_sync_reset("match setup");   // PS3: the sample counter restarts with every connection setup
            party_up.presses.clear();
        }
        uint8_t over = *g(kMatchOver), companion = *g(kMatchOverCompanion);
        if (over || companion) {
            // A previous match's end flags would end the new one at once.
            log("match setup: match-over flag was %u (0x14ce978 %u), cleared", unsigned(over), unsigned(companion));
            *g(kMatchOver) = 0;
            *g(kMatchOverCompanion) = 0;
        }
    }
    if (active && (index == kEstablish || index == kLoadLevels || index == kWaitReady) &&
        get<uint64_t>(ngm() + 8) == 0) {
        log("match setup: the net players were cleared, registering the party again");
        fill_trackers();
    }
    static int64_t playing_since = 0;
    static int setup_sends = 0;
    if (index == kPlaying) {
        if (changed) {
            playing_since = ms;
            setup_sends = 0;
            uint8_t *match = g(kMatchSession);
            if (match[0xc8] == 0) {
                match[0xc8] = 1;
                log("playing: the match session is joined (match +0xc8 = 1)");
            }
            *g(kMatchOver) = 0;
            *g(kMatchOverCompanion) = 0;
            note_players_met();
        }
        // A joiner can apply the setup before it has the host's slot (it then keeps team 0 for
        // the host); the host sends it again once every game has its slots.
        if (hosting && setup_sends < 2 && ms - playing_since >= (setup_sends == 0 ? 1000 : 10000)) {
            guest<void(U2MP_GUEST *)(void)>(0x610840)();
            log("playing: the player setup sent again (%d s)", setup_sends == 0 ? 1 : 10);
            ++setup_sends;
        }
    }
    const bool hold = active && (index == kShowResults || index == kShowRoundResults) && results_hold(index, ms);
    uint64_t result = hold ? 0 : originals[index](state);
    if (index == kPlaying && changed) log("playing: update returned");
    frame_work();
    if (index == kPlaying && changed) log("playing: frame work returned");
    return result;
}

template <int I> U2MP_GUEST uint64_t state_wrapper(void *state) { return state_update(I, state); }

using WrapperFn = uint64_t(U2MP_GUEST *)(void *);
constexpr WrapperFn kWrappers[10] = {state_wrapper<0>, state_wrapper<1>, state_wrapper<2>, state_wrapper<3>,
                                     state_wrapper<4>, state_wrapper<5>, state_wrapper<6>, state_wrapper<7>,
                                     state_wrapper<8>, state_wrapper<9>};

U2MP_GUEST void abort_wrapper(void *self) {
    uint64_t caller = addr_of(__builtin_return_address(0));
    uint64_t base = addr_of(image.base);
    bool in_image = caller >= base && caller < base + image.size;
    log("match abort (0x113a270 = 1) from %s0x%llx, session %p", in_image ? "guest " : "",
        (unsigned long long)(in_image ? caller - base : caller), self);
    guest<void(U2MP_GUEST *)(void *)>(kAbortOriginal)(self);
}

// ---- Start Match --------------------------------------------------------------------------------------
void match_choice_diagnostics() {
    int16_t game = current_game();
    log("match choice: mode %d, custom choice list %d entries, net game %d", get<int32_t>(g(kNetInfo) + 0x2c4),
        get<int32_t>(g(kCustomChoiceCount)), int(game));
    uint8_t *raw = lookup(kNetGamesId);
    Table games = table_of(raw);
    if (!raw || game < 0 || game >= games.count || !games.records) {
        log("match choice: net game %d is not in *net-games* (%d records)", int(game), raw ? games.count : -1);
        return;
    }
    uint8_t *record = games.records + game * 0x90;
    uint32_t level = get<uint32_t>(record + 4);
    int32_t type = get<int32_t>(record + 0x0c);
    const char *name = "?";
    if (uint8_t *raw_levels = lookup(kLevelsId)) {
        Table levels = table_of(raw_levels);
        for (int32_t i = 0; levels.records && i < levels.count; ++i) {
            uint8_t *r = levels.records + i * 0x38;
            if (get<uint32_t>(r) != level) continue;
            if (const char *n = reinterpret_cast<const char *>(ptr_at(r + 0x18))) name = n;
            break;
        }
    }
    log("match choice: game type %d, level id %08x, package %s", type, level, name);
}

void host_create() {
    g(kNetInfo)[0x08] = 1;
    guest<void(U2MP_GUEST *)(void *)>(kClearNetPlayers)(ngm());
    put<int32_t>(g(kNetInfo) + 0x34, 0);
    guest<void(U2MP_GUEST *)(void *)>(kMenuReset)(menu());
    put<uint32_t>(ptr_at(menu() + 0x1828) + 8, kWaitText);
    // The host create page's own handler (stripped from the PS4 menu set-up) waits for the room.
    uint32_t count = get<uint32_t>(menu() + 0x24f0);
    uint8_t *entry = nullptr;
    for (uint32_t i = 0; i < count && !entry; ++i)
        if (get<uint32_t>(handler_entry(i)) == kPageHostCreate) entry = handler_entry(i);
    if (!entry && count < 32) {
        entry = handler_entry(count);
        put<uint32_t>(menu() + 0x24f0, count + 1);
    }
    if (entry) {
        put<uint32_t>(entry, kPageHostCreate);
        put<uint64_t>(entry + 8, addr_of(g(kHostCreateHandler)));
        put<uint64_t>(entry + 0x10, 0);
    }
    register_handler();
    show_page(kPageHostCreate);
    set_net_sm(3);
}

void create_match_now() {
    log("Start Match: creating the match");
    g(kMatchSession)[0xc8] = 0;
    match_choice_diagnostics();
    netinit_match();
    netinit_dump("before create");
    host_create();
    netinit_dump("after create");
    set_session(kMatchSession, kCtx, 1, 1);
    publish(kMatchSession);
    fill_trackers();
    request_match_skin_packages();
    put<int32_t>(g(kMatchSession) + 0x9c, 0);
    if (online::create_match_room()) return;   // online_match_ready() finishes it, or online_match_abort() ends it
    g(kMatchSession)[0x99] = 1;
    send_raw(-1, follow_body(), -1);
}

void pick_matchmaking_game() {
    Table games = table_of(lookup(kNetGamesId));
    if (!games.records || games.count <= 0) return;
    int32_t chosen = playlist_pick_type();
    int16_t game = current_game();
    int32_t type = chosen >= 0 ? chosen : get<int32_t>(games.records + (game < 0 ? 0 : game) * 0x90 + 0x0c);
    std::vector<int32_t> candidates;
    for (int32_t i = 0; i < games.count; ++i)
        if (get<int32_t>(games.records + i * 0x90 + 0x0c) == type && dlc_net_game_available(i)) candidates.push_back(i);
    if (candidates.empty()) return;
    int32_t pick = candidates[size_t(std::rand()) % candidates.size()];
    guest<void(U2MP_GUEST *)(void *, int16_t, void *, int32_t)>(kSetNetGame)(ngm(), int16_t(pick), nullptr, 10);
    log("matchmaking: net game %d of %zu of type %d", pick, candidates.size(), type);
}

// ---- replaced menu functions --------------------------------------------------------------------------
U2MP_GUEST uint32_t state_helper_trace(void *a, void *b, void *c, void *d, uint64_t e, uint64_t f) {
    static int calls = 0;
    if (calls < 8) log("tracker: state helper enter");
    const uint32_t result = guest<uint32_t(U2MP_GUEST *)(void *, void *, void *, void *, uint64_t, uint64_t)>(0x634970)(
        a, b, c, d, e, f);
    if (calls < 8) log("tracker: state helper leave");
    ++calls;
    return result;
}

U2MP_GUEST void *state_alloc_trace(uint64_t size, void *context, uint32_t align) {
    static int calls = 0;
    if (calls < 8) log("tracker: state alloc enter %llu", (unsigned long long)size);
    void *result = guest<void *(U2MP_GUEST *)(uint64_t, void *, uint32_t)>(0xb332a0)(size, context, align);
    if (calls < 8) log("tracker: state alloc leave");
    ++calls;
    return result;
}

U2MP_GUEST void state_send_trace(void *tracker) {
    static int calls = 0;
    if (calls < 8) log("tracker: state send enter");
    guest<void(U2MP_GUEST *)(void *)>(0x60ded0)(tracker);
    if (calls < 8) log("tracker: state send leave");
    ++calls;
}

// ---- Multiplayer entry: the connecting page (PS3 PAGE_MULTIPLAYER_CONNECTING) ---------------------------------
// On PS3 the main menu's Multiplayer button starts the network start-up and shows PAGE_MULTIPLAYER_CONNECTING: the
// "Connecting..." box with a progress bar, which follows the start-up's step count, then the Multiplayer page (or a
// message box over the main menu when the network cannot start). PS4 kept the page and its control: the label is
// text ad9f50d9 and the bar shows netInfo +0x2c0 as steps of 44 (0x5ae980); nothing writes that word on PS4. Ours:
// the button shows the page; a page handler runs our start-up over the next frames (the same calls as before, in the
// same order), moves the bar with it and then opens the Multiplayer page. The box stays at least kConnectingShown
// seconds so it can be seen (PS3's start-up took seconds; ours takes a few frames).
constexpr uint32_t kPageConnecting = sid("PAGE_MULTIPLAYER_CONNECTING");
static_assert(kPageConnecting == 0x9e1863b1u, "connecting page id");
constexpr uint64_t kConnectSteps = 0x2c0;   // netInfo s32: start-up steps done, the bar shows steps / 44
constexpr int32_t kConnectStepCount = 44;
constexpr double kConnectingShown = 1.0;

struct Connecting {
    bool on = false;
    int step = 0;
    double since = 0.0;
};
Connecting connecting;

void set_connect_steps(int32_t steps) { put<int32_t>(g(kNetInfo) + kConnectSteps, steps); }

// The connecting page is on top: take it off (the Multiplayer page or the main menu comes back under it).
void pop_connecting_page() {
    uint8_t *m = menu();
    const int32_t top = get<int32_t>(m + 0x1010);
    uint8_t *page = top >= 0 && top <= 0x40 ? ptr_at(m + 0x9d0 + 8 * uint64_t(top)) : nullptr;
    if (page && get<uint32_t>(page + 0x10) == kPageConnecting) guest<void(U2MP_GUEST *)(void *)>(kPopPage)(m);
}

// The rest of the Multiplayer entry once the network runs (unchanged from the direct entry).
void enter_multiplayer() {
    g(kNetInfo)[0x1c] = 0;
    put<int32_t>(g(kNetInfo) + 0x34, 0);
    put<int16_t>(ngm() + 0x1ca0, -1);
    if (hosting) set_session(kPartySession, kCtx, 1, 1);
    else set_session(kPartySession, kCtx, my_id ? my_id : 0xfff, 1);
    publish(kPartySession);
    sent_mode = -1;
    set_net_sm(18);
    register_handler();
    keep_net_package();
    preload_packages();
    show_page(kPageMultiplayer);
}

// One start-up step per frame while the connecting page is up.
U2MP_GUEST void connecting_handler(uint32_t *event) {
    if (!event || event[0] != 6 || !connecting.on) return;   // 6: per-frame update
    switch (connecting.step) {
    case 0:
        netheap_install();
        netinit_prepare();
        progression_multiplayer_entered();   // the PS3 NetInit leaderboard start-over (progression.md 1.5.19)
        netpatch_load();
        set_connect_steps(kConnectStepCount / 2);
        connecting.step = 1;
        return;
    case 1:
        if (!active) {
            voice::reset_volume();   // PS3 voice init (NetInit) made the speaker port at full volume each time (J6)
            if (!start_transport()) {
                // PS3 0x438bc8 without a network: message box 0x88a082af / OK over the main menu, not in Multiplayer.
                log("Multiplayer could not start: network message, staying in the main menu");
                connecting.on = false;
                pop_connecting_page();
                close_transport();
                online::on_transport_closed("Multiplayer could not start");
                game_message_box(kNoNetworkText, 0);
                return;
            }
            active = true;
        }
        connecting.step = 2;
        return;
    default: {
        const double shown = gamenet::clock() - connecting.since;
        const double part = shown >= kConnectingShown ? 1.0 : shown / kConnectingShown;
        set_connect_steps(kConnectStepCount / 2 + int32_t(part * double(kConnectStepCount / 2)));
        if (shown < kConnectingShown) return;
        connecting.on = false;
        log("connecting page done (%.1f s)", shown);
        pop_connecting_page();
        enter_multiplayer();
        return;
    }
    }
}

U2MP_GUEST void button_multiplayer(void *, void *, void *) {
    log("Multiplayer selected");
    if (connecting.on) return;
    multiplayer_page_seen = false;
    set_connect_steps(0);
    connecting = {true, 0, gamenet::clock()};
    // The handler stays in the table afterwards (inactive); adding it again finds it there.
    if (!add_page_handler(reinterpret_cast<void *>(&connecting_handler))) {
        log("connecting page: no page handler slot, entering Multiplayer directly");
        uint32_t frame[1] = {6};
        connecting.since -= kConnectingShown;   // no page to show: the same steps at once
        while (connecting.on) connecting_handler(frame);
        return;
    }
    show_page(kPageConnecting);
}

// ---- player rows (PS3 0x4bc728 party / lobby, 0x4bfac8 matchmaking) ----------------------------------------
// Query object fields on PS4 (spec parity-b-menus.md 0 and 11.2).
constexpr uint64_t kQValue = 0x04, kQOp10Result = 0x38, kQColour = 0x3c, kQText = 0x40, kQArg = 0x48,
                   kQOp10Selector = 0x4c;

// An op 3 image descriptor: type, value, image id (type 3), colour word (-1 none), scale 1.
void put_image(uint8_t *q, uint32_t type, uint32_t value, uint32_t icon = 0, int32_t colour = -1) {
    put<uint32_t>(q + 8, type);
    put<uint64_t>(q + 0x10, 0);
    put<uint32_t>(q + 0x18, value);
    put<uint32_t>(q + 0x1c, icon);
    put<uint32_t>(q + 0x20, icon ? 1u : 0u);
    put<int32_t>(q + 0x24, -1);
    put<uint32_t>(q + 0x28, 0);
    put<int32_t>(q + 0x2c, colour);
    put<float>(q + 0x30, 1.0f);
}

// Picture 0: the rank badge (type 0x27). Picture 1: the muted icon, else the talking icon while the member talks.
void member_image(uint8_t *q, const Member &m, uint32_t picture) {
    if (picture == 0) {
        put_image(q, 0x27, m.rank);
    } else if (picture == 1) {
        const uint32_t icon = voice::muted(m.net_id) ? kIconMuted : voice::talking(m.net_id) ? kIconTalking : 0;
        if (icon) put_image(q, 3, 0, icon);
    }
}

void put_row_text(uint8_t *q, char (&buffer)[64], const std::string &text) {
    const std::string t = cut(text, 63);
    std::memcpy(buffer, t.c_str(), t.size() + 1);
    put<uint64_t>(q + kQText, addr_of(buffer));
}

// The NpId of a member ("" unknown or LAN).
std::string member_npid(const Member &m) {
    if (m.net_id == my_id) return online::my_npid_text();
    auto it = npids.find(m.net_id);
    return it == npids.end() ? std::string() : it->second;
}

U2MP_GUEST void roster_query(uint8_t *q) {
    const uint32_t op = get<uint32_t>(q);
    const uint32_t arg = get<uint32_t>(q + kQArg);   // 32 bits: +0x4c right after it is op 10's selector
    const int slot = slot_of_row(int(arg % 10));
    const Member *m = slot >= 0 ? &members[slot] : nullptr;
    switch (op) {
    case 7:   // the own row is not enabled (it keeps the cursor, presses do nothing); other members are
        if (m) put<uint32_t>(q + kQValue, m->net_id == my_id ? 0 : 1);
        break;
    case 10: {   // sub index 0, selector 0: "can be selected"; selector 1: connection state (0 = connected)
        if (!m || get<uint32_t>(q + kQValue) != 0) break;
        const uint32_t selector = get<uint32_t>(q + kQOp10Selector);
        if (selector == 0) put<int32_t>(q + kQOp10Result, 1);
        else if (selector == 1 && m->net_id != my_id) put<int32_t>(q + kQOp10Result, 0);
        break;
    }
    case 3:
        if (m) member_image(q, *m, get<uint32_t>(q + kQValue));
        break;
    case 17: {
        if (!m || arg <= 9 || get<int32_t>(g(kNetInfo) + 0x2c4) != 2 || (m->team != 1 && m->team != 2)) break;
        Table games = table_of(lookup(kNetGamesId));
        int16_t game = current_game();
        if (!games.records || game < 0 || game >= games.count) break;
        if (coop_type(get<int32_t>(games.records + game * 0x90 + 0x0c))) break;
        static int colour_ok = -1;   // -1 unchecked, 0 differs, 1 matches
        if (colour_ok < 0) {
            static const unsigned char a[] = {0x89, 0xcb, 0x41, 0x89, 0xd6, 0x49, 0x89, 0xf5, 0x49, 0x89, 0xfc};
            static const unsigned char b[] = {0xc7, 0x05, 0xc7, 0x3f, 0xe8, 0x00, 0x4b, 0x2a, 0x02, 0x6a};
            colour_ok = image.matches(0x64aec1, a, sizeof a) && image.matches(0x64af57, b, sizeof b) ? 1 : 0;
            if (!colour_ok) log("party rows: GetTeamColor 0x64aeb0 differs, no team colours");
        }
        if (!colour_ok) break;
        alignas(16) uint8_t out[0x20] = {};
        guest<void(U2MP_GUEST *)(void *, void *, int32_t, int32_t)>(kTeamColor)(out, ngm(), int32_t(m->team) - 1, 0);
        put<uint32_t>(q + kQColour, (get<uint32_t>(out + 0xc) & 0xffffff) | 0x20000000u);
        break;
    }
    case 1: {
        static const char empty[1] = {0};
        if (m) put_row_text(q, row_text[slot], shown_name(*m));
        else put<uint64_t>(q + kQText, addr_of(empty));
        break;
    }
    default: put<uint32_t>(q + kQValue, 0); break;
    }
}

// The matchmaking slot list (PS3 0x4b0008): our party's members first (present), then the other players of
// the match in slot order (while the gathering runs: "Found Player"), then, while it runs and the playlist
// is known, open slots ("Finding Player...") up to the size of the match, at most 10 entries.
enum : int { kOpenSlot = 0, kFoundPlayer = 1, kPresent = 2 };
struct MmEntry {
    int kind = kOpenSlot;
    int slot = -1;
    uint64_t party = 0;
    std::string npid;
};

std::vector<MmEntry> mm_list() {
    std::vector<MmEntry> list;
    const bool gathering = get<int32_t>(g(kNetInfo) + 0x34) == 1;   // frontend3 phase 1 (PS3 progress 0 / 1)
    int used = 0;
    for (const Member &m : members) used += m.used;
    const bool online_on = online::active();
    for (int pass = 0; pass < 2; ++pass)
        for (int s = 0; s < kSlots && list.size() < 10; ++s) {
            const Member &m = members[s];
            if (!m.used) continue;
            MmEntry e;
            e.slot = s;
            e.npid = member_npid(m);
            // Off line everyone is our party; an unknown NpId counts as ours too.
            const bool own = !online_on || m.net_id == my_id || e.npid.empty() || online::in_own_party(e.npid);
            if (own != (pass == 0)) continue;
            e.kind = own || !gathering ? kPresent : kFoundPlayer;
            e.party = own ? (online_on ? online::own_party_id() : used >= 2 ? 1 : 0) : online::member_party_id(e.npid);
            list.push_back(std::move(e));
        }
    if (gathering && playlist_index(get<uint32_t>(g(kNpd) + 0x104)) >= 0) {
        const int slots = std::min(online::mm_slots(), 10);
        while (int(list.size()) < slots) list.push_back(MmEntry{});
    }
    return list;
}

// Team balance before the vote (PS3 0x4b0a58 state 16, order 0x4af524): the players of the slot list sorted by how
// many listed players share their party id (ascending), then party id, then list position; the last one is kicked
// only when it came alone. -1: nobody (the last one is part of a party, or it is this PC).
int balance_kick_slot() {
    struct Row {
        int share;
        uint64_t party;
        int position;
        int slot;
    };
    const std::vector<MmEntry> list = mm_list();
    std::vector<Row> rows;
    for (size_t i = 0; i < list.size(); ++i) {
        const MmEntry &e = list[i];
        if (e.kind == kOpenSlot || e.slot < 0) continue;
        int share = 0;
        for (const MmEntry &o : list) share += e.party && o.kind != kOpenSlot && o.party == e.party;
        rows.push_back(Row{e.party ? share : 1, e.party, int(i), e.slot});
    }
    if (rows.empty()) return -1;
    std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
        if (a.share != b.share) return a.share < b.share;
        if (a.party != b.party) return a.party < b.party;
        return a.position < b.position;
    });
    const Row &last = rows.back();
    if (last.share != 1 || last.slot == 0 || members[last.slot].net_id == my_id) return -1;
    return last.slot;
}

// The five party group colours (PS3 data, spec 11.3), by the party's position among the shared party ids.
constexpr uint32_t kPartyColours[5] = {0x1020E0E0u, 0x102000E0u, 0x10E000E0u, 0x1020E000u, 0x10E02000u};

int party_position(const std::vector<MmEntry> &list, uint64_t party) {
    if (!party) return -1;
    std::vector<uint64_t> shared;
    for (const MmEntry &e : list) {
        if (!e.party) continue;
        int n = 0;
        for (const MmEntry &o : list) n += o.party == e.party;
        bool known = false;
        for (uint64_t p : shared) known = known || p == e.party;
        if (n >= 2 && !known) shared.push_back(e.party);
    }
    for (size_t i = 0; i < shared.size(); ++i)
        if (shared[i] == party) return int(i);
    return -1;
}

// Connection bars from the round-trip time (PS3 0x4bfac8): <= 49 ms 5, <= 79 4, <= 119 3, <= 199 2, else 1.
uint32_t bars_of(uint32_t rtt_ms) {
    return rtt_ms <= 49 ? 5 : rtt_ms <= 79 ? 4 : rtt_ms <= 119 ? 3 : rtt_ms <= 199 ? 2 : 1;
}

char mm_text[10][64];

U2MP_GUEST void mm_roster_query(uint8_t *q) {
    const uint32_t op = get<uint32_t>(q);
    const uint32_t row = get<uint32_t>(q + kQArg);
    const std::vector<MmEntry> list = mm_list();
    if (row >= list.size()) return;   // past the list: nothing answered
    const MmEntry &e = list[row];
    const Member *m = e.slot >= 0 ? &members[e.slot] : nullptr;
    switch (op) {
    case 1:
        if (e.kind == kPresent && m) put_row_text(q, mm_text[row], shown_name(*m));
        else put_row_text(q, mm_text[row], game_text(e.kind == kOpenSlot ? 0xc0759450u : 0xa91680a8u));
        break;
    case 3: {
        const uint32_t picture = get<uint32_t>(q + kQValue);
        if (picture == 2 && m) {
            uint32_t rtt_ms = 0;
            bool up = false;
            const bool known = m->net_id != my_id && online::peer_rtt(e.npid, rtt_ms, up);
            // PS3 colours the bars by the link state (spec parity-i-connection.md I5); own row, LAN, no record: grey
            const int32_t state = known ? online::peer_link_state(e.npid) : -1;
            put_image(q, 0x1d, known ? bars_of(rtt_ms) : 0, 0, int32_t(peer_rules::bars_colour(state)));
        } else if (e.kind == kPresent && m) {
            member_image(q, *m, picture);
        } else if (e.kind == kOpenSlot && picture == 0) {
            put_image(q, 0x10, 0);   // the searching animation
        }
        break;
    }
    case 10:
        if (get<uint32_t>(q + kQValue) == 0 && get<uint32_t>(q + kQOp10Selector) == 0 && e.kind == kPresent)
            put<int32_t>(q + kQOp10Result, 1);
        break;
    case 17: {
        const int at = e.kind == kPresent ? party_position(list, e.party) : -1;
        if (at >= 0) put<uint32_t>(q + kQColour, kPartyColours[at % 5]);
        break;
    }
    default: break;
    }
}

// ---- Start Match checks (PS3 0x43aab0): each failure shows its message box and nothing starts ----------
// Players in the party, this PC included (PS3 0x4d24a8: 1 with no room).
int party_size() {
    int n = 0;
    for (const Member &m : members) n += m.used;
    return n ? n : 1;
}

bool refuse(const char *why, uint32_t body, uint32_t second) {
    log("Start Match: %s", why);
    game_message_box(body, second);
    return false;
}

// Matchmaking: the party must fit the playlist's maximum party size.
bool matchmaking_start_ok() {
    const int32_t max = playlist_max_party(get<uint32_t>(g(kNpd) + 0x104));
    if (max <= 0 || party_size() <= max) return true;
    return refuse("the party is larger than the playlist allows", kPartyTooBig, kPartyTooBigInfo);
}

// Players a game type takes: 10 for the competitive types, 3 for co-op, 0 for an unknown type.
int type_max_players(int32_t type) {
    if (coop_type(type)) return 3;
    return (type >= 0 && type <= 3) || (type >= 5 && type <= 7) || type == 10 || type == 11 ? 10 : 0;
}

// Custom game: map packs (this PC, then every member), too many, too few, type 10 on one team. Too few: a custom
// game starts with 1 player in every game mode (owner rule 2026-11-25, start_rules.h; the minimum of 2 is for
// matchmaking only; PS3 asked 2 for type 10 and co-op). Type 10 on one team: only with 2 or more players.
bool custom_start_ok() {
    Table games = table_of(lookup(kNetGamesId));
    const int16_t game = current_game();
    if (!games.records || game < 0 || game >= games.count) return true;   // no game chosen: nothing to check
    const int32_t type = get<int32_t>(games.records + game * 0x90 + 0x0c);
    const int players = party_size();
    bool local_ok = true;
    const int32_t pack = dlc_net_game_pack(game, local_ok);
    if (!local_ok)
        return refuse("this PC lacks the map's map pack", pack == 1 ? kNoMapPack1 : kNoMapPack2, kCustomInfo);
    // Only when this PC plays the map pack version (Ruins falls back to the base map without map pack 1).
    if ((pack == 1 || pack == 2) && (dlc_packs() & (1u << pack)))
        for (const Member &m : members)
            if (m.used && m.net_id != my_id && !(m.packs & (1u << pack)))
                return refuse("a party member lacks the map's map pack", kMemberNoMapPack, kCustomInfo);
    if (players > type_max_players(type)) return refuse("too many players for the game type", kTooManyPlayers, kCustomInfo);
    if (!start::may_start(start::kCustom, type, players))
        return refuse("too few players for the game type", kTooFewPlayers, kTooFewInfo);
    int with_data = 0, team1 = 0, team2 = 0;
    for (const Member &m : members) {
        if (!m.used) continue;
        ++with_data;
        team1 += m.team == 1;
        team2 += m.team == 2;
    }
    if (start::one_team_refused(type, players, with_data, team1, team2))
        return refuse("every player is on one team", kOneTeam, kOneTeamInfo);
    if (players == 1) log("Start Match: custom game type %d with 1 player (solo)", int(type));
    return true;
}

U2MP_GUEST void button_start_match(void *, void *, void *) {
    if (!hosting) return;
    const int32_t mode = get<int32_t>(g(kNetInfo) + 0x2c4);
    if (mode == 1) {   // matchmaking
        if (!matchmaking_start_ok()) return;
        online::hide_for_start(true);
        if (matchmaking::installed()) {
            if (!frontend6::leader_starting(true)) matchmaking::leader_start();
            return;
        }
        pick_matchmaking_game();
        create_match_now();
    } else if (mode == 2) {   // custom game
        if (!custom_start_ok()) return;
        online::hide_for_start(true);
        if (frontend6::leader_starting(false)) return;   // the "Starting in" popup creates the match when it runs out
        create_match_now();
    } else if (mode == 3) {   // cinema: play the selected recording (PS3: as Replay 0x43a3dc)
        log("Cinema");
        cinema::start_playback();
    } else {
        log("Start Match: page mode %d: nothing to start", int(mode));
    }
}

U2MP_GUEST void leave_multiplayer(void *, void *, void *) { teardown(true); }

}  // namespace
void sync_netstat_ranks() {
    constexpr uint64_t kMatchPlayers = 0x1147bd8;
    constexpr uint64_t kMatchRecords = 0x114b4bc;
    constexpr uint64_t kRecordStride = 0x80;
    constexpr uint64_t kTrackers = 0x113b770;
    constexpr uint64_t kTrackerStride = 0x2c0;
    uint32_t count = get<uint32_t>(g(kMatchPlayers));
    if (count > 10) count = 10;
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t *record = g(kMatchRecords + uint64_t(i) * kRecordStride);
        const uint32_t id = get<uint32_t>(record);
        if (!id) continue;
        for (int t = 0; t < 10; ++t) {
            const uint8_t *tracker = g(kTrackers + uint64_t(t) * kTrackerStride);
            if (get<uint32_t>(tracker + 0x120) != id) continue;
            const uint32_t rank = get<uint32_t>(tracker + 0x130);
            if (rank && rank <= 0xff && record[0xb] != uint8_t(rank)) record[0xb] = uint8_t(rank);
            break;
        }
    }
}
// ---- public interface -----------------------------------------------------------------------------------
const uint8_t *package_by_id(uint32_t id) { return find_package(id); }

namespace session {

uint8_t *request_package(const char *name) { return request_package_internal(name); }

bool member_at(int row, uint32_t &net_id, std::string &name) {
    int slot = slot_of_row(row);
    if (slot < 0) return false;
    net_id = members[slot].net_id;
    name = members[slot].name;
    return true;
}

bool mm_member_at(int row, uint32_t &net_id, std::string &name) {
    if (row < 0) return false;
    const std::vector<MmEntry> list = mm_list();
    if (size_t(row) >= list.size() || list[size_t(row)].kind != kPresent || list[size_t(row)].slot < 0) return false;
    const Member &m = members[list[size_t(row)].slot];
    net_id = m.net_id;
    name = m.name;
    return true;
}

bool member_name(uint32_t net_id, std::string &name) {
    for (const Member &m : members)
        if (m.used && m.net_id == net_id) {
            name = m.name;
            return true;
        }
    return false;
}

bool member_skins(uint32_t net_id, std::string &hero, std::string &villain) {
    for (const Member &m : members)
        if (m.used && m.net_id == net_id) {
            hero = m.hero;
            villain = m.villain;
            return true;
        }
    return false;
}

uint32_t member_rank(uint32_t net_id) {
    for (const Member &m : members)
        if (m.used && m.net_id == net_id) return m.rank;
    return 0;
}

uint32_t local_id() { return active ? my_id : 0; }

bool is_host() { return active && hosting; }

int member_count() {
    if (!active) return 0;
    int n = 0;
    for (const Member &m : members) n += m.used;
    return n;
}

void note_leaving() {
    if (hosting) return;
    leaving_seen = true;
    if (leaving_since == 0.0) leaving_since = gamenet::clock();
}

std::string local_name() {
    const char *env = std::getenv("UNCHARTED_U2MP_NAME");
    if (env && *env) return cut(env, 31);
    return my_name;
}

int rank() { return local_rank(); }

void send_message(int slot, const std::vector<uint8_t> &body, int except) {
    if (active && !body.empty()) send_raw(slot, body, except);
}

uint32_t net_ticks() {
    if (uint8_t *object = ptr_at(g(kNetClockPtr))) return get<uint32_t>(object + 0xc);
    return own_ticks();
}

void create_match() {
    if (active && hosting) create_match_now();
}

// Kick From Party (PS3 0x439cf8) and the griefer kick both kick with the block flag: the kicked player's online
// id is refused on later joins until this PC stops hosting.
bool kick(uint32_t net_id) {
    if (!active || !hosting || net_id < 2 || net_id > 11 || !members[net_id - 1].used) return false;
    std::string npid;
    net::Address a;
    if (transport.peer_address(uint8_t(net_id - 1), a)) npid = online::npid_at(a);
    log("party: kicking %s (net id %u)", members[net_id - 1].name.c_str(), net_id);
    send_raw(int(net_id - 1), {kKick}, -1);
    if (npid.empty()) {
        log("party: net id %u has no online id: kicked, not blocked", net_id);
    } else {
        blocked.emplace(npid, 0u);
        log("party: %s blocked from joining again until this session ends", npid.c_str());
    }
    return true;
}

bool kick_griefer(uint32_t net_id) {
    if (!active || !hosting) return false;
    if (net_id == my_id) {
        // PS3 treats kicking its own member as leaving the room: the host leaves, as a kicked player does.
        log("party: this PC (the host) was named as the griefer: leaving the session");
        g(kNetInfo)[0x1c] = 1;
        kicked = true;   // the transport closes on the next frame, outside the event Apply
        return true;
    }
    return kick(net_id);
}

bool member_address(uint32_t net_id, net::Address &out) {
    if (!active || !hosting || net_id < 2 || net_id > 11) return false;
    return transport.peer_address(uint8_t(net_id - 1), out);
}

uint32_t net_id_of(const std::string &npid) {
    for (const auto &entry : npids)
        if (entry.second == npid) return entry.first;
    return 0;
}

void set_local_name(const std::string &name) {
    if (name.empty()) return;
    std::string n = cut(name, 31);
    if (n == my_name && local_name() == n) return;
    my_name = n;
    _putenv_s("UNCHARTED_U2MP_NAME", n.c_str());
    log("player name: %s (RPCN)", n.c_str());
    if (!active) return;
    if (hosting && members[0].used) {
        members[0].name = n;
        refresh_sessions();
        broadcast_roster();
    } else if (!hosting && my_id != 0) {
        send_info();
    }
}

void online_reopen(net::Link *link, const net::Address *owner) {
    for (int s = 0; s < kSlots; ++s) gamenet::detach(uint8_t(s));
    close_transport();
    clear_members();
    npids.clear();   // the next roster (host) or kIds (client) fills it for the new transport
    kicked = host_lost = false;
    host_id = 1;
    rehost_pending = false;
    if (!owner) {
        hosting = transport.host_link(link);
        my_id = 1;
        members[0] = Member{true, 1, my_name, "", "", uint8_t(local_rank()), 0};
        set_session(kPartySession, kCtx, 1, 1);
        publish(kPartySession);
        log("online: transport switch: hosting a new party on the online link");
    } else {
        hosting = false;
        my_id = 0;
        set_session(kPartySession, kCtx, 0xfff, 1);
        publish(kPartySession);
        transport.join_link(link, *owner);
        log("online: transport switch: joining the room owner over the online link");
    }
    sent_mode = -1;
    active = true;
}

void online_match_member_joined() {
    if (hosting || net_sm() != 17) return;
    set_net_sm(12);
    const uint8_t joined = 1;
    mailbox_post(0x10e, 1, &joined, 1);
    log("party: match room joined; match start re-posted");
}

void online_party_in(bool ok) {
    if (!active) return;
    const int32_t s = net_sm();
    // JOINING waits (a party member following, or a join from the list while this PC still hosts its own party):
    // 0x10e {1} goes on to 14 -> 17, {0} pops the page and sets NET_SM 18
    if (s == 12) {
        const uint8_t v = ok ? 1 : 0;
        mailbox_post(0x10e, 1, &v, 1);
        log("party: JOINING gets 0x10e {%u}", unsigned(v));
    }
    if (hosting) return;
    if (!ok && s == 17) {   // already in the client lobby: back to the Multiplayer page with the party
        log("party: the leader went back from the custom game lobby: back to PAGE_MULTIPLAYER, NET_SM 18");
        guest<void(U2MP_GUEST *)(void *)>(kMenuReset)(menu());
        show_page(kPageMultiplayer);
        set_net_sm(18);
    }
    if (!ok) clear_session(kMatchSession);
}

void online_match_abort(const char *why) {
    if (!active || !hosting) return;
    log("party: the custom game is dropped (%s): back to PAGE_MULTIPLAYER, NET_SM 18", why ? why : "?");
    g(kMatchSession)[0x99] = 0;
    clear_session(kMatchSession);
    g(kNetInfo)[0x08] = 0;
    guest<void(U2MP_GUEST *)(void *)>(kMenuReset)(menu());
    show_page(kPageMultiplayer);
    set_net_sm(18);
}

std::vector<std::string> member_npids() {
    std::vector<std::string> out;
    if (!active || !hosting) return out;
    for (int s = 1; s < kSlots; ++s) {
        net::Address a;
        if (members[s].used && transport.peer_address(uint8_t(s), a)) {
            const std::string npid = online::npid_at(a);
            if (!npid.empty()) out.push_back(npid);
        }
    }
    return out;
}

bool party_stranger(uint32_t net_id) {
    if (!active || !online::active() || net_id == my_id) return false;
    const auto it = npids.find(net_id);
    return it != npids.end() && !it->second.empty() && !online::in_own_party(it->second);
}

bool party_member(uint32_t net_id) {
    if (!active || !online::active() || net_id == my_id) return true;
    const auto it = npids.find(net_id);
    return it != npids.end() && online::in_own_party(it->second);
}

int mm_balance_slot() { return active && hosting ? balance_kick_slot() : -1; }

void leave_party() {
    if (!active || hosting) return;
    if (online::drop_out_of_party("left matchmaking")) return;   // online: rooms left, online_reopen hosts our party
    log("party: leaving the party (Leave Matchmaking): a party of our own");
    for (int s = 0; s < kSlots; ++s) gamenet::detach(uint8_t(s));
    close_transport();
    clear_members();
    kicked = host_lost = false;
    uint16_t port = kDefaultPort;
    if (const char *p = std::getenv("UNCHARTED_U2MP_PORT")) {
        const int v = std::atoi(p);
        if (v > 0) port = uint16_t(v);
    }
    if (!transport.host(port) && !transport.host(0)) {
        log("party: no UDP port for a party of our own: Multiplayer starts a new one");
        active = false;
        return;
    }
    hosting = true;
    my_id = host_id = 1;
    rehost_pending = false;
    members[0] = Member{true, 1, my_name, "", "", uint8_t(local_rank()), uint8_t(local_team())};
    set_session(kPartySession, kCtx, 1, 1);
    publish(kPartySession);
    sent_mode = -1;
    log("party: hosting a party of our own on UDP port %u", unsigned(transport.bound_port()));
}

void online_match_ready(uint64_t room_id) {
    if (!active || !hosting) {
        log("online: match room %llx ready but this PC no longer hosts: ignored", (unsigned long long)room_id);
        return;
    }
    log("online: Start Match goes on (match room %llx)", (unsigned long long)room_id);
    g(kMatchSession)[0x99] = 1;
    send_raw(-1, follow_body(), -1);
}

void online_migrated(bool host, bool mm) {
    log("party: new leader (%s)%s", host ? "this PC" : "another player", mm ? ", matchmaking goes on" : "");
    if (mm) {
        matchmaking::on_migrated(host);
        return;
    }
    g(kMatchSession)[0x99] = 0;
    clear_session(kMatchSession);
}

void online_host_gone() {
    if (!active || hosting) return;
    log("party: lost the host (nobody took over); back to the Multiplayer lobby");
    g(kNetInfo)[0x1c] = 1;
    host_lost = true;
    set_net_sm(3);
    show_page(kPageMultiplayer);
}

void online_network_error(const char *why) {
    if (!active) return;
    log("party: network error (%s): leaving online play", why ? why : "?");
    g(kNetInfo)[0x1c] = 1;
    host_lost = true;   // the transport closes on the next frame
}

void online_signed_out() {
    if (!active) return;
    log("party: signed out of RPCN in the launcher: leaving online play");
    g(kNetInfo)[0x1c] = 1;
    host_lost = true;
}

int custom_room_slots() {
    Table games = table_of(lookup(kNetGamesId));
    const int16_t game = current_game();
    if (!games.records || game < 0 || game >= games.count) return 0;
    return type_max_players(get<int32_t>(games.records + game * 0x90 + 0x0c));
}

int playlist_room_slots() {
    std::vector<int32_t> types;
    int most = 0;
    if (playlist_game_types(types))
        for (const int32_t t : types) most = std::max(most, type_max_players(t));
    return most > 0 ? most : 10;
}

uint32_t mm_pack_mask() {
    std::vector<int32_t> types;
    Table games = table_of(lookup(kNetGamesId));
    if (!playlist_game_types(types) || !games.records) return 0;
    bool pack_maps = false;   // PS3 0x49b858: the playlist uses map-pack content
    for (int32_t i = 0; i < games.count && !pack_maps; ++i) {
        const int32_t type = get<int32_t>(games.records + i * 0x90 + 0x0c);
        if (std::find(types.begin(), types.end(), type) == types.end()) continue;
        bool local_ok = true;
        const int32_t pack = dlc_net_game_pack(i, local_ok);
        pack_maps = pack == 1 || pack == 2;
    }
    if (!pack_maps) return 0;
    uint32_t mask = dlc_packs() & 0xffu;
    for (const Member &m : members)
        if (m.used && m.net_id != my_id) mask &= m.packs;   // 0xff: the member sent none
    return mask;
}

bool match_migration_ok() {
    if (!active || hosting || !online::active()) return false;
    const char *env = std::getenv("UNCHARTED_U2MP_HOST_MIGRATION");
    if (env && env[0] == '0') return false;
    if (net_sm() != 26) return false;
    Table games = table_of(lookup(kNetGamesId));
    int16_t game = current_game();
    if (games.records && game >= 0 && game < games.count && coop_type(get<int32_t>(games.records + game * 0x90 + 0x0c)))
        return false;
    return true;
}

void online_rehost(net::Link *link, const net::Address *owner, uint32_t new_host_net_id) {
    uint32_t old = host_id;
    log("match migration: host net id %u left; net id %u hosts now%s", old, new_host_net_id,
        owner ? "" : " (this PC)");
    for (int s = 0; s < kSlots; ++s) gamenet::detach(uint8_t(s));
    close_transport();
    clear_members();
    kicked = host_lost = false;
    host_id = new_host_net_id;
    if (!owner) {
        hosting = transport.host_link(link);
        transport.reserve_slot(uint8_t(my_id - 1));   // our old slot keeps our net id for us
        uint8_t r = uint8_t(local_rank());
        uint8_t t = uint8_t(local_team());
        members[0] = Member{true, my_id, my_name, "", "", r, t};
    } else {
        hosting = false;
        transport.join_link(link, *owner, uint8_t(my_id - 1));
    }
    set_session(kPartySession, kCtx, my_id, new_host_net_id);
    set_session(kMatchSession, kCtx, my_id, new_host_net_id);
    player_left(old);
    active = true;
    sent_mode = -1;
    if (!owner) {
        publish(kMatchSession);
        guest<void(U2MP_GUEST *)(void *)>(kOwnerChanged)(nullptr);
    } else {
        rehost_pending = true;   // finished by the joined event of the new host
    }
}

// ---- install ------------------------------------------------------------------------------------------
bool install_links_kept() {
    static const unsigned char orig[6] = {0x0f, 0x8c, 0x13, 0x01, 0x00, 0x00};   // jl 0x5f1906
    static const unsigned char jump[6] = {0x90, 0xe9, 0x13, 0x01, 0x00, 0x00};   // nop; jmp 0x5f1906
    if (image.matches(0x5f17ed, jump, sizeof jump)) return true;
    if (!image.matches(0x5f17ed, orig, sizeof orig)) return false;
    std::memcpy(image.base + 0x5f17ed, jump, sizeof jump);
    log("lost links: Playing's PSN record check jumped (0x5f17ed)");
    return true;
}

bool install_actor_requests() {
    static const unsigned char orig[7] = {0x48, 0x8b, 0x05, 0xd9, 0x90, 0xa0, 0x00};
    if (!image.executable(0x641b20, sizeof orig)) return false;
    if (!image.matches(0x641b20, orig, sizeof orig)) return image.base[0x641b20] == 0xe9;
    unsigned char *stub = near_stub(32);
    if (!stub) return false;
    size_t n = 0;
    stub[n++] = 0x48; stub[n++] = 0xb8;
    const uint64_t fn = uint64_t(uintptr_t(&actor_request_hook));
    std::memcpy(stub + n, &fn, 8); n += 8;
    stub[n++] = 0xff; stub[n++] = 0xd0;
    stub[n++] = 0xff; stub[n++] = 0x25; stub[n++] = 0; stub[n++] = 0; stub[n++] = 0; stub[n++] = 0;
    const uint64_t back = uint64_t(uintptr_t(image.base + 0x641b27));
    std::memcpy(stub + n, &back, 8); n += 8;
    const int64_t delta = int64_t(uintptr_t(stub)) - int64_t(uintptr_t(image.base + 0x641b25));
    if (delta < INT32_MIN || delta > INT32_MAX) return false;
    unsigned char patch[7] = {0xe9, 0, 0, 0, 0, 0x90, 0x90};
    const int32_t relative = int32_t(delta);
    std::memcpy(patch + 1, &relative, 4);
    std::memcpy(image.base + 0x641b20, patch, sizeof patch);
    log("skins: actor requests hooked after StartNetGame's package requests (0x641b20)");
    return true;
}

void install(int &installed) {
    if (!gamenet::install()) return;
    voice::install(installed);   // talking indicator table (N6), mouth patch (N5)
    if (install_links_kept()) ++installed;
    if (install_actor_requests()) ++installed;
    if (retarget_call(0x664d6a, 0x60ded0, reinterpret_cast<void *>(&state_send_trace), "tracker state send"))
        ++installed;
    if (retarget_call(0x60df2f, 0xb332a0, reinterpret_cast<void *>(&state_alloc_trace), "tracker state alloc"))
        ++installed;
    if (retarget_call(0x60e291, 0x634970, reinterpret_cast<void *>(&state_helper_trace), "tracker state helper"))
        ++installed;

    struct Replacement {
        uint64_t offset;
        const char *name;
        void *host;
    };
    const Replacement replacements[] = {
        {0x5eb160, "BUTTON_MULTIPLAYER", reinterpret_cast<void *>(&button_multiplayer)},
        {0x659bb0, "party roster query", reinterpret_cast<void *>(&roster_query)},
        {0x659bc0, "player roster query", reinterpret_cast<void *>(&mm_roster_query)},
        {0x5eb780, "BUTTON_MULTIPLAYER_STARTMATCH", reinterpret_cast<void *>(&button_start_match)},
        {0x5eb180, "LEAVE_MULTIPLAYER", reinterpret_cast<void *>(&leave_multiplayer)},
    };
    static const unsigned char lea_rdi[] = {0x48, 0x8d, 0x3d};
    for (const Replacement &r : replacements) {
        bool expected = r.offset == 0x5eb180 ? image.matches(r.offset, lea_rdi, sizeof lea_rdi) : is_ret_stub(r.offset);
        if (!expected) {
            log("%s at 0x%llx: not the stripped function this port expects, left alone", r.name,
                (unsigned long long)r.offset);
            continue;
        }
        if (replace_function(r.offset, 5, r.host, r.name)) ++installed;
    }

    for (const StateSlot &s : kStates)
        if (get<uint64_t>(g(s.slot)) != addr_of(g(s.original))) {
            log("match state vtables differ: no per-frame network during matches");
            return;
        }
    for (size_t i = 0; i < 10; ++i) {
        originals[i] = reinterpret_cast<UpdateFn>(g(kStates[i].original));
        put<uint64_t>(g(kStates[i].slot), addr_of(reinterpret_cast<void *>(kWrappers[i])));
    }
    log("match state updates wrapped (%zu states)", size_t(10));
    ++installed;

    if (get<uint64_t>(g(kAbortSlot)) == addr_of(g(kAbortOriginal)))
        put<uint64_t>(g(kAbortSlot), addr_of(reinterpret_cast<void *>(&abort_wrapper)));
    // The idle-kick limit (f32 0xe622a8, read by Playing's update) stays the game's 120 s, as PS3 1.10.
}

}  // namespace session
}  // namespace u2mp
