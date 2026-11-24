# G59. Lobby scene: enter / leave, character re-dress, preview look, particle modules

Behaviour description for a reimplementation (reimpl2 pipeline, describer's document). It says what the functions
do, not how the old ones were written. Offsets are hexadecimal when prefixed with 0x, decimal otherwise. "Image
offset X" means the address `base + X`, where `base` is the game image base. All multi-byte values are
little-endian; "u8 / u16 / u32 / s32 / u64" are unsigned / signed integers of that width, "pointer" is 8 bytes.

**There is no floating point in this group.** None of the old functions used the x87 unit or a single-precision
instruction (they only move 32-bit and 64-bit words and call game functions with integer and pointer arguments);
PIPELINE.md's Float rule has nothing to apply to. A look record (3.6) contains game floats, but this group only
compares and copies it as bytes.

Related documents, referred to and not repeated: `G39_mp_startup.md` (the menu scene at image offset 0x17bba90 and
its natives 46 / 47, the level manager at 0x1afcf10, `u3h_crc_sid`, `u3h_active_page`,
`u3x_lobby_scene_shutdown`), `G54_taunt_kit_packages.md` (the player-customisation manager at image offset
0x17f9938 and its package list, which `u3x_loadout_tick` fills), `G43_coop_spectate.md` (looks of 0x28 bytes, net
player team), `G30_post_match.md` (`u3x_pm_ngentry`, `u3x_pm_coop` behind `u3h_netgame_entry`, `u3h_coop`),
`G08_lan_room_client.md` (the frame parts that call `u3x_lobby_scene`).

The group is written as **three independent parts** (about 300 old lines with the strays). No part needs another
part's source: where one calls another, the callee is declared by the signature in 2.1.

| Part | Subject | Exported names to deliver |
|---|---|---|
| 1 | Lobby menu scene enter / leave; the two multiplayer particle modules | `u3x_scene_hunter`, `u3x_scene_free`, `u3x_lobby_scene`, `u3x_fnv1a_id`, `u3x_module_loaded`, `u3x_part_module_wanted`, `u3x_part_modules_tick` |
| 2 | The lobby character and its re-dress | `u3x_s16_char`, `u3x_h01_loaded`, `u3x_h01_hash`, `u3x_h01_tint`, `u3x_h01_redress`, `u3x_h01_tick` |
| 3 | The preview look on the character pages | `u3x_s17_page`, `u3x_s17_apply`, `u3x_s16_refresh`, `u3x_s16_tick` |

Part 3 calls part 2's `u3x_s16_char` and `u3x_h01_tick`; part 2 calls nothing of parts 1 and 3; part 1 calls
nothing of parts 2 and 3.

## 1. Purpose and callers

PS3 1.19 drives the 3D scene behind the multiplayer lobby menus (the "menu scene": a stage, a camera and the
player's own character) from its network update every frame, puts a changed kit on the standing character without
respawning it, shows the look under the cursor on the character pages, and keeps two particle modules loaded while
multiplayer runs. The PS4 game has the building blocks (scene enter / leave, look store, tint pass, set-model) but
not the code that calls them. The helper supplies the callers.

**Part 1.**

- `u3x_lobby_scene(base, s)`: once per frame; decides whether the menu scene is left, or updated and entered, and
  runs the team pass during a match. Called through the C wrapper `k34_scene(base, s)` from the C export
  `u3h_lnb_frame_parts(base, s, 0)` (G08's frame step: after the net-time block, before the side placement fix).
- `u3x_scene_hunter(base, s, p)`: is net player `p` a co-op hunter. Called by `u3x_lobby_scene`, `u3x_scene_free`
  and the C wrapper `k34_368b3c` (which nothing calls in v7.212).
- `u3x_scene_free(base, s)`: may the scene be entered while a popup is up. Called by `u3x_lobby_scene` and the C
  wrapper `k34_310a00` (which nothing calls in v7.212).
- `u3x_part_modules_tick(base)` (old C `h04_tick`): once per frame; wants the two multiplayer particle modules
  while multiplayer is active and gives them back afterwards. Called by the hook dispatcher `u3_profile_event` for
  hook kind 16 (the per-frame tick), only when the image base is not null, after `h22_tick` and before `h08_tick`.
- `u3x_part_module_wanted(base, id)` (old C `h04_wanted`): is a module in the particle manager's want map. Called
  by `u3x_part_modules_tick` only.
- `u3x_fnv1a_id(id)`, `u3x_module_loaded(modules, id)`: the map's hash and the loaded-module test. Called by the
  two functions above only (through the C code in the old build).

**Part 2.**

- `u3x_h01_tick(base, cm)`: once per frame; re-dresses the lobby character when its kit differs from the wanted
  kit and every wanted package is loaded. Called by `u3x_s16_tick` (with `cm` = image offset 0x17f9938). The C
  wrapper `h01_tick` has no caller in v7.212.
- `u3x_h01_redress(base, cm)`: the re-dress itself. Called by `u3x_h01_tick`; the C wrapper `h01_redress` has no
  caller.
- `u3x_h01_tint(base, cm, ch, side)` (old C `h01_tint`): refreshes the character's look record and its colours
  when the kit is unchanged. Called by `u3x_h01_tick` only (in the old build through the C export `u3h_h01_tint`).
- `u3x_h01_loaded(base, id)`, `u3x_h01_hash(base, name)`: "is this actor package loaded" and "name id of a kit
  part". Called inside part 2; their C wrappers `h01_loaded`, `h01_hash` have no caller.
- `u3x_s16_char(base, cm)`: the live character the customisation manager shows. Called by parts 2 and 3 and by the
  C wrapper `s16_char` (C export `u3h_s16_char`).

**Part 3.**

- `u3x_s16_tick(base)`: once per frame. Called through the C wrapper `s16_tick(base)` by `u3_profile_event` for
  hook kind 16, only when the image base is not null, after `m04_tick` and before `f4_tick`.
- `u3x_s16_refresh(base, r)`: the hook at the entry of the game's own preview refresh (function at image offset
  0x995d50). Called through the C wrappers `s16_refresh` / `s_kind` by the dispatcher for hook kind 233, only when
  the register block `r` is not null. Result 1 = the helper handled the call (the game function is skipped),
  0 = the game function runs.
- `u3x_s17_apply(base, cm, own_kill)`: puts the look the menu asks for on the character. Called by
  `u3x_s16_refresh` (own_kill 0), `u3x_s16_tick` (own_kill 1) and the C wrapper `s17_apply` (no caller).
- `u3x_s17_page(page)`: is `page` one of the nine character pages. Called by `u3x_s16_tick` and the C wrapper
  `s17_page` (no caller).

Everything runs on the game's main thread (the per-frame hook, the frame step, a menu callback). No locking;
nothing else writes what the group writes, except that the C part's `u3h_s19_count` writes bits 8..15 of
diagnostic word 11 and word 15 (3.8) from the same thread.

## 2. Interface

### 2.1 Functions to deliver (all `extern "C"`)

| Part | Function | Signature | Result | C wrapper passes |
|---|---|---|---|---|
| 1 | co-op hunter test | `int u3x_scene_hunter(uintptr_t base, void *s, const uint8_t *p)` | 1 / 0 | `k34_368b3c(base, s, p)`: all three (exists) |
| 1 | scene free test | `int u3x_scene_free(uintptr_t base, void *s)` | 1 / 0 | `k34_310a00(base, s)`: both (exists) |
| 1 | scene frame step | `void u3x_lobby_scene(uintptr_t base, void *s)` | - | `k34_scene(base, s)`: both (exists) |
| 1 | map hash | `uint32_t u3x_fnv1a_id(uint32_t id)` | hash | (called from this part only once the C bodies are gone) |
| 1 | module loaded | `int u3x_module_loaded(const uint8_t *modules, uint32_t id)` | 1 / 0 | C `h04_loaded` goes away |
| 1 | module wanted | `int u3x_part_module_wanted(uintptr_t base, uint32_t id)` | 1, 0 or -1 | **new name**: C `h04_wanted(base, sid)` becomes a one-line wrapper passing both |
| 1 | module tick | `void u3x_part_modules_tick(uintptr_t base)` | - | **new name**: C `h04_tick(base)` becomes a one-line wrapper passing base |
| 2 | shown character | `uint8_t *u3x_s16_char(uintptr_t base, uint8_t *cm)` | character or null | `s16_char(base, cm)`: both (exists) |
| 2 | package loaded | `int u3x_h01_loaded(uintptr_t base, uint32_t id)` | 1 / 0 | `h01_loaded`: both (exists) |
| 2 | part name id | `uint32_t u3x_h01_hash(uintptr_t base, const char *name)` | id or 0 | `h01_hash`: both (exists) |
| 2 | look and colours | `void u3x_h01_tint(uintptr_t base, uint8_t *cm, uint8_t *ch, int32_t side)` | - | **new name**: C `h01_tint(base, cm, ch, side)` (agc_port.inc) becomes a one-line wrapper passing all four |
| 2 | re-dress | `void u3x_h01_redress(uintptr_t base, uint8_t *cm)` | - | `h01_redress`: both (exists) |
| 2 | re-dress tick | `void u3x_h01_tick(uintptr_t base, uint8_t *cm)` | - | `h01_tick`: both (exists) |
| 3 | character page test | `int u3x_s17_page(uint32_t page)` | 1 / 0 | `s17_page(pg)` (exists) |
| 3 | apply the look | `int u3x_s17_apply(uintptr_t base, uint8_t *cm, int own_kill)` | 1 = handled, 0 = not | `s17_apply`: all three (exists) |
| 3 | refresh hook | `int u3x_s16_refresh(uintptr_t base, uint64_t *r)` | 1 = skip the game function, 0 = run it | `s16_refresh(base, r)`: both (exists) |
| 3 | frame tick | `void u3x_s16_tick(uintptr_t base)` | - | `s16_tick(base)`: base (exists) |

No float or double crosses any of these. The file-private pieces of the old code (the plausibility test for a
name, the part-name accessor, the look table and model registry lookups, the page list) are part of the new code
and described in section 4.

### 2.2 Functions of the C part, of other groups and of the shared tools that are called

| Name | Signature | What it does | Owner assumed |
|---|---|---|---|
| `u3h_ext` | `uint8_t *u3h_ext(void)` | the helper's state block; `ext_eb()` (CONVENTIONS.h) = its eb words. The ec words are the u32 array at `u3h_ext()` + 0x4450 (ec[n] at + 4·n; CONVENTIONS.h has no accessor) | shared tools |
| `u3h_netgame_entry` | `uint8_t *u3h_netgame_entry(uintptr_t base, void *s)` | the current net game record, or null (forwards to `u3x_pm_ngentry`) | G30 |
| `u3h_coop` | `int u3h_coop(uintptr_t base, void *s)` | non-zero when the current net game is a co-op game (forwards to `u3x_pm_coop`) | G30 |
| `u3h_scene_was` | `uint32_t *u3h_scene_was(void)` | address of one u32 of the C part (old lobby-scene diagnostics) | C part, stays |
| `u3h_s16_diag` | `volatile uint32_t *u3h_s16_diag(void)` | address of the sixteen diagnostic words D[0..15] (3.8) | C part, stays |
| `u3h_crc_sid` | `uint32_t u3h_crc_sid(const char *text)` | the game's 32-bit name id of a NUL-terminated text (G39 2) | C part |
| `u3h_active_page` | `uint32_t u3h_active_page(uintptr_t base)` | id of the menu page on top of the page stack; 0 while the menu changes pages or has none (G39 2) | C part |

The old C++ also reached the character through `u3h_s16_char` and the look refresh through `u3h_h01_tint`; the new
code calls `u3x_s16_char` and `u3x_h01_tint` directly (same results: the C functions only forwarded / are replaced
here). The two exports stay as one-line forwards.

## 3. Game facts

### 3.1 Game functions (plain calls, integer and pointer arguments)

| Image offset | Arguments | Result | Meaning | Part |
|---|---|---|---|---|
| 0x5b7040 | (pointer: menu scene, s32 1) | - | leaves the menu scene (its props and character go away) | 1 |
| 0x5b4e60 | (pointer: menu scene) | - | runs the lobby camera (G39's native 47) | 1 |
| 0x5b7020 | (pointer: menu scene) | - | enters the menu scene (G39's native 46) | 1 |
| 0x5b6e40 | (pointer: menu scene) | - | the scene's team pass during a match | 1 |
| 0x784b20 | (pointer: net player, u32 0) | pointer to a look | the look of a net player | 1 |
| 0x996a60 | (pointer: customisation manager, pointer: look) | u8 (only the low byte is meaningful) | non-zero when every package that look needs is loaded | 1 |
| 0xeb4110 | (pointer: want map, u32 module id) | - | wants a particle module (one more reference) | 1 |
| 0xeb41e0 | (pointer: want map, u32 module id) | - | gives one reference back | 1 |
| 0x10eb380 | (pointer: level manager = image offset 0x1afcf10, u32 package id) | pointer to the actor package object, or null | finds an actor package | 2 |
| pointer stored at image offset 0x1702720 | (s32 0, pointer to a NUL-terminated name, s32 1) | u32 | the game's name id function. The pointer is read from the image at every call and is **not** tested for null | 2 |
| 0x995ec0 | (pointer: customisation manager, s32 user, s32 side) | s32 | index of the character type the menu asks for | 2, 3 |
| 0x9964d0 | (pointer: customisation manager) | - | stores the manager's look into the shown character's look record | 2 |
| 0xbc8630 | (pointer: character, pointer to a part name, s32 part index) | - | sets one kit part of a player character | 2 |
| 0xbcb7c0 | (pointer: character, pointer to a part name, s32 part index) | - | sets one kit part and rebuilds the model from all eleven parts | 2 |
| 0x9965e0 | (pointer: customisation manager) | - | the tint pass (colours of the shown character) | 2 |
| 0xbc9e50 | (pointer: character) | - | re-applies the character's look record colours | 2 |
| 0x11f2bf0 | (u32 symbol id) | pointer to a pointer, or null | script symbol lookup | 3 |
| 0x997a40 | (pointer: 0x40-byte result buffer, pointer: customisation manager, s32 user, s32 type index, s32 side) | - (fills the buffer; its first 0x28 bytes are the look) | builds the look the menu asks for | 3 |
| 0x11a6250 | (pointer: character) | - | kills the character (the scene places a new one with its entrance) | 3 |
| 0x997230 | (pointer: customisation manager) | - | the manager's own update (wants the look's packages) | 3 |
| 0x997670 | (pointer: customisation manager) | - | activates the manager (the same update, then its active byte = 1) | 3 |

**Virtual calls.** An object's first pointer is its function table; "method at table offset T" is the pointer at
that byte offset in the table, called with the object as first argument.

| Object | Table offset | Further arguments | Result | Meaning | Part |
|---|---|---|---|---|---|
| character | 0x1f8 | - | pointer, or null | a sub-object of the character | 2 |
| that sub-object | 0x38 | u32 0x80e056d1 | - | told the model changed (the game does the same when it places the character) | 2 |
| character | 0x190 | - | u32 | id of the character's current model | 3 |
| character | 0x4f8 | - | - | re-applies its look record in place | 3 |

### 3.2 Ids and constants

Every id below is the game's name id of a text (`u3h_crc_sid`): the rule generates it, no table is stored.

| Value | Name | Meaning |
|---|---|---|
| 0xa5a63e9b | `Player` | type id (u32 at object + 0x30) of a local player character |
| 0xb7b3c967 | `TaskManagerOnline` | type id of the object that holds the view while an online task screen is up |
| 0xf114b39d | `none` | the "no part" name |
| 0x67adb399 | `anim-proto-multiplayer` | the multiplayer animation actor package |
| 0x666e04e1 | `part-multiplayer` | particle module 0 |
| 0x4be4375d | `part-obj-shade-char` | particle module 1 |
| 0x81bfb972 | `PAGE_BODY_TYPE` | the character list page ("main look page") |
| 0xcbe59a09 | `PAGE_CUSTOMIZE_COLOR` | the colour picker page |
| 0xbd3bb3b9 | (name not recorded) | script symbol of the character type table |
| 0x80e056d1 | (name not recorded) | the message of the virtual call in 3.1 |
| 8 | - | state of a loaded actor package |
| 11 | - | net game type of Co-op Adventure (s32 at net game record + 0xc) |
| 2 | - | menu scene state "entered"; also match state "in a match" |

**The nine character pages** (`u3x_s17_page`): the old code holds their ids as a literal list. The game has no
such list (each id appears in the PS4 image only as an immediate in the code that registers the page's callback),
but the ids are the name ids of nine page names, so the new code derives them with `u3h_crc_sid` (at first use,
kept in its own memory) from the names: `PAGE_BODY_TYPE`, `PAGE_BODY_BASE`, `PAGE_BODY_TORSO`, `PAGE_BODY_HEAD`,
`PAGE_BODY_ARMS`, `PAGE_BODY_LEGS`, `PAGE_BODY_FEET`, `PAGE_CUSTOMIZE_COLOR`,
`PAGE_MULTIPLAYER_CUSTOMIZE_CHARACTER`. (Checked: the name ids of these nine texts are exactly the nine old
values. Order is irrelevant: the list is only searched.) The same holds for the two particle module ids and the
other named ids above: the texts go into U3X_RO arrays, or the id is written as a named constant with its text in
a comment; neither is a table of game values.

`u3x_fnv1a_id` is the public 32-bit FNV-1a hash (offset basis 0x811c9dc5, prime 0x01000193; Fowler / Noll / Vo,
published) over the four bytes of the id, lowest byte first: for each byte, exclusive-or it into the hash, then
multiply by the prime modulo 2^32.

### 3.3 Game data

| Image offset | Type | Meaning |
|---|---|---|
| 0x13ac250 | text | the game's empty string (one NUL byte) |
| 0x16e2f88 | pointer | model registry: array of 0x28-byte entries (3.7) |
| 0x18bed80 | s32 | model registry: number of entries |
| 0x1702720 | pointer | the name id function (3.1) |
| 0x17bba90 | object | the menu scene; its u32 at +0 is the scene state (2 = entered) |
| 0x17bd080 | object | the menu (3.4) |
| 0x17cb3d0 | pointer | net player 0 of the net game manager (manager 0x17cb060 + 0x370), or null |
| 0x17d1868 | s32 | match state; 2 = in a match |
| 0x17d5c61 | u8 | "multiplayer active" |
| 0x17d7761 | u8 | a flag of the map vote (G30: "closed"); non-zero blocks the scene enter under a popup |
| 0x17f9938 | object | the player-customisation manager CM (3.5) |
| 0x1a4d910 | object | the particle manager (3.9) |
| 0x1afcf10 | object | the level manager (G39 3.2) |
| 0x1b3ee38 | handle | handle of the object that holds the view of screen 0: pointer to a pointer at +0, u32 process id at +8 |
| 0x1bb9500 | handle | handle of the player of screen 0, same layout |

A **handle** {pointer-to-pointer H, u32 id} is **live** when H is not null, id is not 0, the pointer H points to is
not null and the u32 at that object + 0x7c equals id; its object is then the pointer H points to.

### 3.4 The menu (image offset 0x17bd080)

| Offset | Type | Meaning |
|---|---|---|
| 0x8 | s32 | non-zero while the menu is up |
| 0xc | s32 | non-zero while the menu is switching off |
| 0x898 | pointer | the pending popup record, or null; its u32 at +0x10 is non-zero while a popup page is pending |
| 0xdf5 | u8 | non-zero = the menu acts for user 1 |
| 0x2694 | s32 | otherwise: the menu's user |

"The menu user" below = 1 when the u8 at +0xdf5 is non-zero, else the s32 at +0x2694.

### 3.5 The customisation manager CM

| Offset | Type | Meaning |
|---|---|---|
| 0x0 | s32 | side (0 or 1: hero / villain); other values make the group do nothing |
| 4 + 4·i, i = 0..49 | u32 | package list: id of entry i, 0 = free (G54 3.3) |
| 0xe00 + 0x58·side + 8·k, k = 0..10 | pointer | wanted kit part k of that side: a NUL-terminated name, or null |
| 0xf60 | pointer | shown character: pointer to the pointer to the character, or null |
| 0xf68 | u32 | shown character: process id, or 0 |
| 0xf70 | u8 | non-zero = the manager is active (the lobby shows the character) |
| 0xf74 | s32 | the character type under the cursor |
| 0xf78 + 0x28·side | 0x28 bytes | the look being edited for that side |
| 0xfc8, 0xfc9 | u8 | preview modes set by the page callbacks (only printed) |
| 0x104c | u8 | "re-dress asked" (set by the game whenever a wanted part changes) |

### 3.6 The character (a player object)

| Offset | Type | Meaning |
|---|---|---|
| 0x0 | pointer | function table |
| 0x30 | u32 | type id |
| 0x7c | u32 | process id |
| 0x1b80 + 8·k, k = 0..10 | pointer | name of the part k it wears, or null |
| 0x1c40 | 0x28 bytes | its look record; byte 0 = character type index |

Net player record (3.3, 0x17cb3d0): s32 at +0x21c = team; 1 = the hunters.

Actor package object (answer of 0x10eb380): u32 at +0x30 = state.

### 3.7 Character type table and model registry (part 3)

The answer of 0x11f2bf0(0xbd3bb3b9) is a pointer SLOT to a pointer TABLE. **Usable table**: SLOT is not null and
passes `ptr_ok`, TABLE is not null and passes `ptr_ok`. TABLE: u32 at +0 = number of character types; pointer at
+8 = the rows; a row is 0x70 bytes and its u32 at +0x58 is the id of that type's model.

Model registry: the entries pointer (3.3) must pass `ptr_ok`, else the registry has no entries. Entry (0x28
bytes): pointer at +0 = model object, or null (only null is tested); u32 at +0x20 = the model id that rows refer
to. Model object: u32 at +0x50 = the id that the character's method 0x190 returns.

**Registered model of a character** = the u32 at +0x20 of the entry with the **highest index** below the count
whose model object is not null and whose object id (+0x50) equals the character's current model id; 0 when there
is none, when the count is 0 or negative, or when the entries pointer fails `ptr_ok`.

### 3.8 Helper state

**eb words** (`ext_eb()`):

| Word | Meaning |
|---|---|
| eb[1] bit 3 (value 8) | set = the scene frame step is off |
| eb[1] bit 8 (value 0x100) | set = the view holder is taken from the handle at 0x1bb9500, clear = from 0x1b3ee38 |
| eb[2] bit 25 (0x02000000) | set = the refresh hook always lets the game function run |
| eb[2] bit 26 (0x04000000) | set = the frame tick does not handle the character pages |
| eb[2] bit 27 (0x08000000) | set = character type indexes are limited to 0..63 |
| eb[28], eb[29] | scene leaves because the menu is off / because a `TaskManagerOnline` holds the view |
| eb[152..155], eb[157] | scene step runs, enters, leaves, team passes; enters during a match |

**ec words** (u32 at `u3h_ext()` + 0x4450 + 4·n):

| Word | Meaning |
|---|---|
| ec[1] bit 0 (value 1) | clear (default) = the re-dress of part 2 is on and the look application of part 3 is off; set = the opposite |
| ec[1] bit 3 (value 8) | set = the particle modules are not wanted |
| ec[204] | module tick runs |
| ec[205] | want calls made |
| ec[206] | modules released |
| ec[207] | ticks on which the want map was not usable (per module) |
| ec[208] | status word (4.7) |
| ec[209] | bit k set = module k is marked as wanted by the helper |

**D[0..15]**, the sixteen u32 at `u3h_s16_diag()` (owned by the C part; read and written one word at a time, each
"plus 1" is a read followed by a write of the whole word):

| Word | Meaning |
|---|---|
| D[0] | refresh hook calls |
| D[1] | looks stored by part 3; also re-dresses asked by the game (part 2) |
| D[2] | model changes seen by part 3; also kit differences found by part 2 |
| D[3] | in-place look refreshes (part 3); also re-dresses done (part 2) |
| D[4] | look applications without a character (part 3); also frames waiting for packages (part 2) |
| D[5] | part 3: the type under the cursor at the last application. Part 2: low 16 bits = mask of the differing part, high 16 bits = mask of parts set to the empty name |
| D[6] | part 3: last application's {side, +0xfc8, +0xf70, +0xfc9}. Part 2: frames skipped after a re-dress |
| D[7] | part 3: last look's {type index byte, requested index}. Part 2: re-dresses without a player character |
| D[8], D[9], D[10] | frames on the main look page, cursor changes seen there, last state word |
| D[11] | low byte: applications to a non-player character (stops at 255); bits 8..15: the C part's; byte 2 and byte 3: see 4.13 |
| D[12] | manager activations by the tick |
| D[13] | frames with a character page open |
| D[14] | low bits: applications with an index or side out of range; bit 31: "just re-dressed" |
| D[15] | the C part's |

The words are shared between parts 2 and 3 exactly as listed (the old diagnostics overlap; keep them so).

**Own memory** of the new code, zero-initialised (U3X_BSS): part 2: two u32 counters TINT_CALLS and TINT_CHANGES
(nothing reads them; a debugger does); part 3: s32 LAST_CURSOR and u8 PAGE_WAS_OPEN.

### 3.9 The particle manager (image offset 0x1a4d910)

| Offset | Type | Meaning |
|---|---|---|
| 0x298 + 0x30·k | u32 | loaded-module record k: the module id is its first word |
| 0x8c8 | u32 | number of loaded-module records |
| 0x50 | object | the **want map** (image offset 0x1a4d960), the first argument of 0xeb4110 / 0xeb41e0 |

Want map: s32 at +0x4c = bucket count N; pointer at +0x10 = array of N + 1 node pointers (entry N is the map's end
node). A node: pointer at +0 = next node, u32 at +0x10 = module id. The bucket of an id is
`u3x_fnv1a_id(id)` modulo N (unsigned).

## 4. Behaviour

"Plausible pointer" means a value of at least 0x10000 and below 2^47 **without** an alignment condition; "aligned
pointer" additionally needs the low three bits clear (= CONVENTIONS.h `ptr_ok`). "Usable name" = a pointer that
is not null, is a plausible pointer, and whose first byte is not 0. Counters wrap modulo 2^32.

### Part 1

#### 4.1 `u3x_scene_hunter(base, s, p)`

1 when all three hold, tested in this order and stopping at the first that fails: the "multiplayer active" byte is
non-zero; `u3h_coop(base, s)` is non-zero; the s32 at `p` + 0x21c is 1. Else 0. `p` is not tested for null (the
callers pass a non-null record; it is only read after the first two tests passed).

#### 4.2 `u3x_scene_free(base, s)`

1. E = `u3h_netgame_entry(base, s)`. E not null and its type (s32 at +0xc) is 11: result 0.
2. P = net player 0 (pointer at image offset 0x17cb3d0). Null: result 0.
3. Result = 1 when `u3x_scene_hunter(base, s, P)` is 0, else 0.

#### 4.3 `u3x_lobby_scene(base, s)`

SC = the menu scene, M = the menu, CM = the customisation manager (addresses in 3.3).

1. eb[1] bit 3 set: return, nothing else. Otherwise eb[152] plus 1.
2. **Leave reason.** Reason 1 when the menu is not "up and not switching off" (3.4: +8 is 0, or +0xc is not 0).
   Otherwise read the view holder handle (3.8: which of the two image offsets); when it is live (3.3) and its
   object's type id (+0x30) is `TaskManagerOnline`: reason 2. Otherwise no reason.
3. **With a reason:** game 0x5b7040(SC, 1); eb[154] plus 1; eb[28] plus 1 for reason 1 or eb[29] plus 1 for
   reason 2; store 0 into the u32 at `u3h_scene_was()`.
4. **Without a reason:** game 0x5b4e60(SC). Then decide whether to enter; the scene is entered unless one of the
   following says no, and these are looked at **only when CM is active** (u8 at CM + 0xf70 non-zero), in this
   order:
   - a. A popup is pending (M's popup record is not null and its u32 at +0x10 is non-zero) **and** either the u8
     at image offset 0x17d7761 is non-zero or `u3x_scene_free(base, s)` is 0. (`u3x_scene_free` is called only
     when a popup is pending and that byte is 0.) Then: no enter, and b is not looked at.
   - b. P = net player 0. Null: no enter. Otherwise, when `u3x_scene_hunter(base, s, P)` is non-zero: L = game
     0x784b20(P, 0); when the low byte of game 0x996a60(CM, L) is non-zero: no enter.

   When the scene is entered: game 0x5b7020(SC); eb[153] plus 1; and when the match state (s32 at 0x17d1868) is
   2, eb[157] plus 1.
5. In both cases, last: when the match state is 2 (read again): game 0x5b6e40(SC); eb[155] plus 1.

Order of game calls in one run: either {0x5b7040} or {0x5b4e60, [0x784b20, 0x996a60], [0x5b7020]}, then
[0x5b6e40]; `u3h_netgame_entry` / `u3h_coop` as 4.1 and 4.2 say.

#### 4.4 `u3x_fnv1a_id(id)`

The hash of 3.2. Pure.

#### 4.5 `u3x_module_loaded(modules, id)`

`modules` = the particle manager. Count = its u32 at +0x8c8. A count above 64: result 0. Otherwise 1 when one of
the first `count` loaded-module records (3.9) has the id `id`, else 0. Reads only.

#### 4.6 `u3x_part_module_wanted(base, id)`

Reads the want map (3.9) only.

1. N not between 1 and 0x100000 inclusive, or the bucket array pointer fails `ptr_ok`: result -1.
2. Walk the chain that starts at the id's bucket entry, following the next pointers, for at most 4096 nodes, until
   a null pointer:
   - a node pointer that fails `ptr_ok`: result -1;
   - a node whose module id equals `id`: result 0 when that node is the map's end node (the pointer in bucket
     array entry N), else 1.
3. Chain ended (null, or 4096 nodes looked at) without a match: result 0.

#### 4.7 `u3x_part_modules_tick(base)`

Modules: k = 0 is `part-multiplayer`, k = 1 is `part-obj-shade-char` (3.2). MAP = image offset 0x1a4d960.

1. MINE = ec[209]. ON = (ec[1] bit 3 is clear) and (the "multiplayer active" byte is non-zero); both are read once,
   here. ec[204] plus 1.
2. For k = 0, then k = 1:
   - **ON** and bit k of MINE set: nothing.
   - **ON** and bit k clear: W = `u3x_part_module_wanted(base, id_k)`. W negative: ec[207] plus 1 and nothing
     more for this module. W = 0: game 0xeb4110(MAP, id_k) and ec[205] plus 1. For W = 0 **and** for W = 1 (the
     game already wants it; no call): set bit k of MINE.
   - **not ON** and bit k set: when `u3x_part_module_wanted(base, id_k)` is greater than 0: game
     0xeb41e0(MAP, id_k). In either case clear bit k of MINE and ec[206] plus 1.
   - **not ON** and bit k clear: nothing.
3. ec[208] = MINE, plus (loaded 0) · 0x100, plus (loaded 1) · 0x200, plus (wanted 0, low 8 bits) · 0x10000, plus
   (wanted 1, low 8 bits) · 0x1000000, where loaded k = `u3x_module_loaded(particle manager, id_k)` and wanted k =
   `u3x_part_module_wanted(base, id_k)` evaluated now, after step 2 (so -1 shows as 0xff). These four lookups only
   read.
4. ec[209] = MINE.

### Part 2

#### 4.8 `u3x_s16_char(base, cm)`

`base` is not used. H = pointer at `cm` + 0xf60, ID = u32 at `cm` + 0xf68. Null when H is null, ID is 0 or H fails
`ptr_ok`. Otherwise C = the pointer H points to; result C when C is not null, passes `ptr_ok` and its process id
(u32 at +0x7c) equals ID; else null. No game call.

#### 4.9 `u3x_h01_loaded(base, id)` and `u3x_h01_hash(base, name)`

- `u3x_h01_loaded`: 0 when `id` is 0 (no game call). Otherwise A = game 0x10eb380(level manager, id); 1 when A is
  not null, is an aligned pointer and its state (u32 at +0x30) is 8; else 0.
- `u3x_h01_hash`: 0 when `name` is not a usable name (no game call). Otherwise V = the name id function (3.1)
  called with (0, name, 1); result 0 when V is the `none` id, else V.

#### 4.10 `u3x_h01_tint(base, cm, ch, side)`

LOOK = the 0x28 bytes at `ch` + 0x1c40.

1. TINT_CALLS plus 1. Remember LOOK's 0x28 bytes.
2. When `u3h_active_page(base)` is `PAGE_CUSTOMIZE_COLOR`: copy the 0x28 bytes at `cm` + 0xf78 + 0x28·`side` into
   LOOK. Otherwise: game 0x9964d0(`cm`).
3. When LOOK now differs from the remembered bytes: TINT_CHANGES plus 1; game 0xbc9e50(`ch`).

(Why: on the colour page the game's own colour refresh refuses a kit with a `none` part; the helper puts the
edited look on the character itself.)

#### 4.11 `u3x_h01_redress(base, cm)`

1. u8 at `cm` + 0x104c = 0 (always, first).
2. CH = `u3x_s16_char(base, cm)`. Null, or its type id (+0x30) is not `Player`: D[7] plus 1, return.
3. SIDE = s32 at `cm` + 0. Not 0 or 1: return.
4. Only when eb[2] bit 27 is set: I = game 0x995ec0(`cm`, menu user (3.4), SIDE); when I as unsigned is 64 or
   more: return.
5. Game 0x9964d0(`cm`).
6. **Choose the eleven names**, k = 0 .. 10 in order. W = wanted part k of SIDE (3.5). When W is a usable name and
   `u3x_h01_loaded(base, u3h_crc_sid(W))` is 1 the name is W; otherwise it is the game's empty string (image
   offset 0x13ac250), and when W was a usable name bit k of a mask U is set. (`u3h_crc_sid` and the package lookup
   happen only for usable names; all eleven lookups come before step 8.)
7. D[5] = its low 16 bits kept, U in bits 16 and up.
8. For k = 0 .. 9 in order: first, when the chosen name is not empty, the name id function is called once with
   (0, chosen name, 1) and its result is ignored (see section 6, item 1); then game 0xbc8630(CH, chosen name, k).
9. Game 0xbcb7c0(CH, chosen name of part 10, 10). D[3] plus 1.
10. O = CH's method at table offset 0x1f8. When O is not null and is an aligned pointer: O's method at table
    offset 0x38 with the argument 0x80e056d1.
11. Game 0x9965e0(`cm`). D[14] bit 31 set (other bits kept).

#### 4.12 `u3x_h01_tick(base, cm)`

1. ec[1] bit 0 set: return.
2. The "multiplayer active" byte is 0, or the match state (0x17d1868) is 2: return.
3. D[14] bit 31 set: clear it, D[6] plus 1, return (the frame after a re-dress is skipped).
4. CM not active (u8 at `cm` + 0xf70 is 0): return.
5. "Re-dress asked" (u8 at `cm` + 0x104c non-zero): D[1] plus 1 and `u3x_h01_redress(base, cm)`. The tick then
   **continues** with step 6 in the same call.
6. CH = `u3x_s16_char(base, cm)`. Null or not a `Player`: return. SIDE = s32 at `cm` + 0; not 0 or 1: return.
7. **Ready test**, stopping at the first failure:
   - a. `u3x_h01_loaded(base, anim-proto-multiplayer id)` is 1;
   - b. for i = 0 .. 49 in order: package list entry i is 0, or `u3x_h01_loaded(base, entry)` is 1;
   - c. for k = 0 .. 10 in order: wanted part k of SIDE is not a usable name, or its `u3h_crc_sid` is the `none`
     id, or that id is one of the 50 package list entries.

   Not ready: D[4] plus 1, return.
8. **Compare**, k = 0 .. 10 in order: `u3x_h01_hash(base, wanted part k of SIDE)` against
   `u3x_h01_hash(base, the name CH wears as part k)` (pointer at CH + 0x1b80 + 8·k); the wanted one is hashed
   first. Stop at the first k where they differ.
9. No difference: `u3x_h01_tint(base, cm, CH, SIDE)`, return.
10. A difference at k: D[2] plus 1; D[5] = its high 16 bits kept, low 16 bits = 2^k (only the first differing
    part); `u3x_h01_redress(base, cm)`.

### Part 3

#### 4.13 `u3x_s17_page(page)`

1 when `page` is one of the nine ids of 3.2, else 0. (Page id 0 is never one of them.)

#### 4.14 `u3x_s17_apply(base, cm, own_kill)`

1. CH = `u3x_s16_char(base, cm)`. Null: D[4] plus 1, result 0.
2. CH's type id is not `Player`: when the low byte of D[11] is not 0xff, D[11] plus 1; result 0.
3. USER = menu user (3.4); SIDE = s32 at `cm` + 0; I = game 0x995ec0(`cm`, USER, SIDE).
4. Look up the character type table (game 0x11f2bf0, 3.7). LIMIT = 64 when eb[2] bit 27 is set; otherwise the
   table's number of types, or 0 when the table is not usable. When I as unsigned is LIMIT or more, or SIDE is not
   0 or 1: D[14] plus 1, result 0.
5. NEW = a zeroed 0x40-byte buffer aligned to 8; game 0x997a40(NEW, `cm`, USER, I, SIDE).
6. D[5] = the u32 at `cm` + 0xf74. D[6] = SIDE + 0x100 · (u8 at `cm` + 0xfc8) + 0x10000 · (u8 at `cm` + 0xf70) +
   0x1000000 · (u8 at `cm` + 0xfc9). D[7] = NEW's byte 0 + 0x100 · (I's low 24 bits).
7. CHANGED = the first 0x28 bytes of NEW differ from CH's look record. When `own_kill` is non-zero and not
   CHANGED: result 1, nothing else. When CHANGED: copy those 0x28 bytes into CH's look record; D[1] plus 1.
8. Look up the character type table **again** (second call of 0x11f2bf0). Not usable: result 0. ROWS = its rows
   pointer; fails `ptr_ok`: result 0.
9. CUR = CH's method at table offset 0x190. REG = the registered model (3.7) of CUR. WANT = the model id (u32 at
   +0x58) of row number (NEW's byte 0).
   - REG differs from WANT: D[2] plus 1; when `own_kill` is 0: result 0 (the game's own function then kills the
     character); otherwise game 0x11a6250(CH).
   - REG equals WANT and CHANGED: CH's method at table offset 0x4f8; D[3] plus 1.
10. When `own_kill` is non-zero: game 0x997230(`cm`). Result 1.

#### 4.15 `u3x_s16_refresh(base, r)`

`r` = the registers saved by the hook, as u64 words; word 4 is the hooked function's first argument (the
customisation manager it was called for). D[0] plus 1. Result 0 when eb[2] bit 25 is set or ec[1] bit 0 is clear.
Otherwise, when word 4 equals the address of CM (image offset 0x17f9938): result =
`u3x_s17_apply(base, CM, 0)`; else 0.

#### 4.16 `u3x_s16_tick(base)`

CM = image offset 0x17f9938; PAGE = `u3h_active_page(base)`, read once at the start.

1. Only when PAGE is `PAGE_BODY_TYPE`: D[8] plus 1; when the s32 at CM + 0xf74 differs from LAST_CURSOR:
   LAST_CURSOR = it, D[9] plus 1; D[10] = (u8 at CM + 0xfc8) + 0x100 · (u8 at CM + 0xf70) + 0x10000 · (the lowest
   byte of the scene state at image offset 0x17bba90) + 0x1000000 · (1 when `u3x_s16_char(base, CM)` is not null,
   else 0).
2. `u3x_h01_tick(base, CM)` (always).
3. eb[2] bit 26 set: return.
4. OPEN = `u3x_s17_page(PAGE)` is 1, and the "multiplayer active" byte is non-zero, and the scene state (u32 at
   0x17bba90) is 2, and the match state (u32 at 0x17d1868) is not 2.
5. **Not OPEN:** when PAGE_WAS_OPEN is non-zero: PAGE_WAS_OPEN = 0, and when ec[1] bit 0 is set and the scene
   state is 2 and CM is active (u8 at +0xf70 non-zero): `u3x_s17_apply(base, CM, 1)`. Return.
6. **OPEN:** PAGE_WAS_OPEN = 1. Then word D[11] (read once, written once):
   - no character (`u3x_s16_char(base, CM)` null): when D[13] is a multiple of 15 and byte 3 of the word is below
     255, byte 3 plus 1; bytes 0..2 kept;
   - a character and byte 3 non-zero: byte 2 = byte 3, byte 3 = 0, bytes 0..1 kept;
   - a character and byte 3 zero: unchanged.
7. CM not active: game 0x997670(CM); D[12] plus 1.
8. D[13] plus 1.
9. ec[1] bit 0 set: `u3x_s17_apply(base, CM, 1)`.

## 5. Test scenarios

Everything can be tested old against new on a fake image (a zeroed block standing for the game image with the
objects of section 3 built in it, `u3h_ext` returning a test block, recording stubs for the game functions and
virtual methods of 3.1 and for the `u3h_` functions of 2.2); compare memory, results and the order and arguments
of the calls. Nothing needs the live game for equivalence; the live game is needed only to see the effect (the
character changing clothes in the lobby, the kickback glow in a match).

**Part 1.**
- Scene step: switch bit set (nothing, eb[152] unchanged); menu off (+8 = 0) and menu switching off (+0xc = 1):
  leave, eb[28]; view holder live with type `TaskManagerOnline` on each of the two handles (eb[1] bit 8 clear /
  set), holder with another type, dead handle (id mismatch, null pointer, id 0): leave with eb[29] or not.
- No reason, CM inactive: camera, enter. CM active: popup pending with byte 0x17d7761 set (no `u3h_netgame_entry`
  call); popup pending, byte clear, net game type 11 / player 0 null / hunter / free; no popup with player 0 null;
  hunter with 0x996a60 answering 0, 1 and 0x100 (low byte zero = enter); non-hunter. Each with match state 2 and
  not 2 (eb[157], team pass, eb[155]).
- `u3x_scene_hunter`: multiplayer byte 0 (no `u3h_coop` call), not co-op, team 0 / 1 / 2.
- Want lookup: N = 0, negative, 0x100001; unaligned bucket array; empty bucket; match in first / third node; a
  node pointer that fails `ptr_ok`; match on the end node (0); a chain longer than 4096 nodes with the match
  behind (0).
- Module tick: multiplayer on from clean state with both modules unwanted (two want calls), already wanted by the
  game (no call, bit set), map unusable (ec[207], bit stays clear, retried next tick); second tick (nothing);
  multiplayer off with bits set and the module wanted / not wanted any more; switch bit set while bits are set
  (release); status word with loaded counts 0, 64, 65 and wanted -1.

**Part 2.**
- `u3x_s16_char`: null handle, id 0, unaligned handle, null object, unaligned object, id mismatch, live.
- `u3x_h01_loaded`: id 0, null answer, unaligned answer, state 7 / 8. `u3x_h01_hash`: null, value below 0x10000,
  empty text, a text whose id is the `none` id, another text.
- Re-dress: no character; non-player; side 2 and -1; index limit on with index 63 / 64 / negative; names: null,
  empty, loaded, not loaded (mask), part 10 not loaded; sub-object null / unaligned / good. Check the +0x104c
  byte is cleared on every path and bit 31 of D[14] only on the full path.
- Tick: each early return (switch, multiplayer off, match state 2, bit 31, inactive); re-dress asked followed by
  the compare in the same call; not ready at a, at b (entry 0 skipped), at c (part not in the list; `none` part
  and null part skipped); no difference on another page and on the colour page (look copy from side 0 and side 1,
  with and without a change); difference at part 0, at part 10, at two parts (only the first in D[5]).

**Part 3.**
- Page test: the nine ids, 0, another id.
- Apply: no character; non-player with D[11] low byte 0, 0xfe, 0xff; index out of range with the limit switch on
  / off and the table unusable (SLOT null, unaligned, TABLE null); side 2; look unchanged with own_kill 1
  (result 1, no further call) and 0 (continues); look changed; second table lookup unusable; rows pointer bad;
  registry: pointer bad, count 0, two entries for the same model (the higher index wins), entry with a null
  object; model differs with own_kill 0 / 1; model equal with and without a change.
- Refresh hook: switch combinations; word 4 equal / not equal to CM.
- Tick: main look page diagnostics (cursor change / none); eb[2] bit 26; OPEN from each of its four conditions
  failing; the leave edge with ec[1] bit 0 set / clear, scene state 2 / 1, CM active / not; D[11] byte handling
  with D[13] = 0, 14, 15 and byte 3 = 0, 1, 255, with and without a character; CM inactive (activation call).

## 6. Old behaviour that looks like a bug or dead code (to keep)

1. **Re-dress, parts 0..9:** the old code hashes each chosen name and would pass the empty string instead when the
   hash is the `none` id; but the hash helper already turns that id into 0, so the replacement never happens (a
   part named `none` is passed to the game as `none`). What remains observable is one call of the game's name id
   function per non-empty chosen name of parts 0..9, result unused (4.11 step 8). Part 10 has no such call.
2. **Module tick:** a module the game already wants is marked as the helper's without the helper taking a
   reference; at the end of multiplayer the helper then gives back a reference it never took (if the module is
   still wanted). Keep.
3. **Want lookup:** a chain of more than 4096 nodes answers "not wanted" (0), not "unusable" (-1).
4. **Re-dress tick:** after a re-dress asked by the game (step 5) the same call goes on to the compare and may
   re-dress a second time; bit 31 of D[14] set by the first one is not looked at until the next frame.
5. **Shared diagnostic words:** parts 2 and 3 count different things in D[1]..D[7]; D[14]'s low bits (part 3) and
   bit 31 (part 2) share a word, so 2^31 out-of-range applications would set the flag. Keep the layout.
6. **Apply with own_kill 0** stores the changed look into the character before it answers 0 and lets the game's
   function run. Intended (the old notes call it a deviation from PS3); keep.
7. **`u3h_scene_was` cell:** the scene step writes 0 on every leave; nothing reads it in v7.212. Keep the write.
8. **Name id function pointer** (image offset 0x1702720) is called without a null test in part 2 (other groups
   test it). Keep: do not add a test that changes the call sequence.
9. `u3x_s16_char` ignores its `base` argument; the registry lookup tests the entry's object only for null.
10. The comments of the old file call the tick's condition "in a running match"; what is tested is the opposite
    (multiplayer active, scene entered, match state **not** 2). The description in 4.16 is what the code does.

## 7. What stays as it is

- The C wrappers and exports (`k34_scene`, `k34_368b3c`, `k34_310a00`, `s16_char`, `s17_apply`, `s16_refresh`,
  `s_kind`, `s17_page`, `s16_tick`, `h01_loaded`, `h01_hash`, `h01_redress`, `h01_tick`, `u3h_s16_char`,
  `u3h_h01_tint`, `u3h_scene_was`, `u3h_s16_diag`): one-line forwards; the diagnostic block D and the
  `u3h_s19_count` writer are the C part's own.
- `u3x_lobby_scene_shutdown` (G39), `u3x_scene_character`, `u3x_scene_busy` (one field test / one virtual call,
  used only by C wrappers without callers): not delivered again.
- The hook placements (kind 16 tick order, kind 233 at image offset 0x995d50) and `u3x_loadout_tick` (G54), which
  fills the package list that 4.12 step 7 reads.

## 8. Open questions

1. The names behind the ids 0xbd3bb3b9 (character type table symbol) and 0x80e056d1 (the message of 4.11 step 10)
   are not recorded; they are used as plain ids like a function offset.
2. The name id function is called here with a third argument of 1, in G54 / G30 with 0. Both are what the old
   code does; whether the argument matters to the game is not known. Keep 1 here.
3. Whether a wrapper-only C function without callers (`k34_368b3c`, `k34_310a00`, `h01_*`, `s17_apply`,
   `s17_page`) should be removed is the integrator's decision; the `u3x_` names are delivered either way.

## Clarifications (tester, 2026-11-14, by test against v7.212)

1. The name ids that the new code computes from texts (`Player`, `TaskManagerOnline`, `none`,
   `anim-proto-multiplayer`, `part-multiplayer`, `part-obj-shade-char` and the nine page names of 3.2) are, under
   the rule of `u3h_crc_sid`, exactly the values the old code held as literals (all fifteen checked).
2. 4.7 step 3: the old status word joins MINE and the four parts with a bitwise or. Because only this tick writes
   ec[209] (bits 0 and 1 only, from a zero start), MINE never has other bits and "plus" gives the same word. Only an
   ec[209] with other bits set (not reachable in v7.212) would show a difference (e.g. ec[209] = 0x103 and module 0
   loaded: or gives 0x103, plus gives 0x203).
3. 4.6: a bucket count of exactly 0x100000 is usable, 0x100001 is not (both checked with a real array of that size).
   A chain whose match is node 4096 answers 1; node 4097 or later answers 0.
4. 4.3 b: only the low byte of 0x996a60's answer counts (answers 0x100 and 0xffffff00 let the scene be entered).
5. 4.11 / 4.12: with the name id function answering the real rule, a part named `none` is passed to 0xbc8630 as
   `none`, and the call count of the name id function in a re-dress is the number of non-empty chosen names of
   parts 0..9 (section 6, item 1, confirmed for old and new).
6. Section 8 questions 1 to 3 cannot be settled by this test (they need names that are not recorded, the game's
   meaning of an argument, or the integrator's choice); the old and new code agree on all of them as written.
