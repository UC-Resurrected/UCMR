# S02. Level and actor wants: the match setup's two branches, multi-all keeps and drops, the proto actor keep

Behaviour description for a reimplementation (reimpl2 pipeline, describer's document, final sweep group S02). It
says what the functions do, not how the old ones were written. Offsets are hexadecimal when prefixed with 0x,
decimal otherwise. "Image offset X" means the address `base + X`, where `base` is the game image base. All
multi-byte values are little-endian; "u8 / u16 / u32 / s32 / u64" are unsigned / signed integers of that width,
"pointer" is 8 bytes. A "plausible pointer" is exactly `ptr_ok` of `CONVENTIONS.h` (at least 0x10000, below 2^47,
a multiple of 8). There is no floating point in this group.

Names used for the same things in the related documents are kept: the level manager, its two want lists, a
request block, natives 40 / 41 / 42, the state object's native table and the switch word at `s` + 0x3d78 are in
`G39_mp_startup.md` (3.2, 3.4, 3.5 and its Clarification 1: the level manager has **two** lists, no third one);
`u3x_stage_tick` (G39 5.1) is the only caller of the keep and drop steps (through `u3h_multi_all`). The match
setup's taunt wants (hook kind 103) are `G54_taunt_kit_packages.md`; the lobby scene's use of the proto actor
package (`u3x_h01_loaded`) is `G59_lobby_scene.md`. The emblem manager's own NetInit is `G58_emblem_manager.md`
4.11. They are referred to, not repeated.

One part (about 75 old code lines of game logic; the rest of the sweep row's 236 lines is the hook dispatcher, which
stays, see 2.3).

## 1. Purpose and callers

PS3 1.19 kept three things wanted for a whole multiplayer session that PS4 lost:

- the level "multi-all" (PS3 wants it on the Connecting page as a level with the keep bit; NetShutdown drops it):
  it carries the sound bank of the kill chime, hit markers and similar sounds;
- the actors of the PS3 level group "multi-all" (among them the multiplayer weapon models, which the weapon mods
  page's 3D preview only spawns when they are loaded);
- the multiplayer animation actor package `anim-proto-multiplayer`, which the lobby menu scene waits for.

The game's match setup clears every want-list entry that lacks the keep bit (flag bit 1, value 2), so the helper
wants these packages itself and marks their entries with the keep bit; at NetShutdown it clears the bit and
removes them.

Two more pieces sit in the game's match setup (function at image offset 0x72b570) and are reached through the
hook dispatcher `u3_profile_event`:

- hook kind 43 (site 0x72d28b, right after the match setup has wanted `anim-npc-multiplayer`): PS3's match setup
  also wanted the actor package `anim-shade-mp` when the current net game's type is 8 (co-op Shade); PS4 dropped
  that branch, the helper puts it back;
- hook kind 10 (the match setup's call of the emblem setup, image offset 0x66b1d0, at site 0x72c17c, replaced by
  the hook): the emblem setup dereferences three data-module lookups without a test; the helper runs it only when
  all three resolve, and (switch) fills the emblem table first when it is still empty.

Callers, all on the game thread that runs the hooked code (no locking; nothing else writes what this group writes):

- `u3x_proto_keep`, `u3x_multi_all_actors_keep`, `u3x_multi_all_level_keep`: in this order, through the C wrappers
  `k38_keep`, `g01_keep`, `m01_keep`, from the C export `u3h_multi_all(base, s, keep)` with keep non-zero, which
  `u3x_stage_tick` calls on every stage tick while multiplayer (LAN or online) is active (G39 5.1 B step 2; also
  during a match).
- `u3x_proto_drop`, `u3x_multi_all_actors_drop`, `u3x_multi_all_level_drop`: in this order, through `k38_drop`,
  `g01_drop`, `m01_drop`, from `u3h_multi_all(base, s, 0)`, which the stage tick calls once when multiplayer has
  ended (G39 5.1 A step 1). `u3h_multi_all` itself stays as it is (it holds only the order).
- `u3x_actor_want_entry`: by the six functions above (old private `k38_entry`; no other caller).
- `u3x_actor_find`: by `u3x_multi_all_actors_keep` only (old private `i05_actor_find`, a diagnostic).
- `u3x_shade_anim_want`: by `u3_profile_event` for hook kind 43, only when `base` is not 0.
- `u3x_match_emblem_setup`: by `u3_profile_event` for hook kind 10 (no test of `base`).

## 2. Interface

### 2.1 Functions to deliver

All `extern "C"`, no float by value.

| New name | Signature | Result | Old function; what the C side becomes |
|---|---|---|---|
| find an actor want entry | `uint8_t *u3x_actor_want_entry(uintptr_t base, uint32_t sid)` | address of the entry or null | `k38_entry` (C): one-line wrapper (base, sid), or removed when nothing in C calls it any more (integrator) |
| proto keep | `void u3x_proto_keep(uintptr_t base, void *s)` | none | `k38_keep` (C): wrapper (base, s) |
| proto drop | `void u3x_proto_drop(uintptr_t base)` | none | `k38_drop` (C): wrapper (base) |
| find a loaded actor | `uint8_t *u3x_actor_find(uintptr_t base, uint32_t sid)` | the actor's slot or null | `i05_actor_find` (C): wrapper or removed (no C caller left) |
| multi-all actors keep | `void u3x_multi_all_actors_keep(uintptr_t base, void *s)` | none | `g01_keep` (C): wrapper (base, s) |
| multi-all actors drop | `void u3x_multi_all_actors_drop(uintptr_t base)` | none | `g01_drop` (C): wrapper (base) |
| multi-all level keep | `void u3x_multi_all_level_keep(uintptr_t base, void *s)` | none | `m01_keep` (C): wrapper (base, s) |
| multi-all level drop | `void u3x_multi_all_level_drop(uintptr_t base)` | none | `m01_drop` (C): wrapper (base) |
| shade anim want | `void u3x_shade_anim_want(uintptr_t base, void *s)` | none | the body of `u3_profile_event`'s kind 43 branch (2.3) |
| match emblem setup | `void u3x_match_emblem_setup(uintptr_t base, void *s)` | none | the emblem part of `u3_profile_event`'s kind 10 branch (2.3) |

`s` is the helper's state object (`ProfileState`, G39 3.4) as `void *`.

### 2.2 Functions of other groups, of the C part and of the shared tools that this group calls

Not described or reimplemented here. `ptr_ok`, `rd`, `at`, `u3h_ext`, `ext_eb` of `CONVENTIONS.h` are used as
given there.

| Name | Signature | What it does |
|---|---|---|
| `u3x_level_want_entry` | `uint8_t *u3x_level_want_entry(uintptr_t base, uint32_t sid)` | G39 4.1: the first entry of the **level** want list with this sid, or null (also null when that list's pointer is not plausible or its count is above 4096); reads only |
| `u3x_actor_want` | `int u3x_actor_want(uintptr_t base, void *s, const char *name)` | G39 4.2 with Clarification 1: wants an actor package without waiting (appends {hash, sid, 0, flags 1} to the actor want list); 1 = already in a want list, 2 = added, 0 = cannot |
| `u3x_level_state` | `uint32_t u3x_level_state(uintptr_t base, void *s, uint32_t sid)` | G39 2.4: low 8 bits of the level's state (8 = loaded) through native 42, 0xff without the level or without native 42 |
| `u3x_netgame_type` | `int32_t u3x_netgame_type(uintptr_t base, void *s)` | the existing one-liner (G40 2.4): the s32 at +0xc of the current net game entry, 0 when there is none |
| `u3h_stage_unwant` | `int u3h_stage_unwant(uintptr_t base, uint32_t sid)` | removes every entry with this sid from both want lists, closing the gaps; result = number of entries removed |
| `u3h_crc_sid` | `uint32_t u3h_crc_sid(const char *text)` | the game's 32-bit name id of a text (G39 2.4: CRC-32, polynomial 0x04C11DB7, MSB first, start 0, no final inversion) |
| `u3h_so_note` | `void u3h_so_note(const char *a, const char *name, int32_t v, const char *b)` | one line for the session log: a fixed prefix, `a` (when not null), `name` (when not null, at most 20 characters), `v` in decimal (only when v ≥ 0), `b` (when not null) |
| `u3h_multi_all_actor` | `const char *u3h_multi_all_actor(uint32_t i)` | **new C export (integrator), see 5 and open question 1**: the name of multi-all actor `i` (0-based), a NUL-terminated text, or null when `i` is past the end of the list |

### 2.3 What stays in the hook dispatcher `u3_profile_event` (u3_lan_profiles.c lines 9686-9888, v7.212)

Everything in that function stays as the helper's own dispatch, except the two pieces named here:

- **Moves out (kind 43):** the body of the kind 43 branch, lines 9761-9762 (the type read, the three sw counters
  and the conditional want). What stays at that place: "kind 43: when `base` is not 0, call
  `u3x_shade_anim_want(base, s)`; return 1 in every case". The comment block above it (lines 9698-9703) may move
  with it.
- **Moves out (kind 10):** lines 9795-9808 (natives 9, 10, 11 read, the early table fill, the three-id resolution
  test, the setup call or the skip counter). What stays: line 9786-9787 (`fr_match_players(base, s)` first, the
  players-met record of another group), line 9791 (`loadout_tick` when bit 1 of the u32 at `s` + 0x20c0 is set,
  G54), then **"call `u3x_match_emblem_setup(base, s)`"**, then line 9809 (return 1). The order fr_match_players,
  loadout_tick, emblem setup is kept.
- **Stays as own dispatch (no game logic):** every other line of 9686-9888: the null tests and the kind routing,
  `load(s)`, kind 15's material resource-slot guard (a helper safety net), the kind 0 / 1 / 2 / 6 / 8 / 9 / 11 / 12
  menu, page and profile create / select handling, and all calls into other groups.

## 3. Game and helper facts

### 3.1 The level manager (image offset 0x1afcf10), as far as this group uses it

| Offset | Type | Meaning |
|---|---|---|
| 0 | pointer | the manager's slot array: 0xd2 slots of 0x250 bytes each (levels and actors share it) |
| 8 | 4 × u64 | the slot bitmap: slot i is in use when bit (i mod 64) of the u64 at +8 + 8·(i div 64) is set |
| 0x28 / 0x30 | u32 / pointer | level want list: count / entries (G39 3.2) |
| 0x38 | u32 | actor want list: count (image offset 0x1afcf48) |
| 0x3c | u32 | actor want list: capacity (image offset 0x1afcf4c) |
| 0x40 | pointer | actor want list: entries (image offset 0x1afcf50) |

A want-list entry is 0x18 bytes (G39 3.2): u64 hash at 0, u32 sid at 8, u16 at 0xc, flags u8 at 0xe. **Keep bit** =
bit 1 (value 2) of the flags byte: the match setup's want-list clear (game 0x10ee740) keeps an entry that has it.

A slot (0x250 bytes): u16 at +0 = kind, 2 = actor (anything else is a level); u32 at +0x30 = state, 8 = loaded;
u32 at +0x74 = sid. The game's own "get actor" (0x10eb380) walks the slots the same way: in-use slots in increasing
order, the first whose sid matches, and it requires kind 2 (it asserts on a match of another kind; the helper must
not call it for that reason, see 4.4).

### 3.2 Other game data

| Image offset | Type | Meaning |
|---|---|---|
| 0x17d1868 | u32 | net game manager's match state; 2 = in a match (G39 3.1) |
| 0x17c3440 | u8 | the emblem state's first byte: the "dirty" byte; native 11 refills the emblem table only when it is set |
| 0x1b67e60 | 4 entries of 0x18 bytes | the emblem part table that native 11 fills; the u32 at +0xc of entry k (image offset 0x1b67e6c + 0x18·k) is the module id of entry k; 0 = not filled yet |
| 0x1b3af40 | object | the "no module" sentinel: native 9 answers its address when no loaded data module holds the id (G56 uses the same sentinel) |
| 0x66b756, 0x66b808, 0x66b8aa | u32 each | the three emblem module ids, see 3.5 |
| 0x145f915 | text | `anim-proto-multiplayer` (22 characters and NUL), as in G39 3.7 |

### 3.3 Natives used (pointers at `s` + 768 + 8·k; any may be null)

| k | Arguments | Result | Meaning |
|---|---|---|---|
| 9 | (u32 id, int 1) | pointer, or the sentinel address (3.2) | finds the loaded data module that holds the id (game 0x11fd4c0) |
| 10 | none | ignored | the emblem setup (game 0x66b1d0): builds the players' emblems from the three modules; takes no argument |
| 11 | (pointer: image offset 0x17c3440) | ignored | resets the emblem state and fills the emblem table at 0x1b67e60 (game 0x66b510; G39 3.5) |
| 40 | (pointer to a NUL-terminated name) | u64 | the 64-bit name hash of requests (G39 3.5) |
| 41 | (pointer: level manager, pointer to a 0x18-byte request block) | ignored | requests a level (G39 3.5) |

All plain calls with integer or pointer arguments, no float registers.

### 3.4 Helper state

The state object `s`:

| Offset | Type | Meaning |
|---|---|---|
| 768 + 8·k | pointer | native k (3.3) |
| 1896 (0x768) | u32 | emblem setups skipped by the kind 10 guard (counter) |
| 0x3d78 | u32 | second switch word (builder): bit 30 (0x40000000) = no game emblem init at NetInit (G39 3.4); here it switches the early table fill **on** |

The state block (`u3h_ext()`), offsets from the block start; the builder writes eb[1], maintainer tools read the
counters, so offsets and meanings must not move:

| Where | Meaning |
|---|---|
| eb[1] = u32 at +0x4054 (`ext_eb()[1]`) | switches: bit 4 (0x10) = proto keep off (`U3MP_PROTO_KEEP=0`); bit 26 (0x04000000) = multi-all actors limited to the weapon preview set (`U3MP_MULTI_ALL=2`); bit 27 (0x08000000) = all multi-all keeps and drops off (`U3MP_MULTI_ALL=0`) |
| eb[43] = u32 at +0x40fc | proto entries that got the keep bit |
| eb[44] = u32 at +0x4100 | proto wants added again |
| eb[45] = u32 at +0x4104 | proto entries removed at NetShutdown |
| sw[21] = u32 at +0x2184 | kind 43 calls |
| sw[22] = u32 at +0x2188 | `anim-shade-mp` wants added |
| sw[23] = u32 at +0x218c | the last net game type seen by kind 43 (as u32) |

### 3.5 Ids, names and where the game holds them

| What | Value | Where / rule |
|---|---|---|
| `anim-proto-multiplayer` | sid 0x67adb399 | name: the game's text at image offset 0x145f915; sid = `u3h_crc_sid` of it (G59 3.2 lists the same id) |
| `multi-all` (a level) | sid 0x240b27b8 = `u3h_crc_sid("multi-all")` | the PS4 executable holds no copy of this name (PS3-only text): one own text of the new code (`multi-all`, 9 characters) |
| `anim-shade-mp` (an actor package) | sid 0x5956007e = `u3h_crc_sid("anim-shade-mp")` (computed by `u3x_actor_want`) | not in the PS4 executable (PS4 dropped the branch that named it): one own text (`anim-shade-mp`, 13 characters) |
| net game type "co-op Shade" | 8 | the type value PS3's branch tests |
| emblem module ids | three u32 | **read at run time** from the game's own emblem table init (native 11's function), which loads them as 32-bit immediates of three "load constant into the first argument register" instructions: id 0 = u32 at image offset **0x66b756**, id 1 = u32 at **0x66b808**, id 2 = u32 at **0x66b8aa** (the instruction bytes start one byte earlier, 0xbf). Cross-check for the tester: they are 0xc7f8567c, 0xe2311588, 0x03ffae77, the values the old code held, and after a fill the table at 0x1b67e6c + 0x18·k holds the same three in entries 0, 1, 2. The new code keeps no copy |
| the weapon preview set | the first 12 names of the multi-all actor list | the helper's own choice for switch bit 26 |
| request flags of a level the game keeps for a whole session | 0x0a | as G39 3.2 ("what the game uses for its own menu level") |

### 3.6 The helper's own text

For the session log, once (no NUL inside, one trailing blank):

`multi-all level wanted (common-multi sound bank, kill chime), flags `

### 3.7 File-private variables of the new code (diagnostics; zero at start; nothing but maintainer tools reads them)

- MA_D: eight u32 (old `g01_d`): [0] actors wanted (answer 2), [1] keep bits found missing, [2] wants refused
  (answer 0), [3] NetShutdown removals, [4] list actors loaded in the last pass, [5] list size of the last pass,
  [6] sid of the first list actor not loaded in the last pass (0 = all loaded), [7] sid of the last non-actor slot
  that matched in `u3x_actor_find`.
- MA_X: eight u32 (old `g01_x`): [0] actor want list count, [1] its capacity, [2] the first not-loaded actor's slot
  as {state low 8 bits | kind << 8}, 0xffff = no slot, [3] its want entry as {flags | u16 at +0xc << 8 | index << 16},
  0xffffffff = no entry, [4] list actors with a want entry in this pass, [5] passes, [6] the proto entry's flags,
  0xff = none.
- MA_SID: up to 64 u32 and MA_N (u32): the sids of the multi-all actor list, filled once (4.5 step 2); MA_N = 0
  until then.
- ML_D: four u32 (old `m01_d`): [0] level requests made, [1] keep bits set on an existing entry, [2] NetShutdown
  removals, [3] {the entry's flags, 0xff = none} | {level state} << 8.

All U3X_BSS. The old names are only in symbol dumps; renaming is fine.

## 4. Behaviour

Rule: conditions are evaluated from left to right and stop as soon as the outcome is known; a function is called
again each time it is named.

### 4.1 `u3x_actor_want_entry(base, sid)`

Read the actor want list's count (image offset 0x1afcf48), then its entries pointer (0x1afcf50). Null when the
pointer is not plausible or the count is above 4096. Otherwise the address of the first entry, in list order,
whose u32 at +8 equals `sid`; null when there is none. Reads only, calls nothing. (Same rule as G39's
`u3x_level_want_entry`, on the other list.)

### 4.2 `u3x_proto_keep(base, s)`

1. eb[1] bit 4 set: nothing.
2. E = `u3x_actor_want_entry(base, 0x67adb399)`.
3. E null: R = `u3x_actor_want(base, s, text at image offset 0x145f915)`; R = 2: eb[44] plus 1. Then
   E = `u3x_actor_want_entry(base, 0x67adb399)` again.
4. E not null and its flags byte lacks bit 1: set bit 1 (flags |= 2), eb[43] plus 1.

### 4.3 `u3x_proto_drop(base)`

1. eb[1] bit 4 set: nothing.
2. E = `u3x_actor_want_entry(base, 0x67adb399)`; null: nothing more.
3. Clear bit 1 of E's flags byte; then eb[45] grows by `u3h_stage_unwant(base, 0x67adb399)`.

### 4.4 `u3x_actor_find(base, sid)`

The game's "get actor" walk without its assertion (diagnostic only).

1. A = the pointer at image offset 0x1afcf10 (the slot array). Not plausible: null.
2. For i = 0 to 0xd1 in increasing order: skip the slot unless its bit is set in the bitmap (3.1; the bitmap word
   is read for each i). Slot = A + 0x250·i. When its u32 at +0x74 equals `sid`: if its u16 at +0 is 2, the result
   is the slot (stop); otherwise MA_D[7] = sid and the walk goes on.
3. Null when no actor slot matched.

### 4.5 `u3x_multi_all_actors_keep(base, s)`

1. eb[1] bit 27 set: nothing.
2. When MA_N is 0: for i = 0, 1, … ask `u3h_multi_all_actor(i)`; stop at the first null or empty name, or after 64
   names; MA_SID[i] = `u3h_crc_sid(name i)` for each name taken; MA_N = the number taken. (Done once in the run;
   with an empty list it is tried again on every call.)
3. N = 12 when eb[1] bit 26 is set, else MA_N. (Open question 3 covers a list shorter than 12; never the case
   with the current list.)
4. LD = 0 (loaded), MISS = 0 (first not-loaded sid), PRES = 0. For i = 0 to N - 1, with S = MA_SID[i]:
   a. E = `u3x_actor_want_entry(base, S)`.
   b. When E is null and S is in neither want list (`u3x_level_want_entry(base, S)` null; the actor list was just
      searched): R = `u3x_actor_want(base, s, u3h_multi_all_actor(i))`; R = 2: MA_D[0] plus 1; R = 0: MA_D[2] plus 1
      (R = 1: nothing). Then E = `u3x_actor_want_entry(base, S)` again. (When E is null but S is in the level want
      list, nothing is wanted and E stays null.)
   c. E not null and its flags lack bit 1: MA_D[1] plus 1 (the bit itself is set in d).
   d. Read the actor want list's count and entries pointer again; when the pointer is plausible and the count at
      most 4096: set bit 1 in the flags of **every** entry whose sid is S (duplicates included), in list order.
   e. L = `u3x_actor_find(base, S)`. When L is plausible and its u32 at +0x30 is exactly 8: LD plus 1. Otherwise,
      only when MISS is still 0: MISS = S; MA_X[2] = L plausible ? (u32 at L + 0x30, low 8 bits) | (u16 at L + 0)
      << 8 : 0xffff; MA_X[3] = E not null ? E's flags | (u16 at E + 0xc) << 8 | ((E - the actor list's entries
      pointer, read now) / 0x18) << 16 : 0xffffffff. (E is the entry of step b, before step d; its flags are read
      after step d, so bit 1 is set when d ran.)
   f. E not null: PRES plus 1.
5. MA_D[4] = LD, MA_D[5] = N, MA_D[6] = MISS.
6. MA_X[0] = u32 at image offset 0x1afcf48, MA_X[1] = u32 at 0x1afcf4c, MA_X[4] = PRES, MA_X[5] plus 1.
7. MA_X[6] = the flags byte of `u3x_actor_want_entry(base, 0x67adb399)`, or 0xff when null.

### 4.6 `u3x_multi_all_actors_drop(base)`

1. eb[1] bit 27 set: nothing.
2. For i = 0 to MA_N - 1 (the whole remembered list, whatever bit 26 says; nothing when the keep has never filled
   MA_SID): E = `u3x_actor_want_entry(base, MA_SID[i])`; when E is not null and has bit 1: clear bit 1, then MA_D[3]
   grows by `u3h_stage_unwant(base, MA_SID[i])`.

### 4.7 `u3x_multi_all_level_keep(base, s)`

1. eb[1] bit 27 set: nothing.
2. E = `u3x_level_want_entry(base, 0x240b27b8)`.
3. When E is null and the match state (u32 at image offset 0x17d1868) is not 2:
   - H = native 40, Q = native 41 (both read before either is tested). Either null: **end of the function** (steps
     4 and 5 do not run).
   - Request block: 0x18 bytes, all zero; u64 at 0 = H(`multi-all` text, 3.5); u32 at 8 = 0x240b27b8; u8 at 0xe =
     0x0a. Q(level manager = image offset 0x1afcf10, the block). ML_D[0] plus 1.
   - E = `u3x_level_want_entry(base, 0x240b27b8)` again.
   - When ML_D[0] is now 1: `u3h_so_note(text of 3.6, null, E ? E's flags byte : 0xff, null)`.
4. E not null and its flags lack bit 1: set bit 1, ML_D[1] plus 1.
5. T = `u3x_level_state(base, s, 0x240b27b8)`; ML_D[3] = (E ? E's flags byte, read now : 0xff) | T << 8.

In a match with no entry, nothing is requested and ML_D[3] = 0xff | T << 8.

### 4.8 `u3x_multi_all_level_drop(base)`

1. eb[1] bit 27 set: nothing.
2. E = `u3x_level_want_entry(base, 0x240b27b8)`. When E is not null (with or without bit 1): clear bit 1 of its
   flags, then ML_D[2] grows by `u3h_stage_unwant(base, 0x240b27b8)`.

### 4.9 `u3x_shade_anim_want(base, s)` (hook kind 43, PS3's dropped match setup branch)

1. T = `u3x_netgame_type(base, s)`.
2. sw[21] plus 1; sw[23] = T (as u32).
3. When T is 8: R = `u3x_actor_want(base, s, "anim-shade-mp")`; R = 2: sw[22] plus 1.

The want has flags 1 (no keep bit): the package lives for this match only, as on PS3.

### 4.10 `u3x_match_emblem_setup(base, s)` (hook kind 10, replaces the match setup's call of the emblem setup)

1. F = native 9, U = native 10, I = native 11 (all three read first).
2. Early fill: when I is not null, and bit 30 of the u32 at `s` + 0x3d78 is set, and the u32 at image offset
   0x1b67e6c is 0: the byte at image offset 0x17c3440 = 1, then I(image offset 0x17c3440).
3. OK = F not null, and U not null, and the u32 at image offset 0x1b67e6c (read now, after step 2) is not 0.
4. When OK: for k = 0, 1, 2 (the ids of 3.5, in that order): when F(id k, 1) answers exactly the sentinel address
   (image offset 0x1b3af40), OK becomes false and no further id is asked. (A null answer counts as resolved.)
5. OK: U() (no argument). Otherwise: the u32 at `s` + 1896 plus 1.

The early fill is the PS3 order (table init before the setup) for builds that switch the NetInit emblem init off;
the game's own match setup fills the table only later (after this call).

## 5. Tables

- **Emblem module ids (3 values):** resolved; read from the game's code at 0x66b756 / 0x66b808 / 0x66b8aa (3.5).
- **Multi-all actor list (45 names, old literal `g01_names` in the C part; the first 12 are the weapon preview set:
  10 multiplayer gun actors and their two bases; the rest are the other actors of PS3's multi-all level group that
  PS4 ships, in PS3 dlc-patch18 pak23.txt order inside each part; `anim-proto-multiplayer` is left out because 4.2
  handles it, and three PS3 actors without a PS4 package are left out):** the PS4 executable does not hold this
  list (it has no multi-all group; only 4 of the 45 names occur in it, as unrelated texts), and no rule generates
  it. **Open question 1.** Until it is settled the new code takes the names only through `u3h_multi_all_actor(i)`
  (2.2); the C part may keep serving them from the existing literal list, so nothing of it is copied into the C++.
- Single names and ids (3.5) are interoperability facts used at one place each; `anim-proto-multiplayer` is read
  from the image; `multi-all` and `anim-shade-mp` are PS3-only texts (one own text each, like G39's default stage
  name).

## 6. Test scenarios

Everything can be tested without the game: a fake image in which the offsets of 3.1 and 3.2 are memory (the slot
array with a few slots, the bitmap, both want lists with room, the match state, the emblem table, the sentinel, the
three immediates at 0x66b756 / 0x66b808 / 0x66b8aa, the text at 0x145f915), a fake state object with recording
natives 9, 10, 11, 40, 41, and recording stand-ins for every function of 2.2 (the want and unwant stand-ins should
really change the fake lists, so that the second searches see the effect). Compare image, state object, state
block, private variables, and the order and arguments of the recorded calls. Only "the package really loads", the
weapon preview, the kill chime and the Shade animations need the live game.

1. Entry find: actor pointer null / odd / fine; count 0, 1, 4096, 4097; sid first, last, twice (first returned);
   sid only in the level list (null).
2. Proto keep: switch bit 4; entry present with flags 1 (becomes 3, eb[43]) and 3 (nothing); entry missing and the
   want answering 2 (eb[44], then the keep bit on the new entry), 1, 0 (no entry: nothing more).
3. Proto drop: switch; no entry (no unwant); entry with and without bit 1 (bit cleared, unwant called in both;
   eb[45] grows by the stand-in's answer).
4. Actor find: slot array pointer bad; bitmap empty; match in slot 0, 63, 64, 0xd1; a non-actor match before an
   actor match (MA_D[7] = sid, actor returned); only a non-actor match (null, MA_D[7]); a match in a slot whose
   bit is clear (ignored).
5. Actors keep: switch bit 27; first call fills MA_SID from the provider (stops at null, at an empty name, at 64);
   bit 26 (N = 12) and clear (N = MA_N); per name: entry present; missing and in the level list (no want); missing
   everywhere with want answers 2 / 1 / 0; duplicates in the actor list (all get bit 1, MA_D[1] counts once);
   actor list pointer bad in step d; loaded actor (state 8), state 0x108 (not loaded), slot of state 3; the first
   miss records MA_X[2] / [3] and later misses do not; MA_D[4..6], MA_X[0], [1], [4], [5], [6] after the pass.
6. Actors drop: switch; before any keep (nothing); entries with and without bit 1 (only those with it are cleared
   and removed).
7. Level keep: switch; entry present without and with bit 1; no entry outside a match (request block content,
   level manager address, ML_D[0], second search, log line only on the first request, with the entry's flags or
   0xff); no entry with native 40 or 41 null (end, ML_D[3] unchanged); no entry in a match (no request, ML_D[3] =
   0xff | T << 8); `u3x_level_state` answers 8 and 0xff.
8. Level drop: switch; no entry; entry with and without bit 1 (unwant in both cases).
9. Shade want: type 8 with want answers 2 / 1 / 0; type 0 and 9 (no want); sw[21], sw[23] always written.
10. Emblem setup: natives 9 / 10 / 11 null in turn; bit 30 set with table id 0 (dirty byte 1, then native 11, then
    the table re-read) and with id non-zero (no fill); bit 30 clear (no fill); table id still 0 after the fill (skip
    counter, no native 9 call); each of the three ids answering the sentinel (later ids not asked, skip counter); a
    null answer (resolved); all resolved (native 10 called once with no argument, counter unchanged); the ids asked
    are the image values, in order 0, 1, 2.

## 7. Oddities of the old code, to be kept

1. The actors keep wants a name only when it is in neither want list; a name that is in the level want list only
   gets no actor entry and no keep bit.
2. The actors keep counts a missing keep bit once per name (on the first entry) but sets it on every duplicate.
3. The actors drop works on the whole remembered list (even when bit 26 limited the keep) and does nothing before
   the first keep has filled the list; it removes only entries that still carry the keep bit, while the proto drop
   and the level drop remove their entry whether or not it has the bit.
4. `u3x_actor_find` returns only actor slots, so MA_X[2]'s kind byte is 2 whenever a slot was found; "no miss yet"
   is recognised by a recorded sid of 0.
5. The level keep ends early (no keep-bit step, no ML_D[3]) only when it would request but a native is missing.
6. The level keep logs only once per run (ML_D[0] = 1), with 0xff when the request did not produce an entry.
7. Kind 10: only the first table entry's id is tested for "filled"; a null answer of native 9 counts as resolved;
   `base` is not tested; the skip counter also counts a missing native.
8. Kind 43 counts and records the type on every call, also when no want follows.
9. The keeps run on every stage tick, also during a match (only the level keep tests the match state).

## 8. What stays as it is

- `u3_profile_event` except the two pieces of 2.3; `u3h_multi_all` (it holds only the order of the three keeps and
  drops); the C wrappers `k38_keep`, `k38_drop`, `g01_keep`, `g01_drop`, `m01_keep`, `m01_drop` (their callers
  stay); `fr_match_players` and `loadout_tick` (other groups).
- The block offsets and switch bits of 3.4 (the builder writes eb[1] and `s` + 0x3d78; maintainer tools read the
  counters).
- Every function of 2.2.

## 9. Open questions

1. **Multi-all actor list (45 names):** where should the names come from? The PS4 executable has no copy. Options:
   (a) the C part keeps its literal list behind the new export `u3h_multi_all_actor(i)` (no change of behaviour,
   but the list stays in the helper's source); (b) the PS4 "multi-all" level that 4.7 wants may list its actors in
   its package data, so the names could be read from the loaded level object at run time (not checked: needs a
   look at the level object's layout, and the actors would then only be known after the level has loaded); (c) the
   import writes the list into a file the helper reads. Needs the owner's decision; the spec works with any of
   them through the export.
2. `multi-all` and `anim-shade-mp` stay as two own texts (PS3-only names; the PS4 executable holds neither). If the
   owner wants them provided like G51's civilian package names (a C export), the interface is one more
   `u3h_...` text function.
3. With bit 26 set and a list shorter than 12 names, the old code goes on past the list's end with sid 0 (its zeroed
   sid store) and an empty name for the missing positions. Not reachable with the current 45-name list; the
   provider answers null there, so the new code would have no name. Flag only: the integrator may treat it as
   unreachable.

## 10. Clarifications (tester, 2026-11-14, confirmed by the differential test against v7.212)

1. **Open question 3 (switch bit 26 with fewer than 12 names) is settled:** for a position past the list the old code
   takes that position's remembered sid (MA_SID[i] as it stands: 0 from the start of the run, or whatever an earlier
   fill left there) and, when that sid is in neither want list, wants the **empty name** (a text of length 0). Giving
   `u3x_actor_want` an empty text when `u3h_multi_all_actor(i)` answers null does the same (tested with lists of 0, 3
   and 11 names and in random cases). The provider must answer null for **every** index past the end: if it answered
   an empty name at position j and real names after it, the fill would stop at j (as the old one does) but, with bit
   26, a later position would want the later real name where the old code wants the empty name.
2. **Empty or missing names:** the fill stops at the first null or empty name and after 64 names (a 70-name list
   gives MA_N = 64); an empty list leaves MA_N = 0 and is asked again on the next call. The new export is asked once
   per taken name plus once for the answer that ends the list, and once more for each want (two keeps of the full list
   with nothing ever added: 46 + 45 + 45 calls); `u3h_crc_sid` once per taken name. Neither has an effect.
3. **4.5 b:** asking only the level want list (`u3x_level_want_entry`) after the actor list search is exactly the old
   "in neither want list" test (the old code re-read both lists; the actor list part cannot differ, no call lies in
   between).
4. **MA_X[3]'s index:** E is always an entry of the list the re-read pointer points at (the last search of E and the
   re-read have no call between them), so the difference is never negative and unsigned arithmetic equals the old
   result. The u16 at E + 0xc and the flags are read after step d.
5. **4.5 d / e read order:** step d's count and pointer, step e's slot walk and the re-read of the entries pointer
   have no call between them; changing any of these fields at any call of a keep (one-change cases) gives the same
   result on both sides. A count of exactly 4096 is searched and gets the keep bits; 4097 makes every search null (all
   45 names wanted; with every want refused MA_D[2] = 45, MA_X[4] = 0).
6. **4.7:** natives 40 and 41 are both read before either is tested; with either one null nothing at all is called
   (no native, no log line, no `u3x_level_state`) and nothing is written.
7. **Request flags 0x0a already carry the keep bit** (0x0a = bits 1 and 3). After a request that produced the entry,
   step 4 changes nothing and ML_D[1] does not grow; the log line's number is 10 then (255 without an entry).
   ML_D[1] grows only for an entry made by someone else without the bit. The request block is, byte for byte:
   hash (8 bytes), sid b8 27 0b 24, 00 00, 0a, then nine zero bytes; the manager argument is image offset 0x1afcf10.
8. **4.10:** the three ids are asked in the order 0, 1, 2; the image values at 0x66b756 / 0x66b808 / 0x66b8aa of the
   PS4 executable (each after a 0xbf opcode byte) are 0xc7f8567c, 0xe2311588, 0x03ffae77, equal to the old values, so
   old and new ask the same. The sentinel test is an exact comparison with image offset 0x1b3af40 (an address 8 bytes
   later is "resolved"); a null answer is resolved; the first sentinel answer ends the asking. The table id at
   0x1b67e6c is read again after native 11. The text at 0x145f915 is `anim-proto-multiplayer` (22 characters, NUL)
   and its id is 0x67adb399.
9. **Section 7** holds for both sides: 1 (a name only in the level list gets no want, no entry, no keep bit, MA_X[4]
   does not count it), 2 (duplicates: every entry gets the bit, MA_D[1] once per name), 3 (the drop after a bit-26
   keep removes only the 12 entries that got the bit, over the whole remembered list; nothing before the first fill;
   the proto and level drops call the unwant with or without the bit), 4, 5, 6, 7 (also: a missing native 9 or 10
   counts a skip without any native call), 8, 9 (the proto and actors keeps do not read the match state).
