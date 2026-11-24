# S07. Texts, small helpers and leftovers (final sweep)

Behaviour description for a reimplementation (reimpl2 pipeline, describer's document). It says what the functions
do, not how the old ones were written. Offsets are hexadecimal when prefixed with 0x. "Image X" means the address
`base + X` (`base` = game image base). All values little-endian; u8/u16/u32/s32/u64 are integers of that width;
pointers are 8 bytes. Game functions use the System V AMD64 convention (integer arguments rdi, rsi, ...).

**Float.** Only `u3x_xp_scale` (4.6) computes with floats. The old code did it with single-precision SSE
instructions (one rounding per step), not on the x87 unit, so the new code must match it bit for bit with the
CONVENTIONS.h helpers `f_from_int`, `f_mul`, `f_to_int`. Nothing else here touches a float.

## 1. Purpose and callers

Small functions that the earlier groups call "as existing" and that still hold a rule taken from the game:

| Function | Called from |
|---|---|
| `u3x_is_oasis` | C `oasis_tick` (every frame: the Oasis water-splash flag) |
| `u3x_count_quit` | C `mp_quit119` (G32 4.x: leaving a match early) |
| `u3x_last_game_third` | C `last_game_source` / G38 (the "last game" title) |
| `u3x_time_ago` | C `fi_ago` behind `u3h_fi_ago` (G20: "... ago" of an invite row) |
| `u3x_time_limit_text` | C `k_time_source` / G38 5.5 (custom-game Time Limit row) |
| `u3x_xp_scale`, `u3x_level_threshold`, `u3x_level_for_xp`, `u3x_rank_word` | C XP / rank code (`lg_scale`, `lg_thresh`, level and rank answers) and G22 |
| `u3x_friend_in_game` | C `fr_in_title` behind `u3h_fr_in_title` (G18 / G19 friend rows) |
| `u3x_icon_index` | C `icon_index` behind `u3h_icon_index` (G22..G31, G61) |
| `u3x_quick_action_add` | G18, G20, G34 quick-action menus |
| `u3x_player_profile` (new name for the private `player_profile`) | many C++ functions of other groups; C `u3h_player_profile` |
| `u3x_emb_rec_slot`, `u3x_emb_size` | G57 `u3x_emb_pass`; C `emb_rec_slot119`, `emb_size119` |
| `u3x_v_bind`, `u3x_v_copy`, `u3x_v_sub2` | G50 decoder / G52 visitors; C `c10_119`, `vsub2_119` |
| `u3x_emblem_ready` (new, from C `emblem_ready`) | C `emblem_probe`, C `emblem_source` |
| `u3x_limit_source` (new, from C `time_limit_source`) | C datasource chain (tag 0x4c414e29) |
| `u3x_players_met_source` (new, from C `fr_pm_source`) | C `fr_sources` (tag 0x4c414e15) |

Everything runs on the game's main thread; no locking.

## 2. Interface

### 2.1 Functions to deliver (all `extern "C"`, names and signatures of existing exports unchanged)

| Name | Signature |
|---|---|
| `u3x_is_oasis` | `int u3x_is_oasis(uint32_t map_id)` |
| `u3x_count_quit` | `int u3x_count_quit(uint8_t *record)` |
| `u3x_last_game_third` | `int u3x_last_game_third(int32_t game_type, uint32_t subtitle, uint32_t difficulty, const uint32_t *difficulty_texts, uint32_t *text_id)` |
| `u3x_time_ago` | `uint32_t u3x_time_ago(uint32_t seconds, uint32_t *text_id)` |
| `u3x_time_limit_text` | `void u3x_time_limit_text(char *out, char *end, const uint8_t *rule, int32_t count, const char *label)` |
| `u3x_xp_scale` | `int32_t u3x_xp_scale(int32_t xp, int32_t legacy)` |
| `u3x_level_threshold` | `int32_t u3x_level_threshold(const int32_t *table, int32_t count, int32_t level, int32_t legacy)` |
| `u3x_level_for_xp` | `int32_t u3x_level_for_xp(const int32_t *table, int32_t count, int32_t xp, int32_t legacy)` |
| `u3x_friend_in_game` | `int u3x_friend_in_game(const uint8_t *entry, const char *title)` |
| `u3x_icon_index` | `int32_t u3x_icon_index(const uint8_t *table, uint32_t id)` |
| `u3x_rank_word` | `uint32_t u3x_rank_word(int32_t level, uint32_t legacy, uint32_t badge_bits)` |
| `u3x_quick_action_add` | `int u3x_quick_action_add(QuickActions *q, uint32_t text, uint32_t action)` |
| `u3x_player_profile` | `uint8_t *u3x_player_profile(uintptr_t base)` (new export) |
| `u3x_emb_rec_slot` | `uint8_t *u3x_emb_rec_slot(uintptr_t b, uint32_t sl)` |
| `u3x_emb_size` | `uint32_t u3x_emb_size(uintptr_t b)` |
| `u3x_v_bind` | `void u3x_v_bind(void *visitor, uint32_t index)` |
| `u3x_v_copy` | `void u3x_v_copy(void *visitor, uint32_t a, uint32_t b)` |
| `u3x_v_sub2` | `void u3x_v_sub2(void *visitor)` |
| `u3x_emblem_ready` | `int u3x_emblem_ready(uintptr_t base, void *s)` (new) |
| `u3x_limit_source` | `int u3x_limit_source(uintptr_t base, int32_t *ev, void *s)` (new) |
| `u3x_players_met_source` | `int u3x_players_met_source(uintptr_t base, int32_t *ev, void *s)` (new) |

`QuickActions` keeps its layout (the C side passes it as `void *`): u32 argument at +0, u32 page at +4, s32 count
at +8, then 16 u32 at +0xc = up to 8 pairs (text id, action id). The struct declaration stays where it is (G18,
G20, G34 use it).

Wrappers and private names:
- C `emblem_ready(base, s)` becomes `return u3x_emblem_ready(base, s);`; C `time_limit_source(base, event, s)`
  becomes `return u3x_limit_source(base, event, s);`; C `fr_pm_source(base, ev, s)` becomes
  `return u3x_players_met_source(base, ev, s);`. All three C functions keep their names and callers.
- The C++ file-private `player_profile(base)` becomes a one-line forward to `u3x_player_profile(base)` (its many
  callers in other groups stay). The C function `player_profile` (behind `u3h_player_profile`) holds the same
  rule; the integrator may also make it a one-line forward (it is classed glue, so this is optional).
- `u3x_v_bind`, `u3x_v_copy`, `u3x_v_sub2`, `u3x_emb_rec_slot`, `u3x_emb_size` were marked "stay as they are" in
  G52 / G57; this document now delivers them with unchanged contracts (G52 2.2, G57 2.x).

Old code about 160 lines; one implementer is enough. If two are wanted: part A = 4.1 to 4.13 (`u3x_is_oasis`
to `u3x_player_profile`), part B = 4.14 to 4.18 (emblem records, visitor commands, the three former C sources).
The parts do not call each other.

### 2.2 Functions of other groups, of the C part and of the shared tools that are called

| Name | Signature | Effect |
|---|---|---|
| `u3h_crc_sid` | `uint32_t u3h_crc_sid(const char *name)` | string id of a name by G23 3.9 (CRC, polynomial 0x04C11DB7, MSB first, start 0, no final inversion). Pure |
| `u3x_strcasecmp` | `int u3x_strcasecmp(const char *a, const char *b)` | G18 4.1: compare ignoring ASCII case; -1, 0, 1 |
| `u3x_v_command` | `uint32_t *u3x_v_command(void *visitor, uint16_t type, uint16_t words)` | G52 4.1: reserves one command (writes word 0 = type, length; advances the position) and returns its word 0, or null |
| `u3x_v_max` | `void u3x_v_max(void *visitor, uint32_t index)` | G52 4.1: raises the buffer's highest instance index |
| `u3h_gate_closed` | `int u3h_gate_closed(uintptr_t base, int32_t *ev)` | G38 2.2: 0 while multiplayer is active; else answers empty text and returns 1 |
| `u3h_set_text` | `void u3h_set_text(int32_t *ev, const char *t)` | stores `t` as the request's text answer (pointer at ev byte 0x40) |
| `u3x_sym_table` | `uint8_t *u3x_sym_table(void *s, uint32_t id)` | G19 2.2: data of a script symbol through the state's lookup (pointer at s + 656), or null |
| `u3x_has_digit` | `int u3x_has_digit(const char *text)` | G38 2.2: 1 when the text holds '0'..'9' |
| `u3x_page_tab_text` | `uint32_t u3x_page_tab_text(uintptr_t base, void *s)` | G19 4.6: text id of the selected tab of the top page, or 0 |
| `u3x_players_met_fetch` | `void u3x_players_met_fetch(uintptr_t base, void *s)` | G19 5.4: the Players Met refresh |

## 3. Game facts

- **Helper state `s`**: native function table at s + 0x300, entry k (8-byte pointer, may be null) at s + 0x300 +
  8·k. Used: k = 9 "find module" (image 0x11fd4c0; `void *f(uint32_t symbol_id, int one)`; result the module or
  the sentinel image 0x1b3af40 when none), k = 11 "emblem table init" (image 0x66b510; `void f(uint8_t *manager)`),
  k = 22 "limit row datasource" (image 0x753b70; `void f(int32_t *ev)`). Text buffer of 4.17: s + 0x2c00 (121 bytes).
- **Datasource request `ev`** (s32 words): word 0 operation; word 1 value; word 0x15 text index; pointer at byte
  0x40 the text answer; pointer at byte 0x98 the source descriptor, whose u32 at +0x1c is its tag.
- **Image data**: u8 0x17d5c61 multiplayer active (NetInit done); u32 0x1b67e6c + k·0x18 (k = 0, 1, 2) the three
  emblem part table symbol ids; 0x17c3440 the emblem manager (u8 at +0 its dirty byte; u64 at +0x50, +0x58 the
  record pointers of slots 0/1, +0x68, +0x70 the second record set); pointer 0x17dd610 the profile array (0x7b00
  bytes per user); u8 0x17bde75 split-screen byte; u32 0x17bf714 the menu user; s32 0x17d167c the selected net game
  index (net game manager 0x17cb060 + 0x661c); game function 0x75e2e0 (`void f(int32_t *ev)`): the game's own
  players-met list source.
- **Script tables** (through `u3x_sym_table`): 0x3b9a067d net games (s32 count at +0, pointer to entries at +8,
  0xd0 bytes each, s32 game type at +0xc); 0xf985c7b6 rule sets (s32 count at +0, entries pointer at +8, 0x58 bytes
  each; s32 base value at +0x1c, s32 step at +0x24 of an entry).
- **UI texture table** (`u3x_icon_index`): u16 count at +2, pointer to entries at +8, 40-byte entries, icon id u32
  at entry +8.
- **Friend entry** (`u3x_friend_in_game`, G18 3.4): presence title text (NUL-terminated) at +0x28, s32 presence
  state at +0x1b0.
- **Multiplayer record** (`u3x_count_quit`): u32 "in a match" at +0x558, u32 quit counters at +0x5b0 and +0x1f24.
- **Visitor record** (G50 3.2): u8 abort +0x14c, u8 active +0x14d, s32 kind +0x150.
- **Text ids** (single ids, kept by value; see 7.1): seconds 0x72cd40d5, minutes 0x6498bce2, hours 0x624258c1,
  days 0x60f0bee3, weeks 0xd250139c; "no subtitle" ids 0x22a276ec and 0x6f96651f; Players Met tab 0xacb77201.
- **Source tags**: 0x4c414e29 (limit row, 4.17), 0x4c414e15 (players met, 4.18).
- **Oasis map ids**: no literal list. They are the string ids (`u3h_crc_sid`) of the three map names "oasis",
  "oasis-survival" and "oasis-shade" (checked: the three ids the old code held are exactly these CRCs; the same
  naming holds for the other maps with variants, e.g. "fort", "fort-survival", "fort-shade" and
  "gompa-survival", "gompa-shade" give the map ids of those entries in the net map table, symbol 0x0dec97a4, which
  the game loads from dc1 net.bin; the three Oasis entries are consecutive there and all name "net-levels/oasis"
  as their picture).

## 4. Behaviour

4.1 **`u3x_is_oasis(map_id)`**: 1 when `map_id` equals the string id of "oasis", "oasis-survival" or
"oasis-shade"; else 0 (also for 0). The names are U3X_RO arrays; whether the ids are computed per call or once is
not observable (the function is called every frame; computing once is fine).

4.2 **`u3x_count_quit(record)`** (`record` never null; not checked): when the "in a match" u32 is 0: result 0,
nothing written. Otherwise: it becomes 0, then each quit counter (+0x5b0, then +0x1f24) increases by 1 (wrapping
mod 2^32); result 1.

4.3 **`u3x_last_game_third`**: game type 4 or 11 (co-op): when `difficulty` (unsigned) ≥ 5 result 2 and `text_id`
untouched; else `*text_id` = `difficulty_texts[difficulty]`, result 1. Other game types: `subtitle` 0, 0x22a276ec
or 0x6f96651f gives result 0 (untouched); else `*text_id` = `subtitle`, result 1.

4.4 **`u3x_time_ago(seconds, text_id)`**: `*text_id` is always written. seconds < 120: text = seconds id, result
= seconds, but at least 2. Otherwise M = seconds / 60 (integer); M ≤ 119: minutes id, result M. Else H = M / 60;
H ≤ 47: hours id, result H. Else D = H / 24; D ≤ 13: days id, result D. Else weeks id, result D / 7.

4.5 **`u3x_time_limit_text(out, end, rule, count, label)`**: V = (s32 at rule + 0x18) × count + (s32 at rule +
0x10), in 64-bit signed arithmetic (no overflow possible). Writes the decimal text of V: a '-' first when V < 0,
then the digits of |V| (no leading zeros; "0" for 0). Then, only when `label`'s first character is not NUL, one
space and the label's characters. Every character is written only while the write position is before `end`
(characters that do not fit are dropped, the rest is still attempted in order); finally a NUL at the write position
(so possibly at `end` itself). `label` is never null.

4.6 **`u3x_xp_scale(xp, legacy)`**: legacy 4: `f_to_int(f_mul(f_from_int(xp), 0x40200000))` (× 2.5); legacy 5:
the same with 0x40a00000 (× 5.0); any other legacy: `xp` unchanged. Conversion to float rounds to nearest,
the product is rounded once to single, the conversion back truncates toward zero (out of range gives 0x80000000,
as `cvttss2si`).

4.7 **`u3x_level_threshold(table, count, level, legacy)`**: i = `level`, raised to 0 when negative, then lowered
to `count` − 1 when above it (signed; in that order, so `count` ≤ 0 gives i = `count` − 1, an out-of-table read the
callers never cause). Result `u3x_xp_scale(table[i], legacy)`.

4.8 **`u3x_level_for_xp(table, count, xp, legacy)`**: for L = 0, 1, ... while L + 1 < `count`: when
scale(table[L]) ≤ `xp` and `xp` < scale(table[L+1]) (signed, scale = `u3x_xp_scale(…, legacy)`) result L. None:
result `count` − 1.

4.9 **`u3x_rank_word(level, legacy, badge_bits)`**: `badge_bits` OR (`level` AND 0xffff) OR ((`legacy` AND 0x3fff)
shifted left by 16).

4.10 **`u3x_friend_in_game(entry, title)`**: presence state 1: result 1. State 2: result 1 when
`u3x_strcasecmp(entry title, title)` is 0, else 0. Any other state: 0 (no compare made).

4.11 **`u3x_icon_index(table, id)`**: null table: −1. Otherwise for k = 0 .. count − 1 (while the entries pointer
is not null): the first k whose entry icon id equals `id` is the result. None (or null entries): −1.

4.12 **`u3x_quick_action_add(q, text, action)`**: when count > 7 (signed): result 0, nothing written. Else pair
number `count` = (text, action) (text at +0xc + 8·count, action after it), count + 1, result 1.

4.13 **`u3x_player_profile(base)`**: P = pointer at image 0x17dd610; null P: null. User = 1 when the split-screen
byte is non-zero, else the menu user u32. User ≥ 4 (unsigned): null. Else P + user · 0x7b00.

4.14 **`u3x_emb_rec_slot(b, sl)`**: M = image 0x17c3440. The u64 at M + 0x50 + 8·sl; when it is 0, the u64 at
M + 0x68 + 8·sl. Returned as a pointer (may be null). `sl` is 0 or 1 (not checked).
**`u3x_emb_size(b)`**: 0x80 when M + 0x50 or M + 0x58 is non-zero; else 0x100 when M + 0x68 or M + 0x70 is
non-zero; else 0.

4.15 **Visitor commands** (G52 3.2 buffer): `u3x_v_bind(v, i)`: W = `u3x_v_command(v, 0xd, 2)`; null: nothing
more. Else word 1 = i, then `u3x_v_max(v, i)`. `u3x_v_copy(v, a, b)`: W = `u3x_v_command(v, 0xf, 3)`; null:
nothing more. Else word 1 = a, word 2 = b, then `u3x_v_max(v, a)`, then `u3x_v_max(v, b)`.
**`u3x_v_sub2(v)`** (the stream's marker sub-command): kind ≠ 2: nothing. Kind 2: when active is non-zero, abort
= 1 (active unchanged); else active = 1.

4.16 **`u3x_emblem_ready(base, s)`** (PS3 ran the emblem part table init from the menu; PS4 only at match setup):
1. F = native 9; null: result 0, nothing else.
2. I = native 11. When I is not null AND the multiplayer-active byte is non-zero AND the first part table id (u32
   at image 0x1b67e6c) is 0: the manager's dirty byte (image 0x17c3440) = 1, then I(base + 0x17c3440).
3. For k = 0, 1, 2 in order: id = the u32 at image 0x1b67e6c + k·0x18 (read after step 2). When id is 0: result 0
   (no further calls). Else F(id, 1); when it returns the sentinel base + 0x1b3af40: result 0 (stop). A null
   return counts as found.
4. Result 1.

4.17 **`u3x_limit_source(base, ev, s)`** (datasource 0x29: PS4 dropped the number from the row's text):
1. Descriptor pointer null, or its tag ≠ 0x4c414e29: result 0.
2. `u3h_gate_closed(base, ev)` non-zero: result 1.
3. N = native 22; null: result 1. Else N(ev). Everything below ends with result 1.
4. Nothing more unless word 0 = 2 and word 0x15 = 0, and the lookup pointer at s + 656 is not null.
5. L = the text answer (pointer at ev byte 0x40); nothing more when L is null or `u3x_has_digit(L)` is 1.
6. G = `u3x_sym_table(s, 0x3b9a067d)`, then R = `u3x_sym_table(s, 0xf985c7b6)` (both always asked); nothing more
   when either is null.
7. sel = s32 at image 0x17d167c; nothing more when sel < 0 or sel ≥ G's count. type = s32 game type of net game
   entry sel; nothing more when type < 0 or type ≥ R's count (signed).
8. V = (step × word 1) + base value of rule entry `type`, 64-bit signed. Into the buffer at s + 0x2c00 with end
   s + 0x2c00 + 120, same bounded writing as 4.5: '-' when negative, digits, then ALWAYS one space (also for an
   empty L), then L; then a NUL at the write position. `u3h_set_text(ev, s + 0x2c00)`.

4.18 **`u3x_players_met_source(base, ev, s)`** (PS3 handled operation 9 itself; PS4's source lacks it):
1. Descriptor null or tag ≠ 0x4c414e15: result 0.
2. `u3h_gate_closed(base, ev)` non-zero: result 1.
3. Word 0 = 9 and `u3x_page_tab_text(base, s)` = 0xacb77201: `u3x_players_met_fetch(base, s)`, result 1 (the tab
   is only asked for operation 9).
4. Otherwise game function 0x75e2e0(ev); result 1.

## 5. Test scenarios

All parts work with call-recording stubs and fake memory (a fake image block, a fake state with a native table,
fake script tables, a fake visitor and command buffer); nothing needs the live game. Cover:
1. `u3x_is_oasis`: the three CRCs give 1; 0, 0xdeac0df2, the "fort" id give 0. Check value: CRC("oasis") =
   0xdeac0df3 (same as the old constant).
2. `u3x_count_quit`: in-match 0 / non-zero; counters at 0xffffffff wrap.
3. `u3x_last_game_third`: types 4, 11 with difficulty 0, 4, 5, 0xffffffff; type 1 with subtitle 0, both "no
   subtitle" ids, another id; `text_id` untouched in the 0 / 2 cases.
4. `u3x_time_ago`: 0, 1, 2, 119, 120, 7199, 7200, 172799, 172800, 1209599, 1209600, 0xffffffff.
5. `u3x_time_limit_text` and 4.17 step 8: positive, zero, negative values, large products (2^31−1 × 2^31−1),
   empty label (no space in 4.5, a space in 4.17), `end` cutting inside the number, at the space, inside the label.
6. `u3x_xp_scale`: legacy 3, 4, 5; xp 0, 1, 3, −3, 2^24+1, 0x7fffffff (truncation, out of range), random values
   bit-compared. Threshold: level −1, 0, count − 1, count. Level for xp: below the first, on each boundary, above
   the last, unsorted table.
7. Rank word with high bits set in level and legacy.
8. Friend states 0, 1, 2 (title equal ignoring case, different), 3; no compare call outside state 2.
9. Icon table null, entries null, id at index 0, last, missing.
10. Quick actions: count 0, 7 (added), 8 (refused).
11. Player profile: null array; byte set (user 1 whatever the menu user); menu user 0, 3, 4.
12. Emblem records/size: each combination of the four pointers.
13. `u3x_v_bind` / `u3x_v_copy` with room, without room (error 7 through `u3x_v_command`), error already set;
    `u3x_v_max` order. `u3x_v_sub2` with kind 0, 1, 2 × active 0, 1.
14. `u3x_emblem_ready`: native 9 null; native 11 null; byte 0; first id set (no init); init run (dirty byte, then
    call); each table id 0; sentinel at k = 0, 1, 2; null return.
15. `u3x_limit_source`, `u3x_players_met_source`: each early exit in order, call sequences recorded.

## 6. Kept as they are (bugs or oddities, behaviour to keep)

1. 4.7: `count` ≤ 0 reads before the table (never happens with game data).
2. 4.12: a negative count would pass the cap check; callers never make one.
3. 4.16 step 3: a null "find module" result counts as success (only the sentinel fails).
4. 4.17 uses the net game index as a full s32 at 0x17d167c, while G38's Time Limit source reads it as s16 (same
   address); both kept. 4.17 always writes the space; 4.5 only before a non-empty label.
5. 4.2: no null check on the record.

## 7. Open questions

1. The five "ago" text ids and the two "no subtitle" ids are PS3 1.19 text ids: none of these seven 32-bit
   patterns occurs in the PS4 image file, and no names were found that give them by the CRC rule (the Players
   Met tab id 0xacb77201 was not checked; G19 already uses it). They are single ids with a meaning, kept by value like the text ids of the other specs (G38 already
   lists the two "no subtitle" ids). If the owner treats the five "ago" ids as a table: 5 u32 text ids (seconds,
   minutes, hours, days, weeks), source only the PS3 game.
2. Datasource 0x29 (4.17): the old comments call it both "Time Limit" and "Score Limit"; the name of the new
   function is neutral (`u3x_limit_source`). The owner may rename it.
3. Whether the C `player_profile` also forwards to `u3x_player_profile` (2.1) is the integrator's choice.

## Clarifications (tester, 2026-11-14)

Settled by the differential test `reimpl2\S07\test_s07.cpp` (old v7.212 against the new parts, host builds at -O1 and
-O2). Facts only.

1. Oasis ids (4.1): CRC("oasis") = 0xdeac0df3, CRC("oasis-survival") = 0x82fb8e9e, CRC("oasis-shade") = 0x421d5e2c
   with the C part's own string-id function: exactly the three ids the old code held. "fort", "fort-survival",
   "fort-shade", "gompa-survival", "gompa-shade", "oasis-", "OASIS" and "" do not give Oasis. The new code computes
   up to three CRCs per call; the number of computations is not behaviour.
2. Time-limit text (4.5): the value is the 64-bit signed product step × count plus base; extremes checked:
   (2^31−1)² + 0 gives "4611686014132420609", (−2^31)² gives "4611686018427387904", −2^31 × (2^31−1) − 2^31 gives
   "-4611686018427387904", −2^31 × (2^31−1) − 1 gives "-4611686016279904257". Bounded writing: the sign, then the
   digits from the most significant one, then the space and the label, each character only while the position is
   before `end`; a cut keeps the leading characters ("12345" with room for 3 gives "123"; room for exactly the number
   drops the space and the label; room for the number and the space gives "12 "). The NUL goes to the final write
   position, which is `end` itself when everything up to `end` was written, and `out` itself when `end` ≤ `out`.
   4.17 step 8 uses the same writer with the end 120 characters after s + 0x2c00 (a 130-character label leaves 120
   characters and the NUL at s + 0x2c00 + 120).
3. `u3x_has_digit` (4.17 step 5): any non-zero result stops the source (tested with results 1, 2 and −1); only 0
   goes on. The same holds for `u3h_gate_closed` (results 1 and 2 both end with result 1).
4. Script tables (4.17 steps 6 and 7): both lookups are always made, net games (0x3b9a067d) first, then rule sets
   (0xf985c7b6), also when the first gives nothing. A handle of null and a handle holding null both mean "missing".
   The entries pointers at +8 of the two tables are NOT checked: with a null entries pointer the source reads from
   address sel × 0xd0 + 0xc (net games) or type × 0x58 + 0x24 (rule sets) and faults, in the old and the new code
   alike (only the counts are checked). The selection at 0x17d167c is read as a full s32 (0x10001 is out of range).
5. Visitor (4.15): abort is the u8 at +0x14c, active the u8 at +0x14d, kind the s32 at +0x150; any non-zero active
   byte (e.g. 0x80) counts as active and is left as it is when abort is set. `u3x_v_copy` writes both words before
   the two `u3x_v_max` calls (a first, then b).
6. Signed 32-bit extremes (4.7, 4.8), as the host builds of both versions behave: `count` = −2^31 in 4.7: `count` − 1
   wraps to 2^31 − 1, so a level ≥ 0 is not lowered (table[level] is read) and a negative level reads table[0];
   `count` ≤ 0 otherwise reads table[count − 1] (0 → table[−1], −1 → table[−2]). In 4.8 `count` = −2^31 gives no
   loop and the result 2^31 − 1 (wrapped `count` − 1). `count` = 2^31 − 1 with a matching band returns that band.
   These inputs never come from the game; signed overflow is undefined in C++, so the helper's own build is not
   proven by this.
7. `u3x_quick_action_add` with a negative count (spec 6.2 lets it pass the cap): the pair goes to +0xc + 8 × count
   (signed), then the count field is raised by 1 as it is in memory AFTER the two stores. Count −1: text at +4,
   action at +8 (the count itself), so the count ends as action + 1 (wrapping). Count −2: text at −4, action at +0,
   count −1. Count −2^31: the pair address is +0xc − 2^34 (64-bit), far outside the block (both versions fault
   there in the test). The callers never pass a negative count.
8. `u3x_emb_rec_slot` with a slot past 1 (4.14 says 0 or 1; not checked): the WHOLE byte offset, 0x50 + 8 × slot and
   0x68 + 8 × slot, is formed in 32 bits and wraps modulo 2^32 before it is added to the manager address. So slot
   0x20000000 / 0x80000000 reads slot 0's places (+0x50, +0x68), 0x20000001 slot 1's, 0x7fffffff and 0xffffffff
   read +0x48 then +0x60, 0xfffffffe reads +0x40 then +0x58 (the wrap of the sum matters here, not only that of
   8 × slot). Slots 2 and 3 read +0x60 / +0x78 and +0x68 / +0x80.
9. `u3x_xp_scale` (4.6): bit for bit equal over every int32 xp for legacy 4 and 5 (2^33 values). Both versions use
   the same SSE instructions in inline assembly (not the x87 unit), so the helper build gives the same values.
   3 × 2.5 = 7, −3 × 2.5 = −7, (2^24 + 1) × 5 = 5 × 2^24 (the conversion to float rounds first), 2^31 − 1 × 2.5 gives
   0x80000000.
10. `u3x_time_ago` (4.4): equal over every u32.
11. Read points: in the "one change" cases (a stand-in call with effects — native 22, emblem init, find module,
    gate, `u3x_v_command`, `u3x_v_max` — also changes one field of the world) the old and new code read every value
    at the same side of every call: native 22's effects on words 0, 1, 0x15, the text answer, the lookup pointer,
    the selection, the table counts and entries are all read after native 22; the three emblem table ids are read
    after the init, each id after the previous find; the native 9 and 11 pointers before the init.
12. Open question 3 (2.1): the C `player_profile` gives the same result as the C++ one in every tested case, so a
    one-line forward to `u3x_player_profile` keeps the behaviour.
13. Section 6: 6.1 confirmed (table[count − 1] for count ≤ 0); 6.2 confirmed (a negative count passes the cap; see
    item 7 for the count afterwards); 6.3 confirmed (a null find result counts as found at k = 0, 1, 2); 6.4
    confirmed (s32 selection; 4.17 writes the space also for an empty label, 4.5 does not); 6.5: neither version
    checks the record pointer (not run with null).
