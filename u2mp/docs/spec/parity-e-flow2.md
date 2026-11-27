# Spec: PS3 parity, group E (flow.cpp part 2: TaskManagerOnline states, NET_SM transitions, page handlers, per-frame net update, NetInit; slot manager)

Clean-room parity check of the 86 PS3 1.10 functions of `maintainer-tools/research/u2-parity/groupE.tsv`
(74 flow.cpp rows `0x43ba98..0x449e88`, 12 slot manager rows `0x4d987c..0x4da504`) against the UCMR U2 runtime
(`gow3_pc/src/u2mp`, branch `u2mp-ps3-parity-2`). It says what a PS3 player saw and what our port must show; it does
not prescribe code, structure or names. Addresses are PS3 1.10 (`NPUA80698` EBOOT) unless marked PS4 (PS4 1.02 image
offset). Background: `FUNCTIONS-FRONTEND-NET.md` 3 / 4a, `PORT-GAMEPLAY3.md` 1 (state vtables), `PORT-MATCHMAKING.md`
2.2 / 3 / 5.2, `SESSION-INTERFACE.md`, `TRANSPORT-INTERFACE.md`, `NETINIT.md`, `MENUS.md`, specs `session.md`,
`remaining.md`, `parity-b-menus.md`, `parity-c-match.md`, `parity-d-flow1.md` (group D, flow.cpp part 1).

Status words (as in `parity-c-match.md`):

- **COVERED**: our code, or the intact PS4 code it relies on, gives the same visible result.
- **PLATFORM**: PS3 plumbing (PSN, NP Basic, Matching2, signaling, NetCtl, sockets, threads, Naughty Dog servers,
  debug output) that the PC design replaces. Only the visible result has to match; the replacement is named.
- **MISSING**: absent, or present with a different visible result in the part named.

## 0. Facts every item uses

**Module data.** Both PS3 modules of this group address their data through a base word, not through the Ghidra
default TOC `0xe183b8`: flow.cpp functions use the word at `0xe10adc` (= `0xdb82b8`), the slot manager functions the
word at `0xe10b80` (= `0xdbb66c`). Every global, constant and string named below was resolved through the right base
(the group decompile shows the offsets relative to those words; nothing in it had been resolved with the wrong base).

| PS3 | meaning | PS4 / ours |
|---|---|---|
| `0xf11764` s32 | NET_SM | PS4 `0x14ce97c` |
| `0xf0e800` | menu manager | PS4 `0x1136640` |
| `0xf1fa48` | netInfo; `+0x1c` network error, `+0x2c` NAT type, `+0x34` matchmaking-mode flag, `+0x58` current match room handle, `+0x288` own NpId, PS3 `+0x2b4` page mode | PS4 `0x104f1c8`; room handle PS4 `+0x60`; page mode PS4 `+0x2c4` |
| `0xf20c48` / `0xf2c8a0` | match session / party session | PS4 `0x1065b08` / `0x105efb8` |
| `0xf1dc80` | NetGameManager (game state `+0x1a8c`: 0 none, 2 playing, 4 round results; end reason `+0x1a90`; "new host" id `+0x1a94`) | PS4 `0x113b760` |
| `0xf116d4` / `0xf116d0` | net clock object pointer (ticks at object `+0xc`, 300 per second) / this machine's synchronised net time word | PS4 `0x113a040`; session.cpp owns the object (`session.md` 5.12) |
| `0xf119c4` u8 | match abort byte | PS4 `0x113a270` |
| `0xf2da60` | NetPlayerData (profile) | PS4 `0x11474f8` |
| `0xf1db98` | friends list (count `+0`, entries of 0x264 bytes from `+8`, busy word `+0xb0`) | online.cpp friend list |
| `0xf21de8` | match player list of the lobby / postgame (count `+0x140`, entries of 0x20 bytes: `+4` voice record, `+8` member, `+0xc` "is me") | session.cpp `mm_member_at` (matchmaking slot list) |
| `0xf3a060` | connection manager (32 peer records; `+6` dead, `+0x17c` voice port volume) | net.cpp / voice.cpp |

**Player trackers.** PS3 trackers are 0x270 bytes from NetGameManager `+0x10`; PS4 trackers are 0x2c0 bytes from
NetGameManager `+0x10`. Fields used here (PS3 offset / PS4 offset): used `+0x18 / +0x20`; net id `+0x118`;
connection handle `+0x204`; NpId (36 bytes) `+0x208`; party id `+0x11c / +0x124` (PS4 `+0x128` is the rank, 2.6);
excluded (left) `+0x234 / +0x26c`;
"connection done" `+0x23d`; ready `+0x140`; "can talk to all" (NAT) `+0x244`; team `+0x12c` (PS3).

**Times.** "now" is the wall clock in seconds; "frames" is the 300 Hz game clock. Constants quoted below are the
PS3 values (interop facts, all read from the EBOOT data).

**Party mailbox codes** (PS3 NP Basic messages / room messages between members): `0x100` slot reservation request,
`0x101` reservation reply (48 bytes), `0x104` / `0x106` connection hello / ready, `0x105` "I left the results",
`0x107` start matchmaking, `0x10b` "join my room" request, `0x10c` its answer (u64 party room), `0x110` page mode,
`0x117` "the party leader asked you to leave the game" (u64 match room), `0x118` "party up: go back" (u64),
`0x119` "party up: join this party" (48 bytes), `0x11a` "party up pressed". PS4 consumers that are still intact read
them from the local inbox at `0x14d0e48` (`SESSION-INTERFACE.md` 1.5); session.cpp injects them with its mailbox
post helper (`session.md` 5.1) or carries them as session messages.

## 1. Rows with nothing to do (summary)

- **TaskManagerOnline state methods** (vtable pairing `PORT-GAMEPLAY3.md` 1.1): ResettingReady Enter `0x43bc80` /
  Update `0x440130`; WaitReady Enter `0x43bd28` / Update `0x4409c4`; ShowResults Enter `0x43ec08` / Update
  `0x4421b8`; PreResetting Enter `0x43edc8` / Update `0x4413a0`; LoadLevels Enter `0x4405c4` / Update `0x440478`;
  Resetting Enter `0x443e40` / Update `0x440d50` / Exit `0x440f48`; EstablishConnection Enter `0x4416e0` / Update
  `0x440f90`; Leave Update `0x441b88`; Playing Enter `0x444660` / Update `0x442558`; ShowRoundResults Enter
  `0x4430d8` / Update `0x4431a0`. The TSV says "absent", but every one has its PS4 body (`0x5f0fc0`, `0x5f0ec0`,
  `0x5f2d90`, `0x5f26d0`, `0x5f0ea0`+gameplay3.cpp, `0x5f0b30`, `0x5f13f0`, `0x5f1390`, `0x5f2e50`, `0x5f2dc0`,
  `0x5f1240`, `0x5f1000`, `0x5f1210`, `0x5f3050`, `0x5f2ea0`, `0x5eff30`, `0x5f23e0`, `0x5f14d0`, `0x5f0af0`,
  `0x5f0a90`); session.cpp wraps the ten Updates (`session.md` 2.2). Checked by hand for this group: the same NET_SM
  values (26, 27, 29, 30, 35, 3/16/17), the 900-tick StartGame lead (3 s), the 300-frame waits of PreResetting and
  Resetting, the 5 s round-results wait, the 120 s idle limit (`0xe622a8`), host-left -> GoLeave in LoadLevels and
  WaitReady (PS4 tests tracker `+0x26c`), the ShowResults 0x105 handshake and the quit-penalty counter. What PS4
  dropped from them is PS3 plumbing: LeaveRoom, HostRank (`0x440f90`), HostMigrate on/off (`0x4409c4`, Leave),
  SetRoomDataExternal, room hide, the "Windows Full" connection check (`0x442558`), the SQS stats thread
  (`0x43ec08`), stats/score service calls. Exceptions with a visible result are in section 2 (2.5, 2.9) and 2.1
  (results voice reset).
- **State destructors** `0x43baec..0x43bc0c` (vtable slot 0 of each state; base destructor only), the small object
  factory `0x43ba98` and the TaskManagerOnline task factory `0x43bd68`: no visible behaviour; PS4 has the objects.
- **Static initialisers** `0x4443d4` / `0x4443e4` (flow.cpp) and `0x4da438` / `0x4da474` (slot manager file: one word
  set to -1), empty `0x4da434`: no visible behaviour.
- **Match session listener slots** `0x43d660` (created), `0x43d610` (joined), `0x43d5d8` (kicked): PS4 `0x5e9d90`,
  `0x5e9db0`, `0x5e9f20` intact. Slot 3 `0x43d9e0` (member left: log, slot manager release when host, NetEventPlayerLeft
  for the member's net id): PS4 `0x5e9f00` is `ret`; session.cpp `player_left()` sends the same event; the slot release
  belongs to 2.6. Slot 6 `0x440334` (owner changed, "New host", event `0x15f1cf05`, NGM host = owner): PS4 `0x5e9f30`
  intact, called by session.cpp / online.cpp on migration.
- **State event handler** `0x43d76c` (ids `0x6ba8328a`, `0xbcc3b05b`, `0x15f1cf05`): PS4 `0x5efd30` handles the same
  three ids.
- **START_MATCHMAKING** `0x43e99c` (party message 0x107, menu reset, NET_SM 19, PAGE_MULTIPLAYER_MATCHMAKING
  `0xe35f0f1a`): PS4 `0x5eba40` + frontend3.cpp (0x107 as a session message).
- **PAGE_MULTIPLAYER_LEAVING_GAME** `0x43ea8c` (when the match room is left: ClearNetPlayers, pop, NET_SM 19 in
  matchmaking mode else 18): PS4 `0x5eb7d0` + session.cpp (sets match `+0xc8`).
- **LIST_PLAYERSMET** `0x43f5e8` (row -> selected NpId, popup "Add Friend" / "View Profile"): remaining.cpp B11/B12.
- **PAGE_MULTIPLAYER** `0x4465c8` (quit penalty box `0xbde6ab5b`, non-leaders bounced to PAGE_LOBBY, error ->
  PAGE_ROOT_MAIN + box `0x61a3662e`): PS4 `0x5eb1a0` keeps all of it (ids `0xd34ac18e`, `0x33d2252a`, `0xbe2ca34b`
  checked); the event-0 room hide / party leave and the event-1 mode message 0x110 are plumbing (`PORT-FRONTEND2.md`);
  the debug auto-start (`+0x2319`) is developer-only.
- **Mode dispatcher** `0x443248`: cinema.cpp (PS4 `0x5ecb80`).
- **Query helpers** `0x441dd8` ("in a running net match", 31 engine callers) and `0x441d78` (pause gate of the
  per-frame pause state `0x56c74`: offline always allowed; online only when the menu allows it, except in a running
  custom game): engine code, PS4 callers keep their own copies (see open question 6).
- **Bitstream helpers** `0x4da484`, `0x4da504`: they sit in the slot manager file but are readers used by the
  NetCharacter state code (`0x454ab4`, `0x4548a8`; one optional byte read as a float, 75 when absent). They are part of
  the snapshot path covered by `netanim.md`; nothing slot-manager specific.

## 2. MISSING items

### 2.1 In-match voice gating (NpNetUpdate `0x43bdd0`; ShowResults::Enter `0x43ec08`)

PS3 sets the playback volume of every remote player's voice each frame while the NetGameManager game state is
"playing" (2) and the local tracker exists. For each used remote tracker with a voice record:

- Distance d = straight-line distance between the local and the remote character positions; "enemy" = different team;
  "dead" = the character is dead / not spawned.
- **Elimination (game type 10):** a living local player hears living players only, and an enemy only when d < 12;
  a dead local player hears only dead players (the spectators talk among themselves).
- **Every other game type:** teammates are always heard at full volume. An enemy is heard only when d < 12 and both
  characters exist; otherwise silent.
- Volume is 1.0 or 0.0, nothing in between. When the "voice off" option is set (PS3 netInfo `+0x2b8`, PS4 `+0x2c8`),
  every remote voice is set to 0.0 instead.
- Outside the playing state nothing is changed by this rule. **ShowResults::Enter puts every remote voice back to
  1.0**, so everyone hears everyone on the results screen and in the lobby.

Ours: voice.cpp mixes every remote talker at the configured volume at all times (mute per player only). Parity:
apply the rule above per remote talker before mixing (a per-talker gain of 0 or 1, recomputed every frame from the
two trackers and the current game type), reset all gains to 1 when the match reaches ShowResults or leaves the
playing state. Explicit mutes still win. The local "talking" icon is unaffected (PS3 only changed playback).

### 2.2 Silent-peer timeout (NpNetUpdate `0x43bdd0`)

PS3 drops a remote player whose connection received nothing for **20 s** (no data, no keepalive). The check runs only
in matchmaking mode (netInfo `+0x34`) or with a developer flag; in custom games the connection layer's own dead-peer
detection decides. On the match host the silent member is kicked from the room (the others see them leave through the
normal member-left path); on a client the connection is only marked dead. One log line names the player
(`... %s has left : timed out`). After a remote player is marked dead and has no character, PS3 clears its tracker
while NET_SM < 22 (lobby side), so the roster row empties.

Ours: net.cpp drops any peer after 10 s of silence in every mode. Parity: 20 s. Whether custom games should keep a
timeout at all on PC (there is no other dead-peer detection) is open question 1; the minimum is the same 20 s.

### 2.3 Net clock synchronisation (NetConnectionUpdate `0x43cdb8`, NpNetUpdate `0x43bdd0`)

PS3 clients align their net time with the match host's before the match:

- While fewer than 8 samples are collected and the host is another machine, the client sends a clock request to the
  host at random intervals of 20..120 ms. The host answers at once with the request's own send time and its current
  net time.
- Each answer gives one sample: half the round trip, and the host's time corrected by that half round trip minus the
  local time. After 8 samples the client keeps the 4 with the smallest round trip, averages their corrections, and
  sets its synchronised net time from the local clock plus that average. It then resets a per-tracker timing field
  (PS3 tracker `+0x22c`) of all ten trackers. No further samples are taken until the counter is reset for the next
  connection setup.
- Separately, each connection sends a ping every 10 s; the answer stores the round trip in milliseconds for the
  connection (used by the lobby connection quality and the host ranking).

Ours: the host broadcasts its tick count once a second and a client overwrites its game clock only when it differs by
more than 150 ticks (0.5 s), with no latency correction. Effect: round start times, countdowns and timers can differ by
up to 0.5 s plus one-way latency between PCs; PS3 kept them within a few milliseconds. Parity: latency-corrected
sampling as above (best 4 of 8 by round trip) when a client connects to a match host, applied to the clock the game
uses for StartGame / round timers, and a periodic RTT measurement per peer.

### 2.4 Post-match "Party Up" (popup handler `0x43df00`; POSTGAME `0x43fb38` for context)

On the PS3 results / postgame screen the player can press Party Up. POSTGAME (PS4 `0x5ec4f0`, intact) shows a wait
popup whose page handler is `0x43df00` (PS4: page `0x471007a1`, handler `0x5ec4e0` = `ret`). The postgame page itself
closes after 30 s if Party Up was not pressed (leave the match room, pop).

What `0x43df00` does (page event 0 = opened, event 6 = every frame):

1. Opened: if this machine is the party leader, the party room is made visible again. Two deadlines start: "group"
   = now + 4 s, "give up" = now + 15 s.
2. Until 1 s before the group deadline it keeps noting whether it is the match host.
3. **Match host only, once, at the group deadline:** it takes the list of players who pressed Party Up (each entry:
   member, the party it came with, that party's size), logs them, and forms groups: players of the same party stay
   together; parties are merged into one group as long as the merged size stays below 6. The first member of each
   group becomes its leader. The host sends every other member of a group message 0x119 (48 bytes: the leader's party
   room to join) and every remaining player message 0x118 (8 bytes: the current room handle).
4. Every frame it logs the time left of the give-up deadline. At the give-up deadline: a party leader tells its party
   (0x118), then the player leaves the match room and returns to the multiplayer menu.
5. Message 0x118 with the current room handle: leave the match room and go back (same as 4).
6. Message 0x119: join the party room it names; on success a party leader forwards 0x118 to its own party, the player
   leaves the match room, and a party leader also sends "join my room" (0x10b) and shows the "Joining Party..." popup
   `0xe4e03363` (handler `0x43e680`, see 3.3).

Visible PS3 result: after a match, players who chose Party Up end up in new parties of at most 5 players (whole
parties never split), then continue to matchmaking together. Ours: Party Up shows the PS4 wait popup that never
completes (handler is `ret`); nothing groups the players. Parity: on PC the match host (session.cpp is the host of the
transport) collects the Party Up presses (session message standing in for 0x11a), applies the grouping rule after 4 s,
and tells each player either "follow leader X" (online: join that leader's party room; LAN: stay with or reconnect to
that host) or "go back"; clients leave after 15 s at the latest. Message box texts as PS3 (`0xf1d14e49` while waiting,
`0xeba80bd9` for the group popup).

### 2.5 Voluntary leave and the party (Leave::Enter `0x445608`; consumer in Playing::Update `0x442558`)

PS3 Leave::Enter has two paths. The normal end of a match (results done) is intact on PS4 (`0x5f00d0`: 0x105 to the
host, NET_SM 3 / 16 / 17). The **voluntary leave** path (the player chose Leave Game, PS3 leave byte = PS4
`0x104f1c0`) does, in order: leave the match room; then

- **a party member who is not the leader** also leaves the party, unless it is leaving because its leader told it to
  (state flag set by 0x117, see below);
- **the party leader in matchmaking** leaves the party (the party room's automatic owner grant hands the party to
  another member, who stays in the match);
- **the party leader in a custom game** sends 0x117 (the match room handle) to every party member;

then shows PAGE_MULTIPLAYER_LEAVING_GAME `0xb808d03c` and sets NET_SM 35.

A member receiving 0x117 for its current match room (PS4 compares with netInfo `+0x60`) in Playing::Update logs
"PartyLeader asked to leave the game", sets its own leave byte and the state's "asked by leader" flag (PS4 state
`+0x146`), and ends its match with reason 3 at the current net time; it then leaves with the leader and stays in the
leader's party.

PS4 kept the consumer (`0x5f1916..`) but its Leave::Enter sends no 0x117, and session.cpp never posts it. Ours: a
custom-game party leader who quits leaves its members alone in the match; a non-leader who quits stays in the party.
Parity: on a voluntary leave by the custom-game leader, deliver 0x117 (with the room handle the members hold) to every
member; a non-leader's voluntary leave ends its party membership (LAN: it leaves the leader's transport and becomes the
host of a party of its own; online: leave the party room); in matchmaking the leader's voluntary leave passes the
party leadership on (`PORT-ONLINE.md` promote path) instead of dragging the members out.

### 2.6 Slot reservation and party ids (slot manager `0x4d987c..0x4da3f8`, room handler `0x4da278`; used by AssignTeams)

PS3 match hosts keep a reservation table of up to 10 slots (capacity = the room's player limit, set when hosting a
custom game `0x465da0` or a matchmaking room). Each slot: the player's NpId, a party id, an expiry time.

- **Reserve a party** (request 0x100 from a joining party leader: the room handle and the NpIds of the whole party):
  the request is ignored unless it names the host's current room. Every member of the request is added with one new
  party id (a counter that increases after each successful group). Adding a player that already has a slot only
  refreshes its party id and expiry. Before taking a free slot, reservations whose expiry passed are dropped when that
  player is still not a room member ("Timeout User"). If any member of the group cannot get a slot, the members already
  added for this request are removed again and the request fails. The host always answers 0x101 (48 bytes; the PS4
  JOINING state `0x609790` reads byte 8: non-zero = reserved -> state 10, else join fails).
- **Expiry**: now + 10 s at reservation and at every refresh.
- **The host's own party** is reserved as one group when the host creates the room.
- **Member left** (listener slot 3, host only): the member's reservation is removed.
- **AssignTeams** (`0x4a2490`, at "Connections done") reads each player's party id from this table into the tracker's
  party id field (PS3 tracker `+0x11c`); players without a reservation get 0. It then counts party-mates (same id) and
  keeps parties on the same team when balancing.

PS4 has no table. Its AssignTeams (`0x6400d0`) balances the 10 tracker slots (NGM `+0x10 + k * 0x2c0`) with two
values per slot whose PS3 sources are gone:

- **Party group** (tracker `+0x124`, held at `+0x134` of the sort array; PS3 tracker `+0x11c`). In the path without
  team choices (lobby kind 0: every matchmaking match, and a custom game where nobody chose a team, `0x6407b6..0x640ed1`)
  PS4 reads each slot's group from the removed session member list (`0x113a048`, 10 entries of 0x34, word `+0x2c`;
  0 when there is no member) and stores it with `mov [rbx+r14+0x134], eax` at `0x640884` (bytes `42 89 84 33 34 01 00
  00`; r13 = the slot index 0..9; eax is set again at `0x64089d`). It then sorts the groups biggest first, then by the
  per-slot value `+0x12c`, puts each whole group on the team with the smaller sum (5 a side at most) and evens the
  counts. The list is never filled on PS4: every slot is in group 0 and the best half of the players lands on one
  team.
- **Level** (the per-slot value). PS3 read the slot's room member's level (member stats `+8`; with a ranked index the
  skill value at `+0x14 + 4 * index`). PS4 reads it at `0x6405a1..0x6405b5` (bytes `45 85 c9 78 09 c4 a1 fa 2c 7c 87
  14 eb 04 0f b6 7f 08 48 89 c3`: r9d the ranked index, rdi the stats block, rdx the slot's offset `0x27c + k * 0x2c0`,
  result in edi) from one match-session block for every slot (`0x640583..0x6405a0`, patched at `0x640598` so the block
  is never NULL), so all players have the same level and the teams come out in join order.

Tracker `+0x128` is not the party id: it is the rank, which the player info carries and MatchStats::Init copies at
match start (session.cpp `add_tracker` writes it so; `+0x130` holds it too).

online.cpp's join list checks only the room's free-slot count from the search result (no host-side reservation, so
two parties joining at once can overfill; no 10 s hold). Parity:

1. The match host (session.cpp on PC) keeps the reservation table with the rules above; a joining party leader asks
   for its whole party before joining (session message for 0x100/0x101), and the join is refused when the reply says
   no. A reservation not used within 10 s expires.
2. At team assignment each slot gets the party id of its player's reservation (0 when it has none, and on LAN, where
   there is no slot table), in every mode that takes the path without team choices (custom games too, as PS3), so the
   intact PS4 balancing keeps parties together.
3. Each slot's level is its player's own: the level shown for its rank (rank index + 1; an empty slot reads 1). PS3's
   ranked skill value lived on Sony's servers; PC has none, so the level stands for it in a ranked match.
4. Log lines as PS3 (adding / updating / timeout / removing / failed, with the player name and party id).

Ours: teams.cpp (byte-checked, each site left alone with a log line when its bytes differ): the store at `0x640884`
takes the slot's group from a table, the read at `0x6405a1` takes the slot's level from a table (`0x6405a1..0x6405b2`
jump to the stub, which ends with `test r9d, r9d` so the flags are the original's, and returns to `0x6405b3`, the
original `mov rbx, rax`). session.cpp `teams_frame` fills both tables every frame from the trackers' net ids, the
members' ranks and the reservations, and logs "team balance values: slot k: party p level l" when they change.

### 2.7 Player popups in the lobby rows (BUTTON_MULTIPLAYER_PLAYER0..9 `0x440620`, PARTY_PLAYER0..9 `0x4417f0`)

Match / postgame player rows (`0x440620`): the row indexes the match player list `0xf21de8`; empty rows do nothing.
The popup (no title) has, in order:

1. Unless the row is the local player: **"Mute" `0xffbbac12` or "Unmute" `0xfa9c4838`** (Unmute when that player's
   voice is currently muted; also "Mute" when the row has no voice record), action `0x7273924c`.
2. **"Mute All" `0xd381d5d8`** (action `0x8d703d6c`) unless every other player is already muted.
3. **"Unmute All" `0x3efad3ba`** (action `0x65be38af`) unless no other player is muted.

The session used for "all" is the party session in the party pages (page mode < 2) and the match session otherwise.
No Kick / Promote here. Party rows (`0x4417f0`): "Mute" or "Unmute" by the member's voice state, then for the party
leader "Promote To Party Leader" and "Kick From Party" (actions as group D 2.4). (The PS3 1.10 decompile of
`0x4417f0` is truncated after its member-list call; the option order is the one of the earlier PS3 build and matches
the texts the PS4 bank keeps; see open question 4.)

Ours (remaining.cpp B8): both row kinds show "Mute" always (never "Unmute"), no Mute All / Unmute All, and the match
rows also offer the host's Promote / Kick. Parity: the label follows the mute state; the match rows get Mute All /
Unmute All with the two visibility rules and no host options; Mute All / Unmute All toggle every remote player of the
named session (silently, as group D 2.4 says for Mute).

### 2.8 Friend popup: friend already in my party (LIST_FRIENDS `0x43f830`)

PS3 builds the friend popup (title `0xecc3219d`) as: when the friend is **not already a member of my party**: "Join
Party" `0xb1600ce3` (action `0x0ace8463`) if the friend is online, in this game's multiplayer (presence state 2) and its
presence is not marked full/closed (group D 2.6, `0x43a340`); "Invite To Party" `0x4a555973` (action `0x45a9c65b`) if
the friend is online. Always "View Profile" `0x4040b706` (action `0x3fe6f70e`). For a friend already in my party only
View Profile appears.

Ours (remaining.cpp B9): Invite is offered whenever the friend is online, also when the friend is already in my party
(Join is already hidden for my own room). Parity: hide both Join and Invite for a friend who is a member of my party
room (`online::friend_view().party_member`). The PC friend-request rows and Remove Friend stay (PC additions).

### 2.9 Players Met timing (Playing::Enter `0x444660`, helper `0x43bcbc`)

PS3 Playing::Enter, every time a match (or a new round) starts, adds every remote player of the match (their NpId
record) to the Players Met list and saves it. Ours: progression.cpp records players met only at Leave, from the final
match records. A player who left during the match, or a match the local player quit or that crashed, never reaches
Players Met. Parity: add the remote players at match start (each round start may re-add; already-known players only
move to the top), keep the end-of-match statistics update at Leave.

### 2.10 Lobby ticker (builder `0x446c60`, refresh in the per-frame net page update `0x447248`)

Group D 2.10 describes the text format (categories, friend entries, append rules). Two points from this group's
functions complete it:

- The text starts with the heading, then **the local player's own entry first** (own online id and own value of the
  category), then up to 5 random friends. With no friends the line still shows the local player.
- Refresh: from the per-frame net update, only while online after NetInit; the first refresh also asks for a friends
  list update. The category is chosen uniformly at random among the 7. The ticker is rebuilt only when its control is
  visible and has finished a full scroll, and at most every 10 s.

Ours: no ticker text (MENUS.md). Parity as group D 2.10 plus the two points above (LAN: own entry only).

### 2.11 Per-frame net page update `0x447248` (other visible parts)

Runs every frame while NET_SM != 0 and NetInit is done; returns NET_SM. Visible parts besides 2.10:

- While the multiplayer menu level ("menu-temple" package) is loaded and ready, it draws the game data version
  (netInfo first word) as `1.<byte 2>.<low 16 bits>` with the format `%i.%02i.%03i`, small (scale 0.4) near the top
  right of the 1280x720 menu canvas (x 1140, y 60). The PS4 image does not contain that format string.
- During NET_SM 29 (results) and while the loading screen byte is set, it holds three script values
  (`0x4df7190f`, `0x1623a914`, `0x01e6c27f`) at 1.0 (open question 5: their meaning).
- Developer overlays (recorded bytes, per-player connection lines) are debug only.

Parity: draw the version text on the multiplayer menu (our build / data version; owner decision, open question 5) and
hold the three values during results if they are found to matter.

## 3. PLATFORM items and what replaces them

### 3.1 Transport, handshake, per-frame update (`0x43bdd0`, `0x43cdb8`, `0x43d94c`, `0x43dad8`, `0x43dcd8`, `0x440c38`, `0x449e88`)

PS3: NdiNetUpdate (from the frame function, only when online) checks the PSN sign-in (sets the network error flag),
polls NP Basic events and the clan tag censor, advances the net clock by the frame time (300 ticks/s, with fractional
carry), runs NpNetUpdate, the room event queue, party error handling, voice capture/playback and NetConnectionUpdate,
reads the heartbeat socket, and handles party mailbox 0x106 / 0x104. NpNetUpdate drives the connection handshake: the
host takes one pending peer at a time (StartConnection event, `0x43d94c` sets "asked to connect" on the client, which
answers ConnectionDone; `0x43dcd8` logs "Connection established for %s in %3.2f secs canTalkToAll = ..." and marks the
tracker connected), resets the pending peer when it disappears. Listener slot 2 `0x43dad8` connects to every new room
member (not when both the member and this machine have strict NAT type 3 in a type-3 room, unless the member is the
owner) and opens its message channel, whose writer `0x440c38` stamps each outgoing message with the net time. Sends
run at 20 Hz per connection (0.05 s), with a forced keepalive every 2 s.

Ours: net.cpp (UDP or relay link, reliable/unreliable channels, keepalive), gamenet.cpp (connection records, attach /
detach = connected), session.cpp frame work (`session.md` 5.8) and peer events (5.11). The visible gaps are 2.1 – 2.3.

### 3.2 NetInit `0x447a5c`, CONNECTING `0x445020`, EULA `0x442100`, NP Basic `0x449ae8`, heartbeat reply `0x4496ac`, Save Game Stats `0x43bc30`

- NetInit (tickets, DRM, live-data download `net-patch9.bin`, map-DLC checks, NAT probe, sockets, Matching2,
  signaling, voice, score, booster list): netinit.cpp / netpatch.cpp / dlc.cpp / voice.cpp / online.cpp
  (`NETINIT.md`, `PORT-NETPATCH.md`).
- CONNECTING page: PS3 shows a progress page (`0x02661537`) after 1 s while NetInit is below 80 %, then loads the
  profile, NetGameManager::Init, the NAT type, the "net" package, and shows PAGE_MULTIPLAYER, or pops with message
  box `0x0ebd7bba` (NAT check failed) / `0x61a3662e` (network error). Since 2026-11-27 (owner: show it as PS3)
  session.cpp shows the page from the Multiplayer button: PS4 kept it (label text `ad9f50d9` "Connecting...", the
  bar = netInfo +0x2c0 as steps of 44, `0x5ae980`; nothing else writes that word). A page handler runs our start-up
  over the next frames (heap, netinit, leaderboard start-over and net-patch9, then the transport), moves the bar
  (22, then up to 44) and opens PAGE_MULTIPLAYER after at least 1 s; no network: the page goes and the message box
  `0x88a082af` shows over the main menu, as before. The PSN / online login itself is unchanged (online program).
- EULA status callback: no PC counterpart.
- NP Basic event handler: friend/presence/message events -> online.cpp.
- Heartbeat reply (player count, daily player count, location, next interval): no PC server; PAGE_MULTIPLAYER shows
  the party size in "[A] daily players" (MENUS.md 5). A count could come from the online service (open question 2).
- "Save Game Stats" thread (SQS upload): progression.cpp keeps the data locally.

### 3.3 Join Party / invites (`0x43ff60`, `0x43e680`, `0x4400bc`, `0x440948`)

- Join Party (`0x43ff60`, action `0x0ace8463`): PS3 sends "join my room" (0x10b) to the friend (nothing when already
  in that friend's party room) and shows "Joining Party..." `0xe4e03363`; its handler `0x43e680` waits up to 20 s for
  the answer 0x10c (the friend's party room), joins it, and shows PAGE_LOBBY once the host connection is up; join
  failure or timeout -> message box `0xc6467f2d`-style error (`0x4d972420`). Ours: `online::join_friend` joins the room
  from the friend's presence directly (remaining.cpp B19). Same end state.
- Invite wait page `0x4400bc`: waits until the party room exists, then opens the PSN invite dialog and closes. Ours:
  `online::invite_friend` sends at once, or shows "There is no party to invite to yet" (PC text).
- BUTTON_MULTIPLAYER_INVITES `0x440948`: PS3 opens the PSN received-invitations inbox (accepting joins that party) and
  shows PAGE_LOBBY when a page mode is set. Ours: the join list (remaining.cpp B18).

### 3.4 Clan tag (`0x441440`, `0x4443f8`, `0x441e30`)

PS3: BUTTON_CLANTAG opens the on-screen keyboard (title `0x639e0fa8`, current tag as initial text); the
PLAYER_OPTIONS page polls it, replaces `\` and `|` by spaces, stores the tag in NetPlayerData, and pops to the menu
when PSN sign-in is lost. The per-frame update then sends the tag to the PSN censor; a rejected tag becomes `*`; the
profile is saved and the room member data updated. Ours: clantag.cpp (text input, 4 characters A-Z / 0-9, upper-cased)
and its per-frame check (`clan_tag_check_frame`, `parity-a-leaderboards.md` 2.1), which runs the client flow with the
"no censor service" answer: accepted at once, then saved and published. The allowed character set is open question 3.

### 3.5 Host ranking and migration inside the states

HostRank at "Connections done" (members ordered by "can talk to all" first), HostMigrate on at round start unless the
game type is co-op (4, 8, 9, 12) and off at Leave: online.cpp / session.cpp host migration (`PORT-ONLINE.md`; the
co-op exclusion is in session.cpp).

## 4. Test scenarios

1. Voice (2.1): deathmatch, two teams, four PCs. A teammate across the map is heard; an enemy is heard only within
   12 m and only while both are alive; Elimination: a dead player hears only dead players. On the results screen
   everyone is heard again. Explicit mute stays muted in all cases.
2. Timeout (2.2): unplug one client in a matchmaking match: it is dropped after 20 s (not 10 s), one log line.
3. Clock (2.3): two PCs with 100 ms one-way delay: the round countdown and the start of play differ by less than 20 ms
   between them; the log shows 8 samples and the chosen offset.
4. Party Up (2.4): six players from three parties of 2 press Party Up: two groups (4 + 2, or as the < 6 rule gives),
   no party split; a player who did not press is sent back after 15 s at the latest.
5. Leave (2.5): custom game, party of three; the leader quits: both members show LEAVING_GAME, end reason 3, and are
   still in the leader's party in the lobby. A non-leader quits: it is in a party of its own afterwards. Matchmaking:
   the leader quits; the members stay in the match, one of them is the new party leader.
6. Reservation (2.6): host with 8 of 10 slots used; two parties of 2 ask at the same time: one is granted, the other
   refused. A granted party that never arrives frees its slots after 10 s. Team assignment keeps each party on one team
   ("team balance values" in the host's log: the party ids and levels per slot). One party of 2 plus 2 solo players,
   team deathmatch by matchmaking: the party is on one team. Custom game, 6 solo players of different levels, nobody
   picks a team: the two teams' level sums are close (not the best three on one side).
7. Popups (2.7, 2.8): mute player A, open A's row: "Unmute"; Mute All disappears once everyone is muted, Unmute All
   once nobody is; a friend already in the party shows only View Profile (and the PC rows).
8. Players Met (2.9): quit a match after 10 s: the other players are in Players Met.
9. Ticker (2.10): with two online friends in multiplayer, the line shows the category heading, the local player first,
   then the friends; it changes at most every 10 s and only after a full scroll.

## 5. Open questions

1. Custom-game timeout on PC (2.2): PS3 relied on signaling dead-peer detection there. Use 20 s for custom games too?
2. A "daily players" / "players online" count from the online service (3.2), or keep the party size?
3. Clan tag characters (3.4): the PS3 keyboard's allowed set and length limit were set by its dialog parameters, which
   this group's functions do not show. Keep A-Z / 0-9 or allow what the PS3 keyboard allowed (read `0x441440`'s dialog
   parameter block / `0x000d72f0` arguments)?
4. Party row popup (2.7): confirm the 1.10 option list of `0x4417f0` from its assembly (the decompiler drops everything
   after the member-list call `0x4d5e38`, which it treats as not returning; the same happens in `0x4416e0`,
   `0x4d6fa0`, `0x4d6d10`, `0x4c3e38`).
5. Version text and the three held script values of 2.11: does the owner want the PS3 version string on the menu, and
   what are `0x4df7190f`, `0x1623a914`, `0x01e6c27f` (not in the sid dictionary)?
6. Pause gate `0x441d78`: the PS4 copy of the per-frame pause state (`0x56c74`'s counterpart) was not traced; matches
   so far did not pause while online, so it is treated as covered.
7. AssignTeams party grouping (2.6): settled. The store site and the level read are known (2.6); the intact PS4 code
   does the balancing with the real party ids and levels.

## 6. Status table

| PS3 | Role (what it really is) | Status | Where (ours / PS4) |
|---|---|---|---|
| 43ba98 | small object factory (12 bytes, one field) | COVERED | no visible behaviour |
| 43baec | state destructor | COVERED | PS4 destructors |
| 43bb10 | Leave state destructor | COVERED | PS4 destructors |
| 43bb34 | ShowRoundResults state destructor | COVERED | PS4 destructors |
| 43bb58 | state destructor | COVERED | PS4 destructors |
| 43bb7c | state destructor | COVERED | PS4 destructors |
| 43bba0 | state destructor | COVERED | PS4 destructors |
| 43bbc4 | state destructor | COVERED | PS4 destructors |
| 43bbe8 | state destructor | COVERED | PS4 destructors |
| 43bc0c | ResettingReady / Active destructor | COVERED | PS4 destructors |
| 43bc30 | "Save Game Stats" thread body (SQS upload, thread exit) | PLATFORM | progression.cpp local stats (3.2) |
| 43bc80 | ResettingReady::Enter | COVERED | PS4 5f0fc0 |
| 43bcbc | add remote players to Players Met + save | COVERED | 2.9: session.cpp `note_players_met` at every Playing entry -> progression.cpp `progression_note_met` |
| 43bd28 | WaitReady::Enter | COVERED | PS4 5f2d90 |
| 43bd68 | TaskManagerOnline task factory | COVERED | PS4 TaskManagerOnline |
| 43bdd0 | NpNetUpdate: handshake, timeouts, voice gating, clock requests | COVERED | 2.1 voice.cpp `update_gating`; 2.2 net.cpp 20 s + session.cpp log; 2.3 session.cpp ClockSync; rest PLATFORM 3.1 |
| 43cdb8 | NetConnectionUpdate: recv/send, ping, clock sync | COVERED | 2.3 session.cpp ClockSync, net.cpp 10 s ping / pong; rest PLATFORM 3.1 |
| 43d5d8 | match listener slot 5: kicked | COVERED | PS4 5e9f20 |
| 43d610 | match listener slot 1: joined | COVERED | PS4 5e9db0 |
| 43d660 | match listener slot 0: created | COVERED | PS4 5e9d90 |
| 43d76c | state event handler (3 message ids) | COVERED | PS4 5efd30 |
| 43d94c | NetEventStartConnection apply: "asked to connect" | PLATFORM | gamenet attach (3.1) |
| 43d9e0 | match listener slot 3: member left (log, slot release, PlayerLeft) | COVERED | session.cpp player_left; slot release in 2.6 |
| 43dad8 | match listener slot 2: member added, connect, open channel | PLATFORM | net.cpp / session.cpp peer events (3.1) |
| 43dcd8 | NetEventConnectionDone apply: connection established | PLATFORM | gamenet attach marks connected (3.1) |
| 43df00 | Party Up wait popup handler (post-match regroup) | COVERED | 2.4 session.cpp `party_up_frame` / `party_up_group` (PS4 5ec4e0 stays ret) |
| 43e680 | "Joining Party..." popup handler (wait for 0x10c, join) | PLATFORM | online::join_friend (3.3) |
| 43e99c | NET_SM -> START_MATCHMAKING | COVERED | PS4 5eba40 + frontend3.cpp |
| 43ea8c | PAGE_MULTIPLAYER_LEAVING_GAME | COVERED | PS4 5eb7d0 + session.cpp |
| 43ec08 | ShowResults::Enter | COVERED | gameplay3.cpp; voice reset in 2.1; stats PLATFORM |
| 43edc8 | PreResetting::Enter (NET_SM 27) | COVERED | PS4 5f13f0 |
| 43f5e8 | LIST_PLAYERSMET row | COVERED | remaining.cpp B11 |
| 43f830 | LIST_FRIENDS row popup | COVERED | 2.8 remaining.cpp B9 (no Join / Invite for a party member) |
| 43ff60 | Join Party action (0x0ace8463) | PLATFORM | online::join_friend (3.3) |
| 4400bc | invite wait page (party room ready -> invite dialog) | PLATFORM | online::invite_friend (3.3) |
| 440130 | ResettingReady::Update | COVERED | PS4 5f0ec0 |
| 440334 | match listener slot 6: owner changed ("New host") | COVERED | PS4 5e9f30 via session.cpp / online.cpp |
| 440478 | LoadLevels::Update (host left -> leave) | COVERED | PS4 5f2dc0 |
| 4405c4 | LoadLevels::Enter | COVERED | PS4 5f2e50 |
| 440620 | BUTTON_MULTIPLAYER_PLAYER0..9 popup | COVERED | 2.7 remaining.cpp `match_player_row` |
| 440948 | BUTTON_MULTIPLAYER_INVITES (PSN invite inbox) | PLATFORM | remaining.cpp B18 join list (3.3) |
| 4409c4 | WaitReady::Update (start time, HostMigrate, host left) | COVERED | PS4 5f26d0; migration PLATFORM 3.5 |
| 440c38 | message channel writer (net-time stamp) | PLATFORM | net.cpp (3.1) |
| 440d50 | Resetting::Update | COVERED | PS4 5f1000 |
| 440f48 | Resetting::Exit | COVERED | PS4 5f1210 |
| 440f90 | EstablishConnection::Update ("Connections done", teams, HostRank) | COVERED | PS4 5f2ea0; HostRank PLATFORM 3.5; party ids 2.6 |
| 4413a0 | PreResetting::Update | COVERED | PS4 5f1390 |
| 441440 | BUTTON_CLANTAG (on-screen keyboard) | PLATFORM | clantag.cpp (3.4) |
| 4416e0 | EstablishConnection::Enter (net players per member) | COVERED | PS4 5f3050 + session.cpp fill_net_players |
| 4417f0 | BUTTON_MULTIPLAYER_PARTY_PLAYER0..9 popup | COVERED | 2.7 remaining.cpp `player_options` (Unmute label) |
| 441b88 | Leave::Update | COVERED | PS4 5eff30 |
| 441d78 | pause gate for the per-frame pause state | COVERED | engine (open question 6) |
| 441dd8 | "in a running net match" query | COVERED | engine callers |
| 441e30 | clan tag censor poll | PLATFORM | clantag.cpp `clan_tag_check_frame` from `online::frame` (3.4; no censor service) |
| 442100 | EULA status callback | PLATFORM | none (3.2) |
| 4421b8 | ShowResults::Update | COVERED | PS4 5f0b30 + gameplay3.cpp |
| 442558 | Playing::Update (idle kick, 0x117, leave, game over) | COVERED | PS4 5f14d0 + session.cpp; 0x117 sender in 2.5 |
| 4430d8 | ShowRoundResults::Enter (NET_SM 30) | COVERED | PS4 5f0af0 |
| 4431a0 | ShowRoundResults::Update (5 s) | COVERED | PS4 5f0a90 |
| 443248 | mode dispatcher | COVERED | cinema.cpp |
| 443e40 | Resetting::Enter | COVERED | PS4 5f1240 |
| 4443d4 | static initialiser | COVERED | none needed |
| 4443e4 | static destructor | COVERED | none needed |
| 4443f8 | PAGE_MULTIPLAYER_PLAYER_OPTIONS (keyboard poll) | PLATFORM | remaining.cpp B23 + clantag.cpp (3.4) |
| 444660 | Playing::Enter (NET_SM 26, StartNetGame) | COVERED | PS4 5f23e0; Players Met part = 2.9 |
| 445020 | PAGE_MULTIPLAYER_CONNECTING | PLATFORM | session.cpp skips it (3.2) |
| 445608 | Leave::Enter | COVERED | 2.5 session.cpp LeaderLeave (0x117), `host_own_party`; PS4 5f00d0 the normal path |
| 4465c8 | PAGE_MULTIPLAYER (Callback_Multiplayer) | COVERED | PS4 5eb1a0 + session.cpp |
| 446c60 | lobby ticker builder | DONE (CHECK) | 2.10: text built (online.cpp, own entry first); shown through PS4 `5d6500` (ticker.cpp, `parity-ticker-respawn.md` 1) |
| 447248 | per-frame net page update (ticker refresh, version text) | DONE (CHECK) | 2.11 version text done (session.cpp `draw_version_text`); 2.10 refresh: ticker.cpp `ticker_refresh` from the page handler |
| 447a5c | NetInit thread | PLATFORM | netinit.cpp, netpatch.cpp, online.cpp (3.2) |
| 4496ac | heartbeat reply parser | PLATFORM | none; party size shown (3.2) |
| 449ae8 | NP Basic event handler | PLATFORM | online.cpp (3.2) |
| 449e88 | NdiNetUpdate per frame | PLATFORM | session.cpp frame work (3.1) |
| 4d987c | slot manager: remove user | COVERED | 2.6 session.cpp `release_reservation` |
| 4d9a78 | slot manager: remove a list of users | COVERED | 2.6 session.cpp `reserve_party` (all or nothing before adding) |
| 4d9af0 | slot manager: party id of a user | COVERED | 2.6 session.cpp `teams_frame` -> teams.cpp group store `0x640884` |
| 4d9be8 | slot manager: add / update user (10 s expiry) | COVERED | 2.6 session.cpp `reserve_party` |
| 4da168 | slot manager: reserve a party as one group | COVERED | 2.6 session.cpp `reserve_party`, `slots_frame` (own party) |
| 4da278 | slot manager: answer reservation requests 0x100 -> 0x101 | COVERED | 2.6 session.cpp `on_reserve` (Reserve message), `admit_peer` |
| 4da3f8 | slot manager: init with capacity | COVERED | 2.6 session.cpp `slots_frame` |
| 4da434 | empty function | COVERED | none |
| 4da438 | static initialiser body | COVERED | none |
| 4da474 | static initialiser | COVERED | none |
| 4da484 | bitstream read helper of NetCharacter state code | COVERED | snapshot path (netanim.md) |
| 4da504 | optional-byte read helper (default 75) of NetCharacter state code | COVERED | snapshot path (netanim.md) |

Counts: COVERED 66, PLATFORM 18, MISSING 2 (before the implementation: 51 / 18 / 17).

## 7. Implementation (2026-11-25, branch `u2mp-ps3-parity-2`)

Owner decisions: open question 1 = **20 s silent-peer limit in every mode** (custom games too); question 5 = **add
the version text** as PS3 does; matchmaking's minimum players stay 2 in every mode, 1 in co-op (group F; custom
games start with 1 player in every mode, owner rule 2026-11-25); the lobby keeps the party
size where PS3 showed the daily player count (question 2). The three script values PS3 held during results are
skipped (meaning unknown).

| Item | Status | Where / what |
|---|---|---|
| 2.1 | done | voice.cpp `update_gating` (from `voice::frame`): while NGM state `+0x1e80` == 2 and a local tracker exists, every used, not excluded remote tracker (net id `+0x120`, team `+0x134`) gets heard / silenced: teammates always, enemies when both characters are alive (handle `+0x10/+0x18` live, vtable slot 74) and closer than 12 (guest `0x665f70` positions); type 10: living hear the living (enemies within 12), dead hear the dead; netInfo `+0x2c8` set: all silenced. NET_SM 29 (results) and NET_SM < 22 put everyone back. A silenced talker's packets are dropped after the talking-icon time stamp, its buffer is cleared; own mutes and the stranger mute still win. |
| 2.2 | done | net.h `kTimeoutSeconds` 20 s; `Transport::on_timeout` -> session.cpp log `*************** %s has left : timed out (nothing heard for 20 s)` (stars added with parity-i I6). |
| 2.3 | done | session.cpp ClockSync (type 12, see session.md "Parity group E changes"): 8 samples at 20..120 ms, best 4 by round trip, half-RTT correction, game clock moved once per connection setup; net.cpp ping / pong every 10 s per connection, `Transport::rtt_ms(slot)`. |
| 2.4 | done (PC flow) | session.cpp `party_up_frame` / `party_up_group` / `party_up_answer` (type 13); online.cpp `party_up_join`. The wait popup is recognised by its page (`0x471007a1` over POSTGAME); PS4 `0x5ec4e0` stays `ret`. |
| 2.5 | done | session.cpp: custom-game leader's voluntary leave sends LeaderLeave (type 15) before kLobby; members post 0x117 (PS4 consumer) and stay; a member's own voluntary leave ends with `host_own_party` (a party of its own; the matchmaking leader too: the party room hands the party on). |
| 2.6 | done | session.cpp slot manager (type 14, `slots_frame`, `reserve_party`, `admit_peer`, member left), online.cpp `take_led_party` / `reservation_refused`; AssignTeams' party groups and levels: teams.cpp (`0x640884`, `0x6405a1`) filled by session.cpp `teams_frame` (below). |
| 2.7 | done | remaining.cpp `player_options` (Unmute), `match_player_row` (Mute / Unmute, Mute All, Unmute All, no host options, no title), voice.cpp `set_muted`. |
| 2.8 | done | remaining.cpp `friend_options`. |
| 2.9 | done | session.cpp `note_players_met` (every Playing entry), progression.cpp `progression_note_met`, progression_store.cpp `note_met`. Compatible with group G: one call with a name list; filtering friends / batching can go into `note_players_met`. Group G1 (2026-11-25) changed it: friends and players already listed are skipped (no move to the top), batches of 16; see parity-g-friends.md. |
| 2.10 | not possible | online.cpp `stats_ticker_text` now puts the local player's entry first (LAN: own entry only). **Missing call:** the PS4 function (or control field) that sets the ticker control's text, and its "visible" / "finished a full scroll" state, are not named by this spec or group D; without them nothing shows the text and the 10 s / full-scroll refresh cannot be wired. |
| 2.11 | done | session.cpp `draw_version_text` (page handler) -> hud.cpp `hud_menu_text`: "1.09.078" from netInfo +0 (PS4 leaves it 0, so PS3's 0x0009004e stands in) at (1140, 60), scale 0.4, while `menu-temple` is ready. |

**2.6 party ids (2026-11-25):** the store site and the level read are now known (2.6 above): the party group is
stored at `0x640884` from the removed session member list, not written to tracker `+0x128` (which is the rank, as
`add_tracker` has it). teams.cpp feeds both from session.cpp's tables.

Remaining differences from PS3: the per-tracker timing field PS3 cleared after the clock sync (PS3 `+0x22c`, PS4
offset unknown); the clock samples use the frame-granular game clock on both sides (error up to a frame); the Party Up
popup texts are what the PS4 page shows (no "Joining Party..." popup `0xe4e03363`), a match host that did not press
Party Up still groups (PS3 grouped only in the host's own popup), and the party room is not "made visible again" on
opening; members a leader told to follow are on their way before the owner's slot reply (PC transport order), so after
a refusal those who already got a free slot stay and the others fall to the existing "owner not reached" path; the
ticker is not shown (2.10); a ranked match balances by level, not by the server skill PS3 had (2.6).
