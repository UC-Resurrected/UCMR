# Spec: PS3 parity, group G (net-friends: presence, friends, players met; NetGameManager helpers)

Clean-room behaviour description of 52 PS3 1.10 functions (rows of
`maintainer-tools/research/u2-parity/groupG.tsv`, PS3 `0x48f16c..0x4925e0`), measured against the UCMR U2 runtime
(`gow3_pc/src/u2mp`, branch `u2mp-ps3-parity-2`). It states what the PS3 game does as seen by the players and by
game memory, and whether our code gives the same result. It does not prescribe code.

Conventions as in `parity-f-party-mm.md`: addresses without "PS3" are PS4 1.02 image offsets; times "s" are real
seconds; the net clock counts 1/300 s. The friends list rows, the friend card and the presence fields they read are
already specified in `parity-b-menus.md` 3, 4, 5 and 11 (implemented in `menus.cpp query_friends` / card and
`online.cpp friend_view` / `presence_now`); the friends ticker in `parity-d-flow1.md` 2.10; Join Party / Invite To
Party in `parity-f-party-mm.md` F12 and `parity-d-flow1.md` 2.6. This file does not repeat them.

Status words: **COVERED** (same observable result already; file/function named), **PLATFORM** (PS3 NP Basic /
Matching2 / thread plumbing that the RPCN design replaces; the visible result must still match, deltas named),
**MISSING** (visible behaviour we lack or do differently; exact difference named).

Decompile notes: all functions carry TOC `0xe183b8`. The friends functions (`0x48f16c..0x491690`) reach their
globals and strings through the module data pointer read at r2-0x7890; the NetGameManager helpers
(`0x491698..0x4925e0`) through r2-0x788c; the flow.cpp caller `0x444660` through r2-0x78dc. Ghidra dropped the
loops of the two loader threads (`0x490040`, `0x491580`) and every jump table (NP Basic event handler `0x449ae8`,
queries cond 37 `0x4b8760` and cond 39 `0x4b8638`); those were read from the PS3 disassembly.

## 1. Facts every item uses

### 1.1 The PS3 NetFriends object (PS3 `0xf1db98`)

One object, built at NP start-up (PS3 `0x448d9c` -> `0x490178`) and torn down by the MP clean-up / abort path (PS3
`0x4386d0` -> `0x490878`). It holds:

- **Friends**: up to **100** entries. Each entry: a "presence must be fetched again" flag, the friend's user info
  (online id first) and the friend's last fetched presence (PSN presence details: title, status, an opaque data
  block with its size, and a state: 0 offline, 1 online in another title, 2 online in this title).
- **Players met**: up to **50** online ids (an in-memory copy of the system "players history").
- A "players met must be loaded again" flag (set at start-up and whenever new players were written to the
  history).
- A batch of at most **16** online ids waiting to be written to the players history.
- The last presence data block sent (88 bytes) and the net-clock time of the next presence check.
- A cache of one friend's party member list (up to 10 online ids + count), the id of the friend it belongs to, the
  net-clock time it was fetched, and the id of the request still in flight (0 = none).
- Three worker threads (friend list load, players met load, players history write); while the friend list load
  runs, every "find a friend" helper answers "not found" and the list queries show an empty list.

### 1.2 PS3 presence data block (88 bytes, what friends read)

Settled 2026-11-27 from the PS3 1.10 GOTY EBOOT (writer `0x490338`, readers `0x48f020`, `0x4909a8`, `0x43a340`,
friends list `0x4c1fe0`, card `0x4c1a08`, ticker `0x43b30c`). The writer fills a local 88-byte block every presence
check, compares it with the last block sent and, when it differs, sends it with `sceNpBasicSetPresenceDetails`
(options 3 = data and status). Every multi-byte field is written as a native PS3 word, so the block is
**big-endian**. Unused bytes are not cleared on PS3 (whatever the stack held); no reader looks at them.

| Offset | Size | Field (PS4 1.02 source on PC) | Readers |
|---|---|---|---|
| +0x00 | u32 | build key: the net version word, netInfo `+0` (PS3 `0x4a9cd4` only loads that word; see below) | all: block valid only when the size is 88 and this word equals the reader's own key |
| +0x04 | u8 | activity: 0 in multiplayer without a running net game (party, lobby, matchmaking, menus); 1 a net game is running (NGM match state `+0x1a8c` != 0, PS4 `+0x1e80`); 2 a film is played back (NGM film flag `+0x1ba4` set, PS4 `+0x209c`) | list, card: 0 "In lobby", 2 "Watching a cinema", 1 "Playing match" / the match line |
| +0x05 | u8 | party size: 1 without a party room, else the occupied party slots (PS3 `0x4d24a8`) | list, card: "[A] players in party" when 2+ |
| +0x06 | u8 | activity 1: occupied player slots of the NetGameManager (`0x492a20`); else 0 | none |
| +0x07 | u8 | the party room's "hidden" flag (session `+0x112c`, set from room flag 0x10000000 and by Hide `0x4d5338`) | `0x43a340`: Join Party is offered only when 0 |
| +0x08 | u8 | low byte of the multiplayer menu mode, netInfo `+0x2b4` (PS4 `+0x2c4`: 1 matchmaking, 2 custom, 3 cinema, 4 leaderboards) | list op 3: row icon, image type 0x21, value = this byte, at least 1 |
| +0x09 | 3 | unused | - |
| +0x0c | u32 | activity 1: team 0 score (u16 of the score table `+0x3dee`, widened); else 0 | none |
| +0x10 | u32 | activity 1: team 1 score (table `+0x3e22`); else 0 | none |
| +0x14 | s32 | activity 1 / 2: net game index (NGM `+0x18ec`, s16 widened, PS4 `+0x1ca0`); else -1 | card: net game record (map, type, map picture, type icon) |
| +0x18 | s32 | activity 1 / 2: co-op difficulty byte (NGM `+0x1c44` read by `0x4919fc`, PS4 `+0x21c4`); else -1 | card, co-op type 4: difficulty text (`0x492148`) |
| +0x1c | 4 | unused | - |
| +0x20 | u64 | activity 1 with a time limit (`0x49e0e8`: none for co-op types 4, 8, 9, 12): match end as Unix seconds (start `+0x19a0` plus the limit, converted by `0x4e862c`); else 0 | card: "Match ends in [TIME]" |
| +0x28 | u64 | party room id (session `+0x10`), 0 none | G4 party member list (`0x490c28`) |
| +0x30 | u32 | NetPlayerData data `+0x94` (PS4 NPD `+0xd8`): cash earned | ticker category 0 |
| +0x34 | u32 | data `+0x94` minus data `+0x98`, at least 0 (cash left) | none |
| +0x38 | u32 | data `+0x140` (PS4 `+0x184`): kills | ticker 1 |
| +0x3c | u32 | data `+0x144` (`+0x188`): deaths | ticker 4 |
| +0x40 | u32 | data `+0x148` (`+0x18c`): wins | ticker 2 |
| +0x44 | u32 | data `+0x14c` (`+0x190`): losses | ticker 3 |
| +0x48 | u32 | data `+0x150` (`+0x194`): headshots | ticker 5 |
| +0x4c | u32 | data `+0x154` (`+0x198`): seconds played | ticker 6 = this plus +0x50 |
| +0x50 | u32 | data `+0x15c` (`+0x1a0`): seconds played (second counter) | ticker 6 |
| +0x54 | u32 | data `+0x158` (`+0x19c`) | none |

The career values (+0x30..+0x57) are written in every activity; +0x06, +0x0c, +0x10 and +0x20 only in activity 1;
+0x14 and +0x18 in activity 1 and 2.

**Details structure and status text.** The block goes into the system presence details record (title 128 bytes,
status 64, comment 64, data 128, data size, state). The game copies its title text (`fd2de693`) into the **title**
field; the status and comment fields stay empty, and options 3 send the data and the (empty) status. So a PS3 player
shows an empty status line in another client's friend list; the game itself never reads status or comment. At the MP
clean-up (`0x48f16c`) the presence becomes the title with no data (friends then read "Playing Single-Player").

**Readers.** `0x48f020` returns the block of a friend entry only when the stored data size is 88 and its first word
equals the reader's own key (`0x4a9cd4`); the list, card, ticker and `0x43a340` also require the entry's presence
state 2 (online in this title). `0x4909a8` (friend in this game but without such a block) answers "yes" for state 2,
and for state 1 when the presence title equals the game's own title text: the "Playing Single-Player" case.

**Build key.** PS3 `0x4a9cd4` is a two-instruction accessor: it returns the first word of netInfo. That word is the
net version, filled at start-up from the DC symbol `*net-version*` (`6f1c3f87`) with 0x90000 added; live PS3 1.10
shows `0x0009004e`. It is the same value the game shows as its version text ("1.09.078") and puts into the room key
attribute 0x4C (`PORT-MATCHMAKING.md`). It is a version number, not a secret: it neither identifies a copy nor
unlocks anything, and it is already the PC's room key (`online.cpp kPcNetVersion`, used when the PS4 netInfo word is
0). So PC and PS3 friends of the same version accept each other's blocks.

Ours (2026-11-27, `presence_block.h`, `online.cpp presence_block_now` / `publish_presence`): every PC sends this
block with the PS3 field order and byte order, title "Uncharted 2: Among Thieves", empty status; no data outside
multiplayer. Headshots (+0x48) stay the local store's career count (`progression_headshots`), as before. The PC also
writes a join hint into the comment ("u2mp-join <room hex> <kind> <members> <max>"; PS3 neither sets nor reads it),
because a PC in a custom game lobby or a matchmaking room has left its party room, so the block alone would offer
nothing to join. Readers take the PS3 block from every friend and still read the old PC data ("U2MP" versions 1..3,
0.10.172 and older) with the PS3 meaning.

### 1.3 Who calls what (PS3)

| Moment | PS3 | What NetFriends does |
|---|---|---|
| NP start-up | `0x448d9c` | build: 100 friends, 50 players met, start the friend list load |
| NP Basic event "offline" (0) or "presence" (1) | `0x449ae8` | wait for the friend list load, then add the friend if new (flagged for a presence fetch) or flag the existing entry; "friend removed" and the other events do nothing here |
| Friends list op 8 (cond 36) and the first friends ticker pass | `0x4c23b0`, `0x446c60` | fetch the presence of every flagged entry, then sort |
| Friends list op 4/8, card | `0x4c2370..0x4c256c`, `0x4c1a48` | find a friend by online id |
| PAGE_FRIENDACTIONS list (cond 37) op 8 | `0x4b87f8` | the selected friend's party member list (G4) |
| Players met list (cond 39) op 8 | `0x4b8718` | reload players met if flagged (G1) |
| Match start (NET_SM -> 26 NET_SM_UPDATE, flow.cpp `0x444660`) | `0x444820` -> `0x43bcbc` | wait for the friend list load, then queue every other match player for the players history (G1) |
| Every frame on the MP pages and in a match | `0x43455c`, `0x441cc8`, `0x444b04` | presence check (G2) |
| MP clean-up / abort | `0x4387f4` | wait for the friend list load, set presence to "title only, no data", free everything |

## 2. What our port does today (summary)

- Friends come from RPCN through the online ABI (`online.cpp refresh_friends`, `U2O_EV_FRIEND`): the whole list
  is re-read on every friend change and sorted in the PS3 order; up to 250 friends plus request rows.
- Presence: `online.cpp publish_presence` every 2 s while the link is up, sent only when status text or data
  changed; status text is a description ("In a party (2/10)", "Multiplayer menu", ...).
- Players met: `progression.cpp` records every remote player of the scoreboard when a match reaches Leave
  (`note_match`, with stats); `menus.cpp query_players_met` and `remaining.cpp players_met_row` list the 50 most
  recently seen (op 8 reloads). Friends are listed too.
- `U2O_EV_MEMBERS_EXT` (external member list of a room) is only logged; nothing asks for it.

## 3. Items to implement

### G1. Players met: who is added, when, and that friends never appear (PS3 `0x491028`, `0x490a88`, `0x491580`, `0x48f79c`, flow `0x444660`) - MISSING, implemented 2026-11-25 (section 8)

PS3 behaviour:

1. **Adding (match start).** When NET_SM enters NET_SM_UPDATE (the match begins for this console, host or
   client), the game takes every occupied player slot of the NetGameManager (up to 10) except its own and hands
   their online ids to NetFriends, after the friend list has finished loading. NetFriends keeps an id only when it
   is not empty, is **not a friend**, and is **not already in the players met list**; at most 16 per batch. If a
   previous batch is still being written, the whole new batch is dropped. A non-empty batch sets the "load
   again" flag and is written to the system players history (one entry per id, with the game's title text as the
   description; debug lines "Adding player %s to players history" and, on error, "sceNpBasicAddPlayersHistory
   failed %x"). Before writing, each id is checked against the friends once more (a friend is skipped).
2. **Loading.** The players met list is (re)loaded only when the "load again" flag is set (start-up, or after a
   batch) and only when the list's refresh (op 8) is asked: the in-memory list is emptied, then up to 50 entries
   of the system history are read in the history's own order; an entry is kept only when it is not a friend and
   not already listed. So a player who later becomes a friend disappears from Players Met at the next reload.
3. **Showing** (cond 39, already COVERED in `parity-b-menus.md` 7): row text = the online id; while the friend
   list is loading the list is empty (last index -1).

Ours: names are recorded at Leave (end of match) from the scoreboard, friends included, stats added.

To do:
- (a) Record the names of the other match players **when the match starts** (NET_SM 26 reached; the roster at that
  moment: `session` member names, or the NetGameManager player slots), not only at Leave. A player who is already
  in the store is not added twice (PS3 skips known ids; whether the "last seen" time moves is our recency rule,
  open question 2). Late joiners of a running match are not
  added by this step (PS3), but our Leave-time stats update may still add them: keep the Leave step for the stats
  (PC extra used by View Profile, `remaining.cpp met_profile`), and let it only update players that are already
  in the store (so the list's membership follows the PS3 rule).
- (b) **Never list a friend**: online, skip every name that is on the RPCN friend list (`online::friend_index`
  >= 0 for a row of kind friend), both when recording (a) and when building the rows (`menus.cpp
  query_players_met`, `remaining.cpp players_met_row`: same filter, same order, so the row index stays the same in
  the query and the row callback). LAN (no friend list): no filter.
- (c) Before the friend list has arrived (link not ready or first list not received), the online list answers
  "empty" (op 5 = -1), as PS3 does while its friend load runs. LAN: unchanged.
- Cap and order stay ours (50 shown, most recently seen first; PS3: the system history order, see open question 2).

Visible result: after a match with strangers and a friend, Players Met lists the strangers (from the start of that
match) and never the friend; a stranger who is added as a friend leaves the list at the next refresh.

### G2. Presence timing and failure retry (PS3 `0x490338`, `0x48f16c`) - MISSING (timing), implemented 2026-11-25 (section 8)

PS3:

1. A presence check runs at most once every **5 s** (1500 net-clock units; the next time is set before the
   check). It builds the data block (1.2) and sends it only when it differs from the last one sent.
2. When sending fails (debug "sceNpBasicSetPresenceDetails() failed. ret = 0x%x"), the remembered block is cleared
   (so the next check sends again even without a change) and the next check waits **10 s**.
3. At MP clean-up the presence is set to the title text with no data block (friends then read "in this game, not in
   multiplayer" = "Playing Single-Player", `parity-b-menus.md` 3.3).

Ours: period 2 s; sent on change; a failed set is never retried until something changes (the request result is not
checked; `sent_presence` is cleared only on link loss); leaving MP publishes state "menu" (same reader result as
PS3 step 3: COVERED).

To do (`online.cpp publish_presence`, `kPresencePeriod`): period 5 s; on a failed presence request (its `U2O_EV_DONE`
carries an error) clear the remembered presence and wait 10 s before the next try. The `match_end_now` 2 s hold
stays (it only avoids resends).

### G3. Presence status text and data (PS3 `0x490338`, `0x48f16c`) - COVERED (2026-11-27)

Corrected 2026-11-27 (1.2): PS3 puts the title text into the presence **title** field and sends an **empty**
status line (options 3); the data is the 88-byte block. Before 2026-11-27 the PC sent its own 80-byte "U2MP" data
with a descriptive status, so a PS3 friend rejected it (size and key) and the PC rejected PS3's block (no "U2MP"):
each saw the other as "Playing Single-Player". Ours now: title "Uncharted 2: Among Thieves", empty status, the PS3
block (`presence_block.h`), the PC join hint as comment; the description is only in our log line (section 9).

### G4. A friend's party members (PS3 `0x490c28`, `0x48f350` / `0x48f558`, cond 37 `0x4b8760`) - PLATFORM, implemented 2026-11-25 in View Profile (section 8)

PS3: PAGE_FRIENDACTIONS (opened by selecting a friend) shows under the card a list of the friend's party members.

1. On the list's refresh (op 8) the page asks NetFriends for the member list of the friend the friends list
   stored (by online id). NetFriends answers:
   - nothing (the page shows an empty list) when the friend is not found or the friend list is loading;
   - when the friend has a valid presence block of this build (1.2), and either the cached list belongs to another
     friend or is older than **5 s**, and no request is in flight: it asks the room service for the external
     member list of the friend's party room (the room id from the presence block). The answer (up to 10 online ids)
     replaces the cache, tagged with that friend and the time of the request.
   - the cached list when it belongs to this friend (even if a refresh was just started); nothing when it belongs
     to another friend.
   The page keeps its own copy of the last answer (count 0 when nothing came).
2. Rows (column 0) = the members' online ids; op 5 = count - 1; one global cursor (op 4 / 9); title = text
   `b6bd34c3` ("Party") while the list is not empty, else blank.

PC: nothing opens PAGE_FRIENDACTIONS (`parity-b-menus.md` 4, open question 5 there). The room service call exists
(`online_link members_external`, event `U2O_EV_MEMBERS_EXT`, currently only logged). Parity (when the owner picks
the card or extends View Profile): on opening a friend's profile / card, request the external member list of the
friend's presence room with the same 5 s per-friend cache and one request at a time, and show the member names
under the profile text (or as the cond 37 rows with the "Party" title). Without a valid presence room: no list.

## 4. NetGameManager helpers (rows `0x491698..0x4925e0`) - COVERED

These sit after NetFriends in the PS3 image but belong to NetGameManager (PS3 `0xf1dc80`, PS4 NGM `0x113b760`).
PS4 kept the code that uses them; the helpers are inlined or too small for the matching passes (hence "absent" in
the TSV). Data checks in the PS4 image: the 45-name id table of `0x491698` / `0x4916f0` is present, and the sound /
event ids used by `0x491ca0`, `0x491d08`, `0x492104` occur in PS4 code. No port work. Roles (for the table):

- `0x491698` / `0x4916f0`: map a name id to a small number 1..45 (0 = unknown) and back (compact ids in net
  messages).
- `0x491720` / `0x491768`: an object's "held for player P until time T" check (free, or held by the asker, or the
  hold expired) and its reset.
- `0x49177c` / `0x49181c`: a 3-slot per-key amount tally (add to the key's slot or a free one; full = ignored) and
  its read (0 for an unknown key).
- `0x491878`, `0x4918bc`: sort orders (by a count then a float then address; by the float then address); used as
  sort callbacks, no direct callers.
- `0x4918e8`, `0x4919b0`: net player record by player id / by slot (10 slots); `0x4919d4`, `0x4919dc`,
  `0x4919fc`, `0x491b24`: field reads (local record, a second word, a state byte, a start time); `0x491b34`:
  clock now minus a stored mark; `0x4919e4`: set a flag and count one down.
- `0x491a04` / `0x491a60`: position / entry of an id in NetGameManager's object list.
- `0x491ab0`: "is this a co-op game type" (types 4, 8, 9, 12; also used by our card text, `menus.cpp`).
- `0x491af4`: clear one per-player value in all 10 records.
- `0x491b60`: find a live object (validated handle) with a given owner id; `0x491be8`: remove an id from an
  8-entry list (answers whether it was there).
- `0x491ca0`, `0x491d08`: play a match sound cue once per match (two cues, each latched).
- `0x492104`: send one event to every live object of two kinds.
- `0x492298`: shuffle an array of words at random.
- `0x492340`: keep the last five values of a per-frame number and print them when a developer tunable is on (debug
  only).
- `0x4925e0`: compare the two team scores (-1 / 0 / +1), treating a team without active players specially.

## 5. Test scenarios

1. G1: online, A plays a match with B (stranger) and C (friend). On the Players Met page after the match: B listed,
   C not. A adds B as friend; refresh the page: B gone. A match that A quits right after it started: its players
   are still listed. LAN: everyone listed as before.
2. G1 (c): open Players Met right after going online, before the friend list arrived: empty list, then filled.
3. G2: watch the presence log: at most one presence set per 5 s while the state changes quickly (party join, then
   lobby); make a presence set fail (online program refuses): a retry 10 s later with the same data.
4. G3: a friend's RPCN client shows the game title as status in every state.
5. G4 (if built): friend F in a party of 3; open F's profile: three names; reopen within 5 s: no new request in the
   log; after 5 s: one new request.

## 6. Open questions

1. G4 / parity-b open question 5: does the owner want PAGE_FRIENDACTIONS (card + party member list) or the member
   names in our View Profile popup?
2. G1: the PS3 players history is a system list shared by all titles, read in the system's order (believed newest
   first) and limited to 50; ours is per game, most recently seen first. Keep ours (no system history on PC)?
3. G3: settled 2026-11-27: PS3 sends the title text in the title field and an empty status; ours the same.
4. PS3 lists at most 100 friends (its PSN limit); ours up to 250 plus request rows. Keep 250 (RPCN lists can be
   longer) or cap at 100?
5. PS3 keeps a removed friend in the list until the next MP entry (the "friend removed" event is ignored); ours
   drops it at once. Treated as PLATFORM (better result, no PS3 rule depends on it).

## 7. Status table

| PS3 | Role (what it does) | Status | Where (our code / PS4) / item |
|---|---|---|---|
| 0x48f16c | set presence to "title only, no data" (MP clean-up) | PLATFORM | online.cpp `presence_now` state menu when not started (same reader result); G2.3, G3 |
| 0x48f268 | is this friend a member of our party room | COVERED | online.cpp `friend_view` party_member; menus.cpp status dot |
| 0x48f2b8 | fetch one friend's presence again, clear its flag | PLATFORM | RPCN pushes presence: `U2O_EV_FRIEND` -> `refresh_friends` |
| 0x48f350 | room member list answer: store up to 10 ids, clear the request | PLATFORM | online.cpp `party_list_answer` (`U2O_EV_MEMBERS_EXT`, or its `U2O_EV_DONE` error: empty list); G4 done |
| 0x48f558 | callback thunk of 0x48f350 | PLATFORM | online.cpp `on_event` request table (`Req::PartyMembers`); G4 |
| 0x48f5a8 | find a friend entry by online id (none while loading) | COVERED | online.cpp `friend_index` / `friend_view` |
| 0x48f654 | add a friend entry or flag it for a presence fetch | PLATFORM | online.cpp `refresh_friends` (whole list) |
| 0x48f79c | start the players met reload when flagged (empties the list) | PLATFORM | menus.cpp `query_players_met` op 8 (or empty list) reload -> `progression_players_met`; G1 |
| 0x48f858 | generic in-place sort of friend entries | COVERED | online.cpp `refresh_friends` std::stable_sort (same total order) |
| 0x48ff00 | fetch flagged presences, then sort (list op 8, ticker) | COVERED | online.cpp `refresh_friend_list` / `refresh_friends` |
| 0x48ffe0 | wait until the friend list load is done | PLATFORM | no thread on PC: online.cpp `friend_list_ready` (list read since the link came up); Players Met empty until then (G1 c) |
| 0x490040 | friend list load thread (up to 100 friends with presence) | PLATFORM | `U2O_FRIEND_LIST` -> `refresh_friends` (250); question 4 |
| 0x490170 | thread entry thunk of 0x490040 | PLATFORM | - |
| 0x490178 | build NetFriends (100 / 50 / 16 / 10), start the friend load | PLATFORM | online.cpp link start |
| 0x490338 | presence check every 5 s, send on change, 10 s retry on failure | COVERED | online.cpp `publish_presence` (5 s, next time set before the check), `presence_failed` (sent data forgotten, next check 10 s later; on a 0 request id or a `U2O_EV_DONE` error of the last presence request); G2 (content: the PS3 block, `presence_block.h` `encode`, 1.2; title text, empty status, G3) |
| 0x490878 | tear down: wait, clear presence, free lists | PLATFORM | online.cpp `on_transport_closed` / link close |
| 0x4909a8 | friend runs this game but has no MP presence of this build | COVERED | online.cpp `friend_view` this_game / multiplayer; menus.cpp `friend_status` |
| 0x490a88 | players history write thread (skips friends; title text as description) | PLATFORM | progression.cpp `progression_note_met` (synchronous store write, friends skipped, log "adding player %s to Players Met"); G1 |
| 0x490c20 | thread entry thunk of 0x490a88 | PLATFORM | - |
| 0x490c28 | a friend's party member list, cached 5 s per friend | PLATFORM | online.cpp `friend_party` (5 s per-friend cache, one request at a time, room = presence `party_room`); shown in View Profile (remaining.cpp `friend_profile`), not on PAGE_FRIENDACTIONS; G4 |
| 0x490f60 | index of a friend by online id (-1) | COVERED | menus.cpp selection by friend id (`friends_sel`) |
| 0x491028 | add players met: skip friends / known ids, batch of 16, flag reload | COVERED | session.cpp `note_players_met` (NET_SM 26 / every Playing entry) -> progression.cpp `progression_note_met` -> progression_store.cpp `note_met`; G1 |
| 0x491580 | players met load thread: 50 history entries, skip friends / duplicates | COVERED | progression_store.cpp `players_met` (50 rows, no friend, our order: most recently seen first, per game); G1 (b) |
| 0x491690 | thread entry thunk of 0x491580 | PLATFORM | - |
| 0x491698 | name id -> number 1..45 | COVERED | PS4 NGM (table present) |
| 0x4916f0 | number -> name id | COVERED | PS4 NGM |
| 0x491720 | object hold check (free / own / expired) | COVERED | PS4 NGM |
| 0x491768 | clear an object hold | COVERED | PS4 NGM |
| 0x49177c | 3-slot per-key tally add | COVERED | PS4 NGM |
| 0x49181c | 3-slot per-key tally read | COVERED | PS4 NGM |
| 0x491878 | sort order: count, float, address | COVERED | PS4 NGM |
| 0x4918bc | sort order: float, address | COVERED | PS4 NGM |
| 0x4918e8 | net player record by player id | COVERED | PS4 NGM |
| 0x4919b0 | net player record by slot (< 10) | COVERED | PS4 NGM |
| 0x4919d4 | field read (local record) | COVERED | PS4 NGM |
| 0x4919dc | field read (second word) | COVERED | PS4 NGM |
| 0x4919e4 | set a flag, count one down | COVERED | PS4 NGM |
| 0x4919fc | field read (state byte; also in presence) | COVERED | PS4 NGM; presence 1.2 |
| 0x491a04 | position of an id in the object list | COVERED | PS4 NGM |
| 0x491a60 | entry of an id in the object list | COVERED | PS4 NGM |
| 0x491ab0 | co-op game type check (4, 8, 9, 12) | COVERED | PS4 NGM; menus.cpp card |
| 0x491af4 | clear one per-player value in all records | COVERED | PS4 NGM |
| 0x491b24 | field read (start time; presence end time) | COVERED | PS4 NGM; online.cpp `match_end_now` |
| 0x491b34 | clock now minus a stored mark | COVERED | PS4 NGM |
| 0x491b60 | live object with a given owner id | COVERED | PS4 NGM |
| 0x491be8 | remove an id from an 8-entry list | COVERED | PS4 NGM |
| 0x491ca0 | match sound cue A, once per match | COVERED | PS4 NGM (cue id present) |
| 0x491d08 | match sound cue B, once per match | COVERED | PS4 NGM (cue id present) |
| 0x492104 | send one event to all live objects of two kinds | COVERED | PS4 NGM (ids present) |
| 0x492298 | random shuffle of words | COVERED | PS4 NGM |
| 0x492340 | last-five-values history, debug print | COVERED | debug only |
| 0x4925e0 | team score comparison (-1 / 0 / +1) | COVERED | PS4 NGM |

Counts: COVERED 37, PLATFORM 15, MISSING 0 (after the 2026-11-25 implementation, section 8).

## 8. Implementation (2026-11-25, branch `u2mp-ps3-parity-3`)

Owner decisions: G1 PS3 rules with **our order** (most recently seen first, per game; open question 2); G2 PS3
timing; G3 **kept** (our descriptive status text, question 3); G4 in **our View Profile popup** (question 1);
**250** friends kept (question 4); removed friends dropped at once (question 5).

| Item | Status | Where / what |
|---|---|---|
| G1 (a) | done | session.cpp `note_players_met` (every Playing entry, NET_SM 26) -> progression.cpp `progression_note_met` -> progression_store.cpp `note_met`: names not empty, not the local player, not a friend (`online::is_friend` while online), not already a Players Met row; at most 16 (`kMetBatch`) in order; added with no statistics (or a stored one listed again), `last_seen` = now; log `progression: adding player %s to Players Met`. An already listed player is skipped (its time does not move at the start; the Leave statistics move it as before). |
| G1 (a) Leave | done | `note_match` keeps the statistics of every match player (leaderboards, Players Met View Profile) but a name new to the store comes in `unlisted` (file record word +68 bit 0; older files hold 0 = listed): late joiners and friends at the start are no Players Met rows. A different route than the text above ("Leave only updates stored players"), chosen so the local leaderboards keep friends and late joiners. |
| G1 batch drop | n/a | PS3 drops a batch while the previous one is still being written (thread). Our store write is synchronous inside `progression_note_met`, so a batch can never meet a running write; no code. |
| G1 (b) | done | progression_store.cpp `players_met(store, is_friend)`: listed, no friend, newest first, at most 50 (`kMetShown`). Used by menus.cpp `query_players_met` (op 8 / empty reload) and remaining.cpp `players_met_row` (same call, same order). A stranger who becomes a friend is gone at the next reload. LAN: no friend filter. |
| G1 (c) | done | progression.cpp `progression_players_met`: empty while online is active and `online::friend_list_ready()` is false (link up and the list read since then; reset when the link leaves ready or is lost). The list reloads while empty, so it fills once the list is known. |
| G2 | done | online.cpp `publish_presence`: `kPresencePeriod` 5 s, the next check time set before the check, sent only on change. `presence_failed`: a 0 request id, or a `U2O_EV_DONE` error of the last presence request, clears the remembered presence and sets the next check 10 s (`kPresenceRetry`) later, logged `online: presence: not sent: trying again in 10 s`. The `match_end_now` 2 s hold stays. |
| G3 | done 2026-11-27 | PS3 block, title text, empty status (section 9). |
| G4 | done | Presence data: the former padding word (+0x18) is now `party_room` (our party room id, 0 none; older builds sent 0 there, version stays 3). online.cpp `friend_party(i, names)`: nothing while the list is not ready or row i is no friend; asks `members_external(party_room)` (new `Req::PartyMembers`) when the friend's presence is this build with a party room, the cache is another friend's or 5 s (`kPartyListLife`) old, and no such request runs; the cache (friend, request time, up to 10 online ids) is returned when it is this friend's. `party_list_answer` (`U2O_EV_MEMBERS_EXT`, or a `U2O_EV_DONE` error = empty list) fills the cache and calls remaining.cpp `friend_party_arrived`, which renews an open View Profile box of that friend. remaining.cpp `friend_profile`: card lines, then `\` + text `b6bd34c3` ("Party") + one id per line when the list is not empty. Player text buffer 1023 characters. Link lost: cache and request cleared. |

Remaining differences from PS3:
- G1 order and cap: ours per game, newest first, 50 of a 64-entry store; PS3 the system history order.
- G1: a player already listed is not moved at a match start (PS3 the same), but our Leave statistics move it.
- G1: the batch drop of PS3 cannot happen (synchronous write); PS3 waits for its friend list load at the match
  start, ours filters with the list as it is (online the link is up in a match, so it is loaded).
- G2: the 10 s wait starts when the failure is known (the request's answer), not at the send.
- G4: shown in View Profile, not as PAGE_FRIENDACTIONS rows; the first open of a friend shows no list until the
  answer arrives (the open box is renewed if the game re-reads the player text; else the next open shows it). A
  friend on an older build has no `party_room` and so no list. Whether the room service lists the members of a
  hidden party room to a non-member is up to the online program (not checked here).

## 9. PS3 presence block (2026-11-27, branch `u2-presence-ps3`)

Problem: a PC and a PS3 (RPCS3, RPCN) friend saw each other as "Playing Single-Player" in Party Up: the PC sent its
own data, which PS3 rejects (not 88 bytes, no key at +0), and the PC accepted only that data.

| Piece | Where / what |
|---|---|
| Block | `presence_block.h`: `Block`, `encode` / `decode` (1.2 layout, big-endian, exactly 88 bytes). Test `tests/test_u2mp_presence.cpp` (`ninja -C out/gpu u2mp-presence-test`). |
| Writer | online.cpp `presence_block_now`: key `room_key()`; activity from NGM `+0x1e80` / `+0x209c`; party size and the party room's hidden flag; menu mode netInfo `+0x2c4`; in a match the session's player count, the two team scores (`[0x104ae70] + 0x3dee + 0x34 t`), net game `+0x1ca0`, difficulty `+0x21c4`, `match_end_now`; career values from NetPlayerData (cash left = `+0xd8` - `+0xdc`, times `+0x198` and `+0x1a0`, headshots from the local store). `publish_presence`: title text, empty status, the join hint as comment; outside multiplayer no data. Log `online: presence: <what>: activity A, party P, room R, net game N`. |
| Readers | online.cpp `friend_presence` / `our_presence`: the PS3 block from everyone (PS3 and PC); the old "U2MP" data of 0.10.172 and older with the PS3 meaning (`decode_legacy`; activity from its state, mode 1 / 2 / 3 from matchmaking / custom / cinema). `friend_view` fills multiplayer, activity, mode (row icon 0x21, was the rank before), party, difficulty, net game, match end. Ticker: every friend with a valid block; time played = +0x4c plus +0x50 (PS3 `0x43b30c`). G4 party list: block +0x28. |
| Join Party | `join_room_of`: a PC friend's join hint (or old data) names its room (free slot checked, as before); a PS3 friend's party room from +0x28 unless +0x07 (hidden) is set (PS3 `0x43a340`). PS3 itself asks the friend with NP message 0x10b and gets the party room in 0x10c; joining the room id from the presence reaches the same room (see crossplay-ps3.md for the message exchange). |
| Invites | PS3 0x43a164 sends main type INVITE with 16 bytes {u32 key, 4 unused bytes, u64 party room (0 without one)}, big-endian. Ours: to a friend known as a PC the old 32-byte head (`encode_old_room`, every older build reads it); to any other friend the PS3 form (`encode_ps3_invite`, our party room). Both forms are read (`decode_ps3_invite`: a party room, member counts unknown). |

What the PS3 block lacks compared with the old PC data: the rank byte (PS3 shows the menu mode icon there instead:
PS3 behaviour now), and the joinable room, its kind and member counts (kept for PC friends in the join hint). Old
PCs (0.10.172 and older) cannot read the new block: they show an updated PC friend as "Playing Single-Player" until
they update; updated PCs still read old PCs.

Not verified (no game run here): the RPCS3 side end to end; whether RPCN passes the comment through (if not, a PC in a
custom lobby or matchmaking room is simply not joinable from its presence; its party room still is when it has one).
