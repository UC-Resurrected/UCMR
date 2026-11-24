# G45. Medal conditions (the checks a kill runs over the kill history)

Behaviour description for a reimplementation (reimpl2 pipeline, describer's document). It says what the functions
do, not how the old ones were written. All offsets are hexadecimal when prefixed with 0x. "Image offset X" means the
address `base + X`, where `base` is the game image base. All multi-byte values are little-endian; "u32 / s32 / u16 /
s16 / u8 / s8 / u64" are unsigned / signed integers of that width, "pointer" is 8 bytes. A "float pattern" is the
32-bit pattern of a single-precision float, carried in a u32. Names follow `G28_medals_records.md` (medals, player
records) and `G22_rank_xp.md`; the kill record, the kill message and the functions of G44 (kill rewards) and G46
(kill record: history, K/D statistics, medal dispatch) are named here only as far as this group reads them.

## 1. Purpose and callers

PS3 1.19 runs, for every kill in multiplayer, a set of checks over the history of kills ("this player killed three
enemies within the chain window", "the kill came from far away", "it ended an enemy's streak", ...). Each check that
holds reports a **stat event** (an event id, an amount and a player) to the game's stat system, which turns events
into medals, mission progress and statistics. The PS4 game has the stat system and the medal data but lost the
function that makes the checks. The helper's kill record (G46) restores it; this group is the thirteen checks, plus
one small predicate only they use.

All thirteen are called by G46's `kill_statistics`, `kill_aftermath` and `streaks` (C++), in this order, each only
when G46's "medals" switch is on, after record 0 of the kill history has been filled with the current kill:

| Order | Function | G46 calls it when |
|---|---|---|
| 1 | `u3x_kr_379c18` | the kill counts and its victim is not "uncounted" |
| 2 | `u3x_kr_37952c` | as 1, directly after it |
| 3 | `u3x_kr_3817b8` | the victim is not "uncounted" |
| 4 | `u3x_kr_37992c` | as 3, and the kill's weapon is of weapon group 6 |
| 5 to 9 | `u3x_kr_37ab00`, `u3x_kr_37a4bc`, `u3x_kr_379ed8`, `u3x_kr_379ff0`, `u3x_kr_379430` | as 3, in this order |
| 10 | `u3x_kr_37a32c` | as 3, and the victim is a net player |
| 11, 12 | `u3x_kr_3791ec`, `u3x_kr_37a914` | as 10 |
| 13 | `u3x_kr_37a16c` | as 10 (after G44's `u3x_three_of_each`) |

The C file `kr_port.inc` also holds thirteen one-line C wrappers (`kr_379c18` ... `kr_37a32c`) that pass their
arguments on unchanged; they stay. `u3x_kr_3590b0` is called only by `u3x_kr_37a914`.

Everything runs on the game thread that handles the kill message. No locking, no timing rule, no state of its own:
every function is a pure reader of game memory except for the stat events it sends, the game lookups it makes and
one byte of a player record (4.3).

## 2. Interface

All functions are `extern "C"`, exist today under these names and keep name and signature (the callers stay). None
takes or returns a float.

| Function | Signature | What it reports |
|---|---|---|
| steep kill | `void u3x_kr_379c18(uintptr_t base, void *s, uint8_t *knp, uint8_t *info)` | victim steeply above / below the killer |
| deaths before this kill | `void u3x_kr_37952c(uintptr_t base, void *s, uint8_t *S, uint8_t *knp)` | the killer's deaths since its previous kill |
| explosive multi-kill | `void u3x_kr_3817b8(uintptr_t base, void *s, uint8_t *S, uint8_t *knp, uint8_t *info, int team_mode)` | kills by the same explosive within one second |
| group-6 weapon counts | `void u3x_kr_37992c(uintptr_t base, void *s, uint8_t *S, uint8_t *knp, uint8_t *info)` | kills with this weapon, with group 6, and the group-6-only run |
| chains | `void u3x_kr_37ab00(uintptr_t base, void *s, uint8_t *S, uint8_t *knp, uint8_t *info)` | kills chained within a time window (two chains) |
| long range | `void u3x_kr_37a4bc(uintptr_t base, void *s, uint8_t *S, uint8_t *knp)` | number of long-range kills |
| objective chain | `void u3x_kr_379ed8(uintptr_t base, void *s, uint8_t *S)` | run of kills with the same objective value, to a whole team |
| least-killed enemy | `void u3x_kr_379ff0(uintptr_t base, void *s, uint8_t *S, uint8_t *knp)` | the smallest number of kills on any opponent |
| flagged kills | `void u3x_kr_379430(uintptr_t base, void *s, uint8_t *S, uint8_t *knp)` | number of the killer's kills with record flag +0x21 |
| trade kills | `void u3x_kr_3791ec(uintptr_t base, void *s, uint8_t *S, uint8_t *knp, uint8_t *vnp, uint8_t *info)` | a kill made after the killer's own death; a kill by the victim's explosive at the same moment |
| all opponents down | `void u3x_kr_37a914(uintptr_t base, void *s, uint8_t *S, uint8_t *knp)` | every opponent is down, each last killed by this player |
| streak ended | `void u3x_kr_37a16c(uintptr_t base, void *s, uint8_t *S, uint8_t *knp)` | the victim had a run of kills |
| revenge | `void u3x_kr_37a32c(uintptr_t base, void *s, uint8_t *S, uint8_t *knp, uint8_t *info)` | the victim was the killer's last killer |
| player is up | `int u3x_kr_3590b0(uintptr_t base, uint8_t *np)` | predicate of 4.14 (a stray, see 7) |

Parameters:

- `base`: game image base. `s`: the helper's state object, opaque here: it is only passed on to `u3h_kr_stat`.
- `S`: the score manager (image 0x17dd780; G46 passes it), see 3.2. Never null.
- `knp`: the killer's net player (3.3). Never null. `vnp`: the victim's net player; never null where G46 calls
  `u3x_kr_3791ec`. It is only passed on to `u3h_kr_stat`.
- `info`: the game's kill message (3.4). Never null.
- `team_mode`: G46's mode flag (non-zero in the modes in which G46 also handles a co-operative AI victim). Only
  zero / non-zero matters.
- `np` of `u3x_kr_3590b0`: a net player, never null.

Two independent parts, should two implementers work at once (they share only the facts of section 3 and the small
readers of 3.5, which each part writes for itself): **part A** = `u3x_kr_379c18`, `u3x_kr_37952c`, `u3x_kr_3817b8`,
`u3x_kr_37992c`, `u3x_kr_37ab00`, `u3x_kr_37a4bc` (weapons and float maths, about 125 old lines); **part B** =
`u3x_kr_379ed8`, `u3x_kr_379ff0`, `u3x_kr_379430`, `u3x_kr_3791ec`, `u3x_kr_37a914`, `u3x_kr_37a16c`,
`u3x_kr_37a32c`, `u3x_kr_3590b0` (players and teams, about 125 old lines).

### Functions the new code calls (declare them; do not reimplement them)

| Name | Signature | What it does |
|---|---|---|
| `u3h_kr_stat` | `void u3h_kr_stat(uintptr_t base, void *s, uint32_t id, uint32_t amount, uint8_t *np)` | C export (G44): reports stat event `id` with `amount` for net player `np` to the game's stat system (it also counts the event in the helper's diagnostics and obeys the helper's "no stat events" switch). "Report (id, amount, player)" below always means one call of it |
| `u3h_kr_prec` | `uint8_t *u3h_kr_prec(uint8_t *S, uint8_t *np)` | C export (G44): the player record (0x130 bytes) of net player `np` in the score manager, or the helper's spare record when the player is not in the member table; never null |
| `u3x_net_player_id` | `uint32_t u3x_net_player_id(const uint8_t *np)` | existing: the id of a net player (from its fields +0x1e4, +0x1e8, +0x1ec); pure |
| `u3x_net_player_in_game` | `int u3x_net_player_in_game(const uint8_t *np)` | existing: non-zero when the player slot has joined and is not leaving (bytes +0xe1, +0x5e4); pure |
| `u3x_weapon_for_stat` | `uint32_t u3x_weapon_for_stat(const uint32_t *fixed, const uint32_t *extra, uint32_t number)` | existing: weapon id of weapon stat number `number`: `fixed[number]` below 0x13, else from the extra weapon list (0 when `extra` is null or the number is past its end); pure |
| `u3x_weapon_stat_number` | `uint32_t u3x_weapon_stat_number(const uint32_t *fixed, const uint32_t *extra, uint32_t weapon)` | existing: the weapon stat number of a weapon id (0 when unknown; `extra` may be null); pure |
| `u3x_kr_weapon_group` | `int u3x_kr_weapon_group(uintptr_t base, uint32_t weapon)` | G46 (4.15): 6, 4 or 0 for a weapon id (two small weapon classes the kill rewards treat apart); pure. See 3.7 (c) |

The shared tools of CONVENTIONS.h used here: `game_call`, `at`, `rd`, `f_add`, `f_sub`, `f_mul`, `f_div`, `f_root`,
`f_from_int`, `f_less`. "a ≤ b" on float patterns is not in CONVENTIONS.h: it is true when neither value is a NaN
and b < a is false.

## 3. Game facts

### 3.1 Game functions

| Image offset | Arguments → result | Meaning |
|---|---|---|
| 0x721050 | (pointer net game manager) → pointer or null | the current mode entry; s32 at +0xc of it = the mode type |
| 0x11f2bf0 | (u32 symbol id) → pointer or null | script symbol lookup: the result points to a pointer (u64) to the symbol's data |
| 0x793280 | (pointer net player, u32 id, u32 0) → u8 (low byte of the result) | whether the player has the effect / state `id`. Used with id 0xc054e78b only |

None of them uses float registers. The net game manager is the object at image 0x17cb060.

**Mode type**: one call of 0x721050 with the net game manager; the s32 at +0xc of the result, or −1 for a null
result. Type 0xe is the mode without teams ("free for all" below). Every evaluation is one game call; where this
document says "the mode type is read", exactly one call is made at that point.

**Symbol data** of id X: one call of 0x11f2bf0 with X; null when the result is null or the pointer it points to is
null, otherwise that pointer. Every "lookup of X" below is exactly one call, made at the point stated.

### 3.2 The score manager and the kill history

`S` (image 0x17dd780):

| Offset | Type | Meaning |
|---|---|---|
| 0xe0 | u32 | n = number of kill records (at most 0x200) |
| 0xe4 + 0x28·i | record | kill record i; record 0 is the current kill, higher numbers are older |

A kill record (0x28 bytes; written by G46):

| Offset | Type | Name here | Meaning |
|---|---|---|---|
| 0 | u8 | type | 6 = explosive kill (others not used here) |
| 2 | u16 | killer | low 16 bits of the killer's player id (0 = none) |
| 4 | u16 | victim | low 16 bits of the victim's id (0 = none) |
| 6 | s8 | killer team | −1 = none |
| 7 | s8 | victim team | |
| 8 | s32 | time | game clock ticks, 300 per second |
| 0xc | u8 | weapon number | the weapon stat number of the kill's weapon |
| 0xe | 3 × s16 | killer position | x, y, z in sixteenths of a unit |
| 0x14 | 3 × s16 | victim position | same |
| 0x1c | s32 | objective value | 0 = none; otherwise a stat event id (4.7) |
| 0x21 | u8 | flag A | set by G46 (its "avenged betrayal" marking; doubles the kill's points there) |
| 0x22 | u8 | flag B | set by G46 (a marked victim; used in 4.12 only) |
| 0x25 | u8 | uncounted | non-zero: the victim does not count for statistics |

"An enemy kill" = a record whose killer team and victim team differ. "The team pair" of a record = the u16 at +6
(both team bytes together).

A player record (from `u3h_kr_prec`): only the u8 at +0x52, the player's best explosive multi-kill, is used (4.3).

### 3.3 Net players

The net game manager holds ten net player slots: slot i (0 to 9) is at image 0x17cb060 + 0x390 + 0x9c0·i.

| Offset | Type | Meaning |
|---|---|---|
| 0x10 | handle | the player's character: pointer to a pointer at +0x10 (u64), u32 id at +0x18 |
| 0x21c | s32 | team |
| 0x39c | s32 | a game clock time: taken as the player's last death |
| 0x3a0 | s32 | a game clock time: taken as the player's last spawn (a player with spawn < death is down) |
| 0x770 | u8 | a flag (4.14) |

**A valid handle**: the pointer at +0x10 is not null, the id at +0x18 is not 0, the pointer it points to is not
null, and the u32 at +0x7c of that object equals the id.

**The u8 at image 0x17d5c61** ("slots flag"): while it is 0, "slot i" below yields no player (null).

### 3.4 The kill message `info`

| Offset | Type | Meaning |
|---|---|---|
| 0 | s32 | time of the kill (game clock ticks) |
| 0x10 | 3 float patterns | killer position x, y, z |
| 0x20 | 3 float patterns | victim position x, y, z |
| 0x30 | u32 | killer's player id |
| 0x34 | u32 | victim's id |
| 0x38 | u32 | weapon id |
| 0x60 | u32 | flags; bit 0x20 = explosive kill |

### 3.5 Small readers used throughout (part of the new code)

- **id(np)** = `u3x_net_player_id(np)`. Where a record's killer or victim is compared with a player id, the id is
  cut to its low 16 bits first, **except in 4.4**, where the full 32-bit id is compared with the 16-bit field.
- **slot(i)**: null when the slots flag is 0 or i > 9, else the address of slot i.
- **in-game count**: the number of the ten slots for which `u3x_net_player_in_game` is non-zero (the slots flag is
  not looked at).
- **slot of id v**: −1 when v is 0; else the lowest i in 0 .. 9 whose slot's id (full 32 bits) equals v (neither
  the slots flag nor "in game" is looked at); −1 when none. (The inventory lists the old helper for this under
  G44; nothing else uses it: it belongs here.)
- **friendly(a, b)**: true when a and b are the same pointer (no game call). Otherwise the mode type is read; type
  0xe gives false; else true exactly when both are non-null and their teams are equal. (With b null the result is
  false, after the mode type read.)
- **extra weapon list**: symbol data of 0x9e354807 (one lookup per use): u32 count at +0, pointer at +8 to that
  many u32 weapon ids. It may be null.
- **weapon of number k**: `u3x_weapon_for_stat(base + 0x13a6170, extra, k)` where `extra` is null without any
  lookup when k < 0x13, and the extra weapon list (one lookup) otherwise.
- **number of weapon w**: `u3x_weapon_stat_number(base + 0x13a6170, extra weapon list, w)`: always one lookup.
- **seconds(t)** of an s32 t: `f_mul(f_from_int(t), 0x3b5a740e)` (0x3b5a740e = 1/300).
- **squared distance of a record**: with kx, ky, kz the killer position and vx, vy, vz the victim position of the
  record (s16), d_c = `f_mul(f_sub(f_from_int(k_c), f_from_int(v_c)), 0x3d800000)` (0x3d800000 = 1/16) for
  c = x, y, z; the result is ((0 + d_x·d_x) + d_y·d_y) + d_z·d_z, every operation one single-precision operation,
  starting from the pattern 0. (The inventory lists the old helper under G44; only 4.6 uses it.)

### 3.6 Constants

| Pattern | Value | Meaning |
|---|---|---|
| 0x40a00000 / 0xc0a00000 | 5.0 / −5.0 | least height difference of a steep kill |
| 0x3f5db3d7 / 0xbf5db3d7 | 0.8660254 / −0.8660254 | least vertical share of the direction (sine of 60 degrees) |
| 0x3f800000 | 1.0 | explosive multi-kill window, seconds |
| 0x3b5a740e | 1/300 | clock ticks to seconds |
| 0x3d800000 | 1/16 | record positions to units |

Tuning values the game's script data holds (read at run time by symbol lookup, never copied; the first value of
each symbol's data):

| Symbol | Type | Meaning |
|---|---|---|
| 0x47efb8e4 | float pattern | chain window, seconds (4.5, first chain) |
| 0xee602459 | float pattern | chain window of the two special weapons, seconds (4.5, second chain) |
| 0x42b7081b | float pattern | long range, units (4.6) |
| 0xa84641cc | s32 | least number of opponents for 4.11 |
| 0xdd9356ec | u32 | least run of kills for 4.12 |

### 3.7 Ids, and where the running game holds them

**(a) Stat event ids.** Each check reports under its own event id (a string id of the game's script data: the
medal, mission and statistics definitions listen for it). They are single ids, one per condition, given where the
behaviour uses them (section 4): 0x427bfc52, 0x8a14a6a4 (4.1), 0x5852e7ed (4.2), 0x5bc68f9c, 0x2de62476, 0x55917637
(4.3), 0xd2b04848, 0x7fdbcbbf (4.4), 0xf2c76c63, 0x4bae48b4 (4.5), 0xa8932480 (4.6), 0x610ae399 (4.8), 0x8d70ef29
(4.9), 0x9d715fda (4.10), 0x4a9da8a5 (4.11), 0xf69e35a3 (4.12), 0x6623a3f4 (4.13). The PS4 image contains none of
these seventeen 32-bit patterns (checked over the whole image file), and their names are not known, so neither an
image read nor the name rule of G23 3.9 can produce them: see open question 1.

**(b) Weapon ids.** The checks name eleven weapons. The new code holds none of their ids: it reads them from the
running game image, which holds every one of them (checked against the unpatched PS4 image; the old code already
reads both tables at run time; that the builder leaves the four bytes at 0xc478bb alone is for the tester to
confirm on a built image):

- Table F = image 0x13a6170: the game's fixed weapon table, 0x13 u32 entries; entry k is the weapon of weapon stat
  number k.
- Table G = image 0x13a66d0: the game's second weapon table, u32 entries (entries 1 to 0x1a are weapons).
- One u32 at image 0xc478bb (unaligned): the immediate of a compare instruction in a game function that treats
  that weapon and the weapon F[13] alike.

| Name here | Where | Used in |
|---|---|---|
| E1 | F[10] | 4.3: an explosive that reports nothing in team mode |
| E2 | G[17] | 4.3: an explosive with its own event in team mode |
| E3 | F[5] (the string id of "propane") | 4.3: as E2 |
| Q0 | F[1] | 4.4: its id is also the event id of the group count |
| Q1, Q2, Q3, Q4 | F[6], F[7], F[8], F[9] | 4.4: each one's id is also its own event id |
| Q5 | F[13] | 4.4: reports under 0x7fdbcbbf |
| Q5b | u32 at image 0xc478bb | 4.4: as Q5 |
| C1, C2 | G[14], G[15] | 4.5: the two special weapons of the second chain |

All are 32-bit reads at call time (the one at 0xc478bb is unaligned); whether a value is read on every call or
kept after the first is not observable.

**(c) Weapon groups.** `u3x_kr_weapon_group(base, weapon)` (G46 4.15; formerly the old `u3x_weapon_group`) still
holds a literal list of ten weapon ids (inventory section C, "weapon group ids"). This group only calls it. For
whoever replaces it: group 6 is exactly {F[1], F[6], F[7], F[8], F[9], F[13]} (= Q0 to Q5 above) and group 4 is
exactly {G[20], G[21], G[22], G[23]}; the function has no `base` parameter today.

**(d) Other ids**: mode type 0xe (3.1); effect id 0xc054e78b (3.1, 4.14); symbol ids of 3.5 and 3.6.

## 4. Behaviour

Notation: n = the u32 at `S` + 0xe0, read when a loop starts its test (the functions do not change it); r(i) =
kill record i; r0 = record 0. "For i = a .. n − 1" visits the records in rising order and makes no step when a ≥ n.
Nothing checks n against 0x200 or n ≥ 1: r0 is read as it is even when n is 0. "Report (id, amount, player)" = one
`u3h_kr_stat(base, s, id, amount, player)`. All float steps are single-precision operations on patterns, one
rounding each, in the order written; comparisons with a NaN are false. The old code already computed this way (no
extended precision anywhere), so results are expected to match bit for bit.

### 4.1 `u3x_kr_379c18`: steep kill

From `info`: dx, dy, dz = victim position minus killer position, per coordinate (`f_sub(victim, killer)`).

1. Unless 5.0 ≤ dy or dy ≤ −5.0: return (also for a NaN dy).
2. len = `f_root`((dz·dz + dx·dx) + dy·dy), in exactly this grouping; share = `f_div(dy, len)`.
3. If 0.8660254 < share: report (0x427bfc52, 1, `knp`) (the victim is steeply above the killer). Otherwise, if
   share < −0.8660254: report (0x8a14a6a4, 1, `knp`) (steeply below). Otherwise nothing.

No game call, no other read.

### 4.2 `u3x_kr_37952c`: deaths before this kill

me = id(`knp`). count = 0. For i = 1 .. n − 1: when r(i)'s killer equals me (16 bits), stop; otherwise when its
victim equals me, count += 1. (Neither "uncounted" nor the teams are looked at.) Then, always: report (0x5852e7ed,
count, `knp`).

### 4.3 `u3x_kr_3817b8`: explosive multi-kill

1. When bit 0x20 of the message flags is clear: return (nothing else happens).
2. w = message weapon id, k = message killer id, t = message time. count = 0. For i = 0 .. n − 1 (record 0
   included): the record takes part when it is not uncounted, its killer equals k (16 bits), it is an enemy kill,
   and |seconds(r(i).time − t)| < 1.0 (the difference is a wrapping 32-bit subtraction; the absolute value is the
   pattern with its sign bit cleared). Only for a record that takes part, the weapon of its weapon number is
   looked up (3.5; so the extra weapon list is looked up once for each such record with a number of 0x13 or
   more), and count += 1 when that weapon equals w.
3. amount = count & 0xff. The event id is 0x5bc68f9c. When `team_mode` is non-zero: for w = E1 nothing is
   reported; for w = E2 the event id is 0x2de62476; for w = E3 it is 0x55917637. Unless suppressed: report (event
   id, amount, `knp`).
4. Always (also when step 3 reported nothing): rec = `u3h_kr_prec(S, knp)`; when amount is greater than the u8 at
   rec + 0x52, that byte becomes amount.

### 4.4 `u3x_kr_37992c`: group-6 weapon counts

1. When r0 is not an enemy kill: return.
2. k = message killer id as a full u32 (a record's 16-bit killer or victim equals it only when k < 0x10000 and
   the values match), w = message weapon id.
3. First pass, i = 0 .. n − 1: skip the record when it is uncounted, its killer is not k, or it is not an enemy
   kill. Otherwise x = the weapon of its weapon number (3.5): when x equals w, same += 1 and group += 1; otherwise
   when `u3x_kr_weapon_group(base, x)` is 6, group += 1.
4. Report (the id of Q0, group, `knp`).
5. Second pass, i = 0 .. n − 1, run = 0: when the record's killer is k and it is an enemy kill: when
   `u3x_kr_weapon_group(base, ...)` of the weapon of its weapon number is not 6, stop; else run += 1. Otherwise, when the
   record's victim is k, stop. Otherwise go on. ("Uncounted" is not looked at in this pass.)
6. Report (0xd2b04848, run, `knp`).
7. When w is Q1, Q2, Q3 or Q4: report (w, same, `knp`). Otherwise, when w is Q5 or Q5b: report (0x7fdbcbbf, same,
   `knp`). Otherwise nothing more.

Steps 4 and 6 always report once step 1 has passed. The weapon lookups of 3.5 happen once per record that reaches
them, in record order, pass 1 before step 4, pass 2 between steps 4 and 6.

### 4.5 `u3x_kr_37ab00`: chains

1. Lookup of 0x47efb8e4 (always first). me = id(`knp`). When the data is null: return (no further lookup).
2. **First chain**, only when r0 is an enemy kill: last = message time, count = 1. For i = 1 .. n − 1: skip an
   uncounted record. When window < `f_sub`(seconds(last), seconds(r(i).time)) (window = the float pattern at the
   symbol data, read again each time): stop. (This test is made for every record that is not uncounted, whoever
   its killer is.) Then, when the record's killer equals me (16 bits) and it is an enemy kill: count += 1 and
   last = r(i).time. After the loop: report (0xf2c76c63, count, `knp`).
3. Always after step 2, in this order: n1 = number of weapon C1 (one lookup of the extra weapon list), n2 =
   number of weapon C2 (another lookup), then a lookup of 0xee602459.
4. **Second chain**, only when that data is not null, r0's weapon number equals n1 or n2 (the u8 widened to 32
   bits, compared with the whole number), and r0 is an enemy kill: last = message time, count = 1. For
   i = 1 .. n − 1 ("uncounted" is not looked at here): when window2 < `f_sub`(seconds(last), seconds(r(i).time)):
   stop. When the record's killer equals me (16 bits), it is an enemy kill and its weapon number equals n1 or n2:
   count += 1 and last = r(i).time. After the loop: report (0x4bae48b4, count, `knp`).

Neither chain checks that r0's killer is `knp`. A weapon the game does not know has number 0, so it then matches
weapon number 0 (kept as it is).

### 4.6 `u3x_kr_37a4bc`: long range

1. Lookup of 0x42b7081b; null data: return.
2. me = id(`knp`). When r0's killer is not me (16 bits) or r0 is not an enemy kill: return.
3. R2 = `f_mul`(range, range) with range = the float pattern at the symbol data. When R2 ≤ squared distance of r0
   is false (also for a NaN): return.
4. count = 1. For i = 1 .. n − 1: count += 1 when the record is not uncounted, its killer equals me (16 bits), it
   is an enemy kill and R2 ≤ its squared distance.
5. Report (0xa8932480, count, `knp`).

### 4.7 `u3x_kr_379ed8`: objective chain

1. When r0's objective value is 0: return.
2. count = 1. For i = 1 .. n − 1: a record whose team pair differs from r0's is skipped; a record with the same
   team pair stops the loop when its objective value differs from r0's, and otherwise count += 1.
3. m = the in-game count (3.5). For i = 0 .. m − 1: p = slot(i); when p is not null and its team equals r0's
   killer team (the s8 sign-extended): report (r0's objective value as u32, count, p).

Step 3 walks the first m slots, not the m slots that are in game (kept as it is); a slot that is not in game but
lies below m and has the team receives the event.

### 4.8 `u3x_kr_379ff0`: least-killed enemy

1. Ten u8 counters, one per slot, all 0. me = id(`knp`).
2. For i = 0 .. n − 1: when the record's killer equals me (16 bits): j = slot of id (the record's victim, 3.5);
   when j ≥ 0, counter j += 1 (8-bit wrap).
3. least = 0x10. m = the in-game count. For i = 0 .. min(m, 10) − 1: p = slot(i); when friendly(`knp`, p) is false
   (3.5: one mode type read unless p is `knp`; a null p is not friendly) and counter i ≤ least: least = counter i.
4. Report (0x610ae399, least, `knp`), always (0x10 when no slot qualified).

### 4.9 `u3x_kr_379430`: flagged kills

k = r0's killer. Return when k differs from id(`knp`) (16 bits), when r0's victim equals k, or when r0's flag A is
0. Otherwise count = 1; for i = 1 .. n − 1: when the record's killer is k and its victim is not k, count += 1 when
its flag A is not 0. Report (0x8d70ef29, count, `knp`).

### 4.10 `u3x_kr_3791ec`: trade kills

now = message time; flags = message flags.

1. Only when r0 is an enemy kill: t = 0; for i = 0 .. n − 1: the first record that is not uncounted and whose
   victim equals the message killer id (16 bits) gives t = its time, and the search stops. Then, when (s32 at
   `knp` + 0x3a0) < t and t < now and bit 0x20 of flags is set: report (0x9d715fda, 1, `knp`). (The killer died
   after its last spawn and before this explosive kill happened.)
2. Always: for i = 0 .. n − 1: when the record's time is below now, stop. When the record's killer equals the
   message victim id (16 bits), it is an enemy kill and its type is 6: report (0x9d715fda, 1, `vnp`) and return.
   ("Uncounted" is not looked at.)

Both reports can happen in one call, in this order.

### 4.11 `u3x_kr_37a914`: all opponents down

1. Lookup of 0xa84641cc; null data: return.
2. all = true, count = 0 (s32), me = id(`knp`). For i = 0 .. 9: p = slot(i). Skip the slot when p is null, when
   `u3x_net_player_in_game(p)` is 0, or when friendly(`knp`, p) (tested in this order; so one mode type read for
   every in-game slot other than `knp`). When `u3x_kr_3590b0(base, p)` is non-zero: return at once, nothing
   reported. Otherwise v = id(p), and the newest record whose victim equals v (16 bits; i = 0 .. n − 1, first
   match) decides: its killer equals me (16 bits): count += 1; otherwise all = false. A player without such a
   record changes nothing. The slot loop goes on to slot 9 also after all became false.
3. When all is still true and the s32 at the symbol data ≤ count: report (0x4a9da8a5, 1, `knp`).

### 4.12 `u3x_kr_37a16c`: streak ended

1. Lookup of 0xdd9356ec (always first); null data: return (no mode type read).
2. v = r0's victim. The mode type is read: ffa = (type is 0xe).
3. Return when r0's killer equals v. Return unless r0 is an enemy kill, or ffa, or r0's flag B is not 0, or r0's
   flag A is not 0.
4. count = 0. For i = 1 .. n − 1: skip an uncounted record. When the record's victim is v: stop. When its killer
   is v: when its victim team equals r0's victim team and not ffa and r0's flags A and B are both 0: stop;
   otherwise count += 1.
5. When the u32 at the symbol data ≤ count (unsigned): report (0xf69e35a3, 1, `knp`).

### 4.13 `u3x_kr_37a32c`: revenge

1. v = message victim id, k = message killer id. The mode type is read (always first): ffa = (type is 0xe).
2. Return unless r0 is an enemy kill or ffa. Return when n < 2.
3. Return when (s32 at `knp` + 0x3a0) < (s32 at `knp` + 0x39c) (the killer is down).
4. For i = 1 .. n − 1: skip an uncounted record. When the record's victim is v and its killer is k (16 bits
   each): return (the killer already killed this victim since). When the record's victim is k and (its killer
   team equals r0's victim team, or ffa): when its killer is v, report (0x6623a3f4, 1, `knp`); return in either
   case.

### 4.14 `u3x_kr_3590b0`: player is up

1. One call of 0x793280 with (`np`, 0xc054e78b, 0): non-zero low byte: return 1.
2. When the handle at `np` + 0x10 is valid (3.3) and (s32 at +0x39c) ≤ (s32 at +0x3a0): return 1.
3. Otherwise return the u8 at `np` + 0x770 as it is (0 to 255).

## 5. Test scenarios

All of it can be tested with call-recording stubs on a fake image: memory for the net game manager with its ten
slots, the slots flag, the score manager with a history, tables F and G and the u32 at 0xc478bb filled with
distinct made-up ids, a kill message; stubs for 0x721050 (returning a mode entry with a chosen type, or null),
0x11f2bf0 (per symbol: null, a slot holding null, or data) and 0x793280; stand-ins for `u3h_kr_stat` (recording)
and `u3h_kr_prec` (a buffer). Compare the sequence and arguments of all calls and the one byte written. Nothing
needs the live game; the live check is only that medals appear in a match (G46's test).

1. 4.1: dy just below and at ±5.0; share just below, at and above ±0.8660254; a NaN and an infinite coordinate;
   random positions (bit-for-bit comparison of the decision).
2. 4.2: n = 0, 1; the player's own kill directly at record 1; deaths before it; deaths with no earlier kill;
   ids that differ only above bit 15.
3. 4.3: flag clear; records inside / at / outside one second on both sides of the message time; uncounted,
   friendly and other killers' records; weapon numbers below and above 0x13 with a null and a real extra list (the
   lookup count); `team_mode` 0 and 1 with w = E1, E2, E3 and another weapon; a count above 255; the byte at +0x52
   lower, equal and higher than the amount, also with w = E1 in team mode.
4. 4.4: r0 not an enemy kill; k above 0xffff (no record matches); records of the same weapon, of another group-6
   weapon, of a group-0 weapon; pass 2 ended by a non-group-6 kill, by the player's death, by the end; an uncounted
   group-6 kill (not in pass 1, counted in pass 2); w = each of Q1 to Q4, Q5, Q5b, Q0 and a foreign weapon.
5. 4.5: symbol 0x47efb8e4 null (one lookup only); r0 friendly (no first report, lookups still made); gaps below,
   at and above the window; a foreign record beyond the window ending the chain; uncounted records skipped in the
   first chain and not in the second; second symbol null; r0's weapon number equal to n1, to n2, to neither; an
   unknown C1 (number 0) with weapon number 0.
6. 4.6: symbol null; r0 of another killer; r0 friendly; distance just below, at and above the range (positions
   differing by one sixteenth); NaN range; older long and short kills, uncounted ones.
7. 4.7: objective 0; a run broken by another value in the same team pair; records of other team pairs between;
   slots flag 0 (no report); in-game slots not being the first m slots; teams −1.
8. 4.8: no opponent (16 reported); an opponent never killed (0); victims that are not net players, victim 0;
   free for all (mode 0xe: every other slot is an opponent); a null mode entry; slots flag 0.
9. 4.9: each of the three return conditions; older flagged and unflagged kills, self kills.
10. 4.10: r0 friendly (part 1 skipped, part 2 still runs); no death record of the killer (t = 0); spawn time
    below / equal / above t; t equal to now; flag clear; part 2: a record at the same time by the victim with type
    6, with another type, older than now first; both reports in one call.
11. 4.11: symbol null; slots flag 0; an opponent up (immediate return after its predicate call, later slots not
    visited); all opponents down by this player with count below, at and above the minimum; one down by someone
    else (calls for later slots still made); an opponent without a death record; friendly and not-in-game slots.
12. 4.12: symbol null; killer equals victim; each of the four admitting conditions alone; the run ended by the
    victim's death, by a team kill (and not ended by it in free for all or with a flag); count below / at the
    threshold; uncounted records.
13. 4.13: n = 1; friendly r0 with and without free for all; killer down; an earlier kill of the same victim by
    the killer before the death record; the death record by the victim, by someone else of that team, by another
    team (skipped unless free for all).
14. 4.14: each of the three exits; the handle invalid in each of its four ways; death equal to spawn; the flag
    byte 0, 1 and 0x80.

## 6. What must stay as it is

- The thirteen names and signatures, and `u3x_kr_3590b0` (declared in the C part too).
- `u3h_kr_stat`, `u3h_kr_prec` and the existing pure functions of section 2 are called, not rewritten.
- The old file-private helpers of this block (slot, team, mode type, friendly, symbol data, weapon lookups,
  seconds, squared distance, "≤") are not kept: 3.5 describes what the new code needs of them.

## 7. Strays, oddities to keep, open questions

**Strays covered here** (in no work group, or listed under another group although only this one uses them):
`u3x_kr_3590b0` (exported, in no list of the inventory); the private readers of 3.5, among them the two the
inventory puts into G44 (`med_np_index` = "slot of id", `med_distance2` = "squared distance of a record").

**Not covered here**: the weapon group rule itself, delivered by G46 as `u3x_kr_weapon_group` (integration correction 2026-11-15).

**Behaviour that looks like a mistake or a leftover, to keep** (the new code must do the same):

1. 4.4 compares the full 32-bit killer id with the 16-bit record fields; every other check cuts the id to 16 bits.
2. 4.7 and 4.8 walk the first m slots, m being the number of in-game slots, instead of the in-game slots.
3. 4.8 starts from 0x10 and reports it when there is no opponent; its counters are 8 bits wide; a null slot counts
   as an opponent with 0 kills.
4. 4.5: the first chain is ended by any counted record beyond the window, also another player's; the second chain
   ignores "uncounted"; an unknown special weapon matches weapon number 0; r0's killer is never compared with
   `knp`.
5. 4.3 counts record 0 itself, cuts the amount to 8 bits, and updates the player record also when it reports
   nothing.
6. 4.2 and pass 2 of 4.4 and part 2 of 4.10 do not look at "uncounted".
7. 4.10 part 1 uses t = 0 when the killer has no death record (then the test fails unless the spawn time is
   negative).
8. 4.11 keeps calling the game for the remaining slots after the result is already decided, but returns at once
   for an opponent that is up.
9. 4.14 returns the raw flag byte, not 0 / 1.
10. No function guards n = 0 (r0 is then stale memory); G46 never calls them that way.
11. Symbol data whose slot holds a null pointer counts as "no symbol".

**Open questions**

1. The seventeen stat event ids of 3.7 (a) are written in this document as single ids, like the text, page and
   symbol ids of the other specs. The PS4 image holds none of them and their names are unknown, so they can
   neither be read from the image nor generated by the name rule. If they are to be treated as a table of game
   values: 17 u32 event ids, each the key under which the script data's medal / mission / statistics definitions
   receive one check; the only known sources are the script data at run time (the definitions that listen for
   them: layout not known here) or the names, once found, through `u3h_crc_sid`.
2. Q5b is read from an instruction immediate (image 0xc478bb). It is the only place the image holds it. With G46's
   present caller it can never be the message weapon (the weapon is not of group 6), so that branch is dead today;
   the read is still described so that the function does what the old one does.
3. The meaning of net player +0x39c / +0x3a0 (death / spawn time) and of +0x770 is taken from how the checks use
   them; only the comparisons of section 4 matter for the behaviour.

## 8. Clarifications (tester, 2026-11-14)

Facts confirmed by the differential test (`reimpl2\G45\test_g45.cpp`, old v7.212 against the new parts A and B; all
plain cases equal at -O1 and -O2). Items 6 and 7 are read-order facts of the old code that the sections above leave
open; they can only show when `u3h_kr_stat` changes the value named while a check runs, which nothing it does is
known to do.

1. **3.7 (b) names twelve weapon ids, not eleven**: E1, E2, E3, Q0, Q1 to Q4, Q5, Q5b, C1, C2. Eleven come from
   the two tables (F[1], F[5], F[6], F[7], F[8], F[9], F[10], F[13], G[14], G[15], G[17]), one from the compare
   immediate at image 0xc478bb. In the unpatched PS4 image each of the twelve places holds the id the old code
   used, the twelve ids are all different, and the byte before the immediate (image 0xc478ba) is the opcode of a
   32-bit compare with the accumulator. The built v7.212 image still contains that instruction and the one after
   it unchanged (the twelve bytes from 0xc478ba on occur in it exactly once).
2. **The `extra` argument** of `u3x_weapon_for_stat` and `u3x_weapon_stat_number` is the symbol data pointer of
   0x9e354807 itself (the address of the block with the count at +0 and the pointer at +8), or null; `fixed` is
   `base + 0x13a6170`. In 4.5 step 3 the first call asks for C1 (G[14]), the second for C2 (G[15]).
3. **4.3 step 3, order of the tests**: E1 first, then E2, then E3; the first that matches decides. So, should two
   of the three places ever hold the same id: E1 = E2 or E1 = E3 reports nothing, E2 = E3 reports under
   0x2de62476. With the real ids (all different) the order cannot be observed. Without team mode none of the three
   places matters. **4.4 step 7** likewise: Q1 to Q4 are tested before Q5 / Q5b (an id that is both reports under
   the weapon id), and a weapon equal to Q0 only gets no third report.
4. **4.5 step 4, the three conditions**: no call and no write happens between them and nothing is read through a
   null pointer whatever their order (the second symbol's data is only read inside the loop), so their order
   cannot be observed. The three lookups of step 3 are always made, in the order written, also when r0 is not an
   enemy kill.
5. **4.8 step 3, a null slot** (slots flag 0): for each of the first min(m, 10) positions the mode type is read
   once (the killer's net player is never null, so it is never "the same pointer"), the position counts as an
   opponent in every mode, and its counter takes part. The counters are filled without looking at the slots flag,
   so with the flag 0 the result is still the smallest counter of the first m positions (at most 0x10). A counter
   above 0x10 never lowers the result; a counter wraps at 256 (260 kills count as 4).
6. **4.7 step 3 reads r0 again for every slot**: the event id of each report is r0's objective value at the
   moment of that report, and the team compared is r0's killer team at that moment. Only the count is fixed
   before the slot loop. (The new part B takes the objective value once, before step 2: see the tester's report.)
7. **4.10 reads the message ids where it uses them**: the killer id for every record of part 1, the victim id for
   every record of part 2, that is after part 1's report. Time and flags are read once at the start. (The new
   part B takes both ids once at the start: see the tester's report.)
8. **4.14 step 1**: only the low byte of the game function's result counts; a result with a zero low byte and
   other bits set is "no effect".
9. **4.3 step 4**: `u3h_kr_prec` is called after the report (or after the suppressed report), and the byte is
   written only when the amount is greater; an equal amount writes nothing.
10. **4.13 step 2, "n < 2"**: has no effect of its own (with n below 2 the loop of step 4 makes no step and
    nothing is called or written in between).
11. **3.5, squared distance**: the two s16 coordinates are converted and subtracted as floats; subtracting them as
    integers first gives the same pattern for every input (all values involved are exact), so that step's form is
    free. The scaling by 1/16 before squaring and the order of the three additions are not free.
12. **Float**: every float step of the old code is one scalar single-precision operation on patterns (in the
    shipped v7.212 helper the group's code holds only single-precision add, subtract, multiply, divide, square
    root, convert-from-integer and unordered-compare instructions, no x87 and no double-precision instruction).
    Old and new agree bit for bit on every decision, including NaN, infinite and denormal inputs. In 4.1 the
    grouping (dz·dz + dx·dx) + dy·dy matters: 400 inputs were found where another grouping of the same three
    squares gives the other decision.
