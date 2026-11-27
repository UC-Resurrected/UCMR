# Spec: PS3 parity, group F (party, matchmaking, custom game join, mailbox)

Clean-room behaviour description of 63 PS3 1.10 functions (rows of
`maintainer-tools/research/u2-parity/groupF.tsv`: net-custom-games 8, net-matchmaking 24, party 31), measured
against the UCMR U2 runtime (`gow3_pc/src/u2mp`, branch `u2mp-ps3-parity-2`). It states what the PS3 game does as
seen by the players and by game memory, and whether our code gives the same result. It does not prescribe code.

Conventions as in `session.md` and `parity-c-match.md`: addresses without "PS3" are PS4 1.02 image offsets; NET_SM
is the s32 at `0x14ce97c` (PS3 `0xf11764`); "netInfo" = `0x104f1c8` (PS3 `0xf1fa48`); "match" = g_matchSession
`0x1065b08` (PS3 `0xf20c48`); "party" = the party session (PS3 `0xf2c8a0`); "NPD" = NetPlayerData `0x11474f8`
(PS3 `0xf2da60`); NGM = NetGameManager `0x113b760`. PS3 field offsets are given as interop facts (PS3 objects use
32-bit pointers, so PS4 offsets of the same field differ; the PS4 offset is named where known). Times: the net clock
counts 1/300 s (PORT-FRONTEND3 8); "s" below are real seconds.

All 26 sampled OPD descriptors of these functions carry TOC `0xe183b8`, so the decompile `groupF-ps3.c` and every
global/string resolved for this spec use the right TOC (the other-TOC issue of the flow module does not apply).
Ghidra dropped every loop over a session member list in this group (it took the list count as 0); the bodies were
read from the PS3 disassembly instead.

Status words: **COVERED** (same observable result already; file/function named), **PLATFORM** (PS3 plumbing that
the PC design replaces: Matching2 rooms, signaling, NP Basic, the PS3 mailbox; the visible result must still match,
deltas named), **MISSING** (visible behaviour we lack or do differently; exact difference named), **KEPT** (differs on
purpose by an owner rule).

Sections 1 and 2 are background every item uses; section 3 holds the items to implement (F1..F12); section 6 is the
status table for every row.

## 1. Facts every item uses

### 1.1 The PS3 mailbox (PS3 queue `0xf2bba0`, count `0xf2bb98`)

- One queue of at most 64 entries of 52 bytes: sender online id (36 bytes), sender connection id (u32 at +0x24),
  message code (u32 at +0x28), payload pointer (+0x2c), payload size (+0x30). Payloads are copied into their own heap
  block. A post fails when the queue is full or when payload + 4 bytes reach 512.
- Two producers: NP Basic messages from other consoles (the sender's online id is kept) and in-game NetEventMessage
  (type 46; the sender's connection id is kept). One consumer API: "peek the first entry with code C" then "take entry
  i" (copies the payload out when the caller's buffer is big enough, otherwise leaves the entry; then removes it and
  closes the gap so order is kept). Taking is done under a lock (the NP Basic handler runs on another thread).
- A debug dump (PS3 `0x4c2d18`) logs "%i messages : " then each code as "0x%x " when the queue is not empty at
  NetInit; it answers "not empty". No player-visible effect.
- PS4 kept the mailbox (`0x14d0e18`, SESSION-INTERFACE 1.5) and the readers in the intact pages. Our session layer
  posts the codes those pages read (`session.cpp mailbox_post`: 0x10e, 0x110) and carries everything else as
  session messages. This is the PLATFORM replacement for rows `4c2e08`, `4c3018`, `4c3258`, `4c3380`.

### 1.2 Sending helpers (PS3)

| PS3 | What it sends | To whom |
|---|---|---|
| `0x4c2bc0` | NP Basic message: 4-byte code then payload (< 508 bytes); on error logs "sceNpBasicSendMessage() failed = %x" and returns the error | one online id (friend / party invite paths) |
| `0x4c38f0` | in-game message (code, payload, size) | every other member of the party room |
| `0x4af160` | in-game message | every other member of the match room |
| `0x4c3764` | 0x103 {u64 room id} ("leave that room") | the party room's owner only |
| `0x4c3dc8` | 0x103 {u64 room id} | every other party member (when the party list can be read) |
| `0x4c370c` | 0x10d {u8 ok} ("I reached / did not reach the game host") | the party room's owner |
| `0x4c37c8` | 0x100 slot reservation request: the online ids of every party member (376 bytes) | the match room's owner (one connection) |
| `0x4c3e38` | the given code (0x102 or 0x113) with 48 bytes {room owner online id, member count, u64 room id}; optionally first reserves slots for every party member in the slot manager (PS3 `0xf11768`) | every other party member |

PC: `session::send_message(-1, ...)` (host relays in the star) covers "every other member"; online.cpp's
`u2o_send_message` covers NP Basic.

### 1.3 Mailbox codes used by this group (adds to PORT-MATCHMAKING 3.5)

0x100 / 0x101 reservation request / answer (answer byte 0 = 0 refused, else granted); 0x102 "join my custom game
room" (party leader -> members); 0x103 "leave room R" (host -> clients, leader -> members); 0x108 "the leader left
matchmaking" (leader -> members); 0x109 vote result {game, start time; 24 bytes}; 0x10a vote {choice}; 0x10b "tell me
your party room" (NP Basic, friend -> us); 0x10c "my party room is R" (NP Basic, us -> friend); 0x10d member -> leader
"reached the game host" {u8}; 0x10e leader -> members "the party is in" {u8 1 / 0}; 0x112 vote options {game a, game
b, deadline; 16 bytes}; 0x113 "join this matchmaking room" (leader -> members, 48 bytes); 0x116 "join this party room"
(NP Basic, pending room id).

### 1.4 The matchmaking slot list and vote block

- Slot list (PS3 `0xf21de8`; PS4 reader side `parity-b-menus.md` 2 / 11.6): already implemented as
  `session.cpp mm_list`. Entry: kind, voice record, member, "from the party session" flag, u64 party id, and a
  "how many listed entries share this party id" count (PS3 entry +0x18). Party-id colour list (+0x148, up to 10,
  count +0x198).
- Vote block (PS3 `0xf21dcc` = PS4 `0x113db50`): byte 0 open, byte 1 closed, +4 my vote (-1 none), +8 / +0xc the two
  games (s16 sign-extended to s32), +0x10 / +0x14 the two counts. Phase netInfo+0x34: 1 gathering, 2 voting, 3
  starting; deadline netInfo+0x20 (net clock).
- Static construction (rows `4af108`/`4af150`, `4af5bc`/`4af604`, `4b405c`/`4b40c8`, `4c4a30`/`4c4a78`): set the
  match / party session objects' class, empty the slot list, set three "selected index" words to -1 and fill the
  matchmaking tunables block (PS3 `0xf2264c`, `0x4b3f40`). PS4 initialises its own copies; nothing to port.
- Tunables block defaults that this group reads: +0x58 "forced minimum players" 0 (off), +0x68 a "no status line
  wait" byte 0, +0x6c a published value 0, +0x7f "balance teams" byte **1**, +0x80 "colour every row" byte 0. Its
  other fields (floats 10, 4, 1, 0.3, 1.5, 15, 45, int 60, float 0.25 at +0x24..+0x44) are not read by group F.
- Developer menu entries (rows `4b40d8` set +0x6c and republish member data; `4b414c` refresh the career data while
  online; `4b41c0` the host sends net event type 4): not reachable in the retail game, no player-visible behaviour.

## 2. What our port does today (summary)

- LAN and online share one star transport; the party leader hosts it, and the party leader is also the host of any
  match the party starts (`session.cpp`). Online, rooms decide who hosts (`online.cpp`, PORT-ONLINE 2).
- Custom game host: `host_create` + `create_match_now`, PS4 page handler `0x6093f0` runs 3 -> 10 -> 16 off
  match+0x99; members get `kFollow` (room id) and `on_follow` posts 0x10e so the intact JOINING page goes 12 -> 14 -> 17.
- Join from the list: `online.cpp select_join` (free-slot check from search data, then `u2o_join_room`), 20 s to reach
  the room owner (`kJoinTimeout`).
- Matchmaking: `frontend3.cpp` (vote, result, countdown, leave box) + `online.cpp mm_*` (one search of kind-3 rooms
  by key, pick the fullest room within 20 ranks, else create and gather).
- Party: `online.cpp` creates this PC's party room on entry (10 slots, auto owner grant); Join Party joins the
  friend's room from presence; host migration fallbacks (PORT-ONLINE 5a/5b).

## 3. Items to implement

### F1. Matchmaking gather: PS3 wait rule (PS3 `0x4b0a58` state 16, timing data PS3 `0xb40710`) - MISSING (timing), KEPT (minimum 2)

PS3 behaviour of the matchmaking room's host while NET_SM is 16 (SERVER_LOBBY) and phase 1:

1. When the host created its own matchmaking room ("GATHER", logged as a banner line), it records a gather
   reference time = now + 8 s. It is **not** reset when players arrive.
2. Every frame: the host's slot manager answers pending reservations (1.3, 0x100/0x101); the host counts the room's
   other members whose peer connection is active (status "established").
3. The wait for that count N: 0 s for N = 0, 4 s for 1, 8 s for 2, 15 s for 3, then 5 s more per player (20, 25, 30,
   35, 40 s for 4..8), 50 s for 9, and 0 s for 10 (a full room goes on at once). The count is re-read every frame,
   so a newcomer lengthens the remaining wait and a leaver shortens it. A debug line "Game starting in %i seconds"
   carries the remaining whole seconds.
4. Nothing happens before now >= reference + wait(N), and while the slot manager still has an open reservation.
5. Then the slot list is rebuilt sorted (F6) and N is compared with the playlist's minimum player count
   (PS3 `0x49a850`; tunable +0x58, when non-zero, also lets N >= that value pass):
   - enough: team balance check (F6), then the vote (room renamed, room closed and hidden, `0x4b0868` opens the vote,
     menu refresh, NPD career word +0xf4 = 1 and saved, NET_SM 20).
   - not enough: debug line "time out %i players" (N), the host tells its own party members to leave this room
     (0x103 with the room id, 1.2), leaves the room, NET_SM 35. State 35 waits until the room is left, resets the NGM
     and goes to NET_SM 19, which starts a new search from the first criteria (F5).
6. While gathering, a set "leave" request (F9) makes the host leave the room and go to 35 instead.

Ours (`frontend3.cpp host_gather_step`): online room: vote at 10 players, or after 20 s with every member ready, or
after 40 s; below the minimum: leave and search again after 45..60 s; the gather timer restarts on each join. LAN:
3 s minimum, 10 s maximum.

To do (online matchmaking rooms): replace the online timers by the PS3 rule above, with N = the room's other members
whose transport/peer link is up (`online.cpp peer_rtt` "up", or `session::member_count() - 1` for members that
completed the transport hello), reference = room creation + 8 s, never restarted. **KEPT:** the minimum stays ours
(`matchmaking_min_players()`: 2, co-op-only playlists 1; owner rule of issue 104 "PS3 timing, our minimum"). A failed
gather ends exactly as PS3: party told, room left, search again from criteria 1 (no extra 45..60 s delay). The LAN
path (no rooms) may keep its short timers (no strangers can come) - state it in the code comment.

Visible result: the status line keeps "Searching for players..." (phase 1) for the PS3 time, then "Voting ends in
[A]..." starts for everybody in the room at the same moment.

### F2. Custom game host: wait for the party before the lobby (PS3 `0x465da0`, `0x4c3b58`, `0x4c3d00`, `0x4c3e38`) - PLATFORM

PS3, page callback of the host's create-wait page (`69deab50`), NET_SM:

- 3: create status "done": netInfo room id (PS3 +0x58) = the new room's id; slot manager reset to 10 slots; each
  party member's join status cleared to "already in the room ? 1 : 0" (`0x4c3d00`); slots reserved for every party
  member and 0x102 {owner id, member count, room id} sent to them; NET_SM 10. Status "failed": pop page, NET_SM 18.
- 10: each 0x10d updates the sender's status (payload non-zero -> 1 "in", zero -> 255 "failed"). If any member is
  "failed": 0x10e {0} to the party, the host drops its connection to the room owner, log
  "A party member couldn't talk to the game host", pop page, NET_SM 18. If none is still 0: 0x10e {1} to the party,
  the lobby set-up call (PS3 `0x4895b8`), NET_SM 16. No timeout of its own (members time out themselves, F4).
- 16: only when the party has as many other members as the match room has other members: room data external value
  (0x53) = 1 and the lobby game task is played (every frame while that holds).

PS4 `0x6093f0` lost the party parts (10 is immediate, 16 plays the task without the count check). PC: the party leader
is the match host, so party members never fail to reach it; online, members join the match room after `kFollow`.
Parity requirement: (a) 0x53 = 1 only once every party member is in the match room (online.cpp `room_state` sets
it at the first SERVER_LOBBY; make it wait for the party's members to appear in the room member list, timeout = F4's);
(b) if a party member fails to join the match room (join refused / 20 s), the host shows nothing new but returns the
whole party to PAGE_MULTIPLAYER with NET_SM 18 and logs the PS3 line above. Everything else is COVERED.

### F3. Join a listed game: reservation and the Joining page (PS3 `0x465808`, `0x4661e0` states 7/9, `0x4c37c8`) - PLATFORM

PS3 order of effects after SELECT_JOIN_GAME on row i:

1. Debug lines "Selected game %i", "room %s", "Distance to Host %8.2f km"; netInfo gets the room's owner online id
   (+0x60, 36 bytes) and room id (+0x58); a connection to the owner is started; match connection id (PS3 +0x1174) =
   it. Started: PAGE_MULTIPLAYER_JOINING (`1af4bdd2`) is pushed **at once**, NET_SM 7. Not started: pop, NET_SM 18.
2. 7: connection failed -> drop it; a party member (not party owner) also sends 0x10d {0} to its leader; pop,
   NET_SM 18. Established: a party owner (also a player alone) sends the reservation request 0x100 with its whole
   party, deadline = now + **10 s**, NET_SM 9; a party member sends 0x10d {1} to its leader and goes to NET_SM 12.
3. 9: answer 0x101 byte 0 = 0 (no room for the party): pop, NET_SM 18. Granted: statuses reset, 0x102 to the party
   (members follow, F2 logic on the leader side), NET_SM 10. No answer by the deadline, or the connection lost:
   drop the connection, log "Joining Request timed out", pop, NET_SM 18.
4. 10 (leader): as F2 state 10, but success = 0x10e {1} to the party, then JoinRoom -> NET_SM 14; JoinRoom refused:
   leave the party (party "leave" flag, 3.7), drop the connection, log "sceNpMatching2JoinRoom() failed. ret = 0x%x",
   pop, NET_SM 18.
5. 12 (member): 0x10e {0}: drop the connection, pop, NET_SM 18. {1}: NGM reset, JoinRoom -> 14 (refused as in 4).
6. 14: joined -> menu reset, the client lobby wait page (`69deab50` with handler PS3 `0x465b70` = PS4 `0x609d00`),
   the lobby set-up call, NET_SM 17. Join failed: drop the connection, leave the party, pop, NET_SM 18.

No message box appears on any failure: the player is put back on the page below (the list / Multiplayer).

Ours: free-slot check from the search result (refusal only logged), `u2o_join_room`, then 20 s to reach the owner;
the Joining page appears only when the owner's follow arrives. Deltas to implement:
(a) push PAGE_MULTIPLAYER_JOINING at the moment of selection and keep it until the follow (17) or a failure;
(b) a failure (no free slots, join refused, owner not reached) pops it and sets NET_SM 18, as PS3;
(c) time limit: 10 s from "link to the owner up" for the owner to admit us (our transport welcome = PS3 grant), on
top of the link set-up limit (keep 20 s for the link); (d) a party that does not fit is refused by the owner, not only
by stale search data (the owner checks free slots when the leader's request arrives).

### F4. Party members following into a game (PS3 JOINING 12 -> 14, `0x4c370c`) - PLATFORM

Covered by `kFollow` + `on_follow` (posts 0x10e {1}) for the visible path. PS3 also sends 0x10e {0} when the
leader's attempt fails; members then pop the Joining page and go to NET_SM 18. Ours: a leader whose join fails sends
nothing; its members stay in the party lobby (they never left it). Same visible end (members on PAGE_MULTIPLAYER);
nothing to add unless F2(b) or F3 make the leader abort after members were told to follow - then send them the abort
(a session message that makes the member post 0x10e {0} if its Joining page is up).

### F5. Matchmaking search: criteria rows, retry pause, ping check (PS3 `0x4b0a58` states 19/4/5/6/7/9) - MISSING (partly PLATFORM)

PS3:

1. 19 (START_MATCHMAKING), leader or solo: net version key and DLC mask into the search request, debug line
   "Searching Game with Criteria %i" (criteria index, PS3 `0xf22630` = PS4 tip index `0x113ddac`), NET_SM 4 then the
   search. The criteria come from the data table `b25ab071` (one row per step; the status line shows row[index]'s
   text, e.g. "Searching for Close Game" / "Optimal Game" / "Any Game"; each row also has a ping limit at +8).
   A leader whose party may not search (PS3 `0x49a738`) goes straight to GATHER (F1). (NPD +367 bit 0, read next to
   it, is the Profile's Matchmaking Default / DLC setting: it only picks the room key, `parity-h-session.md` H5.)
2. 5: results listed (debug "%s, %i players %8.2f km" per room) -> 6.
3. 6: pick the best room: skip full rooms and rooms already marked bad; score by distance, with a penalty when the
   room creator's party size bits overlap ours; keep the lowest. None: wait a random **1..3 s**, then criteria index
   + 1 and search again (state 4); after the last row: GATHER (create own room, F1). Chosen: debug
   "*********** Joining Room %s %i players", connect to the owner -> 7.
4. 7: connection established -> read RTT and bandwidth (debug "retRtt = ..."); RTT above the row's ping limit:
   "PingCriteria failed %i < %i", drop the connection, mark the room bad, back to 6. OK: reservation request,
   deadline now + **6 s**, NET_SM 9 (matchmaking uses 6 s where custom join uses 10 s). Failed connection: mark bad, 6.
5. 9 / 12 / 14: as F3, then 17 (CLIENT_LOBBY) or 21 when a vote is already open.
6. Party members of a searching leader wait in 19 for 0x113 {room}; then connect to that room's owner (NET_SM 7,
   line "CONNECT_TO_HOST").

Ours: one "any game" search; best = within 20 ranks, then fullest; no retry pause; no RTT check; no criteria text.
To do: (a) criteria steps: three passes with the tip index advanced per pass (so the status line shows the PS3 step
text), each pass a search, a random 1..3 s pause between passes, GATHER after the last; pass 1 = rank within 20 and
RTT limit of row 1, pass 2 = row 2, pass 3 = any (distance: PLATFORM, no location on PC - say so in the log);
(b) after the link to the chosen owner is up, compare our measured RTT (`peer_rtt`) with the row's limit; too slow:
leave, mark bad, next room; (c) the reservation window 6 s. Row values (ping limits) are data in `b25ab071`; read
them at run time, do not copy them.

### F6. Team balance before the vote (PS3 `0x4b0a58` state 16, sort `0x4af6d0` with order `0x4af524`) - MISSING

PS3, when the gather wait ended with enough players (F1 step 5) and the tunable "balance teams" (+0x7f) is on
(default on), the connected count N is odd and the playlist's maximum player count is even (team playlists): the
slot list is sorted (ascending by "entries sharing the party id", then by party id, then by position), and the
**last** entry is looked at. If that player came alone (share count 1), the host hides/closes the room and kicks that
player from the room (KickoutRoomMember with the member id), then keeps gathering (the wait rule of F1 continues).
If the last entry is part of a party, the vote starts with the odd count.

Ours: no such step. To do (online rooms only): same decision on `mm_list()` order as above; the kicked stranger gets
our kick session message (`kKick`), leaves the room and searches again (their game: "matchmaking: removed to balance
the teams", NET_SM 19). The kicker reopens the room after the kick (PS3 hid it; it is shown again when gathering
continues because the next vote hides it anyway - check `room_state`).

### F7. Vote ends early on a majority (PS3 `0x4afe48`, used by the host in state 20) - MISSING

PS3 rule with n = the other members of the match room, h = n / 2 (integer): a game wins as soon as its vote count
is greater than h; if neither does and the two counts add up to n with both equal to h, the winner is picked at random;
otherwise there is no winner yet. The host checks this while voting (state 20); a winner is applied at once with
`0x4afee8` (SetNetGame, phase 3, vote closed, result 0x109 to the room, level preload): the vote screen jumps to
"Game starting in [A]..." without waiting for the 15 s. When the deadline passes first, the higher count wins, ties
random (as ours).

Ours: `frontend3.cpp end_vote` only at the deadline. To do: evaluate the rule above every host update during phase 2
(n = `session::member_count() - 1`), end the vote early with the same result message.

Open question 1 (answered, see "F7 clarification" in section 5): the gate is not a record field. The host takes this
branch only while NGM has **no** current net game (PS3 `0x499908` returns NULL); once a game is set it skips it.

### F8. Voice of strangers in matchmaking (PS3 `0x4b0700`, `0x4b0868`, `0x4c40f0`) - MISSING

PS3: when the vote opens (host and clients), every slot-list entry that is **not** from our party and has a voice
record is unmuted (cellVoiceSetMuteFlag 0). When a member joins our party, it is unmuted too (and its connection
activated; debug "%s joined party"). So strangers are silent while gathering and audible from the vote on. (Where PS3
mutes them on arrival is outside this group: open question 2.)

Ours: everyone on the transport is audible. To do (`voice.cpp` + `frontend3.cpp`): in an online matchmaking room,
players not in our party start muted (a mute separate from the player's own mute choice, not shown in the mute icon
query); unmute them all when phase 2 begins; party members are never muted by this rule.

### F9. Leaving matchmaking: leader versus member (PS3 `0x4b0a58` events 2/3, state 19, `0x4af4fc`) - MISSING

PS3:

- Back on the matchmaking page: if no leave was confirmed yet, the back is held and "Leave Matchmaking?" (Yes / No)
  appears. After Yes (leave flag set) a back is let through only in NET_SM 18 or 19.
- With the leave flag: gathering (16) / lobby (17) / vote (20, 21) leave the match room -> 35 -> 19. In 19:
  - the **leader** sends 0x108 to its party (members set their own leave flag and do the same) and returns to the
    party lobby (PAGE_LOBBY `b99a3235`); the party stays together;
  - a **member** who pressed back itself also leaves the party (party "leave" flag, 3.7) and goes to PAGE_LOBBY
    alone; a member that only followed its leader's 0x108 stays in the party.
- A network error (netInfo error byte) sets the leave flag too.

Ours (LAN): any member's Yes ends matchmaking for the whole party (`kMsgLeave` -> host `leave(true)`). Online:
a member backing out leaves only itself (matches PS3). To do (LAN): a member's Yes makes that member leave the party
(close its transport as Leave Party does, own lobby), the others continue; the leader's Yes keeps today's behaviour
(party back to the lobby, `kMsgLeave` to all).

### F10. Host gone in the matchmaking client lobby (PS3 `0x4b0a58` state 17) - MISSING (owner decision)

PS3, a member in NET_SM 17: host tracker marked gone (NGM host +0x234 PS3) -> log "********* HOST %s LEFT
***********", leave the room, 35 -> 19: search again (a party member waits for its leader). Same for the "kicked"
byte (PS3 `0xf119c4`, PS4 `0x113a270`) and the leave flag. Message 0x103 for our room from the host: log
"Host Asked to leave the room %i", drop the host connection, leave, 35; the party leader also forwards 0x103 to its
members.

Ours: lobby host migration (the room's next owner hosts, matchmaking continues in the same room; PORT-ONLINE 5a). That
is a PC extra. Owner decision: keep migration (KEPT) or follow PS3 (leave and search again). If kept, log it as a
deliberate difference.

### F11. Party room machine (PS3 `0x4c41f8` with states at party +0x1178) - MISSING (two parts)

PS3 states: 0 idle -> 1 decide -> 2 creating own party / 5 joining another -> 6 in party -> 7 leaving -> 0; 8 error
(NET_SM_PARTY_ERROR, logged, stays until the session is re-initialised). Every frame first: a 0x116 message stores
a pending party room id (+0x1180).

- 1: network up; a pending room -> join it (5); none -> create our own party room: 10 slots, owner auto grant, search
  key = net version | 0x100000. **Team preference**: if NPD team (PS4 NPD+0x114) is 0, pick 1 or 2 at random and
  publish it in member data.
- 2: created -> if a friend asked for our room meanwhile (0x10b stored), answer it with 0x10c {room}; -> 6. Failed -> 8.
- 5: join failed -> back to 1 (own party). Joined -> **team auto-pick**: count the party members' published teams
  (member data byte +9: 1 or 2); our team = the team with fewer members, a random team on a tie; publish -> 6.
- 6: 0x10b from a friend -> answer 0x10c {our party room}. If we are not the owner: the connection to the owner must
  be up; if its record is missing or it stays "not established" for more than **5 s**: debug
  "Couldn't talk to the party host %s" and leave. Also leave on a pending room (switch party) or the party "leave"
  flag (3.7). Leave = room left -> 7 -> 0 -> 1 (own party again).

Ours: party room on entry, join from presence, 20 s owner timeout, host migration. Missing visible parts:
(a) team auto-pick on joining a party (members of a party are spread over the two teams; custom game team rows show
it); `online.cpp join_succeeded` (party kind) should set NPD+0x114 from the room members' blob byte +9 as above and
republish; random 1/2 when creating our own party while NPD+0x114 is 0; (b) party owner unreachable: PS3 gives up
after 5 s of a known-but-not-established link to the owner (ours 20 s; keep 20 s for the first contact, use 5 s once
the room is joined and the owner's peer record exists - PS3 timing).

### F12. Joining a friend's party: NP handshake (PS3 0x10b / 0x10c / 0x116, `0x4c36b0`, `0x4c36e8`) - PLATFORM

PS3 Join Party asks the friend (0x10b); the friend's party machine answers 0x10c with its party room; the asker
stores it as pending (`0x4c36b0`: pending room, invite flag +0xb0 cleared) and switches party (F11). Invite To Party
with no party yet first starts the party machine (`0x4c36e8`: state 0 -> 1, "Creating Party..." box). Ours: the
friend's room id comes from presence and the room is joined directly; the invite carries the room. Same visible
result (the player ends in the friend's party lobby). Nothing to add.

## 4. Test scenarios

1. F1: two PCs online, playlist with minimum 2: B joins A's gathering room 5 s after creation; the vote opens at
   creation + 12 s (8 + 4). Third player at +10 s: vote at +16 s (8 + 8). Alone: at +8 s "time out 0 players", room
   left, search starts again with criteria 1 (no 45 s wait).
2. F2/F4: leader with one member starts a custom game online; 0x53 stays 0 until the member is in the match room; a
   member whose join is refused -> whole party back on PAGE_MULTIPLAYER, NET_SM 18, the PS3 log line.
3. F3: select a full room: Joining page shown, then popped, NET_SM 18, no box. Owner silent after link up: pop after 10 s.
4. F5: no rooms: three searches 1..3 s apart, the status line text changes per pass, then own room. Room with RTT
   over row 1's limit: "PingCriteria failed", next room.
5. F6: team playlist, 3 players with the third alone: the third is removed before the vote; with a 2-party third
   entry: vote with 3.
6. F7: 3 players, 2 vote game a: result at once ("Game starting"), not at the 15 s mark. 2 players, 1 vote each: random
   winner at once.
7. F8: stranger's voice not heard while gathering, heard from the vote on; party member always heard.
8. F9 (LAN): member answers Yes: only that member leaves (own lobby); leader answers Yes: all back to the lobby.
9. F11: B joins A's party where A is team 1: B gets team 2; C joins: random or the smaller team.

## 5. Open questions

1. ANSWERED (2026-11-25): F7: what the net-game record field read by PS3 `0x499908` means (decides whether the
   majority rule or the plain timer ends the vote). See "F7 clarification" below.
2. ANSWERED (2026-11-25, `parity-j-voice-medals.md` J3): F8: where PS3 mutes non-party players when they arrive in a
   matchmaking room: the connection record reset mutes every new connection; implemented as one mute flag.
3. F1: whether "other members with an active connection" excludes members still in the reservation stage (PS3 checks
   the open-reservation flag of the slot manager before deciding; our transport has no reservation stage, so count
   members that completed the hello).
4. F5: the three rows of `b25ab071` (texts, ping limits, distance tolerances): read from the game data at run time;
   confirm the row count is 3 on U2 as it is on U3.
5. F10: owner decision (keep lobby migration or PS3 leave-and-search).
6. 4c36c8 / 4c4a88 ("leave the party" flag at party +0x1174 PS3): PORT-FRONTEND3 9 said not needed; F3/F9 now use
   the behaviour (a player whose game join failed, or a member who left matchmaking, leaves its party). Confirm with
   the owner that a failed join should drop the player out of its party as on PS3.

**F7 clarification (question 1, PS3 1.10 GOTY EBOOT).** `0x499908` reads no field of a record. It takes NGM, reads
NGM's current net-game index (s16 at PS3 NGM `+0x18EC`, -1 = none), and returns the address of that entry of the
`*net-games*` table (PS3 record stride 0x80), or NULL when the index is negative or not below the table count. The
only writer of `+0x18EC` is the SetNetGame core `0x49c458`; the matchmaking machine `0x4b0a58` sets the index to -1
when it starts, and `0x4afee8` (the vote result) sets it to the winning game. In host state 20 the early majority
check (`0x4afe48`) runs when `0x499908` returns **NULL**, i.e. while no game has been chosen yet in this vote; when it
returns a record (a winner is already set) the host takes the other branch and does not decide again. So the gate is
"current net game index is -1 / out of range", the same for every net game; there is no per-game flag and no list of
net games "that have it set". PS4 1.02 equivalent: the same index at PS4 NGM `+0x1ca0` (s16, -1 none; PS4 record
stride 0x90), as already read by `natives.cpp current_net_game` and `frontend3.cpp current_net_game`. Note for the
implementer: ours (`early_vote_allowed` = a record exists) is the inverse of PS3; it must be true while the current
net game is -1 (or out of range) during phase 2.

## 6. Status table

| PS3 | Role (what it does) | Status | Where (our code / PS4) / item |
|---|---|---|---|
| 0x4654d0 | PAGE_MULTIPLAYER_JOIN: on search failure pop + NET_SM 18 | COVERED | PS4 `0x609640` + online.cpp match+0xa8/+0xac |
| 0x4655b0 | BUTTON_MULTIPLAYER_JOIN: reset, search custom rooms, push the list page, NET_SM 5 (failure: log, NET_SM 18) | COVERED | PS4 `0x6095d0` + remaining.cpp Invites -> online.cpp `open_join_list` |
| 0x465808 | SELECT_JOIN_GAME: owner/room into netInfo, connect, Joining page, NET_SM 7 | PLATFORM | frontend2.cpp / online.cpp `select_join`; F3 (a) |
| 0x465b70 | client lobby wait page: error/0x103/host-gone/kicked -> LEAVING_GAME; LoadLevel flag -> play match task | COVERED | PS4 `0x609d00` intact (TSV "absent" is wrong); host loss -> netInfo+0x1c |
| 0x465da0 | custom host create page: 3 -> 10 (party wait) -> 16 (0x53 = 1, lobby task) | PLATFORM | PS4 `0x6093f0` + session.cpp `create_match_now`; F2 |
| 0x4661e0 | PAGE_MULTIPLAYER_JOINING: states 7/9/10/12/14 | PLATFORM | PS4 `0x609790` + session.cpp `on_follow`; F3, F4 |
| 0x466ec4 | getter of a module global used by the PS3 voice code | PLATFORM | voice.cpp own design |
| 0x466ee0 | per-8-byte pass over a packet buffer that leaves it unchanged | PLATFORM | net.cpp transport own design |
| 0x4af108 | static init: match session class, a -1 word | COVERED | PS4 own init |
| 0x4af150 | static-init thunk of 0x4af108 | COVERED | - |
| 0x4af160 | send a message to every other match room member | COVERED | `session::send_message(-1)` |
| 0x4af240 | publish member data (48 bytes) to match and party rooms | COVERED | progression.cpp `0x64e810` + online.cpp `member_blob` (+10..+47 zero, no visible use) |
| 0x4af4fc | "Leave Matchmaking?" Yes: set the leave flag | COVERED | frontend3.cpp `leave_yes`; leader/member split F9 |
| 0x4af524 | order of the slot list sort (share count, party id, position) | COVERED | session.cpp `balance_kick_slot` (F6) |
| 0x4af568 | position of a party id in the colour list | COVERED | session.cpp `party_position` |
| 0x4af5bc | static init: slot list empty, a -1 word | COVERED | - |
| 0x4af604 | static-init thunk of 0x4af5bc | COVERED | - |
| 0x4af614 | find the slot-list entry of an online id | COVERED | session.cpp `mm_list` / `mm_member_at` |
| 0x4af6d0 | generic in-place sort (slot list) | COVERED | session.cpp `balance_kick_slot` (std::stable_sort, F6) |
| 0x4afe48 | vote winner by majority / full tie random / undecided | COVERED | frontend3.cpp `majority_winner` / `host_vote_step` (F7; gate see 7) |
| 0x4afee8 | host applies the vote result (SetNetGame, phase 3, 0x109, preload) | COVERED | frontend3.cpp `end_vote` / `apply_result` |
| 0x4b0008 | build the matchmaking slot list | COVERED | session.cpp `mm_list` (parity-b 11.6); sorted copy F6 |
| 0x4b0670 | LIST_MATCHMAKING_VOTE press: my vote, count + 1, 0x10a to the room | COVERED | frontend3.cpp vote row + kMmVote |
| 0x4b0700 | client opens the vote (block, phase 2, unmute strangers, preload) | COVERED | frontend3.cpp `open_vote` -> voice.cpp `unmute_strangers` (F8, `parity-j` J3) |
| 0x4b0868 | host opens the vote (two games, 0x112, block, unmute strangers, preload) | COVERED | frontend3.cpp `start_vote` / `open_vote` (F8) |
| 0x4b0a58 | PAGE_MULTIPLAYER_MATCHMAKING machine | COVERED (PLATFORM parts, KEPT minimum) | F1, F5, F6, F7, F9, F10: frontend3.cpp / online.cpp `mm_*` (see 7) |
| 0x4b3f40 | matchmaking tunables defaults | COVERED | values used as constants by F1/F6 (1.4) |
| 0x4b405c | static init: -1 word + tunables | COVERED | - |
| 0x4b40c8 | static-init thunk of 0x4b405c | COVERED | - |
| 0x4b40d8 | developer menu: set tunable +0x6c and republish | COVERED | not in retail (1.4) |
| 0x4b414c | developer menu: refresh career data while online | COVERED | not in retail |
| 0x4b41c0 | developer menu: host sends net event type 4 | COVERED | not in retail |
| 0x4c2bc0 | NP Basic send (code + payload < 508 bytes) | PLATFORM | online.cpp `u2o_send_message` |
| 0x4c2d18 | debug dump of the mailbox | COVERED | log only |
| 0x4c2e08 | mailbox take (code, payload; keeps order) | PLATFORM | PS4 mailbox readers + session.cpp `mailbox_post` (1.1) |
| 0x4c3018 | mailbox post with sender online id | PLATFORM | session messages (1.1) |
| 0x4c3258 | mailbox post with sender connection id | PLATFORM | session.cpp `mailbox_post` |
| 0x4c3380 | mailbox take with sender online id | PLATFORM | session messages (1.1) |
| 0x4c36a8 | empty listener slot | COVERED | - |
| 0x4c36ac | empty listener slot | COVERED | - |
| 0x4c36b0 | set the pending party room to join, clear the invite flag | PLATFORM | online.cpp `join_friend` / `join_by_id`; F12 |
| 0x4c36c8 | request "leave the party room" (when in one) | PLATFORM | online.cpp `own_party`; used by F3/F9 (question 6) |
| 0x4c36e8 | start the party machine if idle | PLATFORM | online.cpp party room on entry; F12 |
| 0x4c3708 | empty listener slot | COVERED | - |
| 0x4c370c | member -> leader 0x10d "reached the game host" | PLATFORM | party host = game host; F4 |
| 0x4c3764 | 0x103 {room} to the party owner | PLATFORM | frontend3.cpp kMmLeave |
| 0x4c37c8 | reservation request 0x100 with the party's ids | PLATFORM | F3 (d), F5 (c) |
| 0x4c38f0 | send to every other party member | COVERED | `session::send_message(-1)` |
| 0x4c39d0 | thunk to a session method | COVERED | - |
| 0x4c39f4 | thunk to a session method | COVERED | - |
| 0x4c3a18 | party member left: debug "%s left party" | COVERED | session.cpp "party: %s left" |
| 0x4c3ac0 | party teardown: leave the room | COVERED | online.cpp `on_transport_closed` |
| 0x4c3b00 | party session init (clears its party fields, state 0) | COVERED | online.cpp start |
| 0x4c3b58 | leader: apply 0x10d answers, report "someone failed" / "all in" | PLATFORM | F2 |
| 0x4c3d00 | leader: reset members' join status from room membership | PLATFORM | F2 |
| 0x4c3dc8 | 0x103 {room} to every party member | PLATFORM | frontend3.cpp kMmLeave relay; F1 step 5 |
| 0x4c3e38 | 0x102/0x113 room info to the party, optional slot reservation | PLATFORM | session.cpp kFollow / online.cpp follow; F2 |
| 0x4c40f0 | party member joined: debug "%s joined party", activate link, unmute | COVERED | online.cpp `connect_member`; voice default on (F8 for strangers) |
| 0x4c41f8 | party room machine (create / join / owner check / leave / error) | COVERED | online.cpp `create_room` / `party_team_pick` / `party_owner_tick` (F11) |
| 0x4c4a30 | static init: party session class, a -1 word | COVERED | - |
| 0x4c4a78 | static-init thunk of 0x4c4a30 | COVERED | - |
| 0x4c4a88 | request "leave the party room" (second class slot) | PLATFORM | as 0x4c36c8 |
| 0x4c4aa0 | class constant 23 | COVERED | PS4 own class |

Counts (2026-11-25, after section 7): 42 COVERED, 21 PLATFORM, 0 MISSING; KEPT: the minimum players of F1 (owner rule); F10 decided (PS3 behaviour). F7 has one open fact (the net-game record field, 7).

## 7. Implementation (2026-11-25, branch `u2mp-ps3-parity-2`)

Owner decisions: **minimum players** (F1 and every place a minimum decides whether matchmaking may begin): 2 in
every game mode, 1 in co-op (types 4, 8, 9, 12) - frontend3.cpp `matchmaking_min_players` (`start_rules.h`
`playlist_min_players`). Owner rule 2026-11-25: the minimum is for **matchmaking only**; a custom game starts with
1 player in every game mode (session.cpp `custom_start_ok`, `start_rules.h` `may_start(kCustom, ..)`; PS3 asked 2
for type 10 and co-op; in the match: solocoop.cpp, `parity-n-inlined.md` NI2). PS3 wait times otherwise.
**F10**: PS3 behaviour (no lobby host migration in matchmaking). **Question 5/6**: a player whose game join failed
drops out of its party (PS3 behaviour).

| Item | Status | Where / what |
|---|---|---|
| F1 | COVERED, KEPT minimum | frontend3.cpp `host_gather_online`: reference = room creation (kMmHosting) + 8 s, never restarted; wait table `kGatherWait` by N = `session::member_count() - 1` (players that completed the transport hello); no decision while `online::mm_pending_members()` (a room member joined < 6 s ago and is not on the transport yet = PS3 open reservation); "Game starting in %i seconds" when N changes; too few: "time out %i players", `mm_research`, NET_SM 19, criteria 1, no extra delay. LAN keeps 3 s / 10 s (no strangers). PLATFORM: party members of a gathering host are not members of its RPCN room on PC, so there is nobody to send 0x103 to. |
| F2 | COVERED | online.cpp `room_state`: a custom game room sets 0x53 = 1 only when every transport member (`session::member_npids`) is in the match room; otherwise waits up to 20 s, then `abort_custom_lobby` ("A party member couldn't talk to the game host", kOpPartyIn {0} to the party, room left, session.cpp `online_match_abort`: match+0x99 = 0, match session cleared, PAGE_MULTIPLAYER, NET_SM 18). A member's refused join sends kOpJoinResult {0} (PS3 0x10d) and the leader aborts at once. Members of an online custom game wait in JOINING 12 (session.cpp `on_follow`, `online::follow_match_room` returns true) until kOpPartyIn {1} (PS3 0x10e) posts 0x10e; {0} posts 0x10e {0} (page pops, NET_SM 18) or, from 17, back to PAGE_MULTIPLAYER; no word in 25 s: they give up themselves. |
| F3 | COVERED (PLATFORM order) | online.cpp `select_join`: "Selected game", JOINING shown at once with NET_SM 12 for a custom row; failures (`join_page_fail`) post 0x10e {0}: no free slot by the search data, join refused / closed room, no owner, party does not fit by the room service's count at our join (fresh, = the owner's refusal of (d)), owner not reached ("Joining Request timed out"). Admit limit 10 s after the link to the owner is up (`admit_armed`, link limit 20 s). JoinRoom refusals and owner-not-reached drop the player out of its party (`drop_out_after_failed_join`, question 5); a party that does not fit only goes back (PS3 state 9). No message boxes. |
| F4 | COVERED | the abort is kOpPartyIn {0} (F2). A member that cannot follow its leader (JoinLeader refused) leaves the party and goes back to PAGE_MULTIPLAYER with NET_SM 18, no error box (before: network error box). |
| F5 | COVERED (distance PLATFORM) | frontend3.cpp `start_search_pass` / `host_start_matchmaking_step`: passes = rows of `b25ab071` (at most 3, read at run time; stride 0x14, ping limit s32 at +8), "Searching Game with Criteria %i", tip index = pass, created-by-me 0 while searching; online.cpp `mm_search_done` / `mm_pick`: pass 1 only rooms within 20 ranks, the fullest room wins (no location: no distance score, no party-size penalty); `mm_check_tick`: after the RPCN join the link to the owner comes up (20 s), RTT vs the row's limit ("retRtt", "PingCriteria failed"), too slow / failed -> room marked bad, next room of the same pass (`mm_room_failed`); only then the transport and the party go to the owner, which must admit within 6 s. No room: kMmNoRoom -> random 1..3 s -> next pass; after the last: `mm_create` (GATHER). The row's limit applies on every pass (PS3 state 7). |
| F6 | COVERED | frontend3.cpp `host_gather_online` + session.cpp `balance_kick_slot`: N odd, playlist maximum even (not co-op only), last of the sorted slot list alone -> kMsgBalanceKick to it, 5 s quiet, gathering goes on; the kicked player: "removed to balance the teams", room marked bad, searches again (`online::mm_host_gone(.., true)`). The room is not hidden for the kick. |
| F7 | COVERED, one open fact | frontend3.cpp `majority_winner` with n = all players of the room (`session::member_count()`), h = n / 2. Note: the rule text says "other members", but only n = all players gives scenario 6 (2 players, 1 vote each -> random) and a real majority; implemented so. Gate: `early_vote_allowed` = the current net game has a `*net-games*` record (PS3 `0x499908` returns that record); **PS3 gates the other way: early end only while there is NO current net game (index -1), see "F7 clarification" in section 5; to fix.** |
| F8 | COVERED | Since `parity-j-voice-medals.md` J3 one mute flag per player (`voice::muted`): voice.cpp mutes a new connection that is not a `session::party_member`, unmutes it when it joins our party, and frontend3.cpp `open_vote` unmutes every present non-party player (`voice::unmute_strangers`). As PS3: the muted icon and "Unmute" show for a stranger while the room gathers, a hand unmute works, a stranger stays heard after the vote for as long as the connection lives, one who arrives after the vote stays muted until the next vote (question 2 answered: PS3 exactly). |
| F9 | COVERED | frontend3.cpp `update` (leave branch) + `leave(.., Exit)`: transport host (LAN host, gather host, solo) -> kMsgLeave, its party back to the lobby, strangers search again; a member (LAN too) -> `session::leave_party` (online `drop_out_of_party`; LAN: transport closed, UDP host of its own); a leader inside a stranger's room -> kMsgPartyLeave {party id} relayed by the host (PS3 0x108), leaves 0.3 s later; its members `mm_leave_to_leader` (room left, transport back to the leader, lobby). The host only logs a member's kMsgLeave now. |
| F10 | COVERED (owner decision PS3) | online.cpp `on_host_lost` (transport host lost), MEMBER_LEFT of the host, kMsgLeave from a stranger host ("Host Asked to leave the room") -> `mm_host_gone` -> next frame "HOST %s LEFT", room left, then alone / leader: own party + `matchmaking::on_host_left(true)` (NET_SM 19, criteria 1; a leader waits up to 10 s for `party_missing()` members to come back); party member: transport back to the leader, waits in 19. The party room is now kept while in a matchmaking room (`join_succeeded`), so party ids, colours and the leader are known there. In-match host handling unchanged. |
| F11 | COVERED | online.cpp `create_room` (party, team 0 -> 1 or 2 at random), `party_team_pick` (joined a party: the team with fewer members by member data +9, tie random, NPD+0x114, republish), `party_owner_tick` (after the link to the party owner was up once, 5 s without it: "Couldn't talk to the party host", party room left, own party when it hosted us; first contact keeps 20 s; an owner on the unreachable list is left at once, parity-i-connection.md I4). |

Not possible without a new online call: none needed. All PS3 messages travel as session messages (transport) or
room events; the ABI and the online program are unchanged.

Remaining differences from PS3: no distance / region in the criteria (no location on PC) and no party-size penalty in
the room score (fullest room instead); the slot reservation is the RPCN join + transport admit, so a player is briefly a
room member before a ping or fit refusal; F7's gate (inverted vs PS3, see F7 clarification); F8 re-mute after a match; a leader that drops out after a
refused list join leaves its members without a host (no party migration on PC: they get the host-lost path); a leader
not admitted after its party was told to follow leaves the members in that room on their own.
