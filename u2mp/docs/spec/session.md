# Spec: session (session.cpp)

This is the clean-room description of `gow3_pc/src/u2mp/session.cpp`. It says what the code must do, as
seen from outside: guest calls, calls into other module files, game memory, transport messages, log
lines. It does not say how the code is organised. Names of internal helpers are free. Where this text
says "in order", the order is part of the contract.

Conventions used in this spec:

- Addresses are image offsets of the PS4 Uncharted 2 1.02 executable. "base" is the loaded image base.
  A guest pointer value is base + offset. Guest pointers stored in game memory are host-usable pointers
  (the image is mapped at base), so "read the u64 at X and use it as a pointer" means exactly that.
- "menu" = the menu manager object at `0x1136640` (`addr::menu_manager`).
- "netInfo" = the object at `0x104f1c8`. "ngm" = the NetGameManager at `0x113b760`.
- "party" = the party session object at `0x105efb8`. "match" = the match session object at `0x1065b08`.
- "NET_SM" = the s32 at `0x14ce97c` (network state machine; the game treats state > 1 as "online").
- "NPD" = the NetPlayerData object at `0x11474f8`.
- All game memory and all wire bytes are little-endian. `s16`, `s32`, `u8`, `u32`, `u64`, `f32` give
  size and signedness.
- Every guest function is called with the System V x86-64 convention (`U2MP_GUEST`). Every function the
  guest calls (replaced functions, vtable slots, page handlers) must also use it.
- `log(...)` = `u2mp::log` (prints `u2mp: ` + text + newline). Log texts are printf format strings in
  backticks and must match exactly, including punctuation and the PS3 addresses inside some of them.
- `lookup(id)` = guest call `0xa15e70(u32 id)` (script data lookup by id), returning a pointer or NULL.
  The two tables it is used for have the layout `{s32 count; s32 pad; u8 *records}` (records at +8).
- `*net-games*` = `lookup(0x3b9a067d)`: records of 0x90 bytes; record +4 = u32 level id, +0x0c = s32
  game type, +0x78 = u32 second level id.
- `*levels*` = `lookup(0x0dec97a4)`: records of 0x38 bytes; record +0 = u32 level id, +0x18 = pointer
  to the level's package name (C string, may be NULL).
- "the current net game" = the s16 at ngm+0x1ca0 (-1 = none).
- "co-op type" = game type 4, 8, 9 or 12.
- `sid(text)` = `u2mp::sid` (CRC-32, MSB first, poly 0x04C11DB7, no init, no final xor).
- "ctx" = the constant u64 `0x55324d50000001` (any non-zero session context; this exact value is
  required because it is written to game memory).
- Members, slots, net ids: see 3.3.

Shared mechanisms used as they are (not part of this file): `image.matches`, `is_ret_stub` (bytes
`c3 66 66 66 66`), `replace_function(offset, 5, host, name)` (logs `%s: 0x%llx -> C++` on success or its
own failure lines), `show_page(page)` (guest ShowPage(menu, page, 0)), `net::Transport`, `net::Address`,
`gamenet::*`, `voice::*`, `online::*`, `matchmaking::*`, `frontend6::*`, `netheap_install`,
`netheap_alloc`, `netinit_prepare`, `netinit_match`, `netinit_dump`, `netpatch_load`,
`extra_net_package`, `playlist_pick_type`, `dlc_net_game_available`, `unlocks_skin_fingerprint`,
`progression_publish`, `show_notice`, `runtime_file_translate` (extern "C", returns 0 on success); since
parity group D also `game_message_box`, `dlc_packs`, `dlc_net_game_pack`, `playlist_max_party`,
`cinema::start_playback` (u2mp_internal.h, cinema.h).

---

## 1. Purpose and where it hooks in

The PS4 build lost the PS3 game's multiplayer bootstrap (NetInit), its Matching2 party and match
sessions, the lobby roster queries, Start Match, Leave Multiplayer, and the per-frame network update.
This file is the PC replacement:

- It owns the one `net::Transport` of the module. One PC hosts the party (and later the match); the
  others join it by address (LAN) or through the online link (`online.cpp`).
- It keeps the party member list and publishes it into the game's party and match session objects and
  into the NetGameManager player trackers.
- It replaces five stripped guest functions (Multiplayer button, two roster queries, Start Match, Leave
  Multiplayer).
- It runs the network once per game frame, from a menu page-event handler it registers, and from ten
  wrapped TaskManagerOnline state Update slots (so the network also runs during a match).
- It owns the net clock object the game reads (300 ticks per second).
- It keeps the "net" menu art package requested and logs diagnostics on the match start path.

Install entry: `session::install(int &installed)` is called by `install_frontend` (frontend.cpp), which
install.cpp calls first among the frontend installs. `install_matchend` (later) wraps the Playing
Update slot that this file has already wrapped, and it refuses to install while that slot still holds
the original `base + 0x5f14d0`. So this file must write its wrapper pointer into the slot (see 6).

---

## 2. Interface

### 2.1 Declarations other files use (unchanged headers)

From `session_api.h` (namespace `u2mp::session`):

```
bool member_at(int row, uint32_t &net_id, std::string &name);
bool member_name(uint32_t net_id, std::string &name);
uint32_t local_id();
bool is_host();
bool kick(uint32_t net_id);
bool kick_griefer(uint32_t net_id);
std::string local_name();
int rank();
int member_count();
```

From `frontend3.h` (namespace `u2mp::session`):

```
constexpr uint8_t kFrontendMessages = 0x10;
void send_message(int slot, const std::vector<uint8_t> &body, int except = -1);
uint32_t net_ticks();
void create_match();
uint32_t member_rank(uint32_t net_id);
```

From `online.h` (namespace `u2mp::session`):

```
void online_reopen(net::Link *link, const net::Address *owner);
void online_match_ready(uint64_t room_id);
void set_local_name(const std::string &name);
bool member_address(uint32_t net_id, net::Address &out);
void online_migrated(bool host, bool matchmaking);
void online_host_gone();
bool match_migration_ok();
uint32_t net_id_of(const std::string &npid);
void online_rehost(net::Link *link, const net::Address *owner, uint32_t new_host_net_id);
constexpr uint8_t kOnlineMessage = 7;
```

From `u2mp_internal.h` (namespace `u2mp`): `const uint8_t *package_by_id(uint32_t id);`

Declared in frontend.cpp (namespace `u2mp::session`): `void install(int &installed);`

Known callers: frontend.cpp (install), frontend3.cpp (send_message, net_ticks, create_match,
member_count, is_host), frontend6.cpp (create_match, is_host, send_message), menus.cpp (local_name,
member_count, is_host, member_at), remaining.cpp (kick, kick_griefer, member_at, local_id, is_host,
member_name),
voice.cpp (local_id, member_name), online.cpp (member_rank, local_id, local_name, member_count, rank,
online_reopen, send_message, is_host, net_id_of, online_host_gone, online_rehost, online_migrated,
online_match_ready, set_local_name, match_migration_ok, member_address), menuimages / others
(package_by_id).

### 2.2 Entry points the game reaches

Replaced functions (each: 5 bytes at the address become a jump to our function; see 6 for the check):

| address | name passed to replace_function | expected original | our function's arguments |
|---|---|---|---|
| `0x5eb160` | `BUTTON_MULTIPLAYER` | ret stub | menu callback `(void*, void*, void*)`, all ignored |
| `0x659bb0` | `party roster query` | ret stub | `(u8 *query)` |
| `0x659bc0` | `player roster query` | ret stub | `(u8 *query)` (the matchmaking slot list, 5.13) |
| `0x5eb780` | `BUTTON_MULTIPLAYER_STARTMATCH` | ret stub | menu callback `(void*, void*, void*)`, ignored |
| `0x5eb180` | `LEAVE_MULTIPLAYER` | first 3 bytes `48 8d 3d` | menu callback `(void*, void*, void*)`, ignored |

Vtable slots overwritten (each a u64 in the image, holding `base + original` before install):

| slot address | original update | state (TaskManagerOnline) |
|---|---|---|
| `0x100ac58` (`0x100ac40`+0x18) | `0x5f2ea0` | EstablishConnection |
| `0x100ac18` (`0x100ac00`+0x18) | `0x5f2dc0` | LoadLevels |
| `0x100abd8` (`0x100abc0`+0x18) | `0x5f26d0` | WaitReady |
| `0x100ab98` (`0x100ab80`+0x18) | `0x5f14d0` | Playing |
| `0x100ab58` (`0x100ab40`+0x18) | `0x5f1390` | PreResetting |
| `0x100ab18` (`0x100ab00`+0x18) | `0x5f1000` | Resetting |
| `0x100aa98` (`0x100aa80`+0x18) | `0x5f0ec0` | ResettingReady |
| `0x100aa58` (`0x100aa40`+0x18) | `0x5f0b30` | ShowResults |
| `0x100aa18` (`0x100aa00`+0x18) | `0x5f0a90` | ShowRoundResults |
| `0x100a9d8` (`0x100a9c0`+0x18) | `0x5eff30` | Leave |

Each wrapper has the signature `u64 (void *state)` and returns what the original returned. The table
order above is the state index 0..9 used in 5.9 (the index matters only for the "which state ran last"
diagnostics).

Abort slot: `0x103fa70` (`0x103fa40`+0x30, match session vtable slot 6) holds `base + 0x5e9f10`
before install. It is replaced by a logging wrapper `void (void *self)` (5.10).

Menu page-event handler: an entry the module adds to the menu's page handler table (5.2). The
function has the signature `void (u32 *event)`; event[0] is the event number, 6 = per-frame update.

---

## 3. Game facts

### 3.1 Addresses, objects, fields

| fact | value | meaning |
|---|---|---|
| NET_SM | `0x14ce97c` (s32) | state; values below |
| party session | `0x105efb8` | session object |
| match session | `0x1065b08` | session object |
| session +0x10 | u64 | context (0 = no session) |
| session +0xb00 | u32 | my member id |
| session +0xb04 | u32 | host member id |
| session +0x228 | 10 records of 0xd0 | members |
| member record +0x09 | u8 | valid |
| member record +0x10 | u32 | member id |
| member record +0x18 | u32 | key |
| match +0x99 | u8 | "room created" (host page handler goes on) |
| match +0x9c | s32 | create result |
| match +0xb8 | u8 | "joined" |
| match +0xbc | s32 | join result |
| match +0xc0 | u64 | joined room |
| match +0xc8 | u8 | "left the room" (LEAVING_GAME page waits for it) |
| netInfo +0x08 | u8 | match created by me |
| netInfo +0x1c | u8 | network error (every MP page falls back to the error box) |
| netInfo +0x34 | s32 | came from matchmaking |
| netInfo +0x2c4 | s32 | multiplayer mode (1 = matchmaking, 2 = custom game) |
| ngm +0x00 | u64 | local player tracker pointer |
| ngm +0x08 | u64 | host player tracker pointer |
| ngm +0x10 | 10 trackers of 0x2c0 | player trackers |
| ngm +0x1ca0 | s16 | current net game (-1 none) |
| tracker +0x20 | char[64] | name (first byte 0 = free) |
| tracker +0x60 / +0xa0 | char[64] | display name / message-list second name |
| tracker +0x120 | u32 | net id |
| tracker +0x238 | u32 | cleared to 0 |
| tracker +0x23c | s32 | connection key (-1 = local) |
| tracker +0x26c | u8 | excluded |
| tracker +0x275 | u8 | connected |
| tracker +0x27c | u8 | talks to all |
| NPD `0x11474f8` +0xd8 | s32 | cash (rank source) |
| NPD +0x114 | s32 | custom game team preference (1 or 2) |
| NPD +0x44 | s32 | error box kind (diagnostics) |
| NPD +0x6c0 | s32 | error box state (diagnostics) |
| menu +0x9d0 | u64[...] | page stack (pointers to page records) |
| menu +0x1010 | s32 | top index of the page stack |
| page record +0x10 | u32 | page id |
| menu +0x1828 | u64 | pointer to the wait-text object; u32 at its +8 = text id |
| menu +0x24f0 | u32 | page handler count (at most 32) |
| menu +0x24f8 | entries of 0x18 | page handlers: +0 u32 page id (0 = every page), +8 u64 function, +0x10 u64 argument |
| package table | `0x1130390` | package manager |
| package table +0x10 | u64 | bucket array (u64 node pointers; entry [count] = end node) |
| package table +0x4c | s32 | bucket count |
| hash node | +0 u64 next, +0x10 u32 id, +0x18 u64 package pointer | |
| package +0x18 | u16 (flags) | bit 0x40 = requested like the game does; 0x80 = ready |
| package +0x30 | s32 | state (7 = logged in) |
| package +0x924 | s32 | login frame (image fade-in start) |
| package table +0x5a98 / +0x5aa0 | u32 count / pointer | wait list 0, entries 0x10: +8 u32 package id, +0xc u32 flags (bit 0 = skip) |
| package table +0x5aa8 / +0x5ab0 | u32 count / pointer | wait list 1, same entry layout (no skip bit) |
| net image table | `0x1130078` (`0x1130008`+0x70) | u64 pointer; u16 image count at its +2 |
| frame counter | `0x1116180`+0x144 = `0x11162c4` (s32) | used for the fade |
| net clock pointer | `0x113a040` (u64) | object; s32/u32 ticks at +0xc |
| abort byte | `0x113a270` (u8) | diagnostics only |
| match-over byte | `0x104f1c0` (u8) | cleared at match setup |
| match-over companion | `0x14ce978` (u8) | cleared at match setup |
| idle timer | `0x14cec0c` (f32) | diagnostics only |
| custom choice count | `0x113ddb0` (s32) | diagnostics only |
| level list head | `0x1534140` | circular list: head's +0 = first node; node +0 next, +0x10 s32 state, +0x98 u32 id |
| rank table | `lookup(0xc85e199d)` | +0x50 s32 count, +0x58 pointer to s32 thresholds |

### 3.2 Guest functions called

| address | arguments | meaning |
|---|---|---|
| `0x5cc6f0` | (menu) | PopPage |
| `0x63db90` | (ngm) | clear net players |
| `0x662a30` | (tracker) | clear one tracker |
| `0x6180f0` | (s32 type) -> event pointer or NULL | create net event |
| `0x62dc50` | (event, connection) | NetEventMessage Apply |
| `0x644ab0` | (ngm, event, u32 0xffffffff) | send event to every peer |
| `0x56ad90` | (package table, const char *name, s32 0) -> package or NULL | RequestPackage |
| `0x56ce50` | (package table) -> bool | every waited package loaded |
| `0xa18010` | () -> bool | loader busy |
| `0xa15e70` | (u32 id) -> pointer | data lookup |
| `0x63ffb0` | (ngm) -> s32 | present players |
| `0x63dc30` | (ngm, s16 game, NULL, s32 10) | SetNetGame |
| `0x5cd1e0` | (menu) | menu reset |
| `0x64aeb0` | (u8 out[0x20], ngm, s32 team, s32 variant) | team colour; colour = u32 at out+0xc |
| `0x5e9f30` | (NULL) | room owner changed: NGM host = owner, reset round |
| `0x5e9f10` | (self) | original abort (sets abort byte) |
| event vtable slot 1 (+0x08) | (event) | deleting destructor |
| event vtable slot 4 (+0x20) | (event, NULL) | Apply locally |

Net event fields used: +0x00 vtable pointer, +0x0a u8 reference count, +0x0c u32 (code or net id),
+0x10 u64 payload pointer, +0x18 u32 payload size. Event types: 46 = NetEventMessage, 79 =
NetEventPlayerLeft. A connection object handed to `0x62dc50` is a zeroed 0x200-byte, 16-aligned buffer
with the sender id as u32 at +0x160.

### 3.3 Members, slots, net ids

- The module keeps 11 member entries indexed by transport slot 0..10 (`net::kMaxPeers` = 10). Slot 0 is
  the host. Each entry: used flag, u32 net id, name string, u8 rank, u8 team (0 none, 1, 2). An empty
  entry is {unused, 0, "", 0, 0}.
- Net id of a slot: slot s >= 1 has net id s + 1; slot 0 has the current host net id ("host id",
  initially 1; changed only by host migration and reset to 1 by the online paths, see 5).
- "Slot order" = slots 0..10 ascending, used entries only.
- Game sessions have only 10 member records: the 11th used member (if any) is never written into them.

### 3.4 Page ids and other constants

| constant | value |
|---|---|
| PAGE_MULTIPLAYER | `0x53cbb5f2` |
| PAGE_MULTIPLAYER_JOINING | `sid("PAGE_MULTIPLAYER_JOINING")` = `0x1af4bdd2` |
| PAGE_MULTIPLAYER_LEAVING_GAME | `0xb808d03c` |
| host create page | `0x471007a1`, handler `base + 0x6093f0` |
| wait text id | `0x69deab50` |
| "net" package | `sid("net")` = `0x4240ef2e` |
| "net-u2mp" package | `sid("net-u2mp")` = `0x5e516de8` |
| voice icon (talking) | `0xffa98ec5` (`general/net/mic/default`) |
| voice icon (muted) | `0x7a5f5bf1` (`general/net/mic/mute`) |
| rank badge image type | `0x27` |
| net image type | `3` |
| team colour flag | `0x20000000` |
| mailbox 0x10e | party host joined (u8 payload 1) |
| mailbox 0x110 | multiplayer mode (s32 payload) |
| default UDP port | 27210 |

NET_SM values used: 0 off, 3 CREATE_GAME_WAIT, 12 CLIENT_WAIT_PARTY_HOST, 16 SERVER_LOBBY, 18
CHOOSE_HOST_JOIN (idle party), 26 UPDATE (match running), 35 LEAVE_GAME.

The 21 PS3 preload package names, in this order: `beretta-pistol`, `beretta-pistol-magazine`,
`colt-defender-pistol`, `colt-defender-magazine`, `desert-eagle`, `desert-eagle-magazine`,
`dragunov-magazine`, `dragunov-rifle`, `fn-fal-clip`, `fn-fal-rifle`, `m4-rifle`, `m4-rifle-magazine`,
`shotgun`, `spas12-shotgun`, `machine-gun`, `machine-gun-clip`, `rpg`, `anim-proto-multiplayer`,
`anim-proto-shield`, `anim-proto-carry-objects`, `m32-grenade-launcher`.

GetTeamColor byte checks (both must match, checked once): at `0x64aec1` the 11 bytes
`89 cb 41 89 d6 49 89 f5 49 89 fc`; at `0x64af57` the 10 bytes `c7 05 c7 3f e8 00 4b 2a 02 6a`.

---

## 4. Protocol

### 4.1 Transport and tags

All messages go over the module's single `net::Transport`. The first byte of every transport message
is a layer tag: 0 = game packet (`gamenet::receive(slot, data+1, size-1)`), 1 = session message (this
file, 4.2), 2 = voice (`voice::receive(slot, data+1, size-1)`). Other tags and empty messages are
ignored. Every session message this file sends is sent reliable (`reliable = true`).

Sending a session message `body` to "slot, except":
- slot >= 0: `transport.send(slot, true, [1] + body)`.
- slot < 0 and except < 0: `transport.broadcast(true, [1] + body)`.
- slot < 0 and except >= 0: `transport.send(s, true, [1] + body)` for s = 1..10 ascending where member
  s is used and s != except. (This also runs on a client, using the client's own member list.)

Roles: the host is transport slot 0; clients have slots 1..10. A client only talks to slot 0.

### 4.2 Session message types (byte after the tag)

| type | name | direction | layout after the type byte |
|---|---|---|---|
| 1 | Info | client -> host | u8 len (<= 31), name[len], u8 rank, u8 team, u32 skin fingerprint (LE), then three length-prefixed strings: hero skin (<= 47), villain skin (<= 47), clan tag (<= 4; newer clients only, older hosts stop before it), then u8 map packs (`dlc_packs()` bits: 1 base, 2 map pack 1, 4 map pack 2; parity group D, older hosts stop before it) |
| 2 | Roster | host -> all | repeated {u8 slot, u8 len (<= 31), u8 rank, name[len], u8 hero len, hero, u8 villain len, villain} in slot order (`roster_wire.h`) |
| 3 | Mode | host -> all | s32 mode (LE) |
| 4 | Follow | host -> clients | nothing (LAN), or u64 match room id (LE) when online |
| 5 | Kick | host -> one client | nothing |
| 6 | Teams | host -> all | repeated {u8 slot, u8 team} in slot order |
| 7 | Online | both | forwarded to `online::on_message(from, rest, size-1)` |
| 8 | Ids | host -> all (online) | repeated {u8 net id, u8 len (<= 16), npid[len]} |
| 9 | Clock | host -> clients | u32 game clock (LE); a client at NET_SM >= 12 takes it when its own differs by more than 150 ticks |
| 10 | Lobby | host -> clients | nothing: the client returns to the lobby with the host |
| 11 | Tags | host -> all | repeated {u8 slot, u8 len (<= 4), tag[len]}, every used slot in slot order (the host's own slot carries its live tag); sent right after every Roster. Newer than the other types: older peers ignore it, so the Roster layout stays as they read it |
| >= 0x10 | frontend | both | forwarded whole (type byte included) to `matchmaking::on_message(from, body, size)` |

Types 0 and 12..0x0f are ignored. An empty session message is ignored.

Receiving (from = transport slot of the sender):

- Info (host only, from in 1..10, size >= 2). Let L = byte 1. The member's name becomes bytes 2.. of
  length min(L, size - 2) (the member entry's used flag is not checked or changed). If size >= 3 + L,
  rank = byte 2+L. If size >= 4 + L, team = byte 3+L. If size >= 8 + L: fingerprint = u32 LE at 4+L;
  call `unlocks_skin_fingerprint()` once (mine); if fingerprint != 0, mine != 0 and they differ, log
  `party: %s's skin table differs (fingerprint %08x, ours %08x): a different import; skins may show as others`
  (name, theirs, mine) and call `show_notice(name + " has a different Uncharted 2 multiplayer import. Their character skins may show differently. Both players should run the importer again.")`.
  Then the hero and villain skin strings (u8 length, bytes; a cut string ends parsing), the clan tag
  (u8 length, bytes, first 4 kept; absent or cut = no tag) and the map packs byte (absent = 0xff, "not
  told": an older client is never refused for map packs).
  Then: refresh sessions (5.1), broadcast Roster (+ Ids), broadcast Teams. Not host / bad slot / short:
  nothing.
- Roster (client only). Remember every entry's team; clear all 11 entries; parse from offset 1 while
  at least 3 bytes remain: slot, len, rank; stop if slot > 10 or the name would run past the end;
  otherwise entry[slot] = {used, net id of slot (3.3), name, rank, the remembered team of that slot};
  advance 3 + len. Then refresh sessions. Host: ignored.
- Tags (client only). From offset 1 while 2 bytes remain: slot, len; stop if the tag would run past the
  end; if slot <= 10, entry[slot].tag = the first 4 bytes of the tag; advance 2 + len. A Roster clears the
  tags; the Tags that follow it set them again (an older host sends none: no tags).
- Teams (client only). From offset 1, while 2 bytes remain: if slot <= 10, entry[slot].team = team
  (used flag untouched). Pairs with slot > 10 are skipped, parsing goes on.
- Mode (client only, size >= 5): mailbox post (5.1) code 0x110, sender 1, payload the 4 bytes of the
  s32.
- Kick (client only, from == 0): log `party: the host kicked us`; netInfo+0x1c (u8) = 1; set the
  "kicked" flag (handled in the next frame, 5.8).
- Follow (client only): log `following the host into the match`; if size >= 9, call
  `online::follow_match_room(u64 at 1)`. Then in order: `netinit_match()`; netInfo+0x08 = 0;
  guest `0x63db90(ngm)`; match context = ctx, me = my net id, host = 1 (always the literal 1);
  publish members into match (5.1); match+0xbc (s32) = 0; match+0xc8 = 0; match+0xc0 (u64) = ctx;
  match+0xb8 = 1; fill trackers (5.1); `show_page(0x1af4bdd2)`; NET_SM = 12; mailbox post code 0x10e,
  sender 1, payload one byte 1.
- Ids (client only): clear the net-id-to-NpId map; from offset 1 while 2 bytes remain: id, len; stop if
  the text would run past the end; map[id] = text; advance 2 + len.
- Online (both roles): `online::on_message(from, body+1, size-1)`.

Sent:
- Info: built from my name (first min(len, 31) bytes), `local rank` (stored as "sent rank"),
  `local team` (stored as "sent team"), then `unlocks_skin_fingerprint()` LE. Call order: rank (guest
  lookup), team, fingerprint. Then hero, villain and our clan tag (stored as "sent tag") as length-prefixed strings. Always to slot 0.
- Roster: then Tags (every used slot: our live tag for our own slot, stored as "sent tag"; the member's
  tag else), then, only if `online::active()`, Ids:
  clear the map; for each used slot in order: NpId =
  `online::my_npid_text()` for slot 0, else `online::npid_at(address)` if `transport.peer_address(slot)`
  gives one, else empty; skip empty ones; map[net id] = full NpId; append net id (as u8), min(len,16),
  first 16 bytes. Ids is broadcast even when it holds no entries.
- Follow body: `[4]` plus, when `online::match_room()` is non-zero, its u64 LE (9 bytes total).

### 4.3 Environment variables

| variable | read when | effect |
|---|---|---|
| `UNCHARTED_U2MP_NAME` | start of the transport; `local_name()` | non-empty: my name = its first 31 bytes |
| `UNCHARTED_U2MP_PORT` | start of the transport | `atoi > 0`: port = that value truncated to u16; else 27210 |
| `UNCHARTED_U2MP_JOIN` | start of the transport (LAN only) | non-empty: join `ip[:port]` (default port as above) |
| `UNCHARTED_U2MP_PRELOAD` | each Multiplayer entry | exactly `0`: no preload, no log |
| `UNCHARTED_U2MP_HOST_MIGRATION` | `match_migration_ok()` | non-empty and first char `0`: migration off |

`set_local_name` writes `UNCHARTED_U2MP_NAME` with `_putenv_s`.

### 4.4 Timers

- Frame identity: `gamenet::clock()` (seconds, double). The per-frame work runs at most once per
  distinct clock value.
- Frame rate log: every 10 s of that clock.
- Online state log: wall clock (`steady_clock`, milliseconds), every 10 000 ms, plus on state change.
- Net clock: `steady_clock`, 300 ticks per second: ticks = (microseconds since base) * 3 / 10000 in
  64-bit integer arithmetic, truncated to u32.
- Teardown on a closed Multiplayer page: 30 consecutive menu update events.
- Missing level package with a file present: 20 s of `steady_clock`.
- Net image fade: 300 frames of the game frame counter.
- Transport timeouts and retries belong to `net::Transport` (not this file).

---

## 5. Behaviour

### 5.0 Module state (initial values)

members all empty; active = false; hosting = false; my net id = 0; my name = `Drake`; host id = 1;
sent mode = -1; last seen NET_SM = 0; frame counter = 0; kicked = false; host lost = false; sent rank
= 0xff; sent team = 0xff; next rate log = 0 (clock seconds); last frame clock = -1; rehost pending =
false; NpId map empty; block list empty; closed-page frame counter = 0; net clock object = none; last
online state index = -1; next online log = 0 ms. Per-member roster text buffers: 11 buffers of 64 chars (one per slot).

What is NOT reset by teardown (5.7): hosting, my net id, host id, rehost pending, NpId map, sent
rank/team, last seen NET_SM, the frame and rate-log counters, the net clock object.

### 5.1 Shared operations

- Set session(s, context, me, host): u64 s+0x10 = context; u32 s+0xb00 = me; u32 s+0xb04 = host.
- Publish members into session s: for each used member in slot order, while fewer than 10 written:
  record i at s+0x228+i*0xd0 gets u8 +9 = 1, u32 +0x10 = net id, u32 +0x18 = net id. Every remaining
  record up to 9: +9 = 0, +0x10 = 0, +0x18 = 0.
- Clear session s: context 0, me 0, host 0; for all 10 records: +9 = 0 and +0x10 = 0 (the +0x18 key is
  left as it was).
- Refresh sessions: publish into party; then publish into match only if match context (u64 match+0x10)
  is non-zero.
- Add tracker (net id, name, local): find the first tracker i (0..9) whose name byte is non-zero and
  whose net id equals; if none, the first tracker whose name byte is zero. None at all: log
  `no free player slot for %s` (name) and stop. Otherwise, with t = ngm+0x10+i*0x2c0: guest
  `0x662a30(t)`; write name into t+0x20 and the message-list second name at t+0xa0 as C strings
  limited to 64 bytes including the terminator (`Player` if name is empty); t+0x120 = net id;
  t+0x238 = 0; t+0x23c = -1 if local else net id; t+0x26c = 0; t+0x275 = 1; t+0x27c = 1; if
  local, ngm+0 = guest address of t; if net id == host id,
  ngm+8 = guest address of t. Log `player %s: net id %u, slot %d%s` with the given name (empty stays
  empty in the log), net id, i, and ` (local)` or empty.
- Fill trackers: add tracker for every used member in slot order, local = (net id == my net id).
- Player left (net id): nothing if 0. ev = guest `0x6180f0(79)`; nothing if NULL. u32 ev+0xc = net id;
  guest `0x644ab0(ngm, ev, 0xffffffff)`; call ev's vtable slot 4 (ev, NULL); release ev.
- Release event: decrement u8 ev+0x0a; if it reached 0, call ev's vtable slot 1 (ev).
- Mailbox post (code, sender, data, size): ev = guest `0x6180f0(46)`; nothing if NULL. Copy the payload
  to a buffer owned by the module that stays alive during the call; u32 ev+0xc = code; u64 ev+0x10 =
  buffer address; u32 ev+0x18 = size; guest `0x62dc50(ev, connection)` with the zeroed connection
  (+0x160 = sender); then u64 ev+0x10 = 0, u32 ev+0x18 = 0; release ev.
- Local rank: t = `lookup(0xc85e199d)`. If NULL, count (s32 t+0x50) <= 1 or the threshold pointer
  (t+0x58) is NULL: 0. Else money = s32 NPD+0xd8, last = count-1; the first i in 0..last-1 with
  thresholds[i] <= money < thresholds[i+1] gives min(i, 255); if none, min(last, 255) (this includes
  money below thresholds[0]).
- Local team: s32 NPD+0x114 if it is 1 or 2, else 0.
- Top page id: top = s32 menu+0x1010; if top < 0 or top > 0x40: 0. Else page = u64 menu+0x9d0+8*top;
  its u32 +0x10, or 0 if the pointer is NULL.
- Multiplayer page open: top as above; if top < 0 or top >= 200: true. Else true if any non-NULL page
  pointer at index 0..top has id 0x53cbb5f2; else false.
- Finish leaving game: if top page id == 0xb808d03c and match+0xc8 == 0: log
  `leaving the match: LEAVING_GAME may close (match +0xc8 = 1)`; netInfo+0x34 = 0; match+0xc8 = 1.
- Find package (id): buckets = u64 at table+0x10, count = s32 at table+0x4c; none if buckets is 0 or
  count <= 0. h = FNV-1a 32 (offset 0x811c9dc5, prime 0x01000193) over the 4 bytes of id, low byte
  first. node = buckets[h % count]; follow +0 while node is non-zero and its u32 +0x10 != id. None if
  node is 0 or equals buckets[count]. Else the u64 at node+0x18 as the package pointer.
- Request package (name): pkg = guest `0x56ad90(base+0x1130390, name, 0)`; if non-NULL, u8 at pkg+0x18
  |= 0x40. Returns pkg.
- Keep net package: if `extra_net_package()` returns a name, request it; then request `net` and return
  that result.
- Close transport: `transport.close()`, then empty the block list. Every place below that closes the
  transport (teardown, frame work step 6, `online_reopen`, `online_rehost`) does it this way, so the
  block list lives exactly as long as one hosted transport.
- Block list (Kick From Party and the griefer kick, PS3 room kickout with block flag 1): a map online id
  (NpId text) -> number
  of refused Hellos. Admit (installed as `transport.admit`, asked by the host transport before it gives
  a new peer a slot): true when the list is empty, when `online::npid_at(from)` is empty or not in the
  list. Else add 1 to that id's count; when the count before adding was a multiple of 20 (the first
  refusal, then one line per 10 s of 0.5 s Hello retries) log
  `party: refused a join from %s (kicked from this session; %u tries)` (NpId, new count); false. A refused
  Hello gets no slot, no Welcome and no peer event, exactly as when the party is full: online the
  joiner gives up through its "room owner not reached" path, on LAN it keeps waiting as for a full
  host.

### 5.2 Page handler registration

- Register: if any of the first `count` entries (count = u32 menu+0x24f0) has +8 == our page handler's
  address, nothing. If count >= 32: log `menu page handler table full: no per-frame updates in menus`.
  Else entry[count] = {u32 0, u64 handler address, u64 0}; count += 1; log
  `per-frame page handler registered (%u handlers)` (new count).
- Unregister: the first entry whose +8 is our handler is removed by moving every later entry down by
  one (0x18-byte copies), count -= 1. The old last entry's bytes stay in memory. No log. Nothing if
  absent.

### 5.3 Page handler (event pointer)

Only when the pointer is non-NULL and event[0] == 6:
0. (2026-11-25) First: `menus_help_bar_frame()` (menus.cpp, `MENUS.md` 3.8.1: the help bar
   L1 / R1 pictures), even while not active.
1. Finish leaving game (5.1), even while not active.
2. If active, NET_SM == 18 and the Multiplayer page is not open: increase the closed-page counter; when
   it reaches 30, reset it to 0, log
   `PAGE_MULTIPLAYER closed without Leave Multiplayer: multiplayer teardown (PS3 0x4386d0)`, run
   teardown without pop (5.7) and return (no frame work). Otherwise (any of the three conditions false)
   reset the counter to 0.
3. Frame work (5.8).

### 5.4 BUTTON_MULTIPLAYER

In order: log `Multiplayer selected`; `netheap_install()`; `netinit_prepare()`; `progression_multiplayer_entered()`
(2026-11-25, `progression.md` 1.5.19: the PS3 NetInit leaderboard start-over); `netpatch_load()`.
If not active: `voice::reset_volume()` (parity J6, commit dc349a9: PS3 NetInit made the speaker port at full volume
on every online start), then start the transport (5.5); on failure (PS3 bootstrap 0x438bc8 without a network, parity group
D 2.1) log `Multiplayer could not start: network message, staying in the main menu`, close transport (5.1),
`online::on_transport_closed("Multiplayer could not start")`, `game_message_box(0x88a082af, 0)` (body text,
OK button) and return: no error byte, no PAGE_MULTIPLAYER, active stays false, the player stays in the main
menu. On success active = true. Then: netInfo+0x1c = 0; netInfo+0x34 = 0; ngm+0x1ca0 = -1; party
session set to (ctx, 1, 1) when hosting, else (ctx, my net id or 0xfff if it is 0, 1); publish members
into party; sent mode = -1; NET_SM = 18; register the page handler; keep net package; preload (5.6);
`show_page(0x53cbb5f2)`.

### 5.5 Starting the transport

1. If `UNCHARTED_U2MP_NAME` is non-empty, my name = its first 31 bytes.
2. Port = 27210, or `UNCHARTED_U2MP_PORT` as in 4.3.
3. Install the transport's peer callback (5.11), message dispatcher (4.1) and admit check (5.1 block
   list); empty the block list; hand the transport to `gamenet::set_transport` and then
   `voice::set_transport`.
4. Clear all members.
5. link = `online::start()`. If non-NULL: `transport.host_link(link)`; false -> fail. Else hosting =
   true, my net id = 1, host id = 1, member[0] = {used, 1, my name, local rank, team 0}; log
   `online: hosting a party on the online link as %s` (my name); success.
6. Else if `UNCHARTED_U2MP_JOIN` is non-empty: parse it with `net::Address::parse(text, port, out)`,
   then `transport.join(out)` (only if the parse succeeded); either false -> log `cannot join %s` (the
   variable's text) and fail. Else hosting = false, my net id = 0; log `joining the party at %s`
   (`out.text()`); success. Host id is not changed.
7. Else `transport.host(port)`; false -> log `cannot listen on UDP port %u` (port) and fail. Else
   hosting = true, my net id = 1, member[0] = {used, 1, my name, local rank, 0}; log
   `hosting a party on UDP port %u`; success. Host id is not changed.

### 5.6 Package preload

Skipped silently when `UNCHARTED_U2MP_PRELOAD` is exactly `0`. Else for each of the 21 names in order:
if find package(sid(name)) finds it, set its keep flag (package +0x18 |= 0x40, the flag the teardown
cleared on the last leave) and count "present"; else request it and count "requested" when it
returns non-NULL, "not created" when NULL. Then log
`package preload (PS3 0xe155d0): %d requested, %d already present, %d not created`.

### 5.7 LEAVE_MULTIPLAYER and teardown

LEAVE_MULTIPLAYER = teardown with pop. Teardown (pop, leaving: true except the client's "leaving the match
room and returning to the Multiplayer menu" path of the frame work (5.8), which stays in Multiplayer), in order: log `leaving multiplayer`; unregister the
page handler; `gamenet::detach(s)` for s = 0..10; `voice::stop()`; close transport (5.1);
`online::on_transport_closed("Multiplayer left")`; active = false; clear party session; clear match
session; netInfo+0x2c4 = 0; guest `0x63db90(ngm)`; `progression_publish()`; NET_SM = 0;
clear all members; when leaving, the PS3 0x4386d0 releases (parity group D 2.3): release packages, then
`frontend6::preload_net_game(false, true, false)` (PS3 0x4a12e8(ngm, 0, 1, 0), with the net game still
current); then ngm+0x1ca0 = -1; with pop: guest `0x5cc6f0(menu)`.

Release packages: for `net`, the extra net package when `extra_net_package()` names one, and the 21 preload
names: a package that is present and has bit 0x40 at +0x18 loses that bit (counted); absent ones are skipped.
Then log `leaving multiplayer: %d package(s) no longer kept for Multiplayer`. The PS4 function that releases
a package request is not known (parity-d-flow1.md question 6): nothing else is called. A cinema transfer has
nothing to abort: `online::send_cinema` hands the whole file to the online program in one call.

### 5.8 Frame work

Runs from the page handler and after every wrapped state Update.
1. If active or a net clock object exists: update the net clock (5.12).
2. `online::frame()` (always).
3. If not active: stop.
4. now = `gamenet::clock()`. If now equals the last frame clock: stop. Else remember it.
5. `transport.pump(now)`; `gamenet::frame(now)`; `voice::frame()`; `transport.pump(now)`.
6. If kicked or host lost: log `party: closing the transport (%s); Multiplayer starts a new one` with
   `kicked` (kicked wins) or `host lost`; clear both flags; detach slots 0..10; `voice::stop()`;
   close transport (5.1); `online::on_transport_closed` with the same word; active = false; clear all
   members; stop. (Sessions, NET_SM and the page handler are left as they are.)
7. frame counter += 1. Skip net fade: for `net` then `net-u2mp`: find the package; if found with state
   7 and (F - s32 package+0x924) < 300 (signed 32-bit), where F = the s32 game frame counter at
   0x11162c4, set package+0x924 = F - 300.
8. Finish leaving game (5.1).
9. If frame counter % 60 == 1: check net package with "menus" = NET_SM in {18, 3, 12}: package = keep
   net package when menus, else find package(0x4240ef2e) without request; state = its s32 +0x30 or -1;
   table = u64 at 0x1130078; image count = u16 at table+2 or 0. Log
   `net package: %s, state %d, flags 0x%x; net image table %s, %u images` (`present`/`missing`, state,
   u16 package+0x18 or 0, `set`/`NULL`, count) when the state, the table's NULL-ness or the count
   differs from the last logged values (initially state -100, table "non-NULL", count 0xffffffff); then
   remember them.
10. If now >= next rate log: if next rate log > 0, log `frame hook: %u calls in 10 s` (frame counter);
    frame counter = 0; next rate log = now + 10.
11. If frame counter % 120 == 7: r = local rank, then t = local team; "tag changed" = our clan tag differs
    from the sent tag. If hosting, member 0 used and (its rank != r or tag changed): set the rank and
    broadcast Roster (+ Tags, Ids). Else if not hosting, my net id != 0 and (r != sent rank, t != sent team
    or tag changed): send Info. Then, separately: if hosting, member 0 used and its team != t: set it and
    broadcast Teams.
12. If hosting: mode = s32 netInfo+0x2c4; if it differs from sent mode, remember it and broadcast Mode.
13. s = NET_SM. If s differs from the last seen value: log `net state %d -> %d` (old, new); if s is 16
    call `netinit_dump("state 16")`, if s is 3 call `netinit_dump("state 3")`; if hosting, s == 3 and
    the old value was 35, match+0x99 = 0; remember s.

Frame work additions after the rewrite, active only (not described step by step here; the differential test's old
scenarios show them as differences, the new-only scenarios check the parity rules): before the clock check, the
client's Info resend while the roster is not synced and the host's roster rebroadcast every 120 frames in NET_SM 16 /
18, the skin sync (`unlocks_local_skin`, `unlocks_local_villain`, `unlocks_apply_slot_skins`) (934a6ca); after
`gamenet::frame`: `grenades::frame`, the host's lobby return and the client's leave paths, `hud_weapon_tick`
(934a6ca), `mute_players_frame` (parity N2, 50a6b69), the clock sync, slot manager and Party Up frames (parity E,
fa52c63); after the fade skip: tracker and stats team sync and the team preference (u32 NPD+0x114: co-op = the
host's team, else 1 + (local slot & 1); log `party: team preference %d for slot %u (%s, mode %d)`) (934a6ca). The page
handler (5.3) also calls `ticker_refresh` while the Multiplayer page is open (parity M, 514bcfe). The host's team of
a co-op client is the team of member slot 0 (the host is always slot 0, 3.3; 934a6ca read `members[host_id]`, the
host's net id used as a slot index, i.e. another client: fixed 2026-11-25, new-only scenario "5.8 co-op client").

### 5.9 Wrapped state Update (state index I, state pointer)

1. ms = steady_clock milliseconds. If the last state index differs from I (it is set to I in any case)
   or ms >= next online log: next online log = ms + 10000; log
   `online state: update 0x%llx (vtable slot 0x%llx)` (original update offset, state vtable offset =
   slot - 0x18). Then for LoadLevels: load-levels diagnostics (below); for Leave: leave reason
   diagnostics with `leave`.
2. LoadLevels only: missing level package check (below).
3. EstablishConnection only: if u8 0x104f1c0 or u8 0x14ce978 is non-zero: log
   `match setup: match-over flag was %u (0x14ce978 %u), cleared` (both values) and set both to 0.
4. If active, the state is EstablishConnection, LoadLevels or WaitReady, and ngm+8 is 0: log
   `match setup: the net players were cleared, registering the party again` and fill trackers.
5. Call the original Update (base + offset) with the state; keep its return value.
6. Frame work (5.8). Return the value.

Leave reason diagnostics (when): host = u64 ngm+8. Log
`%s: abort byte %u, host tracker %s, host excluded %u` (when, u8 0x113a270, `set`/`NULL`, u8 host+0x26c
or 0). Then players = guest `0x63ffb0(ngm)`; log
`%s: leave flag %u, error box kind %d state %d, idle %.1f s, players %d, room %d` (when, u8 0x104f1c0,
s32 NPD+0x44, s32 NPD+0x6c0, f32 0x14cec0c as double, players, NET_SM).

Load-levels diagnostics (state): leave reason with `load levels`. Task level: task = u64 at state+8;
if non-NULL, a = u64 task+0x80; if non-NULL, b = u64 a+0x88; if non-NULL, level = u32 b+0x28; else 0.
packages = guest `0x56ce50(base+0x1130390)`, then busy = guest `0xa18010()`. second = u32 record+0x78 of
the current net game in `*net-games*` (lookup made here), 0 when the table, its records or the index
(0 <= game < count) is invalid. Level list text: starting at the u64 stored at 0x1534140, for at most 24
nodes while the node is non-zero and not the head address (base+0x1534140), append ` %08x:%d` (u32 +0x98,
s32 +0x10) into a 512-byte buffer, stopping early once 496 or more characters are used. Log
`load levels: packages loaded %d, busy %d, task level %08x, second level %08x; levels%s`. Then only when
packages is false: for list 0 and list 1, for each entry (at most min(count, 256), none if the pointer
is NULL): skip list-0 entries with flag bit 0; find the package by the entry's id; when it is missing,
its state != 7, or its flags lack 0x80, log
`load levels: list %d package %08x (flags %x): %s, state %d, package flags 0x%x` (list, id, entry flags,
`present`/`missing`, state or -1, u16 flags or 0).

Missing level package check: games = `lookup(0x3b9a067d)`, levels = `lookup(0x0dec97a4)` (both calls
always, in this order). Stop unless both and both record pointers are non-NULL and 0 <= game < games
count. level id = record+4 of the current net game. name = the package name of the LAST `*levels*`
record with that level id (no early stop). Stop if none, if shorter than 4 chars, if it does not end in
`-pak`, or if find package(sid(name)) finds it. File wait (state kept across calls): when sid(name)
differs from the remembered id, remember it, remember now (steady_clock), and test whether the guest
file `/app0/u2data/build/main/pak22/ps4/<name>-ingame10.pak` exists (`runtime_file_translate` into a
1024-byte buffer returns 0 and `fopen(..., "rb")` succeeds). If the file exists and the cached
`load_levels_diagnostics` busy flag is set: once per id, log
`load levels: requesting %s while the loader is busy`, request the package with
`request_package(name)`, and stop (the loader is still working; do not set its wait entry skipped). If
the file exists, the loader is not busy, and less than 20 s have passed since that remembered time:
stop. If the file exists
(20 s passed): once per id, log `load levels: %s has a file but no package after 20 s (not registered?):
not waited for`. Then for each wait-list-0 entry (at most min(count, 256), pointer non-NULL) with id ==
sid(name) and flag bit 0 clear: set bit 0 and log
`load levels: %s is defined nowhere (no registry entry, no file): not waited for`.

#### 5.9.1 Playing (state 3, original `0x5f14d0`): additions after the rewrite

Origin: the squash commit 934a6ca (2026-11-24, no message; the legacy commits between the rewrite fc361d5 and the
squash are not in this repository), Players Met from parity group E (fa52c63). In the wrapped Update of Playing:
1. On entry (the last state index was not Playing), after the online state line of step 1 and before the
   original: if u8 match+0xc8 is 0, set it to 1 and log `playing: the match session is joined (match +0xc8 = 1)`;
   u8 0x104f1c0 = 0 and u8 0x14ce978 = 0 (the match-over bytes); Players Met (parity-e-flow2.md 2.9): the names
   (+0x20, at most 64 bytes) of the trackers 0..9 that are used (name byte non-zero), not excluded (+0x26c = 0) and
   not the local one (ngm+0) go to `progression_note_met` (not called when there is none). Remember now (ms) as
   the Playing start and set the resend count to 0.
2. Every Playing update, hosting only: while fewer than 2 resends were made and 1 s (first) / 10 s (second) have
   passed since the Playing start: guest `0x610840()` (the player setup sent again; a joiner can apply the setup
   before it has the host's slot) and log `playing: the player setup sent again (%d s)` (1, then 10).
3. After the original returned (on entry only): log `playing: update returned`; after the frame work (on entry
   only): log `playing: frame work returned`.

Create match (5.14) sets match+0xc8 = 0 before its `Start Match: creating the match` line (934a6ca).

#### 5.9.2 WaitReady (state 2): diagnostics after the rewrite (934a6ca)

What WaitReady's update (`0x5f26d0`) reads: the match task (u64 at state+8) +0x151 (u8, all ready) and +0x154 (s32
deadline in net clock ticks), the net clock object's +0xc (3.1), and the trackers: when no used (name byte non-zero),
not excluded (+0x26c = 0), +0x274 = 0 tracker other than the local one is left, it asks for a start at ticks + 900.
With the online state line of step 1 (state changed or 10 s), at most once per 2 s (steady clock): task = u64 at
state+8, clock = u64 at 0x113a040; log `wait ready: all ready %u, deadline %d, timer %d, local tracker 0x%llx`
(u8 task+0x151 and s32 task+0x154, 0 each when task is NULL; s32 clock+0xc, 0 when clock is NULL; u64 ngm+0), then
for each tracker i = 0..9 (t = ngm + 0x10 + i*0x2c0) whose name byte (+0x20) is non-zero:
`wait ready: slot %d net id %u%s, %02x %02x` (i, u32 t+0x120, ` (local)` when t is the local tracker else empty,
u8 t+0x26c, u8 t+0x274). (934a6ca read the ready byte and deadline at ngm+0x151 / +0x154, the timer at image
0x113a04c beside the clock pointer, an "id" 8 bytes before each tracker and stopped at the local tracker: corrected
2026-11-25 to the reads above.)

#### 5.9.3 Other match-state additions after the rewrite (934a6ca), active only

Not described step by step here and not in the differential test's old scenarios (they need an active session):
the reset watch (PreResetting / Resetting / ResettingReady: `reset: ...` lines; after 7 s stuck in PreResetting,
4 s without the level list shrinking, or 17 s past ResettingReady's start the task is moved on through its vtable
slot 0x178 / 0x180 / 0x168; the host marks remote slots ready (+0x274) after 8 s), the round results hold (ShowResults
and ShowRoundResults: the original is not called for 10 s, then `round results: held 10 s (state 0x%llx)` and the
host marks the other slots done (+0x26d)), the Leave state's lobby return and voluntary-leave paths (parity E 2.5).

### 5.10 Abort wrapper

Log `match abort (0x113a270 = 1) from %s0x%llx, session %p`: when the caller's return address lies in
[base, base + image size), `guest ` and the address minus base; else empty and the raw address; then
self. Then call `base + 0x5e9f10(self)`.

### 5.11 Transport peer events (slot, joined)

Host:
- joined: member[slot] = {used, net id of slot, `Player ` + decimal(slot + 1), 0, 0};
  `gamenet::attach(slot, net id, true)`; log `party: peer in slot %u joined`. If `online::active()`:
  `online::on_host_reached(net id)`; if match+0x99 != 0 and NET_SM == 16, log
  `online: slot %u joined while the lobby is up: sending the follow` and send the Follow body to that
  slot; then `matchmaking::on_peer_joined(slot)`.
- left: log `party: %s left` (the member's name before clearing); player left(its net id); clear the
  member; `gamenet::detach(slot)`.
- Both: refresh sessions; broadcast Roster (+ Ids). (No Teams broadcast here.)

Client (events for slots other than 0 are ignored):
- joined: my net id = net id of `transport.local_slot()` (3.3); party session = (ctx, my id, host id);
  `gamenet::attach(0, host id, true)`; send Info; log `party: joined the host as net id %u`;
  `online::on_host_reached(my id)`. If rehost pending: clear it; match session = (ctx, my id, host id);
  publish members into match; guest `0x5e9f30(NULL)`; log
  `match migration: reached the new host (net id %u); the round restarts under it` (host id).
- left: if `online::host_change_expected()`: log
  `online: the old host closed the link; following the leader to its new room`, `gamenet::detach(0)`.
  Else if `online::on_host_lost()` (called only now; it may call back into this file before
  returning): log `party: lost the host of the matchmaking room; matchmaking searches again`, detach 0. Else log
  `party: lost the host`, detach 0, netInfo+0x1c = 1, host lost = true.

### 5.12 Net clock

Update: if no object of ours exists: if u64 0x113a040 is non-zero, stop (someone else's); else
allocate 0x40 bytes aligned 16 with `netheap_alloc` (stop if NULL), zero it, remember now as its base,
store its address at 0x113a040, log `net clock created (300 ticks per second)`. Then, if 0x113a040
still holds our object, write s32 at +0xc = own ticks. The object is never freed.

Own ticks: since our object's base if it exists, else since a fixed origin taken at the first own-ticks
call of the process; 300 per second (4.4).

`net_ticks()`: if u64 0x113a040 is non-zero, the u32 at its +0xc; else own ticks (not 0).

### 5.13 Roster queries (PS3 parity group B, spec parity-b-menus.md 1, 2, 11)

Query object (PS4): +0 u32 op, +4 u32 value (sub index / picture), +8..+0x30 image descriptor, +0x38 s32 op 10
result, +0x3c u32 row colour, +0x40 u64 text, +0x48 **u32** table arg, +0x4c u32 op 10 selector. A member's
shown name is `"[TAG] name"` when it has a clan tag (ours: NetPlayerData +0x1a8 read live, up to 4 chars;
others: the tag of their Info / the host's Tags), else the name. Image descriptor written by "image(type,
value, icon = 0, colour = -1)": u32 +8 = type, u64 +0x10 = 0, u32 +0x18 = value, u32 +0x1c = icon, u32 +0x20 =
1 with an icon else 0, s32 +0x24 = -1, u32 +0x28 = 0, s32 +0x2c = colour, f32 +0x30 = 1.0. Member pictures:
value 0 -> image(0x27, rank); value 1 -> muted (`voice::muted`) ? image(3, 0, 0x7a5f5bf1) : talking
(`voice::talking`) ? image(3, 0, 0xffa98ec5) : nothing.

**Party / lobby rows** (`0x659bb0`, cond 16..35). row = arg % 10. The row order: slot 0 (the party host) first,
then in slot order the members whose team equals the host's team (the host's team is 0 when it has none, so
members without a team go with it), then the other members in slot order. m = the member of that row (none past
the end).
- op 7: with m, value = 0 for this PC's own row, 1 for the others (the own row keeps the cursor, presses do
  nothing: PS4 clears the button); without m nothing.
- op 10: with m and value (sub index) 0: selector 0 -> result 1 ("can be selected": the PS4 party-row control
  lets the cursor in only then); selector 1 -> result 0 for another member (connection state), nothing for the
  own row. Else nothing.
- op 3: with m, the member picture of value.
- op 17 (colour): stop unless m exists, arg > 9, netInfo+0x2c4 == 2 and m's team is 1 or 2. Look up
  `*net-games*`; stop unless valid and 0 <= game < count; stop if the game's type is a co-op type. Then
  (first time only) the GetTeamColor byte checks of 3.4; on mismatch log
  `party rows: GetTeamColor 0x64aeb0 differs, no team colours` (once) and from then on always stop here.
  Else guest `0x64aeb0(out, ngm, team - 1, 0)` with a zeroed 16-aligned 0x20-byte out; u32 +0x3c =
  (u32 out+0xc & 0xffffff) | 0x20000000.
- op 1 (text): with m, the text buffer of m's slot gets m's shown name cut to 63 chars and +0x40 = its address;
  without m, +0x40 = the address of a static empty string.
- any other op: value = 0.

**Matchmaking rows** (`0x659bc0`, cond 6..15, on MATCHMAKING and POSTGAME). row = arg. The slot list is built on
every call (PS3 `0x4b0008`): "gathering" = netInfo+0x34 == 1 (frontend3's phase; PS3 progress 0 / 1).
1. Our party's members first, then the other members, each in slot order, at most 10. A member is ours when
   online is off, it is this PC, its NpId is unknown, or `online::in_own_party(npid)` (NpId: ours from
   `online::my_npid_text()`, others from the Ids map). Ours are kind 2 (present); the others kind 1 ("Found
   Player") while gathering, else kind 2. Party id: ours `online::own_party_id()` (online) or 1 when the
   party has 2+ members (LAN), else 0; the others `online::member_party_id(npid)`.
2. While gathering and the chosen playlist (NetPlayerData +0x104) is known (`playlist_index` >= 0): open slots
   (kind 0) up to min(`online::mm_slots()`, 10) entries.
3. The party-id list for colours: every distinct non-zero party id held by 2 or more entries, in list order.

A row at or past the list's end answers nothing. Else, with e = the entry and m = its member:
- op 1: kind 2 -> m's shown name; kind 0 -> game text c0759450 ("Finding Player..."); kind 1 -> a91680a8
  ("Found Player"); into the row's own 64-byte buffer.
- op 3: value 2 with m -> image(0x1d, bars, 0, colour) with bars from
  `online::peer_rtt(npid)` (<= 49 ms 5, <= 79 4, <= 119 3, <= 199 2, else 1; 0 for this PC or no connection
  record) and colour from the link state `online::peer_link_state(npid)` (`peer_rules::bars_colour`, spec
  parity-i-connection.md I5: inactive 0x800000FF, pending 0x80FF0000, active 0x8000FF00, dead / none / this PC
  0x80808080); kind 2 -> the member picture of value; kind 0 and value 0 -> image(0x10, 0)
  (searching).
- op 10: sub index 0, selector 0 and kind 2 -> result 1.
- op 17: kind 2 whose party id is at position i of the party-id list -> u32 +0x3c = {0x1020E0E0, 0x102000E0,
  0x10E000E0, 0x1020E000, 0x10E02000}[i % 5].
- other ops (op 7 included): nothing.

### 5.14 BUTTON_MULTIPLAYER_STARTMATCH

Nothing unless hosting (active is not checked). mode = s32 netInfo+0x2c4. PS3 0x43aab0's checks come first
(parity group D 2.2); a failed check logs `Start Match: %s` (the reason) and shows
`game_message_box(body, second)`, and nothing starts. Party size = used members, at least 1.
- mode 1 (matchmaking): when `playlist_max_party(u32 NPD+0x104)` is > 0 and the party is larger: reason
  `the party is larger than the playlist allows`, box 0x58b7e078 / 0x3a586923, stop. (Our playlists carry
  no maximum yet: 0, no check.) Then `online::hide_for_start(true)` (the party room is hidden for the
  countdown; LAN: nothing).
- mode 1 and `matchmaking::installed()`: if `frontend6::leader_starting(true)` returns false, call
  `matchmaking::leader_start()`. Return.
- mode 1 otherwise: table = `lookup(0x3b9a067d)`; when non-NULL, count > 0 and records non-NULL:
  chosen = `playlist_pick_type()`; type = chosen if >= 0, else the type of record (current game, or 0 if
  the current game is negative; no upper bound check). Candidates = every index i in ascending order
  whose type equals it and for which `dlc_net_game_available(i)` is true (asked only for type
  matches). If any: pick = candidates[`std::rand()` % count] (one rand call); guest
  `0x63dc30(ngm, pick, NULL, 10)`; log `matchmaking: net game %d of %zu of type %d`. Then create the
  match now.
- mode 2 (custom game): with a current net game in `*net-games*` (else no checks), type = its +0x0c, in
  this order:
  1. `dlc_net_game_pack(game, local_ok)`: local_ok false -> reason `this PC lacks the map's map pack`, box
     0x0e39fdf3 (pack 1) or 0x2341ea7b (other) / 0x13cabc15. When the pack is 1 or 2 and this PC has it
     (`dlc_packs()` bit 1 << pack; without map pack 1 Ruins is the base map and nothing is checked), any
     other used member whose packs byte (4.2 Info, 0xff when not told) lacks that bit -> reason
     `a party member lacks the map's map pack`, box 0x58d7fbb3 / 0x13cabc15.
  2. Too many: party > 10 for types 0, 1, 2, 3, 5, 6, 7, 10, 11, > 3 for co-op types 4, 8, 9, 12, > 0 for any
     other type -> reason `too many players for the game type`, box 0x4a936499 / 0x13cabc15.
  3. Too few: party < 1 for every game type (owner rule 2026-11-25: the minimum of 2 is for matchmaking only;
     a custom game starts with 1 player in every game mode; `start_rules.h` `may_start(kCustom, ..)`) -> reason
     `too few players for the game type`, box 0xb90de2b1 / 0xcca3ca1f (so it never shows). **Deliberate
     difference**: PS3 needs 2 players for type 10 and the co-op types 4, 8, 9, 12; ours starts every type
     with 1 (solo co-op and the in-match co-op minimum: solocoop.cpp; solo Elimination: parity-n-inlined.md NI2).
  4. Type 10 with 2 or more players: when every used member is on team 1, or every one on team 2 (team 0
     breaks both) -> reason `every player is on one team`, box 0x101c7f10 / 0x5350146e. One player alone
     (or no member yet) is never refused (`start_rules.h` `one_team_refused`).
  Then `online::hide_for_start(true)`; if `frontend6::leader_starting(false)` returns true, return. Else
  create the match now.
- mode 3 (cinema): log `Cinema`; `cinema::start_playback()` (BUTTON_MULTIPLAYER_STARTCINEMA's action: the
  selected recording, menu reset, the replay task). PS3 also clears one netInfo flag (0x4a9c64): its PS4
  counterpart is not known, not done.
- other modes: log `Start Match: page mode %d: nothing to start` (PS3: nothing).

The leader's countdown cancel (frontend3.cpp `leader_back`) calls `online::hide_for_start(false)`.

Create the match now: log `Start Match: creating the match`; match+0xc8 = 0; match choice diagnostics;
`netinit_match()`; `netinit_dump("before create")`; host create; `netinit_dump("after create")`; match
session = (ctx, 1, 1); publish members into match; fill trackers; match+0x9c (s32) = 0. If
`online::create_match_room()` returns true, stop here. Else match+0x99 = 1 and broadcast the Follow
body.

Match choice diagnostics: log `match choice: mode %d, custom choice list %d entries, net game %d`
(netInfo+0x2c4, s32 0x113ddb0, current game). games = `lookup(0x3b9a067d)`. If NULL, game < 0, game >=
count or records NULL: log `match choice: net game %d is not in *net-games* (%d records)` (game, count
or -1 when NULL) and stop. Else level id = record+4, type = record+0x0c; level name = `?`, or (after
`lookup(0x0dec97a4)`, if non-NULL) the name of the FIRST `*levels*` record with that id when its name
pointer is non-NULL (search stops at the first id match either way). Log
`match choice: game type %d, level id %08x, package %s`.

Host create, in order: netInfo+0x08 = 1; guest `0x63db90(ngm)`; netInfo+0x34 = 0; guest
`0x5cd1e0(menu)`; u32 at (u64 at menu+0x1828) + 8 = 0x69deab50 (no NULL check); page handler entry for
page 0x471007a1: the first entry with that page id, else (count < 32) a new entry at count with count +=
1; if there is an entry, set it to {0x471007a1, base + 0x6093f0, 0}; register our page handler (5.2);
`show_page(0x471007a1)`; NET_SM = 3.

`create_match()` (public): create the match now only when active and hosting.

### 5.15 Public functions

- `member_at(row, id, name)`: the member of party row `row` in the order of 5.13 (active not checked); fills
  both and returns true; false otherwise (negative rows never match).
- `mm_member_at(row, id, name)`: the member of matchmaking row `row` (5.13) when that entry is kind 2; false
  for open or "Found Player" slots and past the list.
- `member_name(id, name)`: first used member with that net id; true and fills name; else false.
- `member_rank(id)`: rank of the first used member with that net id; else 0.
- `local_id()`: my net id when active, else 0.
- `is_host()`: active and hosting.
- `member_count()`: number of used members when active, else 0.
- `local_name()`: `UNCHARTED_U2MP_NAME` cut to 31 bytes when non-empty, else my name.
- `rank()`: local rank (one guest lookup).
- `send_message(slot, body, except)`: when active and body non-empty, send as in 4.1.
- `kick(id)` (Kick From Party, PS3 0x439cf8 kicks with the block flag; parity group D 2.4): false unless
  active, hosting, 2 <= id <= 11 and member id-1 used. Else npid = `online::npid_at` of
  `transport.peer_address(id - 1)` when that slot has an address, else empty (read before the kick); log
  `party: kicking %s (net id %u)`, send `[5]` to slot id-1; then empty npid: log
  `party: net id %u has no online id: kicked, not blocked`; else add npid to the block list (count 0; an id
  already there keeps its count) and log `party: %s blocked from joining again until this session ends`
  (npid). True.
- `kick_griefer(id)` (NetEventKickGriefer on the host; PS3 kicks out of the match room with the block
  flag set): false unless active and hosting. If id equals my net id (the host was named): log
  `party: this PC (the host) was named as the griefer: leaving the session`, netInfo+0x1c = 1, kicked =
  true (frame work step 6 closes the transport on the next frame, as for a kicked client), true.
  Otherwise `kick(id)`.
- `member_address(id, out)`: false unless active, hosting and 2 <= id <= 11; else
  `transport.peer_address(id - 1, out)`.
- `net_id_of(npid)`: the smallest net id whose stored NpId equals npid; 0 when none.
- `set_local_name(name)`: nothing if empty. n = first 31 bytes. Nothing if n equals my name and
  `local_name()` equals n. Else my name = n; `_putenv_s("UNCHARTED_U2MP_NAME", n)`; log
  `player name: %s (RPCN)`. Then, only when active: hosting with member 0 used -> its name = n, refresh
  sessions, broadcast Roster (+ Ids); not hosting with my net id != 0 -> send Info.
- `online_reopen(link, owner)`: detach 0..10; close transport (5.1); clear members; kicked = host lost =
  false; host id = 1; rehost pending = false. Owner NULL: hosting = result of
  `transport.host_link(link)`; my id = 1; member 0 = {used, 1, my name, local rank, 0}; party = (ctx, 1,
  1); publish into party; log `online: transport switch: hosting a new party on the online link`.
  Owner given: hosting = false; my id = 0; party = (ctx, 0xfff, 1); publish into party (all records
  cleared); `transport.join_link(link, *owner)` (no wanted slot; result ignored); log
  `online: transport switch: joining the room owner over the online link`. Both: sent mode = -1;
  active = true. (`online::on_transport_closed` is not called; callbacks are not re-installed.)
- `online_match_ready(room)`: if not active or not hosting: log
  `online: match room %llx ready but this PC no longer hosts: ignored`. Else log
  `online: Start Match goes on (match room %llx)`, match+0x99 = 1, broadcast the Follow body.
- `online_migrated(host, mm)`: log `party: new leader (%s)%s` (`this PC` / `another player`, and
  `, matchmaking goes on` when mm, else empty). mm: `matchmaking::on_migrated(host)` and nothing else.
  Else match+0x99 = 0 and clear the match session.
- `online_host_gone()`: nothing if not active or hosting. Else log
  `party: lost the host (nobody took over)`, netInfo+0x1c = 1, host lost = true.
- `match_migration_ok()`: false if not active, if hosting, or if `online::active()` is false (asked
  only after the first two pass); false if `UNCHARTED_U2MP_HOST_MIGRATION` is non-empty and starts with
  `0`; false if NET_SM != 26; then `lookup(0x3b9a067d)`: when valid and 0 <= game < count and the
  game's type is a co-op type, false. Else true.
- `online_rehost(link, owner, new_host)`: old = host id. Log
  `match migration: host net id %u left; net id %u hosts now%s` (old, new, empty with an owner, else
  ` (this PC)`). Detach 0..10; close transport (5.1); clear members; kicked = host lost = false; host id
  = new. Owner NULL: hosting = `transport.host_link(link)`; `transport.reserve_slot(my id - 1)` (as
  u8); member 0 = {used, my id, my name, local rank, local team} (rank lookup before team read).
  Owner given: hosting = false; `transport.join_link(link, *owner, my id - 1)`. Then: party = (ctx, my
  id, new); match = (ctx, my id, new); player left(old); active = true; sent mode = -1. Owner NULL:
  publish members into match, guest `0x5e9f30(NULL)`. Owner given: rehost pending = true.
- `package_by_id(id)`: find package (5.1), no request.

---

## 6. Install

`install(installed)` (steps marked * were added after the clean-room rewrite: 6.1):
1. If `gamenet::install()` returns false: return (no log from this file, nothing patched).
2. * `voice::install(installed)` (voice.cpp: the talking indicator table and the mouth patch, parity N5 / N6,
   commit 50a6b69; it counts its own patches).
3. * Lost links (6.1.1), then the actor request hook (6.1.2), then the three tracker state traces (6.1.3).
4. For the five functions of 2.2, in table order: check the expected bytes (LEAVE_MULTIPLAYER: `48 8d
   3d` at 0x5eb180; the others: `is_ret_stub`). On mismatch log
   `%s at 0x%llx: not the stripped function this port expects, left alone` (name, address) and go on.
   Else `replace_function(address, 5, our function, name)`; +1 to `installed` when it returns true.
5. Check all ten state slots: each u64 must equal base + its original update. If any differs: log
   `match state vtables differ: no per-frame network during matches` and return (no abort hook).
6. Remember the ten originals, write the ten wrapper addresses into the slots, log
   `match state updates wrapped (%zu states)` (10), +1 to `installed`.
7. If u64 at 0x103fa70 == base + 0x5e9f10: write the abort wrapper's address there (no log, no count).
8. The idle-kick limit is left as the game has it (6.1.4).

The page handler is not registered at install; Multiplayer entry and host create register it.

### 6.1 Install additions after the rewrite

Origin of 6.1.1 to 6.1.4: the squash commit 934a6ca (2026-11-24, no message; the session.cpp of the rewrite fc361d5
and of the last legacy commits 28105f6 / 78c3b43 has none of them, and the legacy commits in between are not in this
repository). The texts below are what the code does; the reasons are from its log texts and comments.

#### 6.1.1 Lost links: Playing's PSN record check (`0x5f17ed`)

The 6 bytes at 0x5f17ed (inside Playing::Update `0x5f14d0`): `0f 8c 13 01 00 00` (jl 0x5f1906) -> write
`90 e9 13 01 00 00` (nop; jmp 0x5f1906: the branch is always taken), log
`lost links: Playing's PSN record check jumped (0x5f17ed)`, +1. Already `90 e9 13 01 00 00`: +1, no log, no write.
Any other bytes: nothing (no log, no count).

#### 6.1.2 Actor requests after StartNetGame's package requests (`0x641b20`)

Needs the 7 bytes at 0x641b20 executable (else nothing). `48 8b 05 d9 90 a0 00` (mov rax, [0x104ac00]): take a
32-byte near stub, fill it with `48 b8 <u64 hook address>` `ff d0` (call rax) `ff 25 00 00 00 00 <u64 base+0x641b27>`
(jmp back after the replaced instruction); no stub or the stub out of rel32 reach of base+0x641b25: nothing (no
count). Then write `e9 <s32 stub - (base+0x641b25)> 90 90` at 0x641b20, log
`skins: actor requests hooked after StartNetGame's package requests (0x641b20)`, +1. Bytes starting with `e9`
(patched already): +1, no log. Other bytes: nothing.

The hook (System V, no arguments): `unlocks_apply_slot_skins()`; then the match skin requests: the local skin
(`unlocks_local_skin()`), the local villain (`unlocks_local_villain()`) when it differs, then every used member's hero
and (when different) villain, each name once; a name that is empty or `hero-default` is skipped; else guest
`0x56d170(base+0x1130390, name, 0)` and log `skins: actor '%s' requested from the level manager`. Returns the u64 at
0x104ac00 (what the replaced mov loaded). (The same skin requests also run in create match, 5.14.)

#### 6.1.3 Tracker state traces

`retarget_call(0x664d6a, 0x60ded0, send trace, "tracker state send")`, `retarget_call(0x60df2f, 0xb332a0, alloc trace,
"tracker state alloc")`, `retarget_call(0x60e291, 0x634970, helper trace, "tracker state helper")`, in this order,
+1 for each that returns true. Each trace calls its original with all its arguments and returns its result; for its
first 8 calls in the process (each trace counts its own) it logs around the original call:
- send (tracker): `tracker: state send enter`, `0x60ded0(tracker)`, `tracker: state send leave`;
- alloc (u64 size, context, u32 align): `tracker: state alloc enter %llu` (size), `0xb332a0(size, context, align)`,
  `tracker: state alloc leave`; returns the pointer;
- helper (six arguments, the last two integers): `tracker: state helper enter`, `0x634970(...)`,
  `tracker: state helper leave`; returns the u32 result.
From the 9th call on: the original only. Diagnostics only (parity-n-inlined.md 1 lists them as wrappers that call the
originals).

#### 6.1.4 Idle-kick limit (`0xe622a8`)

Not patched (since 2026-11-25). Playing's update (`0x5f16df`) compares the idle timer (f32 `0x14cec0c`) with the
f32 at `0xe622a8`, 120.0 in the PS4 1.02 image; parity-e-flow2.md 1 checked the same 120 s limit in PS3 1.10's
Playing::Update (`0x442558`), the version U2 parity follows. 934a6ca had raised it to 300 s (log
`idle kick: limit 120 s -> 300 s`, +1 to `installed`); that is removed so idle players are kicked as on PS3.

The file also contains an unused helper for the network heap context 0x14; it is never called and is
not part of the behaviour.

---

## 7. Test scenarios

Install
1. All bytes as expected: five `-> C++` lines, wrapped line, installed += 6, ten slots point to
   wrappers, abort slot replaced. (Since 6.1: plus voice::install and three traces, installed += 9 in an image
   without the 6.1 bytes; with them installed += 11.)
2. One stub differs (e.g. 0x659bc0 not a ret stub): its "left alone" line, installed += 5.
3. gamenet::install false: nothing at all.
4. One state slot differs: "vtables differ" line, no slot written, abort slot untouched.
5. Abort slot holds something else: states wrapped, abort slot untouched.

Multiplayer entry
6. LAN host (no JOIN, no online): `hosting a party on UDP port 27210`, member 0 = {1, name, rank},
   party session (ctx,1,1), record 0 valid id 1 key 1, NET_SM 18, handler registered + log, package
   requests (`net` flag |= 0x40), preload log, ShowPage(0x53cbb5f2).
7. UNCHARTED_U2MP_PORT=0 / -5 / 70000 / abc: port 27210, 27210, 4464, 27210.
8. Host fails to listen: `cannot listen...`, then the start failure of 5.4 (message box 0x88a082af, no
   ShowPage, netInfo+0x1c untouched), active stays false, NET_SM unchanged.
9. LAN join valid / unparsable address: `joining the party at ...` and party (ctx,0xfff,1) / `cannot join
   x` and error path.
10. Online start: link given -> online log; host_link false -> error path.
11. Second entry while active: no transport start; session and state set again; handler not added
    twice (no second registration log).
12. UNCHARTED_U2MP_PRELOAD=0: no preload log; some packages present: counts.
13. Handler table full (count 32): full log, no entry.
14. UNCHARTED_U2MP_NAME longer than 31 bytes: name cut.

Peers and messages (host)
15. Peer joins slot 3: member {4, "Player 4"}, attach(3,4,true), log, roster `02 00 len rank name 03 08 00
    "Player 4"` broadcast, no Teams.
16. Online peer joins with match+0x99 = 1 and NET_SM 16: follow sent to that slot (with room id when
    match_room != 0), on_peer_joined called; NET_SM 17: no follow.
17. Peer leaves: `party: X left`, event 79 created, 0x644ab0 + Apply + release, member cleared, detach,
    roster broadcast.
18. Info full (name, rank, team, fingerprint): name/rank/team stored; roster then teams broadcast.
    Same fingerprint, zero fingerprint, different fingerprint (log + notice).
19. Info short variants: only name; name+rank; L larger than the rest (name cut, no rank); size 1
    (ignored); from slot 0 or 11 (ignored); arriving on a client (ignored).
20. Online host roster: Ids message with NpIds cut to 16 bytes, slots without NpId skipped, map holds
    full NpIds; `net_id_of` finds them.

Messages (client)
21. Roster with 3 members, then Teams: teams applied; a later Roster keeps teams; Roster with slot 11
    or truncated name stops parsing.
22. Teams with slot 12 in the middle: skipped, the rest applied.
23. Mode with 5 bytes: mailbox 0x110 event, payload 4 bytes, sender 1 at connection+0x160, event fields
    cleared and released; 4-byte Mode ignored.
24. Kick from slot 0: log, error byte, next frame closes (`kicked`), active false; Kick from another
    slot: ignored.
25. Follow LAN (1 byte) and online (9 bytes): full sequence of 4.2 including ShowPage(0x1af4bdd2),
    NET_SM 12, mailbox 0x10e with byte 1; follow_match_room only with 9 bytes.
26. Ids parse including a truncated last entry.
27. Type 7 to online::on_message; type 0x10 and 0x17 to matchmaking::on_message with the type byte;
    type 9 ignored; empty ignored.
28. Client joined event: my id from local slot (slot 2 -> 3; slot 0 -> host id), Info sent, logs;
    with rehost pending: match session, 0x5e9f30(NULL), migration log.
29. Client host lost: three branches (host change expected / on_host_lost true / plain lost, which sets
    the error byte and closes on the next frame with `host lost`).

Frames
30. Page handler with event 5: nothing; event 6 with LEAVING_GAME on top and match+0xc8 = 0: log,
    netInfo+0x34 = 0, byte set; second time: nothing.
31. Active, NET_SM 18, Multiplayer page not on the stack for 29 events: no teardown; 30th: teardown log,
    no pop; page back on stack resets the counter; top index 250: treated as open.
32. Same clock value twice: second call only updates the net clock and runs online::frame.
33. Frame 1: net package log; frame-rate log only from the second 10 s window.
34. Every 120 frames (frame % 120 == 7): host rank change -> roster; team change -> teams; client rank
    or team change -> Info; unchanged -> nothing.
35. Host mode change -> Mode message once per change; the first frame after entry always sends it.
36. NET_SM changes: log; 16 and 3 dump; 35 -> 3 while hosting clears match+0x99.
37. Fade: package state 7 with login frame = now - 100 -> set to now - 300; state 6 untouched.

Net clock
38. Global 0 while active: allocation, log, ticks written each frame (300/s); global already non-zero
    before: never created, net_ticks reads the other object; outside MP net_ticks returns own ticks.

Match states
39. EstablishConnection with match-over bytes set: log and cleared; with ngm+8 = 0 and active: refill
    log and trackers; original called before the frame work, return value passed through.
40. LoadLevels diagnostics: once on entry and every 10 s; list logs only when packages not loaded.
41. Missing `<map>-pak`: no file -> skip bit set + log; file present -> no change for 20 s, then the
    20 s log once and the skip bit with the "defined nowhere" log; name not ending in -pak: nothing.
42. Leave state: leave reason logs.
43. Abort wrapper called from guest code: log with `guest ` and offset, original called with self.

Start Match
44. Not hosting: nothing. Mode 2, frontend6 popup takes it: nothing more; popup not installed: full
    create sequence, LAN follow broadcast, match+0x99 = 1.
45. Online create_match_room true: stops before match+0x99; later online_match_ready(room) sets it and
    sends the 9-byte follow; online_match_ready when not hosting: ignored log.
46. Mode 1 with matchmaking installed: leader_starting(true) false -> leader_start.
47. Mode 1 without matchmaking: playlist type or the current game's type; DLC filter; rand pick;
    SetNetGame(ngm, pick, NULL, 10); log; then create. No candidates: no SetNetGame, still create.
48. Host create with an existing 0x471007a1 entry (reused) and without (appended); table full (no
    entry written, still ShowPage and NET_SM 3).
49. Match choice: invalid game log; valid game with unknown level `?`.

Roster queries
50. op 7 rows 0..9 and arg 10..19 (same rows); op 3 badge (value 0) and voice icon (value 1: talking,
    muted, neither); op 1 with and without member (row 0 buffer cleared); op 17 for each stop
    condition, the byte check mismatch (one log), and the colour write; unknown op -> value 0.

Teardown and public calls
51. LEAVE_MULTIPLAYER: full sequence with PopPage; handler entries after ours shift down.
52. kick: own id 1, id 12, unused slot -> false; valid -> log and `[1,5]` to that slot.
53. set_local_name: empty, same name, new name while hosting (roster), while client (Info), inactive.
54. online_reopen with and without owner; online_migrated mm / not mm; online_host_gone when hosting
    (nothing) and as client.
55. match_migration_ok for each false branch and true.
56. online_rehost as new host (reserve slot, member 0 with my id, player left(old), 0x5e9f30) and as
    member (join_link with slot my id - 1, rehost pending, then the joined event finishes it).
57. kick_griefer: not hosting -> false; own id -> log, netInfo+0x1c = 1, next frame closes (`kicked`);
    member with an NpId -> `[1,5]` to it, blocked; its next Hello is refused (no slot, one log line,
    the next line at the 21st try); a member without NpId (LAN) -> kicked, not blocked; after any
    close transport the same NpId joins again. Kick From Party blocks the same way (parity group D).
58. Start Match checks (parity group D): mode 2, type 8 with 4 members -> too many box; 3 -> countdown;
    type 8 with 1 member -> countdown (solo co-op); any other type alone (type 10 too) -> countdown (custom solo); type 10
    with 2 members all on team 2 ->
    one-team box; a member whose Info said packs 1 (base only) on a map pack 2 map -> member box; an older
    member (no packs byte) -> countdown; mode 1 with a playlist maximum below the party -> party box; mode 3
    -> `Cinema`, start playback; mode 0 -> log only. Every started countdown calls hide_for_start(true).
59. BUTTON_MULTIPLAYER, transport start fails: log, close, box 0x88a082af, no page, active false.
60. Teardown releases: present packages with bit 0x40 lose it (count in the log), absent skipped, then the
    level preload release, then ngm+0x1ca0 = -1; the client's leave-the-match-room path does neither.

## Clarifications found by the differential test

- Missing level package (5.9): the 20 s line is logged once per id in a row: only the last logged id is remembered,
  so a different id in between allows the same id again.
- Match choice diagnostics (5.14): a `*levels*` table whose records pointer is NULL has no records; the name stays "?".

## Differential test after the rewrite (2026-11-25)

`gow3_pc/tests/u2mp_rewrite/test_session.cpp` compares the rewrite's base (62fc26b) with today's session.cpp.
`expected_new` turns the old record and image into what the new build must give, by these documented changes only:
6 / 6.1 (voice::install, the three traces, the 6.1.1 / 6.1.2 results for the bytes the image holds, the
installed count), 5.4 (J6 `voice::reset_volume` before the transport start), 5.6 (present preload packages get bit
0x40), 5.9.1 (Playing entry lines, match +0xc8, the match-over bytes). Everything else must match unchanged. The
post-rewrite parity behaviour is not rebuilt in the comparison: new-only scenarios (`N ...`) check 6.1 with the PS4
bytes in place (the idle limit stays 120 s, 6.1.4), the traces, J6, D 2.1 (no network), the D teardown releases,
5.9.1 with Players Met, 5.9.2 (task, clock and tracker reads), the 5.8 co-op team preference (slot 0), H1, H5, H8 and
the E 2.2 / I6 timeout line. Old scenarios still differing: the three that run WaitReady (5.9.2 lines, not mapped), the four
without a network (D 2.1, new-only), LEAVE_MULTIPLAYER without entering (D releases, new-only), Start Match with a
full table (D checks), and every scenario with an active session (the frame work additions of 5.8).

## Parity group F changes (2026-11-25, spec parity-f-party-mm.md 7)

New interface (session_api.h, frontend3.h, online.h's `u2mp::session` part):

- `bool party_stranger(uint32_t net_id)`: active, online, not our own id, the NpId of that net id is known (`npids`)
  and `online::in_own_party(npid)` is false.
- `void leave_party()`: member only (active, not hosting). Online: `online::drop_out_of_party("left matchmaking")`
  does it (rooms left, `online_reopen` hosts). LAN: log `party: leaving the party (Leave Matchmaking): a party of
  our own`, detach every slot, close the transport (Bye), clear members, then host on the configured port
  (`UNCHARTED_U2MP_PORT`, else 27210) or, when that port is taken, on any port; hosting, net id 1, member 0 = us,
  party session published, sent mode -1, log `party: hosting a party of our own on UDP port %u`. Neither port:
  log, active = false.
- `void online_party_in(bool ok)`: NET_SM 12 -> mailbox 0x10e {ok} (log `party: JOINING gets 0x10e {%u}`), also
  while hosting (a join from the list). Then, not hosting and !ok: NET_SM 17 -> menu reset, PAGE_MULTIPLAYER,
  NET_SM 18 (log); the match session is cleared.
- `void online_match_abort()`: host only: log, match+0x99 = 0, match session cleared, netInfo+0x08 = 0, menu reset,
  PAGE_MULTIPLAYER, NET_SM 18.
- `std::vector<std::string> member_npids()`: host only: the NpIds of the used slots 1.. (transport address ->
  `online::npid_at`), unknown ones left out.

Changed behaviour:

- `on_follow`: `wait` = the follow has 9+ bytes and `online::follow_match_room(room)` returned true. JOINING is
  shown only when it is not already the top page (a join from the list shows it at the selection). NET_SM 12; with
  `wait` log `party: waiting in JOINING until the leader says the whole party is in` and no 0x10e (the leader's
  kOpPartyIn posts it through `online_party_in`); else 0x10e {1} as before.
- `online_reopen` also clears the net id -> NpId map (`npids`); the next roster / kIds fills it.
- Start Match "too few" (5.x): 1 in every game type for a custom game (owner rule 2026-11-25: the minimum of 2
  is for matchmaking only; custom games start solo in any game mode). Type 10 one-team box only with 2 or more
  players.

## Parity group E changes (2026-11-25, spec parity-e-flow2.md 7)

New session message types (older peers ignore them, 4.2 "12..0x0f ignored" no longer holds for 12..15):

| type | name | direction | layout after the type byte |
|---|---|---|---|
| 12 | ClockSync | client -> host / host -> client | `{0, u64 client steady us}` request; `{1, u64 echo, u32 host game clock}` reply |
| 13 | PartyUp | client -> host / host -> client | `{0, u64 party, u8 size}` pressed (PS3 0x11a); `{1, u64 party room}` join (0x119); `{2}` back (0x118) |
| 14 | Reserve | client -> host / host -> client | `{0, u64 room, u8 n, n x {u8 len, npid}}` request (0x100); `{1, u8 ok}` reply (0x101) |
| 15 | LeaderLeave | host -> clients | nothing: the custom-game leader leaves, leave with it (0x117) |

- **Silent peer (2.2).** `transport.on_timeout` logs
  `*************** %s has left : timed out (nothing heard for 20 s)` (the PS3 prefix since parity-i-connection.md 8;
  `?` for a slot without a member) with the member's name before the normal "left" path; `net::kTimeoutSeconds` is 20 s in every mode (owner decision).
- **Net clock (2.3).** Client, per frame (`clock_sync_frame`): while fewer than 8 samples and connected, a ClockSync
  request every 20..120 ms (LCG, steady clock in us). Host: answers at once with the echo and `own_ticks()`. Client:
  sample = round trip, offset = int32(host - own ticks) + half the round trip in ticks (300/s); after 8, the 4 with
  the smallest round trip are averaged and the game clock (`0x1116180+0x204`) moves by the rounded average; log
  `net clock: 8 samples, best 4 round trips %.1f..%.1f ms: clock moved by %+d ticks to the host's`. Restarts
  (`net clock: synchronising again (%s)`) when a host is joined, at match setup (EstablishConnection entered), after
  a coarse type 9 correction and when the transport closes. Type 9 (1 s, > 150 ticks) stays as the coarse fallback.
  Not done: PS3 then cleared a per-tracker timing field (PS3 tracker +0x22c); its PS4 offset is not known.
- **Slot manager (2.6), host of an online match room.** `slots_frame`: a new `online::match_room()` while hosting
  starts a table (capacity `online::mm_slots()`, else 10; log `slot manager: room %llx, %d slots`) and reserves the
  present players (our NpId + transport members) as one party. `reserve_party`: players without a reservation need
  free slots = capacity - reservations - present players without one (outside the request); expired reservations of
  absent players go first (`slot manager: Timeout User %s (party %u)`); all or nothing (`failed to reserve %d slot(s)
  for a party of %zu (%d free)`); a new party id per request (`adding user` / `updating user %s: party %u`), expiry
  now + 10 s; a request whose players all hold reservations only renews the hold. Reserve requests name the room
  (others ignored: `request for room %llx ignored`); the reply goes to the sender. `admit_peer`: a Hello from an NpId
  without a reservation is refused while no slot is free (`slot manager: %s refused: every slot is taken or
  reserved`, every 20th try). Member left: `removing user`. The client sends a request when it reaches the owner of a
  match room it joined (`online::take_led_party()` + our NpId); a refusal calls `online::reservation_refused()`.
  Not done: the party ids are not written to the trackers (see parity-e-flow2.md 7, 2.6).
- **Party Up (2.4).** `party_up_frame` (frame work): the wait popup is up when the top page is `0x471007a1` and
  PAGE_MULTIPLAYER_POSTGAME `0x6b7378ea` is in the page stack. First frame: log, give-up = now + 15 s, the press
  (party = `online::party_room_id()`, size `online::party_size()`; LAN: party 1, size = member count) goes to the
  host (or is noted there). Host: 4 s after the first press, parties in press order are merged while the group stays
  below 6 players (size = max(told size, pressed players)); a group of 2+ parties sends "join" with the first party's
  room to the players of the other parties; everyone else gets "back". Answer: join = `online::party_up_join(room)`;
  back = `online::mm_host_gone(.., false)` when a stranger hosts our matchmaking room; both then post mailbox 0x118
  with the u64 at match session +0x10 (POSTGAME's consumer: pop, NET_SM 19, PAGE_MULTIPLAYER_MATCHMAKING). 15 s
  without an answer: back. Log lines start with `party up:`.
- **Voluntary leave (2.5).** Leave state entered with the leave byte `0x104f1c0` set = voluntary. Host in a custom
  game (netInfo +0x2c4 == 2): LeaderLeave to all before kLobby. Client: LeaderLeave posts mailbox 0x117 with its own
  netInfo +0x60 u64 (Playing::Update's consumer) and marks "asked by the leader". A client's own voluntary leave
  (not asked): after the existing teardown, `host_own_party()` leads a party of its own (online link or UDP port,
  never UNCHARTED_U2MP_JOIN; log `party: left the party with the match (PS3 Leave::Enter): leading a party of our
  own`).
- **Players Met (2.9).** Playing entered (every match and round start): the names (+0x20) of the used, not excluded
  trackers other than the local one go to `progression_note_met`.
- **Version text (2.11).** PS3 draws the game data version ("1.09.078") at the top right of the multiplayer
  menus. Not drawn on PC (owner, 2026-11-27): the page handler no longer draws it.

## Parity group H changes (2026-11-25, spec parity-h-session.md)

New functions (online.h, called by online.cpp):

- `int custom_room_slots()`: the slot count of a custom game room (H1): `type_max_players` of the current net
  game's type (`*net-games*` record +0x0c of ngm +0x1ca0): 10 competitive, 3 co-op, 0 when no game is chosen or the
  type is unknown (online.cpp then makes no room and Start Match ends, H2).
- `int playlist_room_slots()`: the slot count of a matchmaking room (H1): the largest `type_max_players` over the
  chosen playlist's game types (`playlist_game_types`); 10 when there is none. Stand-in for the PS3 playlist maximum
  (`0x49a958`), which our playlist data does not carry.
- `uint32_t mm_pack_mask()` (H5, PS3 `0x4d6a08` + `0x49b858`): 0 when no playlist of ours is chosen or no net game of
  its game types has a map-pack map (`dlc_net_game_pack` 1 or 2); else `dlc_packs() & 0xff` ANDed with `packs` of
  every used member other than this PC (0xff = the member sent none: no narrowing). Only the host knows the members'
  packs (kInfo), and only the host searches / creates matchmaking rooms.
- `void online_network_error(const char *why)`: active only: log `party: network error (%s): leaving online play`,
  netInfo +0x1c = 1, host_lost (the transport closes on the next frame), host and members alike (H8, no world).

Changed:

- `void online_match_abort(const char *why)`: the log names the reason: `party: the custom game is dropped (%s):
  back to PAGE_MULTIPLAYER, NET_SM 18`; also used when the custom game room cannot be made (H2, owner decision:
  PS3 behaviour instead of playing without a room). The rest as before.
- `create_match_now`: `online::create_match_room()` true now also covers a room that cannot be made; online.cpp then
  calls `online_match_abort` (next frame) instead of `online_match_ready(0)`. `online_match_ready(0)` has no caller
  left.
- The slot manager's capacity (`online::mm_slots()`) is now the match room's real slot count for a custom game too
  (3 for a co-op room, H1).
