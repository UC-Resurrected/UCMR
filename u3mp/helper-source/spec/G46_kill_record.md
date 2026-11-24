# G46. Kill record: history, K/D statistics, points, medal dispatch

Behaviour description for a reimplementation (reimpl2 pipeline, describer's document). It says what the functions
do, not how the old ones were written. All offsets are hexadecimal when prefixed with 0x. "Image offset X" means the
address `base + X`, where `base` is the game image base. All multi-byte values are little-endian; "u8 / u16 / u32 /
s8 / s16 / s32" are unsigned / signed integers of that width, "pointer" is 8 bytes. Names follow
`G44_kill_rewards.md` (the accessors, points and rewards this group calls), `G45_medal_conditions.md` (the medal
checks it dispatches), `G22_rank_xp.md` and `G28_medals_records.md`.

Float work of this group is tiny: six products "coordinate × 16" turned into integers, and one sum of three squares
compared with 900. Every float step is a single-precision operation on 32-bit patterns with the `f_*` tools of
CONVENTIONS.h (PIPELINE.md's Float rule). The old code already worked that way here (no x87 step), so results must
be equal bit for bit, with no accepted deviation.

## 1. Purpose and callers

PS3 1.19 runs, first thing for every kill, a function of the score keeper that the PS4 game dropped: it writes the
kill into the kill history, counts kills / deaths / assists / suicides, gives team and player score, hands out the
kill points (popup, cash, XP), sends some forty stat events (which drive medals and missions) and runs the medal
checks. The helper restores it.

One entry point, `u3x_kr_port(base, info, s)`. Its only caller is the one-line C function `kr_port` of
`kr_port.inc`, which the helper's hook dispatcher calls for hook kind 49 (the entry of the game's kill handler,
image 0x7350c0: first argument the net game manager, second the kill message) when `base` and the message are not
null. It runs on the game thread, once per kill, before the game's own handler. No locking, no timing rule.

Everything else in this document is the inside of that one call.

## 2. Interface

No function takes or returns a float by value. All entry points are `extern "C"`.

The group is delivered in two independent parts. They share one plain structure, the kill context (2.2), and call
each other only through the names of 2.1.

- **Part A (frame, history, scoring, assists)**: 4.1 to 4.9. Delivers `u3x_kr_port`.
- **Part B (counted kill, statistics, co-op AI victim, aftermath, streaks, weapon group)**: 4.10 to 4.15. Delivers
  `u3x_kr_counted`, `u3x_kr_aftermath`, `u3x_kr_weapon_group`.

### 2.1 Functions to deliver

| Function | Signature | Part | Status |
|---|---|---|---|
| one kill | `void u3x_kr_port(uintptr_t base, uint8_t *info, void *s)` | A | existing name and signature; the C wrapper `kr_port(base, info, s)` stays as it is |
| counted kill | `void u3x_kr_counted(U3xKill *k)` | B | new; called by part A (4.2 step 11) |
| aftermath | `void u3x_kr_aftermath(U3xKill *k)` | B | new; called by part A (4.2 step 12) |
| weapon group | `int u3x_kr_weapon_group(uintptr_t base, uint32_t weapon)` | B | new; replaces the old `u3x_weapon_group(uint32_t weapon)` and its literal id list (3.9, section 7 question 1) |

Parameters: `base` = game image base; `info` = the kill message (3.5); `s` = the helper's state object (opaque; one
field, 3.2); `k` = the kill context; `weapon` = a weapon id.

### 2.2 The kill context `U3xKill`

Filled by part A (4.2), read by part B. Part B changes no field of it. Fields in this order, natural alignment
(pointers 8 bytes, then u32, then u8):

| Field | Type | Meaning |
|---|---|---|
| `base` | `uintptr_t` | image base |
| `info` | `uint8_t *` | the kill message |
| `s` | `uint8_t *` | the state object |
| `S` | `uint8_t *` | the score keeper (image 0x17dd780) |
| `knp` | `uint8_t *` | the killer's net player record; never null when part B is called |
| `vnp` | `uint8_t *` | the victim's net player record, or null (a victim that is not a player) |
| `victim_entity` | `uint8_t *` | the handle of a non-player victim (4.4), or null |
| `krec` | `uint8_t *` | the killer's score record (never null) |
| `flags` | `uint32_t` | the message's flag word |
| `killer_id`, `victim_id`, `weapon` | `uint32_t` each | from the message |
| `n` | `uint32_t` | the number of kill records to look at (after 4.3) |
| `counts`, `medals`, `coop`, `self_kill`, `on` | `uint8_t` each, 0 or 1 | 4.2 |

### 2.3 Functions of the C part, of other groups and shared functions that the new code calls

Declare them; do not reimplement them. "G44 x.y" = the section of `G44_kill_rewards.md` that describes it.

| Name and signature | What it does | Part |
|---|---|---|
| `uint8_t *u3h_ext(void)` | the helper's own state block (3.1); in CONVENTIONS.h | A, B |
| `uint8_t *u3h_kr_np_by_id(uintptr_t base, uint32_t id)` | net player record with that id, null for id 0 or none (G44 4.1). Reads only | A |
| `uint8_t *u3h_kr_prec(uint8_t *S, uint8_t *np)` | the player's score record; the writable dummy record for a null or unknown player; never null (G44 4.4) | A |
| `void u3h_kr_stat(uintptr_t base, void *s, uint32_t id, uint32_t amount, uint8_t *np)` | one stat event (G44 4.5) | A, B |
| `int u3h_kr_30feec(uintptr_t base)` | 1 in a co-op-style mode (G44 4.3; one or two game calls) | A |
| `int32_t u3h_kr_mode_type(uintptr_t base)` | one game call 0x721050(NGM); the mode type (s32 at +0xc of the entry), −1 without an entry | A |
| `int u3x_kr_friendly(uintptr_t base, const uint8_t *a, const uint8_t *b)` | "same side" (G44 4.2): 1 for the same address with no call; else one `u3h_kr_mode_type` call, 0 in mode type 0xe, else 1 when both are not null and their teams are equal | A, B |
| `uint32_t u3x_kr_sid_cat(uintptr_t base, uint32_t h, const char *text)` | a string id continued over a text (G44 4.6) | B |
| `void u3x_kr_prof_add(uintptr_t base, const uint8_t *np, uint32_t n, uint32_t amount)` | adds to profile counter n of the player's user when the game is online (G44 4.7; one game call 0x7270f0) | B |
| `void u3x_kill_popup(uintptr_t base, void *s, uint8_t *np, uint8_t *other, const uint8_t *info, int32_t pts, uint32_t type, uint32_t dbl)` | the "+points" popup (G44 4.10) | A, B |
| `void u3x_add_score(uintptr_t base, void *s, uint8_t *S, uint8_t *np, int32_t pts)` | player score (G44 4.11) | A |
| `void u3x_team_score(uintptr_t base, void *s, uint8_t *S, uint32_t team, int32_t pts)` | team score; only the lowest bit of `team` is used (G44 4.12) | A |
| `void u3x_award(uintptr_t base, void *s, uint8_t *S, uint8_t *np, int32_t pts, uint32_t type, uint32_t flag)` | cash, XP, missions for points (G44 4.13) | A, B |
| `int32_t u3x_ai_kill_points(uintptr_t base, uint32_t w, uint32_t id, uint8_t *knp, int b)` | point value of a killed AI (G44 4.14) | B |
| `int u3x_kr_34fc04(uintptr_t base)` | 1 when the mode type is 0xf (G44 4.13 table) | A |
| `int32_t u3x_kr_37c984(uintptr_t base)`, `int32_t u3x_kr_37c914(uintptr_t base)` | score values: in mode type 0xf from the mode's script data, otherwise 3 / 0 (G44 4.13 table) | A |
| `int32_t u3x_kr_37c9f4(uintptr_t base)` | the game's normal score for a kill (game call 0x7a8150) | A |
| `int32_t u3x_kill_points(uintptr_t base, uint32_t fn, uint8_t *np)` | game call (base + fn)(null, np): one of the game's kill point getters (3.7) | A |
| `int u3x_double_points(uintptr_t base, const uint8_t *np)` | 1 when the double-points event is running and is not this player's team's (null player: team 0) | A |
| `int u3x_kr_30ffa8(uintptr_t base, const uint8_t *np, int b)` | `u3h_kr_30feec(base)` once; 0: result 1; otherwise `b` for a non-null `np`, 0 for null | A |
| `void u3x_booster_kill(uintptr_t base, void *s, uint8_t *knp, uint32_t flags, uint32_t w)` | booster rewards of a kill (G44 4.18) | B |
| `void u3x_weapon_boosters(uintptr_t base, void *s, uint8_t *S, uint8_t *knp)` | weapon booster rewards (G44 4.19) | B |
| `void u3x_category_medals(uintptr_t base, void *s, uint8_t *S, uint8_t *knp, const uint8_t *info, int team_mode)` | weapon category stats (G44 4.20) | B |
| `void u3x_three_of_each(uintptr_t base, void *s, uint8_t *S, uint8_t *knp)` | "three of each" medal (G44 4.21) | B |
| the thirteen medal checks of G45 (signatures below) | each looks at the history and sends stat events | B |
| `int u3x_score_member(const uint8_t *S, uint32_t id)` | existing: index of the player `id` in the score keeper's member table (first match among the first min(count, 128) members), or −1. Reads only | B |
| `uint32_t u3x_weapon_stat_number(const uint32_t *fixed, const uint32_t *extra, uint32_t weapon)` | existing: the weapon's statistics number. The index k (0 .. 0x12) of the first of the 19 u32 at `fixed` equal to `weapon`; otherwise, when `extra` is not null (u32 count at +0, pointer at +8 to u32 ids), 0x13 + the index of the first of its first min(count, 64) ids equal to `weapon`; otherwise 0. Reads only | A |
| `uint8_t *u3x_handle_obj(uint8_t *h)` | existing: the object a game handle {pointer to an object slot at +0, u32 id at +8} still names, or null (stale or empty handle). Reads only | A, B |
| `int u3x_has_tag(const uint8_t *bucket_head, const uint8_t *tags, uint32_t id)` | existing: whether an object's tag set `tags` holds tag `id`, given the head of the tag table bucket of that id (walks the bucket's circular list, at most 4096 nodes). Reads only | B |

The medal checks (G45; all return nothing):

- `u3x_kr_379c18(uintptr_t base, void *s, uint8_t *knp, uint8_t *info)`
- `u3x_kr_37952c(uintptr_t base, void *s, uint8_t *S, uint8_t *knp)`
- `u3x_kr_3817b8(uintptr_t base, void *s, uint8_t *S, uint8_t *knp, uint8_t *info, int team_mode)`
- `u3x_kr_37992c(uintptr_t base, void *s, uint8_t *S, uint8_t *knp, uint8_t *info)`
- `u3x_kr_37ab00(uintptr_t base, void *s, uint8_t *S, uint8_t *knp, uint8_t *info)`
- `u3x_kr_37a4bc(uintptr_t base, void *s, uint8_t *S, uint8_t *knp)`
- `u3x_kr_379ed8(uintptr_t base, void *s, uint8_t *S)`
- `u3x_kr_379ff0(uintptr_t base, void *s, uint8_t *S, uint8_t *knp)`
- `u3x_kr_379430(uintptr_t base, void *s, uint8_t *S, uint8_t *knp)`
- `u3x_kr_3791ec(uintptr_t base, void *s, uint8_t *S, uint8_t *knp, uint8_t *vnp, uint8_t *info)`
- `u3x_kr_37a914(uintptr_t base, void *s, uint8_t *S, uint8_t *knp)`
- `u3x_kr_37a16c(uintptr_t base, void *s, uint8_t *S, uint8_t *knp)`
- `u3x_kr_37a32c(uintptr_t base, void *s, uint8_t *S, uint8_t *knp, uint8_t *info)`

Short words used in section 4:

- **Stat(id, amount, P)** = `u3h_kr_stat(base, s, id, amount, P)`.
- **Ask(a, b)** = `u3x_kr_friendly(base, a, b)`. Every Ask named in section 4 is really made, also where its result
  could be known from an earlier one (each Ask of two different records is one game call 0x721050, and the order
  and number of game calls is part of the behaviour).
- **Local(P)** = the address P equals the pointer at NGM + 0x370 or the pointer at NGM + 0x378.
- **Team(P)** = the s32 at P + 0x21c.
- **Low(x)** = the low 16 bits of x.
- **OnlineOrShown** = game call 0x7270f0(NGM); a non-zero u8 result: yes. Otherwise yes exactly when bit 0 of the
  u32 at `u3h_ext()` + 0x4824 is set (the "co-op cash is shown" switch); that word is read only when the call
  returned 0.

## 3. Game and helper facts

"NGM" = the net game manager, image 0x17cb060. "S" = the score keeper, image 0x17dd780.

### 3.1 The helper's state block (address from `u3h_ext()`)

All are u32; counters are plain increments (wrapping).

| Offset | Meaning |
|---|---|
| 0x2bc0 | counter: calls of `u3x_kr_port` |
| 0x2bc4 | counter: calls that found a mode entry |
| 0x2bd0 | the history count after the last recorded kill |
| 0x2bd4 | counter: deaths counted |
| 0x2bd8 | counter: kills counted |
| 0x2bdc, 0x2be0, 0x2be8 | the last kill's flag word, killer id, victim id |
| 0x2bec | counter: first group of medal checks run (4.11) |
| 0x2bf0 | counter: second group (4.13) |
| 0x2bf4 | counter: third group (4.14) |
| 0x2bf8 | counter: assists counted |
| 0x2bfc | counter: calls skipped by the "off" switch |
| 0x31e0 | counter: non-player victims that still had a character (4.12) |
| 0x31e4 | low 16 bits: counter of those skipped as "not counted"; the high 16 bits belong to other code and must be kept |
| 0x31e8 | counter: co-op AI stat events sent |
| 0x31ec | the id of the last of them |
| 0x4450 | switch word; bit 0 set = no doubling of the points in mode type 10 (4.7) |
| 0x4490 | counter: points doubled in mode type 10 |
| 0x4824 | bit 0: co-op cash is shown (see OnlineOrShown) |

### 3.2 The state object `s`

The u32 at `s` + 0x3d78 holds switch bits (those of G44 3.2 are in the same word):

| Bit value | Meaning when set |
|---|---|
| 0x200 | the whole kill record is off |
| 0x400 | no history record |
| 0x800 | no kill / death / assist counts |
| 0x2000 | no medal checks |
| 0x4000 | no points (here: also no "AI killed" note, 4.12 step 3) |
| 0x8000000 | no look at a non-player victim's character in 4.12 |
| 0x10000000 | no "not counted" mark from a non-player victim's character in 4.4 |

The word is read once, at the start of `u3x_kr_port`, for the bits 0x200, 0x400, 0x800, 0x2000; the other bits
are read where they are used.

### 3.3 Net game manager, net players, characters

| Where | Type | Meaning |
|---|---|---|
| NGM + 0x31d | u8 | "first kill of the match happened" |
| NGM + 0x370, NGM + 0x378 | pointers | the two local players' net player records |
| NGM + 0x6520 | pointer | the entity table: an array of pointers, one bucket per index 0 .. size − 1, plus one more pointer at index size (the table's end marker node) |
| NGM + 0x655c | u32 | the entity table's size (number of buckets) |
| image 0x17d5c61 | u8 | non-zero while a multiplayer match runs |
| image 0x17c3580 | pointer | the game clock object (may be null); u32 at +0xc = the clock value. "Now" = that value, 0 for a null pointer |

An entity table node: pointer at +0 = next node of the bucket (null ends it), u32 at +0x10 = the entity's net id,
a game handle (16 bytes) at +0x18.

Net player record fields used here (see also G44 3.3):

| Offset | Type | Meaning |
|---|---|---|
| 0x1e8 | u32 | local user number |
| 0x21c | s32 | team |
| 0x9a0 | s32 | clock value until which the last attacker counts |
| 0x9a4 | u32 | net player id of the last attacker |

A non-player character (the object of a victim handle):

| Offset | Type | Meaning |
|---|---|---|
| 0x38 | pointer | its tag set (may be null) |
| 0x708 | u64 | bit 48 = "not counted" (no points, no statistics); bit 49 = a marked enemy (4.12) |
| 0xbf8 | pointer | may be null; a non-zero u32 at +4 of it = the enemy was under a player's kickback / special state (4.12) |
| 0xc60 | s32 | enemy kind (2 and 4 matter) |

The tag table: image 0x1b3efd8. Its first u8 is non-zero once the table is built. The bucket head used here is at
image 0x1b3efd8 + 0x9e8 (the bucket of tag id 0xfd2c9c09).

Game functions:

| Image offset | Arguments | Result / meaning |
|---|---|---|
| 0x721050 | (NGM) | pointer to the current mode entry, or null; s32 at +0xc = mode type |
| 0x722670 | (NGM) | u8; non-zero in the modes with AI enemies (co-op). Called "the co-op flag" below; G44 and G45 receive it as `team_mode` |
| 0x7270f0 | (NGM) | u8; non-zero when the game is an online game |
| 0x729b10 | (NGM) | u8; non-zero when the mode gives score for a kill (4.7) |
| 0x77c880 | (image 0x17dc230, net player record, net player record) | u8; only asked in mode type 0xf: chooses between two point values for the first player's kill of / assist on the second (4.7, 4.9) |
| 0x773b50 | (image 0x17da200 + 0x1018·user, u32 weapon id, u32 1, u32 0, u32 weapon id, u32 byte) | the per-user mission tracker's "kill with weapon" note; no result used |
| 0x661ca0 | (image 0x17c3404, u32 byte, pointer to an s32) | the game's "AI enemy killed" note; no result used |
| 0x11f2bf0 | (u32 symbol id) | pointer to a script symbol's slot, or null; the pointer stored in the slot is the symbol's data |
| 0x7aa560 | (S) | recompute the places |

Mode types named here: 0xe = every player for himself; 0xf, 10 and 0x12 = three modes with special rules (4.6,
4.7, 4.9); 0xd = co-op (inside `u3h_kr_30feec`).

### 3.4 The score keeper S

| Offset | Type | Meaning |
|---|---|---|
| 0xe0 | u32 | number of kill records |
| 0xe4 + 0x28·i, i = 0 .. 0x1ff | record | kill record i; record 0 is the newest |
| 0x60cc + 0x130·m | record | player score record of member m |

The kill record (0x28 bytes):

| Offset | Type | Meaning |
|---|---|---|
| 0 | u8 | kind: 0 normal, 1, 2 and 6 by message flags (4.3) |
| 2 | u16 | killer's net player id (low 16 bits), 0 without a player killer |
| 4 | u16 | victim's net player id, 0 without a player victim |
| 6 | s8 | killer's team (−1 unknown) |
| 7 | s8 | victim's team (−1 unknown) |
| 8 | s32 | time of the kill (message clock) |
| 0xc | u8 | weapon statistics number |
| 0xe | 3 × s16 | killer position x, y, z in sixteenths |
| 0x14 | 3 × s16 | victim position x, y, z in sixteenths |
| 0x1c | s32 | copy of the message's s32 at +0x6c |
| 0x21 | u8 | 1 = message flag bit 19 |
| 0x22 | u8 | 1 = message flag bit 20 |
| 0x23 | u8 | 1 = message flag bit 27 |
| 0x24 | u8 | 1 = message flag bit 13 |
| 0x25 | u8 | 1 = the kill is not counted |
| 1, 0xd, 0x1a, 0x1b, 0x20, 0x26, 0x27 | u8 | never written by this group |

A player score record (0x130 bytes), fields this group changes (see G44 3.4 for +2 / +4):

| Offset | Type | Meaning |
|---|---|---|
| 8 | s16 | kills |
| 0xa | s16 | kills of non-player victims |
| 0xc | s16 | deaths |
| 0xe | u16 | assists |
| 0x10 | s16 | suicides |
| 0x12 | s16 | kills of the own side (suicides included) |
| 0x48, 0x4e, 0x54, 0x58 | u16 each | counts of kills with message flag 4, 0x40, 0x80, 0x200 |
| 0x56, 0x57, 0x64, 0x65, 0x6a | u8 each | counts: flag 0x100; bonus kills (4.6, 4.8); kind-2 enemies with weapon 0xb934b2b2; flag 0x2000000; flag 8 against an enemy |
| 0x6c | s16 | kills made on team 1 while the co-op flag is set |
| 0xee + 2·j, j = 0 .. 0x1a | s16 each | kills per weapon slot j (3.8); slot 0 = any other weapon |

### 3.5 The kill message `info`

| Offset | Type | Meaning |
|---|---|---|
| 0 | s32 | time of the kill |
| 0x10, 0x14, 0x18 | u32 float patterns | killer position x, y, z |
| 0x20, 0x24, 0x28 | u32 float patterns | victim position x, y, z |
| 0x30 | u32 | killer's net player id |
| 0x34 | u32 | victim's net id (a player id, or an entity id) |
| 0x38 | u32 | weapon id |
| 0x3c | u32 | only its low byte is used (passed to 0x773b50) |
| 0x40 + 4·i, i = 0 .. 7 | u32 | net player ids of up to eight assisting players (0 = none) |
| 0x60 | u32 | flag word (3.6) |
| 0x64 | u8 | passed to 0x661ca0 |
| 0x68 | u32 | kind id of a killed AI (for `u3x_ai_kill_points`) |
| 0x6c | s32 | copied into the record |
| 0x70 | u32 | string id of the victim's kind; continued with "-killed" it names a stat event |

### 3.6 The flag word and the stat events

The PS4 executable holds none of the stat event ids below (the function that sent them is gone; they only occur in
the game's script data as medal and mission conditions). Each is a single id with its own rule; the names behind
them are not known (section 7, question 2).

Flag bits that steer the flow:

| Bit value | Meaning |
|---|---|
| 1 | the killer is a player (id at +0x30) |
| 2 | the victim is a player (id at +0x34) |
| 0x20 | record kind 6 (explosive; also read by G44) |
| 4 | record kind 1, and stat 0xb759f1c7 (4.13) |
| 0x40 | record kind 2, and stat 0xaa22db16 (4.13) |
| 0x200 | the kill counts in a co-op-style mode (4.2 step 4, 4.9, 4.12), and stat 0x77240247 |
| 0x2000 | record byte 0x24; stat 0xa622ff01 (4.11) |
| 0x4000 | "bonus kill": other score rules (4.5 to 4.8) |
| 0x40000 | extra score in the scoring modes (4.7) |
| 0x80000 | record byte 0x21: a betrayal / revenge kill |
| 0x100000 | record byte 0x22 |
| 0x200000 | the kill's score is the fixed value instead of the game's (4.5, 4.6) |
| 0x8000000 | record byte 0x23; stats of 4.11 step 3 |

Flag bits that only send one stat event each, Stat(id, 1, killer), in 4.11 and 4.13:

| Bit value | Stat id | | Bit value | Stat id |
|---|---|---|---|---|
| 0x10 | 0x5cff7436 | | 0x400000 | 0x6685348e |
| 0x400 | 0xcff265ed | | 0x800000 | 0x0b854093 |
| 0x800 | 0x3838b544 | | 0x1000000 | 0xcd644ff7 |
| 0x1000 | 0x68774d94 | | 0x4000000 | 0x78573763 |
| 0x8000 | 0x87cbf9cb | | 0x10000000 | 0xd323c81d |
| 0x10000 | 0xe5ad0b24 (only without flag 0x2000) | | 0x20000000 | 0xdc380255 |
| 0x20000 | 0xb4210460 | | 0x40000000 | 0x1e8153a6 |

Flag bits with a running count (the count is the stat's amount): 8 → 0x89c96ded (amount 1, counter 0x6a), 0x80 →
0x744a942b, 0x100 → 0x76f0c8c8, 0x2000000 → 0x15fd6bcd; details in section 4.

Other stat ids: 0x5c494554 (kills of the match), 0xb477093d (bonus kills), 0x5258a989 (avenged a team mate),
0xd4f91f9a, 0xbb000af1, 0x5398047b, 0xc2b00a95, 0xd294ccd1 (long shot), 0x1bdbd578 (first kill of the match),
0xffc1c7b5, 0x58b0d53d (kills without dying), 0x6f593a9b (deaths without a kill), 0x83daf02c (assists),
0x0c65fecd, 0x9c498eb3, 0x35cfda1f, 0x38771768 (co-op AI victims, 4.12).

Single weapon ids: 0x3d2f108b (one of the group-4 weapons, 3.9), 0xb934b2b2 and 0xc58ae515 ("net-kill-stealth" by
the string id rule of G23 3.9) (two of the group-6 ids).

Script symbol 0x9e354807: the list of extra statistics weapons (u32 count at +0, pointer at +8 to the ids), the
`extra` argument of `u3x_weapon_stat_number`.

Profile counters (the n of `u3x_kr_prof_add`): 0x17b = AI enemies killed, 0x17c = kills with a group-6 id,
0x17d = stealth kills.

### 3.7 The game's kill point getters

Called through `u3x_kill_points(base, fn, player)`; each returns an s32 number of points for that player.

| fn (image offset) | Used for |
|---|---|
| 0x6621a0 | a plain kill |
| 0x662220 | a kill in a mode without kill score, not co-op (4.7) |
| 0x6622a0 / 0x662320 | a kill in mode type 0xf, 0x77c880 non-zero / zero |
| 0x6623a0 | an assist |
| 0x6624a0 / 0x662520 | an assist in mode type 0xf, 0x77c880 non-zero / zero |

### 3.8 Image tables

| Image offset | Layout | Meaning |
|---|---|---|
| 0x13a6170 | 19 × u32 | the fixed statistics weapons; index = statistics number (entry 0 is 0) |
| 0x13a66d0 | u32 entries; entries 1 .. 0x1a are used | the weapon slots of the per-weapon kill counts |

Constants: 16.0 = pattern 0x41800000; 900.0 = pattern 0x44610000 (30 squared); 0x2ee = 750 clock units (2.5 s).

### 3.9 The weapon groups: read from the image, no literal list

The old code holds a literal list of ten weapon ids in two groups ("6": the close-combat and stealth kill kinds,
which the game treats as weapons; "4": the thrown explosives). The PS4 executable has no table of exactly these
two groups, but both sets sit in the two tables of 3.8, at fixed places (checked against the image file: the ten
old values are exactly these entries):

- **Group 4** = the four u32 at image 0x13a66d0 + 4·j for j = 20, 21, 22, 23 (the last four thrown weapons of the
  weapon slot table).
- **Group 6** = the six u32 at image 0x13a6170 + 4·k for k = 1, 6, 7, 8, 9, 13 (statistics numbers of the
  close-combat kill kinds).

The new code reads them at run time. Nothing else of the old list is kept.

## 4. Behaviour

### Part A

### 4.1 `u3x_kr_port(base, info, s)`: start

1. bits = the u32 at `s` + 0x3d78. Counter 0x2bc0 + 1.
2. bits has 0x200: counter 0x2bfc + 1; stop. Nothing else is read or called.
3. M = game call 0x721050(NGM). Null: stop.
4. Counter 0x2bc4 + 1. counts = bits has no 0x800. medals = bits has no 0x2000. coop = game call 0x722670(NGM) is
   not 0. T0 = the s32 at M + 0xc; ffa = (T0 is 0xe).
5. flags, killer_id, victim_id, weapon = the u32 at `info` + 0x60, + 0x30, + 0x34, + 0x38. Words 0x2bdc, 0x2be0,
   0x2be8 = flags, killer_id, victim_id.
6. n = the u32 at S + 0xe0. R0 = kill record 0 (the record at S + 0xe4), whatever follows.

### 4.2 `u3x_kr_port`: the frame

After 4.1, in this order:

1. Unless bits has 0x400: the history step (4.3).
2. knp = `u3h_kr_np_by_id(base, killer_id)` when flags has bit 1, else null (no call). Then vnp =
   `u3h_kr_np_by_id(base, victim_id)` when flags has bit 2, else null (no call).
3. When flags has no bit 2: the victim entity step (4.4).
4. `u3h_kr_30feec(base)` is called (always). Non-zero and flags has no 0x200: R0's byte 0x25 = 1.
5. krec = `u3h_kr_prec(S, knp)`, then vrec = `u3h_kr_prec(S, vnp)` (both always called).
6. knp not null: R0's killer team = the low 8 bits of Team(knp). vnp not null: R0's victim team likewise.
7. R0's byte 0x22 = flag bit 20; R0's byte 0x21 = flag bit 19 (each 0 or 1). "Betrayal mark" below = R0's byte 0x21
   as now stored.
8. **Suicide** (knp and vnp not null and the same address): self_kill = 1. With counts: the s16 at vrec + 0xc + 1,
   then the s16 at krec + 0x10 + 1, counter 0x2bd4 + 1. Then kill = the player kill step (4.5).
9. **Otherwise**: self_kill = 0.
   - ffa: R0's killer team = 0 and victim team = 1.
   - vnp not null and counts: the s16 at vrec + 0xc + 1; counter 0x2bd4 + 1.
   - knp null (no player killer): the assists step (4.9), then game call 0x7aa560(S); stop.
   - vnp not null: kill = the player kill step (4.5).
   - vnp null (a non-player victim): on = OnlineOrShown; kill = yes.
10. The kill context (2.2) is filled from the values above (`coop`, `counts`, `medals`, `self_kill`, `on` as 0 / 1;
    `on` is the value set in 4.5 step 2 or in step 9).
11. kill is yes: `u3x_kr_counted(&context)` (4.10).
12. `u3x_kr_aftermath(&context)` (4.13).
13. The assists step (4.9).
14. Game call 0x7aa560(S).

With switch 0x400 the history is not moved and record 0 is not filled, but every later step still writes into and
reads from record 0 and the old records as they are (section 6, note 6).

### 4.3 The history step

1. The records move down by one: for i from min(n − 1, 0x1fd) down to 0 (nothing for n = 0), the bytes 0 .. 0x25
   of record i are copied to record i + 1. The bytes 0x26 and 0x27 of a record are not copied. Record 0x1ff is
   never written by this move (section 6, note 1).
2. Record 0 is then written, field by field (its other bytes keep what they held):
   - killer = Low(killer_id) when flags has bit 1, else 0; victim = Low(victim_id) when flags has bit 2, else 0;
   - time = the s32 at `info` + 0;
   - weapon statistics number = the low 8 bits of `u3x_weapon_stat_number(image 0x13a6170, X, weapon)`, where
     L = game call 0x11f2bf0(0x9e354807) and X = the pointer stored at L, or null when L is null. The game call
     is made after the move of step 1;
   - the six positions: for each of the killer coordinates (message +0x10, +0x14, +0x18 → record +0xe, +0x10,
     +0x12) and victim coordinates (message +0x20, +0x24, +0x28 → record +0x14, +0x16, +0x18): the low 16 bits
     of `f_to_int(f_mul(coordinate, 16.0))` (one rounding of the product, truncation toward zero; a NaN or a
     product outside the s32 range gives 0x80000000, so 0 is stored);
   - killer team = victim team = −1;
   - the s32 at +0x1c = the s32 at `info` + 0x6c;
   - byte 0x23 = flag bit 27; byte 0x24 = flag bit 13;
   - kind = 6 when flags has 0x20; otherwise 1 when it has 4; otherwise 2 when it has 0x40; otherwise 0;
   - byte 0x25 = 0.
3. When n is below 0x200: n = n + 1 and the u32 at S + 0xe0 = n. (From here on "n" is this new value.) When n is
   0x200 or more it stays.
4. Word 0x2bd0 = n.

### 4.4 The victim entity step (only for a victim that is not a player)

1. h = the 32-bit FNV-1a hash of the four bytes of victim_id, lowest byte first (start 0x811c9dc5; for each
   byte: h = (h XOR byte) × 0x01000193, 32-bit wrapping).
2. size = the u32 at NGM + 0x655c; table = the pointer at NGM + 0x6520. When size is not 0 and table is not null:
   the nodes of bucket (h modulo size) are walked from the bucket's pointer along the next pointers until a node
   whose id (u32 at +0x10) equals victim_id, or the end (null). A node found that way, when its address differs
   from the pointer stored at table index size (the end marker), gives victim_entity = that node + 0x18. In every
   other case victim_entity is null.
3. When victim_entity is not null and the switch 0x10000000 (read now from `s` + 0x3d78) is clear: C =
   `u3x_handle_obj(victim_entity)`; when C is not null and bit 48 of the u64 at C + 0x708 is set: R0's byte 0x25 = 1.

### 4.5 The player kill step (killer and victim are players; result: "the kill counts as a kill")

1. Ask(knp, vnp). Yes: with counts, the s16 at krec + 0x12 + 1; then, with the betrayal mark, the betrayal credit
   (4.6).
2. on = OnlineOrShown.
3. Not a suicide: Ask(knp, vnp) again.
   - No: Ask(knp, vnp) once more; when that is also no: the enemy kill step (4.8). Go to step 6.
4. (suicide, or the Ask of step 3 said yes) Without the betrayal mark, the other team scores: t = 1 when
   Team(vnp) is 0, else 0;
   - flags has no 0x4000: when flags has 0x200000: `u3x_team_score(base, s, S, t, p)` with p = 0 when
     `u3x_kr_34fc04(base)` is non-zero, else 3 (the call is made also for p = 0). Without 0x200000: nothing.
   - flags has 0x4000: `u3x_team_score(base, s, S, t, u3x_kr_37c984(base))`.
5. With the betrayal mark and not a suicide: one more Ask(knp, vnp), its result unused.
6. Result = the opposite of a last Ask(knp, vnp).

So a suicide makes no Ask call at all (same address) and never counts; a kill of the own side makes three Asks
(four with the betrayal mark) and does not count; a kill of an enemy makes four and counts.

### 4.6 The betrayal credit

A kill of the own side (or a suicide) that carries the betrayal mark gives the kill to the player who attacked
the killer last.

1. a = the u32 at knp + 0x9a4 when (the s32 at knp + 0x9a0) − Now, as a wrapping 32-bit signed difference, is
   greater than 0; otherwise 0. A = `u3h_kr_np_by_id(base, a)` (called also for 0).
2. "Kill score" = when flags has 0x200000: 0 when `u3x_kr_34fc04(base)` is non-zero, else 3; without 0x200000:
   `u3x_kr_37c9f4(base)`.
3. A null:
   - flags has no 0x4000: `u3x_team_score(base, s, S, 1 − Team(knp), kill score)`;
   - flags has 0x4000: when `u3x_kr_34fc04(base)` is 0: `u3x_team_score(base, s, S, 1 − Team(knp),
     u3x_kr_37c984(base))`; otherwise nothing.
4. A not null and Team(A) differs from Team(knp): R = `u3h_kr_prec(S, A)`; with counts, the s16 at R + 8 + 1 (the
   kill counter 0x2bd8 is not touched). Then:
   - flags has 0x4000: the u8 at R + 0x57 + 1; `u3x_add_score(base, s, S, A, u3x_kr_37c984(base))`;
     Stat(0xb477093d, the u8 at R + 0x57, A);
   - otherwise: `u3x_add_score(base, s, S, A, kill score)`.
5. A not null on the killer's team: nothing.

### 4.7 The kill's points (flags without 0x4000; result: points to award, or "no award")

"kp(fn)" = `u3x_kill_points(base, fn, knp)`. "Bonus count" = the u8 at krec + 0x57 + 1, then Stat(0xb477093d, that
u8, knp).

g = game call 0x729b10(NGM).

- **g is 0** (the mode gives no score for a kill):
  - T0 is 10 and on: points = kp(0x6621a0); when bit 0 of the word at ext 0x4450 is clear: points doubled
    (32-bit shift) and counter 0x4490 + 1.
  - otherwise, ffa: `u3x_add_score(base, s, S, knp, 1)`; then, when on: points = kp(0x6621a0); not on: no award.
  - otherwise: when on and not coop: points = kp(0x662220); else no award.
- **g is not 0**:
  1. `u3x_add_score(base, s, S, knp, kill score)` (kill score as in 4.6 step 2).
  2. flags has 0x40000: `u3x_add_score(base, s, S, knp, u3x_kr_37c914(base))`, then the bonus count.
  3. Not on: no award.
  4. T0 is not 0xf: points = kp(0x6621a0). T0 is 0xf: game call 0x77c880(image 0x17dc230, knp, vnp); non-zero:
     points = kp(0x6622a0), zero: points = kp(0x662320).

### 4.8 The enemy kill step

1. flags has no 0x4000: the points of 4.7. When there is an award and points is not 0:
   - d = 0. `u3x_double_points(base, knp)` non-zero: points doubled, d = 1. With the betrayal mark: points doubled
     (again), d = 1.
   - Local(knp): `u3x_kill_popup(base, s, knp, vnp, info, points, 0, d)`.
   - `u3x_award(base, s, S, knp, points, 0, 0)`.
2. flags has 0x4000: `u3x_add_score(base, s, S, knp, u3x_kr_37c984(base))`, then the bonus count (4.7).
3. ffa: done.
4. "Avenged a team mate": the records i = 1, 2, .. below n are looked at in order. The search ends at the first
   record whose time is more than 0x2ee before the message's time ((message time − record time) as s32 > 0x2ee).
   The first record before that with: killer team ≠ victim team, killer = Low(victim_id), victim ≠
   Low(killer_id), and victim team (s8) = Team(knp) gives Stat(0x5258a989, 1, knp) and ends the search.

### 4.9 The assists step

ffa: nothing. Otherwise, for i = 0 .. 7 in turn:

1. kt = R0's killer team (s8). `u3h_kr_mode_type(base)` is called; when it is 0x12 and kt is −1: kt = 1.
2. a = the u32 at `info` + 0x40 + 4·i. A = `u3h_kr_np_by_id(base, a)` for a non-zero a, else null (no call). Next
   i when A is null, when Team(A) equals R0's victim team, or when Team(A) differs from kt.
3. R = `u3h_kr_prec(S, A)`. With counts: the u16 at R + 0xe + 1; counter 0x2bf8 + 1.
4. OnlineOrShown (asked again for every assister): when yes:
   - `u3h_kr_mode_type(base)` again. 0xf: game call 0x77c880(image 0x17dc230, A, vnp); non-zero: points =
     `u3x_kill_points(base, 0x6624a0, A)`, zero: `u3x_kill_points(base, 0x662520, A)`. Any other type: points =
     `u3x_kill_points(base, 0x6623a0, A)` × `u3x_kr_30ffa8(base, knp, flag bit 9 as 0 / 1)` (the point getter is
     called first; 32-bit product).
   - `u3x_double_points(base, knp)` non-zero (the killer is passed, also when null): points doubled.
   - Local(A): `u3x_kill_popup(base, s, A, vnp, info, points, 1, 0)`.
   - `u3x_award(base, s, S, A, points, 0, 0)` (also for 0 points).
5. Stat(0x83daf02c, the u16 at R + 0xe, A) (always, also when nothing was counted or awarded).

`vnp` and `knp` may be null here.

### Part B

All values named without a prefix are the fields of the kill context. R0 = kill record 0 of S; "record teams
differ" = its killer team byte differs from its victim team byte. Stat(id, amount) with two arguments goes to
`knp`. Group(w) = `u3x_kr_weapon_group(base, w)`.

### 4.10 `u3x_kr_counted(k)`: a kill of an enemy player or of a non-player victim

1. With counts:
   - the s16 at krec + 8 + 1; counter 0x2bd8 + 1;
   - when the u8 at image 0x17d5c61 is not 0, then game call 0x722670(NGM) returns non-zero (called only when the
     byte is not 0), and Team(knp) is 1: the s16 at krec + 0x6c + 1;
   - m = `u3x_score_member(S, killer_id)`. When m ≥ 0: j = the first of 1 .. 0x1a with the u32 at image 0x13a66d0
     + 4·j equal to weapon, or 0 when none; the s16 at S + 0x60cc + 0x130·m + 0xee + 2·j + 1.
2. Stat(0x5c494554, the u16 at krec + 8).
3. Local(knp): game call 0x773b50(image 0x17da200 + 0x1018·(u32 at knp + 0x1e8), weapon, 1, 0, weapon, the low
   byte of the u32 at `info` + 0x3c).
4. R0's byte 0x25 is 0: the kill statistics (4.11).
5. `u3x_booster_kill(base, s, knp, flags, weapon)`.
6. victim_entity not null and counts: the s16 at krec + 0xa + 1.
7. coop: the co-op AI victim step (4.12).

### 4.11 The kill statistics

In this order:

1. flags has 0x2000000: the u8 at krec + 0x65 + 1; Stat(0x15fd6bcd, that u8).
2. flags has 0x40000000: Stat(0x1e8153a6, 1).
3. flags has 0x8000000: c = the number of records 0 .. n − 1 whose byte 0x23 is not 0 (any killer); Stat(0xd4f91f9a,
   c). Then Group(weapon) is 6: Stat(0xbb000af1, 1); is 4: Stat(0x5398047b, 1); neither: step 4 is skipped.
4. weapon is 0x3d2f108b: Stat(0xc2b00a95, 1).
5. flags has 0x4000000: Stat(0x78573763, 1). Has 0x10000000: Stat(0xd323c81d, 1). Has 0x20000000:
   Stat(0xdc380255, 1).
6. flags has 0x100: the u8 at krec + 0x56 + 1; Stat(0x76f0c8c8, that u8).
7. flags has 0x200: the u16 at krec + 0x58 + 1; Stat(0x77240247, that u16).
8. One Stat(id, 1) for each of these flags that is set, in this order: 0x400, 0x8000, 0x800, 0x1000, 0x20000,
   0x400000, 0x800000, 0x1000000 (ids in 3.6).
9. Stat(`u3x_kr_sid_cat(base, the u32 at info + 0x70, "-killed")`, 1).
10. flags has 0x2000: c = the number of records 0 .. n − 1 with byte 0x24 not 0, killer = Low(killer_id) and
    differing teams; Stat(0xa622ff01, c). Otherwise, flags has 0x10000: Stat(0xe5ad0b24, 1).
11. Group(weapon) is 4 (asked again): the long shot. sum = pattern 0 (+0.0); for the axes x, y, z in turn: d =
    `f_sub`(killer coordinate, victim coordinate) of the message's float patterns; sum = `f_add`(sum, `f_mul`(d,
    d)). When neither sum nor 900.0 is a NaN and sum is not less than 900.0: Stat(0xd294ccd1, 1).
12. The u8 at NGM + 0x31d is 0: Stat(0x1bdbd578, 1), then that u8 = 1.
13. With medals: `u3x_kr_379c18(base, s, knp, info)`, `u3x_kr_37952c(base, s, S, knp)`, counter 0x2bec + 1.

### 4.12 The co-op AI victim step

1. C = `u3x_handle_obj(victim_entity)` when victim_entity is not null and switch 0x8000000 (read now) is clear;
   otherwise null (no call).
2. C not null: counter 0x31e0 + 1. When bit 48 of the u64 at C + 0x708 is set: the low 16 bits of the word at
   0x31e4 + 1 (wrapping inside 16 bits, the high 16 bits unchanged); stop.
3. t = a local copy of the s32 at `info` + 0. Unless switch 0x4000 (read now) is set: game call 0x661ca0(image
   0x17c3404, the u8 at `info` + 0x64, pointer to t).
4. points = `u3x_ai_kill_points(base, weapon, the u32 at info + 0x68, knp, flag bit 9 as 0 / 1)`.
5. Local(knp): when on: `u3x_kill_popup(base, s, knp, null, info, points, 0, 0)`; then (on or not)
   `u3x_kr_prof_add(base, knp, 0x17b, 1)`.
6. on: `u3x_award(base, s, S, knp, points, 0, 0)`.
7. C null: stop.
8. "AI stat(id, amount)" = Stat(id, amount), then counter 0x31e8 + 1, then word 0x31ec = id.
   - bit 49 of the u64 at C + 0x708 set: AI stat(0x0c65fecd, 1).
   - K = the pointer at C + 0xbf8. K not null and the u32 at K + 4 not 0: AI stat(0x9c498eb3, 1). Otherwise, when
     the character does not carry tag 0xfd2c9c09 (it carries it when the pointer at C + 0x38 is not null, the
     first u8 of the tag table is not 0, and `u3x_has_tag(image 0x1b3efd8 + 0x9e8, that pointer, 0xfd2c9c09)` is
     non-zero): kind = the s32 at C + 0xc60;
     - kind 2 and weapon is 0xb934b2b2: the u8 at krec + 0x64 + 1; AI stat(0x35cfda1f, that u8);
     - kind 4: AI stat(0x38771768, 1).

This step runs for every counted kill while the co-op flag is set, also when the victim is a player (then C is
null and steps 3 to 6 still run).

### 4.13 `u3x_kr_aftermath(k)`

When R0's byte 0x25 is 0, in this order:

1. Not a suicide:
   - when vnp is not null: Ask(knp, vnp) (one call for every non-null victim); no, and flags has 0x80: the u16
     at krec + 0x54 + 1; Stat(0x744a942b, that u16);
   - flags has 4: the u16 at krec + 0x48 + 1; Stat(0xb759f1c7, that u16);
   - flags has 0x40: the u16 at krec + 0x4e + 1; Stat(0xaa22db16, that u16).
2. flags has 0x10: Stat(0x5cff7436, 1).
3. With medals: `u3x_kr_3817b8(base, s, S, knp, info, coop)`.
4. Group(weapon) is 6:
   - Local(knp): `u3x_kr_prof_add(base, knp, 0x17c, 1)`; then, when weapon is 0xc58ae515: `u3x_kr_prof_add(base,
     knp, 0x17d, 1)`;
   - with medals: `u3x_kr_37992c(base, s, S, knp, info)`.

   Otherwise, with medals: `u3x_category_medals(base, s, S, knp, info, coop)`.
5. With medals: `u3x_kr_37ab00(base, s, S, knp, info)`, `u3x_kr_37a4bc(base, s, S, knp)`, `u3x_kr_379ed8(base, s,
   S)`, `u3x_kr_379ff0(base, s, S, knp)`, `u3x_kr_379430(base, s, S, knp)`, counter 0x2bf0 + 1.
6. R0's byte 0x22 is not 0, R0's two team bytes are equal, not a suicide and R0's byte 0x21 is 0:
   Stat(0xffc1c7b5, 1).
7. vnp not null: the streaks (4.14).

Then, whatever byte 0x25 is: Local(knp): `u3x_weapon_boosters(base, s, S, knp)`.

### 4.14 The streaks

1. flags has 8 and the record teams of R0 differ: Stat(0x89c96ded, 1), then the u8 at krec + 0x6a + 1.
2. Kills without dying: c = 0; the records 0 .. n − 1 in order; a record with byte 0x25 not 0 is skipped; the
   walk ends at a record whose victim is Low(killer_id); a record whose killer is Low(killer_id) ends the walk
   when its victim team equals R0's killer team, otherwise c + 1. Stat(0x58b0d53d, c).
3. With medals: `u3x_kr_37a32c(base, s, S, knp, info)`.
4. Deaths without a kill: c = 0; same walk; a skipped record as before; the walk ends at a record whose killer
   is Low(victim_id); a record whose victim is Low(victim_id) ends the walk when its victim team equals R0's
   killer team, otherwise c + 1. Stat(0x6f593a9b, c, vnp) (to the victim).
5. With medals: `u3x_kr_3791ec(base, s, S, knp, vnp, info)`, `u3x_kr_37a914(base, s, S, knp)`,
   `u3x_three_of_each(base, s, S, knp)`, `u3x_kr_37a16c(base, s, S, knp)`, counter 0x2bf4 + 1.

In both walks the "victim is ..." test of a record comes before its "killer is ..." test (step 2) and the
"killer is ..." test before the "victim is ..." test (step 4), so a suicide record ends them.

### 4.15 `u3x_kr_weapon_group(base, weapon)`

6 when `weapon` equals one of the six group-6 entries of 3.9; otherwise 4 when it equals one of the four group-4
entries; otherwise 0. Reads the image only. (A `weapon` of 0 gives 0: none of the ten entries is 0.)

## 5. Test scenarios

Everything can be tested on a fake image with call-recording stubs (the test compares memory, results, and the
order and arguments of every call); the live game is only needed for the checks of section 7.

Part A (stubs for 0x721050, 0x722670, 0x7270f0, 0x729b10, 0x77c880, 0x11f2bf0, 0x7aa560 and for every function of
2.3 that part A calls; `u3x_kr_counted` / `u3x_kr_aftermath` as stubs that record the context's contents;
`u3x_weapon_stat_number` and `u3x_handle_obj` real):

1. Switch 0x200 (two counters only); no mode entry (two counters, one game call).
2. History: n = 0, 1, 5, 0x1fd, 0x1fe, 0x1ff, 0x200, 0x201, each with recognisable record contents (which records
   moved, which bytes kept, record 0x1ff untouched, count stored or not); switch 0x400 (nothing moved, record 0
   still gets teams, bytes 0x21 / 0x22 / 0x25).
3. Record 0 contents: each kind (flags 0x20, 4, 0x40 and combinations), flags 1 / 2 clear (ids 0), weapon in the
   fixed list, in the extra list, unknown, symbol slot null; positions: fractions (truncation toward zero, also
   negative), values whose product leaves 16 bits (low 16 bits kept), beyond the s32 range, NaN, infinite.
4. Victim entity: size 0; null table; empty bucket; hit at the first node, after several nodes; id absent; the
   hit node being the end marker; character with / without bit 48; stale handle; switch 0x10000000.
5. Co-op-style mode with and without flag 0x200 (byte 0x25).
6. Suicide with / without counts; with the betrayal mark (4.6 runs); flags 0x4000 / 0x200000 / neither in step 4
   of 4.5; mode type 0xf.
7. Kill of the own side: Ask counts 3 and 4; betrayal credit with: timer expired, attacker unknown (team score,
   each flag case), attacker on the other team (counts on / off, flag 0x4000 or not), attacker on the own team.
8. Enemy kill: every branch of 4.7 (g zero / non-zero; T0 10, 0xe, 0xf, other; on / not on; coop / not; ext
   0x4450 bit 0; flag 0x40000; 0x77c880 both results); points 0 (no popup, no award); double points; betrayal
   mark; both (×4); local and remote killer; flag 0x4000; the avenge search: hit at record 1, hit after others,
   stopped by the time limit, killer's own death excluded, n = 0 and n = 1, ffa (no search).
9. No player killer: only the assists and the places call; with ffa (nothing but the places call).
10. Non-player victim: context `on` from OnlineOrShown (online; offline with ext 0x4824 bit 0 set / clear).
11. Assists: ids 0, unknown id, assister on the victim's team, on another team than the killer's, mode type 0x12
    with killer team −1; counts off; not online (only the stat); mode type 0xf both ways; multiplier 0 and 1;
    double points; local / remote assister; eight valid assisters (call order).
12. The context handed to part B for each of the cases above.

Part B (context built by the test; stubs for 0x722670, 0x773b50, 0x661ca0 and for every function of 2.3 that part
B calls; `u3x_score_member`, `u3x_has_tag`, `u3x_handle_obj` real):

13. `u3x_kr_weapon_group`: each of the ten image entries; 0; an id of the neighbouring entries (0); a changed
    image (the function follows the image).
14. Counted kill: counts on / off; match byte 0 (no 0x722670 call) / non-zero with flag and team 0 / 1; member
    unknown; weapon in slot 1, 0x1a, not listed (slot 0); local / remote; byte 0x25 set (no statistics);
    victim entity with counts; coop on / off.
15. Statistics: every flag alone and all together (order of the stat events); flag 0x8000000 with group 6, 4, 0
    and weapon 0x3d2f108b (the skipped step); the two history counts with mixed records; the long shot at sum
    just below, equal to and above 900, with NaN and infinite coordinates; first kill byte 0 / 1; medals off.
16. Co-op AI victim: no entity; switch 0x8000000; stale handle; bit 48 (counter low 16 bits, wrap at 0xffff with
    the high half kept); switch 0x4000; local / remote and on / not on (popup, profile add, award); bit 49;
    special state pointer null / with 0 / with a value; tag carried / not / table not built / null tag set;
    kinds 2 (right and wrong weapon), 4, other.
17. Aftermath: byte 0x25 set (only the booster call for a local killer); suicide; victim null; enemy / own side
    with flag 0x80; flags 4, 0x40, 0x10; group 6 local with and without the stealth id; other weapon; medals off
    (no check, no category call); the 0xffc1c7b5 conditions one by one.
18. Streaks: flag 8 with equal / differing teams; walks with uncounted records, an own death first, a team kill
    first, a suicide record, n = 0; the victim's stat going to the victim; medals off.

## 6. Old behaviour to keep (looks like a bug or dead code)

1. The history move never writes record 0x1ff and leaves the bytes 0x26 / 0x27 of every record alone: the move
   stops one record early, so when the count reaches 0x200 the oldest record that the walks read (0x1ff) is
   memory this group never wrote. The count is never limited when it is read: a count above 0x200 found in memory makes every
   walk run past the 512 records.
2. Record 0 is not cleared before it is written: its bytes 1, 0xd, 0x1a, 0x1b, 0x20 keep the previous newest
   record's values.
3. The same-side question is asked again and again in 4.5 (up to four times) although nothing changes in
   between; each ask is a game call and is kept. The branches of the old code for "no victim record" inside the
   player kill step cannot be reached (it is only entered with both records) and are not described.
4. A `u3x_team_score` call is made also with 0 points (4.5 step 4). In 4.6 the team number is 1 − Team(knp), of
   which `u3x_team_score` uses the lowest bit only.
5. The betrayal credit counts the attacker's kill in its record but not in counter 0x2bd8.
6. With switch 0x400 the frame still writes the team bytes and the bytes 0x21, 0x22, 0x25 into the old record 0,
   and byte 0x25 is then never reset to 0 by this group.
7. The count for stat 0xd4f91f9a (4.11 step 3) counts the records of all killers.
8. The victim's "deaths without a kill" walk (4.14 step 4) compares each record's victim team with the newest
   record's killer team.
9. The co-op AI victim step runs for player victims too while the co-op flag is set (4.12), and its popup /
   award use the points of an AI kind read from the message of a player kill.
10. In 4.12 the old code also tests the killer's score record for null before the kind-2 count; that record is
    never null, so the test is not described.
11. Assists: the double-points question is asked for the killer (possibly null), not for the assister; an award
    of 0 points is still made; OnlineOrShown and the mode type are asked again for every assister.
12. In mode type 0x12 an unknown killer team (−1) counts as team 1 for the assists.
13. `u3x_kr_port` does not test `info` or `s` for null (its caller does for `info`).
14. The enemy kill step doubles the points twice when both the double-points event and the betrayal mark apply,
    but passes only "1" as the popup's double mark.

## 7. Open questions and notes for the integrator

1. **Weapon group signature.** The old `int u3x_weapon_group(uint32_t weapon)` has no `base`, so it cannot read
   the image; the new `u3x_kr_weapon_group(base, weapon)` (4.15) replaces it and the literal list goes away. Its
   other users: two places in G45's medal checks and the dead C one-liners `kr_group6` / `kr_group4` of
   `kr_port.inc` (no caller). They have to call the new name with `base`; then `u3x_weapon_group` is deleted.
   The integrator should tell G45's describer / implementer.
2. **Stat event names.** The stat ids of 3.6 are single ids from the PS3 code; the PS4 executable holds none of
   them and their names are not known (only "net-kill-stealth" for 0xc58ae515 is confirmed by the string id
   rule). If the names are found in the game's script data, the new code could hold names and compute the ids
   with `u3h_crc_sid` as G44 3.8 does. Until then they stay numbers. The eight "flag → one stat" rules of 4.11
   step 8 were written as one small list in the old code; here they are eight separate rules of the flag word
   (3.6), not a table of the game.
3. **Meaning of four game functions** (0x729b10, 0x77c880, 0x773b50, 0x661ca0): described by what the old code
   relies on (3.3); their real names are not known. The description needs only their arguments and results.
4. **Private copies.** The old code carried its own copies of "same side", "string id continued" and "profile
   counter add"; this description uses G44's `u3x_kr_friendly`, `u3x_kr_sid_cat` and `u3x_kr_prof_add` instead
   (same effects, same game calls in the same order). They must be exported by G44's part A before this group
   links.
5. **Functions used as they are, in no work group** (final sweep): `u3x_has_tag`, `u3x_score_member`,
   `u3x_weapon_stat_number`, `u3x_handle_obj`, and the one-liners of G44 4.13's table.
6. The kill context structure and the names `u3x_kr_counted` / `u3x_kr_aftermath` are new and only exist for the
   split in two parts; nobody else calls them.

## Clarifications (tester, 2026-11-14)

Facts settled by the differential test (old v7.212 against the new part A / part B on one fake world; real image
tables of the PS4 executable).

1. **Weapon groups (7.1, 3.9).** The ten ids of the old literal list are exactly the image entries named in 3.9:
   the u32 table at image 0x13a6170, entries 1, 6, 7, 8, 9, 13 (group 6) and the u32 table at image 0x13a66d0,
   entries 20 .. 23 (group 4). `u3x_kr_weapon_group(base, w)` gives the old answer for every id of both tables,
   their neighbours, 0 and two million random ids. An id in both sets gives 6.
2. **Weapon slot table.** Entries 1 .. 0x1a of image 0x13a66d0 hold no id twice, so "first matching slot" and
   "last matching slot" (4.10) give the same slot in the shipped game. The special weapon 0x3d2f108b is entry 21,
   i.e. in group 4, so the "weapon step skipped" rule of 4.11 (group neither 6 nor 4) can never hide its stat.
3. **String id.** `u3h_crc_sid("net-kill-stealth")` = 0xc58ae515, and continuing the id of "net-kill" with
   "-stealth" over the image table 0x13a6ff0 gives the same value.
4. **History, record 0's kind byte (4.3 step 2).** When game call 0x11f2bf0 is made, the old code has already set
   record 0's kind byte (byte 0) to 0; the kind byte is given its final value after the call. Only the moment of
   that write differs; the final record is the same.
5. **Values read once or at every use.** The old code reads these from memory again at each use: the mode type
   T0 of the kill points (4.7, read again from the mode entry when that step starts), the betrayal mark (R0's byte
   0x21) at every use, R0's killer team in the streaks (4.14 steps 2 and 4, also after `u3x_kr_37a32c`), and in
   the betrayal credit with flag 0x4000 and no attacker (4.6 step 3) the killer's team after the
   `u3x_kr_34fc04` call. The results differ only when a function called in between changes that field.
6. **Betrayal timer (4.6 step 1).** The old code effectively compares the two s32 values (until > Now) rather
   than the sign of the wrapping difference. Both rules agree whenever the two clock values are less than 2^31
   apart; they disagree only when they are 2^31 or more apart (e.g. until 0x7fffffff, Now 0x80000000: old looks
   the attacker up, wrapping rule does not; until 0x80000000, Now 0x7fffffff: the other way round).
7. **Entity table end marker (4.4).** The entry at index `size` of the table is read only after a node was found
   in the bucket; with the id absent nothing past the table's `size` entries is touched.
