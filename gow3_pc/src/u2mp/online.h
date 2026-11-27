// u2mp online: Uncharted 2 online play over the room service of u2_online.exe (u2_online_abi.h, through
// online_link.cpp), the way the PS3 game used NP Matching2 rooms and NP Signaling. online.cpp implements it;
// u2mp/docs/PORT-ONLINE.md describes the flow. Every function here is a no-op (false / 0) while online is
// off: no online program (UNCHARTED_U2MP_ONLINE_EXE), no relay (UNCHARTED_U2MP_RELAY), or a LAN join address (UNCHARTED_U2MP_JOIN).
// Then the session layer plays over LAN exactly as before.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "net.h"
#include "u2_online_abi.h"

namespace u2mp::online {

// ---- called by the session layer (session.cpp) ----
// Multiplayer was entered: starts the online program and opens the link (once), and returns the link the transport
// hosts the party on. NULL: online is off, use LAN.
net::Link *start();
// True while the session's transport runs over the online link.
bool active();
// Once per game frame: room service events, link state, room state (hide / 0x53), timers.
void frame();
// Start Match in a custom game (host): makes the match room (the game type's slot count, session::custom_room_slots).
// True: session::online_match_ready() follows when the room exists, or session::online_match_abort() when it cannot be
// made (error, no answer in 20 s, no link / world, no slot count: PS3 parity H2); false: go on at once (no room
// needed or online off).
bool create_match_room();
// The match room this PC is in (0 none): sent with the follow so that party members join it too.
uint64_t match_room();
// A client got the host's follow carrying the match room id: join that room as a member. True: a custom game
// join was started and the leader tells when the whole party is in (PS3 0x10e after 0x4c3b58), so the caller
// must not post 0x10e itself; session::online_party_in() follows.
bool follow_match_room(uint64_t room_id);
// The transport reached the room owner (client), or a peer joined / left it (host).
void on_host_reached(uint32_t net_id);
// The transport lost its host while online is switching it to another room on purpose (follow).
bool host_change_expected();
// The session closed the transport (kicked, host lost, Multiplayer left): leave every room.
void on_transport_closed(const char *why);
// A session message of type session kOnlineMessage (from transport slot `from`).
void on_message(uint8_t from, const uint8_t *data, size_t size);
// Party leader, Start Match (PS3 0x43aab0 / 0x4d5338): true hides (closes) the owned party room from the
// countdown on, false shows it again (the leader cancelled, 0x439a88 / 0x439b04). The match's own hide takes
// over once it runs; leaving matchmaking or the transport closing also ends this hide.
void hide_for_start(bool on);

// ---- menus ----
// BUTTON_MULTIPLAYER_INVITES while online: opens the join list (PAGE_MULTIPLAYER_JOIN) and searches.
bool open_join_list();
// The join list (menus.cpp cond 3): row count and texts; -1 rows = online off (the LAN answer applies).
int join_rows();
const char *join_row(int row);
const char *join_title();
// BUTTON_MULTIPLAYER_SELECT_JOIN_GAME while online: join the room of that row. False when online is off.
bool select_join(int row);

// ---- progression.cpp (0x64e810, PS3 0x4af240): NetPlayerData changed, publish the member blob 0x59 ----
void member_data_changed();

// ---- host migration and promote (PS3 RoomOwnerChanged / GrantRoomOwner) ----
// A client lost its host outside a running match: true when online takes it (the room's new owner hosts
// the party; online then calls session::online_reopen / online_migrated, or online_host_gone).
bool on_host_lost();
// This PC's NpId, and the NpId behind a transport address of the online link ("" when unknown).
std::string my_npid_text();
std::string npid_at(const net::Address &a);
// The leader hands the party to the member with this net id. False: not possible (offline, no room).
bool promote(uint32_t net_id);

// ---- friends and invites (RPCN; PS3 NP Basic) ----
int friend_count();
const char *friend_row(int i);       // "name - status"
std::string friend_name(int i);
bool friend_online(int i);
bool friend_joinable(int i);         // in this game, in a party / lobby with room for us
bool join_friend(int i);             // Join Party (0ace8463)
bool invite_friend(int i);           // Invite To Party (45a9c65b)
int invite_count();                  // invites waiting (shown on BUTTON_MULTIPLAYER_INVITES)
enum { kRowFriend = 0, kRowRequestIn = 1, kRowRequestOut = 2 };
int friend_kind(int i);              // what row i is (-1 none)
bool friend_add(const std::string &npid);   // send a friend request (or accept one)
bool accept_request(int i);
bool remove_friend(int i);           // unfriend / decline / cancel
std::string profile_text(int i);     // View Profile: name, state, presence
std::vector<std::string> friend_npids(size_t max);   // the NpIds of the first `max` friends of the friend list
// The rows are kept in the PS3 order: friends in this game, then the others online, then offline, each by
// online id; the friend requests follow. Rows move when presence changes: keep a pick by name (friend_index).
// What the friends list (cond 36) and the friend card (cond 38) show of friend row i (PS3 0x4c1fe0 / 0x4c1a08).
struct FriendView {
    std::string npid;
    bool online = false;
    bool this_game = false;      // runs this game (its presence is this game's, or the title is ours)
    bool multiplayer = false;    // in this game's multiplayer, our build (PS3: presence data accepted)
    bool details = false;        // activity and the values below are known (not for old PC data version 1)
    bool party_member = false;   // a member of our party room
    bool other_build = false;    // its multiplayer presence is of another build (PC note on View Profile)
    int activity = 0;            // 1 lobby, 2 cinema, 3 match; 0 unknown (old version 1)
    int mode = 0;                // the multiplayer menu mode (presence +0x08): the row icon value (PS3 0x4c2830)
    int party = 0, difficulty = 0, net_game = -1;
    uint64_t match_end = 0;      // Unix UTC seconds, 0 none
};
bool friend_view(int i, FriendView &v);      // false: row i is no friend (a request, or past the rows)
int friend_index(const std::string &name);   // the row of that friend (or request) now, -1 none
const char *request_state(int i);            // a request row's state text ("" for other rows)
void refresh_friend_list();                  // fetch the list and presence again (the list's op 8)
// The RPCN friend list has been read since the link came up (PS3: the friend list load is done).
bool friend_list_ready();
bool is_friend(const std::string &npid);     // a friend (not a request); false while the list is not ready
// Friend row i's party members (PS3 parity G4, 0x490c28): the cached list when it belongs to that friend (true),
// else nothing (false). Asks the room service for the members of the friend's party room (its presence) when the
// cache is another friend's or older than 5 s and no such request runs; u2mp::friend_party_arrived follows.
bool friend_party(int i, std::vector<std::string> &names);

// ---- PAGE_MULTIPLAYER's friends stats ticker (PS3 0x446c60 / 0x43b30c / 0x43b290, spec parity-d-flow1.md 2.10) ----
// One ticker text: a random category (0 cash .. 6 time played), then up to 5 random friends (of the first 101)
// whose presence is this build's multiplayer (a valid presence block; old PC data from version 3 on). No such friend: the heading only.
// ticker.cpp hands it to the game's ticker add function 0x5d6500 (spec parity-ticker-respawn.md 1).
std::string stats_ticker_text();
// The ticker's pieces, for tests: one friend entry appended to `out` (false, `out` unchanged, when it would reach
// 511 characters), and the time played text ("Nd Nh Nm", "Nh Nm", "Nm").
bool ticker_append_entry(std::string &out, const std::string &name, int category, uint64_t value);
std::string ticker_time(uint64_t seconds);

// ---- cinema files to and from friends (PS3 fe0e7de7 / 6b2970e2) ----
bool send_cinema(const std::string &npid);   // the selected cinema of the cinema page
int cinema_received();                       // files friends sent us this session

// ---- rankings (RPCN score boards; PS3 NP Score) ----
bool linked();                       // the online link is up (also outside Multiplayer)
bool board_request(uint32_t board, int mode, uint32_t start, uint32_t count);   // mode 0 ranks, 1 friends, 2 me
// 1 loading, 0 ready, -1 failed. A fetch never waits forever: the link going down or 10 s without an answer
// ends it as failed (PS3 parity A 2.4).
int board_state();
int board_rows();
uint32_t board_total();
const U2oScore *board_row(int i);
// At most 4 posts wait for an answer at once (PS3's 4 score slots); a fifth is dropped, not queued.
bool record_score(uint32_t board, int64_t score, const char *comment, const uint8_t *info = nullptr, uint32_t info_len = 0);

// ---- stranger matchmaking (frontend3.cpp; PS3 0x4b0a58 search / join / create) ----
// kMmNoRoom: the search pass found no room that worked out (frontend3 waits 1..3 s and runs the next criteria pass,
// or creates its own room after the last one, PS3 0x4b0a58 state 6).
enum { kMmIdle = 0, kMmBusy = 1, kMmHosting = 2, kMmJoined = 3, kMmFailed = 4, kMmNoRoom = 5 };
// One search pass (PS3 criteria row b25ab071[index]): the row's ping limit (0 none) and whether only rooms within
// 20 ranks of ours count (the first pass).
struct MmCriteria {
    int pass = 0;
    uint32_t ping_limit_ms = 0;
    bool rank_only = false;
};
bool mm_online();                    // online with a world: matchmaking finds strangers
// Host in NET_SM 19: one search pass; joins the best room (link up, ping check, admit) or ends in kMmNoRoom.
void mm_begin(uint32_t playlist, uint32_t playlist_index, const MmCriteria &criteria);
void mm_create();                    // after the last pass: our own matchmaking room (PS3 "GATHER")
int mm_step();
void mm_reset();
void mm_research();                  // nobody came: leave our gather room, search again
void mm_leave();                     // matchmaking ends for this PC
int mm_players();                    // players in our matchmaking room
// Host of a gather room: room members that joined less than 6 s ago and are not on the transport yet (PS3: an
// open slot reservation, 0x100 / 0x101 still running).
int mm_pending_members();
// Party leader: members of our party room that are not on our transport (they come back after a lost host).
int party_missing();
// The transport host is this PC or a member of our party room (false: a stranger hosts our matchmaking room).
bool host_in_party();
// We are in another player's party room (not its owner).
bool party_member();
// A stranger's matchmaking room is over for us (PS3 0x4b0a58 state 17: host gone, 0x103, kicked): leave it, then a
// player alone (or a party leader) searches again from the first criteria; a party member goes back to its
// leader and waits. bad_room: do not join that room again (kicked to balance the teams). Runs on the next frame.
void mm_host_gone(const char *why, bool bad_room);
// A party member whose leader left matchmaking (PS3 0x108): leave the room and go back to the leader's transport.
void mm_leave_to_leader();
// Leave the party (Leave Matchmaking as a member, PS3 party "leave" flag 0x4c36c8): every room, then a party of
// our own. False when online is off (the caller does the LAN part).
bool drop_out_of_party(const char *why);
std::string room_member_name(uint32_t net_id);   // party/match room member name by net id
// The matchmaking rows (session.cpp cond 6..15, PS3 slot list 0x4b0008):
bool in_own_party(const std::string &npid);          // this PC or a member of our party room
uint64_t own_party_id();                             // our party room while it has 2+ members, else 0
uint64_t member_party_id(const std::string &npid);   // the party id a match room member published (+0), 0 none
bool peer_rtt(const std::string &npid, uint32_t &rtt_ms, bool &up);   // false: no connection record
// The link state to that player (U2O_PEER_* of u2_online_abi.h: the matchmaking rows' bar colour, PS3 0x4e8f1c);
// -1 when there is no connection record or no state was reported yet.
int32_t peer_link_state(const std::string &npid);
// The match room's slot count (custom or matchmaking); without one the size a matchmaking room of the chosen
// playlist gets (matchmaking page) or 10. 0 when online matchmaking is off.
int mm_slots();

// ---- PS3 parity group E (session.cpp; spec parity-e-flow2.md 2.4 Party Up, 2.6 slot manager) ----
uint64_t party_room_id();            // our party room (also a party of one), 0 none
int party_size();                    // members of our party room, at least 1
// The NpIds of the members we last told to follow us into another room (cleared by the call): the party a joining
// leader asks the room owner's slot manager for (PS3 0x100).
std::vector<std::string> take_led_party();
// Party Up 0x119: join that party room by ourselves (we lead nobody into it). False: not started, no room, or ours.
bool party_up_join(uint64_t room);
// The room owner's slot manager refused our party (PS3 0x101 "not reserved"): matchmaking goes to the next room;
// a custom game join fails (JOINING pops, NET_SM 18) and we go back to a party of our own.
void reservation_refused();

}  // namespace u2mp::online

namespace u2mp {
// remaining.cpp: a one-button message box with this text (online notices: invites, migration).
void show_notice(const char *text);
// menus.cpp: the friend card of friends-list row i (cond 38 lines: title, status, activity / match, "Match ends
// in"); empty when row i is no friend. View Profile shows them (remaining.cpp).
std::vector<std::string> friend_card_lines(int i);
// remaining.cpp: a friend's party member list arrived (online::friend_party); an open View Profile box of that
// friend shows it.
void friend_party_arrived(const std::string &npid, const std::vector<std::string> &party);
}  // namespace u2mp

namespace u2mp::session {
// online.cpp -> session.cpp: closes the transport and opens it again on `link`, as the host of a new
// party (owner NULL) or as a client of the room owner at `owner`.
void online_reopen(net::Link *link, const net::Address *owner);
// online.cpp -> session.cpp: the match room exists (room_id) or could not be made (0): finish Start Match.
void online_match_ready(uint64_t room_id);
// online.cpp -> session.cpp: a client finished joining the leader's match room after the follow.
void online_match_member_joined();
// online.cpp -> session.cpp: the leader says whether the whole party is in the custom game room (PS3 0x10e {1} /
// {0}); also a failed join from the list (0). Posts mailbox 0x10e for the JOINING page while it waits (NET_SM 12).
void online_party_in(bool ok);
// online.cpp -> session.cpp, leader: the custom game is dropped (`why` is logged): a party member could not join the
// custom game room (PS3 0x465da0 state 10 failure), or the room could not be made (PS3 "Session Create failed",
// parity H2). The leader goes back to PAGE_MULTIPLAYER with NET_SM 18, no box.
void online_match_abort(const char *why);
// online.cpp -> session.cpp: the network error (netInfo +0x1c, the game's error box) and online play ends, for the
// host and the others alike; `why` is logged (no room service world, parity H8).
void online_network_error(const char *why);
// Room sizes (PS3 parity H1, 0x49e620 / 0x49a958): a custom game room gets the current game type's player count (10
// competitive, 3 co-op, 0 unknown type); a matchmaking room the chosen playlist's maximum: the largest player count of
// its game types (the PS3 playlist value is not known to this port), 10 without a playlist of ours.
int custom_room_slots();
int playlist_room_slots();
// The matchmaking room key's map-pack mask (PS3 parity H5, 0x4d6a08): the AND of the map-pack bits (dlc_packs) of
// this PC and every party member (a member that sent none does not narrow it); 0 when the chosen playlist has no
// map-pack map.
uint32_t mm_pack_mask();
// The NpIds of the other members on our transport (host), in slot order; empty when unknown.
std::vector<std::string> member_npids();
// online.cpp -> session.cpp: this player's name is the RPCN online name.
void set_local_name(const std::string &name);
// online.cpp -> session.cpp: the transport address of the member with this net id.
bool member_address(uint32_t net_id, net::Address &out);
// online.cpp -> session.cpp: the party has a new leader (host = this PC); matchmaking: the room goes on.
void online_migrated(bool host, bool matchmaking);
// online.cpp -> session.cpp: the host is gone and nobody took over: the old "host lost" path.
void online_host_gone();
// online.cpp -> session.cpp: the player signed out of RPCN in the launcher: the game's own "network error" path (error
// box, leaving multiplayer), for the host and for the others alike.
void online_signed_out();
// A running match may go on under a new host (PS3 HostMigrate at the match start, not in co-op).
bool match_migration_ok();
// The net id of the player with this NpId (from the host's kIds), 0 unknown.
uint32_t net_id_of(const std::string &npid);
// The room member's display name for a net id (party or match room), empty when unknown.
std::string room_member_name(uint32_t net_id);
// Match host migration: every player keeps its net id; owner NULL = this PC hosts now.
void online_rehost(net::Link *link, const net::Address *owner, uint32_t new_host_net_id);
// Session message type of online.cpp (below session::kFrontendMessages).
constexpr uint8_t kOnlineMessage = 7;
}  // namespace u2mp::session
