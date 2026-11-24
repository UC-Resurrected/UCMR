# G44. Kill rewards: accessors, booster rewards, category medals, points and awards

Behaviour description for a reimplementation (reimpl2 pipeline, describer's document). It says what the functions
do, not how the old ones were written. All offsets are hexadecimal when prefixed with 0x. "Image offset X" means the
address `base + X`, where `base` is the game image base. All multi-byte values are little-endian; "u8 / u16 / u32 /
s8 / s16 / s32" are unsigned / signed integers of that width, "pointer" is 8 bytes. Names follow
`G22_rank_xp.md`, `G23_boosters.md` (the string id rule, 3.9 there), `G24_kickbacks.md` and `G28_medals_records.md`.

Float work of this group is small: four products "integer × booster value", one sum of three squares, one
addition of 1.5. Every float step is a single-precision operation on 32-bit patterns with the `f_*` tools of
CONVENTIONS.h (PIPELINE.md's Float rule); the old code already worked that way (it never used the x87 unit here),
so results must be equal bit for bit, with no accepted deviation.

## 1. Purpose and callers

The PS4 game dropped the function that PS3 1.19 runs first for every kill (the score keeper's kill record). The
helper restores it (group G46: `player_kill` and its pieces, hooked at the entry of the game's kill handler). This
group is what G46 and the medal checks of G45 use to hand out the rewards of a kill:

- **Accessors** (part A): net player by id, index of a net player, "same side" test, the co-op-style mode test, a
  player's score record, the stat event call, the string id continued over a suffix, a profile counter add, the
  squared distance of a kill record.
- **Points** (part A): the "+points" popup over a player, score of a player and of a team, the award call (cash,
  XP, missions), and the point value of a killed AI.
- **Booster rewards** (part B): ammunition / grenades / kickback points that boosters give for a kill, and the
  weapon definition lookup.
- **Category medals** (part B): the stat events counted per weapon category, per sniper / power weapon, and the
  "three of each" medal.

Callers (all stay as they are):

| Function | Called by |
|---|---|
| `u3h_kr_np_by_id` (C export over `kr_np_by_id`) | G46 (three places), `u3x_booster_kill` |
| `u3h_kr_prec` (C export over `kr_prec`) | G45 (two checks), G46 (five places), `u3x_three_of_each`, `u3x_add_score` |
| `u3h_kr_stat` (C export over `kr_stat`) | G45 (all checks), G46, `u3x_category_medals`, `u3x_three_of_each` |
| `u3h_kr_30feec` (C export over `kr_30feec`) | G46, `u3x_kr_30ffa8` (4.13) |
| `kr_friendly`, `kr_sid_cat`, `kr_prof_add` (C, static) | nobody in v7.212: the kill record moved to C++ (G46) with its own private copies of the three; they are still delivered (section 6, note 1) |
| `med_np_index`, `med_distance2` (file-private C++) | G45: `med_np_index` by the "fewest kills on an enemy" check, `med_distance2` by the long-range check (twice) |
| `u3x_kill_popup`, `u3x_add_score`, `u3x_team_score`, `u3x_award`, `u3x_ai_kill_points` | G46 |
| `u3x_booster_kill`, `u3x_weapon_boosters`, `u3x_category_medals`, `u3x_three_of_each` | G46 |
| `u3x_weapon_definition` (exported form of `weapon_definition`) | only a C declaration and the dead C wrapper `kr_wdef`; this group itself |
| `give_grenades` (file-private) | `u3x_booster_kill`, `u3x_weapon_boosters` |

The dead one-line C wrappers `kr_popup`, `kr_addscore`, `kr_teamscore`, `kr_award`, `kr_5b62c`, `kr_booster_kill`,
`kr_37d1cc`, `kr_37ae3c`, `kr_3814d8`, `kr_wdef` of `kr_port.inc` only forward to the `u3x_` names; they stay.

Everything runs on the game thread, inside the kill handler. No locking, no timing rule.

## 2. Interface

No function takes or returns a float by value (float values travel as 32-bit patterns). All are `extern "C"`.
"existing" = exported today under this name, name and signature stay. "new" = a new entry point; the old function
of the same purpose becomes a one-line wrapper that passes its arguments unchanged.

The group is delivered in two independent parts:

- **Part A (accessors and points)**: sections 4.1 to 4.14.
- **Part B (booster rewards and category medals)**: sections 4.15 to 4.21. Part B calls of part A only through the
  existing C exports `u3h_kr_np_by_id`, `u3h_kr_prec`, `u3h_kr_stat`, so the two parts can be written and tested
  apart.

### 2.1 Part A

| Function | Signature | Status / wrapper |
|---|---|---|
| net player by id | `uint8_t *u3x_kr_np_by_id(uintptr_t base, uint32_t id)` | new; C `kr_np_by_id(base, id)` forwards |
| same side | `int u3x_kr_friendly(uintptr_t base, const uint8_t *a, const uint8_t *b)` | new; C `kr_friendly(base, a, b)` forwards |
| co-op-style mode | `int u3x_kr_30feec(uintptr_t base)` | new; C `kr_30feec(base)` forwards |
| score record of a player | `uint8_t *u3x_kr_prec(uint8_t *S, const uint8_t *np)` | new; C `kr_prec(S, np)` forwards |
| stat event | `void u3x_kr_stat(uintptr_t base, void *s, uint32_t id, uint32_t amount, uint8_t *np)` | new; C `kr_stat(base, s, id, amount, np)` forwards |
| string id continued | `uint32_t u3x_kr_sid_cat(uintptr_t base, uint32_t h, const char *text)` | new; C `kr_sid_cat(base, h, str)` forwards |
| profile counter add | `void u3x_kr_prof_add(uintptr_t base, const uint8_t *np, uint32_t n, uint32_t amount)` | new; C `kr_prof_add(base, np, n, amt)` forwards |
| index of a net player | `int u3x_med_np_index(uintptr_t base, uint32_t id)` | new exported name for the file-private `med_np_index`; G45 calls it |
| squared kill distance | `uint32_t u3x_med_distance2(const uint8_t *record)` | new exported name for the file-private `med_distance2`; result is a float pattern; G45 calls it |
| "+points" popup | `void u3x_kill_popup(uintptr_t base, void *s, uint8_t *np, uint8_t *other, const uint8_t *info, int32_t pts, uint32_t type, uint32_t dbl)` | existing |
| player score | `void u3x_add_score(uintptr_t base, void *s, uint8_t *S, uint8_t *np, int32_t pts)` | existing |
| team score | `void u3x_team_score(uintptr_t base, void *s, uint8_t *S, uint32_t team, int32_t pts)` | existing |
| award | `void u3x_award(uintptr_t base, void *s, uint8_t *S, uint8_t *np, int32_t pts, uint32_t type, uint32_t flag)` | existing |
| AI kill points | `int32_t u3x_ai_kill_points(uintptr_t base, uint32_t w, uint32_t id, uint8_t *knp, int b)` | existing |

The existing C exports `u3h_kr_np_by_id`, `u3h_kr_prec`, `u3h_kr_stat`, `u3h_kr_30feec` keep forwarding to the C
functions, so every caller reaches the new code without a change.

### 2.2 Part B

| Function | Signature | Status |
|---|---|---|
| weapon definition | `uint8_t *u3x_weapon_definition(uintptr_t base, uint32_t w)` | existing (the private `weapon_definition` is the same function; deliver one) |
| give grenades | `void u3x_give_grenades(uintptr_t base, uint8_t *inventory, int32_t n, void *out)` | new name for the file-private `give_grenades`; only this part calls it, it may stay file-private |
| booster kill rewards | `void u3x_booster_kill(uintptr_t base, void *s, uint8_t *knp, uint32_t flags, uint32_t w)` | existing |
| weapon boosters | `void u3x_weapon_boosters(uintptr_t base, void *s, uint8_t *S, uint8_t *knp)` | existing |
| category medals | `void u3x_category_medals(uintptr_t base, void *s, uint8_t *S, uint8_t *knp, const uint8_t *info, int team_mode)` | existing |
| three of each | `void u3x_three_of_each(uintptr_t base, void *s, uint8_t *S, uint8_t *knp)` | existing |

### 2.3 Parameters

- `base`: game image base. `s`: the helper's state object (opaque; one field, 3.2).
- `S`: the score keeper (3.4), image 0x17dd780, always passed by the caller.
- `np`, `knp`, `a`, `b`, `other`: net player records (3.3). `knp` = the killer. `other` = the player whose character
  the popup is placed over (G46 passes the victim), may be null.
- `info`: the kill message of the game (3.6).
- `id` (accessors): a net player id. `id` (`u3x_kr_stat`): a stat event id. `id` (`u3x_ai_kill_points`): the id of
  the killed AI's kind.
- `w`: a weapon id (string id of the weapon). `flags`: the kill's flag word (bit 0x20 = grenade kill).
- `pts`: points. `type`, `dbl`: popup kind (0 kill, 1 assist) and "double points" mark; only their low 8 bits are
  stored. `type`, `flag` of `u3x_award`: passed through to the game.
- `team`: a team number; only its lowest bit is used. `team_mode`: non-zero in team modes.
- `b`: 0 or 1, the kill's "counts in co-op" bit as G46 extracts it.
- `out`: a caller's scratch of 8 bytes that the game's inventory functions and the booster function may write.

### 2.4 Functions of the C part, of other groups and shared functions that the new code calls

Declare them; do not reimplement them.

| Name | Signature | What it does | Part |
|---|---|---|---|
| `u3h_ext` | `uint8_t *u3h_ext(void)` | the helper's own state block (3.1); in CONVENTIONS.h | A, B |
| `u3x_net_player_id` | `uint32_t u3x_net_player_id(const uint8_t *np)` | existing: a net player's id from its record (low 12 bits of the u32 at +0x1e4, plus 0x800 when the u8 at +0x1ec is not 0 and those bits are not 0, plus 0x1000 × the u32 at +0x1e8). Reads only | A, B |
| `u3x_score_member` | `int u3x_score_member(const uint8_t *S, uint32_t id)` | existing: index of the player `id` in the score keeper's member table (first match among the first min(count, 128) members), or −1. Reads only | A |
| `u3h_kr_mode_type` | `int32_t u3h_kr_mode_type(uintptr_t base)` | existing C export: one game call 0x721050(NGM) = the current mode entry; result = the s32 at +0xc of that entry, or −1 when the entry is null | A |
| `u3h_kr_30feec` | `int u3h_kr_30feec(uintptr_t base)` | existing C export; after integration it reaches `u3x_kr_30feec` (4.3) | A (4.13) |
| `u3h_kr_statw` | `uint32_t u3h_kr_statw(uintptr_t base, uint32_t index)` | existing C export: the weapon id of a weapon stat index (0 when out of range). For an index below 0x13 it only reads the image; from 0x13 on it first makes one game call 0x11f2bf0(0x9e354807) | B |
| `u3h_kr_np_by_id`, `u3h_kr_prec`, `u3h_kr_stat` | as 2.1 (`u3h_kr_prec(uint8_t *S, uint8_t *np)`) | existing C exports over part A's functions | B |
| `u3h_crc_sid` | `uint32_t u3h_crc_sid(const char *name)` | existing C export: the string id of a name (G23 3.9: 32-bit CRC, polynomial 0x04C11DB7, most significant bit first, start 0, no final inversion). Pure; how often it is called is not part of the behaviour | B |
| float-result call | `uint32_t u3x_float_call(uintptr_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)` | calls the game function `fn` with four integer arguments and returns the 32-bit pattern of its single-precision result. Today this is the file-private `call_float` of the animation lookups group (G51); see section 7, question 1 | B |

"NGM" below = the net game manager, image 0x17cb060.

"The character of a net player" (used in 4.10 and part B): let H = the pointer at record + 0x10 and k = the u32 at
record + 0x18. The character is the pointer P stored at H when H is not null, k is not 0, P is not null and the
u32 at P + 0x7c equals k; in every other case there is none (null). Reads only.

"Virtual call slot X on object O" = the function whose address is the pointer at (pointer at O + 0) + X, called
with O as its first argument, then the arguments given.

## 3. Game and helper facts

### 3.1 The helper's state block (address from `u3h_ext()`)

All counters are u32, plain increments (wrapping).

| Offset | Meaning |
|---|---|
| 0x2bc8 | counter: stat events asked |
| 0x2bcc | running index of the stat event ring |
| 0x2be4 | counter: profile counter adds |
| 0x2c00 | ring of 16 u32: the last stat event ids |
| 0x2c40 | 0x130 bytes: the "dummy" score record, used for a player who is not in the member table. Writable: callers add scores and medal counts into it like into a real record |
| 0x2d70 | counter: player score adds |
| 0x2d74 | counter: team score adds |
| 0x2d78 | counter: awards |
| 0x2d7c | sum of awarded points (u32, wrapping; a negative `pts` adds its two's complement) |
| 0x2d80 | counter: popups asked |
| 0x2d84 | the last popup's points (the s32 stored as u32) |
| 0x2d88 | counter: popups shown |
| 0x2d90 | counter: booster values asked |
| 0x2d94 | counter: ammunition given |
| 0x2d98 | counter: grenades given |
| 0x2d9c | counter: kickback points added |

### 3.2 The state object `s`

The u32 at `s` + 0x3d78 holds switch bits:

| Bit value | Meaning when set |
|---|---|
| 0x1000 | no stat events (the game's stat call is skipped; the counters of 4.5 still run) |
| 0x4000 | no points (scores, awards, popups) |
| 0x8000 | no popups |
| 0x10000 | no booster rewards |

### 3.3 Net game manager and net players

NGM = image 0x17cb060.

| Where | Type | Meaning |
|---|---|---|
| NGM + 0x370, NGM + 0x378 | pointers | the two local players' net player records. "Local" = the record's address equals one of the two (a null record is "local" when one of them is null; callers only ask for non-null records) |
| NGM + 0x390 + 0x9c0·i, i = 0 .. 9 | records | the ten net player records |
| NGM + 0x6a48 | u8 | co-op difficulty (3 and 4 are the two hardest) |

A net player record:

| Offset | Type | Meaning |
|---|---|---|
| 0x10 | pointer | character handle (see "the character of a net player", 2.4) |
| 0x18 | u32 | character handle id |
| 0x1e8 | u32 | local user number |
| 0x21c | s32 | team |
| 0x674 | u32 | id of the net player this one is linked to (for a killer: the player who gets the booster rewards) |

Game functions on the mode:

| Image offset | Arguments | Result |
|---|---|---|
| 0x721050 | (NGM) | pointer to the current mode entry or null; s32 at +0xc = mode type. Reached through `u3h_kr_mode_type` |
| 0x7424a0 | (NGM) | u32: the id of the current game mode |
| 0x7270f0 | (NGM) | u8: non-zero when the game is an online game |

Mode type 0xe = every player for himself; 0xd = a co-op type. Mode id 0x4b720b62 = the second co-op-style mode
that counts like type 0xd.

### 3.4 The score keeper `S` (image 0x17dd780)

| Offset | Type | Meaning |
|---|---|---|
| 0 | u32 | number of members (`u3x_score_member`) |
| 0xe0 | u32 | number of kill records |
| 0xe4 + 0x28·i | record | kill record i; record 0 is the newest (the kill being handled) |
| 0x574c + 0x130·t, t = 0, 1 | record | team score record |
| 0x60cc + 0x130·m | record | player score record of member m |

A kill record (0x28 bytes; G46 owns the full layout, these are the fields this group reads):

| Offset | Type | Meaning |
|---|---|---|
| 0 | u8 | kind of kill (1 and 6 matter here: 6 = explosive) |
| 2 | u16 | killer's net player id (low 16 bits) |
| 4 | u16 | victim's net player id |
| 6 | s8 | killer's team |
| 7 | s8 | victim's team |
| 0xc | u8 | weapon stat index |
| 0xe | 3 × s16 | killer position x, y, z, in sixteenths of a metre |
| 0x14 | 3 × s16 | victim position x, y, z, same unit |
| 0x25 | u8 | non-zero: the kill is not counted |

A score record (player or team, 0x130 bytes), fields this group touches:

| Offset | Type | Meaning |
|---|---|---|
| 2 | s16 | score |
| 4 | s16 | second score copy (always changed together with +2) |
| 0x4c | u16 | number of "three of each" sets already rewarded |

Game functions:

| Image offset | Arguments | Meaning |
|---|---|---|
| 0x7aa560 | (S) | recompute the places |
| 0x7abb70 | (S, u32 member index, u32 points, u32 type, u32 flag) | the game's award: cash, XP, rank-up, missions, profile counters |
| 0x5ae4e0 | (pointer, u32 stat id, u32 amount, net player record, u32) | the game's stat event (drives medals and missions) |

### 3.5 Script data

| Image offset | Arguments | Result |
|---|---|---|
| 0x11f2e30 | (u32 symbol id) | pointer to the data of a script symbol. It is called directly, with no "is the module loaded" check (section 6, note 6) |

Symbols read here:

- **0x61d589d9, the weapon definitions**: u32 n at +0 (number of slots), pointer at +8 to n u32 keys (weapon ids;
  0 = empty slot), pointer at +0x10 to n pointers (definitions). An open-addressing table: a weapon's home slot is
  weapon id modulo n; a collision moves to the next slot, wrapping at n. A definition: u32 at +8 = weapon
  category (3.8).
- **0xa977dad3, AI kinds**: s32 count at +0, pointer at +8 to entries of 0xc bytes: s32 kind id at +0, s32 points
  at +8.
- **0x829f99c6, weapon bonuses**: s32 count at +0, pointer at +8 to entries of 0x24 bytes: u32 weapon id at +0,
  s32 bonus points at +4.

Other game functions on weapons:

| Image offset | Arguments | Result |
|---|---|---|
| 0xd415e0 | (u32 weapon id) | pointer to the net game's data of that weapon ("weapon data") or null. Fields: pointer at +0 to the weapon's tuning data (may be null; s32 at +4 = ammunition of one pick-up, s32 at +0x17c = weapon type, 4 = sniper rifle); s32 at +0x20 = non-zero for a power weapon |
| 0xd436b0 | (weapon object) | pointer to the definition of a weapon object or null; s32 at +0x58 = ammunition of one pick-up |
| 0x689d20 | (image 0x17c3b78, u32 booster effect id, net player record, 8-byte scratch or null) | single-precision float: the value of a booster effect for that player (0.0 without the booster). Called through the float-result call of 2.4 |

A character: pointer at +0x520 = its inventory (may be null). A weapon object: u32 at +0x6ec = its weapon id.
Inventory virtual slots:

| Slot | Arguments after the inventory | Result |
|---|---|---|
| 0x70 | (u32 slot number) | weapon object in that slot or null (slot 2 = grenades) |
| 0x78 | none | the weapon object of 4.19 step 5 (the long gun the player carries), or null |
| 0x80 | none | the weapon object the player holds, or null |
| 0xe8 | (pointer to an ammunition order, `out`) | adds ammunition. The order is 0x10 bytes: u32 weapon id at +0, s32 amount at +4, u8 0 at +8, bytes 9 .. 0xf zero |
| 0xf0 | (pointer to a pick-up block, u32 0, u32 0, `out`) | gives a pick-up (4.16) |

Weapon object virtual slot 0x398, no further argument: s32, the ammunition it holds.

Other image data:

| Image offset | Type | Meaning |
|---|---|---|
| 0x1402ac0 | 16 bytes | the game's identity rotation (x 0, y 0, z 0, w 1); copied, never interpreted |
| 0x13a6ff0 | 256 × u32 | the game's string id (CRC) step table |
| 0x17b4a90 + 0x114 | u32 | the HUD clock |
| 0x17d79f0 + 0x20·u | s32 | the word 4.15 adds kickback points to, u = local user number (section 6, note 4) |
| 0x17dd610 | pointer | the profile objects (may be null); user u's object at + 0x7b00·u; u32 counters at object + 0x250 + 4·n |
| 0x1805fa0 + 0x270·u | block | HUD object of local user u; pointer at +0x238 = its popup owner (may be null) |
| 0x1806480, 0x1b24bd8 | pointers | "current HUD object" globals written before a popup slot is filled |

Popup slots: 12 slots of 0x70 bytes at (popup owner) + 0x2690 + 0x70·k. A slot: 0x60 bytes placement frame
(24 u32 words; words 8, 9, 10 = position x, y, z as float patterns), s32 points at +0x60 (−1 = free), u32 start
clock at +0x64, u8 kind at +0x68, u8 double mark at +0x69. A character's placement frame: the 0x60 bytes at
character + 0xb0, same word layout.

### 3.6 The kill message `info`

| Offset | Type | Meaning |
|---|---|---|
| 0x20, 0x24, 0x28 | u32 float patterns | victim position x, y, z |
| 0x30 | u32 | killer's net player id |
| 0x38 | u32 | weapon id |

### 3.7 Ids

Single ids, with their names where the string id rule (G23 3.9) gives exactly the id (checked by computing the
CRC; the new code may use either the number or `u3h_crc_sid` of the name):

| Id | What | Name |
|---|---|---|
| 0x0b8bd6d6 | weapon id of the hand grenade (in the PS4 image as an immediate) | |
| 0xc58ae515 | "weapon" id of a stealth kill | "net-kill-stealth" |
| 0x08be7587 | weapon id of the propane tank | "propane" |
| 0x159df996, 0xc92d29c1, 0x5136f850, 0x33e29a1b | weapon ids (4.19, 4.20) | not found |
| 0xa828aa55 | stat: kill with weapon 0x159df996 | not found |
| 0xcc5567c6 | stat: kills with weapon 0x33e29a1b | not found |
| 0xceb95908 | stat: sniper rifle kills | "net-sniper-kills" |
| 0xeadf6540 | stat: sniper rifle kills in a row | "net-sniper-streak" |
| 0xa1d3f722 | stat: power weapon kills | "net-power-weapon-kills" |
| 0xb550b700 | stat: propane kill | "net-kill-propane" |
| 0xa6fa41ca | stat: three of each | "net-kill-triple-threat" |
| 0x0a1b799a | booster effect: ammunition for a kill, as a share of a pick-up | not found |
| 0xfca72e93 | booster effect: grenades for a grenade kill | not found |
| 0x8e5e947e | booster effect: kickback points for a stealth kill | not found |
| 0x966ccd32 | booster effect: ammunition for a kill with weapon 0xc92d29c1 / 0x5136f850 | not found |
| 0xed737cfb | booster effect: long gun pick-ups for a stealth kill | not found |
| 0x43d8284d | booster effect: grenades for a stealth kill | not found |
| 0x314d02e8 | booster effect: ammunition every third / fifth category kill | "ammo-award" |
| 0x61d589d9, 0xa977dad3, 0x829f99c6 | script symbols (3.5) | |
| 0x4b720b62 | game mode id (3.3) | |

### 3.8 The category stat ids: a rule, not a table

The old code holds a literal list of six stat ids, one per weapon category 0 .. 5 (category 3 has none). The PS4
executable does not contain these values (the function that used them is gone; the ids only occur in the game's
script data as medal and mission conditions). They are generated by the string id rule from these names, which
give exactly the five old values (checked by computing the CRC of each name):

| Category | Stat name |
|---|---|
| 0 | "net-kill-pistol" |
| 1 | "net-kill-longgun" |
| 2 | "net-kill-grenade" |
| 3 | none |
| 4 | "net-kill-turret" |
| 5 | "net-kill-heavy" |

The new code holds the names (as U3X_RO character arrays) and obtains each id with `u3h_crc_sid` (or its own
bit-by-bit CRC of G23 3.9). No literal list of the ids.

## 4. Behaviour

### Part A

### 4.1 `u3x_kr_np_by_id(base, id)`

`id` 0: null. Otherwise the first of the ten net player records (i = 0 .. 9) whose `u3x_net_player_id` equals `id`;
none: null. Whether the player is in the game is not looked at. Reads only.

### 4.2 `u3x_kr_friendly(base, a, b)`

1. `a` and `b` are the same address (also both null): 1, with no call.
2. Otherwise `u3h_kr_mode_type(base)` is called once; type 0xe: 0.
3. Otherwise 1 when both are not null and their teams (s32 at +0x21c) are equal, else 0.

### 4.3 `u3x_kr_30feec(base)`

`u3h_kr_mode_type(base)` once; 0xd: result 1 with no further call. Otherwise one game call 0x7424a0(NGM); result 1
when its u32 result is 0x4b720b62, else 0.

### 4.4 `u3x_kr_prec(S, np)`

`np` not null and `u3x_score_member(S, u3x_net_player_id(np))` gives m ≥ 0: `S` + 0x60cc + 0x130·m. In every other
case (null `np`, or not a member): the dummy record, `u3h_ext()` + 0x2c40. Never null. Reads only.

### 4.5 `u3x_kr_stat(base, s, id, amount, np)`

In this order:

1. Counter 0x2bc8 + 1.
2. r = the ring index (u32 at 0x2bcc); the ring index + 1 is stored; ring word (r mod 16) = `id`.
3. When switch 0x1000 is set: stop.
4. Game call 0x5ae4e0(null, `id`, `amount`, `np`, 0). `np` is passed as given (may be null).

### 4.6 `u3x_kr_sid_cat(base, h, text)`

Continues a string id over the characters of `text` up to (not including) its NUL. For each character c (as
u8), with T = the 256 u32 at image 0x13a6ff0: h becomes ((h mod 2^24) × 256) XOR T[(h div 2^24) XOR c]. Result:
the final h (an empty text returns `h` unchanged). This is one CRC step per character with the game's own table,
so that `u3x_kr_sid_cat(base, id of "x", "y")` is the id of "xy". The table is read from the image, not computed.

### 4.7 `u3x_kr_prof_add(base, np, n, amount)`

1. Game call 0x7270f0(NGM); a zero u8 result: stop.
2. P = the pointer at image 0x17dd610; null: stop.
3. The u32 at P + 0x7b00·(u32 at `np` + 0x1e8) + 0x250 + 4·n grows by `amount` (wrapping); counter 0x2be4 + 1.

`np` is not checked for null.

### 4.8 `u3x_med_np_index(base, id)`

As 4.1, but the result is the index i (0 .. 9) of the record, −1 for `id` 0 or no match.

### 4.9 `u3x_med_distance2(record)`

The squared distance between the killer and the victim position of a kill record, in metres, as a float pattern.
Single-precision steps, in this order, starting from sum = +0.0 (pattern 0), for the axes x, y, z in turn:

- d = (float(killer coordinate) − float(victim coordinate)) × 0.0625 (pattern 0x3d800000): the two s16 are
  converted to float (exact), subtracted (one rounding), multiplied (one rounding);
- sum = sum + d × d (the product rounded, then the sum rounded).

Result: sum. Reads only.

### 4.10 `u3x_kill_popup(base, s, np, other, info, pts, type, dbl)`

1. Counter 0x2d80 + 1; the word at 0x2d84 = `pts`. (Always, before any test.)
2. Stop when switch 0x8000 or 0x4000 is set, or `np` is null.
3. O = image 0x1805fa0 + 0x270·(u32 at `np` + 0x1e8). L = the pointer at O + 0x238; null: stop.
4. Source character: the character of `other` when `other` is not null and has one. Otherwise the character of
   `np`, and the popup is "moved". No character at all: stop.
5. The frame = the 24 u32 words at source character + 0xb0. When moved: words 8, 9, 10 are replaced by the u32 at
   `info` + 0x20, + 0x24, + 0x28. Then word 9 = word 9 + 1.5 (`f_add` with pattern 0x3fc00000).
6. The first slot k = 0 .. 11 of L whose s32 at +0x60 is −1 is filled, in this order: the pointers at image
   0x1806480 and 0x1b24bd8 = O; the 24 words of the frame to the slot's +0; s32 at +0x60 = `pts`; u32 at +0x64 =
   the HUD clock; u8 at +0x68 = low byte of `type`; u8 at +0x69 = low byte of `dbl`; counter 0x2d88 + 1.
   No free slot: nothing is written (the two globals neither).

`info` is only read when the popup is moved. A `pts` of −1 makes the slot look free again (kept).

### 4.11 `u3x_add_score(base, s, S, np, pts)`

`np` null or switch 0x4000 set: nothing. Otherwise: R = `u3x_kr_prec(S, np)` (through `u3h_kr_prec`; the dummy record
for a non-member); the s16 at R + 2 and the s16 at R + 4 each grow by the low 16 bits of `pts` (wrapping); counter
0x2d70 + 1; game call 0x7aa560(S).

### 4.12 `u3x_team_score(base, s, S, team, pts)`

Switch 0x4000 set: nothing. Otherwise: R = `S` + 0x574c + 0x130·(`team` AND 1); the s16 at R + 2 and at R + 4 each
grow by the low 16 bits of `pts`; counter 0x2d74 + 1; game call 0x7aa560(S).

### 4.13 `u3x_award(base, s, S, np, pts, type, flag)` and the small point functions

`u3x_award`: switch 0x4000 set: nothing. Otherwise: m = `u3x_score_member(S, u3x_net_player_id(np))` for a non-null
`np`, −1 for a null one; counter 0x2d78 + 1; the word at 0x2d7c grows by `pts`; game call 0x7abb70(S, m as u32,
the low 16 bits of `pts` sign-extended to 32 bits, `type`, `flag`).

The following exported one-liners sit between this group's functions, are in no work group and stay as they are
(G46 calls them; given here so they can be declared and stubbed):

| Function | Behaviour |
|---|---|
| `int u3x_kr_30ffa8(uintptr_t base, const uint8_t *np, int b)` | `u3h_kr_30feec(base)` once; 0: result 1. Otherwise: `b` when `np` is not null, 0 when it is null |
| `int u3x_kr_34fc04(uintptr_t base)` | 1 when `u3h_kr_mode_type(base)` is 0xf |
| `int32_t u3x_kr_dc_int(uintptr_t base, uint32_t off)` | game call 0x11f2bf0(0xea5596b8) gives a pointer to a pointer (or null: result 0); result = the s32 at offset `off` of the data |
| `int32_t u3x_kr_37c984(uintptr_t base)` / `u3x_kr_37c914` | in mode type 0xf: `u3x_kr_dc_int(base, 4)` / `(base, 0xc)`; otherwise 3 / 0 |
| `int32_t u3x_kr_37c9f4(uintptr_t base)` | game call 0x7a8150() |
| `int32_t u3x_kill_points(uintptr_t base, uint32_t fn, uint8_t *np)` | game call (base + fn)(null, np) |
| `int u3x_double_points(uintptr_t base, const uint8_t *np)` | 1 when the s32 at image 0x17dd624 is not 0 and the s32 at image 0x17dd634 differs from the player's team (0 for a null player) |

### 4.14 `u3x_ai_kill_points(base, w, id, knp, b)`

1. T = game call 0x11f2e30(0xa977dad3). E = the first entry (index 0 upward, while index < the s32 count) whose
   kind id equals `id`; T null or no such entry: the result is 100 and nothing else is called.
2. points = E's points (s32 at +8).
3. T2 = game call 0x11f2e30(0x829f99c6). bonus = the bonus of the first entry whose weapon id equals `w`; T2 null
   or no such entry: 0.
4. m = `u3x_kr_30ffa8(base, knp, b)` (4.13).
5. d = the u8 at NGM + 0x6a48. factor = 1.5 (0x3fc00000) for d = 3, 2.0 (0x40000000) for d = 4, else 1.0
   (0x3f800000).
6. Result = float(points + bonus) × (float(m) × factor), converted to s32 by truncation toward zero. Steps: the
   integer sum (32-bit wrapping) converted to float (one rounding), m converted (exact), m × factor (rounded),
   then the outer product (rounded), then `f_to_int`.

### Part B

"Booster value v(e, P, o)" below = the float pattern returned by game function 0x689d20(image 0x17c3b78, e, P, o)
(float-result call), followed by counter 0x2d90 + 1. "Whole part" of a float pattern = `f_to_int` (truncation
toward zero; a NaN or an out-of-range value gives 0x80000000, as the conversion instruction does). "Positive" =
greater than +0.0 by a float comparison (false for a NaN).

"Add ammunition (I, weapon id, amount, o)" = an order block as in 3.5 is filled, virtual call slot 0xe8 on the
inventory I with (pointer to the order, o), then counter 0x2d94 + 1.

### 4.15 Kickback points

"Add kickback points (P, v)": the s32 at image 0x17d79f0 + 0x20·(u32 at P + 0x1e8) becomes itself + v, or 0 when
that sum is negative (32-bit signed, wrapping); counter 0x2d9c + 1.

### 4.16 `u3x_give_grenades(base, inventory, n, out)`

A pick-up block of 0xa0 bytes, at an address that is a multiple of 16, is filled: all zero, except the 16 bytes of
image 0x1402ac0 copied to +0x10 and again to +0x30; u8 0xff at +0x6c; u32 0x0b8bd6d6 at +0x78; s32 `n` at +0x7c; u8
0xff at +0x90. Then virtual call slot 0xf0 on `inventory` with (pointer to the block, u32 0, u32 0, `out`); then
counter 0x2d98 + 1.

### 4.17 `u3x_weapon_definition(base, w)`

T = game call 0x11f2e30(0x61d589d9) (always made, also for `w` = 0). Null when T is null, its slot count n is 0, or
`w` is 0. Otherwise the table of 3.5 is probed from slot (`w` modulo n): a slot whose key is `w` gives that slot's
definition pointer (returned as stored, may be null); an empty slot (key 0) gives null; after n slots without
either: null.

### 4.18 `u3x_booster_kill(base, s, knp, flags, w)`

The rewards go to the player the killer is linked to.

1. P = `u3h_kr_np_by_id(base, u32 at knp + 0x674)`. Stop when P is null, P is not a local player (3.3), or switch
   0x10000 is set.
2. When P has a character C and C's inventory I (pointer at C + 0x520) is not null, with o = 8 zero bytes:
   - `flags` bit 0x20 clear: d = v(0x0a1b799a, P, o). When d is positive: W = virtual call slot 0x80 on I. When W
     is not null: D = game call 0xd436b0(W). When D is not null: add ammunition (I, u32 at W + 0x6ec, whole part
     of (float(s32 at D + 0x58) × d), o). When d is not positive nothing more is called.
   - `flags` bit 0x20 set: g = whole part of v(0xfca72e93, P, o). When g is not 0: `u3x_give_grenades(base, I, g,
     o)`.
3. In every case that passed step 1 (with or without character and inventory): when `w` is 0xc58ae515: add
   kickback points (P, whole part of v(0x8e5e947e, P, null)).

`knp` is not checked for null.

### 4.19 `u3x_weapon_boosters(base, s, S, knp)`

1. Switch 0x10000 set: stop.
2. x = `u3h_kr_statw(base, weapon stat index of kill record 0)` (called before the tests of step 3).
3. C = the character of `knp`; none: stop. I = its inventory; null: stop. o = 8 zero bytes.
4. When x is 0xc92d29c1, or x is 0x5136f850 and the kind of kill record 0 is 1: a = whole part of
   v(0x966ccd32, knp, o); when a > 0: add ammunition (I, x, a, o).
5. When x is 0xc58ae515:
   - n = whole part of v(0xed737cfb, knp, o). When n is not 0: W = virtual call slot 0x78 on I; when W is not
     null: D = game call 0xd436b0(W); when D is not null: add ammunition (I, u32 at W + 0x6ec, n × (s32 at
     D + 0x58) as a wrapping 32-bit product, o).
   - Then, also when n was 0: g = whole part of v(0x43d8284d, knp, o). When g is not 0: G = virtual call slot 0x70
     on I with (u32 2). When G is not null and virtual call slot 0x398 on G returns an s32 greater than 0: add
     ammunition (I, u32 at G + 0x6ec, g, o). Otherwise (G null, or it holds none): `u3x_give_grenades(base, I, g,
     o)`.

### 4.20 `u3x_category_medals(base, s, S, knp, info, team_mode)`

"Stat(id, amount)" = `u3h_kr_stat(base, s, id, amount, knp)`. "Weapon data(x)" = one game call 0xd415e0(x).
"Sniper data" = a weapon data pointer whose tuning data pointer (+0) is not null and has weapon type 4.

1. Kill record 0 has equal killer and victim teams: stop, nothing is called.
2. w = u32 at `info` + 0x38; k = the low 16 bits of the u32 at `info` + 0x30. Def = `u3x_weapon_definition(base, w)`
   (one lookup call). category = u32 at Def + 8, or 0xfffffffd without a definition.
3. w is 0x159df996: Stat(0xa828aa55, 1).
4. The kill records i = 0 .. count − 1 (count = u32 at `S` + 0xe0, not limited), newest first, with five counters
   (same category, sniper, sniper run, power, weapon 0x33e29a1b) at 0 and a flag "run ended" clear:
   - a record that is not counted (+0x25 not 0) is skipped entirely;
   - a record whose killer is k and whose two teams differ:
     1. x = `u3h_kr_statw(base, its weapon stat index)`;
     2. x equals w: same category + 1 (no lookup). Otherwise D = `u3x_weapon_definition(base, x)` (one lookup
        call); when D is not null and the u32 at D + 8 equals category: same category + 1;
     3. E = weapon data(x). E null: the run has ended. E sniper data: sniper + 1, and sniper run + 1 while the
        run has not ended. E not sniper data: the run has ended. For a non-null E whose s32 at +0x20 is not 0:
        power + 1;
     4. x is 0x33e29a1b: that counter + 1;
   - any other record whose victim is k (this includes the player's own team kills and suicides): the run has
     ended.
5. E = weapon data(w). When not null: for sniper data, Stat(0xceb95908, sniper) then Stat(0xeadf6540, sniper
   run); then, when the s32 at E + 0x20 is not 0, Stat(0xa1d3f722, power).
6. w is 0x33e29a1b: Stat(0xcc5567c6, its counter).
7. When category is below 6 and has a stat name (3.8): Stat(that category's id, same category). Then, when
   category is 0 or 1 and switch 0x10000 is clear: with every = 5 for a non-zero `team_mode`, else 3: when same
   category is a multiple of every (0 counts as one) and `knp` is a local player: o = 8 zero bytes; d =
   v(0x314d02e8, knp, o). When d is positive: E2 = weapon data(w) (called again), C = the character of `knp`;
   when E2 is not null, the s32 at E2 + 0x20 is 0, the tuning data pointer Tn at E2 + 0 is not null, C exists and
   its inventory I is not null: add ammunition (I, w, whole part of (float(s32 at Tn + 4) × d), o).
8. w is 0x08be7587: Stat(0xb550b700, 1).

`knp` and `info` are not checked for null.

### 4.21 `u3x_three_of_each(base, s, S, knp)`

1. me = low 16 bits of `u3x_net_player_id(knp)`.
2. Over all kill records (count as in 4.20), three counters at 0. A record is looked at only when it is counted,
   its killer is me, killer and victim ids differ, and the two teams differ. Kind 6: explosive + 1 (no call).
   Otherwise: D = `u3x_weapon_definition(base, u3h_kr_statw(base, its weapon stat index))`; D null: nothing; the
   u32 at D + 8 is 0: pistol + 1; is 1: long gun + 1.
3. sets = the smallest of pistol div 3, explosive div 3, long gun div 3.
4. R = `u3h_kr_prec(S, knp)`. When the u16 at R + 0x4c is smaller than sets: the u16 = sets (low 16 bits), then
   `u3h_kr_stat(base, s, 0xa6fa41ca, 1, knp)`. One stat event however many new sets there are.

## 5. Test scenarios

Everything can be tested on a fake image with call-recording stubs (the test compares memory, results and the
order and arguments of all calls); nothing here needs the live game except the checks of section 7.

Part A (stubs for 0x721050 / `u3h_kr_mode_type`, 0x7424a0, 0x7270f0, 0x5ae4e0, 0x7aa560, 0x7abb70, 0x11f2e30;
`u3x_net_player_id` and `u3x_score_member` real):

1. `u3x_kr_np_by_id` / `u3x_med_np_index`: id 0; id of record 0, of record 9; an id of no record; two records with
   the same id (first wins); a guest id (+0x800).
2. `u3x_kr_friendly`: same pointer (no mode call); both null; type 0xe with equal teams; other type with equal /
   different teams; one null.
3. `u3x_kr_30feec`: type 0xd (0x7424a0 not called); other type with mode id equal / different; no mode entry (−1).
4. `u3x_kr_prec`: null player; member 0; member 127; not a member; member index 128 and above (dummy).
5. `u3x_kr_stat`: ring wraps after 16; switch 0x1000 set (counters move, no game call); null `np`.
6. `u3x_kr_sid_cat`: empty text; one character; "-killed" on several start values; characters above 0x7f.
7. `u3x_kr_prof_add`: offline; null profile pointer; users 0 and 1; n = 0 and large n.
8. `u3x_med_distance2`: equal positions; extreme s16 values (−32768 against 32767 on all axes); random records,
   bit for bit.
9. `u3x_kill_popup`: each stop (switch 0x8000, switch 0x4000, null `np`, null owner, no character at all);
   `other` with a character; `other` null; `other` with a stale handle (moved, position from `info`); slots: first
   free, only the last free, none free (globals untouched); `pts` = −1; `type` / `dbl` above 0xff; a NaN and an
   infinite y.
10. `u3x_add_score`, `u3x_team_score`: switch set; null player; non-member (dummy record changes); wrap of the
    s16; team 0, 1, 2, 3.
11. `u3x_award`: switch set; null player (index −1); `pts` beyond 16 bits (the counter gets the full value, the
    game the sign-extended low 16 bits); negative `pts`.
12. `u3x_ai_kill_points`: no kind table; kind not found (100, one call only); found without bonus table; with a
    bonus; difficulty 0, 3, 4; m = 0 and 1 (co-op-style mode with null / non-null killer and b = 0 / 1, other
    mode); negative points; counts 0 and negative.

Part B (stubs for `u3h_kr_np_by_id`, `u3h_kr_prec`, `u3h_kr_stat`, `u3h_kr_statw`, the float-result call, 0x11f2e30,
0xd415e0, 0xd436b0 and fake objects with virtual tables that record their calls; `u3h_crc_sid` real):

13. `u3x_weapon_definition`: null table; n = 0; w = 0 (the lookup call is still made); home slot hit; hit after a
    collision and after the wrap; empty slot met first; full table without the key; a null definition stored.
14. `u3x_give_grenades`: the block bytes, its alignment, the argument order, n negative.
15. `u3x_booster_kill`: linked player missing; not local; switch set; no character; null inventory (kickback
    part still runs for the stealth id); normal kill with d = 0, negative, NaN, positive (null weapon, null
    definition, product truncation such as 30 × 0.35); grenade kill with value 0, 0.9, 2.0; stealth id with a sum
    that goes below 0 (clamp) and users 0 and 1.
16. `u3x_weapon_boosters`: switch set (no call at all); stale handle (the `u3h_kr_statw` call was made); each weapon
    id; 0x5136f850 with kind 1 and with kind 2; a = 0 and negative; stealth id with n = 0 / not 0, missing long gun
    or definition, g = 0, grenade weapon missing, holding 0, holding some.
17. `u3x_category_medals`: record 0 with equal teams (no call); weapon without a definition; each category 0 .. 5
    and 6; category 3 (no stat); histories mixing uncounted records, other killers, own deaths, team kills,
    weapons without data, sniper and non-sniper weapons (run counting), power weapons; the stat order of steps 3,
    5, 6, 7, 8; the ammunition step: counts 0, 3, 4, 5, 6 with `team_mode` 0 and 1, not local, switch set, d not
    positive, power weapon, null tuning data, no character, null inventory; the five category ids equal to the
    old ones.
18. `u3x_three_of_each`: fewer than three of one kind; exactly one set; two sets at once (one stat event, stored
    2); stored value already equal (no event); suicides, team kills, uncounted and other players' records
    ignored; a kind-6 record needs no lookup; the dummy record as R.

## 6. Old behaviour to keep (looks like a bug or dead code)

1. `kr_friendly`, `kr_sid_cat` and `kr_prof_add` have no caller in v7.212 (the compiler drops them); G46 carries
   private copies with the same behaviour. They are described and delivered so that the C file holds no port
   logic; G46's describer may point G46's copies at them.
2. `u3x_kr_prec` hands out one shared, writable dummy record for every unknown player: scores and medal counts of
   such players accumulate there and are never cleared by this group.
3. `u3x_kill_popup` counts and records the points before it tests its switches; a `pts` of −1 leaves the slot
   free; when no slot is free the popup is dropped silently.
4. Kickback points are added at image 0x17d79f0 + 0x20·user, while G25 ("the kickback points of local user u")
   reads that block with 0x30 per user. For user 0 both agree; for a second local user the old code writes a
   different word. Kept as it is (see question 2).
5. In 4.20 the "multiple of every" test passes for a count of 0, which can happen when the newest record is not
   counted; then ammunition is given although no category kill is in the history.
6. Symbol lookups go straight to 0x11f2e30, which (G24 3.2) stops the game for a symbol whose module is not
   loaded. The old code relies on the multiplayer scripts being loaded during a match. Kept.
7. `u3x_weapon_definition` makes its lookup call before it tests `w` = 0.
8. `u3x_ai_kill_points` returns the default 100 unscaled (no co-op factor, no difficulty factor) when the kind is
   unknown.
9. `u3x_award` passes −1 as the member index for a null or unknown player; the record counts are not limited to
   the 512 records the history holds; `knp` / `info` are used without null tests where section 4 says so.
10. `u3x_team_score` uses only the lowest bit of the team number.
11. In 4.18 the kickback booster is asked with a null scratch pointer, the other boosters with a real one.

## 7. Open questions and notes for the integrator

1. **Float-result call.** The booster function returns a float. The only existing tool for that is the
   file-private `call_float` of the animation lookups group (G51: four integer arguments, returns the pattern of
   the float result). The integrator has to make it reachable under an exported name (proposed
   `u3x_float_call`, signature in 2.4) or tell the implementer which name G51's new code gives it. Section 4 only
   needs "the 32-bit pattern of the float result of 0x689d20".
2. **Kickback stride** (section 6, note 4): 0x20 per user here against 0x30 in G25. Not decidable from the old
   code alone; a check in the PS4 image of how the game's own readers of 0x17d79f0 step per user would settle it.
   Until then the old value stays.
3. **Names not found** (3.7): the weapon ids 0x159df996, 0xc92d29c1, 0x5136f850, 0x33e29a1b, the stats 0xa828aa55
   and 0xcc5567c6 and six booster effect ids are single ids without a known name; they stay numbers. Only the
   category list (3.8) is a table, and it is resolved by names.
4. **Exports to add (integrator):** `u3x_med_np_index` and `u3x_med_distance2` for G45 (today file-private in the
   medal checks' namespace; G45's new code must call these names). The C functions `kr_np_by_id`, `kr_friendly`,
   `kr_30feec`, `kr_prec`, `kr_stat`, `kr_sid_cat`, `kr_prof_add` become one-line wrappers of the `u3x_kr_` entry
   points of 2.1.
5. The private helpers of the old file that this description replaces (no separate delivery; their behaviour is
   in section 4): the local player test, the booster value call with its counter, the ammunition order, the
   kickback add, the weapon data call and the sniper data test, and the literal list `category_stats` (removed,
   3.8).
6. The small exported functions of 4.13's table (`u3x_kr_30ffa8`, `u3x_kr_34fc04`, `u3x_kr_dc_int`,
   `u3x_kr_37c984`, `u3x_kr_37c914`, `u3x_kr_37c9f4`, `u3x_kill_points`, `u3x_double_points`) are in no work group;
   they are judged trivial and stay as they are. Final sweep to confirm.

## Clarifications (tester, 2026-11-14)

Facts settled by the differential test (old v7.212 against the new parts):

1. **4.7, offset of the profile counter.** The old function computes "4·n" in 32-bit unsigned arithmetic (wrapping):
   the counter changed is the u32 at P + 0x7b00·user + 0x250 + ((4·n) mod 2^32), with the user term computed without
   wrap. For n below 0x40000000 this is the same as the plain formula; for n = 0x40000001 the old function adds to
   counter 1 (P + 0x7b00·user + 0x254). In v7.212 the function has no caller (section 6, note 1), so no running
   case reaches such an n.
2. **4.20 step 4 and 4.21 step 2, the record count.** The old functions read the count (u32 at `S` + 0xe0) again
   before every round of the history loop, not once before it. This only matters when a function called inside the
   loop (`u3h_kr_statw`, the symbol lookup 0x11f2e30, the weapon data call 0xd415e0) changes that word; none of them
   writes the score keeper in the game as far as this group's description goes (not checked in the live game).
3. Every name of 3.7 and 3.8 that the new code uses gives exactly the old literal id (computed with the string id
   rule): the five category stats, "net-kill-stealth", "propane", "net-sniper-kills", "net-sniper-streak",
   "net-power-weapon-kills", "net-kill-propane", "net-kill-triple-threat", "ammo-award".
4. 4.21 step 4: with more than 0xffff sets the stored u16 is the low 16 bits of the set count, and the test "stored
   value smaller than sets" uses the full count (so 65537 sets after a stored 0xffff store 1 and raise one event).
5. Null `s` (every function that tests a switch): the switch word is read before any game call (and faults there);
   a moved popup with a null `info` faults on the read at `info` + 0x20, after the counters of step 1 and before
   any slot or global is written.
6. Section 7 questions 1 and 2 (the float-result call's export name, the kickback stride) are not decidable by the
   test; the test fed both sides one stand-in for the float-result call and kept the stride 0x20.
