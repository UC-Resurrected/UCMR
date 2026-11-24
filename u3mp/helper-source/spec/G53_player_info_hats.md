# G53. Player-info fields, hat modifiers, hat-stat icons

Behaviour description for a reimplementation (reimpl2 pipeline, describer's document). It says what the functions
do, not how the old ones were written. Offsets are hexadecimal when prefixed with 0x. "Image offset X" / "image X"
means the address `base + X`, where `base` is the game image base. All multi-byte values are little-endian;
"u8 / u16 / u32 / s32" are unsigned / signed integers of that width, "pointer" is 8 bytes. Names follow
`G22_rank_xp.md`, `G23_boosters.md` (profile objects, script symbols, the string id rule of 3.9 there),
`G04_film_hud.md` (the call block, "make a panel" / "draw a panel") and `G41_out_of_bounds.md` (stub blocks).

**Float work** of this group is small: one division and comparisons against 1.0 (4.9), and additions of small
constants to a rectangle (4.12). Every step is ONE single-precision operation on 32-bit patterns with the `f_*`
tools of CONVENTIONS.h. The old build (v7.212) already did exactly that: the compiled code of this group holds
only single-precision instructions (one divide, three unordered compares, twelve adds) and not one x87
instruction. Results must therefore be equal bit for bit; there is no accepted deviation in this group.

## 1. Purpose and callers

PS3 1.19 differs from the PS4 game in three small places that belong together:

- **Player info (part A).** The "player info" net message (message 0x3a) carries three more fields and one more
  flag bit than PS4's: a tournament rank byte (always sent as 0 here), "hat stats off", the equipped taunt, and
  a profile flag. The helper fills them on send, appends them to the game's own serialization, and keeps the
  received values per net player in a small array of the C part (the PS4 net player record has no room).
  From there the per-player taunt package is asked for, and the hat code below learns "hat stats off".
- **Hat modifiers (part A).** When the game rebuilds a net player's stat modifier list, 1.19 (a) leaves one
  booster out in one game type and (b) appends the stat modifiers of the hat parts the player wears.
- **Hat-stat icons (part B).** On the in-match player card ("killed by"), 1.19 shows up to two icons for the
  hat's first two stats after the booster / kickback icons. Part B also holds the small call bridge
  `u3x_call8` and the stat icon lookup that G55 (hat-stat card lines) uses too.

All functions are reached from the helper's event dispatcher `u3_profile_event` (C, stays) by stub kind, through
one-line C wrappers of `agc_port.inc`:

| Kind | Game site (image) | C wrapper (stays, one line) | Function |
|---|---|---|---|
| 105 | 0x6b846c, the player-info sender right before its send call; argument = the message | `pinfo_send119(base, msg)` | `u3x_pinfo_send` |
| 106 / 107 | entries of the message's read 0x70b3c0 / write 0x709dc0 functions (value stubs) | `pinfo_serial119(base, regs, kind == 107)` | `u3x_pinfo_serial` |
| 108 | 0x709da0, the end of the message's process function (the game has found the player and stored its own fields); argument = the message | `pinfo_process119(base, msg)` | `u3x_pinfo_process` |
| 109 / 110 | match start, where the game asks for a net player's look: the local player (109) and the two team loops (110); argument = the net player (may be null) | `np_taunt_want119(base, np)` | `u3x_np_taunt_want` |
| 112 | 0x78f8b3, the exit of the modifier rebuild 0x78f3b0; argument = the net player | `hat_mods119(base, s, np)` | `u3x_hat_mods` (new) |
| 113 | 0x78f589, inside the rebuild's booster loop (value stub) | `booster_skip119(base, s, regs)` | `u3x_booster_skip` (new) |
| 122 | 0xc38989, the card's icon count (register stub) | `hat_icon_count119(base, s, regs)` | `u3x_hat_icon_count` |
| 123 | 0xc3aa8d, the card after the kickback icon (register stub) | `hat_icon_draw119(base, s, regs)` | `u3x_hat_icon_draw` |

The dispatcher calls kinds 105, 106/107, 108, 113, 122, 123 only with a non-null `base` and a non-null argument,
kinds 109/110 and 112 with a non-null `base` (the argument may be null). For kinds 106/107 and 113 the dispatcher
returns the function's result to the stub (see 3.2); for the others it returns 1.

Other callers: `u3x_stat_icon` and `u3x_call8` are also called by G55 (`u3x_hs_draw`, `u3x_hs_meas`); the dead
one-line C wrappers `call8_119`, `stat_icon119`, `hat_icon119` only forward and stay.

Everything runs on the game's main thread (net message handling, match setup, HUD draw). No locking, no timing
rule, no file or socket.

## 2. Interface

All `extern "C"`, all `U3X_FN`. No float is passed or returned by value (float values travel as 32-bit patterns).
"existing" = exported today under this name; name and signature stay. "new" = a new entry point; the C function
becomes a one-line wrapper that passes its arguments unchanged (the state pointer as `void *`) and returns the
result where there is one.

The group is delivered in two independent parts:

- **Part A (player info and hat modifiers)**: 4.1 to 4.7. No float.
- **Part B (call bridge and hat-stat icons)**: 4.8 to 4.12.

Both parts use the private pieces of 2.3; each part writes its own copy under its own prefix (`g53a_`, `g53b_`).

### 2.1 Functions to deliver

| Part | Function | Signature | State | Parameters, result |
|---|---|---|---|---|
| A | `u3x_pinfo_send` | `void u3x_pinfo_send(uintptr_t base, uint8_t *msg)` | existing | `msg`: the player-info message about to be sent (3.3), never null |
| A | `u3x_pinfo_serial` | `int u3x_pinfo_serial(uintptr_t base, uint64_t *regs, int write)` | existing | `regs`: value-stub block (3.2); `write`: 0 = the read site, 1 = the write site. Result 0 = let the game's function run, 1 = the game's function was replaced (the stub returns `regs[8]` to its caller) |
| A | `u3x_pinfo_process` | `void u3x_pinfo_process(uintptr_t base, const uint8_t *msg)` | existing | `msg`: a received player-info message, never null |
| A | `u3x_np_taunt_want` | `void u3x_np_taunt_want(uintptr_t base, uint8_t *np)` | existing | `np`: a net player record or null |
| A | `u3x_booster_skip` | `int u3x_booster_skip(uintptr_t base, void *s, uint64_t *regs)` | new (C `booster_skip119`) | `s`: the helper's state object; `regs`: value-stub block. Result always 0 |
| A | `u3x_hat_mods` | `void u3x_hat_mods(uintptr_t base, void *s, uint8_t *np)` | new (C `hat_mods119`) | `np`: the net player whose modifier list was just rebuilt, or null |
| A | `u3x_add_hat_mods` | `int u3x_add_hat_mods(uint8_t *np, const uint8_t *rows, int32_t row_count, uint32_t item, uint32_t slot, uint32_t *added)` | existing | see 4.7. Result 0, or -1 when the list is full. After this group only `u3x_hat_mods` calls it; the name and its C declaration stay |
| B | `u3x_call8` | `uint64_t u3x_call8(uintptr_t fn, uint32_t f0, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t s0, uint64_t s1)` | existing | call bridge, 4.8 |
| B | `u3x_stat_icon` | `uint8_t *u3x_stat_icon(uintptr_t base, void *s, uint32_t stat, uint32_t m)` | existing | `stat`: a stat id; `m`: the stat's multiplier as a float pattern. Result: a texture or null |
| B | `u3x_hat_icon` | `uint8_t *u3x_hat_icon(uintptr_t base, void *s, uint8_t *np, int32_t i)` | existing | `np`: a net player or null; `i`: 0 or 1, which stat of the hat. Result: a texture or null |
| B | `u3x_hat_icon_count` | `void u3x_hat_icon_count(uintptr_t base, void *s, const uint64_t *regs)` | existing | `regs`: register-stub block (3.2) |
| B | `u3x_hat_icon_draw` | `void u3x_hat_icon_draw(uintptr_t base, void *s, const uint64_t *regs)` | existing | `regs`: register-stub block |

### 2.2 Functions of other groups, of the C part and of the shared tools that the new code calls

Not to be described or reimplemented here.

| Name | Signature | What it does |
|---|---|---|
| `u3h_ext` | `uint8_t *u3h_ext(void)` | the helper's state block (C part). This group uses its diagnostic words "ec": u32 `ec[k]` is at `u3h_ext() + 0x4450 + 4·k` (CONVENTIONS.h has the same accessors for `ea` and `eb` only) |
| `u3h_np119` | `uint8_t *u3h_np119(void)` | the address of the per-player info array of the C part (3.4); reads nothing |
| `u3h_taunt_want` | `void u3h_taunt_want(uintptr_t base, uint32_t id)` | G54 (`taunt_want119`): asks for the taunt package of taunt `id` to be loaded |
| `u3h_mp_code_is` | `int u3h_mp_code_is(uintptr_t base, uint32_t off, const char *hex12)` | 1 when the 12 bytes at image `off` equal the 24 lower-case hex digits of `hex12`, else 0 (G12 2.2). The new code may compare the bytes itself instead; a test must accept both |
| `u3x_sym_table` | `uint8_t *u3x_sym_table(void *s, uint32_t id)` | data of a script symbol through the state's lookup function (pointer at `s` + 656); null when that pointer, the handle or the data is null (G23 2.2). One lookup call per call |
| `u3x_netgame_cur` | `uint8_t *u3x_netgame_cur(uintptr_t base, void *s)` | the record of the current net game (0xd0 bytes) or null (G40) |
| `u3x_netgame_type` | `int32_t u3x_netgame_type(uintptr_t base, void *s)` | the game type of the current net game (s32 at record + 0xc), 0 when there is none |
| `u3x_net_player_id` | `uint32_t u3x_net_player_id(const uint8_t *np)` | a net player's id from its record (G43 3.2); reads only |
| `u3x_k26_call` | `uint64_t u3x_k26_call(void *game_call_block)` | CONVENTIONS.h: a game call with float registers |

### 2.3 Private pieces (part of the new code, described here)

- **Slot of a net player** (old file-private `player_slot` in C++ and `np_slot119` in C, both in no work group):
  the net player records are an array of 10 records of 0x9c0 bytes that starts at image 0x17cb060 + 0x390. For a
  pointer `np`: d = `np` minus the array start, as a signed byte distance. The slot is d / 0x9c0 when d >= 0, d is
  an exact multiple of 0x9c0 and the quotient is below 10; otherwise -1 ("not one of the ten records").
- **Code check**: "the code check of X passes" = the 12 bytes at image X are the expected ones of 3.6.
- **Per-player info entry** `info[i]`: entry i of the array of 3.4.

## 3. Game facts

### 3.1 Switches and diagnostic words

`ec[0]` is a switch word set at start-up: bit 3 (8) = taunt packages off; bit 5 (32) = the 1.19 player-info
fields off; bit 6 (64) = the hat modifiers off; bit 13 (8192) = the hat-stat icons off.

Words written by this group (all u32, "+1" wraps): ec[37] sends seen; ec[38] reads; ec[39] writes; ec[42]
messages processed; ec[43] last processed (4.3); ec[44] last taunt received; ec[45] taunt asks; ec[46] boosters
skipped; ec[47] hat modifiers added; ec[48] rebuilds that reached the hat block; ec[49] list-full stops; ec[105]
the card's icon count (the stub at 0xc38989 loads it into the game's ecx and tests it, so it must always be
written by 4.11); ec[110] cards counted; ec[111] icons drawn; ec[112] last number of hat icons.

Words read: ec[40] and ec[41]: the image offsets of two trampolines that the patch builder made (the game's own
read function / write function of the message: its replaced first instructions followed by a jump into the
rest); 0 = not there. ec[106..109]: the rectangle x, y, width, height (four float patterns) of the card's icon
slot, stored by a separate small stub at 0xc3a945 from the game's xmm0 before kind 123 runs.

### 3.2 Stub blocks

**Value stub** (kinds 106, 107, 113): `regs` = nine saved registers of 8 bytes in the order r11, r10, r9, r8,
rdi, rsi, rdx, rcx, rax (index 4 = rdi, 5 = rsi, 6 = rdx, 7 = rcx, 8 = rax). When the function returns 0 the
stub restores all nine from the block (so a changed element changes the game's register) and continues with the
game's replaced instructions. When it returns non-zero the stub returns to the caller of the interrupted game
function with rax = `regs[8]`.

**Register stub** (kinds 122, 123): `regs` = fourteen saved registers in the order r15, r14, r13, r12, rbx, r11,
r10, r9, r8, rdi, rsi, rdx, rcx, rax, followed at index 14 by the game's rbp (the frame pointer of the HUD draw
function, "FP" below). Used here: `regs[13]` (rax) and `regs[14]`. Nothing is written back.

### 3.3 The player-info message (message 0x3a)

The helper's patch lets the game allocate 0xc8 bytes for it instead of 0xc0. Fields used:

| Offset | Type | Meaning |
|---|---|---|
| 0x14 | u32 | the net player id of the player the message is about (low 12 bits: member number, with 0x800 added for a second local player; bits 12 and up: the user number, used in 4.1 as the index of the sender's profile object) |
| 0x25 | u8 | the game's flag byte; bits 0..2 are PS4's own value, bit 3 is the 1.19 profile flag |
| 0x69 | u8 | 1.19: tournament rank (unused padding in PS4's layout) |
| 0x6a | u8 | 1.19: hat stats off, 0 / 1 (unused padding in PS4's layout) |
| 0xc0 | u32 | 1.19: the equipped taunt's id (the eight bytes the patch added) |

**Profile objects** (G22 3.4): the pointer at image 0x17dd610 is an array of profile objects of 0x7b00 bytes, one
per local user; null while they do not exist. Fields of a profile object used here: +0x784 u32 the equipped
taunt's id; +0x215c u32 non-zero = hat stats switched off by the player; +0x2164 u32 the 1.19 profile flag.

**Bit stream functions of the game** (first argument: the bit stream object the read / write function received):

| Image | Call | Meaning |
|---|---|---|
| 0x1272b50 | (stream, u32 value, u32 bit count), no result | appends the low `bit count` bits of the value |
| 0x1272c90 | (stream, u32 value), no result | appends one truth bit (value 0 / non-zero) |
| 0x1272c00 | (stream, u32 bit count) → u32 | reads that many bits |

The trampolines (3.1) are called as (message, stream), no result used.

### 3.4 Net players and the per-player info array

Image 0x17cb060 is the net game manager ("NGM"). Net player records: 2.3. Fields of a net player record: +0x208
u8 (PS4's player-info value, bits 0..2 of message +0x25); +0x84c u32 number of entries of the stat modifier list;
+0x850 the list, 16 entries of 16 bytes:

| Entry offset | Type | Meaning |
|---|---|---|
| 0 | u32 | stat id |
| 4 | u32 | the value (float pattern) |
| 8 | u32 | source (for a hat entry: the look slot number 2..6) |
| 0xc | u8 | type (4 = hat) |
| 0xd | u8 | level (0 for a hat entry) |

The **per-player info array** lives in the C part (`np119`, 10 entries of 8 bytes, zero at start; it stays there
and is reached through `u3h_np119`). Entry i belongs to net player slot i:

| Offset | Type | Meaning |
|---|---|---|
| 0 | u32 | taunt id |
| 4 | u8 | tournament rank |
| 5 | u8 | hat stats off |
| 6 | u8 | profile flag (0 / 1) |
| 7 | u8 | 1 once a message was processed for this slot |

Other game data: image 0x17d5c58 + 9 (u8): non-zero = boosters are possible in this session; NGM + 0x6808 (u32):
non-zero = a match exists; net game record + 0x6c (u32): non-zero = the net game switches boosters off.

### 3.5 Game functions of the hat code

| Image | Call | Meaning |
|---|---|---|
| 0x73fbc0 | (NGM address) → u8 | non-zero = boosters (and so hat stats) are on in the running game |
| 0x784b20 | (net player, u32 0) → pointer or null | the player's look record: u8 at +0 and +1 identify the character, u8 at +2 .. +6 are the choices of the five part slots (look slot 2 is the hat) |
| 0x9952a0 | (u64 0, u32 look byte 0, u32 look byte 1, u32 part slot 0..4, u32 the choice of that slot) → pointer or null | the item record of that part; its first u32 is the item id |
| 0x1189430 | (address of image 0x1b31128, pointer to a NUL-terminated texture name) → pointer or null | the game's texture-by-name lookup (the "native 20" of G22 3.2, called directly here): the texture, or null while it is not loaded (a miss queues the load) |
| 0x640ac0 | "make a panel" (G04): see 4.12 | lays out one panel item |
| 0x63d6f0 | "draw a panel" (G04): see 4.12 | draws it |

All look bytes are passed zero-extended. 0x640ac0 and 0x63d6f0 take a float in xmm0 and two stack arguments:
they are called through the bridge of 4.8.

**Script symbols** (through `u3x_sym_table`):

- 0xb8a3581b, the hat modifier table: +0 s32 row count, +8 pointer to the rows. Row, 16 bytes: +0 u32 item id,
  +4 s32 number of pairs, +8 pointer to the pairs. Pair, 8 bytes: +0 u32 stat id, +4 u32 value / multiplier
  (float pattern). The table is added to the PS4 data by the asset converter; while it is missing every hat
  function does nothing.
- 0x53ed596a, the stat icon table: +0 s32 entry count, +8 pointer to the entries. Entry, 0x28 bytes: +0 u32 stat
  id, +8 pointer to the name of the "better" icon texture, +0x18 pointer to the name of the "worse" icon texture
  (the other bytes are not used).

Single ids: 0xdff8181f = the booster that 1.19 leaves out (the u32 at +4 of a booster record); 0x13 = the game
type in which it is left out.

### 3.6 Code checks

A game function of this list is called only when the first 12 bytes of its code are the expected ones (kept in
`U3X_RO` data; as text for `u3h_mp_code_is` in lower-case hex without spaces):

- 0x784b20: 55 48 89 E5 41 56 53 48 89 FB 40 0F
- 0x9952a0: 55 48 89 E5 41 57 41 56 41 55 41 54
- 0x1272b50: 55 48 89 E5 53 50 44 8B 5F 08 45 89
- 0x1272c00: 55 48 89 E5 41 56 53 8B 57 08 89 D3

0x73fbc0, 0x1272c90, 0x1189430, 0x640ac0, 0x63d6f0 and the trampolines are called without a check.

### 3.7 The stat classes: a table that is NOT in this document (open question 1)

Which of a stat's two icons is the "better" one depends on the stat's class. PS3 1.19 decides the class by
comparing the stat id with three fixed lists of stat ids:

| Class | Number of ids | Meaning |
|---|---|---|
| INV | 3 | the multiplier is shown as its inverse (1 ÷ multiplier) before it is judged |
| UP | 5 | the stat's change is always shown as "better" |
| LOW | 4 | a multiplier below 1 is the better one |
| (any other stat) | - | a multiplier of 1 or more is the better one |

The twelve ids are literal values of the PS3 1.19 code. The running PS4 game does not hold them as a list or a
classification: checked over the whole image file (`eboot.elf`), eleven of the twelve 32-bit patterns occur only
as single instruction immediates of unrelated code (two of the INV ids side by side at image 0x68a295 / 0x68a29a
and at 0xa6015b / 0xa60160, as the two alternatives of one stat query), one occurs nowhere. They are string ids
(G23 3.9) of stat names that are not known, so the name rule cannot generate them either, and neither table of
3.5 carries a class field. This document therefore does not give them. The implementer writes 4.9 against a
private function "class of stat id → INV / UP / LOW / other" and reports the question; see section 7.

### 3.8 Constants

| Pattern | Value | Use |
|---|---|---|
| 0x3f800000 | 1.0 | class comparisons (4.9); the item's scale word (4.12) |
| 0x40000000 | 2.0 | inset of an icon in its slot (4.12) |
| 0x40800000 | 4.0 | gap between two slots (4.12) |
| 0xc0800000 | -4.0 | added to the slot's width and height (4.12) |

## 4. Behaviour

"Stop" = return without any further effect. Game calls, `u3h_` / `u3x_` calls and memory writes happen exactly in
the order written; nothing else is called.

### 4.1 `u3x_pinfo_send(base, msg)` (kind 105)

1. ec[37] + 1 (always).
2. Stop when ec[0] bit 5 is set.
3. user = the u32 at `msg` + 0x14, shifted right by 12. Stop when the profile array pointer (image 0x17dd610) is
   null or user > 3. P = that pointer + user · 0x7b00.
4. When the u32 at P + 0x2164 is non-zero: bit 3 of the byte at `msg` + 0x25 is set (it is never cleared here).
5. `msg`[0x69] = 0 (there are no tournaments).
6. `msg`[0x6a] = 1 when the u32 at P + 0x215c is non-zero, else 0.
7. The u32 at `msg` + 0xc0 = the u32 at P + 0x784.

### 4.2 `u3x_pinfo_serial(base, regs, write)` (kinds 106 / 107)

M = `regs[4]` (the message), B = `regs[5]` (the bit stream). T = ec[40] when `write` is 0, ec[41] when it is 1.

1. Return 0 when ec[0] bit 5 is set or T is 0.
2. Return 0 unless the code checks of 0x1272b50 and of 0x1272c00 both pass (both for either direction).
3. Call the trampoline at image T with (M, B): the game's own body of the message.
4. `write` non-zero: call 0x1272b50(B, the byte at M + 0x69, 8); call 0x1272c90(B, the byte at M + 0x6a); call
   0x1272b50(B, the u32 at M + 0xc0, 32); ec[39] + 1.
   `write` zero: the byte at M + 0x69 = the low 8 bits of 0x1272c00(B, 8); the byte at M + 0x6a = the low 8 bits
   of 0x1272c00(B, 1); the u32 at M + 0xc0 = 0x1272c00(B, 32); ec[38] + 1. (Each read call is made, then its
   store, in this order.)
5. `regs[8]` = 0. Return 1.

Nothing is written to `regs`, the message or the counters on the "return 0" paths.

### 4.3 `u3x_pinfo_process(base, msg)` (kind 108)

1. Stop when ec[0] bit 5 is set.
2. id = the u32 at `msg` + 0x14. A = `u3h_np119()` (called once, before the search).
3. Go through the ten net player records in slot order; for each, `u3x_net_player_id(record)` is compared with
   id. For the FIRST record that matches (slot i), with F = the byte at `msg` + 0x25:
   - record + 0x208 (u8) = F AND 7;
   - info[i]: profile flag = bit 3 of F (0 / 1); rank = `msg`[0x69]; hat stats off = `msg`[0x6a] (the byte as it
     is); taunt = the u32 at `msg` + 0xc0; the "processed" byte = 1;
   - ec[42] + 1; ec[43] = i OR (`msg`[0x6a] shifted left by 8) OR (bit 3 of F shifted left by 9); ec[44] = the
     taunt id;
   - stop (later records are not looked at).
4. No record matches: nothing is written.

### 4.4 `u3x_np_taunt_want(base, np)` (kinds 109 / 110)

1. ec[45] + 1 (always).
2. Stop when ec[0] has bit 3 or bit 5 set, or `np` is null.
3. i = the slot of `np` (2.3). Stop when it is -1.
4. `u3h_taunt_want(base, taunt of info[i])` (`u3h_np119()` is called once, here). The call is made whatever the
   taunt id and the "processed" byte are (also with id 0).

### 4.5 `u3x_booster_skip(base, s, regs)` (kind 113)

The game is about to read the number of level entries of a booster record: a 32-bit load from address
rdx + rcx + 4; a count of 0 makes the game pass over the booster. rax (`regs[8]`) is the booster record.

1. Return 0 when ec[0] bit 6 is set.
2. When the record is not null AND the u32 at record + 4 is 0xdff8181f AND `u3x_netgame_type(base, s)` is 0x13
   (tested in this order; the net game call is made only when the first two hold): `regs[6]` = Z - 4 and
   `regs[7]` = 0, where Z is the address of a 4-byte word of the helper that holds 0 for as long as the helper is
   loaded and is never written (so the game's replayed load reads 0); ec[46] + 1.
3. Return 0 (always).

### 4.6 `u3x_hat_mods(base, s, np)` (kind 112)

Appends the modifiers of the worn hat parts to the list the game has just rebuilt. In this order:

1. Stop when ec[0] bit 6 is set. Stop when `np` is null.
2. i = the slot of `np` (may be -1).
3. Stop when the u8 at image 0x17d5c58 + 9 is 0, or the u32 at NGM + 0x6808 is 0.
4. N = `u3x_netgame_cur(base, s)`. Stop when N is not null and the u32 at N + 0x6c is non-zero.
5. Stop unless the code check of 0x784b20 passes and then that of 0x9952a0 (the second is only looked at when the
   first passed).
6. Stop when game function 0x73fbc0(NGM address) returns a zero byte.
7. T = `u3x_sym_table(s, 0xb8a3581b)`. Stop when T is null, or when i >= 0 and "hat stats off" of info[i] is
   non-zero.
8. ec[48] + 1.
9. L = game function 0x784b20(`np`, 0). Stop when L is null.
10. count = the s32 at T, rows = the pointer at T + 8. Stop when rows is null.
11. For look slot k = 2, 3, 4, 5, 6 in this order: R = game function 0x9952a0(0, L[0], L[1], k - 2, L[k]). When R
    is null, go on with the next k. Otherwise call `u3x_add_hat_mods(np, rows, count, the u32 at R, k, the address
    of ec[47])`; when it returns a negative value: ec[49] + 1 and stop (the remaining slots are not looked at).

### 4.7 `u3x_add_hat_mods(np, rows, row_count, item, slot, added)`

For every row r = 0 .. `row_count` - 1 (none when `row_count` <= 0) whose item id equals `item`, in row order (ALL
matching rows, not only the first): with n = the row's pair count and Q = its pair pointer, for pair j = 0 ..
n - 1 in order (none when n <= 0 or Q is null):

- when the list count (u32 at `np` + 0x84c) is 16 or more: return -1 at once (what was added before stays);
- otherwise the entry at `np` + 0x850 + 16 · count gets: +0 = the pair's stat id, +4 = the pair's value word,
  +8 = `slot`, +0xc = 4, +0xd = 0 (its bytes +0xe and +0xf are left as they are); the list count grows by 1; the
  u32 at `added` grows by 1.

Return 0 when all rows are done. The "list full" test is made only when there is a pair to add: a full list and no
matching pair gives 0.

### 4.8 `u3x_call8(fn, f0, a0 .. a5, s0, s1)`: the call bridge

Calls the function at address `fn` with the System V AMD64 convention: integer arguments 1 to 6 = `a0` .. `a5`
(full 64 bits), the low 32 bits of xmm0 = `f0` (a float pattern; bits 32 and up of the register are zero), and
two stack arguments: `s0` at [rsp] and `s1` at [rsp + 8] at the moment of the call instruction, with rsp a
multiple of 16 there. The result is the callee's rax, unchanged. The contents of the other vector registers and
of al at the call are not defined (the callees used are not variadic and take one float). The bridge is our own
platform code, not game logic; it may be delivered as one call of `u3x_k26_call` with a block that is all zero
except `fn`, `i[0..5]` = `a0..a5`, the low 4 bytes of `x[0]` = `f0`, `s[0]` = `s0`, `s[1]` = `s1` (a test
compares only what this paragraph defines).

### 4.9 `u3x_stat_icon(base, s, stat, m)`

1. T = `u3x_sym_table(s, 0x53ed596a)`. Null → return null.
2. E = the LAST entry of the table (count = s32 at T, entries at the pointer at T + 8) whose stat id equals
   `stat`. No entries pointer, count <= 0 or no such entry → return null. (All entries are looked at.)
3. "Better" or "worse", by the class of `stat` (3.7); "m < 1" is the single-precision ordered comparison
   `f_less(m, 0x3f800000)`, which is false when m is a NaN:
   - INV: q = `f_div(0x3f800000, m)` (1.0 ÷ m, one rounding); better unless q < 1.
   - UP: better.
   - LOW: better when m < 1, else worse.
   - any other stat: better unless m < 1.
   (So a NaN multiplier gives "better" for INV and other stats and "worse" for LOW; m = +0 gives q = +infinity,
   "better"; m = -0 gives -infinity, "worse".)
4. name = the pointer at E + 8 for "better", at E + 0x18 for "worse". Null → return null.
5. Return game function 0x1189430(address of image 0x1b31128, name).

The class is worked out only after step 2 succeeded; no game call is made before step 5.

### 4.10 `u3x_hat_icon(base, s, np, i)`

The texture of the icon for stat number `i` of the hat that net player `np` wears, or null. In this order:

1. `np` null → null. Code check of 0x784b20, then of 0x9952a0 (only when the first passed); a failed check → null.
2. L = game function 0x784b20(`np`, 0) (called before the table is looked up).
3. T = `u3x_sym_table(s, 0xb8a3581b)`.
4. k = the slot of `np`. When k >= 0: `u3h_np119()` and "hat stats off" of info[k]; non-zero → null. (No
   `u3h_np119` call when k is -1.) Then: T null → null; L null → null.
5. R = game function 0x9952a0(0, L[0], L[1], 0, L[2]) (the hat: part slot 0). Null → null.
6. count = the s32 at T, rows = the pointer at T + 8; rows null → null.
7. The FIRST row (in row order) whose item id equals the u32 at R decides; no such row → null. For that row:
   `i` not below its pair count (signed comparison) → null; its pair pointer null → null; otherwise return
   `u3x_stat_icon(base, s, stat id of pair i, value word of pair i)`.

### 4.11 `u3x_hat_icon_count(base, s, regs)` (kind 122)

FP = `regs[14]`. P = the pointer at FP - 0x12b0 (the net player the card shows; may be null). n = the low 32 bits
of `regs[13]` (the game's count of booster icons) + 1 when the 8-byte value at FP - 0x11e0 (the kickback icon) is
non-zero.

When ec[0] bit 13 is clear: h = (1 when `u3x_hat_icon(base, s, P, 0)` is not null) + (1 when
`u3x_hat_icon(base, s, P, 1)` is not null), both calls always made, stat 0 first; n = n + h; ec[110] + 1;
ec[112] = h.

In every case: ec[105] = n (32-bit wrap).

### 4.12 `u3x_hat_icon_draw(base, s, regs)` (kind 123)

1. Stop when ec[0] bit 13 is set.
2. FP = `regs[14]`. P = the pointer at FP - 0x12b0; C = the pointer at FP - 0x11f8 (the draw context). x, y, w, h =
   ec[106], ec[107], ec[108], ec[109] (float patterns; read once, here).
3. Step = "x becomes `f_add(x, f_add(w, 0x40800000))`" (first w + 4.0, rounded; then x + that, rounded).
   When the 8-byte value at FP - 0x11e0 is non-zero, one Step (the kickback icon took the slot).
4. For i = 0, then 1: V = `u3x_hat_icon(base, s, P, i)`. Null → nothing for this i (no Step). Otherwise:
   - Item (48 bytes, 16-byte aligned, all zero except): +0 u32 = 2 (a texture item); +8 pointer = V; +0x1c u32 =
     0xffffffff; +0x24 u32 = 0xffffffff; +0x28 u32 = 0x3f800000.
   - Rect (four float patterns, 16-byte aligned): `f_add(x, 0x40000000)`, `f_add(y, 0x40000000)`,
     `f_add(w, 0xc0800000)`, `f_add(h, 0xc0800000)` (x + 2, y + 2, w + (-4), h + (-4)).
   - Size (16-byte aligned): two float patterns = Rect's third and fourth. Zero: 16 zero bytes, 16-byte aligned.
   - Make: `u3x_call8(base + 0x640ac0, the u32 at FP - 0x450, FP - 0x420, address of Item, 0, address of Size,
     address of Zero, 0xffffffff, 0, 0)`. (FP - 0x420 is the game's own panel result block of the card; the float
     is the first word of the card's style block at FP - 0x450.)
   - Draw: `u3x_call8(base + 0x63d6f0, 0, base + 0x17bd090, C, FP - 0x450, FP - 0x420, address of Rect, 0, 0,
     0xffffffff)` (float 0; stack arguments 0 and 0xffffffff).
   - ec[111] + 1; one Step.

Results of the two game calls are not used. The four blocks must stay valid and unchanged by the helper between
Make and Draw of the same icon.

## 5. Test scenarios

All of the group can be tested with a fake image (a writable buffer as `base`), a fake state `s` whose lookup
function (at +656) serves the two tables, a fake helper state for `u3h_ext`, a fake info array for `u3h_np119`,
and call-recording stubs for the game functions, the trampolines, `u3h_taunt_want`, `u3x_netgame_cur` /
`u3x_netgame_type` and `u3x_net_player_id`; compare memory, results and the order and arguments of all calls
(for 4.8 / 4.12 also xmm0's low 32 bits and the two stack words as the callee sees them, and rsp modulo 16).
Nothing here needs the live game except the final look of the icons on the card.

Part A:
1. Send: switch on / off (ec[37] counts in both); null profile pointer; user 0, 3, 4 (id shifted); flag word 0 /
   non-zero with bit 3 already clear / already set and other bits of +0x25 kept; hat-off word 0, 1, 0x100; taunt
   id copied; +0x69 forced to 0 from a non-zero byte.
2. Serial: switch set; trampoline word 0 for the asked direction while the other is set; each of the two code
   checks failing (for both directions); write (three calls with the values and bit counts, order: trampoline
   first); read (returned values with high bits set: only the low 8 bits stored for the two bytes, all 32 for the
   taunt); `regs[8]` cleared and result 1; counters.
3. Process: switch set; no player with the id; the id in slot 0, 9; the same id in two slots (only the first);
   F with bits above bit 3 set (record byte = F AND 7, flag = bit 3); `msg`[0x6a] = 0, 1, 2, 0xff (stored as it
   is; ec[43] as the formula gives); other info entries and other record bytes untouched.
4. Taunt ask: ec[0] bit 3, bit 5; null `np`; `np` before the array, misaligned by 1 byte, slot 10, slot 0 and 9;
   taunt 0 and "processed" 0 still ask.
5. Booster skip: switch; null record; other id; right id with game type 0x13 / other / no net game (0); the net
   game call only in the last three; the word behind `regs[6]` + `regs[7]` + 4 reads 0; other registers kept;
   result 0 always.
6. Hat modifiers: each stop of 4.6 on its own, with the calls made before it (and none after); hat stats off
   with slot >= 0 and with a foreign `np` (slot -1: goes on); item null for some slots; table with no, one and
   several matching rows; a row with null pairs or a count <= 0; list already holding 0, 14, 15, 16 entries (stop
   in the middle of a row; ec[49]; later slots not asked); entry bytes +0xe / +0xf kept; ec[47], ec[48].
7. `u3x_add_hat_mods` alone: `row_count` 0 and negative; full list without a matching pair (0); random tables.

Part B:
8. Bridge: random arguments; a callee that records rdi .. r9, xmm0 and the stack words and returns a 64-bit
   value; rsp alignment.
9. Stat icon: no table; empty table; null entries; stat absent; stat twice (the last one counts); for one stat of
   each class and one of no class: m = 0.5, 1.0, the patterns next below and above 1.0, 2.0, +0, -0, negative,
   infinities, NaN; a null name on the chosen side with a non-null one on the other; the name and the dictionary
   address handed to the game.
10. Hat icon: null `np`; each code check failing; look null with / without table (the look call is still made);
    hat stats off; slot -1; item null; rows null; item in no row; in two rows (the first decides, also when it is
    too short); i = 0, 1 with pair counts 0, 1, 2; null pairs.
11. Count: switch on / off; kickback value zero / non-zero; `regs[13]` with high bits set; 0, 1, 2 icons (only
    the second present counts 1); null player; ec[105] written in every case.
12. Draw: switch; kickback zero / non-zero; none, first only, second only, both icons (positions: a missing first
    icon leaves no gap); rectangles with fractional values where w + 4 rounds; all blocks' contents and
    alignment at both calls; ec[111].

## 6. Things in the old code that look like bugs or dead code (behaviour to keep)

1. The tournament rank is sent as 0, serialized, received and stored, and nothing reads it. The profile flag and
   the "processed" byte are stored and never read by this group.
2. 4.1 only ever sets bit 3 of the flag byte; a message that arrives with the bit set and a zero profile flag
   keeps it.
3. 4.3 stores "hat stats off" as the raw byte and puts it unmasked into ec[43] (a value above 1 spills into the
   higher bits). Every non-zero value counts as "off".
4. 4.2 checks the code of the writer and the reader for both directions, but not the code of the truth-bit
   writer 0x1272c90.
5. 4.4 asks for the taunt of a player whose info was never received (taunt 0), once per
   call of a site.
6. 4.5 always returns 0; its effect is only the two changed registers.
7. 4.6 looks up the table before the look; 4.10 fetches the look (a game call) before the table. 4.6 uses every
   matching row, 4.10 only the first.
8. 4.6 stops for the whole player when the list is full in the middle of a part (PS3 1.19 has no such limit and
   would write past the list; the limit and ec[49] are the helper's own).
9. 4.10 accepts a negative `i` (it passes the count test and reads before the pairs); no caller passes one.
10. The card looks every icon up twice per frame (count and draw), and the count site can see a texture that is
    not loaded yet (null) which the draw site then gets: the count and the drawn icons may differ for a frame.
11. 4.12 reads the slot rectangle from ec[106..109], which another stub fills only when the game reaches
    0xc3a945 in the same card; otherwise the values of the last card are used.
12. 4.12's Size block is two words in a 16-byte aligned place; the eight bytes after it are whatever is there.
13. `u3x_call8` leaves al and the other vector registers undefined at the call.

## 7. Open questions and notes

1. **The stat class table (3.7): 3 + 5 + 4 stat ids, not given here.** They are PS3-only literal values: not held
   as a table by the PS4 game, their names are unknown (so not generated by the string id rule), and no field of
   the two script tables carries the class. To settle before the function can be finished, one of: (a) find the
   twelve stat names, then generate the ids with the string id rule (`u3h_crc_sid`, G23 3.9) from names kept in
   `U3X_RO`; (b) have the asset converter add the class to the stat icon table (it builds symbol 0x53ed596a from
   the PS3 data; its entries have unused bytes) and read it at run time. Until then the implementer delivers 4.9
   with the class lookup as one private function and says so.
2. The booster id 0xdff8181f and the two symbol ids are written as single ids, like the ids of the other specs.
3. Part B's `u3x_stat_icon` and `u3x_call8` are shared with G55, which calls them by the names and signatures of
   2.1 (item type 1 / 2 panels of its own).
4. Private pieces of 2.3 (`player_slot`, C `np_slot119`) are in no work group; they are described here. The old
   `player_slot` is also used by the spotter code of another group, so the old function is not removed by this
   group.

**What stays as it is**: the dispatcher `u3_profile_event` and its kind numbers; the stubs and trampolines of the
patch builder (sites of section 1, the 0xc0 → 0xc8 message size, the rectangle stub); the one-line C wrappers;
the info array `np119` and its export `u3h_np119` in the C part (its layout is 3.4; the C
part asserts its entry size); `u3h_taunt_want` (G54); every exported name and signature of 2.1. The C word
`zero119` is no longer needed once `booster_skip119` is a wrapper (the new code brings its own zero word).

## Clarifications (tester, 2026-11-14)

Settled by the differential test `reimpl2\G53\test_g53.cpp` (old v7.212 against part_a / part_b, -O1 and -O2, all
cases equal). Facts only.

1. **Stat classes (3.7, open question 1).** The new 4.9 asks a new C-part export `int u3h_stat_class(uint32_t stat)`
   (1 = INV, 2 = UP, 3 = LOW, 0 = any other stat). With an export that answers exactly as the old code classifies,
   every result of 4.9 to 4.12 is equal to the old one (all twelve listed ids, unlisted ids, every multiplier pattern
   of scenario 9 including both zeros, infinities and NaNs). The old classification was probed from the old code at
   run time: three, five and four ids in the three classes, and none of 20000 random ids belongs to a class. The
   export does not exist yet in the C part; how it gets the twelve ids is still open (7.1 (a) or (b)).
2. **`u3h_np119` in 4.6.** The new 4.6 reaches the info array through `u3h_np119()` (one call, only when the table
   was found and the slot is not -1); the old C function read its own array directly. `u3h_np119` reads nothing, so
   this call is free; nothing else in 4.6 differs.
3. **Code checks (3.6).** The new code compares the twelve bytes itself in both parts; the result is the same as
   with `u3h_mp_code_is` for a change of any one of the twelve bytes. 0x1272c90 is called without a check (6.4).
4. **Zero word (4.5).** The new code's zero word is a 4-byte read-only constant of the helper; after a skip,
   `regs[6] + regs[7] + 4` addresses it and it reads 0, `regs[7]` = 0, ec[46] + 1; every other register is unchanged.
5. **Call bridge (4.8).** Delivered over `u3x_k26_call` (block all zero except `fn`, `i[0..5]`, `x[0]` = f0, `s[0..1]`,
   `al` = 0). The callee sees the same six integer registers, the low 32 bits of xmm0 = f0 with bits 32..127 zero,
   s0 at [rsp] and s1 at [rsp + 8] with rsp a multiple of 16 at the call, and the result is its rax unchanged.
6. **Float (section 3 note).** Built with the helper's flags (-O2, -mno-sse, x86_64 ELF), the two new parts hold
   exactly twelve single-precision adds, one divide and three unordered compares (all inside the `f_*` tools) and
   no x87 instruction; the results equal the old ones bit for bit.
7. **Section 6.** Every item that can be seen off-line was observed as described: rank / flag / "processed" stored
   (6.1), bit 3 only set (6.2), the raw hat-off byte in info and in ec[43] (6.3), the unchecked truth-bit writer
   (6.4), the ask with taunt 0 for a never-received slot (6.5), result 0 always (6.6), table-before-look in 4.6 and
   look-before-table in 4.10, all rows in 4.6 and the first row in 4.10 (6.7), the stop in the middle of a part at 16
   entries with bytes +0xe / +0xf kept (6.8), `i` = -1 reading before the pairs (6.9), the slot rectangle read once
   from ec[106..109] (6.11), only the two defined words of the Size block (6.12). 6.10 and the look of the icons
   need the live game.
