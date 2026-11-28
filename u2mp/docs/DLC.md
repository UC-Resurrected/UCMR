# Uncharted 2 DLC maps on PC (map packs 1 and 2)

Static analysis only (the game was not run). Addresses are PS4 1.02 image offsets
(`boot-linked.bin` file offset - `0x183ce8`). No earlier third-party Uncharted 2 multiplayer notes or
code were used. Code: `gow3_pc/src/u2mp/dlc.cpp` (`install_dlc`, called after `install_menus`); its
header comment has the details.

## How the PS4 executable treats DLC

| what | address | reads | result on PC |
|---|---|---|---|
| entitlement refresh | `0xb887c0` (callers `0x3f76b2`, `0x3fbccc`) | `sceAppContentGetAddcontInfoList`; list at `0x1244e14` (+0 count, +4 labels of 0x14 bytes) | runtime reports 0 add-ons: list empty |
| MountDLCArchives | `0xb88ba0` | `"%s/dlc-map%d.psarc"` under each add-on mount point, `g_NumDLCArchives` `0x15d2a70` (max 8) | never reached (only after `sceAppContentAddcontMount`) |
| entitlement condition | `0xa4bcb0` type 12, via `0xa4bb40` | condition string minus ".edat" vs the list; tweak "Fake DLC Entitlement" `[0x172b9c0]+0x2da3` | only 5 store.bin costumes (`NATHANDRAKEPACK1`); no map |
| map pack field | map table dc `0x0dec97a4` (net.bin), record 0x38, `+0x10` pack | facility, ruins = 1; highrise, museum = 2 | |
| GetRandomNetGameIndexPair | `0x64a440` (dead; PS3 `0x49bdb8`) | map `+0x10 != 0` -> weight 0 (PS3: kept if every member owns the pack) | unused |
| custom game lists | `0x654c80` | every `*net-games*` game whose map id is in the map table | **no DLC check**: shows DLC maps even without data |
| GetDlcRequired | `0x5ab090` | trophy record `+0x20` | trophy text only |
| licence table | net.bin `0xf5899ce4` (`dlc1.edat`, `skin-dlc2/3.edat`, ...) | | PS3 leftover, no PS4 reader |

On PS3 1.10 the map pack field comes from the server: the `net.bin` of GOTY NPUA80698 and of the BCUS title
updates `dlc-patch5/7` has 0 in every map record (0x28 bytes there), and the server live data
`net-patch9.bin` (NPUA80698DATA2) replaces the map table with facility and both Ruins ids = 1, highrise and museum =
2 (checked 2026-11-27). So online, PS3 offered the Ruins only to parties that all owned map pack 1, whose
`dlc-map1.psarc` (mounted over the base archives) replaced `ruins-multi` with the map pack version: the import takes
that Ruins whenever it imports the map packs (IMPORT.md, "Map packs from any release").

So nothing on PS4 gates the maps by entitlement; the problem is the opposite one: the DLC maps are
always listed. No entitlement is faked and no archive mount is changed (the import writes loose files
that the `/app0` overlay serves; there is no `dlc-map%d.psarc` on PC and nothing asks for one).

## What dlc.cpp does

* **Map pack present** = through the runtime's `/app0` resolution (overlays first),
  `u2data/build/main/pak22/ps4/<level>.pak` exists, `<level>-ingame10.pak` exists for the main levels,
  and `pak22_ps4.txt` has `level-name <level>` (the ruins wide level may also be a `package` line):
  * map pack 1: `facility-multi`, `ruins-multi-wide` (the map pack Ruins: imported with the map packs)
  * map pack 2: `highrise-multi-2`, `museum-multi`, `museum-multi-eva`
  The result is logged once at start (`dlc: map pack N imported / not imported`).
* **Custom game**: the call at `0x654f98` (dc lookup of the map table inside `0x654c80`) is
  retargeted (byte-checked) to a copy of the map table without the records of absent packs; their
  games get no map slot, exactly as an unknown map would.
* **Matchmaking / playlists**: `dlc_net_game_available()` filters the playlist game choice and playlist
  list (`menus.cpp`).
* **The same map on every PC** (`session::net_game_same_map`, rule `start::pack_map_same_for_all`): the
  host's random game of Start Match (`session.cpp`), the map vote (`frontend3.cpp`) and the custom game
  Start Match check take a map pack map only when every member has the pack (the members' map-pack byte,
  PS3 0x49bdb8 / Start Match), and the Ruins also when no member has map pack 1 (all load the PS4 base
  Ruins). A party with and without map pack 1 never gets the Ruins: in a custom game the host sees "a party
  member lacks the map pack" when it has the pack, else the box for this PC lacking map pack 1 (it is the
  one lacking the pack its members play). Members of older PCs that send no map-pack byte are not counted.

## Matchmaking: Default / DLC (Profile, Other Settings)

PS3 (code read in GOTY NPUA80698; its "DLC" text is in the server text of NPUA80698DATA2 and BCES00757DATA2 alike)
has one Profile row for the map packs, not one per pack:
"Matchmaking" with the values "Default" and "DLC", between Taunt and Golden Guns, shown only to a player who owns a map
pack (without one the row is hidden and leaves a gap). It is NetPlayerData `+0x1b0` bit 0 (PS3 data block `+0x16c`),
saved with the career; a new career has Default. Only matchmaking reads it (PS3 `0x4b10d8` / `0x4b29d0` in the
matchmaking machine, before SearchRoom `0x4d3c50` and CreateJoinRoom `0x4d421c`):

* **DLC**: when the playlist has map-pack maps (`0x49b858`) and the party's common map packs (`0x4d6a08`, the AND of
  every member's map-pack byte) are not 0, the room key 0x4C carries those packs from bit 12 up, on search and create:
  the party only meets parties with the same map packs, and the pack maps are played.
* **Default**: the plain playlist key, the rooms every player shares; a pack map is still picked only when every
  member has its pack (`session::net_game_same_map`, above).

Custom games, the map vote and the map pick rules do not read it. PS3 also shows a one-time box when the Multiplayer
menu opens with a map pack owned ("... Would you like to go to your Profile page to set your Matchmaking preference?",
`041a85ca`; NetPlayerData `+0x1b0` bit 0x8000 = shown); the PC does not show it (the owned-DLC word stays 0 outside the
row's query).

On PC: menus.cpp `query_matchmaking` (MENUS.md 3.3) shows the row when `dlc_packs()` has map pack 1 or 2;
session.cpp `mm_pack_mask` (the H5 key mask of `parity-h-session.md`) returns 0 unless the row says DLC, and also 0
when the party has no map pack in common (bits 1, 2 of the AND; bit 0, the base maps, is always set): such a party
searches the plain key as with Default, as PS3 does when the common mask is 0. A career saved as DLC on a PC whose map
packs are gone (the row is hidden there) therefore searches the plain key too.

## Not verified

* Nothing was run. Whether the game then loads the imported DLC levels correctly is the import's work
  (IMPORT.md gaps 1-3).
* A saved custom game setting or a remote host's choice that names an absent DLC map is not filtered:
  a client without the pack would be sent to a map it does not have.
* The game type carousel (`0x655810`) falls back to the first game of a type when the current map has
  none; in the retail net.bin the first game of every type is a base map.
