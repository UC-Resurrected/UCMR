// u2mp online: Uncharted 2 online play (rooms + peer-to-peer), the PC counterpart of what the PS3 GOTY
// build (NPUA80698 1.10) did with NP Matching2, NP Signaling and NP Basic (u2mp/docs/PORT-MATCHMAKING.md;
// this port: u2mp/docs/PORT-ONLINE.md).
//
// The room service is u2_online.exe, a separate program (it ships separately) that this runtime starts and
// talks to over pipes: online_link.cpp, with the calls of u2_online_abi.h. The launcher signs the player in
// and hands the session to the game through UNCHARTED_U2MP_RELAY; the program's path is
// UNCHARTED_U2MP_ONLINE_EXE. Without either, every function here is a no-op and the session layer plays over
// LAN as before.
//
// Model (the LAN session layer, session.cpp, stays the core): the transport (net::Transport, a star with
// the party leader / match host in the middle) runs over the program's peer-to-peer socket instead of a UDP
// socket of its own (net::Link below). The room owner hosts the transport, the other members are its
// clients. So everything that already works over LAN (roster, follow, matchmaking vote, game events,
// voice) works unchanged online; this file only decides who hosts and connects the players:
//   - Multiplayer entered: link up (u2o_open, U2O_READY, u2o_find_world), then this PC's party room
//     (PS3 0x4c48d0: 10 slots, OWNER_AUTO_GRANT) - searchable here, PS3 hid it and used invites.
//   - Start Match, custom game (PS3 0x444bb0 -> CreateJoinRoom 0x4d421c): the match room with the PS3
//     attribute set (1.3) and the game type's slot count (10, co-op 3), then match+0x99
//     (session::online_match_ready); a room that cannot be made ends Start Match as on PS3 (NET_SM 18). Party
//     members join it too.
//   - Invites button -> join list (PS3 BUTTON_MULTIPLAYER_JOIN 0x4655b0 = PS4 0x6095d0 + SearchRoom
//     0x4d3c50, 0x4C == version): at most 5 of the rooms found, picked at random; selecting one (SELECT_JOIN_GAME
//     0x465808 = 0x609680) checks its free slots (the PS3 reservation 0x100/0x101, simplified) and joins it
//     (JoinRoom 0x4d4578).
//   - Every room request has the PS3 time limit (time_limit: join 5 s, create / search 20 s, the rest 10 s);
//     no answer in time counts as a failure answer.
//     The transport then joins the room owner; a party leader takes its party along.
//   - Member data 0x59 (48 bytes, PS3 0x4af240 = PS4 0x64e810), room hide (0x4d5338) and state 0x53
//     (0x4d3af8) follow NET_SM.
// No owner change or kick from the service: a host leaving ends the match as over LAN (the transport
// loses its host); kicks travel as session messages (session.cpp kKick).
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>

#include "cinema.h"
#include "frontend3.h"
#include "online.h"
#include "online_link.h"
#include "peer_rules.h"
#include "presence_block.h"
#include "progression.h"
#include "session_api.h"
#include "u2_online_abi.h"
#include "u2mp_internal.h"

namespace u2mp::online {
namespace {

// ---- guest data (PS4 1.02 image offsets) ------------------------------------------------------
constexpr uint64_t kNetState = 0x14ce97c;     // NET_SM (PS3 0xf11764)
constexpr uint64_t kMatch = 0x1065b08;        // g_matchSession (PS3 0xf20c48)
constexpr uint64_t kNetInfo = 0x104f1c8;      // PS3 0xf1fa48
constexpr uint64_t kNpd = 0x11474f8;          // NetPlayerData
constexpr uint64_t kNgm = 0x113b760;          // NetGameManager
constexpr uint64_t M_SEARCH_DONE = 0xa8, M_SEARCH_RESULT = 0xac;   // read by PAGE_MULTIPLAYER_JOIN 0x609640
constexpr uint64_t NI_VERSION = 0x00, NI_MODE = 0x2c4;
constexpr uint64_t NPD_TEAM = 0x114;          // custom game team preference 1 / 2 (PS3 NetPlayerData+211)
constexpr uint64_t fButtonJoin = 0x6095d0;    // BUTTON_MULTIPLAYER_JOIN (PS3 0x4655b0), intact but SearchRoom
constexpr uint64_t fPopPage = 0x5cc6f0;
constexpr uint32_t kPageJoin = 0xe8d59d98;    // PAGE_MULTIPLAYER_JOIN
static_assert(sid("PAGE_MULTIPLAYER_JOIN") == kPageJoin, "page id");
constexpr uint32_t kPageJoining = 0x1af4bdd2;   // PAGE_MULTIPLAYER_JOINING (PS4 0x609790)
static_assert(sid("PAGE_MULTIPLAYER_JOINING") == kPageJoining, "page id");
// NET_SM values
enum : int32_t { kServerLobby = 16, kChooseHostJoin = 18, kUpdate = 26, kReset = 27, kReplay = 28, kResults = 29,
                 kRoundResults = 30 };

// ---- room service values (PORT-MATCHMAKING.md 1.3) ---------------------------------------------
constexpr char kCommunicationId[] = "NPWR00462_00";   // retail NPWR00462 (PS3 0xe161b0)
constexpr uint16_t kVport = 1;                       // the one virtual port of the match traffic
constexpr uint32_t kFlagOwnerAutoGrant = 0x80000000u, kFlagClosed = 0x40000000u, kFlagHidden = 0x10000000u;
// 0x4D: the party's common map-pack mask of a matchmaking room (0 otherwise; spec parity-h-session.md 1.4 item 1).
constexpr uint16_t A_KEY = 0x4c, A_PACKS = 0x4d, A_LAT = 0x4e, A_LON = 0x4f, A_REGION = 0x50, A_TIER = 0x51,
                   A_KIND = 0x52, A_STATE = 0x53, A_LATLON = 0x55, A_NAME = 0x58;
// 0x52 (searchable int 7) is unused by PS3; it labels PC rooms for the local UI but is never a search
// filter. Matchmaking rooms carry the playlist index in 0x4C (PS3: index << 20 | version | 0x10000) and
// the tier in 0x51.
enum : uint32_t { kKindParty = 1, kKindCustom = 2, kKindMatchmaking = 3 };
// The 0x4C key: PS3 netInfo+0 (DC 6f1c3f87[0] | 0x90000); PS4 never fills it, then this PC value.
constexpr uint32_t kPcNetVersion = 0x00090000u | 0x004eu;   // PS3 netInfo+0 seen live (0x0009004e)
constexpr uint32_t kWorldOverride = 0u;                     // PS3 world 0x2001 is rejected by RPCN; 0 keeps the server world
// PS3 matchmaking range: Optimal/Close add 0x4E/0x4F distance and 0x50 region/language filters; Any adds
// none (SearchRoom 0x4655b0 passes 0, 0, -1). Keep Any so another player's room is never filtered out.
enum class MatchRange { Optimal, Close, Any };
constexpr MatchRange kMatchRange = MatchRange::Any;
constexpr int32_t kPcLat = 0, kPcLon = 0;                  // PS3 location object read 0/0
constexpr uint32_t kPcRegion = 0x0000004eu;                 // PS3 netInfo+0 language mask, region 0
// Room sizes (PS3 parity H1): the party room 10; a custom game room the game type's player count (0x49e620: 10
// competitive, 3 co-op; session::custom_room_slots); a matchmaking room the playlist's maximum
// (session::playlist_room_slots).
constexpr uint32_t kPartySlots = 10;
constexpr size_t kBlobSize = 48;                     // member bin 0x59 (PS3 0x4af240)
// kJoinTimeout: how long the link to a room owner may take to come up (not a room request; those: time_limit).
constexpr double kJoinTimeout = 20.0, kSwitchDelay = 0.3, kRttLogPeriod = 10.0;
// online message ops (session message type session::kOnlineMessage)
enum : uint8_t { kOpFollowRoom = 1,     // leader -> party: {u64 room id}: I joined that room, come along
                 kOpPromote = 2,        // leader -> party: {u64 room id, npid}: that player leads now (PS3 GrantRoomOwner)
                 kOpPartyIn = 3,        // leader -> party: {u8 ok}: the whole party is in the custom game room (PS3 0x10e)
                 kOpJoinResult = 4,     // member -> leader: {u8 ok}: joining the custom game room (PS3 0x10d)
                 kOpTalkAll = 5,        // member -> host: {u8 all}: our links to every other match room member are up
                                        // (PS3 tracker "canTalkToAll")
                 kOpOrder = 6 };        // host -> all: {u8 n, n x u8 net id}: the match room's owner succession (HostRank)
// Presence data (RPCN presence of this game, what friends' lists read): the PS3 88-byte block
// (presence_block.h, spec parity-g-friends.md 1.2), the same for PS3 and PC friends. PC builds up to 0.10.172 sent
// "U2MP" data (versions 1..3); it is still read. A PC also names the room a PC friend joins in the presence comment
// (the join hint; PS3 neither sets nor reads the comment).
namespace pres = u2mp::presence;
using pres::kOldMenu, pres::kOldParty, pres::kOldCustom, pres::kOldMatchmaking, pres::kOldMatch, pres::kOldCinema;
static_assert(pres::kBlockSize <= sizeof(U2oFriend::data), "presence data fits the ABI's 128 bytes");
constexpr char kPresenceTitle[] = "Uncharted 2: Among Thieves";
constexpr uint64_t kScoreTable = 0x104ae70;   // pointer; table + 0x3dee + 0x34 * t = u16 score of team t
// Presence (PS3 parity G2): checked every 5 s, sent on change; after a failed send the next check is 10 s later.
constexpr double kPresencePeriod = 5.0, kPresenceRetry = 10.0;
constexpr double kInviteLife = 600.0, kMigrateTimeout = 15.0, kPromoteDelay = 0.4;
constexpr double kPartyListLife = 5.0;   // a friend's party member list is asked again after 5 s (PS3 parity G4)
constexpr size_t kPartyListMax = 10;     // members kept of that list (PS3: 10 online ids)

template <typename T> T &G(uint64_t offset) { return *image.at<T>(offset); }
template <typename F> F guest(uint64_t offset) { return reinterpret_cast<F>(image.base + offset); }

double now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// ---- the online program (online_link.cpp) -----------------------------------------------------
struct Api {
    bool on = false;   // the program runs
    u2o_abi_fn abi = nullptr;
    u2o_open_fn open = nullptr;
    u2o_state_fn state = nullptr;
    u2o_poll_fn poll = nullptr;
    u2o_close_fn close = nullptr;
    u2o_find_world_fn find_world = nullptr;
    u2o_create_room_fn create_room = nullptr;
    u2o_join_room_fn join_room = nullptr;
    u2o_leave_room_fn leave_room = nullptr;
    u2o_search_rooms_fn search_rooms = nullptr;
    u2o_set_room_external_fn set_room_external = nullptr;
    u2o_set_room_internal_fn set_room_internal = nullptr;
    u2o_set_member_internal_fn set_member_internal = nullptr;
    u2o_members_external_fn members_external = nullptr;
    u2o_peer_address_fn peer_address = nullptr;
    u2o_peer_connect_fn peer_connect = nullptr;
    u2o_peer_disconnect_fn peer_disconnect = nullptr;
    u2o_peer_state_fn peer_state = nullptr;
    u2o_send_fn send = nullptr;
    u2o_recv_fn recv = nullptr;
    // ABI 2
    u2o_identity_fn identity = nullptr;
    u2o_friends_fn friends = nullptr;
    u2o_set_presence_fn set_presence = nullptr;
    u2o_send_message_fn send_message = nullptr;
    u2o_set_owner_rank_fn set_owner_rank = nullptr;
    u2o_friend_requests_fn friend_requests = nullptr;
    u2o_add_friend_fn add_friend = nullptr;
    u2o_remove_friend_fn remove_friend = nullptr;
    u2o_record_score_fn record_score = nullptr;
    u2o_get_scores_fn get_scores = nullptr;
} api;

bool load() {
    static int tried = 0;   // the environment is fixed for the process: decide once
    if (tried) return api.on;
    tried = 1;
    if (const char *join = std::getenv("UNCHARTED_U2MP_JOIN"); join && *join) {
        log("online: off (UNCHARTED_U2MP_JOIN is set: LAN join by address)");
        return false;
    }
    online_link::Path path;
    if (!online_link::env_path("UNCHARTED_U2MP_ONLINE_EXE", path)) {
        log("online: off (UNCHARTED_U2MP_ONLINE_EXE not set: no online extension): LAN only");
        return false;
    }
    if (const char *relay = std::getenv("UNCHARTED_U2MP_RELAY"); !relay || !*relay) {
        log("online: off (UNCHARTED_U2MP_RELAY not set: not signed in to RPCN in the launcher): LAN only");
        return false;
    }
    std::string why;
    if (!online_link::start(path, why)) {
        log("online: off (%s): LAN only", why.c_str());
        return false;
    }
    namespace L = online_link;
    api.abi = L::abi; api.open = L::open; api.state = L::state; api.poll = L::poll; api.close = L::close;
    api.find_world = L::find_world; api.create_room = L::create_room; api.join_room = L::join_room;
    api.leave_room = L::leave_room; api.search_rooms = L::search_rooms; api.set_room_external = L::set_room_external;
    api.set_room_internal = L::set_room_internal; api.set_member_internal = L::set_member_internal;
    api.members_external = L::members_external;
    api.peer_address = L::peer_address; api.peer_connect = L::peer_connect; api.peer_disconnect = L::peer_disconnect;
    api.peer_state = L::peer_state; api.send = L::send; api.recv = L::recv; api.identity = L::identity;
    api.friends = L::friends; api.set_presence = L::set_presence; api.send_message = L::send_message;
    api.set_owner_rank = L::set_owner_rank; api.friend_requests = L::friend_requests; api.add_friend = L::add_friend;
    api.remove_friend = L::remove_friend; api.record_score = L::record_score; api.get_scores = L::get_scores;
    api.on = true;
    log("online: u2_online.exe started (ABI %d)", int(U2O_ABI));
    return true;
}

// ---- state -------------------------------------------------------------------------------------
struct Member {
    uint16_t id = 0;
    std::string npid, name;
    uint64_t party = 0;   // the party room id its member data 0x59 names (+0), 0 none
    uint8_t team = 0;     // the team preference its member data names (+9): 1 or 2, 0 none
    double joined_at = 0; // when it joined while we were in the room (0: it was there before us)
};
uint64_t blob_party(const U2oMember &m) {
    uint64_t party = 0;
    if (m.bin_len >= 8) std::memcpy(&party, m.bin, 8);
    return party;
}
uint8_t blob_team(const U2oMember &m) {
    const uint8_t t = m.bin_len >= 10 ? m.bin[9] : 0;
    return t == 1 || t == 2 ? t : 0;
}
Member member_of(const U2oMember &m, double joined_at = 0) {
    return Member{m.id, std::string(m.npid, strnlen(m.npid, 20)), m.name, blob_party(m), blob_team(m), joined_at};
}
struct Room {
    uint64_t id = 0;
    uint32_t kind = 0;
    uint16_t my_id = 0, owner_id = 0, max_slot = 0;
    uint32_t flags = 0;
    bool hidden = false;
    int32_t state53 = 0;
    bool lobby_started = false;   // 0x53 = 1 sent for this lobby (PS3 0x465fe8)
    std::array<uint8_t, kBlobSize> sent_blob{};
    bool blob_sent = false;
    uint64_t rank_sig = 0;        // the member set the owner succession was last sent for
    std::vector<Member> members;
    bool owned() const { return id && my_id && my_id == owner_id; }
    const Member *member(uint16_t mid) const {
        for (const Member &m : members)
            if (m.id == mid) return &m;
        return nullptr;
    }
};

enum class Req { World, CreateParty, CreateMatch, JoinList, JoinLeader, JoinMatchMember, Leave, Search, SetExternal,
                 SetInternal, SetMember, CreateMatchmaking, JoinMatchmaking, MmSearch, Presence, Message, OwnerRank,
                 Rejoin, ScoreGet, ScoreRecord, PartyMembers };
struct Pending {
    Req kind;
    uint64_t room;
    uint32_t room_kind = 0;
    bool lead_party = false;   // JoinList: we led a party with others, it follows
    std::string what;
    double deadline = 0;       // no answer by then: failed (time_limit); 0 = no limit
};

// PS3 room request time limits (spec parity-h-session.md 1.4 item 3, H2): JoinRoom 5 s; CreateJoinRoom, SearchRoom
// and the world list the context default 20 s; LeaveRoom, room and member data, owner order 10 s. 0: no limit here
// (presence, messages, friends: not room requests; the leaderboards have their own, board_expire).
double time_limit(Req k) {
    switch (k) {
    case Req::JoinList: case Req::JoinLeader: case Req::JoinMatchMember: case Req::JoinMatchmaking: case Req::Rejoin:
        return 5;
    case Req::World: case Req::CreateParty: case Req::CreateMatch: case Req::CreateMatchmaking: case Req::Search:
    case Req::MmSearch:
        return 20;
    case Req::Leave: case Req::SetExternal: case Req::SetInternal: case Req::SetMember: case Req::OwnerRank: return 10;
    default: return 0;
    }
}
// The answer a request that ran out of time is failed with (the event kind its real answer has).
int32_t answer_kind(Req k) {
    switch (k) {
    case Req::World: return U2O_EV_WORLD;
    case Req::CreateParty: case Req::CreateMatch: case Req::CreateMatchmaking: return U2O_EV_ROOM_CREATED;
    case Req::JoinList: case Req::JoinLeader: case Req::JoinMatchMember: case Req::JoinMatchmaking: case Req::Rejoin:
        return U2O_EV_ROOM_JOINED;
    case Req::Leave: return U2O_EV_ROOM_LEFT;
    case Req::Search: case Req::MmSearch: return U2O_EV_SEARCH;
    default: return U2O_EV_DONE;
    }
}
constexpr int32_t kErrTimeout = -2;   // ours: the request's time limit passed without an answer

struct Peer {
    std::string npid;
    int32_t state = -1;
    uint32_t rtt_us = 0;
    bool was_active = false;    // this link was up at some point (a dead link that never was: "unreachable", I4)
    bool ended_by_us = false;   // we asked the online program to end the link (its death is not "unreachable")
};

bool started = false;          // Multiplayer entered with online on (the transport runs over the link)
int32_t link_state = U2O_OFF;
bool link_opened = false;
uint32_t world = 0;
uint16_t world_server = 0;
uint64_t world_req = 0;
double next_world_try = 0;
double next_party_try = 0, next_link_try = 0;   // backoffs: our party room create, reopening a lost link
Room party_room, match_room_;
std::map<uint64_t, Pending> pending;
std::vector<uint64_t> expired;   // requests failed by their time limit: a late answer is not taken (late_answer)
std::vector<Peer> peers;
std::string my_npid;

// join list (PS3: at most 5 rooms, random)
constexpr size_t kListRooms = 5;
std::vector<U2oRoomExt> found;
bool searching = false;
uint64_t search_req = 0;
char row_text[U2O_MAX_ROOMS][96];

// timers / flows
uint64_t create_party_req = 0, create_match_req = 0;
bool match_waiting = false;    // Start Match waits for the match room
std::string create_fail_why;   // the match room cannot be made: Start Match ends on the next frame (custom_create_failed)
bool waiting_host = false;     // the transport joins a room owner
double host_deadline = 0;
uint64_t waiting_room = 0;
bool following_leader = false; // the old host closes on purpose (session.cpp host_change_expected)
std::vector<std::string> led_party;   // the members we last led into another room (take_led_party)
net::Address switch_owner{};
double switch_at = 0;          // the leader switches its transport once the follow went out
double switch_admit = 20.0;    // the admit limit of that switch
double next_rtt_log = 0, next_blob_check = 0;
double join_page_deadline = 0;  // reached a host from the join list: no follow in 3 s -> back to PAGE_MULTIPLAYER
double admit_limit = kJoinTimeout;  // the owner must let us in this long after the link to it is up (PS3 10 s / 6 s)
bool admit_armed = false;       // the admit limit runs (the link to the owner came up)
bool joining_page = false;      // PAGE_MULTIPLAYER_JOINING is up for a join from the list (PS3 0x465808)
bool party_in_wait = false;     // member: the leader tells when the whole party is in its custom game room
double party_in_deadline = 0;
double party_wait_since = 0;    // leader: the custom game lobby waits for the party to be in the match room
std::string party_owner_tracked;   // member: the party owner whose link is watched (PS3 party state 6)
bool party_owner_seen_up = false;
double party_owner_down_since = 0;
bool host_gone_pending = false; // a stranger's matchmaking room is over for us (mm_host_gone), done next frame
bool host_gone_bad = false;
std::string host_gone_why;
bool leave_to_leader_pending = false;
int32_t last_net_state = -1;
bool in_match = false;
bool start_hidden = false;      // the leader pressed Start Match: the party room is hidden until the match runs

// identity, friends, invites, presence (ABI 2)
std::string online_name;       // the RPCN online name (Login reply), once the link is ready
std::vector<U2oFriend> friend_list;
std::vector<std::string> requests_in, requests_out;   // friend requests to us / ours
bool friends_dirty = true;
struct Invite {
    std::string npid;
    uint64_t room = 0;
    uint32_t kind = 0;
    uint8_t members = 0, max = 0;
    double at = 0;
};
std::vector<Invite> invites;
std::string sent_presence;     // status + data last published
double next_presence = 0;      // time of the next presence check
uint64_t presence_req = 0;     // the last presence request
bool friends_loaded = false;   // the friend list was read since the link came up (PS3: friend load done)
// A friend's party member list (PS3 parity G4): one cache, the friend it belongs to and when it was asked for,
// and the one request that may run.
struct PartyList {
    std::string npid;
    double at = 0;
    std::vector<std::string> names;
};
PartyList party_list, party_list_asked;   // the cache; the friend and time of the request in flight
uint64_t party_list_req = 0;
// Party Up rows: up to kMaxFriends friends (PS3 PSN allowed 100), then up to kMaxRequests requests each way
constexpr int kMaxFriends = 250, kMaxRequests = 32, kMaxFriendRows = kMaxFriends + 2 * kMaxRequests;
char friend_text[kMaxFriendRows][128];
char invite_text[16][128];

// rankings (RPCN score boards; PS3 NP Score)
struct BoardView {
    uint32_t board = 0;
    int mode = 0;
    uint64_t req = 0;
    bool loading = false, failed = false;
    double deadline = 0;       // no answer by then: failed
    uint32_t total = 0;
    std::vector<U2oScore> rows;
} board_view;
constexpr double kBoardTimeout = 10;   // PS3's limit on its stats storage requests
constexpr int kMaxScorePosts = 4;      // PS3's score post slots

// stranger matchmaking (PORT-MATCHMAKING.md 3.3)
// Checking: the room is joined and the link to its owner comes up; then the ping check of the pass (PS3 state 7)
// and only then the transport (and the party) goes to the owner.
enum class Mm { Idle, Searching, Joining, Checking, Creating, Hosting, Joined, Failed, NoRoom };
Mm mm_state = Mm::Idle;
uint64_t mm_search_req = 0;
uint32_t mm_playlist_index = 0;
uint32_t mm_playlist_id = 0;          // the playlist of the last search (its record gives the room size)
std::vector<uint64_t> mm_bad_rooms;   // rooms that did not work out this session (our own, by room id)
// PS3 0x4e8978 (spec parity-i-connection.md I4): players whose link died unreachable; no new link to them, their
// rooms are skipped, a listed party owner is left at once. Emptied when online multiplayer is entered (start).
peer_rules::UnreachableList unreachable;
MmCriteria mm_criteria;               // the pass running
std::vector<U2oRoomExt> mm_found;     // its search result: the next room is picked from it (PS3 state 6)
uint64_t mm_room_id = 0;              // the room being joined / checked
bool mm_lead_party = false;           // that join takes our party along
double mm_check_deadline = 0;         // the link to its owner must come up by then
constexpr double kMmAdmit = 6.0, kListAdmit = 10.0, kPartyHostLost = 5.0, kPartyInWait = 25.0;

// host migration / promote (PORT-MATCHMAKING.md 3.7)
bool migrating = false;
bool migrating_match = false;  // the host left a running match: the match goes on (session::online_rehost)
double migrate_deadline = 0;
uint64_t migrate_room = 0;
std::string migrate_old_owner; // the npid of the host that left
std::string promote_target;    // members: the npid the leader promoted (kOpPromote)
uint64_t promote_room = 0;
double promote_at = 0;         // the leader leaves and rejoins the room then
std::string host_npid;         // the player whose transport we are in (us while we host)
std::string pending_notice;    // a message box for the player, shown once a menu page allows it
// The match room's owner succession (PS3 HostRank 0x4d4868, parity H6): the host's order of the other players' net
// ids, sent to everyone at the match start (kOpOrder), so that every PC picks the same next owner (migration_tick).
std::vector<uint32_t> match_order;
std::map<std::string, bool> talk_all;   // host: per NpId, what the member last reported (kOpTalkAll)
int talk_all_sent = -1;                 // member: the value last reported, -1 none
std::string talk_all_host;              // member: the host it was reported to

const char *link_name(int32_t s) {
    switch (s) {
    case U2O_OFF: return "off";
    case U2O_CONNECTING: return "connecting";
    case U2O_READY: return "ready";
    case U2O_FAILED: return "failed";
    default: return "?";
    }
}
const char *peer_state_name(uint32_t s) {
    switch (s) {
    case U2O_PEER_INACTIVE: return "inactive";
    case U2O_PEER_PENDING: return "pending";
    case U2O_PEER_ACTIVE: return "active";
    case U2O_PEER_DEAD: return "dead";
    default: return "?";
    }
}
const char *kind_name(uint32_t k) {
    return k == kKindParty ? "party" : k == kKindCustom ? "custom game" : k == kKindMatchmaking ? "matchmaking" : "room";
}
const char *error_hint(int32_t e) {
    switch (e) {
    case 0: return "ok";
    case 14: return "no such room";
    case 16: return "room full";
    case 26: return "player not online";
    case kErrTimeout: return "no answer in time";
    default: return "room service error";
    }
}
// Peer addresses are logged without their last two bytes.
std::string masked(const uint8_t ip[4], uint16_t port) {
    char s[40];
    std::snprintf(s, sizeof s, "%u.%u.x.x:%u", ip[0], ip[1], port);
    return s;
}
std::string npid_text(const char *npid) { return std::string(npid, strnlen(npid, 20)); }

uint32_t room_key() {
    const uint32_t v = G<uint32_t>(kNetInfo + NI_VERSION);
    return v ? v : kPcNetVersion;
}

// The matchmaking room key (PS3 0x4b2a18 create / 0x4b1134 search): version, playlist index and, in a playlist with
// map-pack maps, the party's common map-pack mask from bit 12 up (`packs`, session::mm_pack_mask; 0 none).
uint32_t match_key(uint32_t playlist_index, uint32_t packs) {
    return ((playlist_index & 0xfff) << 20) | room_key() | 0x10000 | ((packs & 0xff) << 12);
}

uint32_t room_kind(const U2oRoomExt &x) {
    const uint32_t kind = x.int_attr[A_KIND - A_KEY];
    if (kind == kKindParty || kind == kKindCustom || kind == kKindMatchmaking) return kind;
    return (x.int_attr[A_KEY - A_KEY] & 0xfff00000u) ? kKindMatchmaking : kKindCustom;
}

uint32_t top_page_id() {
    const uint64_t menu = addr::menu_manager;
    const int32_t top = G<int32_t>(menu + 0x1010);
    if (top < 0 || top > 0x40) return 0;
    const uint64_t page = G<uint64_t>(menu + 0x9d0 + 8 * uint64_t(top));
    return page ? *reinterpret_cast<const uint32_t *>(uintptr_t(page) + 0x10) : 0;
}

// ---- peers and the link --------------------------------------------------------------------------
int peer_index(const std::string &npid, bool create) {
    for (size_t i = 0; i < peers.size(); ++i)
        if (peers[i].npid == npid) return int(i);
    if (!create || npid.empty()) return -1;
    peers.push_back(Peer{npid});
    return int(peers.size() - 1);
}
net::Address address_of(int index) { return net::Address{uint32_t(index + 1), kVport}; }
const Peer *peer_at(const net::Address &a) {
    if (a.port != kVport || a.ip == 0 || a.ip > peers.size()) return nullptr;
    return &peers[a.ip - 1];
}

class OnlineLink final : public net::Link {
public:
    void send(const net::Address &to, const uint8_t *data, size_t size) override {
        const Peer *p = peer_at(to);
        if (!p || !api.send) return;
        api.send(p->npid.c_str(), kVport, kVport, data, uint32_t(size));
    }
    int receive(net::Address &from, uint8_t *data, size_t capacity) override {
        if (!api.recv) return 0;
        char npid[20] = {};
        const int n = api.recv(kVport, data, uint32_t(capacity), npid);
        if (n == -1) {
            static bool once = false;
            if (!once) log("online: a datagram larger than %zu bytes was dropped", capacity);
            once = true;
            return 0;
        }
        if (n <= 0) return 0;
        from = address_of(peer_index(npid_text(npid), true));
        return n;
    }
} link;

// A new link state of a peer (event or poll). PS3 0x4e9618 lists a peer whose link died "unreachable" (spec
// parity-i-connection.md I4); the online program gives no reason, so a link that died before it was ever active and
// that we did not end stands for it (spec question 2).
void note_peer_state(Peer &p, int32_t s, uint32_t rtt_us) {
    const int32_t old = p.state;
    p.state = s;
    p.rtt_us = rtt_us;
    if (old == U2O_PEER_DEAD && s != U2O_PEER_DEAD) p.was_active = p.ended_by_us = false;   // a new link (PS3: a new record)
    if (s == U2O_PEER_ACTIVE) p.was_active = true;
    if (s != U2O_PEER_DEAD || old == U2O_PEER_DEAD || p.npid.empty() || p.npid == my_npid) return;
    if (!peer_rules::dead_means_unreachable(p.was_active, p.ended_by_us)) return;
    unreachable.add(p.npid);
    log("online: peer %s: the link died before it was ever up: unreachable, not tried again until Multiplayer is entered "
        "again (%zu of %zu listed; the online program gives no reason, so this is the PC stand-in for PS3's unreachable)",
        p.npid.c_str(), unreachable.size(), peer_rules::UnreachableList::kSlots);
}

// PS3 connect by online id (0x4e9e98): refused at once for a player on the unreachable list.
int connect_peer(const std::string &npid, const uint8_t ip[4], uint16_t port) {
    if (unreachable.contains(npid)) return -1;
    const int i = peer_index(npid, true);
    if (i >= 0) {
        Peer &p = peers[size_t(i)];
        p.ended_by_us = false;
        if (p.state != U2O_PEER_ACTIVE && p.state != U2O_PEER_PENDING) p.was_active = false;   // a new link
    }
    return api.peer_connect(npid.c_str(), ip, port);
}

void disconnect_peer(const std::string &npid) {
    const int i = peer_index(npid, false);
    if (i >= 0) peers[size_t(i)].ended_by_us = true;
    api.peer_disconnect(npid.c_str());
}

void connect_member(const U2oMember &m, uint16_t my_member_id, uint64_t room) {
    const std::string npid = npid_text(m.npid);
    if (npid.empty() || m.id == my_member_id || npid == my_npid) return;
    peer_index(npid, true);
    if (unreachable.contains(npid)) {
        log("online: peer %s (%s) in room %llx: on the unreachable list, not connecting (PS3 0x4e9e98 refuses it)",
            npid.c_str(), m.name, (unsigned long long)room);
        return;
    }
    const bool no_address = !m.ip[0] && !m.ip[1] && !m.ip[2] && !m.ip[3];
    if (no_address) {
        const uint64_t r = api.peer_address(npid.c_str());
        log("online: peer %s (%s) in room %llx: no address in the room data, asked for it (request %llu)", npid.c_str(),
            m.name, (unsigned long long)room, (unsigned long long)r);
        return;
    }
    const int r = connect_peer(npid, m.ip, m.port);
    log("online: peer %s (%s) in room %llx: connecting to %s (u2o_peer_connect %d)", npid.c_str(), m.name,
        (unsigned long long)room, masked(m.ip, m.port).c_str(), r);
}

// ---- rooms ---------------------------------------------------------------------------------------
void take_room(Room &r, const U2oRoom &src, uint32_t kind) {
    r.id = src.room_id;
    r.kind = kind;
    r.my_id = src.my_id;
    r.owner_id = src.owner_id;
    r.max_slot = src.max_slot;
    r.flags = src.flag_attr;
    r.hidden = (src.flag_attr & kFlagHidden) != 0;
    r.state53 = 0;
    r.lobby_started = false;
    r.blob_sent = false;
    r.members.clear();
    for (uint32_t i = 0; i < src.member_count && i < U2O_MAX_MEMBERS; ++i) r.members.push_back(member_of(src.members[i]));
}
bool in_room(const Room &r, const std::string &npid) {
    for (const Member &m : r.members)
        if (m.npid == npid) return true;
    return false;
}
Room *room_by_id(uint64_t id) {
    if (id && party_room.id == id) return &party_room;
    if (id && match_room_.id == id) return &match_room_;
    return nullptr;
}
void log_room(const char *what, const Room &r) {
    log("online: %s %s room %llx: %zu member(s) of %u, owner member %u, our member id %u, flags 0x%x", what,
        kind_name(r.kind), (unsigned long long)r.id, r.members.size(), unsigned(r.max_slot), unsigned(r.owner_id),
        unsigned(r.my_id), r.flags);
    for (const Member &m : r.members)
        log("online:   member %u %s (%s)%s", unsigned(m.id), m.name.c_str(), m.npid.c_str(),
            m.id == r.owner_id ? " owner" : "");
}

uint64_t request(uint64_t id, Pending p) {
    if (!id) return id;
    const double limit = time_limit(p.kind);
    p.deadline = limit > 0 ? now() + limit : 0;
    pending[id] = std::move(p);
    return id;
}

// A leaderboard fetch always ends (PS3 parity A 2.4): when the link is down (or `link_lost`) or no answer came within
// 10 s, it ends as failed; an answer arriving later is not taken.
void board_expire(bool link_lost = false) {
    if (!board_view.loading) return;
    if (!link_lost && link_state == U2O_READY && now() < board_view.deadline) return;
    log("rankings: board %u: no answer, failed", board_view.board);
    pending.erase(board_view.req);
    board_view.req = 0;
    board_view.loading = false;
    board_view.failed = true;
    board_view.total = 0;
    board_view.rows.clear();
}

void leave_room(Room &r, const char *why) {
    if (!r.id) return;
    const uint64_t id = api.leave_room(r.id);
    log("online: leaving %s room %llx (%s): request %llu", kind_name(r.kind), (unsigned long long)r.id, why,
        (unsigned long long)id);
    request(id, Pending{Req::Leave, r.id, r.kind, false, "leave"});
    r = Room{};
}

std::array<uint8_t, kBlobSize> member_blob() {
    // PS3 0x4af240 (PORT-MATCHMAKING.md 1.8): +0 party room id (party of more than one), +8 rank badge,
    // +9 team preference. +10..+47 (NetPlayerData stats / address words, G) are not known on PS4: 0.
    std::array<uint8_t, kBlobSize> b{};
    const uint64_t party = party_room.id && party_room.members.size() > 1 ? party_room.id : 0;
    std::memcpy(b.data(), &party, 8);
    b[8] = uint8_t(session::member_rank(session::local_id()));
    const int32_t team = G<int32_t>(kNpd + NPD_TEAM);
    b[9] = team == 1 || team == 2 ? uint8_t(team) : 0;
    return b;
}

void publish_blob(Room &r, bool force) {
    if (!r.id || link_state != U2O_READY) return;
    const auto b = member_blob();
    if (!force && r.blob_sent && b == r.sent_blob) return;
    const uint64_t id = api.set_member_internal(r.id, r.my_id, 0, b.data(), uint32_t(b.size()));
    log("online: member data 0x59 to %s room %llx: party room %llx, rank %u, team %u (request %llu)", kind_name(r.kind),
        (unsigned long long)r.id, (unsigned long long)(party_room.members.size() > 1 ? party_room.id : 0),
        unsigned(b[8]), unsigned(b[9]), (unsigned long long)id);
    request(id, Pending{Req::SetMember, r.id, r.kind, false, "member data 0x59"});
    if (id) {
        r.sent_blob = b;
        r.blob_sent = true;
    }
}

// A matchmaking room's size: the playlist's maximum player count (record +0x18, PS3 0x49a958); when the record has
// none, the largest player count of the playlist's game types (session::playlist_room_slots, H1).
uint32_t mm_room_slots(uint32_t playlist) {
    const int32_t n = playlist_max_players(playlist);
    if (n > 0 && n <= int32_t(kPartySlots)) return uint32_t(n);
    return uint32_t(std::max(1, session::playlist_room_slots()));
}

// The slot count a new room of this kind gets (PS3 parity H1): fixed at creation, later game type changes in the
// lobby do not resize it (PS3 neither). 0: the custom game's type is unknown (no room, as on PS3).
uint32_t room_slots(uint32_t kind) {
    if (kind == kKindCustom) return uint32_t(std::max(0, session::custom_room_slots()));
    if (kind == kKindMatchmaking) return mm_room_slots(mm_playlist_id);
    return kPartySlots;
}

uint64_t create_room(uint32_t kind) {
    // PS3 CreateJoinRoom 0x4d421c: searchable ints 0x4C key, 0x4D map-pack mask, 0x4E/0x4F lat/lon, 0x50
    // region/languages, 0x51 tier (matchmaking only), 0x52 kind (PC only), 0x53 state; bin 0x55 lat/lon
    // floats (8 bytes), bin 0x58 room name "%s.%lld"; member bin 0x59.
    static uint8_t latlon[8];
    static char name[64];
    const uint32_t slots = room_slots(kind);
    if (!slots) {
        log("online: the %s room is not made: the game type has no player count (0 slots)", kind_name(kind));
        return 0;
    }
    std::snprintf(name, sizeof name, "%s.%lld", session::local_name().c_str(), (long long)std::time(nullptr));
    const uint32_t packs = kind == kKindMatchmaking ? session::mm_pack_mask() : 0;
    const uint32_t key = kind == kKindMatchmaking ? match_key(mm_playlist_index, packs) : room_key();
    U2oAttr attrs[10] = {
        {A_KEY, 0, key, nullptr},                 {A_PACKS, 0, packs, nullptr},
        {A_LAT, 0, uint32_t(kPcLat), nullptr},    {A_LON, 0, uint32_t(kPcLon), nullptr},
        {A_REGION, 0, kPcRegion, nullptr},        {A_KIND, 0, kind, nullptr},
        {A_STATE, 0, 0, nullptr},                 {A_LATLON, uint16_t(sizeof latlon), 0, latlon},
        {A_NAME, uint16_t(std::strlen(name)), 0, reinterpret_cast<const uint8_t *>(name)},
    };
    uint32_t attr_count = 9;
    if (kind == kKindMatchmaking) attrs[attr_count++] = {A_TIER, 0, uint32_t(session::rank()), nullptr};
    if (kind == kKindParty && G<int32_t>(kNpd + NPD_TEAM) == 0) {
        // PS3 party machine state 1: no team preference yet -> 1 or 2 at random, published with the member data
        G<int32_t>(kNpd + NPD_TEAM) = 1 + std::rand() % 2;
        log("online: party: team preference %d picked at random", G<int32_t>(kNpd + NPD_TEAM));
    }
    const auto blob = member_blob();
    U2oCreate c{};
    c.max_slot = slots;
    // PS3 gave match rooms no auto grant and switched it on for host migration (HostMigrate 0x4d4b28);
    // RPCN hands a room to the next member whenever its owner leaves, so every room has it.
    c.flag_attr = kFlagOwnerAutoGrant;
    c.attrs = attrs;
    c.attr_count = attr_count;
    c.member_bin = blob.data();
    c.member_bin_len = uint32_t(blob.size());
    c.team = 0;
    c.signaling = 1;   // mesh: the room service hands out every member's peer address
    const uint64_t id = api.create_room(world, &c);
    log("online: creating the %s room in world %u: key 0x%x, map packs 0x%x, %u slots, name %s (request %llu)",
        kind_name(kind), world, key, packs, slots, name, (unsigned long long)id);
    request(id, Pending{kind == kKindParty ? Req::CreateParty : kind == kKindMatchmaking ? Req::CreateMatchmaking : Req::CreateMatch,
                        0, kind, false, "create"});
    return id;
}

void set_hidden(Room &r, bool on, bool starting = false) {
    // PS3 Hide 0x4d5338: flag filter CLOSED | HIDDEN, attr both or none.
    const uint64_t id = api.set_room_internal(r.id, kFlagClosed | kFlagHidden, on ? kFlagClosed | kFlagHidden : 0, nullptr, 0);
    log("online: %s room %llx %s (request %llu)", kind_name(r.kind), (unsigned long long)r.id,
        on ? (starting ? "hidden and closed (Start Match)" : "hidden and closed (match running)")
           : "shown and open again (lobby)",
        (unsigned long long)id);
    request(id, Pending{Req::SetInternal, r.id, r.kind, false, on ? "hide" : "show"});
    r.hidden = on;
}

void set_state53(Room &r, int32_t value) {
    // PS3 SetRoomDataExternal 0x4d3af8: searchable int 0x53 (1 at SERVER_LOBBY 0x465fe8, 0 at Leave::Enter).
    const U2oAttr a{A_STATE, 0, uint32_t(value), nullptr};
    const uint64_t id = api.set_room_external(r.id, &a, 1);
    log("online: %s room %llx: state 0x53 = %d (request %llu)", kind_name(r.kind), (unsigned long long)r.id, value,
        (unsigned long long)id);
    request(id, Pending{Req::SetExternal, r.id, r.kind, false, "state 0x53"});
    r.state53 = value;
}

// ---- join list -----------------------------------------------------------------------------------
void search_failed() {
    G<uint8_t>(kMatch + M_SEARCH_DONE) = 1;     // PAGE_MULTIPLAYER_JOIN 0x609640: pop, NET_SM 18
    G<int32_t>(kMatch + M_SEARCH_RESULT) = -1;
}

void start_search() {
    if (link_state != U2O_READY || !world) {
        log("online: join list: link %s, world %u: no search", link_name(link_state), world);
        search_failed();
        return;
    }
    // PS3 0x4655b0 -> SearchRoom 0x4d3c50(0, 0, -1, 0, 0, 0, 0): only 0x4C == netInfo+0; open rooms only.
    // That is the "Any game" range: no 0x4E/0x4F distance and no 0x50 region/language filters.
    U2oSearch s{};
    s.flag_filter = kFlagHidden | kFlagClosed;
    s.flag_attr = 0;
    s.ints[0] = U2oIntFilter{1, 0, A_KEY, room_key()};
    s.int_count = 1;
    s.start = 1;
    s.max = U2O_MAX_ROOMS;
    search_req = api.search_rooms(world, &s);
    searching = search_req != 0;
    found.clear();
    G<uint8_t>(kMatch + M_SEARCH_DONE) = 0;
    G<int32_t>(kMatch + M_SEARCH_RESULT) = 0;
    log("online: join list: searching world %u for key 0x%x, range %s, %u filter(s) (request %llu)",
        world, room_key(), kMatchRange == MatchRange::Any ? "any" : kMatchRange == MatchRange::Close ? "close" : "optimal",
        s.int_count, (unsigned long long)search_req);
    if (!searching) search_failed();
    else request(search_req, Pending{Req::Search, 0, 0, false, "search"});
}

// ---- transport switching -------------------------------------------------------------------------
void own_party(const char *why) {
    log("online: back to a party of our own (%s)", why);
    following_leader = false;
    waiting_host = false;
    switch_at = 0;
    host_npid = my_npid;
    session::online_reopen(&link, nullptr);
}

// `admit`: how long the owner may take to let us in once the link to it is up (PS3 reservation deadline: 10 s for a
// custom game, 6 s in matchmaking); the link itself has kJoinTimeout.
void switch_to_owner(const net::Address &owner, uint64_t room, double admit = kJoinTimeout) {
    waiting_host = true;
    waiting_room = room;
    host_deadline = now() + kJoinTimeout;
    admit_limit = admit;
    admit_armed = false;
    const Peer *p = peer_at(owner);
    host_npid = p ? p->npid : std::string();
    session::online_reopen(&link, &owner);
}

const Member *party_owner() {
    return party_room.id && !party_room.owned() ? party_room.member(party_room.owner_id) : nullptr;
}

// ---- PAGE_MULTIPLAYER_JOINING for a join from the list (PS3 0x465808 / 0x4661e0) -------------------------
// Failure: the page pops by itself on 0x10e {0} and NET_SM becomes 18; no message box (PS3).
void join_page_fail(const char *why) {
    log("online: joining failed (%s): PAGE_MULTIPLAYER_JOINING pops, NET_SM 18", why);
    joining_page = false;
    join_page_deadline = 0;
    session::online_party_in(false);
}

// A player whose game join failed drops out of its party (PS3 party "leave" flag, owner decision): the party room
// is left when others are in it, and this PC hosts a party of its own.
void drop_out_after_failed_join(const char *why) {
    if (party_room.id && party_room.members.size() > 1) leave_room(party_room, why);
    own_party(why);
}

// ---- stranger matchmaking: picking a room of the pass (PS3 0x4b0a58 state 6) -----------------------------
constexpr int kTierSpan = 20;   // rank badges

bool mm_bad(uint64_t room) { return std::find(mm_bad_rooms.begin(), mm_bad_rooms.end(), room) != mm_bad_rooms.end(); }

void mm_pick() {
    const int party = std::max(1, session::member_count());
    const U2oRoomExt *best = nullptr;
    for (const U2oRoomExt &x : mm_found) {
        const int free_slots = int(x.max_slot) - int(x.cur_members);
        const int dr = int(x.int_attr[A_TIER - A_KEY]) - session::rank();
        const bool close_rank = dr <= kTierSpan && dr >= -kTierSpan;
        if (mm_bad(x.room_id) || free_slots < party || room_by_id(x.room_id)) continue;
        if (unreachable.contains(npid_text(x.owner_npid))) continue;   // PS3 state 6 (0x4b14d4): owner unreachable
        if (mm_criteria.rank_only && !close_rank) continue;
        // PS3 scored by distance (no location on PC: every room is at distance 0); the fullest room wins, so
        // players gather.
        if (!best || x.cur_members > best->cur_members) best = &x;
    }
    if (!best) {
        log("online: matchmaking: criteria %d: no room to join", mm_criteria.pass);
        mm_state = Mm::NoRoom;
        return;
    }
    const auto blob = member_blob();
    const uint64_t id = api.join_room(best->room_id, blob.data(), uint32_t(blob.size()), 0);
    log("online: matchmaking: *********** Joining Room %s %u players (room %llx, party of %d): request %llu",
        best->owner_name, unsigned(best->cur_members), (unsigned long long)best->room_id, party, (unsigned long long)id);
    if (!id) {
        mm_bad_rooms.push_back(best->room_id);
        mm_pick();
        return;
    }
    mm_room_id = best->room_id;
    mm_lead_party = session::is_host() && party > 1;
    request(id, Pending{Req::JoinMatchmaking, best->room_id, kKindMatchmaking, mm_lead_party, "join (matchmaking)"});
    mm_state = Mm::Joining;
}

// The room being joined did not work out (join refused, no room for the party, owner unreachable, too slow, not
// admitted): mark it bad, leave it, pick the next room of the same pass.
void mm_room_failed(const char *why) {
    if (mm_state != Mm::Joining && mm_state != Mm::Checking) return;
    log("online: matchmaking: room %llx: %s; next room", (unsigned long long)mm_room_id, why);
    if (mm_room_id) mm_bad_rooms.push_back(mm_room_id);
    if (Room *r = room_by_id(mm_room_id)) leave_room(*r, why);
    if (waiting_host || switch_at) own_party(why);
    mm_room_id = 0;
    mm_pick();
}

// The transport did not reach the room owner (link dead, not in time, not admitted in time).
void owner_not_reached(const char *why) {
    if (mm_state == Mm::Checking && waiting_room && waiting_room == mm_room_id) {
        mm_room_failed(why);
        return;
    }
    if (Room *r = room_by_id(waiting_room)) leave_room(*r, why);
    if (joining_page) {   // PS3 "Joining Request timed out": the page below again, and the player leaves its party
        log("online: Joining Request timed out (%s)", why);
        join_page_fail(why);
        drop_out_after_failed_join(why);
        return;
    }
    own_party(why);
    if (top_page_id() == kPageJoin) search_failed();
}

// ---- party team auto-pick (PS3 party machine state 5 -> 6) --------------------------------------------------
void party_team_pick() {
    int on[3] = {0, 0, 0};
    for (const Member &m : party_room.members)
        if (m.npid != my_npid && m.id != party_room.my_id) on[m.team] += 1;
    const int team = on[1] < on[2] ? 1 : on[2] < on[1] ? 2 : 1 + std::rand() % 2;
    G<int32_t>(kNpd + NPD_TEAM) = team;
    log("online: party: %d member(s) on team 1, %d on team 2: our team is %d", on[1], on[2], team);
    publish_blob(party_room, true);
}

void back_to_multiplayer_page();

// The transport (and the party, when we lead one) goes to the owner of room `r`.
void go_to_owner(const Room &r, uint32_t kind, bool lead_party, double admit) {
    const Member *owner = r.member(r.owner_id);
    if (!owner) return;
    const net::Address addr = address_of(peer_index(owner->npid, true));
    log("online: the room owner is %s (%s): the transport joins it", owner->name.c_str(), owner->npid.c_str());
    following_leader = false;
    if (lead_party) {
        // PS3: the leader's party followed through member data +0 / mailbox; here a session message,
        // then this PC switches once it went out.
        std::vector<uint8_t> b{session::kOnlineMessage, kOpFollowRoom, 0, 0, 0, 0, 0, 0, 0, 0, uint8_t(kind)};
        std::memcpy(b.data() + 2, &r.id, 8);
        session::send_message(-1, b);
        led_party = session::member_npids();   // asked for at the owner's slot manager (session.cpp 0x100)
        log("online: told the party (%d member(s)) to follow into room %llx", session::member_count() - 1,
            (unsigned long long)r.id);
        switch_owner = addr;
        switch_at = now() + kSwitchDelay;
        switch_admit = admit;
        waiting_room = r.id;
    } else {
        switch_to_owner(addr, r.id, admit);
    }
}

void join_succeeded(const Pending &p, const U2oRoom &src) {
    const uint32_t kind = p.room_kind ? p.room_kind : kKindCustom;
    // The rooms we were in are left: the transport goes to the new room's owner. A matchmaking room is a match
    // room beside the party room (PS3 keeps the party session while matchmaking): the party room stays.
    if (party_room.id && party_room.id != src.room_id && kind != kKindMatchmaking) leave_room(party_room, "joined another room");
    if (match_room_.id && match_room_.id != src.room_id) leave_room(match_room_, "joined another room");
    Room &r = kind == kKindParty ? party_room : match_room_;
    take_room(r, src, kind);
    log_room("joined", r);
    for (uint32_t i = 0; i < src.member_count && i < U2O_MAX_MEMBERS; ++i) connect_member(src.members[i], src.my_id, r.id);
    const bool mm_join = p.kind == Req::JoinMatchmaking;
    const bool list_join = p.kind == Req::JoinList && kind == kKindCustom;
    const Member *owner = r.member(r.owner_id);
    if (!owner || owner->npid.empty()) {
        log("online: room %llx has no owner in its member list: leaving it", (unsigned long long)r.id);
        if (mm_join) {
            mm_room_failed("no owner");
            return;
        }
        leave_room(r, "no owner");
        if (list_join && joining_page) {
            join_page_fail("the room has no owner");
            drop_out_after_failed_join("join failed");
            return;
        }
        own_party("join failed");
        if (top_page_id() == kPageJoin) search_failed();
        return;
    }
    // PS3 reservation 0x100 / 0x101: the owner's slot manager refused a party that does not fit. Here the room
    // service's own member count at our join decides (fresh, not the search data): every member of our party
    // needs a slot.
    const int party = p.lead_party ? std::max(1, session::member_count()) : 1;
    if ((mm_join || list_join) && int(src.max_slot) - int(src.member_count) < party - 1) {
        log("online: room %llx has %d free slot(s) after our join, the party needs %d more: refused",
            (unsigned long long)r.id, int(src.max_slot) - int(src.member_count), party - 1);
        if (mm_join) {
            mm_room_failed("no room for the party");
        } else {
            leave_room(r, "no room for the party");
            join_page_fail("no room for the party");   // PS3 state 9 refusal: no party leave
        }
        return;
    }
    publish_blob(r, true);
    if (kind == kKindParty && !r.owned()) party_team_pick();
    if (mm_join) {
        // PS3 state 7: the link to the owner first, then the ping check of the pass (mm_check_tick).
        mm_state = Mm::Checking;
        mm_room_id = r.id;
        mm_lead_party = p.lead_party;
        mm_check_deadline = now() + kJoinTimeout;
        return;
    }
    go_to_owner(r, kind, p.lead_party, list_join ? kListAdmit : p.kind == Req::JoinLeader && kind == kKindMatchmaking ? kMmAdmit : kJoinTimeout);
    if (kind == kKindParty && p.kind == Req::JoinList && top_page_id() == kPageJoin) {
        // A party: back to PAGE_MULTIPLAYER (PS3 party join 0x4c4704 ended in NET_SM 18).
        guest<void(U2MP_GUEST *)(void *)>(fPopPage)(image.at<void>(addr::menu_manager));
        G<int32_t>(kNetState) = kChooseHostJoin;
    }
}

// ---- pages and notices ---------------------------------------------------------------------------
constexpr uint32_t kPageMultiplayer = sid("PAGE_MULTIPLAYER"), kPageLobby = sid("PAGE_LOBBY"),
                   kPageFriends = sid("PAGE_MULTIPLAYER_FRIENDS_LIST");

bool match_running(int32_t s) { return s == kUpdate || s == kReset || s == kReplay || s == kResults || s == kRoundResults; }

void notice(const std::string &text) {
    log("online: notice: %s", text.c_str());
    pending_notice = text;
}

void notice_tick() {
    if (pending_notice.empty()) return;
    if (match_running(G<int32_t>(kNetState))) return;
    const uint32_t top = top_page_id();
    if (top != kPageMultiplayer && top != kPageLobby && top != kPageFriends && top != kPageJoin) return;
    show_notice(pending_notice.c_str());
    pending_notice.clear();
}

bool page_in_stack(uint32_t id) {
    const uint64_t menu = addr::menu_manager;
    const int32_t top = G<int32_t>(menu + 0x1010);
    for (int32_t i = 0; i <= top && i <= 0x40; ++i) {
        const uint64_t page = G<uint64_t>(menu + 0x9d0 + 8 * uint64_t(i));
        if (page && *reinterpret_cast<const uint32_t *>(uintptr_t(page) + 0x10) == id) return true;
    }
    return false;
}

// Back to PAGE_MULTIPLAYER, NET_SM 18 (CHOOSE_HOST_JOIN), as PS3's party paths ended.
void back_to_multiplayer_page() {
    if (page_in_stack(kPageMultiplayer))
        for (int guard = 0; guard < 12 && top_page_id() != kPageMultiplayer; ++guard)
            guest<void(U2MP_GUEST *)(void *)>(fPopPage)(image.at<void>(addr::menu_manager));
    G<int32_t>(kNetState) = kChooseHostJoin;
}

// ---- friends, presence, invites (ABI 2; PS3 NP Basic, PORT-MATCHMAKING.md 1.10 / 3.6) -------------
// A friend's multiplayer presence of this game: the PS3 block (PS3 and PC), or the old PC data of 0.10.172 and
// older read with the PS3 meaning. `join` is the room a PC friend names for joining (old data or the join hint;
// room 0 = none), `pc` whether the friend is a PC (it sent old data or a join hint), `details` whether activity and
// values are known (not for old version 1 data). The key is not checked here (another build: see the callers).
struct FriendPresence {
    pres::Block b;
    pres::OldRoom join;
    bool pc = false, details = false;
};
bool friend_presence(const U2oFriend &f, FriendPresence &p) {
    p = FriendPresence{};
    if (!f.online || !f.same_game) return false;
    if (f.data_len == pres::kBlockSize && pres::decode(f.data, f.data_len, p.b)) {
        p.details = true;
        p.pc = pres::parse_join_hint(f.comment, sizeof f.comment, p.join);
        return true;
    }
    p.pc = true;
    return pres::decode_legacy(f.data, std::min<size_t>(f.data_len, sizeof f.data), p.b, p.join, p.details);
}
// The same, only when the friend plays our build (PS3 0x48f020: size 88 and our key).
bool our_presence(const U2oFriend &f, FriendPresence &p) { return friend_presence(f, p) && p.b.key == room_key(); }

// The friends list order (PS3 0x4c1fe0): friends in this game first, then the others online, then offline;
// within a group by online id (byte order, ascending).
int friend_group(const U2oFriend &f) {
    if (!f.online) return 2;
    return f.same_game || std::strncmp(f.title, kPresenceTitle, sizeof f.title) == 0 ? 0 : 1;
}

void refresh_friends() {
    if (!api.friends) return;
    friends_dirty = false;
    if (link_state == U2O_READY) friends_loaded = true;
    const int n = std::min(api.friends(nullptr, 0), kMaxFriends);
    friend_list.assign(size_t(std::max(n, 0)), U2oFriend{});
    if (n > 0) api.friends(friend_list.data(), n);
    std::stable_sort(friend_list.begin(), friend_list.end(), [](const U2oFriend &a, const U2oFriend &b) {
        const int ga = friend_group(a), gb = friend_group(b);
        if (ga != gb) return ga < gb;
        return npid_text(a.npid) < npid_text(b.npid);   // std::string compares bytes unsigned
    });
    for (int in = 1; in >= 0; --in) {
        std::vector<std::string> &v = in ? requests_in : requests_out;
        v.clear();
        char names[2048] = {};
        api.friend_requests(in, names, int(sizeof names));
        for (const char *p = names; *p && p < names + sizeof names && v.size() < size_t(kMaxRequests); p += std::strlen(p) + 1) v.push_back(p);
    }
}

// The room a friend of our build can be joined in (`r`; false none). A PC friend names it (old data: in a party /
// lobby / matchmaking room or cinema mode; join hint: whenever it has one), a free slot needed. A PS3 friend (no
// hint): its party room unless the party is hidden (PS3 0x43a340 offers Join Party only then).
bool join_room_of(const FriendPresence &p, pres::OldRoom &r) {
    r = p.join;
    if (p.b.key != room_key()) return false;
    if (p.pc) {
        if (!r.room || (r.max && r.members >= r.max)) return false;
        return !r.version || (r.state >= kOldParty && r.state <= kOldMatchmaking) || r.state == kOldCinema;
    }
    if (!p.b.party_room || p.b.party_hidden) return false;
    r = pres::OldRoom{};
    r.key = p.b.key;
    r.room = p.b.party_room;
    r.kind = kKindParty;
    return true;
}

// The running match's end in Unix UTC seconds (the PS3 presence clock), 0 without a time limit. Kept while
// it moves by 2 s or less, so the presence is not sent again for the clock's rounding.
uint64_t match_end_now() {
    static uint64_t kept = 0;
    const float limit = G<float>(kNgm + 0x21b8);
    if (!(limit > 0.0f) || int32_t(limit * 300.0f) <= 0) return kept = 0;
    const int16_t game = G<int16_t>(kNgm + 0x1ca0);
    const uint8_t *games = guest<const uint8_t *(U2MP_GUEST *)(uint32_t)>(0xa15e70)(0x3b9a067du);   // *net-games*
    int32_t type = -1;
    if (games && game >= 0 && game < *reinterpret_cast<const int32_t *>(games)) {
        const uint8_t *records = *reinterpret_cast<uint8_t *const *>(games + 8);
        if (records) type = *reinterpret_cast<const int32_t *>(records + 0x90 * size_t(game) + 0xc);
    }
    // PS3 (parity-n-inlined N1): no end time without a current net game record, nor for co-op (types 4, 8, 9, 12),
    // so friends see no "Match ends in" line for a co-op game.
    if (type < 0 || type == 4 || type == 8 || type == 9 || type == 12) return kept = 0;
    // The clock the HUD counts down: from the round start, from the match start in game type 10.
    const int32_t start = G<int32_t>(kNgm + (type == 10 ? 0x1d60 : 0x1d5c));
    const int32_t elapsed = int32_t(uint32_t(G<int32_t>(0x1116384)) - uint32_t(start));
    const double left = std::max(0.0, double(limit) - double(elapsed) / 300.0);
    const uint64_t utc = uint64_t(std::chrono::duration_cast<std::chrono::seconds>(
                                      std::chrono::system_clock::now().time_since_epoch()).count());
    const uint64_t end = utc + uint64_t(left);
    if (!kept || end > kept + 2 || end + 2 < kept) kept = end;
    return kept;
}

// The room a friend joins us in now, which an invite names too (nullptr: none); `state` in the old data's terms.
const Room *join_target(uint8_t &state, const char *&what) {
    const bool cinema = G<int32_t>(kNetInfo + NI_MODE) == 3;
    if (!started) return state = kOldMenu, what = "not in multiplayer", nullptr;
    if (cinema) {
        state = kOldCinema, what = "cinema";
        return match_room_.id ? &match_room_ : party_room.id ? &party_room : nullptr;
    }
    if (in_match) return state = kOldMatch, what = "match", nullptr;
    if (match_room_.id && match_room_.kind == kKindMatchmaking) return state = kOldMatchmaking, what = "matchmaking", &match_room_;
    if (match_room_.id) return state = kOldCustom, what = "custom game lobby", &match_room_;
    if (party_room.id) return state = kOldParty, what = "party", &party_room;
    return state = kOldParty, what = "multiplayer menu", nullptr;   // the party room not made yet: nothing to join
}

// What we do now, for our friends (PS3 NetFriends 0x490338; spec parity-g-friends.md 1.2): the PS3 block.
void presence_block_now(pres::Block &b) {
    b = pres::Block{};
    b.key = room_key();
    b.party = uint8_t(std::clamp<size_t>(party_room.id ? party_room.members.size() : 1, 1, 255));
    b.party_hidden = party_room.id && party_room.hidden;
    b.mode = uint8_t(G<int32_t>(kNetInfo + NI_MODE));
    b.party_room = party_room.id;
    // A net game exists (NGM match state, PS3 +0x1a8c): a film being played back (NGM film flag, PS3 +0x1ba4), else
    // a match with its players, team scores, end time.
    if (G<int32_t>(kNgm + 0x1e80) != 0) {
        b.net_game = G<int16_t>(kNgm + 0x1ca0);
        b.difficulty = G<uint8_t>(kNgm + 0x21c4);
        if (G<uint8_t>(kNgm + 0x209c)) {
            b.activity = pres::kActFilm;
        } else {
            b.activity = pres::kActMatch;
            b.players = uint8_t(std::clamp(session::member_count(), 0, 255));
            if (const uint8_t *table = reinterpret_cast<const uint8_t *>(G<uint64_t>(kScoreTable)))
                for (int t = 0; t < 2; ++t) std::memcpy(&b.score[t], table + 0x3dee + 0x34 * t, 2);
            b.match_end = match_end_now();
        }
    }
    // The career values (NetPlayerData data, spec progression.md); headshots: the local store's career count
    // (progression.cpp), as before.
    const uint32_t cash = G<uint32_t>(kNpd + 0xd8), spent = G<uint32_t>(kNpd + 0xdc);
    b.cash = cash;
    b.cash_left = cash > spent ? cash - spent : 0;
    b.kills = G<uint32_t>(kNpd + 0x184);
    b.deaths = G<uint32_t>(kNpd + 0x188);
    b.wins = G<uint32_t>(kNpd + 0x18c);
    b.losses = G<uint32_t>(kNpd + 0x190);
    b.headshots = progression_headshots();
    b.time_a = G<uint32_t>(kNpd + 0x198);
    b.time_b = G<uint32_t>(kNpd + 0x1a0);
    b.extra = G<uint32_t>(kNpd + 0x19c);
}

// A presence send failed (PS3 parity G2): forget what was sent, so the next check sends again, 10 s from now.
void presence_failed(double t) {
    sent_presence.clear();
    presence_req = 0;
    next_presence = t + kPresenceRetry;
    log("online: presence: not sent: trying again in %.0f s", kPresenceRetry);
}

// PS3 parity G2: one check every 5 s (the next time is set before the check); sent only when it changed. As PS3:
// the game title text in the title field, an empty status line, the block as data; outside multiplayer (PS3
// 0x48f16c at the MP clean-up) no data. The comment carries the PC join hint.
void publish_presence(double t) {
    if (link_state != U2O_READY || !api.set_presence || t < next_presence) return;
    next_presence = t + kPresencePeriod;
    uint8_t state = 0;
    const char *what = "";
    const Room *r = join_target(state, what);
    std::string hint;
    uint8_t data[pres::kBlockSize];
    pres::Block b;
    if (started) {
        presence_block_now(b);
        pres::encode(b, data);
        hint = pres::join_hint(r ? r->id : 0, r ? r->kind : 0, r ? unsigned(r->members.size()) : 0,
                               r ? unsigned(r->max_slot ? r->max_slot : kPartySlots) : 0);
    }
    std::string key = hint;
    key.push_back('\0');
    if (started) key.append(reinterpret_cast<const char *>(data), sizeof data);
    if (key == sent_presence) return;
    const uint64_t id = api.set_presence(kPresenceTitle, "", hint.c_str(), started ? data : nullptr,
                                         started ? uint32_t(sizeof data) : 0);
    if (started)
        log("online: presence: %s: activity %u, party %u%s, room %llx, net game %d (request %llu)", what,
            unsigned(b.activity), unsigned(b.party), b.party_hidden ? " (hidden)" : "", (unsigned long long)b.party_room,
            int(b.net_game), (unsigned long long)id);
    else
        log("online: presence: %s, no data (request %llu)", what, (unsigned long long)id);
    if (!id) {
        presence_failed(t);
        return;
    }
    request(id, Pending{Req::Presence, 0, 0, false, "presence"});
    sent_presence = key;
    presence_req = id;
}

// The answer to the party member request (PS3 parity G4): up to 10 online ids replace the cache, which then
// belongs to the friend asked for, timed at the request. A failed request leaves an empty list for that friend.
void party_list_answer(const U2oEvent &e) {
    party_list_req = 0;
    party_list.npid = party_list_asked.npid;
    party_list.at = party_list_asked.at;
    party_list.names.clear();
    for (uint32_t k = 0; !e.error && e.member && k < e.count && party_list.names.size() < kPartyListMax; ++k)
        party_list.names.push_back(npid_text(e.member[k].npid));
    log("online: friends: %s's party: %zu member(s)%s", party_list.npid.c_str(), party_list.names.size(),
        e.error ? " (the request failed)" : "");
    u2mp::friend_party_arrived(party_list.npid, party_list.names);
}

// A cinema file (PS3 0x449ae8 event 20 -> 0x43302c -> thread 0x433360): {"U2CN", key, name[64], size} + the file.
constexpr uint32_t kCinemaMagic = 0x4e433255u;   // "U2CN"
struct CinemaMeta {
    uint32_t magic, key;
    char name[64];
    uint32_t size, pad;
};
static_assert(sizeof(CinemaMeta) == 80, "cinema meta");
int cinemas_received = 0;

void on_cinema(const U2oMessage &m, const std::string &from) {
    CinemaMeta meta{};
    const uint8_t *all = m.blob ? m.blob : m.data;
    const uint32_t len = m.blob ? m.blob_len : std::min<uint32_t>(m.data_len, sizeof m.data);
    if (len < sizeof meta) return;
    std::memcpy(&meta, all, sizeof meta);
    if (meta.magic != kCinemaMagic) return;
    meta.name[sizeof meta.name - 1] = 0;
    if (meta.size != len - sizeof meta) {
        log("online: cinema from %s: %u bytes announced, %u came: dropped", from.c_str(), meta.size, unsigned(len - sizeof meta));
        notice("A cinema file from " + from + " did not arrive whole.");
        return;
    }
    const int r = cinema::add_received(all + sizeof meta, meta.size, meta.name);
    log("online: cinema file %s (%u bytes) from %s: %s", meta.name, meta.size, from.c_str(),
        r > 0 ? "saved" : r == 0 ? "no free slot" : "invalid");
    if (r > 0) {
        ++cinemas_received;
        // netInfo+0x18 = PS3's NP message count (0x43b200 / 0x449ae8): the cinema list panel (cond 100,
        // 0x65eda8) shows the L1 "Messages" entry (6b2970e2, remaining.cpp cinema_receive) only while it is set.
        ++*image.at<int32_t>(kNetInfo + 0x18);
    }
    notice(r > 0 ? from + " sent you a cinema file. It is in your cinema list now."
           : r == 0 ? from + " sent you a cinema file, but all 20 cinema slots are protected. You have no free slots available."
                    : from + " sent a cinema file that could not be read.");
}

void on_invite(const U2oEvent &e) {
    const U2oMessage &m = *e.message;
    const std::string from = npid_text(m.npid);
    if (m.main_type == U2O_MSG_CUSTOM_DATA) {
        on_cinema(m, from);
        return;
    }
    pres::OldRoom d;
    if (m.main_type != U2O_MSG_INVITE) {
        log("online: message from %s (type %u) ignored", from.c_str(), unsigned(m.main_type));
        return;
    }
    if (!pres::decode_old_room(m.data, std::min<size_t>(m.data_len, sizeof m.data), d) || !d.room) {   // 32+ bytes
        log("online: invite from %s without a room: ignored", from.c_str());
        return;
    }
    if (d.key != room_key()) {
        log("online: invite from %s for another build (key 0x%x, ours 0x%x): ignored", from.c_str(), d.key, room_key());
        notice(from + " invited you, but plays another version of the game.");
        return;
    }
    for (size_t i = 0; i < invites.size(); ++i)
        if (invites[i].npid == from) {
            invites.erase(invites.begin() + ptrdiff_t(i));
            break;
        }
    if (invites.size() >= 16) invites.erase(invites.begin());
    invites.push_back(Invite{from, d.room, d.kind, d.members, d.max, now()});
    log("online: invite from %s: %s room %llx (%u/%u)", from.c_str(), kind_name(d.kind), (unsigned long long)d.room,
        unsigned(d.members), unsigned(d.max));
    notice(from + " invited you to play. Choose Invites to join.");
}

void expire_invites(double t) {
    for (size_t i = invites.size(); i-- > 0;)
        if (t - invites[i].at > kInviteLife) invites.erase(invites.begin() + ptrdiff_t(i));
}

// Joins a room by its id (an invite, a friend's room): as from the join list.
bool join_by_id(uint64_t room, uint32_t kind, const std::string &owner, unsigned members, unsigned max) {
    if (!started || link_state != U2O_READY || !room) return false;
    if (room_by_id(room)) {
        notice("You are already there.");
        return true;
    }
    const bool leader = session::is_host();
    const int party = leader ? std::max(1, session::member_count()) : 1;
    if (max && int(max) - int(members) < party) {
        log("online: join refused: room %llx has %d free slot(s), the party needs %d", (unsigned long long)room,
            int(max) - int(members), party);
        notice("That game is full.");
        return true;
    }
    const auto blob = member_blob();
    const uint64_t id = api.join_room(room, blob.data(), uint32_t(blob.size()), 0);
    const uint32_t k = kind == kKindParty || kind == kKindMatchmaking ? kind : kKindCustom;
    log("online: joining %s's %s room %llx (party of %d): request %llu", owner.c_str(), kind_name(k), (unsigned long long)room,
        party, (unsigned long long)id);
    if (!id) return false;
    request(id, Pending{k == kKindMatchmaking ? Req::JoinMatchmaking : Req::JoinList, room, k, leader && party > 1, "join"});
    return true;
}

// ---- stranger matchmaking (PS3 0x4b0a58 states 19 -> 5 -> 6 -> 7 -> 9 / 3, PORT-MATCHMAKING.md 3.3) --
// State 5: the search result is listed, then state 6 picks a room from it (mm_pick). A failed search counts as a
// pass without rooms.
void mm_search_done(const U2oEvent &e) {
    if (mm_state != Mm::Searching) return;
    mm_found.clear();
    if (e.error) log("online: matchmaking: criteria %d: search failed: error %d (%s)", mm_criteria.pass, e.error, error_hint(e.error));
    for (uint32_t i = 0; i < e.count && e.rooms && !e.error; ++i) {
        const U2oRoomExt &x = e.rooms[i];
        // PS3 "%s, %i players %8.2f km": no location on PC, so no distance either
        log("online: matchmaking: %s, %u players (room %llx, %u slots, rank %u)%s", x.owner_name, unsigned(x.cur_members),
            (unsigned long long)x.room_id, unsigned(x.max_slot), unsigned(x.int_attr[A_TIER - A_KEY]),
            mm_bad(x.room_id) ? " (failed before)" : unreachable.contains(npid_text(x.owner_npid)) ? " (owner unreachable)" : "");
        mm_found.push_back(x);
    }
    mm_pick();
}

// State 7 on PC: the room is joined (the room service hands out the owner's address), the link to the owner comes
// up, its round trip time is checked against the pass's ping limit, and only then the transport (and the party)
// goes to the owner, which must let us in within 6 s (the PS3 reservation window of matchmaking).
void mm_check_tick(double t) {
    if (mm_state != Mm::Checking || waiting_host || switch_at) return;
    const Room *r = room_by_id(mm_room_id);
    const Member *owner = r ? r->member(r->owner_id) : nullptr;
    if (!r || !owner) {
        mm_room_failed("the room is gone");
        return;
    }
    if (unreachable.contains(owner->npid)) {
        mm_room_failed("the owner is on the unreachable list");
        return;
    }
    uint32_t rtt_us = 0;
    const int s = api.peer_state(owner->npid.c_str(), &rtt_us);
    if (const int i = peer_index(owner->npid, false); i >= 0) note_peer_state(peers[size_t(i)], s, rtt_us);
    if (s == U2O_PEER_DEAD) {
        mm_room_failed("the link to the owner failed");
        return;
    }
    if (s != U2O_PEER_ACTIVE) {
        if (t >= mm_check_deadline) mm_room_failed("the link to the owner did not come up");
        return;
    }
    const uint32_t rtt = rtt_us / 1000, limit = mm_criteria.ping_limit_ms;
    log("online: matchmaking: retRtt = %u ms to %s (criteria %d limit %u ms)", rtt, owner->name.c_str(), mm_criteria.pass, limit);
    if (limit && rtt > limit) {
        log("online: matchmaking: PingCriteria failed %u < %u", limit, rtt);
        mm_room_failed("too slow for this criteria");
        return;
    }
    go_to_owner(*r, kKindMatchmaking, mm_lead_party, kMmAdmit);
}

// ---- a stranger's matchmaking room is over for us (PS3 0x4b0a58 state 17 -> 35 -> 19) ------------------------
void mm_host_gone_now() {
    host_gone_pending = false;
    if (!started) return;
    const uint64_t room = match_room_.kind == kKindMatchmaking && !match_room_.owned() ? match_room_.id : 0;
    log("online: matchmaking: ********* HOST %s LEFT *********** (%s)", host_npid.c_str(), host_gone_why.c_str());
    if (room) {
        if (host_gone_bad) mm_bad_rooms.push_back(room);
        leave_room(match_room_, host_gone_why.c_str());
    }
    mm_state = Mm::Idle;
    mm_room_id = 0;
    const Member *leader = party_owner();
    if (leader && leader->npid != host_npid && !leader->npid.empty()) {
        // a party member waits for its leader (PS3: it stays in the party room and waits in 19 for 0x113)
        log("online: matchmaking: back to the party leader %s, waiting for it", leader->name.c_str());
        switch_to_owner(address_of(peer_index(leader->npid, true)), 0);
        matchmaking::on_host_left(false);
    } else {
        own_party(host_gone_why.c_str());
        matchmaking::on_host_left(true);
    }
}

void mm_leave_to_leader_now() {
    leave_to_leader_pending = false;
    if (!started) return;
    if (match_room_.kind == kKindMatchmaking && !match_room_.owned()) leave_room(match_room_, "the party leader left matchmaking");
    mm_state = Mm::Idle;
    const Member *leader = party_owner();
    if (leader && !leader->npid.empty() && leader->npid != host_npid) {
        log("online: matchmaking: the party leader %s left matchmaking: back to it", leader->name.c_str());
        switch_to_owner(address_of(peer_index(leader->npid, true)), 0);
    } else if (host_npid != my_npid) {
        own_party("the party leader left matchmaking");
    }
}

// ---- the party owner must stay reachable (PS3 party machine state 6, 5 s) ----------------------------------
void party_owner_tick(double t) {
    const Member *o = party_owner();
    if (!o || o->npid.empty()) {
        party_owner_tracked.clear();
        return;
    }
    if (party_owner_tracked != o->npid) {
        party_owner_tracked = o->npid;
        party_owner_seen_up = false;
        party_owner_down_since = 0;
    }
    const int i = peer_index(o->npid, false);
    const bool up = i >= 0 && peers[size_t(i)].state == U2O_PEER_ACTIVE;
    if (up) {
        party_owner_seen_up = true;
        party_owner_down_since = 0;
        return;
    }
    if (unreachable.contains(o->npid)) {   // PS3 state 6 (0x4c49fc): a listed owner is left at once
        log("online: party: Couldn't talk to the party host %s (on the unreachable list): leaving the party", o->name.c_str());
    } else {
        if (!party_owner_seen_up) return;   // the first contact has kJoinTimeout (waiting_host)
        if (!party_owner_down_since) party_owner_down_since = t;
        if (t - party_owner_down_since <= kPartyHostLost) return;
        log("online: party: Couldn't talk to the party host %s (no link for %.0f s): leaving the party", o->name.c_str(),
            kPartyHostLost);
    }
    const bool our_host = host_npid == o->npid;
    party_owner_tracked.clear();
    leave_room(party_room, "the party host is unreachable");
    if (our_host) own_party("the party host is unreachable");
}

// ---- the custom game lobby waits for the party (PS3 0x465da0 states 10 / 16, 0x4c3b58) ---------------------
bool party_in_match_room() {
    for (const std::string &npid : session::member_npids())
        if (!in_room(match_room_, npid)) return false;
    return true;
}

void send_party_in(bool ok) {
    session::send_message(-1, {session::kOnlineMessage, kOpPartyIn, uint8_t(ok ? 1 : 0)});
}

void abort_custom_lobby(const char *why) {
    log("online: A party member couldn't talk to the game host (%s): the party goes back to PAGE_MULTIPLAYER", why);
    send_party_in(false);
    party_wait_since = 0;
    start_hidden = false;   // back in the party: its room shows again
    leave_room(match_room_, "a party member could not join");
    session::online_match_abort("a party member could not join the custom game room");
}

// PS3 parity H2 (owner decision: PS3 behaviour): the custom game room could not be made (error answer, no answer in
// 20 s, link or world missing, no player count): "Session Create failed", the create page pops and NET_SM becomes
// 18 (PAGE_MULTIPLAYER with the party, no box), instead of playing on without a room.
void custom_create_failed(const char *why) {
    log("online: Session Create failed (%s): Start Match ends, back to PAGE_MULTIPLAYER, NET_SM 18", why);
    match_waiting = false;
    create_fail_why.clear();
    start_hidden = false;   // the party room shows again
    session::online_match_abort(why);
}

// PS3 parity H8 (owner decision): online play needs a room service world; without one PS3's NetInit failed with the
// network error. In Multiplayer: the network error (netInfo +0x1c, its box) and online play ends, not a local party.
// Outside Multiplayer the world is looked for again later.
void no_world_error() {
    if (!started) return;
    log("online: no room service world: network error, leaving online play (PS3 NetInit fails without a world)");
    session::online_network_error("no room service world");
}

// ---- host migration (PS3 0x4409c4 / 0x440478 -> RoomOwnerChanged -> listener slot 6) ---------------
void finish_migration(Room &r, const Member &owner) {
    migrating = false;
    promote_room = 0;   // a promotion ends here too
    promote_target.clear();
    const bool mine = r.owned();
    const uint64_t room_id = r.id;
    if (migrating_match) {
        migrating_match = false;
        const uint32_t id = mine ? session::local_id() : session::net_id_of(owner.npid);
        log("online: match migration: %s (%s, net id %u) owns room %llx now%s", owner.name.c_str(), owner.npid.c_str(), id,
            (unsigned long long)room_id, mine ? " (this PC)" : "");
        if (!id) {
            log("online: match migration: the new owner's net id is unknown: the match ends");
            session::online_host_gone();
            return;
        }
        if (mine) {
            host_npid = my_npid;
            session::online_rehost(&link, nullptr, id);
        } else {
            host_npid = owner.npid;
            waiting_room = room_id;
            const net::Address a = address_of(peer_index(owner.npid, true));
            session::online_rehost(&link, &a, id);
        }
        return;
    }
    log("online: host migration: %s (%s) owns %s room %llx now%s", owner.name.c_str(), owner.npid.c_str(), kind_name(r.kind),
        (unsigned long long)r.id, mine ? " (this PC)" : "");
    // A custom game lobby without its host goes on as a party (its members are in no party room).
    if (&r == &match_room_ && r.kind == kKindCustom && !party_room.id) {
        party_room = r;
        party_room.kind = kKindParty;
        match_room_ = Room{};
        if (mine) {
            const U2oAttr a[] = {{A_KIND, 0, kKindParty, nullptr}, {A_STATE, 0, 0, nullptr}};
            const uint64_t id = api.set_room_external(party_room.id, a, 2);
            request(id, Pending{Req::SetExternal, party_room.id, kKindParty, false, "kind 0x52 = party"});
        }
    }
    Room &room = match_room_.id == room_id ? match_room_ : party_room;
    const bool mm_room = room.kind == kKindMatchmaking;
    if (mine) {
        host_npid = my_npid;
        session::online_reopen(&link, nullptr);
    } else {
        switch_to_owner(address_of(peer_index(owner.npid, true)), room.id);
    }
    session::online_migrated(mine, mm_room);
    if (!mm_room) back_to_multiplayer_page();   // the party's leader page (PS3: the party room's new owner leads)
    notice(mine ? std::string("The leader left. You lead the party now.") : owner.name + " leads the party now.");
}

void migration_tick(double t) {
    if (!migrating) return;
    Room *r = room_by_id(migrate_room);
    if (!r) {
        log("online: host migration: room %llx is gone", (unsigned long long)migrate_room);
        migrating = false;
        session::online_host_gone();
        return;
    }
    const Member *o = r->member(r->owner_id);
    if (o && !o->npid.empty() && o->npid != migrate_old_owner) {
        const Member owner = *o;
        finish_migration(*r, owner);
        return;
    }
    if (t >= migrate_deadline) {
        // RPCN moves a room only when it notices the owner's session end; a PC that dropped off the network can
        // take minutes. Every PC then picks the same successor, the first one left of the succession the room was
        // given (RPCN's own move later agrees with it): the match room the host's order sent at the match start
        // (kOpOrder, parity H6), else (the party room, PS3 sent no order for it) the lowest member id left
        // (send_succession).
        const Member *next = nullptr;
        for (const Member &m : r->members)   // the player the leader promoted, if any (kOpPromote)
            if (!promote_target.empty() && m.npid == promote_target && m.npid != migrate_old_owner) next = &m;
        const bool by_order = !next && r == &match_room_ && !match_order.empty();
        for (size_t k = 0; by_order && !next && k < match_order.size(); ++k)
            for (const Member &m : r->members)
                if (!next && !m.npid.empty() && m.npid != migrate_old_owner && session::net_id_of(m.npid) == match_order[k])
                    next = &m;
        const bool ordered = next != nullptr;
        for (const Member &m : r->members)
            if (!next && m.npid != migrate_old_owner && !m.npid.empty()) next = &m;
        for (const Member &m : r->members)
            if (!ordered && promote_target.empty() && m.npid != migrate_old_owner && !m.npid.empty() && m.id < next->id)
                next = &m;
        if (next) {
            const Member owner = *next;
            log("online: host migration: RPCN has not moved room %llx in %.0f s; member %u (%s) takes over (%s)",
                (unsigned long long)migrate_room, kMigrateTimeout, unsigned(owner.id), owner.npid.c_str(),
                owner.npid == promote_target ? "promoted" : ordered ? "first of the match order" : "lowest id");
            r->owner_id = owner.id;
            finish_migration(*r, owner);
            return;
        }
        log("online: host migration: no new owner of room %llx in %.0f s: the party ends", (unsigned long long)migrate_room,
            kMigrateTimeout);
        migrating = false;
        migrating_match = false;
        session::online_host_gone();
    }
}

// ---- events --------------------------------------------------------------------------------------
// The link is gone: every room request with a time limit fails at the next check as if its limit had passed (no
// answer comes any more); the others are dropped.
void fail_pending_now() {
    for (auto it = pending.begin(); it != pending.end();) {
        if (it->second.deadline) {
            it->second.deadline = 1e-9;   // due at the next check (expire_requests)
            ++it;
        } else {
            it = pending.erase(it);
        }
    }
}

// The real answer of a request that already failed by its time limit (PS3: a timed-out request reaches its callback
// as a failure and nothing else). A room it made or joined is left at once.
bool late_answer(const U2oEvent &e) {
    if (!e.request || e.error == kErrTimeout) return false;
    const auto it = std::find(expired.begin(), expired.end(), e.request);
    if (it == expired.end()) return false;
    expired.erase(it);
    const bool room = (e.kind == U2O_EV_ROOM_CREATED || e.kind == U2O_EV_ROOM_JOINED) && !e.error && e.room;
    log("online: the answer to request %llu came after its time limit: %s", (unsigned long long)e.request,
        room ? "leaving that room" : "ignored");
    if (room) {
        const uint64_t id = api.leave_room(e.room->room_id);
        request(id, Pending{Req::Message, e.room->room_id, 0, false, "leave (answer after the time limit)"});
    }
    return true;
}

void on_event(const U2oEvent &e) {
    if (late_answer(e)) return;
    Pending p{Req::World, 0, 0, false, "request"};
    bool known = false;
    if (e.request) {
        auto it = pending.find(e.request);
        if (it != pending.end()) {
            p = it->second;
            known = true;
            pending.erase(it);
        }
    }
    switch (e.kind) {
    case U2O_EV_LINK_LOST:
        log("online: link lost (%s): every room is gone", e.text ? e.text : "no reason given");
        board_expire(true);
        party_room = Room{};
        match_room_ = Room{};
        fail_pending_now();   // no answer comes any more: each room request fails as its time limit would make it
        expired.clear();
        world = 0;
        world_req = 0;
        mm_state = mm_state == Mm::Idle ? Mm::Idle : Mm::Failed;
        mm_search_req = 0;
        sent_presence.clear();
        presence_req = 0;
        friends_loaded = false;
        party_list = PartyList{};
        party_list_req = 0;
        if (migrating) {
            migrating = false;
            session::online_host_gone();
        }
        break;
    case U2O_EV_SIGNED_OUT:   // the launcher signed out on purpose: no rejoin through a session that is gone
        log("online: signed out of RPCN in the launcher%s", started ? ": leaving online play" : "");
        if (started) session::online_signed_out();
        break;
    case U2O_EV_WORLD:
        world_req = 0;
        if (e.error) {
            log("online: no world: error %d (%s); trying again in 10 s", e.error, error_hint(e.error));
            next_world_try = now() + 10;
            no_world_error();
            break;
        }
        world = kWorldOverride ? kWorldOverride : e.world;
        world_server = e.server;
        if (const char *w = std::getenv("UNCHARTED_U2MP_WORLD")) {
            char *end = nullptr;
            const unsigned long v = std::strtoul(w, &end, 0);
            if (end && *end == '\0' && v && v <= 0xffff) {
                world = uint32_t(v);
                log("online: world override %u", world);
            }
        }
        log("online: world %u on server %u%s", world, unsigned(world_server), kWorldOverride ? " (override)" : "");
        break;
    case U2O_EV_ROOM_CREATED: {
        if (known && p.kind == Req::CreateMatchmaking) {   // PS3 "GATHER": CreateJoinRoom 0x4b2a18
            if (e.error || !e.room) {
                log("online: matchmaking: creating the room failed: error %d (%s)", e.error, error_hint(e.error));
                mm_state = Mm::Failed;
                break;
            }
            if (match_room_.id && match_room_.id != e.room->room_id) leave_room(match_room_, "a matchmaking room of our own");
            take_room(match_room_, *e.room, kKindMatchmaking);
            log_room("created", match_room_);
            mm_state = Mm::Hosting;
            break;
        }
        const bool match = known ? p.kind == Req::CreateMatch : e.request == create_match_req;
        if (match) create_match_req = 0;
        else create_party_req = 0;
        if (e.error || !e.room) {
            log("online: creating the %s room failed: error %d (%s)", match ? "custom game" : "party", e.error,
                error_hint(e.error));
            if (match && match_waiting) custom_create_failed(error_hint(e.error));
            if (!match) next_party_try = now() + 5;   // not again every frame
            break;
        }
        Room &r = match ? match_room_ : party_room;
        take_room(r, *e.room, match ? kKindCustom : kKindParty);
        log_room("created", r);
        if (match) {
            if (match_waiting) {
                match_waiting = false;
                session::online_match_ready(r.id);
            } else {
                log("online: the match room came after Start Match had given up waiting; kept");
            }
        }
        break;
    }
    case U2O_EV_ROOM_JOINED:
        if (!known) {
            log("online: room %llx joined for an unknown request %llu", (unsigned long long)e.room_id,
                (unsigned long long)e.request);
            break;
        }
        if (e.error || !e.room) {
            log("online: joining room %llx failed: error %d (%s)", (unsigned long long)p.room, e.error, error_hint(e.error));
            if (p.kind == Req::JoinList) {
                // PS3 "sceNpMatching2JoinRoom() failed": the page below again, no box, and the player leaves its party
                if (joining_page) {
                    join_page_fail("sceNpMatching2JoinRoom() failed");
                    drop_out_after_failed_join("could not join the game");
                } else if (top_page_id() == kPageJoin) {
                    search_failed();
                }
            } else if (p.kind == Req::JoinMatchmaking) {
                mm_room_failed("join refused");
            } else if (p.kind == Req::Rejoin) {
                own_party("could not rejoin the room after the promotion");
            } else if (p.kind == Req::JoinLeader) {
                // PS3 JOINING 12 -> 14 JoinRoom refused: leave the party, back to the page below, NET_SM 18 (no box)
                leave_room(party_room, "could not follow the leader");
                leave_room(match_room_, "could not follow the leader");
                own_party("could not follow the leader");
                back_to_multiplayer_page();
            } else if (p.kind == Req::JoinMatchMember) {
                // PS3 0x10d {0}: "I did not reach the game host"; the leader takes the whole party back (abort)
                log("online: could not join the leader's custom game room: telling the leader");
                session::send_message(0, {session::kOnlineMessage, kOpJoinResult, 0});
            }
            break;
        }
        if (p.kind == Req::JoinMatchMember) {   // a party member joins its leader's match room (no transport change)
            if (match_room_.id && match_room_.id != e.room->room_id) leave_room(match_room_, "the leader's new match room");
            take_room(match_room_, *e.room, kKindCustom);
            log_room("joined (party follow)", match_room_);
            for (uint32_t i = 0; i < e.room->member_count && i < U2O_MAX_MEMBERS; ++i)
                connect_member(e.room->members[i], e.room->my_id, match_room_.id);
            publish_blob(match_room_, true);
            session::online_match_member_joined();
            break;
        }
        // RPCN does not refuse a CLOSED room (it checks only FULL): an invite or a stale list can lead into a
        // match that is already running (the host closes and hides its room for the match, set_hidden).
        if ((p.kind == Req::JoinList || p.kind == Req::JoinMatchmaking) && (e.room->flag_attr & kFlagClosed)) {
            log("online: room %llx is closed (its match is running): leaving it again", (unsigned long long)e.room->room_id);
            const uint64_t id = api.leave_room(e.room->room_id);
            request(id, Pending{Req::Message, e.room->room_id, 0, false, "leave (closed room)"});
            if (p.kind == Req::JoinMatchmaking) {
                mm_room_failed("the room is closed");
            } else if (joining_page) {   // PS3: JoinRoom refused on a closed room, no box
                join_page_fail("the game has already started");
                drop_out_after_failed_join("could not join the game");
            } else {
                notice("That game has already started.");
                if (top_page_id() == kPageJoin) search_failed();
            }
            break;
        }
        join_succeeded(p, *e.room);
        break;
    case U2O_EV_ROOM_LEFT:
        log("online: left room %llx: error %d (%s)", (unsigned long long)e.room_id, e.error, error_hint(e.error));
        break;
    case U2O_EV_SEARCH: {
        if (e.request && e.request == mm_search_req) {
            mm_search_req = 0;
            mm_search_done(e);
            break;
        }
        if (e.request != search_req) break;   // an older search
        searching = false;
        if (e.error) {
            log("online: join list: search failed: error %d (%s)", e.error, error_hint(e.error));
            if (top_page_id() == kPageJoin) search_failed();
            break;
        }
        found.clear();
        for (uint32_t i = 0; i < e.count && e.rooms && i < U2O_MAX_ROOMS; ++i) {
            const U2oRoomExt &x = e.rooms[i];
            const bool ours = room_by_id(x.room_id) != nullptr;
            const uint32_t kind = room_kind(x);
            log("online: join list: room %llx %s of %s, %u/%u members, kind %u, state %u, flags 0x%x%s",
                (unsigned long long)x.room_id, kind_name(kind), x.owner_name, unsigned(x.cur_members),
                unsigned(x.max_slot), kind, x.int_attr[A_STATE - A_KEY], x.flag_attr,
                ours ? " (ours, not listed)" : "");
            if (!ours && kind != kKindMatchmaking) found.push_back(x);
        }
        // PS3 SearchRoom 0x4d3c50 (parity H3): at most 5 rooms, a random pick of the matches each time the list
        // opens. The room service hands out the first 20 matches; 5 of the listable ones are picked here at random
        // (all of them, in random order, when 5 or fewer).
        for (size_t i = found.size(); i > 1; --i) std::swap(found[i - 1], found[size_t(std::rand()) % i]);
        if (found.size() > kListRooms) found.resize(kListRooms);
        log("online: join list: %zu room(s) listed (%u found, %u in total)", found.size(), e.count, e.total);
        G<uint8_t>(kMatch + M_SEARCH_DONE) = 1;
        G<int32_t>(kMatch + M_SEARCH_RESULT) = 0;
        break;
    }
    case U2O_EV_DONE:
        if (e.error)
            log("online: %s on room %llx failed: error %d (%s)", known ? p.what.c_str() : "request",
                (unsigned long long)p.room, e.error, error_hint(e.error));
        else
            log("online: %s on room %llx done", known ? p.what.c_str() : "request", (unsigned long long)p.room);
        if (known && p.kind == Req::Presence && e.error && e.request == presence_req) presence_failed(now());
        if (known && p.kind == Req::PartyMembers) party_list_answer(e);   // an error without a list
        break;
    case U2O_EV_MEMBER_JOINED: {
        Room *r = room_by_id(e.room_id);
        if (!e.member) break;
        log("online: %s (%s) joined %s room %llx as member %u", e.member->name, npid_text(e.member->npid).c_str(),
            r ? kind_name(r->kind) : "an unknown", (unsigned long long)e.room_id, unsigned(e.member->id));
        if (!r) break;
        r->members.push_back(member_of(*e.member, now()));
        connect_member(*e.member, r->my_id, r->id);
        if (r == &party_room) {
            log("online: party: %s joined party", e.member->name);
            publish_blob(party_room, false);   // +0 party room id once the party has 2
        }
        if (r == &match_room_ && r->owned() && r->lobby_started) {
            set_state53(*r, 1);   // a joiner can miss the lobby-started flag sent before it joined
            log("online: custom game room %llx: state 0x53 re-sent after a member joined", (unsigned long long)r->id);
            if (r->kind == kKindCustom) send_party_in(true);   // a late party member waits for it (members not waiting ignore it)
        }
        break;
    }
    case U2O_EV_MEMBER_LEFT: {
        Room *r = room_by_id(e.room_id);
        if (!e.member) break;
        log("online: %s (%s) left room %llx (cause %u: %s)", e.member->name, npid_text(e.member->npid).c_str(),
            (unsigned long long)e.room_id, e.cause, e.cause == 1 ? "left" : e.cause == 5 ? "disappeared" : "other");
        if (!r) break;
        for (size_t i = 0; i < r->members.size(); ++i)
            if (r->members[i].id == e.member->id) {
                r->members.erase(r->members.begin() + ptrdiff_t(i));
                break;
            }
        if (e.member->id == r->owner_id)
            log("online: the owner of room %llx left; the new owner comes with the room update (U2O_EV_ROOM_UPDATED)",
                (unsigned long long)r->id);
        const std::string npid = npid_text(e.member->npid);
        // PS3 state 17: the host of a stranger's matchmaking room went away (not in a match: in-match host loss is the
        // match's own business).
        if (r == &match_room_ && r->kind == kKindMatchmaking && !r->owned() && !in_match && !npid.empty() &&
            npid == host_npid && npid != my_npid)
            mm_host_gone("the host left the room", false);
        const bool elsewhere = (r != &party_room && in_room(party_room, npid)) || (r != &match_room_ && in_room(match_room_, npid));
        if (!elsewhere && !npid.empty()) {
            disconnect_peer(npid);
            log("online: peer %s disconnected (in none of our rooms now)", npid.c_str());
        }
        if (r == &party_room) publish_blob(party_room, false);
        break;
    }
    case U2O_EV_ROOM_UPDATED: {
        Room *r = room_by_id(e.room_id ? e.room_id : e.room ? e.room->room_id : 0);
        if (!r || !e.room) break;
        const uint16_t old_owner = r->owner_id;
        r->owner_id = e.room->owner_id;
        r->flags = e.room->flag_attr;
        const Member *o = r->member(r->owner_id);
        log("online: %s room %llx updated: owner member %u (%s), flags 0x%x", kind_name(r->kind), (unsigned long long)r->id,
            unsigned(r->owner_id), o ? o->name.c_str() : "?", r->flags);
        if (old_owner != r->owner_id) {
            log("online: room %llx owner changed %u -> %u%s", (unsigned long long)r->id, unsigned(old_owner),
                unsigned(r->owner_id), r->owned() ? " (this PC now)" : "");
            if (r == &match_room_ && r->kind == kKindCustom && !migrating && promote_target.empty()) {
                log("online: the custom match room's host left; ending the session");
                session::online_host_gone();
            }
        }
        break;
    }
    case U2O_EV_MEMBER_UPDATED:
        if (e.member && e.member->bin_len >= 10) {
            uint64_t party = 0;
            std::memcpy(&party, e.member->bin, 8);
            log("online: member data of %s in room %llx: party room %llx, rank %u, team %u", e.member->name,
                (unsigned long long)e.room_id, (unsigned long long)party, unsigned(e.member->bin[8]),
                unsigned(e.member->bin[9]));
        }
        if (Room *r = e.member ? room_by_id(e.room_id) : nullptr)
            for (Member &m : r->members)
                if (m.id == e.member->id) {
                    m.party = blob_party(*e.member);   // the matchmaking rows' party colours
                    m.team = blob_team(*e.member);     // the team auto-pick of a party joiner
                }
        break;
    case U2O_EV_PEER_ADDRESS: {
        const std::string npid = npid_text(e.npid);
        if (e.error) {
            log("online: no address for peer %s: error %d (%s)", npid.c_str(), e.error, error_hint(e.error));
            break;
        }
        peer_index(npid, true);
        if (unreachable.contains(npid)) {
            log("online: peer %s: address %s, on the unreachable list: not connecting", npid.c_str(), masked(e.ip, e.port).c_str());
            break;
        }
        const int r = connect_peer(npid, e.ip, e.port);
        log("online: peer %s: address %s, connecting (u2o_peer_connect %d)", npid.c_str(), masked(e.ip, e.port).c_str(), r);
        break;
    }
    case U2O_EV_PEER_STATE: {
        const std::string npid = npid_text(e.npid);
        log("online: peer %s: %s, rtt %u ms", npid.c_str(), peer_state_name(e.state), e.rtt_us / 1000);
        // Run-time check (spec parity-i-connection.md question 2): the ABI names no reason for a dead link; if the
        // program ever fills one in, it shows here (the unreachable rule does not use it).
        if (e.state == U2O_PEER_DEAD && (e.error || e.cause))
            log("online: peer %s: dead with error %d, cause %u (not described by the ABI)", npid.c_str(), e.error, e.cause);
        const int i = peer_index(npid, true);
        if (i >= 0) note_peer_state(peers[size_t(i)], int32_t(e.state), e.rtt_us);
        if (waiting_host && e.state == U2O_PEER_DEAD) {
            Room *r = room_by_id(waiting_room);
            const Member *o = r ? r->member(r->owner_id) : nullptr;
            if (o && o->npid == npid) {
                log("online: the link to the room owner died before the transport reached it: leaving room %llx",
                    (unsigned long long)waiting_room);
                owner_not_reached("room owner unreachable");
            }
        }
        break;
    }
    case U2O_EV_MEMBERS_EXT:
        log("online: %u external member(s) of room %llx", e.count, (unsigned long long)e.room_id);
        if (known && p.kind == Req::PartyMembers) party_list_answer(e);
        break;
    case U2O_EV_PUBLIC_ADDRESS:
        log("online: the room service sees our peer socket at %s", masked(e.ip, e.port).c_str());
        break;
    case U2O_EV_FRIEND: {
        friends_dirty = true;
        const std::string who = npid_text(e.npid);
        switch (int32_t(e.state)) {
        case U2O_FRIEND_LIST:
            refresh_friends();
            log("online: friends: %zu on the RPCN friend list", friend_list.size());
            for (const U2oFriend &f : friend_list)
                log("online:   friend %s: %s%s%s", npid_text(f.npid).c_str(), f.online ? "online" : "offline",
                    f.same_game ? ", in Uncharted 2: " : "", f.same_game ? f.status : "");
            break;
        case U2O_FRIEND_ADDED: log("online: friends: %s added", who.c_str()); break;
        case U2O_FRIEND_REMOVED: log("online: friends: %s removed", who.c_str()); break;
        case U2O_FRIEND_ONLINE: log("online: friends: %s is online", who.c_str()); break;
        case U2O_FRIEND_OFFLINE: log("online: friends: %s went offline", who.c_str()); break;
        case U2O_FRIEND_PRESENCE: break;   // frequent: not logged
        case U2O_FRIEND_REQUEST:
            log("online: friends: friend request from %s (accept it in the launcher)", who.c_str());
            notice(who + " sent you a friend request. Accept it in the launcher.");
            break;
        default: break;
        }
        break;
    }
    case U2O_EV_MESSAGE:
        if (e.message) on_invite(e);
        break;
    case U2O_EV_SCORES:
        if (known && p.kind == Req::ScoreRecord) {
            log("online: rankings: score on board %llu %s (rank %u)", (unsigned long long)p.room,
                e.error ? "not kept" : "recorded", e.state);
            break;
        }
        if (!board_view.req || e.request != board_view.req) break;   // an older view, or one that timed out
        board_view.loading = false;
        board_view.failed = e.error != 0;
        board_view.total = e.total;
        board_view.rows.assign(e.scores, e.scores + (e.scores ? e.count : 0));
        log("online: rankings: board %u mode %d: %u row(s) of %u (error %d)", board_view.board, board_view.mode, e.count,
            e.total, e.error);
        break;
    default:
        log("online: event %d (error %d) ignored", e.kind, e.error);
        break;
    }
}

// PS3 parity H2: a room request whose time limit passed fails exactly as an error answer of its kind would (its own
// handler in on_event runs with the error "no answer in time"); its real answer, should it still come, is not taken.
void expire_requests(double t) {
    std::vector<uint64_t> due;
    for (const auto &[id, p] : pending)
        if (p.deadline > 0 && t >= p.deadline) due.push_back(id);
    for (const uint64_t id : due) {
        const auto it = pending.find(id);
        if (it == pending.end()) continue;   // an earlier failure's handler dropped it (link lost)
        log("online: %s on room %llx (request %llu): no answer in %.0f s: failed", it->second.what.c_str(),
            (unsigned long long)it->second.room, (unsigned long long)id, time_limit(it->second.kind));
        U2oEvent e;
        std::memset(&e, 0, sizeof e);
        e.kind = answer_kind(it->second.kind);
        e.error = kErrTimeout;
        e.request = id;
        e.room_id = it->second.room;
        if (expired.size() >= 64) expired.erase(expired.begin());
        expired.push_back(id);
        on_event(e);
    }
}

void open_link() {
    const char *relay = std::getenv("UNCHARTED_U2MP_RELAY");   // never logged
    if (link_opened) api.close();
    const int r = api.open(relay ? relay : "", kCommunicationId);
    link_opened = true;
    link_state = U2O_CONNECTING;
    world = 0;
    world_req = 0;
    log("online: opening the link (communication id %s): u2o_open %d", kCommunicationId, r);
}

// NET_SM-driven room state (PS3 call sites in PORT-MATCHMAKING.md 1.6 / 1.7 and section 6 item 9).
// The owner succession of a room we own: the other members, lowest member id first (RPCN otherwise picks any).
void send_succession(Room &r) {
    if (!r.owned() || promote_room == r.id) return;
    std::vector<uint16_t> ids;
    for (const Member &m : r.members)
        if (m.id != r.my_id && ids.size() < U2O_MAX_MEMBERS) ids.push_back(m.id);
    std::sort(ids.begin(), ids.end());
    uint64_t sig = 0;
    for (uint16_t i : ids) sig = sig * 1000003u + i;
    if (ids.empty() || sig == r.rank_sig) return;
    r.rank_sig = sig;
    const uint64_t id = api.set_owner_rank(r.id, ids.data(), uint32_t(ids.size()));
    log("online: %s room %llx: owner succession of %zu member(s) (request %llu)", kind_name(r.kind),
        (unsigned long long)r.id, ids.size(), (unsigned long long)id);
    request(id, Pending{Req::OwnerRank, r.id, r.kind, false, "owner succession"});
}

// PS3 HostRank 0x4d4868 at the match start (parity H6): who takes the match room over when the host leaves. The other
// players in game slot order (transport net id = slot + 1), those whose links to every other player are up
// (kOpTalkAll, PS3 "canTalkToAll") in front, the order otherwise kept; room members without a net id last, by member
// id. The order goes to the room and to every player (kOpOrder), so that a PC that has to pick the next owner itself
// (migration_tick) picks the one RPCN picks.
void send_match_order() {
    struct Entry {
        uint32_t net_id;
        uint16_t member;
        bool all;
    };
    std::vector<Entry> v;
    for (const Member &m : match_room_.members) {
        if (m.id == match_room_.my_id || m.npid.empty() || m.npid == my_npid) continue;
        const auto it = talk_all.find(m.npid);
        const uint32_t nid = session::net_id_of(m.npid);
        v.push_back(Entry{nid ? nid : 0xffffffffu, m.id, nid && it != talk_all.end() && it->second});
    }
    std::sort(v.begin(), v.end(), [](const Entry &a, const Entry &b) {
        return a.net_id != b.net_id ? a.net_id < b.net_id : a.member < b.member;
    });
    std::stable_partition(v.begin(), v.end(), [](const Entry &x) { return x.all; });
    std::vector<uint16_t> ids;
    std::vector<uint8_t> msg{session::kOnlineMessage, kOpOrder, 0};
    match_order.clear();
    for (const Entry &x : v) {
        if (ids.size() < U2O_MAX_MEMBERS) ids.push_back(x.member);
        if (x.net_id != 0xffffffffu && match_order.size() < 16) {
            match_order.push_back(x.net_id);
            msg.push_back(uint8_t(x.net_id));
        }
    }
    msg[2] = uint8_t(match_order.size());
    match_room_.rank_sig = 0;
    for (uint16_t i : ids) match_room_.rank_sig = match_room_.rank_sig * 1000003u + i;
    if (ids.empty()) return;
    const uint64_t id = api.set_owner_rank(match_room_.id, ids.data(), uint32_t(ids.size()));
    std::string order;
    for (uint32_t n : match_order) order += (order.empty() ? "" : ", ") + std::to_string(n);
    log("online: match room %llx: owner succession of %zu player(s), net ids %s (request %llu)",
        (unsigned long long)match_room_.id, ids.size(), order.empty() ? "-" : order.c_str(), (unsigned long long)id);
    request(id, Pending{Req::OwnerRank, match_room_.id, match_room_.kind, false, "owner succession"});
    session::send_message(-1, msg);
}

// Member of a match room: tells the host whether its links to every other member are up (PS3 tracker canTalkToAll),
// once and whenever it changes or the host changes.
void talk_all_tick() {
    if (session::is_host() || !match_room_.id || host_npid.empty() || host_npid == my_npid || waiting_host || switch_at) {
        if (session::is_host()) talk_all_sent = -1;
        return;
    }
    bool all = true;
    for (const Member &m : match_room_.members) {
        if (m.id == match_room_.my_id || m.npid.empty() || m.npid == my_npid) continue;
        const int i = peer_index(m.npid, false);
        all = all && i >= 0 && peers[size_t(i)].state == U2O_PEER_ACTIVE;
    }
    if (talk_all_sent == int(all) && talk_all_host == host_npid) return;
    talk_all_sent = int(all);
    talk_all_host = host_npid;
    session::send_message(0, {session::kOnlineMessage, kOpTalkAll, uint8_t(all ? 1 : 0)});
    log("online: told the host %s that our links to every other player are %s", host_npid.c_str(), all ? "up" : "not all up");
}

void room_state() {
    const int32_t s = G<int32_t>(kNetState);
    const bool match_running = s == kUpdate || s == kReset || s == kReplay || s == kResults || s == kRoundResults;
    if (s != last_net_state) {
        if (match_running && !in_match) log("online: NET_SM %d: match running", s);
        if (!match_running && in_match) log("online: NET_SM %d: back from the match", s);
        last_net_state = s;
    }
    if (s == 35) session::note_leaving();
    const bool ended = in_match && !match_running;
    const bool began = !in_match && match_running;
    in_match = match_running;
    if (began) start_hidden = false;   // the match's own hide takes over (and ends with the match)
    if (began && link_state == U2O_READY && match_room_.owned()) send_match_order();
    if (link_state != U2O_READY) return;
    send_succession(party_room);   // the party room's owner order, whenever its members change
    for (Room *r : {&party_room, &match_room_}) {
        if (!r->owned()) continue;
        // 0x4d5338: closed during the match, and the party room from the leader's Start Match on (0x43aab0)
        const bool starting = r == &party_room && start_hidden && !match_running;
        const bool hide = match_running || starting;
        if (r->hidden != hide) set_hidden(*r, hide, starting);
    }
    if (match_room_.owned()) {
        if (s == kServerLobby && !match_room_.lobby_started) {   // 0x465fe8: the lobby task started
            // PS3 0x465da0 state 16: a custom game room says "lobby" (0x53 = 1) only once every party member is in
            // it (states 3 / 10, 0x4c3b58); a member that cannot join takes the whole party back.
            const bool custom = match_room_.kind == kKindCustom;
            if (custom && !party_in_match_room()) {
                const double t = now();
                if (!party_wait_since) {
                    party_wait_since = t;
                    log("online: custom game room %llx: waiting for the party to join it", (unsigned long long)match_room_.id);
                }
                if (t - party_wait_since >= kJoinTimeout) abort_custom_lobby("not in the room in time");
                return;
            }
            party_wait_since = 0;
            match_room_.lobby_started = true;
            set_state53(match_room_, 1);
            if (custom) {
                log("online: custom game room %llx: the whole party is in", (unsigned long long)match_room_.id);
                send_party_in(true);
            }
        }
        if (ended && match_room_.state53 != 0) set_state53(match_room_, 0);   // Leave::Enter 0x445e9c
    }
}

}  // namespace

// ---- online.h --------------------------------------------------------------------------------------
net::Link *start() {
    if (!load()) return nullptr;
    if (const char *n = std::getenv("UNCHARTED_U2MP_NPID"); n && *n) my_npid = std::string(n).substr(0, 16);
    if (!link_opened || link_state == U2O_FAILED || link_state == U2O_OFF) open_link();
    if (!started && unreachable.size()) {   // PS3 NetInit (0x4e9d28): a new online session starts with an empty list
        log("online: Multiplayer entered: the unreachable list (%zu) is emptied", unreachable.size());
        unreachable.clear();
    }
    started = true;
    waiting_host = following_leader = false;
    switch_at = 0;
    host_npid = my_npid;
    log("online: Multiplayer entered online as %s (NpId %s): link %s, world %u", session::local_name().c_str(),
        my_npid.empty() ? "?" : my_npid.c_str(), link_name(link_state), world);
    return &link;
}

bool active() { return started; }

void frame() {
    clan_tag_check_frame();   // runs online and on LAN alike (NET_SM > 1), with or without the online program
    if (!api.on || !link_opened) return;
    const double t = now();
    char text[128] = {};
    const int32_t st = api.state(text, int(sizeof text));
    if (st != link_state) {
        log("online: link %s -> %s (%s)", link_name(link_state), link_name(st), st == U2O_READY ? text : (*text ? text : "-"));
        if (link_state == U2O_READY) fail_pending_now();   // no answer comes over a link that went down
        link_state = st;
        if (st != U2O_READY) friends_loaded = false;
        if (st == U2O_READY) {   // the RPCN online name is this player's name everywhere (PS3: NP online id)
            char npid[20] = {}, name[52] = {};
            if (api.identity(npid, name) && *name) {
                online_name = std::string(name, strnlen(name, sizeof name));
                if (my_npid.empty()) my_npid = npid_text(npid);
                log("online: signed in as %s (NpId %s)", online_name.c_str(), my_npid.c_str());
                session::set_local_name(online_name);
            }
            refresh_friends();
        }
    }
    if (link_state == U2O_READY && !world && !world_req && t >= next_world_try) {
        world_req = api.find_world();
        log("online: looking for a world (request %llu)", (unsigned long long)world_req);
        request(world_req, Pending{Req::World, 0, 0, false, "find world"});
        if (!world_req) {
            next_world_try = t + 10;
            no_world_error();
        }
    }
    U2oEvent e;
    for (int guard = 0; guard < 256; ++guard) {
        std::memset(&e, 0, sizeof e);
        if (api.poll(&e) <= 0) break;
        on_event(e);
    }
    expire_requests(t);
    board_expire();
    if (friends_dirty) refresh_friends();
    publish_presence(t);
    expire_invites(t);
    notice_tick();
    if (!started) return;
    migration_tick(t);
    if (promote_at && t >= promote_at) {   // the leader steps down: leave the room and join it again
        promote_at = 0;
        if (Room *r = room_by_id(promote_room)) {
            const uint32_t kind = r->kind;
            leave_room(*r, "promoted another player");
            const auto blob = member_blob();
            const uint64_t id = api.join_room(promote_room, blob.data(), uint32_t(blob.size()), 0);
            log("online: promote: joining room %llx again as a member (request %llu)", (unsigned long long)promote_room,
                (unsigned long long)id);
            following_leader = true;
            if (id) request(id, Pending{Req::Rejoin, promote_room, kind, false, "rejoin"});
            else own_party("could not rejoin after the promotion");
        }
    }
    // the link went down (launcher reconnect, network): open it again every 15 s while in multiplayer
    const char *relay_env = std::getenv("UNCHARTED_U2MP_RELAY");   // the launcher's relay (never logged)
    if (started && link_opened && relay_env && *relay_env && (link_state == U2O_FAILED || link_state == U2O_OFF) &&
        t >= next_link_try) {
        next_link_try = t + 15;
        log("online: the link is down: opening it again");
        open_link();
    }
    // our own party room, while this PC leads a party and is in no other room (PS3 0x4c48d0)
    if (link_state == U2O_READY && world && session::is_host() && !party_room.id && !match_room_.id && !create_party_req &&
        !waiting_host && !switch_at && t >= next_party_try) {
        create_party_req = create_room(kKindParty);
        if (!create_party_req) next_party_try = t + 5;   // refused locally: not again every frame
    }
    if (host_gone_pending) mm_host_gone_now();
    if (leave_to_leader_pending) mm_leave_to_leader_now();
    if (switch_at && t >= switch_at) {
        switch_at = 0;
        log("online: transport switch: the party was told, joining the owner of room %llx", (unsigned long long)waiting_room);
        switch_to_owner(switch_owner, waiting_room, switch_admit);
    }
    mm_check_tick(t);
    if (waiting_host && !admit_armed) {
        // PS3: the reservation answer is due a fixed time after the link to the owner is up (10 s / 6 s)
        const int i = peer_index(host_npid, false);
        if (i >= 0 && peers[size_t(i)].state == U2O_PEER_ACTIVE) {
            admit_armed = true;
            host_deadline = std::min(host_deadline, t + admit_limit);
        }
    }
    if (waiting_host && unreachable.contains(host_npid)) {   // PS3 0x4e9e98: no link to a listed player
        log("online: the room owner %s is on the unreachable list: the join fails at once (room %llx)", host_npid.c_str(),
            (unsigned long long)waiting_room);
        owner_not_reached("room owner unreachable");
    }
    if (waiting_host && t >= host_deadline) {
        log("online: the room owner did not let us in in time (%s): leaving room %llx",
            admit_armed ? "admit limit after the link was up" : "link limit", (unsigned long long)waiting_room);
        owner_not_reached("room owner not reached");
    }
    if (join_page_deadline && t >= join_page_deadline) {
        join_page_deadline = 0;
        const uint32_t top = top_page_id();
        if (top == kPageJoin || joining_page) {   // the host is not in its custom game lobby: we are in its party now
            log("online: no follow from the host (not in its lobby): back to PAGE_MULTIPLAYER, NET_SM 18");
            joining_page = false;
            back_to_multiplayer_page();
        }
    }
    if (party_in_wait && t >= party_in_deadline) {   // the leader went quiet: PS3 members time out themselves
        party_in_wait = false;
        log("online: no word from the leader about the custom game room in %.0f s: back to the page below", kPartyInWait);
        if (match_room_.kind == kKindCustom && !match_room_.owned()) leave_room(match_room_, "the leader went quiet");
        session::online_party_in(false);
    }
    party_owner_tick(t);
    if (match_waiting && !create_fail_why.empty()) {   // create_match_room could not ask for the room
        const std::string why = create_fail_why;
        custom_create_failed(why.c_str());
    }
    room_state();
    if (t >= next_blob_check) {
        next_blob_check = t + 1.0;
        publish_blob(party_room, false);
        publish_blob(match_room_, false);
        if (link_state == U2O_READY) talk_all_tick();
    }
    if (t >= next_rtt_log) {
        next_rtt_log = t + kRttLogPeriod;
        for (Peer &p : peers) {
            uint32_t rtt = 0;
            const int s = api.peer_state(p.npid.c_str(), &rtt);
            if (s == U2O_PEER_INACTIVE && p.state < 0) continue;
            log("online: peer %s: %s, rtt %u ms", p.npid.c_str(), peer_state_name(uint32_t(s)), rtt / 1000);
            note_peer_state(p, s, rtt);
        }
    }
}

bool create_match_room() {
    if (!started) return false;
    if (G<int32_t>(kNetInfo + NI_MODE) != 2) {
        log("online: Start Match in mode %d: no custom game room (matchmaking plays in its own room %llx)",
            G<int32_t>(kNetInfo + NI_MODE), (unsigned long long)match_room_.id);
        return false;
    }
    if (match_room_.owned()) {
        log("online: Start Match: our match room %llx is still open, reused", (unsigned long long)match_room_.id);
        return false;
    }
    // PS3 parity H2 (owner decision): a room that cannot be made ends Start Match (custom_create_failed, next frame).
    match_waiting = true;
    create_fail_why.clear();
    if (link_state != U2O_READY || !world) {
        log("online: Start Match: link %s, world %u: no match room", link_name(link_state), world);
        create_fail_why = "no link to the room service";
        return true;
    }
    if (match_room_.id) leave_room(match_room_, "a match room of our own");
    create_match_req = create_room(kKindCustom);
    if (!create_match_req) {
        log("online: Start Match: the match room request was refused");
        create_fail_why = "the room request was refused";
    }
    return true;
}

uint64_t match_room() { return started ? match_room_.id : 0; }

bool follow_match_room(uint64_t room_id) {
    if (!started || !room_id) return false;
    joining_page = false;   // a join from the list ends here: the host's follow came
    join_page_deadline = 0;
    if (match_room_.id == room_id) {
        log("online: the leader's match room %llx: already a member", (unsigned long long)room_id);
        return false;
    }
    const auto blob = member_blob();
    const uint64_t id = api.join_room(room_id, blob.data(), uint32_t(blob.size()), 0);
    log("online: joining the leader's match room %llx as a member (the transport stays): request %llu",
        (unsigned long long)room_id, (unsigned long long)id);
    if (!id) return false;
    request(id, Pending{Req::JoinMatchMember, room_id, kKindCustom, false, "join (party follow)"});
    // PS3 JOINING 12: wait for the leader's 0x10e (kOpPartyIn) once the whole party is in a custom game room;
    // a matchmaking follow (the vote's match) needs no such wait.
    if (G<int32_t>(kNetInfo + NI_MODE) != 2) return false;
    party_in_wait = true;
    party_in_deadline = now() + kPartyInWait;
    return true;
}

void on_host_reached(uint32_t net_id) {
    if (!started) return;
    if (session::is_host()) {
        log("online: transport: net id %u connected to this host over the online link", net_id);
        return;
    }
    if (waiting_host) {
        waiting_host = false;
        log("online: transport switch done: reached the owner of room %llx, our net id %u", (unsigned long long)waiting_room,
            net_id);
        if (mm_state == Mm::Checking && waiting_room == mm_room_id) {
            mm_state = Mm::Joined;   // PS3 state 9 granted -> JoinRoom -> 17 CLIENT_LOBBY (the host's messages follow)
            log("online: matchmaking: the host of room %llx let us in", (unsigned long long)mm_room_id);
        }
        if (top_page_id() == kPageJoin || joining_page) join_page_deadline = now() + 3.0;   // the host's follow normally comes first
    }
}

bool host_change_expected() { return started && (following_leader || waiting_host || switch_at); }

void on_transport_closed(const char *why) {
    if (!started) return;
    log("online: the transport closed (%s): leaving every room", why ? why : "?");
    leave_room(match_room_, why ? why : "closed");
    leave_room(party_room, why ? why : "closed");
    for (const Peer &p : peers)
        if (p.state == U2O_PEER_ACTIVE || p.state == U2O_PEER_PENDING) disconnect_peer(p.npid);
    for (Peer &p : peers) p.state = -1;
    started = false;
    start_hidden = false;
    waiting_host = following_leader = false;
    switch_at = 0;
    match_waiting = false;   // the match is gone with the transport: nothing to finish
    create_fail_why.clear();
    match_order.clear();
    talk_all.clear();
    talk_all_sent = -1;
    migrating = false;
    promote_at = 0;
    mm_state = Mm::Idle;
    mm_search_req = 0;
    mm_room_id = 0;
    mm_found.clear();
    joining_page = party_in_wait = false;
    join_page_deadline = party_wait_since = 0;
    host_gone_pending = leave_to_leader_pending = false;
    party_owner_tracked.clear();
}

void on_message(uint8_t from, const uint8_t *data, size_t size) {
    if (!started || !size) return;
    if (data[0] == kOpFollowRoom && size >= 9 && from == 0 && !session::is_host()) {
        uint64_t room = 0;
        std::memcpy(&room, data + 1, 8);
        following_leader = true;
        const auto blob = member_blob();
        const uint64_t id = api.join_room(room, blob.data(), uint32_t(blob.size()), 0);
        log("online: the party leader moved to room %llx: following (request %llu)", (unsigned long long)room,
            (unsigned long long)id);
        if (!id) {
            following_leader = false;
            log("online: could not follow the leader (request refused)");
            return;
        }
        const uint32_t kind = size >= 10 && (data[9] == kKindParty || data[9] == kKindMatchmaking) ? data[9] : kKindCustom;
        request(id, Pending{Req::JoinLeader, room, kind, false, "join (follow the leader)"});
    } else if (data[0] == kOpPartyIn && size >= 2 && from == 0 && !session::is_host()) {
        if (!party_in_wait && data[1]) return;   // not waiting (a late "in", or a follow that needed no wait)
        party_in_wait = false;
        log("online: the leader says the party is %s the custom game room", data[1] ? "in" : "not in");
        if (!data[1] && match_room_.kind == kKindCustom && !match_room_.owned()) leave_room(match_room_, "the party went back");
        session::online_party_in(data[1] != 0);
    } else if (data[0] == kOpJoinResult && size >= 2 && session::is_host()) {
        if (!data[1] && match_room_.owned() && match_room_.kind == kKindCustom && party_wait_since)
            abort_custom_lobby("a member's join was refused");
    } else if (data[0] == kOpTalkAll && size >= 2 && session::is_host()) {
        net::Address a;
        const std::string npid = session::member_address(uint32_t(from) + 1, a) ? npid_at(a) : std::string();
        if (!npid.empty()) talk_all[npid] = data[1] != 0;
    } else if (data[0] == kOpOrder && size >= 2 && from == 0 && !session::is_host()) {
        match_order.assign(data + 2, data + std::min<size_t>(size, size_t(2) + data[1]));
        std::string order;
        for (uint32_t n : match_order) order += (order.empty() ? "" : ", ") + std::to_string(n);
        log("online: the host's owner succession for the match: net ids %s", order.empty() ? "-" : order.c_str());
    } else if (data[0] == kOpPromote && size >= 9 && from == 0 && !session::is_host()) {
        std::memcpy(&promote_room, data + 1, 8);
        promote_target.assign(reinterpret_cast<const char *>(data + 9), std::min<size_t>(size - 9, 16));
        log("online: the leader promoted %s in room %llx: the party moves to that player", promote_target.c_str(),
            (unsigned long long)promote_room);
    }
}

bool open_join_list() {
    if (!started) return false;
    log("online: Invites -> join list (PAGE_MULTIPLAYER_JOIN, BUTTON_MULTIPLAYER_JOIN 0x6095d0)");
    G<uint8_t>(kMatch + M_SEARCH_DONE) = 0;
    G<int32_t>(kMatch + M_SEARCH_RESULT) = 0;
    found.clear();
    guest<void(U2MP_GUEST *)(void *, void *, void *)>(fButtonJoin)(nullptr, nullptr, nullptr);   // page, NET_SM 5
    start_search();
    return true;
}

// The join list: the invites first (PS3: the NP Basic invite inbox of BUTTON_MULTIPLAYER_INVITES 0x440948),
// then the rooms of the search.
int join_rows() {
    if (!started) return -1;
    return int(invites.size()) + (found.empty() ? 1 : int(found.size()));
}

const char *join_row(int row) {
    if (!started || row < 0) return "";
    if (size_t(row) < invites.size()) {
        const Invite &v = invites[size_t(row)];
        std::snprintf(invite_text[row], sizeof invite_text[row], "Invite from %.40s - %s (%u/%u)", v.npid.c_str(),
                      v.kind == kKindParty ? "Party" : v.kind == kKindMatchmaking ? "Matchmaking" : "Custom Game",
                      unsigned(v.members), unsigned(v.max));
        return invite_text[row];
    }
    row -= int(invites.size());
    if (found.empty()) return searching ? "Searching for games..." : "No open games found. Select to search again.";
    if (size_t(row) >= found.size() || row >= U2O_MAX_ROOMS) return "";
    const U2oRoomExt &x = found[size_t(row)];
    std::snprintf(row_text[row], sizeof row_text[row], "%.40s - %s (%u/%u)", x.owner_name,
                  room_kind(x) == kKindParty ? "Party" : "Custom Game", unsigned(x.cur_members),
                  unsigned(x.max_slot));
    return row_text[row];
}

const char *join_title() { return "Online Games"; }

bool select_join(int row) {
    if (!started) return false;
    if (row >= 0 && size_t(row) < invites.size()) {
        const Invite v = invites[size_t(row)];
        invites.erase(invites.begin() + row);
        if (!join_by_id(v.room, v.kind, v.npid, v.members, v.max) && top_page_id() == kPageJoin) search_failed();
        return true;
    }
    row -= int(invites.size());
    if (found.empty()) {
        if (!searching) start_search();
        return true;
    }
    if (row < 0 || size_t(row) >= found.size()) {
        log("online: join list row %d of %zu: nothing there", row, found.size());
        return true;
    }
    const U2oRoomExt &x = found[size_t(row)];
    const bool leader = session::is_host();
    const int party = leader ? std::max(1, session::member_count()) : 1;
    const int free_slots = int(x.max_slot) - int(x.cur_members);
    const uint32_t kind = room_kind(x) == kKindParty ? kKindParty : kKindCustom;
    // PS3 SELECT_JOIN_GAME 0x465808: "Selected game %i", "room %s"; the Joining page comes at once (NET_SM 12 here:
    // the intact page 0x609790 then waits for 0x10e, which the host's follow or a failure posts).
    log("online: Selected game %d, room %llx of %s (Distance to Host: no location on PC)", row, (unsigned long long)x.room_id,
        x.owner_name);
    if (kind == kKindCustom) {
        show_page(kPageJoining);
        G<int32_t>(kNetState) = 12;
        joining_page = true;
    }
    // PS3 RESERVE_SLOTS (mailbox 0x100 / 0x101 with the host's slot manager 0x4da278): the whole party
    // must fit; first by the room data of the search, then by the room service's count at our join (join_succeeded).
    if (free_slots < party) {
        log("online: join refused: room %llx has %d free slot(s), the party needs %d", (unsigned long long)x.room_id,
            free_slots, party);
        if (joining_page) join_page_fail("no room for the party");
        return true;
    }
    const auto blob = member_blob();
    const uint64_t id = api.join_room(x.room_id, blob.data(), uint32_t(blob.size()), 0);
    log("online: joining %s room %llx of %s (%d free slot(s), party of %d): request %llu", kind_name(kind),
        (unsigned long long)x.room_id, x.owner_name, free_slots, party, (unsigned long long)id);
    if (!id) {
        if (joining_page) join_page_fail("the join request was refused");
        else if (top_page_id() == kPageJoin) search_failed();
        return true;
    }
    request(id, Pending{Req::JoinList, x.room_id, kind, leader && party > 1, "join"});
    return true;
}

void member_data_changed() {
    if (!started) return;
    publish_blob(party_room, false);
    publish_blob(match_room_, false);
}


// ---- host migration -------------------------------------------------------------------------------
bool on_host_lost() {
    if (!started || link_state != U2O_READY) return false;
    // PS3 0x4b0a58 state 17 (owner decision F10): the host of a stranger's matchmaking room went away outside a
    // match: leave the room and search again (a party member goes back to its leader). No lobby host migration.
    if (match_room_.kind == kKindMatchmaking && !match_room_.owned() && !in_match) {
        mm_host_gone("the host is gone", false);
        return true;
    }
    log("online: host migration disabled: the host left, the session ends");
    return false;
}

// ---- promote (PS3 GrantRoomOwner 0x4d3a10; RPCN: owner succession + leave + join again) -------------
bool promote(uint32_t net_id) {
    if (!started || !session::is_host() || link_state != U2O_READY) return false;
    net::Address a;
    if (!session::member_address(net_id, a)) return false;
    const Peer *p = peer_at(a);
    if (!p) return false;
    Room *r = party_room.owned() ? &party_room : match_room_.owned() ? &match_room_ : nullptr;
    if (!r) return false;
    uint16_t mid = 0;
    for (const Member &m : r->members)
        if (m.npid == p->npid) mid = m.id;
    if (!mid) {
        log("online: promote: %s is not in room %llx", p->npid.c_str(), (unsigned long long)r->id);
        return false;
    }
    const uint64_t id = api.set_owner_rank(r->id, &mid, 1);
    request(id, Pending{Req::OwnerRank, r->id, r->kind, false, "owner succession"});
    std::vector<uint8_t> b{session::kOnlineMessage, kOpPromote, 0, 0, 0, 0, 0, 0, 0, 0};
    std::memcpy(b.data() + 2, &r->id, 8);
    b.insert(b.end(), p->npid.begin(), p->npid.end());
    session::send_message(-1, b);
    promote_room = r->id;
    promote_at = now() + kPromoteDelay;
    log("online: promote: %s (member %u) leads room %llx next (request %llu)", p->npid.c_str(), unsigned(mid),
        (unsigned long long)r->id, (unsigned long long)id);
    return id != 0;
}

// ---- friends ------------------------------------------------------------------------------------
// Rows: the friends (online first), then friend requests to us, then ours still waiting.
int friend_count() {
    if (!api.on || link_state != U2O_READY) return 0;
    return int(std::min<size_t>(friend_list.size() + requests_in.size() + requests_out.size(), size_t(kMaxFriendRows)));
}

int friend_kind(int i) {
    if (i < 0) return -1;
    if (size_t(i) < friend_list.size()) return kRowFriend;
    i -= int(friend_list.size());
    if (size_t(i) < requests_in.size()) return kRowRequestIn;
    i -= int(requests_in.size());
    return size_t(i) < requests_out.size() ? kRowRequestOut : -1;
}

std::string request_name(int i) {
    i -= int(friend_list.size());
    if (i >= 0 && size_t(i) < requests_in.size()) return requests_in[size_t(i)];
    i -= int(requests_in.size());
    return i >= 0 && size_t(i) < requests_out.size() ? requests_out[size_t(i)] : std::string();
}

const char *friend_row(int i) {
    if (i < 0 || i >= kMaxFriendRows) return "";
    if (friend_kind(i) == kRowRequestIn) {
        std::snprintf(friend_text[i], sizeof friend_text[i], "%.20s - Friend request", request_name(i).c_str());
        return friend_text[i];
    }
    if (friend_kind(i) == kRowRequestOut) {
        std::snprintf(friend_text[i], sizeof friend_text[i], "%.20s - Request sent", request_name(i).c_str());
        return friend_text[i];
    }
    if (size_t(i) >= friend_list.size()) return "";
    const U2oFriend &f = friend_list[size_t(i)];
    const char *state = !f.online ? "Offline" : f.same_game && f.status[0] ? f.status : f.title[0] ? f.title : "Online";
    std::snprintf(friend_text[i], sizeof friend_text[i], "%.20s - %.90s", f.npid, state);
    return friend_text[i];
}

std::string friend_name(int i) {
    if (i < 0) return {};
    if (size_t(i) >= friend_list.size()) return request_name(i);
    return npid_text(friend_list[size_t(i)].npid);
}

// Friend requests (RPCN AddFriend / RemoveFriend through the launcher's session).
bool friend_add(const std::string &npid) {
    if (!api.add_friend || link_state != U2O_READY || npid.empty()) return false;
    const uint64_t id = api.add_friend(npid.c_str());
    log("online: friends: add %s (request %llu)", npid.c_str(), (unsigned long long)id);
    request(id, Pending{Req::Message, 0, 0, false, "add friend"});
    friends_dirty = true;
    return id != 0;
}

bool accept_request(int i) {
    const std::string n = friend_name(i);
    const bool ok = friend_kind(i) == kRowRequestIn && friend_add(n);
    notice(ok ? n + " is your friend now." : "The friend request could not be answered.");
    return ok;
}

bool remove_friend(int i) {
    const std::string n = friend_name(i);
    const int kind = friend_kind(i);
    if (kind < 0 || !api.remove_friend || link_state != U2O_READY) return false;
    const uint64_t id = api.remove_friend(n.c_str());
    log("online: friends: remove %s (%s, request %llu)", n.c_str(),
        kind == kRowFriend ? "friend" : kind == kRowRequestIn ? "declined" : "request cancelled", (unsigned long long)id);
    request(id, Pending{Req::Message, 0, 0, false, "remove friend"});
    friends_dirty = true;
    notice(!id ? "That did not work." : kind == kRowFriend ? n + " was removed from your friends."
                                     : kind == kRowRequestIn ? "Friend request declined." : "Friend request cancelled.");
    return id != 0;
}

std::vector<std::string> friend_npids(size_t max) {
    std::vector<std::string> out;
    for (size_t i = 0; i < friend_list.size() && i < max; ++i) out.push_back(npid_text(friend_list[i].npid));
    return out;
}

std::string profile_text(int i) {
    const int kind = friend_kind(i);
    const std::string n = friend_name(i);
    if (kind == kRowRequestIn) return n + " wants to be your friend.";
    if (kind == kRowRequestOut) return n + " has not answered your friend request yet.";
    if (kind != kRowFriend) return {};
    const U2oFriend &f = friend_list[size_t(i)];
    std::string t = n + ": " + (f.online ? "Online" : "Offline");
    if (f.online && f.title[0]) t += std::string(". ") + f.title;
    if (f.online && f.status[0]) t += std::string(". ") + f.status;
    pres::OldRoom hint;
    if (f.online && f.comment[0] && !pres::parse_join_hint(f.comment, sizeof f.comment, hint))
        t += std::string(". ") + f.comment;
    FriendPresence p;
    if (friend_presence(f, p) && p.b.key != room_key()) t += ". Plays another version of the game.";
    return t;
}

bool friend_online(int i) { return i >= 0 && size_t(i) < friend_list.size() && friend_list[size_t(i)].online; }

bool friend_view(int i, FriendView &v) {
    if (i < 0 || size_t(i) >= friend_list.size() || friend_kind(i) != kRowFriend) return false;
    const U2oFriend &f = friend_list[size_t(i)];
    v = FriendView{};
    v.npid = npid_text(f.npid);
    v.online = f.online != 0;
    v.this_game = v.online && friend_group(f) == 0;
    v.party_member = party_room.id && in_room(party_room, v.npid);
    FriendPresence p;
    const bool data = friend_presence(f, p);
    v.other_build = data && p.b.key != room_key();
    // PS3 takes the data only when its signature is our build's (0x48f020): another build reads as "this game".
    if (data && p.b.key == room_key()) {
        v.multiplayer = true;
        v.details = p.details;
        v.activity = !p.details                         ? 0
                     : p.b.activity == pres::kActMatch  ? 3
                     : p.b.activity == pres::kActFilm   ? 2
                     : p.b.activity == pres::kActLobby  ? 1
                                                        : 0;
        v.mode = p.b.mode;
        v.party = p.b.party;
        v.difficulty = p.b.difficulty;
        v.net_game = p.b.net_game;
        v.match_end = p.b.match_end;
    }
    return true;
}

bool friend_list_ready() { return api.on && link_state == U2O_READY && friends_loaded; }

bool is_friend(const std::string &npid) {
    if (!friend_list_ready()) return false;
    for (const U2oFriend &f : friend_list)
        if (npid_text(f.npid) == npid) return true;
    return false;
}

// PS3 parity G4 (0x490c28): nothing while the list is not known or for no friend; a new request when the friend's
// presence (this build) names a party room, the cache is another friend's or older than 5 s, and none runs.
bool friend_party(int i, std::vector<std::string> &names) {
    names.clear();
    if (!friend_list_ready() || friend_kind(i) != kRowFriend) return false;
    const U2oFriend &f = friend_list[size_t(i)];
    const std::string npid = npid_text(f.npid);
    const double t = now();
    FriendPresence p;
    if (api.members_external && !party_list_req && our_presence(f, p) && p.b.party_room &&
        (party_list.npid != npid || t >= party_list.at + kPartyListLife)) {
        party_list_req = api.members_external(p.b.party_room);
        log("online: friends: asking for %s's party (room %llx, request %llu)", npid.c_str(),
            (unsigned long long)p.b.party_room, (unsigned long long)party_list_req);
        request(party_list_req, Pending{Req::PartyMembers, p.b.party_room, 0, false, "party members"});
        if (party_list_req) party_list_asked = PartyList{npid, t, {}};
    }
    if (party_list.npid != npid) return false;
    names = party_list.names;
    return true;
}

int friend_index(const std::string &name) {
    const int n = friend_count();
    for (int i = 0; i < n; ++i)
        if (friend_name(i) == name) return i;
    return -1;
}

const char *request_state(int i) {
    const int kind = friend_kind(i);
    return kind == kRowRequestIn ? "Friend request" : kind == kRowRequestOut ? "Request sent" : "";
}

void refresh_friend_list() {
    if (api.on && link_state == U2O_READY) friends_dirty = true;
}

bool friend_joinable(int i) {
    FriendPresence p;
    pres::OldRoom r;
    return i >= 0 && size_t(i) < friend_list.size() && friend_presence(friend_list[size_t(i)], p) &&
           join_room_of(p, r) && !room_by_id(r.room);
}

bool join_friend(int i) {
    FriendPresence p;
    pres::OldRoom r;
    if (i < 0 || size_t(i) >= friend_list.size() || !friend_presence(friend_list[size_t(i)], p) || !join_room_of(p, r)) {
        notice("There is no game to join.");
        return false;
    }
    return join_by_id(r.room, r.kind, npid_text(friend_list[size_t(i)].npid), r.members, r.max);
}

bool invite_friend(int i) {
    if (i < 0 || size_t(i) >= friend_list.size()) return false;
    const std::string to = npid_text(friend_list[size_t(i)].npid);
    if (!started || link_state != U2O_READY) {
        notice("Online play is not connected.");
        return false;
    }
    pres::OldRoom d;
    const char *what = "";
    const Room *r = join_target(d.state, what);
    if (!r || d.state == kOldMatch) {
        notice("There is no party to invite to yet. Try again in a moment.");
        return false;
    }
    d.key = room_key();
    d.kind = r->kind;
    d.room = r->id;
    d.members = uint8_t(std::min<size_t>(r->members.size(), 255));
    d.max = uint8_t(r->max_slot ? r->max_slot : kPartySlots);
    U2oMessage m{};
    m.main_type = U2O_MSG_INVITE;
    std::snprintf(m.subject, sizeof m.subject, "Uncharted 2 invite");
    std::snprintf(m.body, sizeof m.body, "%s invites you to play Uncharted 2: Among Thieves.", session::local_name().c_str());
    m.data_len = uint32_t(pres::encode_old_room(d, m.data));
    const char *npids[] = {to.c_str()};
    const uint64_t id = api.send_message(npids, 1, &m);
    log("online: invite to %s: %s room %llx (request %llu)", to.c_str(), kind_name(d.kind), (unsigned long long)d.room,
        (unsigned long long)id);
    request(id, Pending{Req::Message, d.room, d.kind, false, "invite"});
    notice(id ? "Invite sent to " + to + "." : "The invite could not be sent.");
    return id != 0;
}

int invite_count() { return started ? int(invites.size()) : 0; }

// ---- stranger matchmaking -------------------------------------------------------------------------
bool mm_online() { return started && link_state == U2O_READY && world != 0; }

void mm_begin(uint32_t playlist, uint32_t playlist_index, const MmCriteria &criteria) {
    if (!mm_online()) return;
    mm_playlist_index = playlist_index;
    mm_playlist_id = playlist;
    mm_criteria = criteria;
    if (match_room_.owned() && match_room_.kind == kKindMatchmaking) {   // our gather room is still open
        mm_state = Mm::Hosting;
        return;
    }
    // PS3 0x4b0f48 -> SearchRoom 0x4d3c50: 0x4C == key (version + playlist), open rooms only. PS3
    // matchmaking rooms carry no 0x52, so the key is the only filter. The criteria row's distance and region
    // parts need a location, which PC has not: every pass searches the same rooms, the rank and ping parts differ.
    // In a playlist with map-pack maps the key carries the party's common map packs (parity H5): parties only meet
    // rooms made by parties with the same common packs.
    const uint32_t key = match_key(playlist_index, session::mm_pack_mask());
    log("online: matchmaking: criteria %d: ping limit %u ms, %s (no distance on PC)", criteria.pass, criteria.ping_limit_ms,
        criteria.rank_only ? "rank within 20" : "any rank");
    U2oSearch s{};
    s.flag_filter = kFlagHidden | kFlagClosed;
    s.ints[0] = U2oIntFilter{1, 0, A_KEY, key};
    s.int_count = 1;
    s.start = 1;
    s.max = U2O_MAX_ROOMS;
    mm_search_req = api.search_rooms(world, &s);
    log("online: matchmaking: searching for playlist 0x%x (index %u), key 0x%x (request %llu)", playlist, playlist_index, key,
        (unsigned long long)mm_search_req);
    if (!mm_search_req) {
        mm_state = Mm::Failed;
        return;
    }
    request(mm_search_req, Pending{Req::MmSearch, 0, kKindMatchmaking, false, "matchmaking search"});
    mm_state = Mm::Searching;
}

void mm_create() {
    if (!mm_online()) return;
    log("online: matchmaking: no room after the last criteria: creating a room of our own (PS3 GATHER)");
    mm_state = create_room(kKindMatchmaking) ? Mm::Creating : Mm::Failed;
}

int mm_step() {
    switch (mm_state) {
    case Mm::Idle: return kMmIdle;
    case Mm::Searching: case Mm::Joining: case Mm::Checking: case Mm::Creating: return kMmBusy;
    case Mm::Hosting: return kMmHosting;
    case Mm::Joined: return kMmJoined;
    case Mm::NoRoom: return kMmNoRoom;
    default: return kMmFailed;
    }
}

void mm_reset() {
    mm_state = Mm::Idle;
    mm_room_id = 0;
    mm_found.clear();
}

int mm_pending_members() {
    if (!match_room_.owned() || match_room_.kind != kKindMatchmaking) return 0;
    const double t = now();
    int n = 0;
    for (const Member &m : match_room_.members)
        if (m.id != match_room_.my_id && !m.npid.empty() && m.joined_at && t - m.joined_at < kMmAdmit &&
            !session::net_id_of(m.npid))
            ++n;
    return n;
}

int party_missing() {
    if (!started || !party_room.owned()) return 0;
    int n = 0;
    for (const Member &m : party_room.members)
        if (m.id != party_room.my_id && !m.npid.empty() && !session::net_id_of(m.npid)) ++n;
    return n;
}

bool host_in_party() {
    if (!started || session::is_host() || host_npid.empty() || host_npid == my_npid) return true;
    return party_room.id && in_room(party_room, host_npid);
}

bool party_member() { return started && party_room.id && !party_room.owned(); }

void mm_host_gone(const char *why, bool bad_room) {
    if (!started || host_gone_pending) return;
    host_gone_pending = true;
    host_gone_bad = bad_room;
    host_gone_why = why ? why : "?";
}

void mm_leave_to_leader() {
    if (started) leave_to_leader_pending = true;
}

bool drop_out_of_party(const char *why) {
    if (!started) return false;
    log("online: leaving the party (%s)", why);
    mm_state = Mm::Idle;
    start_hidden = false;
    if (match_room_.id) leave_room(match_room_, why);
    if (party_room.id && !party_room.owned()) leave_room(party_room, why);
    else if (party_room.id && party_room.members.size() > 1) leave_room(party_room, why);
    own_party(why);
    return true;
}

void mm_research() {
    if (match_room_.owned() && match_room_.kind == kKindMatchmaking) {
        mm_bad_rooms.push_back(match_room_.id);
        leave_room(match_room_, "nobody came: searching again (PS3 'time out %i players')");
    }
    mm_state = Mm::Idle;
}

void mm_leave() {
    mm_state = Mm::Idle;
    start_hidden = false;   // back in the party without a match: the party room shows again
    if (!started || !match_room_.id || match_room_.kind != kKindMatchmaking) return;
    const bool stranger = !match_room_.owned() && !(party_room.id && in_room(party_room, host_npid));
    leave_room(match_room_, "left matchmaking");
    if (stranger) own_party("left a stranger's matchmaking");
}

int mm_players() { return match_room_.kind == kKindMatchmaking ? int(match_room_.members.size()) : 0; }

// ---- PS3 parity group E (spec parity-e-flow2.md 2.4, 2.6) ----
uint64_t party_room_id() { return started ? party_room.id : 0; }
int party_size() { return started && party_room.id ? std::max<int>(1, int(party_room.members.size())) : 1; }

std::vector<std::string> take_led_party() {
    std::vector<std::string> out;
    out.swap(led_party);
    return out;
}

bool party_up_join(uint64_t room) {
    if (!started || !room || room == party_room.id) return false;
    if (link_state != U2O_READY) return false;
    // Each player of the group joins by itself (the host told every one of them), so nobody leads others here.
    const auto blob = member_blob();
    const uint64_t id = api.join_room(room, blob.data(), uint32_t(blob.size()), 0);
    log("online: Party Up: joining party room %llx (PS3 0x119): request %llu", (unsigned long long)room,
        (unsigned long long)id);
    if (!id) return false;
    request(id, Pending{Req::JoinList, room, kKindParty, false, "Party Up join"});
    return true;
}

void reservation_refused() {
    if (!started || !match_room_.id || match_room_.owned()) return;
    if (match_room_.kind == kKindMatchmaking) {   // PS3 JOINING state 9 refusal in matchmaking: the next room
        mm_host_gone("the room owner has no slots for our party (0x101)", true);
        return;
    }
    leave_room(match_room_, "no slots reserved for our party");
    join_page_fail("the room owner has no slots for our party (0x101)");
    own_party("no slots reserved for our party");
}

// The matchmaking rows (session.cpp, PS3 slot list 0x4b0008).
bool in_own_party(const std::string &npid) {
    return !npid.empty() && (npid == my_npid || (party_room.id && in_room(party_room, npid)));
}

uint64_t own_party_id() { return party_room.id && party_room.members.size() >= 2 ? party_room.id : 0; }

uint64_t member_party_id(const std::string &npid) {
    for (const Member &m : match_room_.members)
        if (m.npid == npid) return m.party;
    return 0;
}

bool peer_rtt(const std::string &npid, uint32_t &rtt_ms, bool &up) {
    const int i = npid.empty() ? -1 : peer_index(npid, false);
    if (i < 0) return false;
    rtt_ms = peers[size_t(i)].rtt_us / 1000;
    up = peers[size_t(i)].state == U2O_PEER_ACTIVE;
    return true;
}

int32_t peer_link_state(const std::string &npid) {
    const int i = npid.empty() ? -1 : peer_index(npid, false);
    return i < 0 ? -1 : peers[size_t(i)].state;
}

// The match room's real slot count (parity H1: 3 for a co-op custom game, the playlist's maximum in matchmaking);
// without a room the size a matchmaking room of the chosen playlist gets, else the party's 10.
int mm_slots() {
    if (!mm_online()) return 0;
    if (match_room_.id && match_room_.max_slot) return match_room_.max_slot;
    // on the matchmaking page: the size our room for the chosen playlist would have
    return G<int32_t>(kNetInfo + NI_MODE) == 1 ? int(mm_room_slots(G<uint32_t>(kNpd + 0x104))) : int(kPartySlots);
}
std::string my_npid_text() { return my_npid; }

std::string room_member_name(uint32_t net_id) {
    for (const Room *r : {&party_room, &match_room_})
        for (const Member &m : r->members)
            if (m.id == net_id) return m.name;
    return {};
}

std::string npid_at(const net::Address &a) {
    const Peer *p = peer_at(a);
    return p ? p->npid : std::string();
}

// ---- rankings --------------------------------------------------------------------------------------
bool board_request(uint32_t board, int mode, uint32_t start, uint32_t count) {
    if (!api.get_scores || link_state != U2O_READY) return false;
    board_view = BoardView{};
    board_view.board = board;
    board_view.mode = mode;
    board_view.req = api.get_scores(board, mode, start, count);
    board_view.loading = board_view.req != 0;
    board_view.failed = !board_view.req;
    board_view.deadline = now() + kBoardTimeout;
    request(board_view.req, Pending{Req::ScoreGet, board, 0, false, "scores"});
    log("online: rankings: board %u mode %d from %u (request %llu)", board, mode, start, (unsigned long long)board_view.req);
    return board_view.req != 0;
}

int board_state() {
    board_expire();
    return board_view.loading ? 1 : board_view.failed ? -1 : 0;
}
int board_rows() { return int(board_view.rows.size()); }
uint32_t board_total() { return board_view.total; }
const U2oScore *board_row(int i) { return i >= 0 && size_t(i) < board_view.rows.size() ? &board_view.rows[size_t(i)] : nullptr; }

bool record_score(uint32_t board, int64_t score, const char *comment, const uint8_t *info, uint32_t info_len) {
    if (!api.record_score || link_state != U2O_READY) return false;
    const auto posts = std::count_if(pending.begin(), pending.end(),
                                     [](const auto &kv) { return kv.second.kind == Req::ScoreRecord; });
    if (posts >= kMaxScorePosts) {
        log("rankings: 4 scores pending, score %lld for board %u dropped", (long long)score, board);
        return false;
    }
    const uint64_t id = api.record_score(board, score, comment, info, info_len);
    log("online: rankings: recording %lld on board %u (request %llu)", (long long)score, board, (unsigned long long)id);
    request(id, Pending{Req::ScoreRecord, board, 0, false, "record score"});
    return id != 0;
}

bool linked() { return api.on && link_state == U2O_READY; }

// ---- cinema files ------------------------------------------------------------------------------
bool send_cinema(const std::string &npid) {
    if (!linked() || !api.send_message) return false;
    std::vector<uint8_t> file;
    std::string name;
    if (!cinema::read_selected(file, name)) {
        notice("That cinema file could not be read.");
        return false;
    }
    if (file.size() + sizeof(CinemaMeta) > U2O_MAX_BLOB) {
        notice("That cinema file is too large to send (" + std::to_string(file.size() >> 20) + " MB).");
        return false;
    }
    CinemaMeta meta{kCinemaMagic, room_key(), {}, uint32_t(file.size()), 0};
    std::snprintf(meta.name, sizeof meta.name, "%s", name.c_str());
    std::vector<uint8_t> blob(sizeof meta);
    std::memcpy(blob.data(), &meta, sizeof meta);
    blob.insert(blob.end(), file.begin(), file.end());
    U2oMessage m{};
    m.main_type = U2O_MSG_CUSTOM_DATA;   // PS3 mainType 4
    std::snprintf(m.subject, sizeof m.subject, "Cinema File");
    std::snprintf(m.body, sizeof m.body, "Check out this Uncharted 2 Cinema file!");
    m.blob = blob.data();
    m.blob_len = uint32_t(blob.size());
    const char *to[] = {npid.c_str()};
    const uint64_t id = api.send_message(to, 1, &m);   // the program copies the attachment
    log("online: cinema %s (%zu bytes) to %s (request %llu)", name.c_str(), file.size(), npid.c_str(), (unsigned long long)id);
    request(id, Pending{Req::Message, 0, 0, false, "cinema file"});
    notice(id ? "Cinema file sent to " + npid + "." : "The cinema file could not be sent.");
    return id != 0;
}

int cinema_received() { return cinemas_received; }

// ---- Start Match room hide (PS3 0x43aab0 / 0x439a88 / 0x439b04 -> 0x4d5338) --------------------------------
void hide_for_start(bool on) {
    if (!started || start_hidden == on) return;   // LAN: no room to hide
    start_hidden = on;
    log("online: Start Match: the party room is %s", on ? "hidden for the countdown" : "shown again (cancelled)");
    room_state();   // at once, not on the next frame
}

// ---- friends stats ticker (PS3 0x446c60, 0x43b30c, 0x43b290) ------------------------------------------------
namespace {
constexpr size_t kTickerLimit = 511;   // 0x43b290: an append must leave the text below 511 characters
constexpr int kTickerFriends = 5, kTickerScan = 101;
constexpr uint32_t kTickerCategory[7] = {0xc0cd1b00u, 0x49f5e231u, 0xb9602dbbu, 0x966fa9aeu,
                                          0xcd3394e9u, 0x99e71e1bu, 0x780b62e0u};
constexpr uint32_t kCurrencyPrefix = 0x9b5b3c66u;

const char *ticker_game_text(uint32_t id) {
    const char *t = guest<const char *(U2MP_GUEST *)(void *, uint32_t)>(0xcddc00)(image.base + 0x1066660, id);
    return t ? t : "";
}

bool ticker_append(std::string &out, const std::string &s) {
    if (out.size() + s.size() >= kTickerLimit) return false;
    out += s;
    return true;
}

// PS3 0x43b30c: block +0x30 cash, +0x38 kills, +0x40 wins, +0x44 losses, +0x3c deaths, +0x48 headshots, time played
// = +0x4c plus +0x50.
uint64_t ticker_value(const pres::Block &d, int category) {
    switch (category) {
    case 0: return d.cash;
    case 1: return d.kills;
    case 2: return d.wins;
    case 3: return d.losses;
    case 4: return d.deaths;
    case 5: return d.headshots;
    case 6: return uint64_t(d.time_a) + d.time_b;
    default: return 0;
    }
}
}  // namespace

std::string ticker_time(uint64_t seconds) {
    const uint64_t days = seconds / 86400, hours = seconds / 3600 % 24, minutes = seconds / 60 % 60;
    char b[64];
    if (days) std::snprintf(b, sizeof b, "%llud %lluh %llum", (unsigned long long)days, (unsigned long long)hours,
                            (unsigned long long)minutes);
    else if (seconds >= 3600) std::snprintf(b, sizeof b, "%lluh %llum", (unsigned long long)hours, (unsigned long long)minutes);
    else std::snprintf(b, sizeof b, "%llum", (unsigned long long)minutes);
    return b;
}

bool ticker_append_entry(std::string &out, const std::string &name, int category, uint64_t value) {
    std::string v;
    if (category == 0) v = std::string(ticker_game_text(kCurrencyPrefix)) + std::to_string(value);
    else if (category >= 1 && category <= 5) v = std::to_string(value);
    else if (category == 6) v = ticker_time(value);
    else v = "Unknown Value!";
    const size_t before = out.size();
    if (ticker_append(out, name) && ticker_append(out, ": |@CFFF0C828|") && ticker_append(out, v) &&
        ticker_append(out, "|@CFF999999|     "))
        return true;
    out.resize(before);
    return false;
}

std::string stats_ticker_text() {
    const int category = std::rand() % 7;
    std::string out = std::string("|@CFFFFFFFF|") + ticker_game_text(kTickerCategory[category]) + ":|@CFF999999|      ";
    // PS3 0x446c60: the local player's own entry comes first (also on LAN, where it is the only one). Headshots:
    // no NetPlayerData field is known for them; the local store's career count (as in our presence).
    static constexpr uint64_t kOwnValue[7] = {0xd8, 0x184, 0x18c, 0x190, 0x188, 0, 0x198};
    const uint64_t own = kOwnValue[category] ? G<uint32_t>(kNpd + kOwnValue[category])
                         : category == 5     ? progression_headshots()
                                             : 0;
    ticker_append_entry(out, linked() && !my_npid.empty() ? my_npid : session::local_name(), category, own);
    if (!linked()) return out;
    std::vector<std::pair<std::string, uint64_t>> pool;
    for (size_t i = 0; i < friend_list.size() && i < size_t(kTickerScan); ++i) {
        // PS3: every friend whose block is valid (in this build's multiplayer); old PC data only from version 3 on.
        FriendPresence p;
        if (our_presence(friend_list[i], p) && (!p.join.version || p.join.version >= 3))
            pool.emplace_back(npid_text(friend_list[i].npid), ticker_value(p.b, category));
    }
    for (int n = 0; n < kTickerFriends && !pool.empty(); ++n) {
        const size_t pick = size_t(std::rand()) % pool.size();
        if (!ticker_append_entry(out, pool[pick].first, category, pool[pick].second)) break;
        pool.erase(pool.begin() + std::ptrdiff_t(pick));
    }
    return out;
}

}  // namespace u2mp::online
