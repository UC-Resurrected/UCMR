# S09. Small game rules found by the tiny-function check

Behaviour description for a reimplementation (reimpl2 pipeline, describer's document, sweep group S09). It says
what the functions do, not how the old ones were written. Offsets are hexadecimal when prefixed with 0x. "Image X"
means the address `base + X` (`base` = game image base). All values little-endian; u8/u16/u32/s32/u64 are integers
of that width; pointers are 8 bytes. Game functions use the System V AMD64 convention.

Source of the list: `reimpl2\S09\FUNCTIONS.txt` (26 GAME rows, 13 UNSURE rows of the one-by-one tiny check,
`reimpl2\TINY\result00/01/02/4.md`). Old code: v7.212 (`u3_lan_profiles.c` hand-written part, `kr_port.inc`,
`u3x_fixes.cpp`), about 45 lines in all (one or two lines per function).

Related documents (names and rules used here, not described again): `G11_game_sockets_time.md` (net clock),
`G12_packets_monitor.md` 3.1 (net player id), `G18_friends_list.md` 4.x (friend click), `G22_rank_xp.md` 4.5
("box XP"), `G23_boosters.md`, `G30_post_match.md`, `G36_matchmaking.md` 5.9, `G42_death_cameras.md`,
`G44_kill_rewards.md` 4.13, `G46_kill_record.md`, `G05_cinema_directory.md`, `S01_matchmaking_leftovers.md` 4.9,
`S06_net_kill_leftovers.md` 4.17, `S07_texts_helpers_leftovers.md` 4.13, `film_helpers.md`, `G03_film_playback.md`.

**Float.** Nothing delivered here computes with floats. The only float row of the list (`gt_timed_out`) is
already described by S06 4.17 and gets no new code here (1.3).

## 1. Purpose and callers

### 1.1 Rows that get new code (game rules with live callers)

| Old function | Where | Called from (v7.212) | Section |
|---|---|---|---|
| `u3h_rp_assert` | C :13932 | `u3x_stream_read`, `u3x_stream_write` (film bit streams, past the capacity) | 4.1 |
| `so_blob_swap` | C :7532 | C `so_msg_swap` (message 0x11d of 0x30 bytes), `so_iv_new` (copy before base64), `so_inv_poll` (rows handed out) | 4.2 |
| `np_code` | C :3084 | C export `u3h_np_code` (C++ callers at :12252, :12312, :13898) | 4.3 |
| `fj_solo` | C :4479 | C `fj_host_answer` (a join request asking for a fresh party) | 4.4 |
| `player_user`, `lg_menu_user` (C), `player_user` (C++ private) | C :2170, :4700; C++ :12668 | C `comp_weapon_source`; C `legacy_kind`, `lg_profile_box`, `records_source`; C++ :12691 | 4.5 |
| `splitscreen` | C :2660 | C export `u3h_splitscreen`, from `u3x_menu_action` (action 0xcd127c29) | 4.6 |
| `onm_state` | C :11570 | C `onm_stop`, `onm_fail`, `onm_tick`; C export `u3h_onm_state` (G36's search start) | 4.7 |
| `n04_due` | C :12447 | C `m05_tick` (stand-in tick, stays C); C `n09_tick` (replaced by G36) | 4.8 |
| `kr_statw` | kr_port.inc:105 | C export `u3h_kr_statw` (G44 kill rewards, three sites) | 4.9 |
| `u3x_click_sound` | C++ :805 | C `k49_st` (loadout list click) | 4.10 |
| `u3x_booster_already` | C++ :808 | C booster click handler (:9400) | 4.11 |
| `friend_group` (C++ private) | C++ :951 | `friend_order` of `u3x_friend_sort` | 4.12 |
| `u3x_xp_percent` | C++ :1089 | C `lg_profile_box`; G22's new `u3x_lg_profile_box` | 4.13 |
| `u3x_shows_match_stats` | C++ :1108 | post-match handler (C++ :21884; G30 part C) | 4.14 |
| `u3x_waiting_joiner` | C++ :1203 | agc_port.inc:74 (Elimination round end); G35 | 4.15 |
| `u3x_net_player_in_game` | C++ :1899 | medal code (:19228, :19486; G45), `kr_active` (kr_port.inc:80) | 4.16 |
| `u3x_gt_bytes` | C++ :1581 | C `gt_words` (game socket datagram length; G11) | 4.17 |
| `u3x_session_host` | C++ :5846 | C++ :5862, :5908, :16214, :16226, :22315; C `gt_is_host` (G12, G14, G30, G34) | 4.18 |
| `u3x_kr_34fc04`, `u3x_kr_37c984`, `u3x_kr_37c914`, `u3x_kr_30ffa8` | C++ :8111..:8125 | kill record / rewards (:8145, :16422, :16503..:16840; G44, G46); kr_port.inc forwards | 4.19 |
| `u3x_split_screen` | C++ :9087 | C++ :9168; C `pm_splitscreen` (G31, G60) | 4.20 |
| `u3x_rp_trk_live`, `u3x_rp_dead` | C++ :18542, :18543 | film player cycle (:18999; G03); C `rp_dead` (:5346, :5422, :5435; film cameras, G60) | 4.21 |

### 1.2 UNSURE rows judged own glue (no new code)

| Old function | Reason |
|---|---|
| `f4_mp` (C :12238) | Gates the helper's own switch word (eb[3]) in its own kind-234 draw diagnostic/fix `f4_inst`. It only reads two documented game flags (u8 image 0x17d5c61 "net active", u8 image 0x17d5c60 "LAN"); the decision to apply the helper's switch in multiplayer is the helper's, not a game condition. Stays as it is. |
| `n03_offline` (C :12363) | Combines the helper's own switch (ec[2] bit 0), the helper's own backend mode (`onb_mode` = 2) and the same two game flags. Helper logic only. Stays. |
| `u3x_booster_level_mark` (C++ :1104) | The tag 0x2e4c0000 ("." "L") OR level is the helper's own marker: written into an unused word of a type-3 icon answer by the helper (G23 4.3) and read back only by the helper's own draw hook `k46_draw` (kind 168, C :9518). The game never reads it. Stays as it is (G23 2.x already gives its contract). |

### 1.3 Rows whose rule is already described by another spec (no new code here)

These are game rules, but the spec named here already describes the same rule inside the new code of the function
that calls it. When that group is merged, the old function has no caller left and the integrator deletes it.

| Old function | Rule | Described in | After the merge |
|---|---|---|---|
| `gt_timed_out` (C :6483) | PS3 f_947d28 link silence test (20 s) | S06 4.17 (`u3x_link_timeout`) | only caller `gt_link_timeout` becomes S06's wrapper: delete |
| `fr_closed` (C :4159) | PS3 f_283640: closed when presence data is missing or its byte 7 is set | G18 (friend click, "D byte 7 is 0") | only caller `fr_click` replaced by G18: delete |
| `lg_xp4fc` (C :4736) | PS3 1.19 f_5b134 "box XP": record +0x4fc only in load state 3 | G22 4.5 ("Box XP of a profile") | callers `legacy_kind`, `lg_profile_box` replaced by G22: delete |
| `cin_net_flag` (C :4954) | PS3 f_3255c4: net object +0x14 or +0x10 non-zero | G05 4.5 ("current online flag", 3.7), S04 A4 | only caller `cin_scan` replaced by G05: delete |
| `onm_members` (C :11582) | count of the 10 member slots in use | S01 4.9 ("members") | S01 deletes it with `onm_tick`'s helpers (4.4 below is the same count, for `fj_solo`) |
| `u3x_camera_slot` (C++ :2279) | fixed-camera slot = index remainder, −1 when negative | G42 step 5 (G42 6.4) | only caller (C :3014) replaced by G42: delete |
| `u3x_mm_starting`, `u3x_mm_joining` (C++ :2225, :2226) | net states 0x2c..0x30 / joining 0xb, 0xf, creating 0x11, 0x28 | G36 5.9 step 2 | only caller `mm_start_tick` replaced by G36: delete |
| `local` (C++ :16407, member of the kill-record struct) | PS3 f_359694 "local player": address equals NGM +0x370 or +0x378 | G44 3.x / 6.5, G46 ("Local(P)") | the struct goes with G44 / G46's new code |

### 1.4 Rows with no caller (dead code)

| Old function | Rule | Action |
|---|---|---|
| `np_in_game` (C :3038) | a null record is not in the game; else rule 4.16 | no caller in v7.212 (only its definition): delete |
| `kr_local` (kr_port.inc:77) | same as `local` in 1.3 | no caller: delete |
| `kr_np_at` (kr_port.inc:82) | net player slot i: null unless the net-active byte (image 0x17d5c61) is set and i ≤ 9 (unsigned); else image 0x17cb060 + 0x390 + 0x9c0·i | no caller: delete |

Deleting a function that is never called changes no behaviour. If the integrator prefers to keep them, `np_in_game`
becomes "null pointer: 0, else `u3x_net_player_in_game`", `kr_local` keeps its one comparison (it is not delivered
again).

All functions run on the game's main thread, except the bit-stream assert (4.1), which runs on whichever thread
reads or writes a film stream (the film worker included); none keeps state.

## 2. Interface

### 2.1 Functions to deliver (all `extern "C"`)

Part A: new C++ entry points for rules that live in the C part today. Each old C function becomes a one-line
wrapper (C declarations with `__attribute__((visibility("hidden")))`, as the C part declares its other `u3x_`
imports).

| New name | Signature | C wrapper |
|---|---|---|
| `u3x_stream_assert` | `void u3x_stream_assert(uintptr_t base)` | `u3h_rp_assert(base)` = `u3x_stream_assert(base);` |
| `u3x_invite_blob_swap` | `void u3x_invite_blob_swap(uint8_t *blob)` | `so_blob_swap(b)` = `u3x_invite_blob_swap(b);` |
| (existing) `u3x_net_player_id` | `uint32_t u3x_net_player_id(const uint8_t *np)` (G12 3.1, S06 4.5) | `np_code(q)` = `return u3x_net_player_id(q);` (nothing new to write) |
| `u3x_session_member_count` | `uint32_t u3x_session_member_count(const uint8_t *session)` | `fj_solo(ps)` = `return u3x_session_member_count(ps) <= 1;` |
| `u3x_menu_user` | `uint32_t u3x_menu_user(uintptr_t base)` | C `player_user(base)` and C `lg_menu_user(base)` = `return u3x_menu_user(base);`; the C++ file-private `player_user(base)` (:12668) likewise |
| `u3x_splitscreen_dialog` | `void u3x_splitscreen_dialog(uintptr_t base, void *s)` | `splitscreen(base, s)` = `u3x_splitscreen_dialog(base, s);` |
| `u3x_net_state_set` | `void u3x_net_state_set(uintptr_t base, uint32_t state)` | `onm_state(base, st)` = `u3x_net_state_set(base, st);` |
| `u3x_net_deadline_due` | `int u3x_net_deadline_due(uintptr_t base)` | `n04_due(base)` = `return u3x_net_deadline_due(base);` |
| `u3x_kr_statw` | `uint32_t u3x_kr_statw(uintptr_t base, uint32_t index)` | `kr_statw(base, idx)` = `return u3x_kr_statw(base, idx);` (`u3h_kr_statw` unchanged) |

Part B: existing C++ exports, re-implemented in place under the same names and signatures (their callers in the C
part and in other groups' new code stay), plus one new export for a file-private helper.

| Name | Signature |
|---|---|
| `u3x_click_sound` | `int u3x_click_sound(int unlock_status)` |
| `u3x_booster_already` | `int u3x_booster_already(uint32_t slot_word, uint32_t player_word, uint32_t booster)` |
| `u3x_friend_group` (new export) | `int u3x_friend_group(const uint8_t *entry)`; the private `friend_group(e)` becomes `return u3x_friend_group(e);` (or `friend_order` calls it directly) |
| `u3x_xp_percent` | `int32_t u3x_xp_percent(int32_t low, int32_t high, int32_t xp)` |
| `u3x_match_stats_shown` (new, replaces `u3x_shows_match_stats`) | `int u3x_match_stats_shown(uintptr_t base, uint32_t mode, int flag)`; see the note below |
| `u3x_waiting_joiner` | `int u3x_waiting_joiner(const uint8_t *net_player)` |
| `u3x_net_player_in_game` | `int u3x_net_player_in_game(const uint8_t *np)` |
| `u3x_gt_bytes` | `uint32_t u3x_gt_bytes(const GtStream *s)` (`GtStream` keeps its layout, 3.6) |
| `u3x_session_host` | `int u3x_session_host(const uint8_t *s)` |
| `u3x_kr_34fc04` | `int u3x_kr_34fc04(uintptr_t base)` |
| `u3x_kr_37c984` | `int32_t u3x_kr_37c984(uintptr_t base)` |
| `u3x_kr_37c914` | `int32_t u3x_kr_37c914(uintptr_t base)` |
| `u3x_kr_30ffa8` | `int u3x_kr_30ffa8(uintptr_t base, const uint8_t *np, int b)` |
| `u3x_split_screen` | `int u3x_split_screen(uintptr_t base)` |
| `u3x_rp_trk_live` | `int u3x_rp_trk_live(uint8_t *t)` |
| `u3x_rp_dead` | `int u3x_rp_dead(const uint8_t *t)` |

**Note on `u3x_shows_match_stats`.** Its rule excludes a list of three game mode values, and the running game holds
that list (3.4). Reading it needs `base`, which the old signature lacks. So the function is delivered as
`u3x_match_stats_shown(base, mode, flag)`; the old export `u3x_shows_match_stats(mode, flag)` is removed and its
callers are renamed at integration: the old C++ post-match handler (:21884) and G30's `part_c.cpp` (line 108; `base`
is in scope at both). Results are identical (4.14). See open question 7.2.

The kr_port.inc forwards `kr_34fc04`, `kr_37c984`, `kr_37c914`, `kr_30ffa8`, `kr_active`, `kr_pid` and the C
forwards `pm_splitscreen`, `gt_is_host`, `gt_words`, `k49_st`, `rp_dead` are already one-line forwards to the names
above and stay as they are.

Old code about 45 lines; the new code is of the same order. One implementer is enough; for two, part A and part B
above are independent (part B's `u3x_net_player_in_game` is used by part B's `u3x_waiting_joiner` only if the
implementer chooses to; neither part calls the other).

### 2.2 Functions of other groups, of the C part and of the shared tools that are called

| Name | Signature | Effect |
|---|---|---|
| `u3x_rp_assert` | `void u3x_rp_assert(uintptr_t base, uintptr_t a, uintptr_t b, uintptr_t c, uint32_t line)` | film helpers (stays): when the game's assert handler pointer (image 0x1bf27b8) is not null, calls it with (base + a, base + b, base + c, base + 0x13ac250, line, 0); then executes `int 0x41` (the game's break; in practice it does not return) |
| `u3x_net_player_id` | `uint32_t u3x_net_player_id(const uint8_t *np)` | G12 3.1: low = u32 +0x1e4 AND 0xfff; +0x800 when u8 +0x1ec ≠ 0 and low ≠ 0; plus (u32 +0x1e8) · 0x1000, mod 2^32. Pure |
| `u3h_show_dialog` | `void u3h_show_dialog(uintptr_t base, void *s, uint32_t body, uint32_t title, uint32_t action, int32_t buttons, uint32_t extra, uint32_t close)` | C part: fills the game's dialog (menu +0x1438) and shows the dialog page; does nothing without a dialog object or a page shower |
| `u3x_net_clock` | `int32_t u3x_net_clock(uintptr_t base)` | G11: s32 at +0xc of the object the pointer at image 0x17c3580 points to; 0 when that pointer is null |
| `u3x_weapon_for_stat` | `uint32_t u3x_weapon_for_stat(const uint32_t *fixed, const uint32_t *extra, uint32_t number)` | G45 / S06: weapon id of stat number `number`: `fixed[number]` below 0x13; otherwise from the extra list (u32 count at extra +0, pointer to the u32 ids at extra +8), 0 when `extra` is null or the number is past the list |
| `u3h_kr_mode_type` | `int32_t u3h_kr_mode_type(uintptr_t base)` | C part: the current mode entry's type (s32 at +0xc of game call 0x721050(image 0x17cb060)), −1 without an entry |
| `u3h_kr_30feec` | `int u3h_kr_30feec(uintptr_t base)` | C part (PS3 f_30feec): 1 when the mode type is 0xd, else whether game call 0x7424a0(image 0x17cb060) returns 0x4b720b62 |
| `u3x_kr_dc_int` | `int32_t u3x_kr_dc_int(uintptr_t base, uint32_t off)` | G44 4.13 table (stays): game call 0x11f2bf0(0xea5596b8) gives a pointer to a pointer; result 0 when either is null, else the s32 at byte offset `off` of the data |
| `u3x_rp_h` | `uint8_t *u3x_rp_h(const uint8_t *h)` | film helpers (stays): the object of a game handle {pointer to pointer at +0, u32 id at +8}, or null when the handle does not resolve |

From CONVENTIONS.h: `rd`, `at`, `game_call`.

## 3. Game facts

### 3.1 The film bit-stream assert (4.1)

The game raises its own assert, through the handler pointer at image 0x1bf27b8, when a bit stream read goes past
its capacity. Its four arguments at the reader's assert sites are interface facts of that one call:

| Argument | Image offset | Content |
|---|---|---|
| condition text | 0x14b92fc | "m_maxBits >= m_numBits" |
| source file text | 0x14b9313 | the game's own bitstream source path |
| function name text | 0x14b9357 | "ReadBits" |
| line | (value) 0xac = 172 | the reader's line |
| message text (added by `u3x_rp_assert`) | 0x13ac250 | empty text |

Checked in the PS4 image: the game inlines this assert at 886 places with exactly these values, e.g. image
0x667040..0x66707c (load of the handler pointer 0x1bf27b8, test, the three text addresses, 0x13ac250, line 0xac,
0, call, `int 0x41`). The game's writer uses the function name at 0x14b934d ("WriteBits") and line 0x97 instead
(815 places); the helper passes the reader's values also for its writer (6.1).

### 3.2 The invite record (4.2)

G20 3.3: 0x30 bytes; u32 "key" at +0, u64 party id at +8, the name (0x14 bytes) at +0x10, u64 send time at +0x28;
bytes 4..7 and 0x24..0x27 unused. PS3 1.19 (PS3 f_294888) keeps the two u64 fields and the key big-endian, PS4
little-endian; the name is bytes.

### 3.3 Sessions, players, menu

| What | Where | Meaning |
|---|---|---|
| session member slots | session + 0x680 + 0x120·i, i = 0..9 | u8 at slot + 0x25 = in use (G07 3.6, G21) |
| session room id | u64 at session + 0x18 | 0 = no room |
| own / host member id | u32 at session + 0x12e4 / + 0x12e8 | |
| net player record | (G12, G35) | u8 +0xe1 joined; u8 +0x5e4 leaving; u8 +0x5f9 joined late; u8 +0x5fa spawned this round |
| menu user | u8 image 0x17bde75 (non-zero: the menu acts for user 1); u32 image 0x17bf714 (the menu's user otherwise) | G22, G24 3.5 |
| split screen | u8 image 0x17d7b06; s32 image 0x17f2fc0 | second local player present when the byte is set or the word is 3 |
| net state | u32 image 0x1b68810 (state), u32 image 0x1b68814 (clock value when it was entered) | G30, G32, G35, G36 |
| game clock | u32 image 0x17b4ba4 | |
| status deadline | s32 at image 0x17d5c58 + 0x2c (net state object + 0x2c) | the clock value at which the current status ("GAME STARTING IN" etc.) ends (G36 3.5) |
| friend entry | s32 at entry + 0x1b0 | presence state: 1 = in this game, 2 = online elsewhere, other = offline (G18) |
| film tracker | s32 +0x39c death time, s32 +0x3a0 (G60, film cameras); handles at +0x10 and +0x20 (G03) | |
| weapon ids of stat numbers 0..0x12 | 19 u32 at image 0x13a6170 (read at run time, as before) | G30, G45 |
| extra weapon list | game call 0x11f2bf0 (`void **f(uint32_t symbol)`) with symbol 0x9e354807 | pointer to the list pointer, or null |

### 3.4 Match statistics page: the excluded modes (4.14)

The u32 "mode" word of the ended match (NGM + 0x680c, G30) decides whether the statistics page is pushed. The PS4
game applies the same rule in its own code and holds the excluded modes as a bit mask: at image 0x67c2a6 it reads
the mode word, compares it with 8 (immediate byte at image 0x67c2ac), and tests bit `mode` of the u32 immediate at
**image 0x67c2b0 = 0x00000109** (bits 0, 3 and 8: modes 0, 3 and 8, the three values of the old code); right after
it tests the u8 at image 0x17d6420 + 0x1339 (the old rule's flag). The same mask is also at image 0x67c476 and
0x73d50f. The helper does not patch these bytes (no builder or source reference to 0x67c2xx). Read it at run time
from image 0x67c2b0 (file offset 0x6802b0 of `eboot.elf`).

### 3.5 Single ids and values (kept by value)

- Mode type 0xf (`u3x_kr_34fc04`); defaults 3 and 0; DC symbol 0xea5596b8 with value offsets 4 and 0xc. The PS4
  game uses the same symbol after the same mode-type test (image 0x7a81ad..0x7a81bd: compare type with 0xf, then
  lookup 0x11f2bf0(0xea5596b8)). Its name is not known, so it is not formed by `u3h_crc_sid` (7.3).
- Sounds 0 ("picked") and 8 ("refused") (4.10).
- Splitscreen dialog: body text 0x7c31f4d0, title text 0xc6467f2d (two different texts of one dialog, not a
  table). 0xc6467f2d occurs in the PS4 image; 0x7c31f4d0 does not (a PS3 1.19 text id; 7.4).
- Stat number threshold 0x13 = the length of the fixed weapon list (3.3).
- Split-screen word value 3.

### 3.6 Game socket bit stream (4.17)

`GtStream` (helper's replica of the game's bit stream, G11): pointer to the u32 words at +0, u32 bit position at
+8, u32 capacity in bits at +0xc, u32 at +0x10, u32 at +0x14. Words hold bits most significant first; a datagram is
always a whole number of these words (the peer reads it as words).

## 4. Behaviour

4.1 **`u3x_stream_assert(base)`**: one call `u3x_rp_assert(base, 0x14b92fc, 0x14b9313, 0x14b9357, 0xac)` (3.1).
Nothing else. The four values are named constants of the new code (interface facts of the one call site).

4.2 **`u3x_invite_blob_swap(blob)`**: reverses the byte order of three fields in place: the 4 bytes at +0, the 8
bytes at +8 and the 8 bytes at +0x28. The other 28 bytes are untouched. Applying it twice gives the original
record. `blob` is never null (not checked).

4.3 **`np_code`**: the rule is exactly G12 3.1's net player id (low 12 bits of +0x1e4, plus 0x800 when byte +0x1ec
is set and those bits are non-zero, plus the u32 at +0x1e8 times 0x1000, all mod 2^32). The old code multiplied a
signed copy of +0x1e8; modulo 2^32 the result is the same. Nothing to write: the wrapper forwards.

4.4 **`u3x_session_member_count(session)`**: the number of the 10 member slots (3.3) whose in-use byte is non-zero,
0..10. Pure read; `session` not checked. (`fj_solo` = this count is 0 or 1.)

4.5 **`u3x_menu_user(base)`**: 1 when the u8 at image 0x17bde75 is non-zero (the u32 is then not read); otherwise
the whole u32 at image 0x17bf714 (not masked; callers mask it themselves). S07's `u3x_player_profile` uses the same
rule and may call this function.

4.6 **`u3x_splitscreen_dialog(base, s)`** (PS3 1.19 Splitscreen menu action on a system without a second user, the
branch a single-user PS4 takes): exactly one call `u3h_show_dialog(base, s, 0x7c31f4d0, 0xc6467f2d, 0, 0, 0, 0)`.

4.7 **`u3x_net_state_set(base, state)`** (the game's "enter net state"): first the u32 at image 0x1b68810 =
`state`, then the u32 at image 0x1b68814 = the u32 at image 0x17b4ba4 (the game clock, read at that moment).

4.8 **`u3x_net_deadline_due(base)`**: 1 when `u3x_net_clock(base)` ≥ the s32 at image 0x17d5c58 + 0x2c, compared
as signed 32-bit numbers (not a wrapping difference: the shipped helper compares; e.g. clock 0x7fffffff with
deadline 0x80000000 gives 1, clock 0x80000000 with deadline 0x7fffffff gives 0); else 0. Both values are read once,
the clock first (tester's Clarification 1).

4.9 **`u3x_kr_statw(base, index)`**: index < 0x13 (unsigned): one call `u3x_weapon_for_stat(base + 0x13a6170, null,
index)` (it gives the u32 at image 0x13a6170 + 4·index); no game call. Otherwise: one game call
0x11f2bf0(0x9e354807); L = the pointer it points to, or null when the call returned null; then one call
`u3x_weapon_for_stat(base + 0x13a6170, L, index)`, its result is the result (tester's Clarification 2).

4.10 **`u3x_click_sound(status)`**: 0 when `status` is 0 or 1; 8 for every other value (negative ones too).

4.11 **`u3x_booster_already(slot_word, player_word, booster)`**: the shown booster is `slot_word`, or `player_word`
when `slot_word` is 0; result 1 when it equals `booster`, else 0.

4.12 **`u3x_friend_group(entry)`**: presence state (s32 at +0x1b0) 1 gives 0, 2 gives 1, anything else gives 2.
(The list sorts by this group first.)

4.13 **`u3x_xp_percent(low, high, xp)`**: 0 when `xp` < `low` (signed) or `high` = `low`. Otherwise
(`xp` − `low`) · 100 / (`high` − `low`): subtraction and product in 32-bit two's complement (wrapping), then signed
division truncating toward zero.

4.14 **`u3x_match_stats_shown(base, mode, flag)`**: M = the u32 at image 0x67c2b0 (3.4). Result 0 when `mode` < 32
and bit `mode` of M is set (with the game's M = 0x109: modes 0, 3, 8); 0 when `flag` is non-zero; else 1. Same
results as the old `u3x_shows_match_stats(mode, flag)` for every input. (Whether the mask is read before or after
the flag test is not observable.)

4.15 **`u3x_waiting_joiner(np)`**: 1 when, in this order, u8 +0xe1 is non-zero, u8 +0x5e4 is 0, u8 +0x5f9 is
non-zero and u8 +0x5fa is 0; else 0. No null check.

4.16 **`u3x_net_player_in_game(np)`**: 1 when u8 +0xe1 is non-zero and u8 +0x5e4 is 0; else 0. No null check.

4.17 **`u3x_gt_bytes(s)`**: U = the bit position, but at most the capacity (both u32). Result = 4 · ⌊(U + 31) / 32⌋
in 32-bit unsigned arithmetic (U + 31 wraps for U above 0xffffffe0; never with real streams).

4.18 **`u3x_session_host(s)`**: 1 when the u64 at +0x18 is 0 (the member ids are then not read); otherwise 1 when
the u32 at +0x12e4 equals the u32 at +0x12e8; else 0.

4.19 **Kill score values** (PS3 f_34fc04, f_37c984, f_37c914, f_30ffa8):
- `u3x_kr_34fc04(base)`: 1 when `u3h_kr_mode_type(base)` is 0xf, else 0.
- `u3x_kr_37c984(base)`: when `u3x_kr_34fc04(base)` is non-zero, `u3x_kr_dc_int(base, 4)`; else 3 (no lookup).
- `u3x_kr_37c914(base)`: the same with offset 0xc and default 0.
- `u3x_kr_30ffa8(base, np, b)`: `u3h_kr_30feec(base)` once; 0: result 1. Otherwise `b` when `np` is not null, 0
  when it is null (np is only compared with null).

4.20 **`u3x_split_screen(base)`**: 1 when the u8 at image 0x17d7b06 is non-zero (the word is then not read); else 1
when the s32 at image 0x17f2fc0 equals 3; else 0.

4.21 **Film trackers**:
- `u3x_rp_trk_live(t)`: 1 when `u3x_rp_h(t + 0x10)` is not null (no second call); else 1 when `u3x_rp_h(t + 0x20)`
  is not null; else 0. `t` not checked (G03 6.x: a null tracker faults in the old code too).
- `u3x_rp_dead(t)`: 1 when the s32 at +0x3a0 is less than the s32 at +0x39c (signed), else 0.

## 5. Test scenarios

Everything can be tested with fake memory (a fake image block, fake sessions, players and friend entries) and
call-recording stubs for `u3x_rp_assert`, `u3h_show_dialog`, `u3x_net_clock`, `u3h_kr_mode_type`,
`u3h_kr_30feec`, `u3x_kr_dc_int`, `u3x_rp_h`, `u3x_weapon_for_stat` and game call 0x11f2bf0. Nothing needs the live
game. Cover:

1. 4.1: one recorded call with (base, 0x14b92fc, 0x14b9313, 0x14b9357, 0xac).
2. 4.2: random 0x30-byte records: the three fields reversed, the other bytes equal, twice = identity.
3. 4.3: `np_code` and `u3x_net_player_id` equal for random records (high +0x1e8 values, +0x1ec 0 / non-zero, low
   bits 0).
4. 4.4: 0, 1, 2, 10 slots in use; non-zero byte values other than 1; `fj_solo` for each.
5. 4.5: byte 0 / non-zero; word 0, 1, 3, 4, 0xffffffff (not masked).
6. 4.6: one recorded dialog call with the eight arguments.
7. 4.7: write order (state first, then clock), clock read after the state write (a fake where the two overlap is not
   needed: the addresses differ).
8. 4.8: deadline equal to the clock (1), one ahead (0), one behind (1), clock 0x7fffffff with deadline 0x80000000 (1)
   and clock 0x80000000 with deadline 0x7fffffff (0) (signed compare), null clock pointer (clock 0).
9. 4.9: index 0, 0x12 (one `u3x_weapon_for_stat` call with a null list, no game call), 0x13, large values; lookup
   returning null, a pointer to null, a list.
10. 4.10..4.13: statuses −1, 0, 1, 2; slot word 0 / non-zero; friend states 0, 1, 2, 3, −1; XP below / at / above
    low, high = low, high < low, extremes (INT_MIN / INT_MAX neighbours; see 6.2).
11. 4.14: every mode 0..40 and 0xffffffff with flag 0 / 1, against the old rule; mask read from a fake image holding
    0x109; also check that the real `eboot.elf` holds 09 01 00 00 at file offset 0x6802b0.
12. 4.15, 4.16: all 16 combinations of the four bytes (values 0, 1, 0x80).
13. 4.17: position below, equal to, above the capacity; 0, 1, 31, 32, 33 bits; capacity 0.
14. 4.18: room id 0 (ids not read) / non-zero with equal and different ids.
15. 4.19: mode type 0xf / 0xe / −1; dc lookup calls made only in mode 0xf; `u3h_kr_30feec` 0 / 1 with null and
    non-null np, b = 0, 1, −1.
16. 4.20: byte set (word not read), byte 0 with word 3, 2, −3.
17. 4.21: first handle resolves (one call), second only, none; death time comparisons with negative values.

## 6. Kept as they are (oddities, behaviour to keep)

1. 4.1: the helper raises the reader's assert ("ReadBits", line 172) also when its film stream WRITER overruns; the
   game's own writer names "WriteBits" (image 0x14b934d) and line 151 (0x97). Kept (only the assert's text differs;
   the effect, the game's break, is the same). The owner may switch the writer to the writer's values (7.1).
2. 4.13: a 32-bit overflow of the product wraps; `high` − `low` = −1 with a product of INT_MIN would trap in the
   division as in the old code (callers pass thresholds with `high` ≥ `low`).
3. 4.15 / 4.16 / 4.21 / 4.4: no null checks.
4. 4.17: the wrap of U + 31 for positions near 2^32.
5. 4.5: the menu user is returned unmasked (4 or more is possible; callers test or mask).

## 7. Open questions

1. 4.1: whether the film stream writer should raise the game's writer assert (0x14b934d, line 0x97) instead of the
   reader's. Today's behaviour (reader's values for both) is what this spec delivers.
2. 4.14: the signature change (`u3x_shows_match_stats(mode, flag)` to `u3x_match_stats_shown(base, mode, flag)`) so
   the mode list is read from the image. It touches G30's already tested `part_c.cpp` (one call, line 108) at
   integration. If the owner prefers to keep the old signature, the three modes 0, 3, 8 would have to stay as values
   (a three-value list of game constants: against the table rule), so the change is recommended.
3. 3.5: DC symbol 0xea5596b8 (mode-type-0xf score values) is used by value; its name is not known, so it cannot be
   formed by `u3h_crc_sid`. Single id, kept like the other specs' single ids.
4. 3.5: the splitscreen body text 0x7c31f4d0 is a PS3 1.19 text id not present in the PS4 image (the title
   0xc6467f2d is). Single id, kept by value; on PS4 the dialog shows whatever the text database gives for it.
5. 1.4: the three dead functions are deleted rather than forwarded (integrator's choice, no behaviour either way).

## Coordinator decisions (2026-11-15)
1. Open question 1: keep today's behaviour (the reader's assert values also for writer overruns); exactness rule.
2. Open question 2: accepted. u3x_match_stats_shown(base, mode, flag) reads the mode mask from the PS4 image; the integrator updates G30 part_c line 108 and the old caller; G30's test is re-run on the merged file.
3. Open questions 3 and 4: single ids stay as values (precedent "single ids may stay as values").
4. Open question 5: the three dead functions (np_in_game, kr_local, kr_np_at) are deleted by the integrator.
5. Split: part_a = the nine C-side entry points, part_b = the C++ exports rewritten in place, as suggested in the spec.

## Clarifications (tester, differential test against v7.212, 2026-11-15)
1. 4.8 (`u3x_net_deadline_due`): the old code, as every build compiles it (host -O1 / -O2 and the helper's own build,
   `--target=x86_64-unknown-elf -O2 -mno-sse`: compare of the clock with the deadline word, then "greater or equal"),
   gives 1 exactly when clock >= deadline as signed 32-bit numbers. It is not a wrapping difference: its subtraction is
   signed and may not overflow, so the compiler compares. The two rules differ when the values are 2^31 or more
   apart: clock 0x7fffffff with deadline 0x80000000 gives 1 (not 0); clock 0x80000000 with deadline 0x7fffffff gives
   0 (not 1). Scenario 8's wrap case expects the old (signed) answers. Both values are read once, clock first.
2. 4.9 (`u3x_kr_statw`): for index < 0x13 the old code calls `u3x_weapon_for_stat(base + 0x13a6170, null, index)`
   (one call, no game call); for index >= 0x13 the game call 0x11f2bf0(0x9e354807) comes first, then
   `u3x_weapon_for_stat(base + 0x13a6170, list or null, index)`.
3. 3.4: the instruction that reads the mode word starts at image 0x67c2a4 (`mov eax,[rbx+0x680c]`; 0x67c2a6 holds its
   displacement); then `cmp eax,8` (8 at 0x67c2ac), `ja`, `mov ecx,imm32` (opcode at 0x67c2af, imm32 at 0x67c2b0),
   `bt ecx,eax`. Checked in `eboot.elf`: first LOAD segment file 0x4000 -> image 0 (file offset = image + 0x4000);
   file 0x6802b0 holds 09 01 00 00 = 0x109 = modes {0, 3, 8}, exactly the old list. With that value in the image the
   new rule gives the old results for every u32 mode with flags 0 and 1.
4. 6.2 (`u3x_xp_percent`): (low, high, xp) = (INT_MIN, INT_MAX, INT_MIN + 0x20000000) (product INT_MIN, span -1) traps
   with the integer overflow exception in the old code and in the new; the old build's product wraps (the helper's
   disassembly: `imul` then `idiv`), as 4.13 says.
