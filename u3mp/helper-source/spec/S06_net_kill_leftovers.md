# S06. Leftovers: net ids and connection entries, kill-record lookups, late-join rules, scoreboard levels, HUD values

Behaviour description for a reimplementation (reimpl2 pipeline, describer's document, final-sweep group S06). It
says what the functions do, not how the old ones were written. Offsets are hexadecimal when prefixed with 0x. "Image
offset X" means the address `base + X` (`base` = game image base). Multi-byte values are little-endian; u8 / u16 /
u32 / s32 / u64 are integers of that width, "pointer" is 8 bytes. "ptr_ok(p)" is the shared tool of CONVENTIONS.h.
"eb[i]" / "ea[i]" are the helper's diagnostic words (`ext_eb()`, `ext_ea()` of CONVENTIONS.h). Names follow
`G12_packets_monitor.md` (net player record, connection entries, reliable header), `G11_game_sockets_time.md`
(NOW, game sockets), `G40_respawn.md` (state object `s`, natives) and `G44_kill_rewards.md` (score keeper `S`).

**Float.** Only two places touch floats. 4.17 (link timeout) computes NOW − t and compares it with 20.0: single
precision per PIPELINE.md's Float rule (the old build may have kept NOW unrounded in an x87 register; a last-bit
difference there is the accepted deviation). 4.23 (weapon panel origin) only compares and stores float patterns:
done on the 32-bit patterns, no arithmetic.

**Tables.** Four literal tables were in the old code; none is copied here.
- HUD material renames (16 pairs of string ids) and score-plate texture ids (4 pairs): every value is the game's
  string id of a material / texture NAME (3.9): the PS4 id is the id of the name as PS4 writes it (with a ':'), the
  PS3 id is the id of the same name with that ':' replaced by '/'. All 40 values were checked against this rule. The
  new code holds the 20 names (asset names, given in 3.9) and computes both ids with `u3h_crc_sid`. See open
  question 1.
- Weapon panel origin (two pairs of floats, PS4's and PS3 1.19's): the PS4 pair is the shipped PS4 value of the very
  field the code tests; the PS3 pair is PS3-only data with no rule. Open question 2; the values are not given here.
- The eight test words of the rank-up popup aid (4.25) are the helper's own test choice, not game data.

## 1. Purpose and callers

Two independent parts; no function of one part calls one of the other.

| Part | Content | Exported names |
|---|---|---|
| **A** | Pure rules and lookups: late-join rules, weapon-bar answer, LAN address book, net player id, tag test, score member, weapon statistics numbers, connection entries, double points, hunter index, reliable header write, data packet read, link timeout | `u3x_round_mostly_over`, `u3x_match_nearly_decided`, `u3x_comp_weapon_answer`, `u3x_lan_address`, `u3x_net_player_id`, `u3x_rp_trk_id`, `u3x_has_tag`, `u3x_score_member`, `u3x_weapon_stat_number`, `u3x_weapon_for_stat`, `u3x_entry_by_address`, `u3x_entry_by_id`, `u3x_connection_state`, `u3x_double_points`, `u3x_hunter_index`, `u3x_gt_header`, `u3x_gt_base_read`, `u3x_link_timeout` |
| **B** | Per-frame game-state jobs: alive test, local players' respawn tick, scoreboard levels, HUD material names, score-plate textures, weapon panel origin, co-op buddy marker icon, rank-up popup gate and test aid | `u3x_np_alive`, `u3x_net_local_players_tick`, `u3x_x53_levels`, `u3x_hud_alias_names`, `u3x_k47_hud`, `u3x_e01_hud`, `u3x_ci_icon`, `u3x_rank_popup_tick` |

Callers (all stay): part A functions are called as "existing" by G11, G12, G14, G21, G23, G30, G35, G41, G43,
G44, G45, G46, G53, G58 and by C wrappers (`hunter_index`, `rp_trk_id`, `gt_base_header` users, `lnb_cm_lanaddr`,
`gt_entry_by_address`, `u3h_link_timeout`). Those specs already state the signatures used below; they must not
change. Part B: `u3h_np_alive` (C) and C++ callers; the C hook kinds 17 / 18 (`net_local_players_tick`); C
`x45_level_tick` every 2 s in a match (`x53_levels`); `u3h_hud_alias_names` (lobby / page events, once per frame);
the C frame job list (`k47_hud`, then `e01_hud`, per frame while multiplayer runs); hook kind 219 (`ci_icon`); C
`n25_tick` (per frame). All on the game's main thread except `u3x_gt_base_read` / `u3x_link_timeout` (the game's
net update, also main thread).

## 2. Interface

### 2.1 Functions to deliver (all `extern "C"`, U3X_FN)

| Function | Signature | C wrapper / note |
|---|---|---|
| late join, time | `int u3x_round_mostly_over(int32_t limit, int32_t elapsed)` | as G35 2.2 |
| late join, score | `int u3x_match_nearly_decided(int32_t lead_wins, int32_t rounds, int32_t round)` | as G35 2.2 |
| weapon bar answer | `int u3x_comp_weapon_answer(uint32_t query, uint32_t weapon, int locked, int32_t *answer)` | as G23 2.2 |
| LAN address book | `void u3x_lan_address(uint32_t *count, uint8_t *records, const char *name, uint32_t ip)` | C `lnb_cm_lanaddr` passes (CM + 0x488c, CM + 0x4890, name, ip), CM = image 0x17f31b0 |
| net player id | `uint32_t u3x_net_player_id(const uint8_t *np)` | as G12 2.2 |
| tracker id | `uint32_t u3x_rp_trk_id(const uint8_t *tr)` | same value as `u3x_net_player_id`; may simply call it |
| tag test | `int u3x_has_tag(const uint8_t *bucket_head, const uint8_t *tags, uint32_t id)` | as G46 2.2 |
| score member | `int u3x_score_member(const uint8_t *S, uint32_t id)` | as G44 2.2 |
| weapon → number | `uint32_t u3x_weapon_stat_number(const uint32_t *fixed, const uint32_t *extra, uint32_t weapon)` | as G46 2.2 |
| number → weapon | `uint32_t u3x_weapon_for_stat(const uint32_t *fixed, const uint32_t *extra, uint32_t number)` | as G45 2.2 |
| entry by address | `uint8_t *u3x_entry_by_address(uint8_t *entries, uint32_t ip, uint32_t port)` | as G11 2.2 |
| entry by id | `uint8_t *u3x_entry_by_id(uintptr_t base, int32_t id)` | as G12 2.2 |
| connection state | `int32_t u3x_connection_state(uintptr_t base, int32_t id)` | as G14 2.2 |
| double points | `int u3x_double_points(uintptr_t base, const uint8_t *np)` | as G44 / G46 |
| hunter index | `uint32_t u3x_hunter_index(uintptr_t base, int32_t id)` | as G43 2.2; replaces the file-private `hunter_index` too |
| header write | `void u3x_gt_header(const uint8_t *sub, void *b, uint32_t flag)` | as G12 2.2; `b` is a `GtStream *` |
| data packet read | `int u3x_gt_base_read(uintptr_t base, uint8_t *sub, void *b)` | as G11 2.2 |
| link timeout | `int u3x_link_timeout(uintptr_t base, uint8_t *e, int32_t *kick_at)` | C `gt_link_timeout(base, e)` = `return u3x_link_timeout(base, e, gt_kick_at);` (the 32-word array stays in C, so `u3h_link_timeout` of G12 is unchanged) |
| alive | `int u3x_np_alive(void *s, uint8_t *p)` | replaces the file-private `alive`; `u3h_np_alive` keeps forwarding |
| local players tick | `void u3x_net_local_players_tick(uintptr_t base, uint8_t *mgr, void *s, int variant)` | C `net_local_players_tick(base, mgr, s, variant)` forwards |
| scoreboard levels | `void u3x_x53_levels(uintptr_t base)` | C `x53_levels(base)` forwards |
| HUD material names | `void u3x_hud_alias_names(uintptr_t base, void *s)` | C `hud_alias_names(base, s)` forwards |
| score-plate textures | `void u3x_k47_hud(uintptr_t base, void *s, uint32_t *diag)` | C `k47_hud` = `u3x_k47_hud(base, s, (uint32_t *)k47_d);` (k47_d stays in C for the peek tool) |
| weapon panel origin | `void u3x_e01_hud(uintptr_t base, void *s, uint32_t *diag)` | C `e01_hud` = `u3x_e01_hud(base, s, (uint32_t *)e01_d);` |
| buddy marker icon | `void u3x_ci_icon(uint64_t *r, uint32_t *diag)` | C `ci_icon(base, r)` = `u3x_ci_icon(r, ci_d);` |
| rank popup | `void u3x_rank_popup_tick(uintptr_t base)` | see below |

`n25_tick` keeps its own glue in C and calls the new function last: `x72_skins(base)`; when the u8 at image
0x17d5c61 is 0: ea[63] = 0 and return; `x45_level_tick(base)`; `x61_round_clock(base)`; `u3x_rank_popup_tick(base)`.
(Everything before the last call is the helper's own sequencing of other jobs.)

### 2.2 Functions of other groups, of the C part and shared ones (declare, do not write)

| Name | Signature | What it does |
|---|---|---|
| `u3h_cm_entry` | `uint8_t *u3h_cm_entry(uintptr_t base, int i)` | address of connection entry `i` (image 0x17f31b0 + 8 + i·0x220) |
| `u3h_crc_sid` | `uint32_t u3h_crc_sid(const char *p)` | the game's string id of a zero-terminated name (CRC-32, polynomial 0x04C11DB7, most significant bit first, start 0, no final XOR); pure |
| `u3x_gt_write` | `void u3x_gt_write(GtStream *s, uint32_t value, int count)` | G11 2.2: appends the `count` low bits of `value` |
| `u3x_gt_base_header` | `int u3x_gt_base_header(uintptr_t base, uint8_t *sub, void *b)` | G12: reads and handles the reliable header; non-zero = the payload is to be read |
| `u3x_gt_vcall` | `int64_t u3x_gt_vcall(void *sub, int slot, void *a, void *b)` | calls the function in slot `slot` of `sub`'s function table with (sub, a, b) |
| `u3h_gt_stamp` | `void u3h_gt_stamp(uintptr_t base, uint8_t *sub, int off)` | stores NOW (G11) as float bits at `sub + off` |
| `u3h_bc_state` | `volatile uint32_t *u3h_bc_state(int which)` | address of a C diagnostic word: 0 = "where", 1 = "reads" |
| `u3x_loc_now` | `int32_t u3x_loc_now(uintptr_t base)` | s32 at image 0x17b4ba4 (local clock, 300 ticks per second) |
| `u3x_np_has_effect` | `int u3x_np_has_effect(void *s, uint8_t *p, uint32_t h)` | G40: one native 56 call; non-zero when `p` has effect `h` |
| `u3x_handle_obj_valid` | `int u3x_handle_obj_valid(uint8_t *h)` | G40 3.4: a game handle still names its object |
| `u3x_respawn_tick` | `void u3x_respawn_tick(uintptr_t base, void *s, uint8_t *p, int idx)` | G40: the respawn state machine of one local player |
| `u3x_net_player_in_game` | `int u3x_net_player_in_game(const uint8_t *np)` | u8 +0xe1 non-zero and u8 +0x5e4 zero |
| `GtStream` | `struct GtStream { uint32_t *words; uint32_t pos, max, x, pad; }` | G12 3.8 |

## 3. Game facts

### 3.1 Net player record (0x9c0 bytes; ten at image 0x17cb3f0 + i·0x9c0)

As G12 3.1: +0x10 character handle; +0x1e4 u32 low id bits; +0x1e8 u32 user number; +0x1ec u8 guest flag; +0x204
u32 scoreboard level; +0x21c s32 team; +0x39c s32 / +0x3a0 s32 (health-type pair: alive while +0x39c ≤ +0x3a0);
+0x5b8 player key; +0x5e4 u8 leaving; +0x770 u8 alive byte; +0xe1 u8 joined. A tracker (G60 / film) has the same
first 0x9c0-byte layout, so its id uses the same fields.

### 3.2 Connection entries and the connection manager (image 0x17f31b0 = CM)

32 entries of 0x220 bytes from CM + 8: +0 s32 id; +4 u8 in use; +0x2c u32 IPv4 address; +0x30 u16 port; +0x34 s32
peer link handle; +0x40 the entry's reliable sub-object (its +0xa8 is a float: game time in seconds of the last
receive, 0 = never). LAN address book of the manager: u32 count at CM + 0x488c, records of 0x28 bytes from
CM + 0x4890 (0x24-byte name, then u32 IPv4 address at +0x24), 32 records (port of PS3 f_39ed40).

### 3.3 Reliable sub-object (G12 3.5 / 3.7)

+8 u32 out sequence, +0xc u32 in sequence, +0x14 u32 ack bits; function table at +0, slot 1 = payload reader
(sub, stream, null).

### 3.4 Score keeper and snapshots

Score keeper `S` = image 0x17dd780: u32 member count at +0; member records of 0xa4 bytes from +0x50e4, player id
(u32) at record +0, level byte at record +0xb. A second snapshot of the same layout is at image 0x17e8388.
Double-points event: s32 at image 0x17dd624 (non-zero = running), s32 at image 0x17dd634 = the team it is for.

### 3.5 Hunters, tags, weapons

Hunter AI list: u32 count at image 0x17cb060 + 0x318; entries of 0x18 bytes from image 0x17cb060 + 0x18, s32 id at
entry +0x10. Tag table bucket: a circular singly linked list whose end is the bucket head; node +0 next pointer,
+0x10 u32 tag id, +0x14 u32 bit number; an object's tag bits are u64 words from `tags` + 0x20. Weapon statistics:
`fixed` = the game's 19-entry u32 weapon table (callers pass image 0x13a6170); `extra` (may be null) = u32 count at
+0, pointer to u32 weapon ids at +8.

### 3.6 Game functions called directly

| Image offset | Arguments | Result | Used by |
|---|---|---|---|
| 0x7a3400 | (match session = image 0x17d6420, pointer to a player key) | pointer to that member's lobby data record or null; its u8 at +0xe is the member's level | 4.20 |
| 0xc22cb0 | (popup object, pointer to an 18-word popup entry) | none | 4.25 |
| native 13 (`s` + 0x368) | (u32 symbol id) | pointer to the symbol's value or null | 4.22, 4.23 |

Natives: function pointers at `s` + 0x300 + 8·n (n = 13 symbol lookup, 53 net player spawn, 62 DC type id).

### 3.7 HUD objects

UI object: pointer at image 0x1b24bc8; its +0x20 = hud material table: u16 entry count at +2, pointer to entries at
+8, entries of 0x28 bytes with the material string id (u32) at +8. Hud-settings symbol 0x9353ed16: value +0x78 =
score struct, pointer slots at +0x08 .. +0x118 (35 slots, 8 apart), each a sprite or text descriptor whose first
u32 is a texture id for sprites (text descriptors start with a float); value +0x28 = weapon panel sub-struct, its
first two floats = panel origin x, y (1280×720 HUD space). HudNet (the hook's rbx): +0x10e0 u64 + +0x10e8 u32 = the
generic buddy icon pair; +0x10bc u64 (not 8-aligned) + +0x10c4 s32 = the co-op grapple icon pair (index −1 =
unresolved). Marker (the hook's r12): +0x50 u32 type (2 = grappled buddy).

### 3.8 Popups and clocks

Player 0's HUD popup object: pointer at image 0x18061c8 (= 0x1805fa0 + 0x228); its +0x2b8 = pointer to the current
popup entry, whose first u32 is the entry id; 0xc2e4ad43 = rank-up entry. Rank-up word (entry word 11, +0x2c):
low 16 bits = rank − 1 shown as "ranks/<rank>", bits 16.. = prestige byte. Before a push the game sets the u64 at
image 0x1806480 and the u64 at image 0x1b24bd8 to the address image 0x1805fa0 (the UI popup owner).
Local clock: u32 at image 0x17b4ba4, 300 ticks per second.

### 3.9 Names for the string-id rule

The id of a name is `u3h_crc_sid(name)`; "slash form" = the same name with its one ':' replaced by '/'.
HUD materials (rename the PS4 id to the slash-form id), 16 names:
`general/hud/alpha-mask-icon:default`, `general/hud/alpha-only-icon:default`,
`general/hud/buttons/up-button-pulse-big:default`, `general/hud/store:locked`, `general/hud/store:purchased`,
`general/hud/trophy:blank`, `general/hud/trophy:outline`, `general/hud/trophy:solid`,
`general/menu/gradient-bar:gradient-bar`, `general/net/flag-icons:treasure`,
`general/net/medals/medal-sheen:default`, `general/net/mode-buttons:check-mark`,
`general/net/mode-buttons:notification`, `general/net/mode-buttons:question-mark`,
`general/net/mode-buttons:warning`, `general/net/treasures:treasure-chest`.
Score-plate textures (same rule), 4 names: `general/net/mini-score:default`,
`general/net/mini-score:hud-round-score-border`, `general/net/mini-score:hud-round-score-fill`,
`general/net/message-back:default`.
The ids are pure; when they are computed (each pass, or once and kept in U3X_BSS) is free, and testers must not
record `u3h_crc_sid` calls. No PS4 id equals any slash-form id.

## 4. Behaviour

### Part A

**4.1 `u3x_round_mostly_over`.** `limit` ≤ 0: 0. Otherwise R = `limit` − `elapsed` as a wrapping 32-bit difference
read as signed; result 1 when 4·R (in 64 bits) < `limit`, else 0 (a negative R gives 1).

**4.2 `u3x_match_nearly_decided`.** 1 when `lead_wins` > 1 or `round` ≥ `rounds` − 1 (signed; `rounds` − 1 in 32
bits), else 0.

**4.3 `u3x_comp_weapon_answer`.** Query 0xa1443de7: `*answer` = 1 when `locked` ≠ 0 else 0; result 1. Query
0x1720a1ed: `*answer` = `weapon` as s32; result 1. Other: result 0, `*answer` untouched.

**4.4 `u3x_lan_address`.** n = `*count`. For k = 0 .. min(n, 32) − 1: record k (`records` + 0x28·k) "has the name"
when its first 0x24 bytes equal `name`'s bytes up to and including the record's first 0 byte (or all 0x24 bytes
are equal when the record has no 0 among them); `name` is read only up to the first difference or that 0. The first such record gets u32 +0x24 = `ip`; done (count unchanged).
Otherwise record (n AND 31) gets exactly 0x24 bytes copied from `name` (bytes after its terminator are copied too)
and +0x24 = `ip`; `*count` = n + 1 (wrapping).

**4.5 `u3x_net_player_id` / `u3x_rp_trk_id`.** G12 3.1: low = u32 +0x1e4 AND 0xfff; when u8 +0x1ec ≠ 0 and low ≠
0, low += 0x800; result = low + 0x1000·(u32 +0x1e8), modulo 2^32. Reads only.

**4.6 `u3x_has_tag`.** Walk from the node the bucket head points to; stop at a null node, at the head itself, or
after 4096 nodes. The first node whose tag id equals `id`: bit b = its bit number; result = bit (b AND 63) of the
u64 at `tags` + 0x20 + 8·(b >> 6) (0 or 1). No such node: 0.

**4.7 `u3x_score_member`.** For k = 0 .. min(count, 128) − 1: the first k whose record id equals `id` → k. Else −1.

**4.8 `u3x_weapon_stat_number`.** The first k in 0 .. 18 with `fixed[k]` = `weapon` → k. Else `extra` null → 0.
Else the first k in 0 .. min(count, 64) − 1 with ids[k] = `weapon` → 0x13 + k (the ids pointer is not tested
for null). Else 0.

**4.9 `u3x_weapon_for_stat`.** `number` < 0x13: `fixed[number]`. Else `extra` null, or `number` ≥ count + 0x13
(32-bit wrapping sum): 0. Else ids[`number` − 0x13] (no 64 cap, no null test of ids).

**4.10 `u3x_entry_by_address`.** The first of the 32 entries from `entries` (stride 0x220) whose in-use byte is ≠ 0,
whose address equals `ip` and whose u16 port equals the low 16 bits of `port`; null when none.

**4.11 `u3x_entry_by_id`.** For i = 0 .. 31 in order: E = `u3h_cm_entry(base, i)`; the first whose s32 id equals
`id` (in use or not) → E. None: null. (Every call up to the match is made.)

**4.12 `u3x_connection_state`.** E = `u3x_entry_by_id(base, id)`; E null, in-use byte 0 or s32 +0x34 = 0: −1. Else 2.

**4.13 `u3x_double_points`.** 1 when the event word (3.4) ≠ 0 and the event team ≠ (`np` ? s32 `np` + 0x21c : 0);
else 0.

**4.14 `u3x_hunter_index`.** The first i in 0 .. min(count, 32) − 1 whose hunter id equals `id`; none: min(count,
32). Reads only.

**4.15 `u3x_gt_header`.** Four `u3x_gt_write` calls on the stream `b`, in this order: (`flag`, 1); (u32 sub+8 AND
0x7ff, 11); (u32 sub+0xc AND 0x3ff, 10); (u32 sub+0x14, 32). `flag` is passed whole (the writer keeps its low bit).

**4.16 `u3x_gt_base_read`.** In order: `u3h_gt_stamp(base, sub, 0xa8)`; r = `u3x_gt_base_header(base, sub, b)`;
W = `u3h_bc_state(0)`, C = `u3h_bc_state(1)`. When r ≠ 0: *W = 3; *C = *C + 1; `u3x_gt_vcall(sub, 1, b, null)`
(result ignored); eb[102] + 1. Then (always) *W = 2. Result r. W and C are volatile: each store is made once, in
this order.

**4.17 `u3x_link_timeout`.** i = (byte distance from `u3h_cm_entry(base, 0)` to `e`) / 0x220, signed, truncated
toward zero. now = `u3x_loc_now(base)` (read once, before the range test). i < 0 or i > 31: result 0, nothing
else. Timed out (the game's player-loop test, PS3 f_947d28 with 20 s): t = float bits at `e` + 0x40 + 0xa8; timed
out when t is not ±0 and `f_less(0x41a00000 /*20.0*/, f_sub(NOW, t))`, NOW as G11 (`f_mul(f_from_int(T),
0x3b5a740e)`, T = s32 at frame clock + 0x114, frame clock = pointer at image 0x1b24b98, no null test). A NaN t
counts as non-zero and then fails the comparison. Not timed out: `kick_at[i]` = 0, result 0. Timed out: when
`kick_at[i]` ≠ 0 and (now − `kick_at[i]`, 32-bit wrapping, signed) < 3000 (10 s): result 0, nothing written.
Otherwise `kick_at[i]` = now, or 1 when now is 0; result 1.

### Part B

**4.18 `u3x_np_alive`.** 1 when, tested in this order (stopping at the first that holds): `u3x_np_has_effect(s, p,
0xc054e78b)` ≠ 0; `u3x_handle_obj_valid(p + 0x10)` ≠ 0 and s32 +0x39c ≤ s32 +0x3a0; u8 +0x770 ≠ 0. Else 0.

**4.19 `u3x_net_local_players_tick`.** u32 at `s` + 0x3d44 + 1 (always). Then nothing more when native 53 or native
62 (3.6) is null. Otherwise for i = 0, 1: P = pointer at `mgr` + 0x370 + 8·i; skip a null P; with `variant` = 17
skip P whose u8 +0x5e4 ≠ 0 (PS3 f_31ad5c; variant 18 = PS3 f_31b458 has no such skip); else
`u3x_respawn_tick(base, s, P, i)`.

**4.20 `u3x_x53_levels`.** Fills levels that a guest stored as 0 before its member data arrived. For i = 0 .. 9:
NP = net player i; skip unless `u3x_net_player_in_game(NP)`. R = game 0x7a3400(image 0x17d6420, NP + 0x5b8);
skip when R is null or its u8 +0xe is 0 (L = that byte). When u32 NP + 0x204 is 0, it becomes L. id =
`u3x_net_player_id(NP)`; skip when id is 0. Then for the score keeper (image 0x17dd780) and then the second
snapshot (image 0x17e8388), for j = 0 .. 15 (count not consulted): member record j whose id equals id and whose
level byte (+0xb) is 0 gets L.

**4.21 `u3x_hud_alias_names`.** D = u32 at `s` + 0x3e00 (low 32 bits of the table pointer last renamed). UI =
pointer at image 0x1b24bc8; T = UI ? pointer at UI + 0x20 : null. T null: D = 0, return. D equals the low 32 bits
of T: return. N = u16 T + 2; E = pointer T + 8; E null: return (D unchanged). For entries 0 .. min(N, 1024) − 1:
when the entry's id equals the id of one of the 16 HUD names of 3.9, it becomes that name's slash-form id (at most
one change per entry). Then D = low 32 bits of T.

**4.22 `u3x_k47_hud`.** eb[1] bit 0x2000 set: return. F = native 13; F null: return. V = F(0x9353ed16); not
ptr_ok: return. ST = pointer at V + 0x78; not ptr_ok: return. diag[0] + 1; diag[2] = low 32 bits of ST. For slot
offsets 0x08, 0x10, .. 0x118: D = pointer in the slot; skip unless ptr_ok; when u32 D + 0 equals the id of one of
the 4 score-plate names of 3.9, it becomes its slash-form id and diag[1] + 1. `base` is not used.

**4.23 `u3x_e01_hud`.** eb[1] bit 0x80000000 set: return. Symbol as 4.22; W = pointer at V + 0x28; not ptr_ok:
return. diag[0] + 1; diag[2] = low 32 bits of W. X, Y = the two u32 at W, W + 4. When (X, Y) are PS4's origin
patterns: write PS3's origin patterns (X first), diag[1] + 1. Else, unless (X, Y) are PS3's patterns, diag[3] =
X. (Pattern equality = float equality here: none of the four values is 0 or NaN.) The four patterns: open
question 2.

**4.24 `u3x_ci_icon`.** `r`: the hook's saved registers, r[3] = game r12 = marker M, r[4] = game rbx = HudNet H,
r[10] = rsi, r[11] = rdx (written back to the game). First, always: r[10] = u64 H + 0x10e0; r[11] = u32 H + 0x10e8
(zero-extended); diag[0] + 1 (H is not tested). Then stop unless ptr_ok(M) and u32 M + 0x50 = 2. G = u64 at
H + 0x10bc, X = s32 at H + 0x10c4. When the low 32 bits of G are 0 or X < 0: diag[2] + 1, stop. Else r[10] = G,
r[11] = X as u32 (zero-extended), diag[1] + 1.

**4.25 `u3x_rank_popup_tick`.** O = pointer at image 0x18061c8 (read once). (1) sRGB gate: when eb[3] bit 0x400000
is clear and ptr_ok(O): P = pointer at O + 0x2b8; when ptr_ok(P) and u32 P = 0xc2e4ad43: ea[240] |= 0x20, ea[166]
+ 1. (2) Test aid, only with eb[3] bit 0x800000: when ea[123] (match start, kept by another job; bit 0 is a flag) is
0: ea[167] = 0, stop. O not ptr_ok: stop. n = ea[167]; n ≥ 8: stop. now = u32 image 0x17b4ba4; stop while (now −
(ea[123] AND ~1)) as signed 32 bits < 6300 + 3600·n (21 s, then every 12 s). Entry: 18 u32, all 0 except word 0 =
0xc2e4ad43, words 4 and 5 = 0xffffffff, word 9 = 0x40800000 (4.0), word 11 = test word n. Store the u64 at image
0x1806480, then the u64 at image 0x1b24bd8, both = base + 0x1805fa0; call 0xc22cb0(O, entry); ea[167] = n + 1;
ea[168] = test word n. Test words (own choice: ranks 10, 20, 30, 50, 80, then prestige 1, 2, 3): 9, 19, 29, 49,
79, 0x10000, 0x10001, 0x10002. Keep the entry 16-byte aligned (it is the game's stack-built structure).

## 5. Test scenarios

Part A (pure, or with call-recording stubs of the 2.2 functions and a fake image; no live game):
1. 4.1: limit 0, −5; elapsed 0, limit/2, 3·limit/4 ± 1, > limit, negative, wrapping extremes (0x7fffffff, INT_MIN).
   4.2: lead 0/1/2 × round rounds−2/rounds−1/rounds, rounds INT_MIN.
2. 4.3: both queries with locked 0 / 5, another query (answer untouched).
3. 4.4: empty book; known name (updates address only); new name (copy of 36 bytes incl. bytes after the
   terminator); 31 → 32 → 33 entries (wrap to slot 0, count 33, search still only 32); 36-byte name without terminator.
4. 4.5: low 0 with guest flag (no 0x800), guest with low ≠ 0, user 1, user 0xfffff (wrap); 4.5 tracker = same.
5. 4.6: empty bucket, match at node 1 / n, bit 0, 63, 64, 200; a 5000-node ring without match (stops at 4096).
6. 4.7–4.9: count 0, 128, 200 (only 128 looked at); weapon in fixed / extra / neither; extra null; extra count 70
   (stat number caps at 64, for-stat does not); number 0x12, 0x13, count+0x12, count+0x13; count 0xfffffff0.
7. 4.10–4.12: entry in use / not, port high bits ignored, two matches (first wins); id at 0 / 31 / none; state with
   handle 0, not in use, both set.
8. 4.13: event 0; same team; other team; null np vs team 0.  4.14: count 0, 40 (cap 32), id at 31, missing.
9. 4.15: flag 0 / 1 / 2; sequences with high bits set (masked). Compare stream words.
10. 4.16: header result 0 and 1: call order, W ends at 2, C and eb[102] only with 1, vcall arguments.
11. 4.17: e at entry 0 / 31 / 32 / below entry 0 / 1 byte before entry 0 (index 0); t = 0, −0, NaN, recent, old;
    kick_at 0, recent, 3000 ticks ago, now = 0 (stores 1); wrapping clock.

Part B (stubs and fake memory; 4.22–4.25 are best confirmed once in the live game):
12. 4.18: each of the three tests deciding; effect true makes no handle call.
13. 4.19: natives null (counter still +1); null players; variant 17 / 18 with a leaving player.
14. 4.20: player not in game, R null, level 0 in R, +0x204 already set (kept), id 0 (only +0x204 written), records in
    both snapshots with level 0 / non-zero, a match at index 16 (not looked at).
15. 4.21: UI null, table null (D reset), same table twice (second pass does nothing), entries null (D kept), 1500
    entries (1024 looked at), each of the 16 ids, slash ids left alone.
16. 4.22 / 4.23: switch bits, native null, symbol / struct not ptr_ok, a slot with a text descriptor, all four ids,
    second pass (no change, diag[0] still counts); origin PS4 / PS3 / other.
17. 4.24: M invalid, type ≠ 2, grapple pair unresolved (low 32 bits 0, index −1), resolved (full 64 bits copied).
18. 4.25: gate bit set / clear, O invalid, popup id other; aid off; match start 0 (ea[167] reset); n = 8; times just
    before / at the threshold; entry contents and the two stores before the call.

## 6. Kept as it is (looks like a fault or dead code)

1. 4.4 copies 36 bytes of `name` even when it is shorter; the count keeps growing past 32.
2. 4.8 / 4.9 do not test the extra ids pointer; 4.9 has no 64 cap and its bound wraps.
3. 4.17: an `e` just below entry 0 truncates to index 0.
4. 4.21 remembers only the low 32 bits of the table pointer; a null entry array is retried on every call.
5. 4.22 / 4.23 count diag[0] on every frame; 4.24 reads H without a test.
6. 4.20 looks at 16 snapshot records whatever the count.
7. 4.1 treats an elapsed time past the limit as "mostly over".

## 7. What stays outside this spec

The C wrappers and `u3h_` exports; `x72_skins`, `x45_level_tick`, `x61_round_clock` and the in-match gate of
`n25_tick` (own sequencing); the arrays `gt_kick_at`, `k47_d`, `e01_d`, `ci_d` (stay in C); `u3x_net_player_in_game`,
`u3x_loc_now`, `u3x_gt_vcall` (tiny, shared). Judged own glue and left out: none of the 26 sweep rows (`hunter_index`
becomes `u3x_hunter_index`, `alive` becomes `u3x_np_alive`, `n25_tick` delivers only its game part).

## 8. Open questions

1. Are the 20 asset names of 3.9 acceptable in the new source (they are names, from which every table value is
   computed by the game's own id rule), or should they come from a file the import writes?
2. Weapon panel origin: two pairs of floats (PS4's shipped origin, which is what the field holds before the patch,
   and PS3 1.19's origin, PS3-only data with no rule). Options: (a) the owner rules them interop constants (like
   ids) and they are given to the implementer; (b) the import reads PS3's hud-settings and writes the pair to a file
   the helper reads; (c) the tester supplies them as the expected values. Until decided, `u3x_e01_hud` cannot be
   finished.

## Clarifications

Added by the tester (2026-11-14). Facts only, confirmed by the old-versus-new test (`reimpl2\S06\test_s06.cpp`,
with the old C part built both as the helper ships it, x87 / `-mno-sse`, and with SSE), where the text above is
silent or can be read two ways. Every statement of sections 4 and 6 that the test checks against v7.212 holds; the
new code (part_a.cpp, part_b.cpp) gives the same results, memory and call sequences at -O1 and -O2.

1. Tables (3.9, open question 1): each of the 20 names gives, with the game's id rule, exactly one PS4 / slash-form
   pair of the old tables, in the same order as the names; no PS4 id equals any slash-form id. `u3h_crc_sid` is pure,
   so computing the ids every pass (as the new code does) changes nothing observable.
2. Weapon panel origin (4.23, open question 2): the two pairs are whole numbers in 1280×720 HUD space. Running the old
   function over every whole-number origin of that space rewrites exactly one pair (PS4's) and leaves exactly one
   pair alone without a note (PS3's, the one it writes). The new code asks `u3h_e01_origin(0)` and then
   `u3h_e01_origin(1)` once on every pass that reaches the origin test (after diag[0] / diag[2] are written).
3. 4.17, order and read points: the `u3h_cm_entry(base, 0)` call comes first, then the `u3x_loc_now` call; both are
   made for every `e`, also out of range. The frame clock pointer (image 0x1b24b98) and its +0x114 are read only
   when the index is in range and t is not ±0 (a null or unmapped clock pointer is never touched then); t is read
   after both calls.
4. 4.17, index: v7.212 truncates the quotient to 32 bits (signed). This differs from the 64-bit quotient only when `e`
   lies 2^31 entries (about 1.2 TB) or more from entry 0: an `e` exactly 2^32 entries past entry 0 is index 0 in
   v7.212 (it then reads t there and may write kick_at[0]) and out of range (result 0, nothing written) in the new
   code. Real callers only pass connection entries, so no reachable input differs.
5. 4.17, wraps: (now − kick_at[i]) is a wrapping 32-bit difference read as signed in v7.212 as compiled at -O1 and
   -O2 (checked at INT_MIN / INT_MAX neighbours). 4.1's R and 4.2's `rounds` − 1 wrap the same way (INT_MIN checked).
6. 4.17, float: v7.212 built with SSE equals the new code bit for bit in every case. The shipped x87 build keeps
   NOW = T × (1/300 pattern) unrounded (T is not rounded to a float either; x87 precision control 64-bit and 53-bit
   give identical decisions). With t stamped by the C part's own `gt_stamp`, the new NOW − t differs from the shipped
   value by at most 0.80 ulp of NOW for clocks below 2^24 ticks (15.5 h) and 1.35 ulp above. Decisions: the 20 s
   test differs only at the boundary tick: the new code reports a timed-out link 1 tick (1/300 s) later than the
   shipped helper in 49 % of links when the clock is below 1 h, up to 2 ticks later in 56 % of links up to 15.5 h,
   and up to 7 ticks (23 ms) in 86 % of links from 15.5 h to 72 h (in 4 % of those earlier). Evaluating T × k − t
   in double precision (T, k, t widened exactly) gave the shipped decision at every tick of 60,000 simulated links.
7. 4.16: `u3h_bc_state(0)` then `u3h_bc_state(1)` are called right after the header call, also when r = 0; with
   r ≠ 0 the stores are *W = 3, then *C + 1, then the slot-1 call, then eb[102] + 1, then *W = 2.
8. 4.19: only `variant` = 17 skips a leaving player; every other value (18, 0, 19, any) behaves as 18.
9. 4.20: when R's byte +0xe is 0 nothing at all is written for that player (not even +0x204). The member-data call
   is made with (image 0x17d6420, NP + 0x5b8) for every player in the game, in player order.
10. 4.22 / 4.23: native 13 is called once per pass with the single integer argument 0x9353ed16 (host calling
    convention, nothing else passed).
11. 4.23, diag[3] with a signaling-NaN x (exponent all ones, quiet bit 22 clear): the shipped x87 build at -O2 notes x
    with bit 22 set (the value passes through the x87 unit); the -O1 x87 build, the SSE build and the new code note x
    unchanged. Diagnostics only; the origin field never holds a NaN.
12. 4.24: the u64 at H + 0x10bc is read as one 64-bit value (address 4 mod 8); two 32-bit halves give the same value.
13. 4.25: the entry handed to 0xc22cb0 is 16-byte aligned in v7.212 and in the new code; both owner words are stored
    before the call; with ea[123] = 0 ea[167] is reset even when O is not ptr_ok.
14. 4.6: the 4096th node (index 4095 from the first) is still tested; a match first found at the 4097th is not.
