# Spec: PS3 parity, group H (room sessions and the session manager)

Clean-room behaviour description of 85 PS3 1.10 functions (rows of
`maintainer-tools/research/u2-parity/groupH.tsv`: net-session.cpp 50, net-session-manager.cpp 35), measured against
the UCMR U2 runtime (`gow3_pc/src/u2mp`). It states what the PS3 game does as seen by the players and by game memory,
and whether our code gives the same result. It does not prescribe code.

Conventions as in `session.md`, `parity-e-flow2.md` and `parity-f-party-mm.md`: addresses without "PS3" are PS4 1.02
image offsets; NET_SM is the s32 at `0x14ce97c` (PS3 `0xf11764`); "match" = g_matchSession (PS3 `0xf20c48`, PS4
`0x1065b08`); "party" = the party session (PS3 `0xf2c8a0`); "NPD" = NetPlayerData (PS3 `0xf2da60`); NGM =
NetGameManager. PS3 field offsets are interop facts (PS3 objects use 32-bit pointers; PS4 offsets of the same field
differ, `PORT-MATCHMAKING.md` 5.1 has the PS4 layout).

**Reference.** `PORT-MATCHMAKING.md` sections 1 and 2 already map this layer call by call (request contents,
callbacks, event codes, listener slots, signaling). This spec does not repeat that material; it gives the status of
every row, the corrections found while reading the rows (section 1.4) and the visible differences to implement.

**Binary notes.** All sampled descriptors of these rows carry TOC `0xe183b8` (the other-TOC issue of flow.cpp and
the slot manager does not apply). The two modules' string/data pools sit at PS3 `0xdbb42c` (manager) and `0xdbb504`
(session). The last five rows (`0x4d90e0`..`0x4d9398`) use a third pool (`0xdbb61c`, strings of
`game/net/net-simple-snapshot.cpp`): they are not session code (section 3, H9). Ghidra dropped the bodies of
`0x4d6a08`, `0x4d6ab0`, `0x4d6d10`, `0x4d6fa0` (treated the member-list call as not returning) and the request
structures of HostRank, HostMigrate, Hide, GrantRoomOwner and Kickout; those were read from the PS3 disassembly.

Status words: **COVERED** (same observable result already; file/function named), **PLATFORM** (PS3 plumbing that
the PC design replaces: NP Matching2 context/servers/worlds, the room event queue, signaling and the owner relay;
the visible result must still match, deltas named), **MISSING** (visible behaviour we lack or do differently; exact
difference named).

## 1. Facts every item uses

### 1.1 The room session object (PS3 net-session.cpp)

- One object per room the console can be in: the match session and the party session (the manager registers up to
  4; it gives each the context id and the first world of the first server).
- It holds: the room id (0 = in no room), the room name text, the "owner auto grant" flag of the room (taken from the
  room's flags on create/join), the "room hidden" flag (kept in step with the room's HIDDEN flag by every
  room-data update), the stored external state value (searchable int 0x53, published by the next create when set
  while in no room), the room's blob (internal bin attribute 0x57 / external 0x55, at most 256 bytes), our own
  member data (at most 64 bytes, normally 48), our member id, the owner's member id, and **10 member records**.
- A member record: used flag, a running "order number" (a counter that starts at a fixed value and goes up by one
  per member ever added; not shown anywhere), member id, team byte, NpId, online name, avatar URL, join date, the
  signaling connection used for that player, and the member's published data (48 bytes; only data of exactly 48
  bytes is ever answered by the lookup).
- Every room request has a "finished" flag and a result word (create, search, join, leave, hide, host migrate,
  host rank, room name, external state). The query helpers answer **1** while the request runs, **2** when it
  failed, **3** when it succeeded; the search query also answers a fourth value (all bits set) when it succeeded but
  no result data was kept. A freshly initialised session (`0x4d1e2c`) has every flag finished and result 0, so it
  reads "succeeded" until a request is made; it also clears every member record and both ids.
- Listener interface (9 slots: created, joined, member added, member removed, room gone, kicked, owner changed,
  room message, unused): `PORT-MATCHMAKING.md` 2.2. Slot 0 and 1 clear the session's "kicked" byte (PS3
  `0x4d26dc` / `0x4d26e8`), slot 5 sets it (`0x4d26f4`); PS4 keeps these slots (`0x5e9d90`, `0x5e9db0`,
  `0x5e9f20`: match+8).

### 1.2 Member lookups and member lists

- Lookups (all COVERED by our member tables): member record at slot i (null when unused); the owner's record; our own
  record; "is member id X me"; "is member id X the owner"; **IsHost** = true when in no room, else "my id equals the
  owner's id"; member count = 1 when in no room, else the number of used records; member by NpId; published data by
  NpId (our own copy answers for us).
- **Member list for the UI** (`0x4d5e38`): in no room, one entry: ourselves (own NpId and avatar URL). In a room, the
  used records with the **owner first**, then the others in slot order. In "team" mode the list is sorted: the owner
  first, then the members whose published team (data byte +9) equals the owner's team, then the rest; members with
  the same standing keep slot order (the sort `0x4d2790` with order `0x4d2668`). This is the order of the lobby rows.
- **Name with clan tag** (`0x4d2700`): when the member's published data has a non-empty clan tag (data +10), the
  shown name is "[TAG] name", else the plain name.

### 1.3 Sending to the members (`0x4d3060`, `0x4d3748`, `0x4d3754`)

- "Send to members in a set" (default: all): for every other member that has a connection record, the packet is
  queued directly when the link to that player is usable; members whose direct link is not usable are collected and
  the packet goes **once to the room owner with the list of those member ids**, and the owner forwards it. The owner
  itself is always sent to directly. "Send to member id X" uses the same path; addressed to ourselves, the packet
  only goes to the debug network recorder.
- Visible result: every addressed member receives each message exactly once, in send order per sender, whether or
  not it can reach the sender directly.

### 1.4 Corrections to `PORT-MATCHMAKING.md` found here

1. **Searchable int 0x4D and the bits from 12 up of 0x4C are a map-pack mask, not a party size.** The caller value
   is `0x4d6a08` = the AND of byte +19 of every member's published data (bits start all set; members without data
   do not narrow it). Byte +19 is the low byte of the word at PS3 `0xe1d728`+12, the console's map-pack bits (the
   same AND decides Start Match's "a party member lacks the map pack" box, `parity-d-flow1.md` 2.2). Matchmaking
   create (`0x4b2a18`) and search (`0x4b1134`) pass it with the "include" flag set only when NPD+367 bit 0 is set,
   the playlist uses map-pack content (`0x49b858`) and the mask is not 0; custom games pass 0.
2. **Member data layout** (48 bytes, producer `0x4af240`): +0 party room id (u64), +8 rank badge, +9 team, +10..+17
   clan tag (8 bytes, the two NPD words at +356/+360; +18 stays 0), +19 map-pack byte, +20..+35 four NPD skill words
   (+228..+243), +36..+43 four 16-bit words of the console's network address. (`parity-b-menus.md` already uses +10
   tag and +0x14 skill.)
3. **Request time limits** (option parameters): JoinRoom 5 s; LeaveRoom, the four SetRoomDataInternal users,
   SetRoomDataExternal, SetRoomMemberDataInternal, GrantRoomOwner, Kickout 10 s; CreateJoinRoom and SearchRoom use
   the context default, 20 s. A request that times out reaches its callback as a failure.
4. **SearchRoom** asks for at most **5** rooms, picked **at random** by the server among the matches (option "with
   NpId, with online name, random"), with external bin attribute 0x55 returned.
5. **HostRank** (`0x441290`, host, when "Connections done" at match start): the remote players' trackers in game slot
   order, those flagged "can talk to everybody" moved in front of the others (order otherwise kept); the member ids of
   that list become the room's owner succession.
6. **HostMigrate** (owner auto grant on) is switched on by the host when the match starts, unless the game type is
   co-op, and off again at Leave::Enter (`PORT-MATCHMAKING.md` 3.7, corrected text).
7. **Custom game room size** (`0x49e620`): 10 slots for the competitive game types (0, 1, 2, 3, 5, 6, 7, 10, 11), 3
   for the co-op types (4, 8, 9, 12), 0 for an unknown type. Matchmaking rooms: the playlist's maximum player count
   (`0x49a958`). Party room: 10.

### 1.5 Room events (`PORT-MATCHMAKING.md` 2.1)

Every frame (NdiNetUpdate) the queued room events (at most 64; a full queue is a fatal assert) are applied in
arrival order to the session whose room id matches; events for a room no session is in are dropped. Member joined:
record added, signaling started, listener slot 2. Member left: listener slot 3, record removed, connection released
(a connection shared by both sessions stays until its last user releases it). Kicked out (slots 4 and 5) / room
destroyed (slot 4): room id 0, leave finished, every record removed. Owner changed: owner id updated, slot 6. Room
data updated: hidden flag, room blob 0x57, room name 0x58. Member data updated: that member's 48 bytes. Room
messages: nothing sends them in this build.

## 2. What our port does (summary)

- `online.cpp` is the room client over `u2_online_abi.h` (RPCN rooms through `u2_online.exe`): one party room and one
  match room (`Room` records with members, owner id, our id, flags), the PS3 attribute set on create (0x4C key, 0x4D,
  0x4E/0x4F, 0x50, 0x51 for matchmaking, 0x52 PC kind, 0x53 state, 0x55, 0x58 name, 0x59 member data), hide/close
  (`set_hidden`), state 0x53 (`set_state53`), member data (`publish_blob`, `member_blob`), owner succession
  (`send_succession`, match start in `room_state`), join list search (`start_search`), host migration
  (`migration_tick`, `finish_migration`), promote (leave and rejoin), events polled every frame (`frame`, `on_event`).
- `session.cpp` is the transport-level party (star through the host; 10 slots): roster, names with clan tags, the
  team-sorted member rows (`member_at`), net players, kicks with rejoin block (`kick`, `kick_griefer`), the map-pack
  byte in kInfo and Start Match checks, match migration only in a running non-co-op match (`match_migration_ok`).
- `voice.cpp`: per-member mute (`toggle_mute`, `muted`); no "all" helpers.

## 3. Items

### H1. Room size by game type and playlist (`0x4d421c` callers) - MISSING, implemented 2026-11-25 (section 7)

PS3: the room's slot count is fixed at creation (1.4 item 7). A custom game of a co-op type gets a 3-slot room, so a
4th player is refused by the room service ("room full"): the join fails like any failed join (F3: Joining page
popped, NET_SM 18, no box), and the join list row shows "n/3". A matchmaking room has the playlist's maximum; it is
"full" (gather ends at once, `parity-f` F1) at that size.

Ours: every room has 10 slots (`create_room`), so a 4th and later player can join a co-op lobby (Start Match then
refuses "too many players"), list rows and presence show "/10", and a co-op matchmaking room keeps gathering.

To do: custom match room: slots = `session.cpp type_max_players(type)` of the custom game's current game type at
Start Match (0 = refuse to create, log it; the host then goes on as for a create failure, H2); party room: 10. The
type is read when the room is created; later game-type changes in the lobby do not resize the room (PS3 neither).

Done (2026-11-25): matchmaking room: the playlist's maximum player count (record `+0x18`, open question 1: 10, co-op
3; 10 for a playlist not known), online.cpp `create_room` / `mm_room_slots`. `mm_slots()` reports the joined or
made room's real slot count (custom and party rooms: their own, 10), else the size our room for the chosen playlist
would get; the presence "max" already reads the room's slot count. The matchmaking minimum stays the owner rule (2
players, 1 for a co-op-only playlist, frontend3.cpp `matchmaking_min_players`).

### H2. Every room request ends: PS3 time limits and failure results (`0x4d421c`, `0x4d4578`, `0x4d3c50`, `0x4d8220`) - MISSING, implemented 2026-11-25 (section 7)

PS3: no room request waits forever (1.4 item 3). The visible ends:

- **Join** (5 s): no answer = join failed: room id stays 0, "JoinRoom Failed: %s (%x)" in the debug log, then the
  caller's failure path (custom JOINING page: pop, NET_SM 18; matchmaking: room marked bad, next room, `parity-f` F3,
  F5; party join: back to an own party, F11).
- **Create** (20 s): no answer = create failed ("Session Create failed %x"): custom host: the create-wait page is
  popped, NET_SM 18 (back on PAGE_MULTIPLAYER, no box); matchmaking GATHER: its failure path; party room: the party
  machine's error state (F11).
- **Search** (20 s): no answer = search failed: the join list page pops, NET_SM 18 (PS4 `0x609640` through
  match+0xa8/+0xac).
- Leave, hide, state, member data, owner order, grant, kick (10 s): failure is only logged (leave: see H8).

Ours: requests wait for the online program's answer without a limit, except Start Match (15 s, then "goes on
without a room") and the leaderboards (10 s, parity A). A create that fails or times out lets the party play the
custom game without a room (nobody else can join).

To do: each pending room request gets the PS3 limit above (measured from the request); when it passes, the request
is treated exactly as a failure answer of that kind and a later answer is ignored (as `board_expire` does). Create
failure / timeout of the custom match room: PS3 result (pop to PAGE_MULTIPLAYER, NET_SM 18) instead of playing
without a room - **owner decision** (open question 4); the 15 s becomes 20 s either way.

### H3. Join list: at most 5 rooms, random choice (`0x4d3c50` from BUTTON_MULTIPLAYER_JOIN) - MISSING, implemented 2026-11-25 (section 7)

PS3: the custom game list shows at most 5 rooms of the same build key, a random pick of the open rooms each time the
list is opened. Ours: up to 20 rooms in the service's order.

To do: ask for the matching rooms as now, then keep a random choice of 5 of the rooms that would be listed (drop
our own rooms first), so that a fresh search can show other rooms. With 5 or fewer matches, all of them in random
order.

### H4. Party rooms are not in the join list (`0x4d3c50`, party create key) - MISSING (owner decision 2026-11-25: kept, the party rows stay as a PC extra)

PS3: the party room is created with the build key plus bit 20 (`parity-f` F11), so the custom game search (key equal
to the build key) never finds it; friends reach a party only through Join Party / invites. Ours: the party room has
the plain build key, so the join list shows "Party" rows next to custom games and selecting one joins that party
(a deliberate PC extra in the `online.cpp` header, from before friends and invites existed).

To do if parity is chosen: party room key = build key with bit 20 set; the join list then shows custom games only
(the "Party" row kind and its select path go away). Join Party from the friends list and invites keep working (they
use the room id, not the search).

### H5. Map-pack mask in the matchmaking room key (`0x4d421c`, `0x4d3c50`, `0x4d6a08`) - MISSING (small), implemented 2026-11-25 (section 7)

PS3 (1.4 item 1): in a playlist with map-pack content, the matchmaking room key (0x4C) carries the party's common
map-pack bits from bit 12 up and 0x4D holds the same mask, both on create and on search, so parties only meet rooms
made by parties with the same common packs. Ours: 0x4C without the mask and 0x4D = party size.

To do: common mask = AND of the `dlc_packs()` bits of every party member (session.cpp already has each member's
packs from kInfo; a member that sent none counts as all packs); when the playlist uses map-pack maps and the mask is
not 0, add it to the matchmaking key from bit 12 up and put it in 0x4D, on create and search alike; otherwise key
as now and 0x4D = 0. Custom game and party rooms: 0x4D = 0. (`parity-f` F5's "party size bits" in the room pick are
these mask bits.)

### H6. Owner succession order of a match room (`0x4d4868`) - MISSING (small), implemented 2026-11-25 (section 7)

PS3 (1.4 item 5): at match start the host gives the room the remote players in game slot order, players that can
talk to everybody first. Who takes over after the host leaves a running match is the first of that list still there.
Ours: lowest RPCN member id first (`room_state`, `migration_tick` fallback), which can differ from slot order after
players left and rejoined.

To do: at match start, order = the other players by transport net id (net id = slot + 1, the game slot order every
PC knows from the roster), players without a working link to some other player behind the others (open question 5);
the `migration_tick` fallback picks the first remaining player of the same order, so every PC agrees without the
service. Party room succession (lowest member id) stays: PS3 sent no order for party rooms.

### H7. Mute All / Unmute All helpers (`0x4d6ab0`, `0x4d6d10`, `0x4d6fa0`) - MISSING (see `parity-e-flow2.md` 2.7), implemented 2026-11-25 (section 7)

The popup rules and texts are `parity-e` 2.7; this gives the three helpers' exact results (read from the
assembly; this answers `parity-e` open question 4 for `0x4d6d10` / `0x4d6fa0`). "Others" = the members of the given
session except ourselves that have a voice/connection record; members without one are ignored.

- "Everybody muted" (`0x4d6fa0`): true when every other with a record is muted (also true when there is none).
  Mute All is offered unless this is true.
- "Nobody muted" (`0x4d6d10`): true when no other with a record is muted (also true when there is none). Unmute All
  is offered unless this is true.
- "Set all" (`0x4d6ab0`, on/off): sets the mute state of every other with a record; nothing is shown or logged.

To do (`voice.cpp`, used by remaining.cpp's popup): the same three operations over the remote members of the party
(party pages) or the match (otherwise); with nobody else both rows are hidden.

### H8. Platform replacements: what must match (PLATFORM rows)

- **Session manager start/stop** (`0x4d0fc0`, `0x4d1a00`, callbacks `0x4cff50`, `0x4d0040`, `0x4d0130`, `0x4d02c8`,
  `0x4d0d90`, `0x4d0e78`, registration `0x4d01a8`, `0x4d1ebc`, `0x4d1ec8`). PS3: start the room context (up to 10
  tries), list servers, drop unavailable ones, one world list per server, then the match and party sessions get the
  first world; no server or no world makes NetInit fail with the network error (`NETINIT.md`). Stop: server contexts
  deleted, context stopped. Ours: `u2o_open`, `u2o_find_world` (retry every 10 s, link reopened every 15 s), world
  shared by all rooms; leaving Multiplayer leaves the rooms and disconnects peers, the link stays up for presence.
  Delta: with the online program configured but no world, PS3 refused online play with the error box, ours stays in
  Multiplayer as a local party (open question 6).
- **Event queue and dispatch** (`0x4d0c88`, `0x4d0bd0`, `0x4d040c`): `online.cpp frame` polls up to 256 events per
  frame on the game thread, in order, and applies them as 1.5. Kicked-out and room-destroyed events have no RPCN
  counterpart; their visible results come from session kicks (`session.cpp` kKick, kicked byte, abort byte
  `0x113a270`) and the host-gone path (`online_host_gone`).
- **Owner relay** (`0x4d3060`): our transport is a star; the host relays every client message (1.3 result holds).
- **Grant owner** (`0x4d3a10`): Promote To Party Leader is done by owner succession plus the leader leaving and
  rejoining (`online::promote`); visible result the same (the chosen member leads, the old leader stays in the
  party).
- **Kick** (`0x4d8a98`): session kick message with rejoin block (`session::kick`, `parity-c`); kicking yourself =
  leaving, as PS3.
- **Host migrate** (`0x4d4b28`): rooms always have owner auto grant; `match_migration_ok` gives the PS3 result (a
  running non-co-op match goes on under the next owner; lobbies and co-op do not migrate).
- **Room rename** (`0x4d57b0`): after the matchmaking vote PS3 gives the room a fresh "%s.%lld" name; nothing shows
  room names, so there is no visible effect. Optional (log only).
- **Ping** (`0x4d3848`, `0x4d4e0c`): PS3 logs "RTT Room Id %lld : %i ms" when a listed game is selected; ours logs
  peer RTT every 10 s. Debug only.
- **Location bin attribute** (`0x4d2e68`): PS3 copies a listed room's 0x55 floats for the "Distance to Host" log and
  the matchmaking distance score; PC has no location (`parity-f` F5).
- **Leave retry** (`0x4d8bf4`): PS3 repeats LeaveRoom when the service reports an error and counts the leave as
  finished only on success. Ours sends one leave and forgets the room at once (the page flag match+0xc8 is set by
  session.cpp). No visible difference; a second try on an error other than "no such room" is optional.

### H9. Rows that are not session code (`0x4d90e0`, `0x4d911c`, `0x4d912c`, `0x4d9178`, `0x4d9398`) - COVERED

They belong to the simple-snapshot class (the interpolated state of networked moving objects; callers are object
update code far from the menus): a module word set to -1 at start-up, two "clear the snapshot buffer" functions and
"advance the buffer's play time" (waits for 3 snapshots, then follows the newest time at the frame rate, never
running ahead of the latest snapshot nor behind a floor the owning object reports). PS4 keeps the class: clear
`0x66a920` / `0x66a9f0`, play time `0x66ade0`, interpolation `0x66ab90` (PS3 `0x4d93e8`). Nothing to port.

## 4. Test scenarios

1. H1: host a custom co-op game online; three players join; a 4th selects it in the list: refused as "room full",
   Joining page popped, NET_SM 18; the row shows "3/3". A competitive custom game still takes 10.
2. H2: make the online program hold a JoinRoom answer: after 5 s the join fails as a refused join. Hold a search: the
   list page pops after 20 s. Hold the custom room create: after 20 s the PS3 result (or the owner's choice).
3. H3: 8 open custom rooms: the list shows 5; reopening it can show a different 5.
4. H4 (if chosen): another PC's party room never appears in the join list; Join Party from the friends list still
   reaches it.
5. H5: two parties with different common map packs search a map-pack playlist: they never meet; with equal packs
   they do; non-map-pack playlists ignore packs.
6. H6: four players, net ids 1..4; the host leaves a running match: net id 2 hosts on every PC, also when RPCN moves
   the room late.
7. H7: three others, mute one: both Mute All and Unmute All shown; Mute All: only Unmute All shown; Unmute All: only
   Mute All shown; alone: neither.

## 5. Open questions

1. H1: the playlist maximum player count: answered. PS3 `0x49a958` looks the playlist up by id (NetPlayerData
   `+0xc0`, or `0x0b70ed28` with the AutoMatchmaking boot byte) and returns its record's word `+0x18`, 0 when not
   found. The PS3 *playlists* records hold 10 for the competitive playlists and 3 for the co-op ones (maximum party
   size `+0x1c`: 5 and 3). menus.cpp writes both into our records; online.cpp makes a matchmaking room that size.
2. H5: meaning of NPD+367 bit 0 (it gates the mask) and of `0x49b858` ("the playlist uses map-pack content" is the
   reading); `parity-f` F5 calls NPD+367 bit 0 "may not search" - the two readings must be reconciled from the
   matchmaking page's own code. **Answered 2026-11-27**: it is the Profile's "Matchmaking" setting (Default 0 / DLC
   1, PS3 query `0x4c09a8`, PS4 `0x65a790`, PS4 NetPlayerData `+0x1b0` bit 0). At `0x4b10d8` / `0x4b29d0` the bit
   only chooses the key: set, `0x49b858` true and the common mask not 0 -> mask in the key; else the plain key. It
   does not stop the search (`0x49a738` alone does that).
3. H4: keep listing party rooms (PC extra) or follow PS3? (Owner 2026-11-25: keep.)
4. H2: custom room create failure: PS3 abort (back to PAGE_MULTIPLAYER) or ours (play without a room)? (Owner
   2026-11-25: PS3.)
5. H6: what sets the trackers' "can talk to everybody" flag on PS3 (tracker +580); until known, put players whose
   reported peer links are not all up behind the others, or keep plain net id order. (Owner 2026-11-25: reported
   links first, net id order as the fallback; section 7.)
6. H8: with the online program present but no world, should Multiplayer show the PS3 network error instead of going
   on as a local party? (Owner 2026-11-25: the network error.)
7. `PORT-MATCHMAKING.md` 1.3 / 1.8 / 1.7 should get the corrections of 1.4 (0x4D = map-pack mask, member data
   layout, HostRank order). (Done 2026-11-25.)

## 6. Status table

| PS3 | Role (what it does) | Status | Where (our code / PS4) / item |
|---|---|---|---|
| 0x4cff50 | DeleteServerContext callback: log, done flag | PLATFORM | online.cpp link close; H8 |
| 0x4d0040 | CreateServerContext callback: log, done flag | PLATFORM | online.cpp `frame` find world; H8 |
| 0x4d0130 | default request callback: log, clear event data | PLATFORM | online.cpp `on_event` (unknown requests logged); H8 |
| 0x4d01a8 | register a session with the manager (max 4), give it context and first world | PLATFORM | online.cpp `world` shared by all rooms; H8 |
| 0x4d02c8 | GetServerInfo callback: keep server status | PLATFORM | `u2o_find_world`; H8 |
| 0x4d040c | apply one room event to its session (1.5) | PLATFORM | online.cpp `on_event`; H8 |
| 0x4d0bd0 | per frame: apply every queued room event, empty the queue | PLATFORM | online.cpp `frame` poll loop; H8 |
| 0x4d0c88 | room event callback: queue it (max 64, assert) | PLATFORM | online program event queue; H8 |
| 0x4d0d90 | context callback: "started" flag and error | PLATFORM | `u2o_state` link states; H8 |
| 0x4d0e78 | GetWorldInfoList callback: keep the list | PLATFORM | `u2o_find_world`; H8 |
| 0x4d0fc0 | session manager start (context, servers, worlds) | PLATFORM | online.cpp `open_link`, `frame`; no world in Multiplayer: network error (`no_world_error`); H8 |
| 0x4d1a00 | session manager stop | PLATFORM | online.cpp `on_transport_closed`; H8 |
| 0x4d1c80 | room message callback (nothing sends) | COVERED | none needed |
| 0x4d1e2c | session init: no room, records cleared, requests "finished" | COVERED | online.cpp `Room{}` reset; session.cpp sets match+0xc8 (`NETINIT.md` 3) |
| 0x4d1ebc | set the session's context id | PLATFORM | none (one link); H8 |
| 0x4d1ec8 | set the session's world id | PLATFORM | online.cpp `world`; H8 |
| 0x4d1ed4 | member record at slot i (null if unused) | COVERED | session.cpp member slots |
| 0x4d1f08 | the owner's member record | COVERED | online.cpp `Room::member(owner_id)` |
| 0x4d2080 | the owner's member record (second copy) | COVERED | as 0x4d1f08 |
| 0x4d21d4 | our own member record | COVERED | online.cpp `Room::my_id`; session.cpp local member |
| 0x4d234c | is member id X ours | COVERED | online.cpp `my_id` compare |
| 0x4d2370 | is member id X the owner | COVERED | online.cpp `owner_id` / `owned()` |
| 0x4d2394 | IsHost: in no room, or our id = owner id | COVERED | `session::is_host` (PS4 inlined, PORT-FRONTEND2) |
| 0x4d23cc | the room's owner-auto-grant flag | COVERED | online.cpp rooms always auto grant (H8) |
| 0x4d23d4 | set the 0x4C key base before create/search | COVERED | online.cpp `room_key` / `match_key` |
| 0x4d23dc | store the map-pack word (no reader) | COVERED | none |
| 0x4d23e4 | create status (busy / failed / done) | COVERED | PS4 match+0x99 via `session::online_match_ready` |
| 0x4d2410 | search status (busy / failed / done / done without data) | COVERED | PS4 match+0xa8/+0xac via online.cpp `search_failed` |
| 0x4d245c | join status | COVERED | PS4 JOINING page `0x609790` + session.cpp `on_follow` |
| 0x4d24a8 | member count (1 in no room) | COVERED | `session::member_count` (MP always has a party) |
| 0x4d2668 | order of the team-sorted member list | COVERED | `session::member_at` order |
| 0x4d26b0 | leave status | COVERED | PS4 match+0xc8 set by session.cpp |
| 0x4d26dc | clear the session's kicked byte (match listener 0) | COVERED | PS4 `0x5e9d90` |
| 0x4d26e8 | clear the kicked byte (listener 1 / party) | COVERED | PS4 `0x5e9db0` |
| 0x4d26f4 | set the kicked byte (listener 5) | COVERED | PS4 `0x5e9f20`; session.cpp kKick |
| 0x4d2700 | name as "[TAG] name" when the member has a clan tag | COVERED | session.cpp shown names (clan tag from kInfo) |
| 0x4d2790 | generic sort used by the member list | COVERED | `session::member_at` |
| 0x4d2e68 | copy a listed room's 0x55 location floats | PLATFORM | no location on PC; H8 |
| 0x4d2ed4 | publish our member data (<= 64 bytes) | COVERED | online.cpp `publish_blob`; content parts via session.cpp kInfo |
| 0x4d3060 | send to members, unreachable ones relayed by the owner | PLATFORM | net.cpp star transport; H8 |
| 0x4d3748 | send to every member | COVERED | `session::send_message(-1)` |
| 0x4d3754 | send to member id X (self: debug recorder) | COVERED | `session::send_message(slot)` |
| 0x4d3848 | ping info request (debug RTT) | PLATFORM | online.cpp RTT log; H8 |
| 0x4d3920 | apply a room internal attribute (blob 0x57, name 0x58, "ROOM NAME" log) | COVERED | online.cpp `take_room` (no reader of the name) |
| 0x4d3a10 | GrantRoomOwner request | PLATFORM | `online::promote`; H8 |
| 0x4d3af8 | set external state 0x53 (stored while in no room) | COVERED | online.cpp `set_state53` |
| 0x4d3c50 | SearchRoom (key, location, region, tier filters; 5 random) | COVERED | online.cpp `start_search` / `mm_begin` (20 s, `time_limit`), join list 5 random (`U2O_EV_SEARCH`), map-pack key (`match_key`); H2, H3, H5; party rows kept (H4, owner) |
| 0x4d4020 | member record by NpId | COVERED | online.cpp `in_room` / session.cpp `net_id_of` |
| 0x4d421c | CreateJoinRoom (slots, flags, attributes, member data) | COVERED | online.cpp `create_room` (`room_slots`, 0x4D / 0x4C mask, 20 s); H1, H2, H5; party key kept (H4, owner) |
| 0x4d4578 | JoinRoom (member data, 5 s) | COVERED | online.cpp joins (5 s, `time_limit` / `expire_requests`); H2 |
| 0x4d479c | member data callback: log | COVERED | online.cpp `U2O_EV_DONE` log |
| 0x4d4868 | HostRank: owner succession list | COVERED | online.cpp `send_match_order` (slot order, kOpTalkAll first; kOpOrder to all), `migration_tick`; H6 |
| 0x4d4a0c | HostRank callback: log, result | COVERED | `U2O_EV_DONE` log |
| 0x4d4b28 | HostMigrate: owner auto grant on/off | PLATFORM | always on + session.cpp `match_migration_ok`; H8 |
| 0x4d4cf0 | HostMigrate callback: log, result | COVERED | `U2O_EV_DONE` log |
| 0x4d4e0c | ping info callback: "RTT Room Id" log | PLATFORM | RTT log; H8 |
| 0x4d4f68 | GrantRoomOwner callback: log | COVERED | log only |
| 0x4d5034 | Kickout callback: log | COVERED | log only |
| 0x4d5100 | SetRoomDataExternal callback: log, result | COVERED | `U2O_EV_DONE` log |
| 0x4d521c | room name callback: log, result | COVERED | log only |
| 0x4d5338 | Hide: HIDDEN and CLOSED together, hidden flag kept | COVERED | online.cpp `set_hidden` |
| 0x4d550c | Hide callback: log, result | COVERED | `U2O_EV_DONE` log |
| 0x4d5628 | SearchRoom callback: keep results, finished | COVERED | online.cpp `U2O_EV_SEARCH` |
| 0x4d57b0 | rename the room "%s.%lld" | PLATFORM | none (no visible use); H8 |
| 0x4d5940 | search results -> rows (room id, owner name) | COVERED | online.cpp `found` / join rows |
| 0x4d5a18 | a member's published data by NpId (48 bytes only) | COVERED | session.cpp member info (kInfo) |
| 0x4d5e38 | member list for the UI (owner first; team-sorted mode) | COVERED | `session::member_at`, `fill_net_players` |
| 0x4d6a08 | AND of every member's map-pack byte | COVERED | session.cpp Start Match packs check, `mm_pack_mask` (matchmaking key, H5) |
| 0x4d6ab0 | mute / unmute every other member | COVERED | voice.cpp `set_all_muted`, remaining.cpp `mute_others`; H7 |
| 0x4d6d10 | "nobody else muted" | COVERED | voice.cpp `none_muted`; H7 |
| 0x4d6fa0 | "everybody else muted" | COVERED | voice.cpp `all_muted`; H7 |
| 0x4d7200 | remove a member ("REMOVING MEMBER", listener 3, release link) | COVERED | online.cpp `U2O_EV_MEMBER_LEFT`, session.cpp `player_left` |
| 0x4d74b8 | remove every member | COVERED | online.cpp `leave_room` / `on_transport_closed` |
| 0x4d76e8 | add a member ("ADDING MEMBER", connect, listener 2) | COVERED | online.cpp `connect_member`, session.cpp `on_peer` |
| 0x4d7e48 | JoinRoom callback: members, room id, attributes, listener 1 | COVERED | online.cpp `U2O_EV_ROOM_JOINED`, `join_succeeded` |
| 0x4d8220 | CreateJoinRoom callback: members, room id, listener 0 | COVERED | online.cpp `U2O_EV_ROOM_CREATED`; failure / timeout of the custom room: `custom_create_failed` (PS3 result, owner); H2 |
| 0x4d8540 | LeaveRoom: records removed at once, request sent | COVERED | online.cpp `leave_room` |
| 0x4d8a98 | Kickout (block flag); self = leave | PLATFORM | `session::kick`; H8 |
| 0x4d8bf4 | LeaveRoom callback: retry on error | PLATFORM | single leave; H8 |
| 0x4d8e88 | remove every member, then leave (MP teardown) | COVERED | online.cpp `on_transport_closed` |
| 0x4d90e0 | snapshot module: a word set to -1 (start-up) | COVERED | PS4 own init; H9 |
| 0x4d911c | start-up thunk of 0x4d90e0 | COVERED | H9 |
| 0x4d912c | snapshot buffer clear | COVERED | PS4 `0x66a920`; H9 |
| 0x4d9178 | snapshot buffer play time | COVERED | PS4 `0x66ade0`; H9 |
| 0x4d9398 | snapshot buffer clear (second copy) | COVERED | PS4 `0x66a9f0`; H9 |

Counts: 54 COVERED, 23 PLATFORM, 8 MISSING (H4 and H2's create result wait on owner decisions).
After the implementation (section 7): 62 COVERED, 23 PLATFORM, 0 MISSING (H4 kept as a PC extra by owner decision).

## 7. Implementation (2026-11-25, branch `u2mp-ps3-parity-3`)

Owner decisions: H1 as specified; H2 with the PS3 result for a failed custom room create; H3 as PS3; **H4 kept**
(party rows stay in the join list, PC extra); H5 as specified; H6 slot order with "can talk to everybody" first,
net id order as the fallback; H7 as specified; H8 no world = the PS3 network error.

- **H1** (online.cpp `room_slots`, `create_room`; session.cpp `custom_room_slots`, `playlist_room_slots`): party room
  10; custom game room `type_max_players` of the current game's type at Start Match (10 / co-op 3; 0 = no room, log
  `the custom game room is not made: the game type has no player count (0 slots)`, then H2's create failure);
  matchmaking room the largest `type_max_players` of the chosen playlist's game types (open question 1 stand-in; 10
  without a playlist). `mm_slots()` (slot manager capacity, gathering rows) answers the match room's real
  `max_slot` for every room kind, without a room the matchmaking size on the matchmaking page, else 10; presence and
  the join list rows show the service's `max_slot`. A reused custom room keeps its size.
- **H2** (online.cpp `time_limit`, `request`, `expire_requests`, `late_answer`, `fail_pending_now`,
  `custom_create_failed`): every request gets the PS3 limit when it is sent (join kinds 5 s; create kinds, both
  searches and the world list 20 s; leave, room / member data, owner order 10 s; presence, messages, friends and
  scores none here). Each frame a request past its limit is logged (`%s on room %llx (request %llu): no answer in %.0f
  s: failed`) and handed to `on_event` as an answer of its kind with error -2 ("no answer in time"), so the existing
  failure paths run (join list: JOINING pops, NET_SM 18, party left; matchmaking: next room; follow: back to an own
  party; search: list page pops; party room: next try in 5 s). The real answer, if it still comes, is dropped
  (`the answer to request %llu came after its time limit: ...`); a room it made or joined is left at once. A link that
  goes down (U2O_EV_LINK_LOST, or the state leaving READY) fails every such request on the spot. Custom game room
  that cannot be made (error answer, 20 s, link / world missing, request refused, 0 slots): `custom_create_failed`
  logs `Session Create failed (%s): Start Match ends, back to PAGE_MULTIPLAYER, NET_SM 18`, shows the party room
  again and calls `session::online_match_abort(why)` (match session cleared, menu reset, PAGE_MULTIPLAYER, NET_SM 18,
  no box); `create_match_room` now returns true for those cases and the abort runs on the next frame. The 15 s Start
  Match timer and "goes on without a room" are gone.
- **H3** (online.cpp `U2O_EV_SEARCH`): the listable rooms (not ours, not matchmaking) are shuffled and the first 5
  kept, so every search can show another 5; 5 or fewer: all, in random order.
- **H4**: unchanged (party room plain build key, "Party" rows).
- **H5** (session.cpp `mm_pack_mask`; online.cpp `match_key`, `create_room`, `mm_begin`): in a playlist with a
  map-pack map the key gets mask << 12 and 0x4D the mask, on create and on search; else 0x4D = 0 (all room kinds).
  Since 2026-11-27 the mask is used only when the Profile's Matchmaking setting is DLC (NetPlayerData `+0x1b0` bit 0,
  open question 2; menus.cpp shows the row when a map pack is imported, DLC.md); Default: the plain key, 0x4D = 0 (PS3 still fills 0x4D there; no search filters on it).
- **H6** (online.cpp `send_match_order`, `talk_all_tick`, `on_message` kOpTalkAll / kOpOrder, `migration_tick`):
  every match room member that is not the host reports once a second whether its links to every other room member
  are up (session message kOnlineMessage op 5 `{u8 all}`, sent on change or to a new host). At the match start the
  host orders the other players by net id, the "all up" ones first (stable), room members without a net id last by
  member id; the member ids go to `u2o_set_owner_rank`, the net ids to every player (op 6 `{u8 n, n x u8 net id}`,
  log `owner succession of %zu player(s), net ids %s`). `migration_tick`'s fallback picks the promoted player, else
  the first of that order still in the match room, else the lowest member id (party room, no order). Older PCs ignore
  both ops (net id order then, as the fallback).
- **H7** (voice.cpp `all_muted`, `none_muted`, `set_all_muted`; remaining.cpp `mute_others`, `party_others`): see
  `remaining.md` "Parity group H changes".
- **H8 no world** (online.cpp `no_world_error`; session.cpp `online_network_error`): in Multiplayer, a world request
  that fails (error, 20 s, refused) gives the network error (netInfo +0x1c, transport closed next frame, log `party:
  network error (no room service world): leaving online play`); outside Multiplayer it is tried again every 10 s.
- `PORT-MATCHMAKING.md` 1.3, 1.5, 1.7, 1.8, 3.3, 3.7 and 6 carry the corrections of 1.4.

Not possible without the online program: the PS3 search option "random" (the room service picks among all matches;
`U2oSearch` has no option field, so we pick 5 of the first 20 the service returns) and a per-request time limit
inside the program (`u2_online_abi.h` has none; the limit runs in online.cpp and a late answer is undone).
