# G54. Taunt and kit packages: the lobby's package list, taunt package wants

Behaviour description for a reimplementation (reimpl2 pipeline, describer's document). It says what the functions
do, not how the old ones were written. Offsets are hexadecimal when prefixed with 0x, decimal otherwise. "Image
offset X" means the address `base + X`, where `base` is the game image base. All multi-byte values are
little-endian; "u8 / u16 / u32 / s32 / u64" are unsigned / signed integers of that width, "pointer" is 8 bytes.

**There is no floating point in this group** (no x87 and no single-precision instruction in the old build of these
functions); PIPELINE.md's Float rule has nothing to apply to.

Related documents, referred to and not repeated: `G39_mp_startup.md` (the level manager at image offset 0x1afcf10,
its want lists and the 0x18-byte request block: 3.2 with clarification 11.1; the state object `s`, its natives and
switch word: 3.4, 3.5; `u3h_crc_sid`: 2), `G31_mp_page_events.md` (the content flags at image offset 0x17c3a40:
3). The player-info taunt of a net player and `u3x_np_taunt_want` belong to G53.

The group is written as **two independent parts**. Neither part calls the other.

| Part | Subject | Exported names to deliver |
|---|---|---|
| 1 | The taunt package wants of a match and the "previous taunt" store | `u3x_taunt_want`, `u3x_taunt_start_record`, `u3x_match_taunt_wants` |
| 2 | The lobby's package list (kit models, optional taunt packages), content flags, diagnostics | `u3x_pkg_taunt_name`, `u3x_pkg_taunt_slot`, `u3x_pkg_taunt_tick`, `u3x_loadout_tick` |

## 1. Purpose and callers

PS3 1.19 loads the animation package of a taunt before a match (one actor package per taunt, named like the taunt's
animation) and keeps the kit models of the lobby characters requested every frontend frame. The PS4 game has
neither: its match setup wants no taunt package, and nothing fills the package list of its player-customisation
manager. The helper supplies both.

**Part 1.**

- `u3x_taunt_want(base, id)`: wants the actor package of taunt `id`. Called by `u3x_match_taunt_wants`, and through
  the C wrapper `taunt_want119` and its export `u3h_taunt_want(base, id)` by G53's
  `void u3x_np_taunt_want(uintptr_t base, uint8_t *np)` (hook kinds 109 / 110: the taunt of a net player's player
  info).
- `u3x_taunt_start_record(base, controller)`: remembers, per taunt controller, the taunt that was current before
  a new one starts. Called through the C wrapper `taunt_start_rec119` by the hook dispatcher `u3_profile_event`
  for hook kind 104, only when the image base and the argument are not null. The hook sits in the game's taunt
  Start function (image offset 0xb85010) just before it stores the new taunt id (at 0xb85126), so the controller
  still holds the old id.
- `u3x_match_taunt_wants(base)`: wants the local player's current taunt and, in one case, the previous one.
  Called through the C wrapper `match_taunt_wants119` by `u3_profile_event` for hook kind 103, only when the image
  base is not null. The hook sits in the game's match setup (function at image offset 0x72b570, at 0x72d12d: right
  after it wants its multiplayer animation package).

**Part 2.**

- `u3x_loadout_tick(base, s)`: the package list tick. Called through the C wrapper `loadout_tick(base, s)` and its
  export `u3h_loadout_tick(base, s)`:
  from `u3_profile_event` for hook kind 3 (a menu data-source event of the startup pages) when the base is known
  and the session is LAN or online; for hook kind 10 (match start) only when bit 1 of the switch word is set; and
  from the player-list sources (the C `onm_players` and three C++ list sources of other groups) on their "last row
  index" request, i.e. on every draw of a page that lists players.
- `u3x_pkg_taunt_tick(base, s, m)`: the taunt part of the tick. Called by `u3x_loadout_tick` only.
- `u3x_pkg_taunt_slot(m, slot, name)`: one owner's entry in the list. Called by `u3x_pkg_taunt_tick` only.
- `u3x_pkg_taunt_name(sid, out)`: the package name of one of the newer taunts. Called by `u3x_pkg_taunt_tick` only.
  (The C part declares the last three; nothing in C calls them.)

Everything runs on the game thread that executes the hooked code (menu callbacks, match setup, the player's
update). There is no locking; nothing else touches what the group writes.

## 2. Interface

### 2.1 Functions to deliver (all `extern "C"`)

| Function | Signature | Result | C wrapper passes |
|---|---|---|---|
| want a taunt's package | `void u3x_taunt_want(uintptr_t base, uint32_t id)` | - | `taunt_want119(base, id)`: both arguments |
| record a taunt start | `void u3x_taunt_start_record(uintptr_t base, uint8_t *controller)` | - | `taunt_start_rec119(base, tc)`: both |
| match setup wants | `void u3x_match_taunt_wants(uintptr_t base)` | - | `match_taunt_wants119(base)`: base |
| taunt package name | `const char *u3x_pkg_taunt_name(uint32_t sid, char out[64])` | `out`, or null | (no wrapper) |
| one owner's list entry | `int u3x_pkg_taunt_slot(uint8_t *m, uint32_t *slot, const char *name)` | 1 = the list changed, 0 = not | (no wrapper) |
| taunt part of the tick | `int u3x_pkg_taunt_tick(uintptr_t base, void *s, uint8_t *m)` | 1 = the list changed, 0 = not | (no wrapper) |
| package list tick | `void u3x_loadout_tick(uintptr_t base, void *s)` | - | `loadout_tick(base, s)`: both (exists already) |

The first three are new names; the three C functions become one-line wrappers. The last four keep their names and
signatures.

### 2.2 Functions of the C part and of other groups that are called

| Name | Signature | What it does |
|---|---|---|
| `u3h_ext` | `uint8_t *u3h_ext(void)` | the helper's state block (CONVENTIONS.h) |
| `u3h_crc_sid` | `uint32_t u3h_crc_sid(const char *text)` | the game's 32-bit name id of a NUL-terminated text (G39 2) |
| `u3h_mp_code_is` | `int u3h_mp_code_is(uintptr_t base, uint32_t off, const char *hex12)` | 1 when the 12 bytes at image offset `off` equal the bytes written as 24 lower-case hexadecimal digits in `hex12`, else 0. Reads only |
| `u3h_s19_count` | `void u3h_s19_count(int what, uint32_t v)` | counters of another group's diagnostics: what 0 = one list entry dropped, 1 = one part refused because the list is full, 2 = `v` entries are in use |
| `u3h_taunt_row_name` (to add) | `const char *u3h_taunt_row_name(uint32_t row)` | name of row `row` of the taunt name list (3.6), null when `row` is past its end |
| `u3h_content_packs` (to add) | `const uint8_t *u3h_content_packs(uint32_t *count)` | the content pack numbers the helper marks as owned (3.7); stores their number in `*count` |

The last two do not exist yet; the integrator adds them (see 3.6, 3.7 and the open questions for where their data
comes from).

### 2.3 Functions that go away

The old C++ file has three exported helpers that only the three C functions of part 1 used, and that are in no
work group (strays, covered here): `u3x_nth_name(list, index)` (the n-th name of a list of NUL-terminated names),
`u3x_taunt_slot(slots, controller, player, fallback)` and `u3x_taunt_previous(slots, controller, player)` (the
slot choice and the lookup of the previous-taunt store, 4.2 / 4.3). With part 1 the store lives in the new C++
code, so the C array `taunt_prev119` and these three functions are removed (`u3x_nth_name` stays only as long as
the C function behind `u3h_taunt_row_name` needs it). The file-private pieces of part 2 (`list_put`, the list
accessors, the actor finder, the plausibility test) are part of the new code and described in section 4.

## 3. Game facts

### 3.1 Game functions

| Image offset | Arguments | Result | Meaning |
|---|---|---|---|
| 0x11f31b0 | (u32 symbol id, u32 module id, u32 0) | pointer to the symbol's data, or null | script symbol lookup in a module |
| 0x10e95b0 | (pointer to a NUL-terminated name) | u64 | the 64-bit name hash of package requests (G39's native 40) |
| 0x10ebb90 | (pointer: level manager = image offset 0x1afcf10, pointer to a 0x18-byte request block) | - | wants an actor package |
| pointer stored at image offset 0x1702720 | (u64 0, pointer to a NUL-terminated name, u64 0) | u32 | the game's 32-bit name id function. The pointer is read from the image each time; it may be null |
| native 49 = pointer at `s` + 768 + 8·49 (game function 0x996db0) | (pointer: package manager, 3.3) | - | makes the level manager want every package of the manager's list |
| native 50 = pointer at `s` + 768 + 8·50 (game function 0x10eb380) | (pointer: level manager, u32 sid) | pointer to the actor package object, or null | finds an actor package |
| entry 6 of a taunt controller's function table | (pointer: controller) | u32 | the controller's current taunt id (the game's function at 0xb85000 returns the u32 at controller + 0x128) |

All are plain calls with integer or pointer arguments. Part 1 calls the first three at their image offsets only
after checking their first 12 code bytes with `u3h_mp_code_is`:

| Image offset | First 12 bytes |
|---|---|
| 0x11f31b0 | 55 48 89 e5 41 56 53 48 83 ec 10 49 |
| 0x10e95b0 | 55 48 89 e5 41 57 41 56 41 55 41 54 |
| 0x10ebb90 | 55 48 89 e5 41 57 41 56 53 50 49 89 |

### 3.2 Ids and constants

| Value | Meaning |
|---|---|
| 0xb2b6e512 | script symbol of the taunt table |
| 0x4240ef2e | the net module (`u3h_crc_sid("net")`) |
| 0x0e69839d | the "no taunt" id (`u3h_crc_sid("multi-celebrate-none")`, the name of row 0) |
| 0xf114b39d | the "no part" name id (`u3h_crc_sid("none")`) |
| 12 | state of an actor package that failed to load (8 = loaded, G39 3.2) |
| 3 | value of a profile object's load state when the profile is loaded |

### 3.3 Game data

| Image offset | Type | Meaning |
|---|---|---|
| 0x1afcf0a | u8 | a flag byte just in front of the level manager; the tick sets it to 1 (its meaning is not recorded) |
| 0x1afcf10 | object | the level manager (G39 3.2) |
| 0x17c3a40 + n | u8 | content flag of pack number n: non-zero = the player owns that content (G31 3) |
| 0x17cb3b0 | s32 | the match manager's saved random seed (manager at 0x17cb060, field +0x350); the game expects -1 when no synced random sequence is open |
| 0x17dd610 | pointer | the array of the local users' multiplayer profile objects (0x7b00 bytes each), or null |
| 0x17f9938 | object | the player-customisation manager, called "package manager" M below |
| 0x1bb9500 | u64 | handle of the player of screen 0: a pointer to a pointer to the player object, or 0 |
| 0x1bb9508 | u32 | that player's process id, or 0 |

The **taunt table** (answer of 0x11f31b0 for the ids above): u32 row count at +0; pointer at +8 to the rows; a row
is 12 bytes and its first u32 is the taunt id, which is the name id of the taunt's animation name. Row 0 is "no
taunt".

**Package manager M** (image offset 0x17f9938):

| Offset | Type | Meaning |
|---|---|---|
| 4 + 4·i, i = 0..49 | u32 | package list: name id of entry i, 0 = free |
| 0xcc + 0x40·i, i = 0..49 | 64 bytes of text | package list: name of entry i, NUL-terminated |
| 0xe00 + 0x58·p + 8·k, p = 0..1, k = 0..10 | pointer | wanted kit part k of local player p: a NUL-terminated model name, or null |
| 0xf70 | u8 | non-zero = the manager is active (the lobby shows the characters) |

**Profile object** of local user p (array pointer + p·0x7b00): u32 at +0x784 = the equipped taunt id; u32 at
+0x7ae8 = load state.

**Player object**: u32 at +0x7c = process id; pointer at +0x6e8 = its taunt controller, or null; u8 at +0xcea =
non-zero when the controller works for the menu's lobby character.

**Taunt controller**: pointer at +0 = its function table (pointers of 8 bytes; entry 6 see 3.1); pointer at +0x118
= its owner (a player object), or null; u32 at +0x128 = the current taunt id.

**Actor package object** (answer of native 50): u32 at +0x30 = state; u32 at +0x78 = number of load requests;
pointers at +0x80 + 8·q = the requests (a request's u32 at +0x20 is its status; the pointer may be null); pointer
at +0x110 = a definition record, or null (u16 at +0x14 and u16 at +0x18 of it are only printed).

**Request block** for 0x10ebb90: 0x18 bytes as in G39 3.2: u64 name hash at +0, u32 name id at +8, u16 0 at +0xc,
flags u8 at +0xe = 1 (an actor package wanted in the foreground), everything else zero.

### 3.4 The helper's state object `s`

| Offset | Type | Meaning |
|---|---|---|
| 768 + 8·k | pointer | native k (3.1: k = 49 and 50; either may be null) |
| 0x20c0 | u32 | switch word: bit 1 (value 2) = keep requesting the list during a match; bit 7 (value 0x80) = do not set content flags |
| 0x3e40 | 48 u32 | D[0..47]: [0] ticks, [1] ticks with a changed list, [2] requests made, [3] entries in use, [4..47] diagnostics (4.7 steps 11, 12) |

### 3.5 The helper's state block (`u3h_ext()`)

| Where | Meaning |
|---|---|
| TS = sixteen u32 at block + 0xb80 | [2k], [2k+1] for k = 0, 1, 2: owner slot k = {list index + 1, name id}, {0, 0} = owns nothing. Owners 0 and 1 are the equipped taunts of local users 0 and 1; owner 2 is a retired source that is only released. [8] entries added for owners; [9] taunt ids that match no name; [10] the last such id; [11] failed packages dropped; [12..15] ids of packages that failed to load (0 = unused); [6], [7] unused |
| eb[2] = `ext_eb()[2]`, bit 28 (0x10000000) | set = do not drop unowned list entries |
| ec[k] = u32 at block + 0x4450 + 4·k | [0] switches: bit 3 (value 8) = match taunt wants off, bit 4 (value 16) = taunt packages in the lobby list on (off by default); [30] seed corrections; [31] runs of the match setup hook; [32] packages wanted; [33] last id wanted; [34] matched rows without a usable name; [35] taunt starts recorded; [36] the last recorded previous id |

The **previous-taunt store** is part 1's own zero-initialised memory: 8 records of {pointer controller, u32 player
id, u32 previous taunt id}. A record with a null controller is free. Records are never released.

### 3.6 Table: the taunt names (not in the PS4 game)

The old code holds two literal name lists: the 44 animation names of the taunt table's rows 0..43 (C, used by the
want), and the 29 names of rows 15..43 without their common beginning `multi-celebrate-` (C++, used by the lobby
list). They are the same names: the second list is rows 15 to 43 of the first, in the same order. Rows 1..14 are
the taunts whose animations are in the game's multiplayer animation package; rows 15..43 are the taunts PS3 1.19
added, each with its own actor package of the same name.

The running PS4 game does **not** hold these texts: its converted taunt table (3.3) holds only the name ids, and
the texts are neither in the PS4 image nor in its converted `net.bin` (checked: no text beginning with
`multi-celebrate` in either; the PS3 patch-18 `net.bin` holds all 44). There is no formula that produces them. A
name is needed as text because both the request block (0x10e95b0) and the manager's list request (native 49) hash
the text. **Open question 1** (section 9). The new code therefore uses **one** source for both uses,
`u3h_taunt_row_name(row)` (2.2), and holds no name itself; the second list is not delivered again. A name returned
by it is the full name (with the common beginning), shorter than 64 characters.

### 3.7 Table: the content pack numbers (not in the PS4 game)

A list of 73 pack numbers (u8, all between 15 and 124, no order relied on, no duplicates) of the downloadable
content that the import puts on disk. The PS4 game only reads the content flags (3.3); it has no table of pack
numbers and never writes the flags (checked: every reference to the flag array in the PS4 image is a read). There
is no known rule that generates the list. **Open question 2** (section 9). The new code gets the list from
`u3h_content_packs(&count)` (2.2) and holds no number itself.

## 4. Behaviour

"Plausible pointer" in this group means a value of at least 0x10000 and below 2^47, **without** an alignment
condition (this is not CONVENTIONS.h's `ptr_ok`). Where the text says "not null", only null is tested.

### 4.1 `u3x_taunt_want(base, id)` (part 1)

1. `id` equal to the "no taunt" id: nothing happens.
2. If any of the three code checks of 3.1 fails (`u3h_mp_code_is`; checked in the order of the table, stopping at
   the first failure): nothing else happens.
3. T = game 0x11f31b0(0xb2b6e512, 0x4240ef2e, 0). Null: nothing else.
4. Read the row count and the rows pointer of T. Look at rows 1, 2, ... count - 1 in order (row 0 is never looked
   at; a count of 0 or 1 means no row) for the **first** row whose id equals `id`. None: nothing else. The steps
   below run for that row only; whatever their outcome, no later row is looked at.
5. H = the pointer stored at image offset 0x1702720; N = `u3h_taunt_row_name(row index)`. If N is null, or H is
   null, or H(0, N, 0) differs from `id`: ec[34] plus 1, end. (H is not called when N or H is null.)
6. Build a request block (3.3) on the stack: hash = game 0x10e95b0(N); name id = H(0, N, 0), **called a second
   time**, after the hash call; flags byte = 1; the rest zero. Then game 0x10ebb90(level manager, block).
7. ec[32] plus 1; ec[33] = `id`.

Order of calls on the full path: 0x11f31b0, H, 0x10e95b0, H, 0x10ebb90.

### 4.2 `u3x_taunt_start_record(base, controller)` (part 1)

`base` is not used. Null controller: nothing. Otherwise:

1. Owner = pointer at controller + 0x118; P = the owner's process id (u32 at +0x7c), or 0 when the owner is null.
2. Choose a record of the store: the first whose controller and player id equal (controller, P); else the first
   free one; else the record with index (ec[35] and 7), using ec[35] as it is before step 4.
3. Write the record: controller, P, previous = the u32 at controller + 0x128.
4. ec[35] plus 1; ec[36] = that previous id.

### 4.3 `u3x_match_taunt_wants(base)` (part 1)

1. ec[31] plus 1. If ec[0] bit 3 is set: end.
2. Read the handle (u64 at 0x1bb9500) and the process id (u32 at 0x1bb9508). Either zero: end.
3. PL = the pointer the handle points to. Null, or its process id (u32 at +0x7c) differs from the one read: end.
4. C = PL's taunt controller (pointer at +0x6e8). Null: end.
5. Call entry 6 of C's function table with C; `u3x_taunt_want(base, result)`.
6. Then read PL's u8 at +0xcea (after step 5's calls). Non-zero: look up the store for (C, P) with P computed as in
   4.2 step 1; the previous id is that record's, or 0 when there is none; `u3x_taunt_want(base, previous id)`
   (also when it is 0: 4.1 then runs its checks and finds no row).

### 4.4 `u3x_pkg_taunt_name(sid, out)` (part 2)

1. Null when `sid` is 0 or the "no taunt" id. Null when one of TS[12..15] equals `sid`. `out` is not written in
   these cases.
2. For row = 15, 16, ... while `u3h_taunt_row_name(row)` is not null: copy the name into `out` (at most 63
   characters, then a NUL); if `u3h_crc_sid(out)` equals `sid`: result `out`. One `u3h_crc_sid` call per name, in
   row order, none after the match.
3. No name matches: TS[9] plus 1, TS[10] = `sid`, result null (`out` then holds the last name tried).

### 4.5 `u3x_pkg_taunt_slot(m, slot, name)` (part 2)

`m` is the package manager; `slot` points to an owner slot {index + 1, name id} (two u32); `name` may be null.
"Writing entry i" means: copy the name (at most 63 characters, then a NUL) into the list's name i and store the name
id in the list's id i.

1. S = `u3h_crc_sid(name)`, or 0 when `name` is null (an empty name also gives 0).
2. The owner **owns** entry e = slot[0] - 1 when slot[0] is between 1 and 50 and the list's id e equals slot[1];
   otherwise it owns nothing. (This also holds for slot[1] = 0 with a free entry e.)
3. If it owns an entry and slot[1] equals S: result 0, nothing is written.
4. If S is not 0: go through the list's ids i = 0..49 in order and stop at the first entry, other than the owned
   one, whose id is S ("listed already"). While going, remember the first entry whose id is 0 ("free"); entries
   after the stop point are not looked at. If S is 0 there is no search: nothing is listed, nothing is free.
5. If the owner owns an entry: set its id to 0 and the first byte of its name to 0; the list has changed; the
   owned entry becomes the free one when no free one was remembered or its index is lower.
6. slot = {0, 0}.
7. If S is 0, or S is listed already, or there is no free entry: result = whether the list changed in step 5.
8. Otherwise write the free entry (name, S); slot = {its index + 1, S}; TS[8] plus 1; result 1.

### 4.6 `u3x_pkg_taunt_tick(base, s, m)` (part 2)

The result is 1 when any `u3x_pkg_taunt_slot` call below answered 1, else 0.

1. F = native 50.
2. If ec[0] bit 4 is clear: `u3x_pkg_taunt_slot(m, owner slot k, null)` for k = 0, 1, 2, and end.
3. If F is not null: for k = 0, 1, 2 whose slot's name id is not 0: A = F(level manager, that id). If A is a
   plausible pointer and its state (u32 at +0x30) is 12: store the id in the first of TS[12], TS[13], TS[14] that
   is 0, or in TS[15] when none is; TS[11] plus 1; `u3x_pkg_taunt_slot(m, owner slot k, null)`.
4. PR = the pointer at image offset 0x17dd610 (read once, here). For p = 0, then 1: the name is null unless PR is
   a plausible pointer and the load state of profile object p is 3, in which case it is
   `u3x_pkg_taunt_name(equipped taunt id of profile p, a local 64-byte buffer)`. Then
   `u3x_pkg_taunt_slot(m, owner slot p, name)`.
5. `u3x_pkg_taunt_slot(m, owner slot 2, null)`.

### 4.7 `u3x_loadout_tick(base, s)` (part 2)

M = the package manager, D = the words at `s` + 0x3e40, R = native 49, F = native 50. "A part name" of (p, k) is
the pointer at M + 0xe00 + 0x58·p + 8·k; it counts as present when it is not null and its first character is not
0. Parts are always visited p = 0 then 1, and k = 0..10 inside.

1. R null: nothing happens at all.
2. Set the u8 at image offset 0x1afcf0a to 1.
3. If the s32 at image offset 0x17cb3b0 is 0: set it to -1 and ec[30] plus 1.
4. If bit 7 of the switch word is clear: for every pack number n of `u3h_content_packs`, set the u8 at image
   offset 0x17c3a40 + n to 1.
5. D[0] plus 1.
6. L = whether M's active byte (+0xf70) is non-zero, read once here and used for steps 7 and 8.
7. **Dropping unowned entries**, only when L and eb[2] bit 28 is clear. Collect the name ids
   (`u3h_crc_sid`) of all present part names, leaving out those equal to the "no part" id (at most 22). Then for
   every list entry i = 0..49 whose id V is not 0: it is kept when V is among the collected ids, or when one of
   the three owner slots is exactly {i + 1, V}; otherwise its id is set to 0, the first byte of its name to 0, the
   list has changed, and `u3h_s19_count(0, 0)` is called (once per dropped entry).
8. **Parts in**, only when L. For every present part name: Sp = `u3h_crc_sid(name)` (computed again); skip it when
   Sp is the "no part" id; search the list's ids 0..49 for Sp, remembering the first free entry met before it;
   found: skip; not found and no free entry: `u3h_s19_count(1, 0)`, skip; otherwise write the free entry (name,
   Sp) as in 4.5 and the list has changed.
9. If M's active byte, read again, is non-zero: `u3x_pkg_taunt_tick(base, s, M)`; a non-zero answer means the list
   has changed.
10. U = number of list ids that are not 0. D[3] = U. `u3h_s19_count(2, U)`.
11. **Request.** When the list has changed, or when U is not 0 and (D[0] and 63) is 0 and (M's active byte, read
    again, is non-zero or bit 1 of the switch word is set): if the list has changed D[1] plus 1; call R(M);
    D[2] plus 1.
12. **Per-part diagnostics**, only when F is not null. For every (p, k), with j = 11·p + k and the pair
    E0 = D[4 + 2j], E1 = D[5 + 2j]: if M's active byte (read again for every part, i.e. after R's call) is
    non-zero and the part name is present, E0 = `u3h_crc_sid(name)` (the "no part" id is not left out here);
    otherwise E0 keeps its old value. A = F(level manager, E0) when E0 is not 0, else null (F is not called).
    With st = A's state or 0xff when A is null, n = A's request count or 0 when A is null, and a 16-bit value q
    that starts as 0xffff and whose byte 0 / byte 1 is replaced, for request 0 / request 1 when n is above that
    index, by the low byte of that request's status (0xfe when the request pointer is null):
    E1 = (st and 0xff) + q·0x100 + (n and 0xff)·0x1000000. A is only tested against null here.
13. **First failed package**, only when F is not null. Go through the first 20 pairs (j = 0..19) and take the
    first whose E1 has the low byte 12: A = F(level manager, its E0). If A is null: stop, nothing is written.
    Otherwise with n = A's request count, build 16 status nibbles: nibble i (i below n and below 16) is the low 4
    bits of request i's status, 15 when the request pointer is null, 0 for i not below n. D[44] = that E0;
    D[45] = nibbles 0..7 (nibble 0 lowest); D[46] = nibbles 8..15 (nibble 8 lowest) combined by OR with n shifted
    left 24; D[47] = 0 when A's definition pointer (+0x110) is null, else 0x80000000 OR its u16 at +0x14 OR its
    u16 at +0x18 shifted left 16. Only the first such pair is reported per tick.

## 5. Test scenarios

Part 1 can be tested completely with a fake image (the code bytes of 3.1, a taunt table, a function pointer at
0x1702720, a player with a controller and a function table) and call-recording stubs for the three game functions
and `u3h_taunt_row_name`; compare memory, ec words, and the order and arguments of the calls.

1. Want: the "no taunt" id (no call at all); each code check failing in turn; table null; count 0 and 1; id only in
   row 0 (not found); id in two rows (first wins); name null (ec[34]); function pointer null (ec[34], no call);
   name id differs (ec[34], one H call); full path (five calls in order, block bytes, ec[32], ec[33]).
2. Start record: null controller; owner null (player id 0); new record in the first free slot; same controller and
   player again (same record updated); same controller with another player id (new record); a full store (record
   ec[35] and 7 overwritten, for several ec[35] values); ec[35], ec[36].
3. Match wants: ec[0] bit 3; handle 0; id 0; null player; id mismatch; no controller; flag at +0xcea clear (one
   want); set with a record (two wants, the second with the recorded id); set without a record (second want with
   id 0: checks and table lookup, no request).

Part 2 can be tested the same way: a fake manager, profile array and state block, recording stubs for natives 49
and 50, `u3h_crc_sid` (the real function is public: G39 2), `u3h_s19_count`, `u3h_taunt_row_name` and
`u3h_content_packs`; compare the manager, D, TS, ec, the content flags and the calls.

4. Name: sid 0, the "no taunt" id, a sid in TS[12..15] (out untouched); each of the names; an unknown sid (TS[9],
   TS[10], out holds the last name).
5. Slot: name null with and without an owned entry; same name as owned (0, nothing written); new name with a free
   entry before / after the owned one; the name already listed by someone else before / after the first free
   entry; full list; slot index 0, 51, and an index whose list id differs from slot[1]; slot {k, 0} on a free
   entry; an empty name; a name of 63 and of more than 63 characters.
6. Taunt tick: ec[0] bit 4 clear (three releases); F null; a failed package for each owner with TS[12..15] empty,
   partly and completely filled; an implausible answer of F; profile array null / implausible; load state 3 and
   not 3 for each user; both users with the same taunt (one entry, the second owner owns nothing).
7. Tick: R null (nothing); seed 0 and non-zero; switch bit 7 set / clear; inactive manager (no drop, no parts, no
   taunt tick; request only by the 64-tick rule with switch bit 1); active manager with parts for both players,
   null, empty and "none" names, duplicate names; stale entries dropped / kept by an owner slot / kept with eb[2]
   bit 28; a full list (refused parts counted); D[0] at multiples of 64 with and without entries; a stub R that
   clears the active byte (step 12 then keeps the old E0 values); F null; packages with 0, 1, 2 and more
   requests, null request pointers; a failed package among pairs 0..19 with 0, 8, 16 and 20 requests, with and
   without a definition; a failed package only in pair 20 or 21 (not reported); F answering null in step 13.

Only the live game shows that a wanted taunt package loads and the taunt plays, and that the lobby characters get
their kit models.

## 6. Oddities to keep (the new code does 100% of what the old does)

1. 4.1 calls the name id function twice for the same name; a matched row whose name fails ends the search.
2. 4.1 wants a package for rows 1..14 too (taunts whose animations are in the multiplayer animation package).
3. 4.3 step 6 wants "taunt 0" when nothing was recorded.
4. The previous-taunt store never frees a record; when it is full a record is overwritten by a counter's low bits.
5. 4.5 step 2 counts a slot {k, 0} on a free entry as owning it (then "changes" the list when a name comes).
6. 4.6: a fourth and later failed package overwrite TS[15]; TS[12..15] are never cleared, so such a taunt is not
   requested again until the helper's state is reset. Owner 2 is checked for a failed package although nothing
   fills it any more.
7. 4.7 steps 2 to 4 run on every tick, also outside the lobby; the seed correction is a counted safety net.
8. 4.7 computes each part's name id up to three times per tick (steps 7, 8, 12).
9. 4.7 step 12 does not leave out the "no part" id and keeps stale ids, so F is asked about them; steps 12 and 13
   share D[44..47]: step 13's report overwrites the pairs of parts (1, 9) and (1, 10), step 13 looks only at the
   first 20 pairs, and on the next tick step 12 may ask F about a reported id left in D[44] or D[46].
10. The plausibility test of this group has no alignment condition, and 4.7 tests F's answers only against null.

## 7. What must stay as it is

- The C wrappers and exports (`taunt_want119`, `taunt_start_rec119`, `match_taunt_wants119`, `loadout_tick`,
  `u3h_taunt_want`, `u3h_loadout_tick`) and the dispatcher's conditions for kinds 3, 10, 103, 104: other groups
  and the hooks call them.
- `u3h_s19_count`, `u3h_crc_sid`, `u3h_mp_code_is`: C part helpers of other groups.
- The positions of D, TS and the ec words: the C part asserts them and the diagnostics of other groups print them.

## 8. Strays

Covered here, in no work group of the inventory: `u3x_nth_name`, `u3x_taunt_slot`, `u3x_taunt_previous` (2.3; to be
removed), and the C helpers `taunt_row_name119` and `taunt_prev_get119` (folded into 4.1 / 4.3). File-private
pieces of part 2 with logic: `list_put` (in the group), the plausibility test and the actor finder (described in
section 4).

## 9. Open questions

1. **Taunt names (3.6).** 44 texts that only the PS3 patch data holds. Until a run-time source exists they stay in
   one place in the C part behind `u3h_taunt_row_name`. Possible sources that need no literal list: the import
   keeps the name texts when it converts the player's own patch `net.bin` (for example as an extra table the helper
   reads through the script symbol lookup), or the helper lists the imported taunt package files and takes their
   names. To decide by the lead.
2. **Content pack numbers (3.7).** 73 numbers that state which packs the import ships; the game has no such
   table. Until decided they stay in one place behind `u3h_content_packs`. A possible source: the import writes
   the pack numbers it installed into a file the helper reads. Not verified: whether the list equals "every pack
   number the game's tables refer to" (playlist and level records +0x2c, unlock conditions of type 6); if it does,
   that rule could replace the list.
3. The meaning of the byte at image offset 0x1afcf0a is not recorded in the old code (only "set to 1 each tick").

## 10. Clarifications (tester, 2026-11-14)

Settled by the differential test `reimpl2\G54\test_g54.cpp` (old v7.212 against part_1.cpp / part_2.cpp).

1. 3.1: the first 12 code bytes listed for 0x11f31b0, 0x10e95b0 and 0x10ebb90 equal the PS4 image (eboot file
   offset = image offset + 0x4000). Each of the 36 bytes made wrong in turn stops 4.1 at that check.
2. 4.6 step 3, read order: the old code reads the owner slot's name id again after F's call when it stores it into
   TS[12..15]; the new code stores the id it read before the call. They differ only if F wrote the helper's state
   block, which a game function does not do. Accepted as equal behaviour.
3. 4.7 step 13, read order: likewise the old code reads E0 again after F's call when it writes D[44]; the new code
   uses the value read before. Differs only if F wrote the state object's D words. Accepted.
4. 4.5 step 3 also applies to a slot {k, 0} owning a free entry when the name is null or empty (or has name id 0):
   S = 0 equals slot[1], so the result is 0 and the slot is kept. Oddity 5's "change" happens only when a name with a
   non-zero id comes.
5. 4.7 step 13 tests only the low byte of E1: an actor state such as 0x10c counts as "failed" (12).
6. `u3h_content_packs`: the count starts at 0; a null list or a count of 0 sets no flag, also when the export does
   not store a count. It is asked once per tick while switch bit 7 is clear, never while it is set.
7. `u3h_taunt_row_name`: a shorter list makes 4.1 count ec[34] for a matched row past its end, and 4.4 try only the
   rows that exist (TS[9] / TS[10] when none matches; with no row from 15 on, `out` is not written). A name longer
   than 63 characters is compared on its first 63 characters (`u3h_crc_sid` of the cut copy).
8. Open questions 1 to 3 are not decidable by test and stay with the lead.
