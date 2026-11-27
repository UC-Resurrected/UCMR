# Spec: Uncharted 2 crossplay, PS3 1.10 (RPCS3, RPCN) <-> PC runtime

Gap index, 2026-11-27 (branch `u2-presence-ps3`). Goal: a PS3 player (U2 GOTY NPUA80698 1.10 in RPCS3 on RPCN,
communication id NPWR00462) and a PC player of our runtime find each other, form a party, start a custom game or
a matchmaking match together, join one in progress, spawn, see each other, shoot, score and finish.

Sources: the PS3 1.10 EBOOT (read for this file where marked "EBOOT"), the existing U2 specs (`PORT-MATCHMAKING.md`,
`PORT-ONLINE.md`, `TRANSPORT-INTERFACE.md`, `SESSION-INTERFACE.md`, `spec/session.md`, `spec/events.md`,
`spec/netanim.md`, `parity-*.md`) and our code (`gow3_pc/src/u2mp`). Behaviour descriptions only; no game code.
Nothing here was run against a live PS3.

## 0. Result in one paragraph

Presence and invites now use the PS3 formats (done, section 1). Everything after "find each other" does not work
yet, and not because of a few values: the PC game link is a PC design. A PS3 puts its bit stream straight into each
UDP datagram (big-endian words, its own kinds for data, clock and ping), sends session messages as game events
(NetEventMessage type 46, codes 0x100..0x11a) and talks to every room member directly; the PC wraps everything in
its own transport (`net.h`: "U2MP" magic, hello / welcome, reliable channels, little-endian), uses its own session
messages (`session.cpp` kInfo, kRoster, kMode, kFollow, kClock, ...) and a star around one PC host. Each side drops
the other's datagrams at the first check. Making a PS3 and a PC play together needs a "PS3 peer" path in the game
link (section 3, items L1..L6), the PS3 join handshake, and a per-event layout check like the one done for
Uncharted 3. That is weeks of work and needs RPCS3 captures for the facts the EBOOT reading here did not settle.
The room-level fixes (section 2) are listed but **not applied**: alone they make PS3 and PC rooms visible to each
other without the link to play in them, and some of them change how PCs find each other.

## 1. Presence and invites (done)

| Step | PS3 | PC before | PC now |
|---|---|---|---|
| Presence | 88-byte big-endian block, title text in the title field, empty status (`parity-g-friends.md` 1.2) | own 80-byte "U2MP" data, descriptive status; PS3 block rejected | the PS3 block, read from everyone (commit "send and read the PS3 presence block") |
| Friends list / card / ticker on PS3 | reads the block | rejected ours | reads ours |
| Invite (EBOOT `0x43a164`) | message main type INVITE (3), 16 bytes {u32 key, 4 unused, u64 party room}, big-endian | 32/48/80-byte "U2MP" head; PS3 invites dropped | PS3 form to any friend not known as a PC, PC head to PCs; both read (`presence_block.h` `encode_ps3_invite` / `decode_ps3_invite`) |
| Join Party (EBOOT `0x43ff60`) | NP message 0x10b to the friend, the friend answers 0x10c with its party room | joins the room from the presence | the same; a PS3 friend's party room from block +0x28 unless +0x07 is set. A PS3 asking a PC with 0x10b gets no answer (gap R7) |

## 2. Rooms (Matching2): gaps, not applied

| # | Item | PS3 | PC | Fix |
|---|---|---|---|---|
| R1 | World | party and matchmaking: the first world of the first server; custom host: a world from DC `e5350103`; PORT-ONLINE says 0x2001 | the one world the online program picks (65537, "lowest server/world id"); `kWorldOverride` 0 | log the worlds RPCN lists for NPWR00462 and use the PS3's. Searches are per world: with different worlds nobody sees anybody. Check first in a test (T2). |
| R2 | 0x4C party room key | version \| 0x100000 (`parity-f` F11, `parity-h` H4) | the plain version | PS3 form; hide the party room (R5) at the same time, else PS3's custom game list shows PC parties |
| R3 | 0x4C / 0x4D map packs | the party's common pack mask (member data +19, AND over members) only when the career bit (PS4 NetPlayerData +0x1b0 bit 0, "Matchmaking: DLC") is set and the playlist has pack maps; 0x4D carries the packs | `session.cpp mm_pack_mask` | **Do not change on this branch**: the map-pack code comes from branch `u2-dlc-regions`. For PS3 <-> PC matchmaking the 0x4C key must be equal (EQ filter), so the PC must add the mask to 0x4C under exactly the PS3 condition and with the PS3 bit meaning; 0x4D must hold the PS3 value (the packs) because PS3 reads it, also under "Default" (where the key has no mask). Apply after the merge. |
| R4 | 0x50 region, 0x51 tier | region (country << 16) \| language, EQ in the criteria rows that ask; tier 1..4 (`0x4c54b0`), searched GE tier - pass and LE tier | region 0x4E; tier = our rank | PS3 formulas. A PC rank above 4 in 0x51 hides every PC matchmaking room from PS3 searches: a blocker for matchmaking once the link exists |
| R5 | Party room flags | hidden and closed from NetInit (`0x448ed4`) | open and listed | PS3 form, with R2. Note: the presence block +0x07 is this hidden flag; if PS3 parties are always hidden, PS3 never offers Join Party from presence, only through 0x10b / 0x10c (open question Q5) |
| R6 | Member data 0x59 (48 bytes, `0x4af240`) | +0 u64 party room **big-endian**, +8 rank, +9 team, +10..17 clan tag, +19 map packs, +20..35 skill words, +36..43 address words | +0 little-endian, the rest 0 | PS3 byte order and fields; old PCs read +0 little-endian, so switch together with the link work (a mixed PC party would lose its follow data) |
| R7 | Join handshake | link to the owner first, NetEventMessage 0x100 reserve, wait up to 10 s for 0x101, then JoinRoom (`PORT-MATCHMAKING.md` 3.2) | JoinRoom first, then our hello / Info / kReserve | needs the PS3 peer link (L1) and the type 46 codes (L4) |
| R8 | Party follow | NetEventMessage mailboxes over the game link: 0x102 / 0x113 {owner id, count, u64 room}, 0x10d {u8}, 0x10e {u8}, 0x107 / 0x114, 0x103 {u64}; member data +0 | session messages kOpFollowRoom {u64 LE, kind}, kOpPartyIn, kOpJoinResult | type 46 codes in PS3 form (L4) |
| R9 | Signaling | rooms with signaling NONE, own NP signaling by NpId | Matching2 mesh signaling | check in a capture whether the online program's signaling reaches RPCS3 (T3) |
| R10 | Playlist index (0x4C bits 20..) | position in the PS3 playlist table (`0x491a04`) | PS3 order 0..8 since PR #58 (Gold Rush 9, Survival 10 PC only) | only Deathmatch (key 0x19004e) checked live; check the others in T6 |

Equal already: custom game key (0x0009004e), slot counts (party 10, custom 10 or co-op 3, matchmaking the
playlist's), 0x53 state, 0x55 (8 bytes), 0x58 name ("%s.%lld"), owner succession, no room password, no room
messages. 0x52 (kind) is PC only and harmless.

## 3. Game link: gaps, not applied

| # | Item | PS3 | PC | Fix |
|---|---|---|---|---|
| L1 | Datagram | the bit stream itself on vport 1: u8 kind (0 data, 2/3 clock request / answer, 4/5 ping / pong), kind 0: has-messages bit, seq 11, ack 10, ack bits 32, messages with relay blocks (`TRANSPORT-INTERFACE.md` 2) | `net.h` transport: 15-byte little-endian header, message frames {channel, reliable id, len}, the game packet inside channel tag 0 | a per-peer "PS3 form" in `net.cpp` / `gamenet.cpp`: send the bare bit stream, no U2MP header or frames; answer kinds 2..5 |
| L2 | Word order | big-endian 32-bit words, PS3 length rounding unknown | native little-endian words, whole words only, other sizes dropped (`gamenet.cpp` 177 / 188) | reverse every word for PS3 peers, pad received datagrams to whole words (U3: B1) |
| L3 | Topology | mesh: every member sends to every member it can reach, else through the owner with a relay list | star: clients talk only to the host (`net.cpp` accepts only the host's address) | accept PS3 packets from non-host peers, or confirm a PS3 relays through the host |
| L4 | Session messages | NetEventMessage (type 46) codes 0x100 reserve, 0x101 reserved, 0x103, 0x105 host leaving, 0x10d, 0x10e, 0x110 mode, 0x117, 0x118..0x11a party up | our own session messages; the PC only injects mailbox entries locally | send and accept the type 46 codes for PS3 peers |
| L5 | Clock | packet kinds 2 / 3 | session kClock / kClockSync | kinds 2 / 3 for PS3 peers |
| L6 | Net ids, trackers | net ids from the room (12-bit in events) | host slot + 1; trackers get the fake NpId "lan-player-%u" and preset ready flags (`session.cpp` 321..331) | PS3 ids and real NpIds; wait for the PS3's real ConnectionDone / ReadyToStart |

## 4. Events and gameplay: gaps, not checked

- Type ids 0..79 are the same on both builds (factory switch). PS3 has no own case for type 62; no PS3-only ids
  above 79 are known.
- Bit layouts checked equal: 37, 38, 44, 47, 49, 51..55, 57, 61, 67, 76, and the empty 0, 60, 65.
- Known different: 8, 9, 11 (the PC adds 32-bit words after the game's fields) and 12 (the PC writes 10-bit anim
  phases and pads the snapshot bit count to words; PS3 uses 4/8/16 bits; the anim blob's byte order needs a second
  swap).
- Unknown: every other live type. Highest risk (by the U3 findings): 2 LoadLevel, 18 AssignTeam, 29 Respawn, 45
  PlayerInfo, 46 Message, 12 CharacterMove. Method: compare each PS3 Read/Write body with the PS4 one, field by
  field (as `research/ps3cross/SPEC-events.md` did for U3).
- Net game index: the PC keeps the PS4 `*net-games*` table (records 0x90; PS3 0x80, from net-patch9 online). The
  index order PS3 vs PC is not checked; a PS3 host's LoadLevel index must map to the same game on PC.
- Spawners, maps: PS3 spawner ids must exist in the PC's imported packages; the PC invents a fallback spawner when an
  id is missing.
- Join in progress does not exist on PS3 (the room is hidden during a match): nothing to sync.

## 5. Open questions (need an RPCS3 capture or more EBOOT reading)

Q1 the world a PS3 uses under RPCN; Q2 the PS3 datagram length rounding and the RPCS3 6-byte header byte order;
Q3 the payloads of 0x100 / 0x101 and the other type 46 codes; Q4 the NP message main type and body of 0x10b / 0x10c;
Q5 whether PS3 party rooms are always hidden (then presence Join Party never shows on PS3); Q6 the tier source
(NetPlayerData +224 on PS3) and the region value under RPCS3; Q7 every event layout in section 4; Q8 the
`*net-games*` order of PS3 net-patch9 vs PS4.

## 6. Test steps for the owner (RPCS3 + PC, both signed in to RPCN, friends)

Presence and invites (this branch):

- T1 Party Up, both in the multiplayer menu. Expected on PC: the PS3 friend shows the title line and "In lobby"
  (no longer "Playing Single-Player"); with a PS3 party of 2+: "2 players in party". On RPCS3: the PC friend shows the
  same. PC log: `online: presence: party: activity 0, party 1, room <id>, net game -1 (request N)` every change, at
  most every 5 s.
- T2 PC log when the party room is made: `online: creating the party room in world W: key 0x...`; RPCS3 log: the world its game creates or searches
  rooms in. Different numbers = R1 (nobody can see the other's rooms).
- T3 PS3 starts a match (with a time limit) with another player or bots if possible; PC Party Up card (View
  Profile): the match line with map / type and "Match ends in N minutes". PC log: none needed; the card shows it.
  PC in a match: RPCS3's card shows the same for the PC.
- T4 PC invites the PS3 friend (Invite To Party). PC log `online: invite to <ps3> (PS3 form): party room <id>`;
  RPCS3 shows the system invite. Accepting it will not get further than the room join (section 3).
- T5 PS3 invites the PC. PC log `online: invite from <ps3>: party room <id> (0/0)` and the "invited you" message.
- T6 (later, matchmaking) both search Deathmatch at the same time; compare the PC line `online: creating the matchmaking room in world W: key 0x...` (or `join list: searching world W for key 0x...`) with the key RPCS3 logs.

Everything after a room join (party forming, custom game, matchmaking match, spawning, playing) is expected to fail
until section 3 is done; a failure there is not a regression.
