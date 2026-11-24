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

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "frontend3.h"
#include "frontend6.h"
#include "gamenet.h"
#include "net.h"
#include "online.h"
#include "progression.h"
#include "roster_wire.h"
#include "session_api.h"
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
constexpr uint64_t kIdleLimit = 0xe622a8;
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
constexpr uint32_t kNetPackage = 0x4240ef2e;
constexpr uint32_t kNetU2mpPackage = 0x5e516de8;
constexpr uint32_t kIconTalking = 0xffa98ec5;
constexpr uint32_t kIconMuted = 0x7a5f5bf1;
constexpr uint16_t kDefaultPort = 27210;
constexpr uint64_t kCtx = 0x55324d50000001ull;   // any non-zero session context; this one is ours

static_assert(sid("PAGE_MULTIPLAYER_JOINING") == kPageJoining, "page id");
static_assert(sid("net") == kNetPackage && sid("net-u2mp") == kNetU2mpPackage, "package ids");

// Session message types (the byte after the layer tag).
enum : uint8_t { kInfo = 1, kRoster = 2, kMode = 3, kFollow = 4, kKick = 5, kTeams = 6, kIds = 8, kClock = 9,
                  kLobby = 10 };

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
double next_rate_log = 0;
double last_clock = -1;
bool rehost_pending = false;
bool client_leave_requested = false;
bool lobby_return = false;
bool lobby_returning = false;
void *online_state = nullptr;
double lobby_grace = 0.0;
std::map<uint32_t, std::string> npids;   // net id -> NpId (ordered, so the smallest id is found first)
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

// The row-th used member in slot order, or -1.
int slot_of_row(int row) {
    if (row < 0) return -1;
    for (int s = 0; s < kSlots; ++s)
        if (members[s].used && row-- == 0) return s;
    return -1;
}

std::string cut(const std::string &s, size_t n) { return s.size() > n ? s.substr(0, n) : s; }

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

void preload_packages() {
    const char *env = std::getenv("UNCHARTED_U2MP_PRELOAD");
    if (env && std::strcmp(env, "0") == 0) return;
    // The weapons and animation packages the PS3 game loaded on entering Multiplayer.
    static const char *const names[] = {
        "beretta-pistol", "beretta-pistol-magazine", "colt-defender-pistol", "colt-defender-magazine",
        "desert-eagle", "desert-eagle-magazine", "dragunov-magazine", "dragunov-rifle", "fn-fal-clip",
        "fn-fal-rifle", "m4-rifle", "m4-rifle-magazine", "shotgun", "spas12-shotgun", "machine-gun",
        "machine-gun-clip", "rpg", "anim-proto-multiplayer", "anim-proto-shield", "anim-proto-carry-objects",
        "m32-grenade-launcher"};
    int requested = 0, present = 0, missing = 0;
    for (const char *name : names) {
        if (find_package(sid(name))) ++present;
        else if (request_package_internal(name)) ++requested;
        else ++missing;
    }
    log("package preload (PS3 0xe155d0): %d requested, %d already present, %d not created", requested, present,
        missing);
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
        if (at + 1 + n <= size) m.villain.assign(reinterpret_cast<const char *>(d + at + 1), n);
    }
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

void on_follow(const uint8_t *d, size_t size) {
    if (hosting) return;
    log("following the host into the match");
    if (size >= 9) online::follow_match_room(get<uint64_t>(d + 1));
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
    show_page(kPageJoining);
    set_net_sm(12);
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
            }
        }
        break;
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
        log("party: lost the host; the room's new owner takes over");
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

void register_handler() {
    uint32_t count = get<uint32_t>(menu() + 0x24f0);
    uint64_t fn = addr_of(reinterpret_cast<void *>(&page_handler));
    for (uint32_t i = 0; i < count; ++i)
        if (get<uint64_t>(handler_entry(i) + 8) == fn) return;
    if (count >= 32) {
        log("menu page handler table full: no per-frame updates in menus");
        return;
    }
    uint8_t *e = handler_entry(count);
    put<uint32_t>(e, 0);   // page 0: called for every page
    put<uint64_t>(e + 8, fn);
    put<uint64_t>(e + 0x10, 0);
    put<uint32_t>(menu() + 0x24f0, count + 1);
    log("per-frame page handler registered (%u handlers)", count + 1);
}

void unregister_handler() {
    uint32_t count = get<uint32_t>(menu() + 0x24f0);
    uint64_t fn = addr_of(reinterpret_cast<void *>(&page_handler));
    for (uint32_t i = 0; i < count; ++i) {
        if (get<uint64_t>(handler_entry(i) + 8) != fn) continue;
        for (uint32_t j = i + 1; j < count; ++j) std::memcpy(handler_entry(j - 1), handler_entry(j), 0x18);
        put<uint32_t>(menu() + 0x24f0, count - 1);
        return;
    }
}

// ---- teardown -----------------------------------------------------------------------------------------
void teardown(bool pop) {
    log("leaving multiplayer");
    unregister_handler();
    for (int s = 0; s < kSlots; ++s) gamenet::detach(uint8_t(s));
    voice::stop();
    transport.close();
    online::on_transport_closed("Multiplayer left");
    active = false;
    clear_session(kPartySession);
    clear_session(kMatchSession);
    put<int32_t>(g(kNetInfo) + 0x2c4, 0);
    put<int16_t>(ngm() + 0x1ca0, -1);
    guest<void(U2MP_GUEST *)(void *)>(kClearNetPlayers)(ngm());
    progression_publish();
    set_net_sm(0);
    clear_members();
    if (pop) guest<void(U2MP_GUEST *)(void *)>(kPopPage)(menu());
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
            teardown(false);
            set_net_sm(3);
            show_page(kPageMultiplayer);
        }
    }
    if (last_state == kPlayingState && frames % 60 == 0) tier2_patch_weapon_damage();
    hud_weapon_tick();
    voice::frame();
    transport.pump(now);

    if (kicked || host_lost) {
        const char *why = kicked ? "kicked" : "host lost";
        log("party: closing the transport (%s); Multiplayer starts a new one", why);
        kicked = host_lost = false;
        for (int s = 0; s < kSlots; ++s) gamenet::detach(uint8_t(s));
        voice::stop();
        transport.close();
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
            team = hosting ? local_team() : int32_t(members[host_id].team);
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
        if (hosting && members[0].used && members[0].rank != r) {
            members[0].rank = uint8_t(r);
            broadcast_roster();
        } else if (!hosting && my_id != 0 && (r != sent_rank || t != sent_team)) {
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

U2MP_GUEST void page_handler(uint32_t *event) {
    if (!event || event[0] != 6) return;   // 6: per-frame update
    finish_leaving_game();
    leaving_step();
    cleanup_leaving_page();
    const bool page_open = multiplayer_page_open();
    if (page_open) multiplayer_page_seen = true;
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

// WaitReady only leaves once every player slot has signalled in, so show what each slot reports.
void wait_ready_diagnostics() {
    static int64_t next = 0;
    const int64_t ms = steady_ms();
    if (ms < next) return;
    next = ms + 2000;
    uint8_t *base = ngm();
    const uint64_t end = get<uint64_t>(base) - 0x10;
    log("wait ready: all ready %u, deadline %d, timer %d, end 0x%llx", unsigned(base[0x151]),
        get<int32_t>(base + 0x154), get<int32_t>(g(0x113a040) + 0xc), (unsigned long long)end);
    for (int i = 0; i < 10; ++i) {
        uint8_t *slot = base + 0x2c0 * i;
        if (reinterpret_cast<uint64_t>(slot) == end) break;
        log("wait ready: slot %d id %d in game %u, %02x %02x, state %d", i, get<int32_t>(slot + 8),
            unsigned(slot[0x30]), unsigned(slot[0x27c]), unsigned(slot[0x284]), get<int32_t>(slot + 0x2c));
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
    if (index != held_index) show_round_results();
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
        if (index == kWaitReady) wait_ready_diagnostics();
        if (index == kLeave) {
            leave_reason("leave");
            if (active) {
                if (hosting) {
                    send_raw(-1, {kLobby}, -1);
                    log("party: host left the match; telling the clients to return to the lobby");
                } else if (!lobby_returning && gamenet::clock() > lobby_grace) {
                    client_leave_requested = true;
                }
            }
        }
    }
    if (active) reset_watch(index, state, ms);
    if (index == kLoadLevels) missing_level_package();
    if (index == kEstablish) {
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
    if (online::create_match_room()) return;   // online_match_ready() finishes it
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

U2MP_GUEST void button_multiplayer(void *, void *, void *) {
    log("Multiplayer selected");
    multiplayer_page_seen = false;
    netheap_install();
    netinit_prepare();
    netpatch_load();
    if (!active) {
        if (!start_transport()) {
            g(kNetInfo)[0x1c] = 1;
            show_page(kPageMultiplayer);
            return;
        }
        active = true;
    }
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

U2MP_GUEST void roster_query(uint8_t *q) {
    uint32_t op = get<uint32_t>(q);
    uint64_t arg = get<uint64_t>(q + 0x48);
    int slot = slot_of_row(int(arg % 10));
    const Member *m = slot >= 0 ? &members[slot] : nullptr;
    switch (op) {
    case 7: put<uint32_t>(q + 4, m ? 1 : 0); break;
    case 3: {
        uint32_t value = get<uint32_t>(q + 4);
        if (m && value == 1) {
            uint32_t icon = 0;
            if (voice::talking(m->net_id)) icon = kIconTalking;
            else if (voice::muted(m->net_id)) icon = kIconMuted;
            if (!icon) break;
            put<uint32_t>(q + 8, 3);
            put<uint64_t>(q + 0x10, 0);
            put<uint32_t>(q + 0x18, 0);
            put<uint32_t>(q + 0x1c, icon);
            put<uint32_t>(q + 0x20, 1);
            put<int32_t>(q + 0x24, -1);
            put<uint32_t>(q + 0x28, 0);
            put<int32_t>(q + 0x2c, -1);
            put<float>(q + 0x30, 1.0f);
        } else if (m && value == 0) {
            put<uint32_t>(q + 8, 0x27);   // rank badge
            put<uint64_t>(q + 0x10, 0);
            put<uint32_t>(q + 0x18, m->rank);
            put<uint64_t>(q + 0x1c, 0);
            put<int32_t>(q + 0x24, -1);
            put<uint32_t>(q + 0x28, 0);
            put<int32_t>(q + 0x2c, -1);
            put<float>(q + 0x30, 1.0f);
        }
        break;
    }
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
        put<uint32_t>(q + 0x3c, (get<uint32_t>(out + 0xc) & 0xffffff) | 0x20000000u);
        break;
    }
    case 1: {
        static const char empty[1] = {0};
        char *buffer = row_text[m ? slot : 0];
        std::string name = m ? cut(m->name, 63) : std::string();
        std::memcpy(buffer, name.c_str(), name.size() + 1);
        put<uint64_t>(q + 0x40, m ? addr_of(buffer) : addr_of(empty));
        break;
    }
    default: put<uint32_t>(q + 4, 0); break;
    }
}

U2MP_GUEST void button_start_match(void *, void *, void *) {
    if (!hosting) return;
    int32_t mode = get<int32_t>(g(kNetInfo) + 0x2c4);
    if (mode == 1 && matchmaking::installed()) {
        if (!frontend6::leader_starting(true)) matchmaking::leader_start();
        return;
    }
    if (mode == 1) {
        pick_matchmaking_game();
    } else if (frontend6::leader_starting(false)) {
        return;   // the "Starting in" popup creates the match when it runs out
    }
    create_match_now();
}

U2MP_GUEST void leave_multiplayer(void *, void *, void *) { teardown(true); }

}  // namespace

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

bool kick(uint32_t net_id) {
    if (!active || !hosting || net_id < 2 || net_id > 11 || !members[net_id - 1].used) return false;
    log("party: kicking %s (net id %u)", members[net_id - 1].name.c_str(), net_id);
    send_raw(int(net_id - 1), {kKick}, -1);
    return true;
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
    transport.close();
    clear_members();
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

void online_signed_out() {
    if (!active) return;
    log("party: signed out of RPCN in the launcher: leaving online play");
    g(kNetInfo)[0x1c] = 1;
    host_lost = true;
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
    transport.close();
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
        {0x659bc0, "player roster query", reinterpret_cast<void *>(&roster_query)},
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

    static const unsigned char idle_120[] = {0x00, 0x00, 0xf0, 0x42};
    if (image.matches(kIdleLimit, idle_120, sizeof idle_120)) {
        put<float>(g(kIdleLimit), 300.0f);
        log("idle kick: limit 120 s -> 300 s");
        ++installed;
    } else {
        log("idle kick limit at 0x%llx differs: left alone", (unsigned long long)kIdleLimit);
    }
}

}  // namespace session
}  // namespace u2mp
