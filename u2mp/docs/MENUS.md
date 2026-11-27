# Uncharted 2 multiplayer menus: PS3 1.10 vs PS4 1.02, and what the PC port restores

Static analysis only (nothing was run). Addresses are PS4 1.02 image offsets (`boot-linked.bin`
file offset - `0x183ce8`) unless marked PS3. No game bytes are stored here. No earlier third-party
Uncharted 2 multiplayer notes or code were used. Code: `gow3_pc/src/u2mp/menus.cpp`
(`install_menus`, called after `install_remaining`), texts in `remaining.cpp` (TextDb::Find hook).

## 1. Method and the two facts that decide the work

1. **menu.bin.** Every multiplayer page of the PS4 `dc1/ps4/menu.bin` was compared, control by
   control, with the PS3 `NPUA80698` `build/main/dc1.psarc:menu.bin` (big-endian, 32-bit pointers:
   page record 0x44 bytes, `+0x20` -> `{u32 count, u32 base}`, control record 0x48 bytes with
   `+0x08` kind, `+0x0c` flags, `+0x14` label, `+0x1c` action, `+0x20` condition, `+0x38` column;
   the PS4 layout is in `u2mp/tools/u2_menu_pages.py`, whose scan skips pages with label-less
   rows, so read page records directly from their name pointers). Action numbers are one higher on
   PS4 (PS3 21 = PS4 22, "query"). **All multiplayer pages have the same controls, kinds, flags,
   columns, labels and condition numbers on both**, and the page records keep the same panel query
   (`+0x58..+0x5c` on PS4: action 22, condition 97..101/130/137, help-bar id). The only menu.bin
   difference that matters is the missing Multiplayer button in the main menus, which
   the menu overlay (`u2import --overlay`, overlay.cpp) already adds. The overlay also takes the Trophies mode
   button (BUTTON_NETTROPHIES) off PAGE_MULTIPLAYER (owner 2026-11-25: PC has no trophies; 3.1). Since overlay
   version 7 it also makes PAGE_LOBBY's five custom game option rows drop-downs (kind 6 -> 13): the data is the
   same as PS3, but the PS4 code draws kind 6 differently (3.2). Nothing else is patched in menu.bin.
   Since overlay version 6 (`spec/parity-n-remaining.md` N3) the overlay gives the three main menus new control
   lists with a new Multiplayer code button and leaves BUTTON_MULTIMEDALS as shipped, in the PS3 order:
   PAGE_NEWPROFILE New Game, Load Game, Multiplayer, Options, Multiplayer Medals; PAGE_NEWPROFILE_BETA Multiplayer,
   Beta Trailer, Options, Multiplayer Medals; PAGE_PROFILE as before (no medals there, as PS3). Before version 6 the
   medals button itself became Multiplayer, so PAGE_MULTIMEDALS had no route.
2. **Query table.** Query controls call `fn(query)` from the table at `0x105e370` (PS3
   `0xe15b50..0xe16168`, 12-byte entries `{cond, OPD, arg}`): 132 entries, the same conditions in the
   same order on both, ended by `fn == 0`, read on every call by the helpers `0x65f4e0..0x660660`
   (`call [entry+8]`, entry at `query+0x78`, arg at `query+0x48`; only while NET_SM > 1).
   Pairing PS3 and PS4 functions by condition and comparing size and callees (u2_func_match
   pairs) shows: 5 stubs (`ret`) besides the cond-64 one that is a `blr` on PS3 too; every other
   query keeps its code. The PS3 callees they lost are small accessors PS4 inlined (IsHost
   `0x4d2394`, GetVolatileData `0x4c4f74`, live-data getters `0x4a9c38..0x4a9d34`), the text
   formatter `0x40bb24` (now `0xcdde40`), or PSN/voice calls.

So the pages looked empty because of **data**, not layout: NetPlayerData never loaded (now
`progression.cpp`), the party unknown (now `session.cpp`), live data from Naughty Dog's server
(playlists, the status count) never downloaded, NetInit's online id never written, a few texts that
only PS3 title updates carried, and the 5 stubbed queries.

Query object: `+0` op (1 title text, 2 row text, 3 image, 4 get index, 5 last index, 7 enabled,
8 reset, 9 set index; 6 and others per query), `+4` value / row, `+0x08..+0x30` image descriptor
(op 3), `+0x40` text out, `+0x48` table arg, `+0x50` element id, `+0x54` column, `+0x80` the
page's help-bar id.

## 2. Query table audit (PS4 fn / PS3 fn, by condition)

State: **stub** = `ret`; **wrap** / **port** = changed by menus.cpp; otherwise intact.

| cond | control(s) | PS4 | PS3 | state / source of its data |
|---|---|---|---|---|
| 3 | PAGE_MULTIPLAYER_JOIN: BUTTON_MULTIPLAYER_SELECT_JOIN_GAME list | `659590` | `4b8ebc` | **stub -> port** (empty list; PC joins by address) |
| 5 | (no MP control) | `659560` | `4b7d10` | intact |
| 6..15 | PLAYER0..9 on MATCHMAKING, POSTGAME | `659bc0` | `4bfac8` | stub -> session.cpp `mm_roster_query` (the matchmaking slot list: own party, other players, open slots) |
| 16..35 | PARTY_PLAYER0..9 on PAGE_MULTIPLAYER / PAGE_LOBBY | `659bb0` | `4bc728` | stub -> session.cpp `roster_query` (host first, host's team, clan tags, op 7 / op 10) |
| 36 | PAGE_MULTIPLAYER_FRIENDS_LIST: LIST_FRIENDS | `65bbb0` | `4c1fe0` | **stub -> port** (online: RPCN friends in the PS3 order and layout plus the PC rows; LAN: the party) |
| 37 | PAGE_FRIENDACTIONS list | `65bbd0` | `4b8760` | intact (page unreachable on PC) |
| 38 | PAGE_FRIENDACTIONS header (k17) | `65bbc0` | `4c1a08` | **stub -> port** (friend card of the selected friend; the page is unreachable on PC, View Profile shows the lines) |
| 39 | LIST_PLAYERSMET | `65bcd0` | `4b8638` | **wrap** (rows from the progression store) |
| 40, 41 | PLAYER_OPTIONS: LIST_PREFERED_GOOD_GUY / BAD_GUY (character skins) | `6595a0` | `4c0ea8` | intact; NetPlayerData + DC skin tables |
| 44, 87, 88 | PAGE_MULTIPLAYER_SCOREBOARD header, k15 widget | `65b830`, `65cc80`, `65b490` | `4bf4a8`, `4bf218`, `4be434` | intact; NGM trackers, `*net-games*`; text 64370402 added |
| 45, 46 | (in-game) | `65b1c0`, `65b320` | `4bc600`, `4bacb4` | intact |
| 48 | BUTTON_MULTIPLAYER_INVITES | `659b50` | `4b8e08` | **port**: remaining.cpp `invites_query` (blank while no invite waits, column 1 = `online::invite_count()`); button callback remaining.cpp |
| 49 | status line under the party list (PAGE_MULTIPLAYER, PAGE_LOBBY) | `659a90` | `4bae1c` | **wrap**: count = party size (text 69cbee70 from the import) |
| 50 | BUTTON_MULTIPLAYER_STARTMATCH | `655c40` | `4b7d80` | intact (callback: session.cpp) |
| 51..53, 131, 136 | MATCHMAKING / CUSTOM / CINEMA / LEADERBOARD / NETTROPHIES labels | `655cf0` | `4b9a88` | intact; mode table `0xdef390` (texts 47a936dc, c21027d1 added); 136 is never asked: the overlay removes BUTTON_NETTROPHIES (DROPPED, owner) |
| 54 | (pause) | `655f90` | `4c1348` | intact |
| 55 | PAGE_LOBBY: PLAYLIST | `6562d0` | `4bd9c8` | **wrap**: playlists built, choice applied, picture as type 3 |
| 57..63, 65, 66 | PAGE_LOBBY custom game carousels and team | `656550`..`657900` | `4bb898`.. | intact; game type name `+0`, description `+4` (column 3), image `+8` of `f985c7b6` |
| 64 | (none) | `657ae0` | `4b712c` | stub on both |
| 67..76 | PAGE_CINEMA, cinema list, progress | `659410`.. | `4baef8`.. | intact (cinema.cpp) |
| 77, 78 | PLAYER_OPTIONS: LIST_BOOSTERS0/1 | `659bd0` | `4be788` | intact; NetPlayerData unlocks |
| 79..86, 105, 106 | results, round results, PLAYER_OPTIONS rank widget (k19), last game results | `65bd80`.. | `4baa4c`.. | intact; NetStatGame, NetPlayerData |
| 89, 90 | PAGE_MULTIPLAYER_MATCHMAKING status, vote list | `65a900`, `65ae50` | `4bcc68`, `4bf768` | intact; their data (phase, deadline, vote block `0x113db50`) now comes from the matchmaking port (frontend3.cpp, 3.6) |
| 92..94 | (in-game) | | | intact |
| 95, 96 | EULA, MOTD | `65e210`, `65e2e0` | `4b74f8`, `4ba660` | intact; pages never shown on PS4 |
| 97..101, 130, 137 | page panel queries (title, help bar from `0x1041560`) | `65ecc0` | `4bc310` | intact |
| 102..104 | controller layout, kick griefer, voice volume | | | intact (voice volume lost its cellVoice calls) |
| 107..110 | PLAYER_OPTIONS toggles | `65c610` | `4c0780` | intact |
| 111..127 | cinema render settings | `660760`.. | `4b7184`.. | intact |
| 128 | LEAVEGAME_CONFIRM | `65e3b0` | `4c15c8` | intact |
| 129 | BUTTON_POSTGAME_PARTYUP | `65e7f0` | `4b7c48` | intact |
| 133 | PAGE_MULTIPLAYER profile panel (k23) | `65e890` | `4bddd4` | **wrap**: online id written |
| 134 | PLAYER_OPTIONS: BUTTON_CLANTAG label | `6609e0` | `4bdc30` | intact; text 639e0fa8 added |
| 135, 138 | PLAYER_OPTIONS Matchmaking Default / DLC, (138) | `65a790`, `65a660` | `4c09a8` (135) | 135 **wrap**: shown when a map pack is imported (3.3); 138 intact |

## 3. Page by page

### 3.1 PAGE_MULTIPLAYER (handler `5eb1a0`, PS3 `0x4465c8`)

- **PS3:** mode buttons Matchmaking / Custom Game / Cinema / Rankings / Trophies with a description
  line; the profile panel (k23, cond 133): online id, rank badge from cash, character, kills, wins,
  money, co-op points; party leader and members (cond 16..25); a status line with a server count
  (cond 49); help bar (cond 98, table `0x1041560` row 1): Profile -> PAGE_MULTIPLAYER_PLAYER_OPTIONS,
  Party Up -> FRIENDS_LIST, Store -> PAGE_MULTIPLAYER_STORE, Results -> LASTGAMERESULTS.
- **PS4 as shipped:** same controls. Rankings / Trophies labels blank (texts 47a936dc and
  f1b9728c not in the PS4 tables, descriptions c21027d1 / 067e1f80 neither), panel zeros and no
  name (NetPlayerData state never 3, netInfo+0x298 never written), party rows `ret`, status line
  blank (text 69cbee70 absent, count from the dropped live data). The help bar row is intact (PS4
  calls the store "Rewards", text 90354b88).
- **Restored:** NetPlayerData (progression.cpp, on event 0 through `64e810`), party rows
  (session.cpp), panel name = `UNCHARTED_U2MP_NAME` (written to netInfo+0x298 before the panel
  query), status line "[A] daily players" (PS3 text, from the import) with the party size (netInfo+0x10 set around the
  call, then restored), the Leaderboards label and description.
- **Removed:** the Trophies mode button. The overlay gives the page a new control list without BUTTON_NETTROPHIES
  (the 5th of 22 controls), the other 21 in their order; the page's default control (Matchmaking, page record
  `+0x58`) and the mode dispatcher are untouched, the cursor moves over the remaining buttons. Mode 5 and
  PAGE_NET_TROPHIES are no longer reachable (DROPPED, owner 2026-11-25: PC has no trophies).
- **Store entry (owner report 2026-11-27):** the help bar's Store record (`0x10417a0`) uses text `90354b88` on PS3 and
  PS4 alike; the PS3 banks say "Store", the PS4 banks "Rewards". menus.cpp gives the record our text
  `sid("u2mp/help/store")` (texts.cpp: "Store" in every language, the PS3 words), together with its R1 token (3.8.1).

### 3.2 PAGE_LOBBY (handler `5ebac0`, PS3 `0x443bc8`)

- **PS3:** Start Match, playlist (matchmaking) or the custom game carousels (game type, map,
  variation, score / time limit, friendly fire, rounds, co-op difficulty), each with its name and
  description; party column; team; invites; status line.
- **PS4 as shipped:** carousels intact (they read `*net-games*` and the game types `f985c7b6`: name
  `+0`, description `+4` shown in column 3, image `+8`). The playlist row was empty: the playlists
  `ngm+0x20a0[]/+0x21a0` came from PS3 live data and nothing on PS4 fills them. Start Match was a
  `ret` (session.cpp).
- **Restored:** one playlist per game type in `*net-games*` (record `{id = game type image id,
  name, description, +0x28 game list NULL, +0x30 0, +0x34 skill board -1}`, the fields read by
  `6562d0`, `6400d0`, `640f00`, `64a460`, `64aac0`). The lobby's event 0 picks the first one when
  none is saved (`5ebb92`). When the leader's choice (NetPlayerData+0x104) changes, the lobby's net
  game becomes a game of that type (SetNetGame `63dc30`); Start Match then picks a random game of
  that type (session.cpp). The status line as in 3.1.

**Option boxes and the stray "<>" (owner report 2026-11-25, PS3 video `u2menu.mp4`).** On PS3, X on an option
row opens a box over the dimmed page: a list (Variation, Score Limit, Time Limit, Rounds, Co-op Difficulty,
Friendly Fire, Team) or, for the picture carousels (Game Type, Map, Playlist; 3.3: Boosters, Hero, Villain), the
row's label and value above a fan of pictures with the arrows at the box's edges; a focused, closed row shows no
arrows. On PC Variation .. Co-op Difficulty had no box and every focused carousel showed a small "<>" pair right of
(or above) its value. menu.bin is not the cause: PAGE_LOBBY and PAGE_MULTIPLAYER_PLAYER_OPTIONS have the same
kinds, flags, columns, conditions and `+0x68` (PS3 `+0x44`) on both games. Kinds (PS4 control construction switch
`0x5c5871`, jump table `0x5c7b30`, record `+0x10`):

| kind | rows | PS4 class (vtable; update, draw) | PS4 behaviour |
|---|---|---|---|
| 6 | Variation, Score Limit, Rounds, Time Limit, Co-op Difficulty | `0x57df50` list, `0x1007090`; `0x57e410`, `0x581e60` -> `0x57e7c0` | inline: left / right change the value at once, arrows (type 6) at value right + 11 / + 31 when editable; no box |
| 8 | Playlist, Game Type, Map, Boosters 0/1, Hero, Villain | `0x5881c0`, `0x1007220`; `0x588360`, `0x588ed0` | X / left / right open the box (`+0x24c`, fraction `+0x250` over 1/6 s), X commits, O reverts; box, fan (`0x58ab40`, `+0x254` = 5 per side) and arrows drawn while open |
| 13 | Friendly Fire, Team, Co-op Difficulty (profile), Taunt, Matchmaking | `0x586b80`, `0x10071b0` (a kind-7 list `0x1007120` inside, box rows = record `+0x68`) | drop-down box as PS3 |

PS4 kind 6 is Bluepoint's inline spinner of the Collection's options (their own rows BP_MENU_SPEEDRUN_TYPE, the
audio output page and Motion Blur are kind 6); Bluepoint changed the campaign's difficulty row (PAGE_GAMEPLAY, PS3
kind 6) to kind 13 for the box. Kind 8 still opens the PS3 box, but its draw adds an arrow pair for a focused
closed row (`0x58a0ab..0x58a68d`, value rect + (11 / 31, -100)): the stray "<>".
Fix: the overlay (version 7) makes the five kind-6 rows kind 13, the record otherwise unchanged (queries, flags 0,
`+0x64` 0, `+0x68` 5 rows as Team); menudraw.cpp skips the kind-8 closed-row arrows while NET_SM > 1 (the
campaign's chapter carousel, also kind 8, keeps them). Friendly Fire was kind 13 already. Not verified in the game.

**Hidden rows and row widths of kind 13 (owner report 2026-11-27).** After the change above, Custom Game showed a
blank line between Score Limit and Time Limit and its panel sat higher, and Find Match's panel moved up (the whole
lobby panel grows with the left column and is centred). The page lays out a column from its rows' sizes (virtual
+0x28, `{width, height}`): each row gets the column's x and width (the widest row) and its own height, and a row of
height 0 takes no space (`0x5a2110`). A row hidden by its condition has no label; kind 6 then measures 0 x 0
(`0x57f370`), kind 13 one empty text line (`0x587df0`, path `0x587e74`). So each hidden converted row (Rounds in
Deathmatch; in matchmaking all five) left a line. Second difference: the kind-13 draw (`0x587300`, `0x587359..0x58737e`)
replaces the laid-out width with the record's width (`+0x5c`, 320 on the Profile page), so Co-op Difficulty and Taunt
ended at 320 units while the column (and the Hero / Villain rows) is wider; on PS3 (video, 2:55) these rows reach the
column's right edge. Fix (menudraw.cpp `install_dropdown_rows`): the size of a row with no label is 0 x 0 for the
five converted rows (conditions 59, 60, 61, 63, 65; Friendly Fire and Team keep the shipped measure), and the draw
keeps the laid-out width for the Profile page's drop-downs (conditions 80 Co-op Difficulty, 82 Taunt, 108, 135
Matchmaking; the lobby's rows are unchanged). Not verified in the game.

### 3.3 PAGE_MULTIPLAYER_PLAYER_OPTIONS ("Profile"; page handler `5ed8b0` = remaining.cpp)

Rank widget (k19, cond 105: rank and progress to the next rank from cash), Boosters Slot 1/2
(cond 77/78), clan tag (cond 134), preferred hero / villain character (cond 40/41: the character
skin selection), co-op difficulty, taunt, matchmaking preference, toggles (108..110). All queries
intact; their data is NetPlayerData (now loaded and saved). Restored texts: NET_CLANTAG label
(d4143b18 "Clan Tag") and the empty-tag text (639e0fa8 "None"). The clan tag button explains it is
not available on PC (remaining.cpp). Boosters and Hero / Villain are kind-8 carousels, Co-op Difficulty, Taunt and
Matchmaking kind-13 drop-downs: no "<>" beside a focused closed carousel in multiplayer (3.2).

Boosters greyed out in a co-op custom game (owner report 2026-11-27): PS3 does the same. The booster list query (PS3
`0x4be788`, PS4 `0x659bd0`) answers op 7 (enabled) with 0 when the lobby's current net game is a co-op type (PS3
`0x4a0bcc`: game type 4, 8, 9 or 12; PS4 inlines the same test) and the page under the Profile is PAGE_LOBBY in
custom game mode (net info `+0x2c4` == 2; PS3 `+0x2b4`) or page `e35f0f1a` otherwise.
Owner decision (2026-11-27): the boosters stay selectable there anyway. menus.cpp wraps the query for cond 77 and 78
(`query_boosters`): op 7 always answers 1, every other op is the game's answer. This differs from PS3 on purpose.

Golden Guns (owner report 2026-11-27: missing under Other Settings; PS3 video 2:55 shows "Golden Guns On"). The
rows cond 108 / 109 / 110 share the query `0x65c610`; the table entry's argument is the row's bit (108: 0x10, the
Golden Guns label `b8a9d6ed`, values On `d1831446` / Off `32066ec9`). A row shows only when its bit is in the licence
word `[0x104f4b0 + 0x78]` (or the region bits the query adds); PS3 got 0x10 from the golden-weapons licence (net.bin's
licence table), PS4 never sets it, so the row had no label and stayed hidden. The setting is NetPlayerData `+0x13c`
(op 4 reads it, op 5 writes it; inside the saved career blob), and the PS4 spawn code still uses it (`0x663494`: when
it is 1 and the net game is not co-op, the game's weapon sets `8cf8e1cc` / `469fefc8` become the golden sets
`e58a9398` / `556dd82a`, which net-patch9 defines). menus.cpp wraps the cond 108 entry (`query_golden_guns`): the
query runs with bit 0x10 added and the word is put back after, so nothing else sees the bit. Not checked in the game.

Matchmaking (owner request 2026-11-27: a DLC option in Profile as PS3 has it). The gap between Taunt and Golden Guns is
this row, hidden. Cond 135, query `0x65a790` (PS3 `0x4c09a8`): label "Matchmaking" `ea2119f2`, values "Default"
`81af6b75` (0) / "DLC" `9811415d` (1, texts.cpp); the setting is NetPlayerData `+0x1b0` bit 0 (PS3 data block
`+0x16c`, inside the saved career blob; a new career has 0, Default). The query gives the label only while the
owned-DLC word `[0x1115a30 + 0xa0]` is not 0: PS3's map pack entitlements set it, PS4 nothing, so on PC the row never
showed (a PS3 without map packs shows the same gap, video 2:55). menus.cpp wraps the cond 135 entry
(`query_matchmaking`): the query runs with the imported map packs' bits (dlc.cpp `dlc_packs`, bits 1 / 2) added and
the word is put back after; no pack imported, no row. PS3 has this one row, not one row per map pack. What the value
does: DLC.md, "Matchmaking: Default / DLC". Not checked in the game.

### 3.4 Store ("Rewards"): PAGE_MULTIPLAYER_STORE, PAGE_MULTIPLAYER_STORE_ITEMS

LIST_MP_STORE_SECTIONS (action 25) and LIST_STORE_ITEMS (action 26) are list classes, not query
conditions. With NET_SM > 1 the store shows the multiplayer item table `0x92c6db02` of net.bin (Hero
Skins, Villain Skins, Boosters Slot 1 / 2, Co-op Weapon Upgrades, Taunts), else the single-player Rewards
`0x855a9c58` (store.bin); columns: name (`0x5aef40`), description (`0x5af1a0`), LOCKED / UNLOCKED from
the item state `0xa4c390`, picture `0x5b0070`. Every multiplayer item is reported unlocked (IsLocked
`0xa4bb40` and the co-op upgrade level `0xa4d280` ported, unlocks.cpp). Skin and booster rows were blank
because the PS4 net.bin skin / booster tables are 2-record stubs; the GOTY tables from net-patch9 are bound
over them (unlocks.cpp). Details: `UNLOCKS.md`. The PSN store button (sceNpCommerce2) explains it is not
available (remaining.cpp).

### 3.5 PAGE_MULTIPLAYER_FRIENDS_LIST ("Party Up") and PAGE_FRIENDACTIONS

- **PS3:** LIST_FRIENDS = PSN friends (label "Friends" from menu.bin; `ba8db526` "[A] players in party" is a friend's detail line), rows name + presence; selecting
  one opened PAGE_FRIENDACTIONS (friend card cond 38 + actions). LIST_PLAYERSMET = NP "players met".
- **PS4 as shipped:** LIST_FRIENDS query and callback `ret`; players met intact but its source
  (`0x113b668`, PS3 fetch `0x48f79c`) never filled.
- **Online (PORT-ONLINE.md 5a):** the RPCN friends with Join Party / Invite To Party.
- **Restored (LAN):** LIST_FRIENDS lists the party (leader marked "Party Leader"), title "N players in
  party"; selecting a row opens the player popup (remaining.cpp friends_row, same row order).
  LIST_PLAYERSMET lists the players of recent matches from the local progression store, most recent
  first. PAGE_FRIENDACTIONS (cond 37/38) stays as is: nothing on PC opens it.

### 3.6 PAGE_MULTIPLAYER_MATCHMAKING, JOIN / JOINING, LOBBY start

- **PS3:** Find Match -> "Searching for players..." (status cond 89, tips line), the players found
  (PLAYER0..9, cond 6..15), then the vote screen: two games (map name, map picture, game type, vote
  counts; LIST_MATCHMAKING_VOTE cond 90), "Voting ends in [A]...", the winner, "Game starting in
  [A]..." and the match. After it, back to this page and the next vote.
- **PS4 as shipped:** the widgets and their queries are intact; the page handler (the matchmaking state
  machine `64e910`, PS3 `0x4b0a58`) is a `ret`, the vote was never broadcast, nothing filled the vote
  block. session.cpp started matchmaking games straight from the lobby.
- **Restored (frontend3.cpp, PORT-FRONTEND3.md):** Start Match in matchmaking mode runs the PS3 flow on
  the party: gather (3..10 s, until every member's page is up), vote between two games of the chosen
  playlist (15 s), result, 10 s countdown, then the custom-game create path takes the party into the
  match. After the match Leave::Enter returns everyone to this page for the next vote. Back asks "Leave
  Matchmaking?" and returns the party to the lobby.

The join list (cond 3) is answered as an empty list; PC joins by address. PAGE_MULTIPLAYER_LEAVING_GAME
now closes (session.cpp sets match+0xc8 while it is up; PORT-FRONTEND3.md section 6).

Party and player rows (cond 16..35, 6..15): as PS3 `0x4bc728` / `0x4bfac8` (spec `parity-b-menus.md` 1, 2,
12): "[TAG] name", rank badge (type 0x27) and voice icon; party rows host first, then the host's team; the
own row not enabled (op 7 = 0), every listed row selectable (op 10); matchmaking rows show the slot list (own
party, "Found Player" / "Finding Player..." while gathering online, connection bars, party colours).

### 3.7 Post-game: POSTGAME, RESULTS, ROUND_RESULTS, SCOREBOARD, LASTGAMERESULTS

Player lists are action-23 lists with a column set (`c441623e`, `dcae45f4`, `2eb65ce8`,
`519c67c`: icon, money-rank, player, co-op points, kills, deaths, ... in menu.bin), filled from the
NetGameManager trackers and NetStatGame, which the session layer fills. Header and widget queries
(81..88, 106) are intact. Restored: the scoreboard countdown text 64370402 ("Starting in [A]"),
absent from both text tables. POSTGAME's PLAYER0..9 rows: session.cpp.
In the match, scoreboard.cpp draws SCOREBOARD / ROUND_RESULTS / RESULTS as the four tracker overlay entries with
the PS3 fade, priority and slide; the scoreboard also opens by itself while the local player is dead, so its last
row (cond 44, `65b830`: "Respawn in [A]..." / "Respawning" / "[A] Tries Remaining" / "Last Try") is seen as on PS3,
and the hint line e2186a0c is drawn under it (`spec/gameplay.md` 1, `spec/parity-m-features.md` 9).

PAGE_MULTIPLAYER's stats ticker (k24): ticker.cpp hands online.cpp's line to the game's add function `5d6500`
every 10 s while the strip is drawn and the previous line has come in (`spec/parity-ticker-respawn.md` 1).

### 3.8 Leaderboards (PAGE_MENU_LEADERBOARD) and Trophies (PAGE_NET_TROPHIES, DROPPED)

Leaderboard rows: progression.cpp (local boards, RPCN boards when linked). Leaderboard texts: 3.1 and section 4.

The page (one LIST_LEADERBOARD control, kind 10) drew **nothing** but its help bar until 2026-11-25: PS4 builds the
list object without its board / page / column fields and nothing on PS4 sets the leaderboard's reset request, so the
list update never fetched and the column set stayed 0, and the kind-10 draw stops when a list has 0 columns.
The rule is PS3's (spec/progression.md 1.5.19, settled from PS3 1.10 on 2026-11-25): the leaderboard is set back to
board 0, Global, "my page" with its reset request once each time Multiplayer is entered (PS3 NetInit leaderboard
init `0x4ab854`; ours: `progression_multiplayer_entered()` from the Multiplayer button); later visits keep board,
filter and page; a list object that never fetched starts over by itself. (A first version set the reset on every
page shown through a page-registry handler; it is replaced.) The table is then as on PS3 ("Competitive Kills -
Global", Rank / Level / Players / Kills / Deaths / Ratio, 11 stripes, empty when no scores; Money: Rank / Level /
Players / Money / Time Played; Wins: ... Wins / Losses / Ratio; Co-op: ... Points / Time Played). Controls: the two
shoulder buttons of 3.8.1 previous / next board, triangle Global / Friends, square first page, the D-pad pages (11
rows a page).

#### 3.8.1 Help bar shoulder icons (Prev / Next)

PS3 draws "△ Filter  [L1] Prev  [R1] Next  □ Top of List" (owner's video `u2menu.mp4`, 106-113 s; the Multiplayer
page also shows "[R1] Store"). Ours drew no picture before Prev / Next. Cause and fix (PS4 1.02 facts; addresses are
image offsets, file offset = address + 0x4000):

- Help-bar entries are 0x18-byte records in the read-only data segment (`0xfe8000..0x104c000`, read-only once the
  loader is done): u64 +0 token string (an R_X86_64_RELATIVE pointer, 0 in the file), u32 +8 menu action (8 square,
  9 triangle, 0xa = next side, 0xb = previous side, 0xc select), u32 +0xc text id, u32 +0x10 menu callback id,
  u8 +0x14 shown. The page panel query `0x65ecc0` picks a row of six record pointers from `0x1041560` and builds
  each entry as token + text ("%s%s", `0x65ef20..0x65f148`, buffer `0x14cd510`) for the text printer.
- Leaderboard row (`0x1041680`): Filter `0x1041728` ("|T| ", 9, `d82785e7`, callback `cacfc66d`), Prev `0x1041710`
  ("|L1-NOSWAP| ", 0xb, `5ba2fdfb`, `d1683525`), Next `0x10416f8` ("|R1-NOSWAP| ", 0xa, `8994a031`, `035e68ef`),
  Top of List `0x10416e0` ("|S| ", 8, `320a6fc8`, `61f5718d`). Two more records use the same tokens:
  `0x1041758` (0xb, text `1842529c`) and `0x10417a0` (0xa, text `90354b88`, the Multiplayer page's Store entry).
  Token strings: `0xe8956e` "|R1-NOSWAP| " (`7c 52 31 2d 4e 4f 53 57 41 50 7c 20 00`), `0xe8957b` "|L1-NOSWAP| "
  (`7c 4c 31 2d 4e 4f 53 57 41 50 7c 20 00`). Record bytes +8..+0x17: `0x10416f8` `0a 00 00 00 31 a0 94 89 ef 68 5e
  03 01 00 00 00`, `0x1041710` `0b 00 00 00 fb fd a2 5b 25 35 68 d1 01 00 00 00`, `0x1041758` `0b 00 00 00 9c 52 42
  18 e2 70 29 6b 01 00 00 00`, `0x10417a0` `0a 00 00 00 88 4b 35 90 72 cc 0e 13 01 00 00 00`.
- Why no picture: the PS4 text printer (`0xccb270..`) turns `|NAME|` into the string id of NAME (CRC-32
  0x04c11db7, MSB first, start 0) and looks it up in a compare chain. L1 (`0x5ed97e90`, `0xccbe52`: `81 fa 90 7e d9
  5e`), R1 (`0x21161977`, `0xccb9e0`: `81 fa 77 19 16 21`), L2 (`0x539a5849`), R2 (`0x2c553fae`) are there;
  L1-NOSWAP (`0x622f490f`) and R1-NOSWAP (`0xd1a997dd`) are not. An unknown token gives icon code 0 and is skipped
  (`0xccc165`), so only the trailing space is drawn. PS4 shipped without multiplayer and dropped the two tokens;
  the menu data kept them. The picture itself is present: no asset is missing.
- The pictures: the button icon font `symbol3.fnt` (the same file in the U1 / U2 / U3 PS4 games; ids and atlas cells
  in `gow3_pc/gpu/shim/gow3_prompts.cpp`): L1 = 8 (large text 42), R1 = 6 (40), L2 = 9 (43), R2 = 7 (41). The PS4
  printer starts from these codes (`0xccb305..0xccb342`: `ba 06`, `ba 07`, `be 08`, `bf 09` with the large set
  0x28..0x2b) and lets the PS4 shoulder setting u32 `[0x1059640 + 0x4888]` choose: `|L1|` draws the L2 picture
  while it is 0 and the L1 picture otherwise, `|R1|` the R2 / R1 picture (`0xccbe86`: `83 b8 88 48 00 00 00`).
- The buttons: the PS4 menu input mapper (`0x5d0c7b..0x5d0d0f`) gives action 0xa to R1 and 0xb to L1 while
  `[0x1059640 + 0x4888]` != 0 (`0x5d0c82`: `8b 86 88 48 00 00`), and to R2 / L2 while it is 0; u32
  `[0x1059640 + 0x4870]` != 0 exchanges 0xa and 0xb (`0x5d0c96`: `83 be 70 48 00 00 00`). So `|L1|` / `|R1|` always
  show the button that runs 0xb / 0xa, except that they ignore the `+0x4870` exchange.
- PS3 (1.10, printer `0x73ca94..`, token names at `0xdc4ec4..`): "L1-NOSWAP" is always code 8 (L1) and "R1-NOSWAP"
  always 6 (R1); plain "L1" / "R1" give 8 / 6 or 6 / 8 by a PS3 swap word (+0x21bc of the object in r15), "L2" / "R2"
  9 / 7 or 7 / 9 by +0x21c0. NOSWAP = "the picture of the button this entry's action is on, whatever the swap
  setting": the same codes as the PS4 font.
- The PS4 printer has the opposite tokens too (`0xccbab7` R2 `0x2c553fae`, `0xccbdbc` L2 `0x539a5849`): `|R2|` draws
  the R1 picture while the setting is 0 and the R2 picture otherwise, `|L2|` the L1 / L2 picture
  (`0xccbaeb` / `0xccbf01`: the same `+0x4888` test with the two codes the other way round).

Fix (runtime, menus.cpp; no import step, no new asset, no PS3 asset):

1. At install: check the four records' bytes +8..+0x17 and the two token strings above. If any differ, log
   `menus: help bar shoulder records differ, Prev / Next icons left alone` and do nothing more.
2. Keep four strings of our own: "|L1| ", "|R1| ", "|L2| ", "|R2| ".
3. Every frame (from the per-frame page handler session.cpp already registers for every page): swapped = u32
   `[0x1059640 + 0x4888]` == 0. For each of the four records: wanted = the token that draws the NOSWAP picture under
   the current setting: action 0xb "|L1| " (not swapped) or "|L2| " (swapped), action 0xa "|R1| " or "|R2| ". The
   `+0x4870` exchange word is not read (PS3's NOSWAP ignores the swap setting). When the u64 at record +0 is not the
   address of wanted, write it (8 bytes; the segment is read-only: make it writable for the write and put it back,
   as `netinit.cpp` `write_code` does with `HostCodeWrite`) and log `menus: help bar shoulder icons L1 / R1 (shoulder
   setting %u: tokens %s)` (`|L1| / |R1|` or `|L2| / |R2|`) once per change of the setting.
   If a record cannot be made writable: log `menus: help bar record 0x%llx cannot be made writable (%lu), Prev /
   Next icons left alone` and stop the fix-up for the session.

Built 2026-11-25, changed 2026-11-27 (owner: PS3 always shows L1 / R1, also with the L2 / R2 setting; the first
version followed the setting and showed L2 / R2 with it at 0, the owner's `menus: shoulder setting 0, exchange 0`).
`menus.cpp` `menus_help_bar_frame()`, called first in `session.cpp` `page_handler` after the version text; old-vs-new
harness `menus`: scenarios HB-1 and HB-2. Not yet checked in the game.

Result: "[L1] Prev [R1] Next" and "[R1] Store" / "[R1] Rewards" as PS3, whatever the PS4 shoulder setting. The PC
prompt shim redraws codes 6-9 / 40-43 for keyboard and Xbox pads like every other button icon (code 6 / 8: the key
bound to PS4 R1 / L1).

Not covered: the PS4-only string "|R1_NOSWAP| %s" (`0xe8721f`, used once at `0x5ae6a0`) is equally undrawn.

Tests: (a) Multiplayer > Leaderboards: "△ Filter [L1] Prev [R1] Next □ Top of List" with the shoulder setting on and
off; press L1 / R1: the board changes (Next, callback `0x64e120`: Kills -> Money -> Wins -> Co-op -> Kills; Prev,
`0x64e0f0`: the other way). (b) Multiplayer, Find Match and Custom Game pages: the Store / Rewards entry shows R1.
(c) Keyboard / Xbox pad: the two pictures carry the bound key / Xbox labels. (d) Change the controller option in the
game's options and come back: the pictures stay L1 / R1 (one `menus: help bar shoulder icons` log).
Leaderboard outline (owner report 2026-11-27: a thick white frame around the table; PS3 has none). The table control
(kind 10, draw `0x58c7a0`) asks its list for property `0xffce262d` (virtual +0x60) and, while the answer is 0, draws
an opaque white 20-unit outline around the table and its title tab (`0x58cc72..0x58d181`, polygon `0x5b5de0`). The
leaderboard list's getter answers 0 for everything on PS4 (`0x5b3990`); on PS3 (`0x4116a4`, vtable `0xd70098` slot
10) it answers 1 for `0xffce262d` and for `0x348751f2`, and for `0x6a9b4a0c` a row test. menudraw.cpp
(`install_leaderboard_outline`) puts a getter in the slot (`0x100a360 + 0x60`) that answers 1 for `0xffce262d` only.
Not taken over: `0x348751f2` (the control's focus type then becomes 4 instead of the record flag, which changes how the
list takes the D-pad) and `0x6a9b4a0c` (a per-row test); both stay as PS4 until the owner wants them.

PAGE_NET_TROPHIES: DROPPED (owner 2026-11-25: PC has no trophies). Its only route, BUTTON_NETTROPHIES, is removed by
the overlay (3.1); the page stays in menu.bin unused and its title MENU_TITLE_TROPHIES (85ed01cc) and the button label
f1b9728c are no longer served. Its list LIST_STATS stays: it is the game's own code (it reads NetPlayerData), and
PAGE_STATS has a LIST_STATS control too.

### 3.9 Cinema pages

Unchanged here; see `CINEMA.md`.

## 4. Texts (TextDb::Find hook, texts.cpp; import texts.cpp; details in TEXTS.md)

The multiplayer code and menu.bin use 26 text ids that no PS4 text bank has (the PS3 game got them from
title updates and its server; they are in the BCUS dlc-patch7 networking bank, which is not a source):

| ids | used by | text (ours, English) |
|---|---|---|
| f67d9ad2, 70c81d2e, c0cd1b00 | profile panel k23 (`59ddb0`) stat labels; also the board titles (`0xdef0a0`) and c0cd1b00 the money column | Competitive Wins / Competitive Kills / Money (PS3; was "Cash" until 2026-11-25) |
| 47a936dc, c21027d1 | mode 4 label / mode 4 and 5 description (`0xdef390`) | Leaderboards (as PS3; English replaces the GOTY base bank's "Rankings") / View your stats |
| 780b62e0, e663a74e, b9602dbb, 966fa9ae, 4915c4b7, 7d24a9f9 | stats ticker k24 (`5b3240`) | Time Played, Global, Wins, Losses, Ratio, Rank |
| 320a6fc8, 8994a031, 5ba2fdfb, d82785e7 | help bar table `0x1041560` (leaderboard footer) | Top of List, Next, Prev, Filter (PS3 wording; were Go to Top / Previous) |
| 69cbee70 | status line, cond 49 | [A] players today |
| d4143b18, 639e0fa8 | NET_CLANTAG label, clan tag query `6609e0` | Clan Tag / Enter Clan Tag... |
| 64370402, 9b0dd3c2, e012d5e3 | scoreboard countdown `65cc80`, leave-game confirm `65e750` | Quit Penalty [A]... / If you leave now, you will get a [TEXT] second wait penalty ... |
| 9811415d, b7e89ee7, 6cff3670, c4c378d1, bde6ab5b | DLC notes and messages | DLC / Requires the ... Pack / You didn't finish your last game ... |

(`067e1f80`, which earlier versions served as a text, is the Trophies **icon** id of the mode table.) Not served any
more (owner 2026-11-25, the Trophies button is removed, 3.1): f1b9728c (mode 5 label "Trophies") and 85ed01cc
(MENU_TITLE_TROPHIES, the PAGE_NET_TROPHIES title).

47a936dc comes from the GOTY base bank (the import merges every GOTY archive), except in English, where the base bank
says "Rankings" and PS3 (title update) shows "Leaderboards": texts.cpp serves its own English text first for it
(`kOwnFirstInEnglish`), and greek / czech / turkish (no GOTY bank) get ours too. The other 25 are our own texts in
all 22 languages (texts.cpp `kOwn`), with the other ids no bank has: 041a85ca (DLC pack bought), 63801125
(Guardians), the three DLC map titles, the Siege description, net-patch9's game type variants, playlists and 48
skin names, the five server playlist texts of `kPlaylistDefs` (were `extra_text` in menus.cpp) and the PC message
boxes. The import writes `text1/ps4/<lang>.networking` and `.common` (u2text.complete: PS4 bank + GOTY of the same
language + sam-spanish <- spanish, brazilian <- portuguese for their `_` placeholders; English always). The game
reads its banks from `core.psarc` (loader `0xcdd960`), so it does not open those files: the TextDb::Find hook
answers a miss or a `_` from them, then from our texts, then English, and logs every id it can't answer once
per language ("texts: no text for ...", "texts: ... shown in English").

## 5. Gaps

- "[A] daily players" (69cbee70) was a count from Naughty Dog's server; the party size is shown with it.
- PAGE_FRIENDACTIONS, the MOTD/EULA pages and the PSN friend card (cond 38) are not restored (no PC
  counterpart). The matchmaking machine `64e910` is restored on the party (frontend3.cpp); the PS3
  5 s "Starting in [A]..." popup before it is ported too (frontend6.cpp, PORT-FRONTEND6.md 1).
- `dd2d35e7` (a net event's text, `62f250`) is absent from both tables; not a menu text, left.
- In-match overlay (2026-11-25): result pages come from the NGM match states, the menu background is dimmed behind
  them and no page is drawn while a menu is up (`spec/parity-m-features.md` 9, 10). Left open: the film-only parts
  and the two PS3 toggle blocks (open questions 12, 13, 15).
- Not verified in the running game (static analysis only): the k23 panel and k19 rank widget
  drawing (`59ddb0`, `59bf50`) and the action-23 list classes are intact by size and callees, and
  get their data from NetPlayerData / NetStatGame / the trackers.
- Playlist descriptions are the game types' descriptions, not the PS3 server playlists (Deathmatch,
  Objective, Co-op Arena, ...): those were server data.
- Multiplayer Medals (N3, `spec/parity-n-remaining.md` 4.3): not looked at in the game yet. Whether PS4 condition 9
  shows the button on a fresh profile and whether LIST_MEDALS (condition 1) lists the multiplayer medals is a CHECK.

## 6. Page by page against the PS3 game (2026-11-08)

Reference: RPCS3 screenshots of PS3 1.10. "Before": the PC build tested on 7 November. Not run here
(static analysis); every "now" needs a check in game.

Three causes cover most of the gaps:

1. **Texts** (section 4): every "UNTRANSLATED STRING!!!" and blank label.
2. **The net image table** `0x1130008+0x70`. Every type 3 image of the MP menus (mode buttons
   `0x655f11`, game type `0x656864`, map card `0x656ac8`, boosters `0x65a0e0`) is looked up there
   (`0x5558c0`; selector 1 = net, 0 = hud `+0x68`, 2 = menu `+0x78`). `0x554090` (every frame, from
   Draw `0x3fc37a`) sets it from the package `"net"` (sid `4240ef2e`) only while that package is logged in
   (`+0x30 == 7`), else NULL. Without an entry the image gets size 0 (`0x5bcc1a` -> `0x5bbd90`), and the
   rich rows (kind 8) of PAGE_LOBBY take their height from it: Game Type / Map / Variation collapse onto
   one line. The game's own request (`0x5ed839`, PAGE_MULTIPLAYER_CONNECTING) sets package flag `0x40`
   right after RequestPackage (as the hud request `0x4b8c7f` does); session.cpp requested without it.
   Now: the flag is set, the package is requested again while the MP menus are up (states 18 / 3 / 12),
   and the log says `net package: present, state N, flags 0x..; net image table set|NULL, N images`
   whenever that changes. If the table stays NULL with state 7, the cause is elsewhere (next round).
3. **Image types PS4 cannot draw.** The menu image renderer (`0x5bd852`, types 4..0x29) has no case for
   `0x1e` (booster pair of the profile panel), `0x22..0x25` (0x25 = playlist picture, 0x22 = cinema list)
   and `0x27` (rank badge: profile panel, k19 widget, PS3 party rows): they go to its default
   (`0x5c1ab1`, draw nothing). The queries are intact; the drawing code went with the PS4 multiplayer.
   The PS4 net.pak has the materials (`general/net/rank-badges:default`, `general/net/boosters:*`,
   `map-photos:*`, `playlists:*`). Now ported (section 7); 0x23 is the MOTD picture, not needed.

| page (reference) | PS3 | before | now |
|---|---|---|---|
| PAGE_MULTIPLAYER (6) | mode icons, labels Leaderboards / Trophies (14%), panel: name, Competitive Wins / Kills / Money / Co-op, rank badge 28, booster icons; party leader with badge; "0 daily players"; ticker "Time Played: name: 10h 5m"; footer Profile / Party Up / R1 Store | no icons, stat labels UNTRANSLATED, no badge, "1 players in party", empty ticker | labels, Leaderboards, no Trophies button (removed by the overlay, owner 2026-11-25), ticker and status texts from the PS3 bank; "N daily players" = party size; mode icons for Matchmaking / Custom Game / Cinema once the net table is set (the PS4 net.pak has no Leaderboards / Trophies button images: `mode-buttons:` cinema, customgame, matchmaking only). Rank badge and booster pair: cause 3 |
| PAGE_LOBBY, matchmaking (7) | Find Match, playlist "Deathmatch" with picture card | playlist "Machinima", empty picture box | Machinima (the cinema game type, text 5614c769) is no playlist; Deathmatch first; picture: type 0x25 replaced by the type 3 net image `general/net/playlists/<deathmatch, objective, coop, survival, default>` chosen by game type (the PS4 net.pak has these five) |
| PAGE_LOBBY, custom game (8, 14-18) | Game Type with mode icon, Map with card + name, Variation, Score / Time Limit, Friendly Fire; party leader with badge; Team | rows on one line ("Variation/Game Type"), party leader and members empty | party rows: cond 26..35 have table args 10..19 and the roster query read them as rows 10..19 (empty); now row = arg % 10 as PS3 `0x4bc780` (and the PARTY_PLAYERn callback likewise). Rows / icons / map cards: cause 2. DLC map cards are not in the PS4 net.pak (IMPORT.md). Leader badge: cause 3 |
| PAGE_MULTIPLAYER_PLAYER_OPTIONS, Profile (9-11) | rank ring and money, Boosters Slot 1/2 with names and icons, Clan Tag, Hero / Villain portraits, Co-op Difficulty, Taunt, Matchmaking, Golden Guns; booster carousel with descriptions | empty, footer UNTRANSLATED | Clan Tag label and "Enter Clan Tag..."; footer texts; booster and portrait images are type 3 (cause 2). Rank badge in the k19 widget (type 0x27): cause 3. Why the whole page was empty is not explained by the code (all its queries are intact): check with the new build |
| PAGE_MENU_LEADERBOARD (12) | Rank, Level badge, Players, Kills, Deaths, Ratio; footer Filter / Prev / Next / Top of List | footer UNTRANSLATED; no table at all (column set never written, 3.8) | footer texts (PS3 wording); the table (PS3 start-over at Multiplayer entry + fresh-list rule, 3.8); footer L1 / R1 pictures (3.8.1, built 2026-11-25, not yet checked in the game); rows from progression.cpp; level badge = type 0x27 (cause 3) |
| PAGE_NET_TROPHIES (13) | list with icons, progress bars, counts, description | title / label only | DROPPED (owner 2026-11-25: PC has no trophies): no button leads there (3.1, 3.8) |
| PAGE_CINEMA (19) | Local Files, map picture, Author / Players / Length / Size / Date | not reported | texts; the list picture uses type 0x22 (cause 3) |

Check in game (log `u2mp`): `texts: N strings from 2 imported bank(s) for language 2` (N about 2,700),
the `net package:` lines (state 7 and "set, N images" on the Multiplayer page), mode icons, custom game
rows on separate lines with icon and map card, party leader on the custom game page, the playlist card.

## 7. Why no net image drew, and the lost image types (menuimages.cpp, 2026-11-07)

Run 13 had the net table set (251 images) and still no menu image. Cause: **id naming**. The net
table is the `net` package's material table (package+0x940; entry 0x28 bytes, `+8` id, name via
`+0 -> +0x10 -> [0] -> [0]`). ConstructFromBoundMaterialTable (`0xbe81f0`) sets each id to the string
id (`[0x10666c8]` = `0xb26fc0`, the CRC sid) of the material name up to `,`. PS4 names them
`general/net/<set>:<name>`; the menu data and code ask for the PS3 ids `sid("general/net/<set>/<name>")`
(mode table `0xdef390+8`, map records `+0xc`, booster records `+8`, the playlists). No id matched, the
lookup returned NULL, the image size stayed 0 (`0x5bcc1a`), nothing drew and rich rows collapsed.
Fix: `0x5558c0` is ported whole; an id not in the table is looked up again among the names with `:`
read as `/` (cached per table). The queries that search inline (`0x655ec0`, `0x656a22`) get -1 and
then go through that lookup. Other checks of the type 3 path, all fine: selector 1 fades in over 300
frames from the package's login frame (`+0x924`, `0x5bca18`); size = the largest texture of
technique 0 (`0x5bbd90`).

Renderer `0x5bcd70`: jump table `0x5c2ec4` (types 4..0x29) entries for 0x1e, 0x22..0x25, 0x27 now
point at a stub (r11 descriptor, `[rbp-0x1fe0]` draw list, `[rbp-0x1ff0]` x, y, w, h, colour bytes in
ebx/r14d/r12d/r13d) that calls C++ and jumps to the exit `0x5c1ab1`. Quads are drawn with
`0xbdce90(ctx {list, 0...}, {material, 0, 0, 0}, 4 vertices of 0x40: x y z u v, colour at +0x20)`
as the type 3 case does. Descriptor value = `+0x10` (query `+0x18`). From PS3 `0x41a790`:

| type | PS3 case | drawn |
|---|---|---|
| 0x27 rank badge | `0x41aa48` | `general/net/rank-badges/default` (index > 63: `.../default2`, absent in both games), cell `index & 7`, `(index & 63) / 8` of 1/8 |
| 0x1e booster pair | `0x41c2d8` | low / high 16 bits = boosters (table `916e03b0`, rec `+8` image, ids 05454b23 / 01845694 none), each w x w/2, centred at 28 % / 72 % height |
| 0x22 map picture | `0x41eb50` | map table `0dec97a4` (0x38 bytes, `+0` id, `+0xc` image), u 0.0898..0.9102 |
| 0x24, 0x25 playlist | `0x41f418` | ngm playlist with that id, image `+0x10` (menus.cpp sets it), u 0.0703..0.9297 |
| 0x23 | `0x41cd34` | not needed: its only PS3 producer is the MOTD query (cond 96, `0x4ba660`, at `0x4ba760`), whose server page PS4 / PC never shows; the PS4 MOTD query `65e2e0` no longer produces it, and no other PS4 query sets 0x23 |

The 0x25 bottom-edge adjustment is ported (50 units, PS3 `0x41f4f4`). The PS3 overlay drawn when
descriptor `+0x20` is non-zero is not needed: no PS4 producer sets it (PORT-FRONTEND6.md 4).

Profile page empty: all its controls are k0 headers, k19 (cond 105, rank badge 0x27), k8 rich rows
(boosters, hero / villain portraits: type 3) and k13 settings; the queries are intact and the page
handler matches PS3 `0x4443f8` (clan tag keyboard, pop when not connected). Not explained statically:
if the page stays empty with the id fix, the log tells whether images are looked up / drawn on it.

Log (`u2mp: menu images:`): the first 6 entries of each net / menu table (id, PS3 id, name), the first
48 id lookups that miss the table (`found by PS3 name` or `missing`), the first 24 draws of the ported
types (type, value, image id, found, box). Check in game: mode icons, game type / map cards and
separate rows on the custom game page, playlist card, rank badge 28 and the booster pair on the
Multiplayer panel, Profile page contents, store skin pictures.

## 8. Menu images PS4 dropped: net-u2mp.pak (import netimg.cpp, 2026-11-07)

Run 15 still missed two ids on the Multiplayer page: `e2c34a4a` = `general/net/mode-buttons/leaderboards`
and `067e1f80` = `.../trophies`. The PS4 net.pak has no such materials. Compared material by material
(PS3 id = sid of the `/` name, PS4 id = sid of the name with `:` read as `/`), the newest PS3 net.pak
(dlc-patch7) has 104 materials the PS4 one lacks: those two buttons, the DLC map photos (highrise,
museum, sts, sge, wtr), rank-badges/default2 (ranks above 63), the-lab playlist, medal icons, and the
player photos / icons of the PS3-only skins (store rows without a picture).

The import converts only those (the rule: what PS4 has stays the PS4 one) into
`pak22/ps4/net-u2mp.pak` with the Uncharted 3 net converter (dctools `ps4mats` convert_net), with three U2
differences in `u2mp/patcher/importer/netimg.cpp`: the U2 PS3 MATERIAL_TABLE_1 (two tables, texture hashes in a shared
array on another page, no name array), a few textures that share one path under several hashes get a
unique name, and each material takes the effect of a PS4 net material of its set (the PS4 net.pak is
the effect reference). The registry gets a `net-u2mp` level / package entry. The trophies button image stays in that
general rule (no per-image exception) but is not shown since the overlay removes the Trophies button (3.1).

In game: `keep_net_package` requests `net-u2mp` next to `net` when the file exists; the image lookup
(0x5558c0 port) answers a net id that neither the net table nor its PS3-name pass finds from the
`net-u2mp` package's material table (`+0x940`, once the package state is 7). Log:
`menu images: net-u2mp.pak imported`, `net-u2mp table ..., N materials`, `-> found in net-u2mp`.

## 9. Loading screen, playlist pictures, image sizes (2026-11-09)

Details: PORT-FRONTEND4.md.

- **Loading screen** (PS3 `0x4ae638`, absent on PS4): the fixed-layout version was rejected in run 43;
  loading.cpp now follows PS3 element by element (fill, edges, rosters page, band, map name, game
  description; not yet the fort.dds panels, coins and tip). On by default. PORT-FRONTEND5.md.
- **Playlist pictures**: no two built playlists share a picture; PS4 net.pak pictures are used where PS4
  has them, net-u2mp.pak only for elimination, grab-bag, plunder, the-lab. The image lookup now prefers
  the PS4 net table over net-u2mp.pak for every net id.
- **Image sizes**: PS4 net.pak textures are twice the PS3 resolution and a type 3 image takes its size
  from its texture, so on the multiplayer pages (PS3 layout) every PS4 net image was twice the PS3
  size. Those images are now measured at half their texture size.

## Audit of the PS3 net-menu helpers (2026-11-09)

The 75 PS3 net-menu functions the matcher left unpaired (0x4b7124..0x4c2bac) all have live PS4 slots in
the query table (PS3 0xe15b50 = PS4 0x105e370, paired by cond), except the known `ret` conds (3, 6..35, 36,
38, 64: ported or not needed). Two player-visible gaps were found and fixed:
- cond 100 (cinema list panel, 0x65ecc0) shows the L1 "Messages" entry (action 6b2970e2, cinema receive)
  only while netInfo+0x18 (PS3 NP message count) is set: online.cpp now counts received cinema files
  there and remaining.cpp's cinema_receive clears it.
- cond 104 (voice volume slider, 0x65f3f0 = PS3 0x4b77ac): menus.cpp answers op 4 (volume x 20), op 5
  (max 20, original) and op 9 (volume = value x 0.05) with voice.cpp's playback volume.
