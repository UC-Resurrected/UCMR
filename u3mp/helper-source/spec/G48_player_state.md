# G48. Player state message: weapons, fill, send loop, spotted flags

Behaviour description for a reimplementation (reimpl2 pipeline, describer's document). It says what the functions
do, not how the old ones were written. All offsets are hexadecimal when prefixed with 0x. "Image offset X" means the
address `base + X`, where `base` is the game image base. All multi-byte values are little-endian; "u8 / u16 / u32 /
s32 / u64" are integers of that width, "pointer" is 8 bytes. Names follow `G40_respawn.md` (net player record `np`,
state object `s`, "manager"), `G11_game_sockets_time.md` (net clock), `G12_packets_monitor.md` (net player id) and
`G44_kill_rewards.md` (float-result call).

**Float.** All float work here is single precision on 32-bit patterns with the `f_*` tools of CONVENTIONS.h
(`f_add`, `f_sub`, `f_mul`, `f_div`, `f_root`, `f_less`, `f_from_int`), one rounding per operation, in the order
given. The old code already worked this way (no x87 arithmetic in this group), so results must match bit for bit.
"a < b" on floats always means `f_less(a, b)`: false when either side is NaN.

"Method M of object O" = the function pointer stored at byte offset M of O's virtual table (the table pointer is the
u64 at O + 0), called with O as the first argument (System V AMD64, integer arguments only). Results are machine
words; "as u8 / u16 / s32 / u32" says which low part is used.

## 1. Purpose and callers

The PS4 game (built from PS3 1.10) never builds the "player state" network message, so remote players would not
move, and it keeps only one "spotted" flag per player. The helper restores, by PS3 1.19's rules:

- the per-player, per-screen **spotted flags** (Spotter booster): part A;
- the **send loop** that decides, per remote player, when the local player's state is due, and the building,
  sending and release of the message: part B;
- the **fill** of the message (flags, ids, time, aim, pose, animation bit stream, weapons, "who targets whom"):
  part C.

Three independent parts (they reach each other only through the exports named in section 2):

| Part | Content | Exported names |
|---|---|---|
| **A** | spotted flags | `u3x_spotter`, `u3x_spot_clear`, `u3x_spot_flag`, `u3x_spot_read` |
| **B** | distance, message build / send / release, send loop | `u3x_distance`, `u3x_send_player_state`, `u3x_replication` |
| **C** | message fill, weapons, weapon numbering, effect test, character to net player | `u3x_player_fill`, `u3x_weapons_fill` (new), `u3x_weapon_item` (new), `u3x_weapon_fixed_id` (new), `u3x_weapon_mode_119`, `u3x_has_eff`, `u3x_char_np` |

Callers (all stay as they are):

| Function | Called from | When |
|---|---|---|
| `u3x_spotter` | C `spotter119` (hook kind 114, game code at 0x78ecc7: the game's spotter loop, replaced; the game continues at 0x78ee54) | each time the game evaluates a living player that may have the Spotter booster |
| `u3x_spot_clear` | C `spot_clear119` (hook kind 115: the net task at 0x7262a4 with a null argument, the player reset at 0x782ed4 with the player) | the game clears its own flag |
| `u3x_spot_flag` | C `spot_flag119` (behind the export `u3h_spot_flag`, used by G33's spotted icon and by a tracker test of another group) and `u3x_spot_read` | drawing |
| `u3x_spot_read` | C `spot_read119` (hook kind 116: the game's readers of the flag) | drawing |
| `u3x_replication` | `u3x_respawn_tick` (G40, step 7 "State sends"), C `repl_loop119` (no caller of its own) | once per frame per local net player |
| `u3x_send_player_state` | C `player_send119` (no caller today) | - |
| `u3x_distance` | C `dist119` (no caller today) | - |
| `u3x_player_fill` | C `player_fill119`, behind the export `u3h_player_fill` (called by part B) | for each message built |
| `u3x_weapons_fill` | C `weapons119`, behind the export `u3h_weapons_fill` (called by part C's fill) | for each message built |
| `u3x_weapon_item` | C `witem119` (called only by `weapons119`) | - |
| `u3x_has_eff`, `u3x_char_np` | C `has_eff119`, `char_np119` and C++ code of the animation groups | any time |

Everything runs on the game's main thread; no locking.

## 2. Interface

### 2.1 Part A

| Name | Signature | Meaning |
|---|---|---|
| `u3x_spotter` | `void u3x_spotter(uintptr_t base, void *s, uint8_t *spotter)` | sets the spotted flags of the enemies near net player record `spotter` (4.1). `spotter` is never null (the hook passes a live record) |
| `u3x_spot_clear` | `void u3x_spot_clear(uintptr_t base, uint8_t *np)` | clears second-screen flags (4.2) |
| `u3x_spot_flag` | `uint8_t u3x_spot_flag(uintptr_t base, uint8_t *np)` | the flag of `np` for the screen being drawn (4.3); `np` not null |
| `u3x_spot_read` | `void u3x_spot_read(uintptr_t base, uint8_t *np)` | stores that flag for the hook stub (4.3); `np` may be null |

### 2.2 Part B

| Name | Signature | Meaning |
|---|---|---|
| `u3x_distance` | `uint32_t u3x_distance(const uint8_t *a, const uint8_t *b)` | distance between two positions (three floats at +0, +4, +8 of each), as a float pattern (4.4) |
| `u3x_send_player_state` | `void u3x_send_player_state(uintptr_t base, void *s, uint8_t *player, uint32_t mask)` | nothing when `player` is null; else builds, fills, sends and releases one message (4.5) |
| `u3x_replication` | `void u3x_replication(uintptr_t base, void *s, uint8_t *np, uint8_t *player)` | the send loop of local net player `np` with character `player` (4.6); `np` not null, `player` may be null |

### 2.3 Part C

| Name | Signature | Meaning |
|---|---|---|
| `u3x_player_fill` | `uint32_t u3x_player_fill(uintptr_t base, void *s, uint8_t *msg, uint8_t *pl)` | fills message `msg` for player character `pl` (4.7); result = the bit count stored in the message |
| `u3x_weapons_fill` | `void u3x_weapons_fill(uintptr_t base, uint8_t *msg, uint8_t *pl)` | NEW entry point for C `weapons119` (4.8). The C function becomes `u3x_weapons_fill(base, msg, pl);` |
| `u3x_weapon_item` | `uint32_t u3x_weapon_item(uintptr_t base, uint32_t id)` | NEW entry point for C `witem119` (4.9): the 1.19 wire number of a weapon id. The C function becomes `return u3x_weapon_item(base, id);` |
| `u3x_weapon_fixed_id` | `uint32_t u3x_weapon_fixed_id(uintptr_t base, uint32_t index)` | NEW: the weapon id of fixed number `index` (0..19) by 3.5; 0 for a larger index. For the integrator (section 7, note 2) |
| `u3x_weapon_mode_119` | `uint32_t u3x_weapon_mode_119(uint32_t mode)` | existing name: 1 gives 1, 5 gives 3, 0 gives 0, every other value gives 4 |
| `u3x_has_eff` | `int u3x_has_eff(uint8_t *c, uint32_t sid)` | 1 when character `c` currently has the effect record with id `sid` (4.10) |
| `u3x_char_np` | `uint8_t *u3x_char_np(uintptr_t base, uint8_t *c)` | the net player record behind character `c`, or null (4.11) |

To be REMOVED: `u3x_weapon_number_119(const uint32_t *fixed20, const uint8_t *extra, uint32_t weapon)` (its only
caller is `witem119`; `u3x_weapon_item` does its whole job) and the C function `call_f32_119` (it has no caller; a
float-result call is `u3x_float_call`, below). The C table `wtab119` goes away (3.5; section 7, note 2).

### 2.4 Functions of other groups and of the C part that these functions call

| Name | Signature | What it does | Used by |
|---|---|---|---|
| `u3h_ext` | `uint8_t *u3h_ext(void)` | CONVENTIONS.h. The diagnostic / switch words "ec[n]" are the u32 array at `u3h_ext()` + 0x4450 | A, B, C |
| float-result call | `uint32_t u3x_float_call(uintptr_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)` | calls game function `fn` with four integer arguments and returns the 32-bit pattern of its single-precision result (the animation lookups group, G51; same name as G44 2.4 proposes) | A, C |
| `u3h_spot_flags` | `uint8_t *u3h_spot_flags(void)` | C export: the 10 second-screen flag bytes ("F1[0..9]", one per net player slot, zero at start) | A |
| `u3h_netgame` | `uint8_t *u3h_netgame(uintptr_t base, void *s)` | C export over G40's `u3x_netgame_cur`: the current net game record or null; s32 at +0xc = game type | A |
| `u3h_netgame_type` | `int32_t u3h_netgame_type(uintptr_t base, void *s)` | G40 section 2: the game type, 0 without a net game | C |
| `u3h_player_fill` | `uint32_t u3h_player_fill(uintptr_t base, void *s, uint8_t *msg, uint8_t *player)` | C export forwarding to `u3x_player_fill` (part C) | B |
| `u3h_weapons_fill` | `void u3h_weapons_fill(uintptr_t base, uint8_t *msg, uint8_t *pl)` | C export forwarding to `weapons119`, that is `u3x_weapons_fill` | C |
| `u3h_np_code` | `uint32_t u3h_np_code(uint8_t *np)` | G34 section 2: the net player id of a record: low = (u32 at +0x1e4) AND 0xfff; + 0x800 when the u8 at +0x1ec is non-zero and low is not 0; + (s32 at +0x1e8) x 0x1000 | B, C |
| `u3h_np_by_id` | `uint8_t *u3h_np_by_id(uintptr_t base, uint32_t id)` | C export over G34's `u3x_np_by_id`: the record (of 10) whose id is `id`, or null | C |
| `u3h_net_now` | `int32_t u3h_net_now(uintptr_t base)` | the net clock: the s32 at image 0x17b4ca4 (ticks of 1/300 s) | C |
| `u3h_repl_error` | `uint32_t *u3h_repl_error(void)` | C export: the address of the bit writer's overflow word (the writers of G49 set it non-zero when a write does not fit) | C |
| `u3h_anim_pack_full` | `uint32_t u3h_anim_pack_full(uintptr_t base, uint8_t *msg, uint8_t *bs, uint8_t *actor, uint8_t **tabs, int32_t p5, int32_t p6)` | G49's anim state packer: writes the animation state of `actor` into bit stream `bs` (3.4) and returns the number of bits written | C |
| `u3h_repl_loop` | `void u3h_repl_loop(uintptr_t base, uint8_t *actor, uint8_t **tabs, uint8_t *msg, uint32_t bits)` | the decoder group's loop-back test hook (decodes what was just packed when its switch is on); it may change the message | C |

Part B may use a file-private net player id identical to `u3h_np_code` (no call needs to be recorded for it).

## 3. Game facts

### 3.1 Image data

| Image offset | Type | Meaning |
|---|---|---|
| 0x17cb060 | object | the net game manager ("manager") |
| manager + 0x370 + 8·n (0x17cb3d0 + 8·n), n = 0, 1 | pointer | the local net player record of screen n, or null |
| manager + 0x390 + 0x9c0·i, i = 0..9 | record | the ten net player records |
| 0x17b4ca4 | s32 | the net clock, ticks of 1/300 s |
| 0x17c3580 | pointer | the net clock object; s32 at +0xc = its time |
| 0x17c3b78 | object | first argument of the booster value function (0x689d20) |
| 0x17d5c61 | u8 | non-zero while multiplayer is active ("net-active") |
| 0x1806480 | pointer | the HUD object being drawn, or null; s32 at +0x1d0 = its screen number |
| 0x1b28330 (0x1b24ee0 + 0x3450) | u64 | non-zero while a net session exists |
| 0x17b39f0, 0x17b3138 | objects | the game's two camera lists (only their addresses are passed on) |
| 0x13ac250 | text | a source-file name of the game, passed to the allocator context (only its address is used) |
| 0x16d5b90 | virtual table | the game's class of the player state message |
| 0x13a6170 | 19 × u32 | the game's fixed weapon ids, numbers 0..18 (3.5) |

### 3.2 Net player record fields used here

| Offset | Type | Meaning |
|---|---|---|
| +0, +4, +8 | float | position x, y, z |
| +0xe1 | u8 | non-zero = joined |
| +0x1e4 | u32 | id word (low 12 bits used) |
| +0x1e8 | s32 | user (screen) number of the record on its own machine: 0 or 1 |
| +0x1ec | u8 | guest mark |
| +0x21c | s32 | team |
| +0x398 | u32 | a value sent as message +0x1d4 |
| +0x39c, +0x3a0 | s32 | the record is dead while (+0x39c) > (+0x3a0) |
| +0x430 | u8 | a byte sent as message +0x1de |
| +0x5a4 | u32 (float pattern) | a value sent as message +0x1d8 |
| +0x5e4 | u8 | "leaving" mark |
| +0x5f0, +0x5f4 | float | next send time (seconds) of this machine's screen 0 / screen 1 player to that record (G11 zeroes them on a clock change) |
| +0x9a8 | s32 | "targeted until" time (ticks of the net clock object) |
| +0x9ac | u32 | id of the player targeting this one |
| +0x9b1 | u8 | the game's own spotted flag ("F0", used for screen 0) |

The slot number of a record = (address − (manager + 0x390)) / 0x9c0 when the address is not below the first record,
the difference is an exact multiple of 0x9c0 and the quotient is below 10; otherwise the record "has no slot".

### 3.3 Character (player object) fields used here

| Offset | Type | Meaning |
|---|---|---|
| +0x30 | u32 | class id: 0xa5a63e9b = local player, 0xf2c86c22 = networked character |
| +0x80 | pointer | effect slot: pointer to a pointer to the current effect entry; entry + 0x10 = pointer to its record; record + 0x18 = u32 effect id |
| +0x138 | pointer | animation control; +0x40 = pointer P1; P1 + 0x10 = pointer P2; P2 + 0x120 = float |
| +0x198 | pointer | state object; s32 at +0x50 = state number |
| +0x310 | pointer | object with method 0x70 = weapon pose id (u16) |
| +0x39c | u32 | a value sent as message +0x1d0 |
| +0x3a8 | u32 | a value sent as message +0x154 |
| +0x3b0 | 16 bytes | look direction (four 32-bit words), sent as message +0x160 |
| +0x3c0 | u32 | a value sent as message +0x150 |
| +0x3f8 | u32 | (networked character) the id of its net player |
| +0x520 | pointer | weapon controller: method 0x60 = weapon mode; method 0x70 (slot number) = weapon object of a slot or null; method 0x78 = pointer to a name text or null; method 0x198 = a number |
| +0x528 | pointer | aim object: method 0xc8 (pointer to a 0x40-byte zeroed block, u32 0) = the character aimed at, or null |
| +0x538 | pointer | pose object or null; 0x60 bytes at +0x140 = the bound frame |
| +0x548 | pointer | object with method 0x98 = stance word |
| +0x550 | pointer | move controller: method 0x80 = mode (s32) |
| +0x598 | pointer | object with method 0xa8 = a test (u8) |
| +0x5f8 | pointer | cover object: method 0x68 = in cover (u8), method 0x70 = a second test (u8) |
| +0x668 | pointer | lock-on object: method 0x60 = pointer to the lock target record or null; target + 0x98, + 0x9ac, + 0x9b0 = three u32 |
| +0x688 | pointer | carry object: method 0x120 (u64 0) = the carried object or null |
| +0x6c0 | pointer | ride object or null: method 0x58 = what the player rides, or null |
| +0x1338 | u8 | non-zero = frozen |
| +0x1c20 | pointer | the animation tables (passed on to the packer) |
| +0x1c68 | s32 | screen number |

Weapon object: u32 at +0x6ec = weapon id; u8 at +0x6f0, u8 at +0x6f2 = two ammunition bytes; method 0x438 = a
state (u8); method 0x398 = a count (s32). Carried object: u32 at +0x30 = class id (0x310f465e = the carried kind
that is reported); method 0xe8 = its spawner or null (u32 at spawner + 0x28 = name id); u32 at +0x440 = its own
name id.

### 3.4 Game functions

Called directly (System V AMD64, integer arguments only) unless noted:

| Image offset | Arguments | Result | Meaning |
|---|---|---|---|
| 0x689d20 | (image 0x17c3b78, u32 effect id, net player record or null, null) | float (through `u3x_float_call`) | value of a booster effect for that player. Used with 0x01ca807a (Spotter radius, metres) and 0x974e6226 (the "on screen" value) |
| 0x793280 | (net player record, u32 booster id, u32 0) | u8 | non-zero when the player has that booster. Used with 0x41479fa0 (the counter booster) |
| 0xda1590 | (pointer to a u32 holding 0x14, image 0x13ac250, u64 0, image 0x13ac250) | none | enter the network allocator's context |
| 0xd9e990 | (u64 0x240, pointer to two u32 {0x14, 0}, u64 0x10) | pointer or null | allocate 0x240 bytes, alignment 0x10 |
| 0xda18b0 | () | none | leave the allocator context |
| 0x7336e0 | (manager, message, u32 mask) | none | the game's sender: queues the message for every net player slot i whose bit i is set in `mask` |
| 0x6ae2a0 | (object, u32 mask) | none | the game's own state sender for a non-player character |
| 0x4ef790 | (s32 screen, pointer to a u64 holding a camera list address) | pointer or null | the camera of that screen in that list |
| 0x11f2e30 | (u32 symbol id) | pointer or null | data of a script symbol. Used with 0x9e354807: the extra weapon list: s32 count at +0, pointer at +8 to u32 weapon ids |

Before its first game call `u3x_spotter` checks the code it is about to call: the 12 bytes at image 0x689d20 must
be 55 48 89 E5 41 57 41 56 41 55 41 54 and the 12 bytes at image 0x793280 must be 55 48 89 E5 41 57 41 56 41 54 53
49.

Bit stream block handed to the packer ("stream", 16 bytes, 16-byte aligned): pointer at +0 = the byte buffer, u32
at +8 = bit position (0), u32 at +0xc = capacity in bits.

### 3.5 The weapon numbers (table resolved)

The message carries weapons as small numbers in PS3 1.19's numbering: numbers 0..19 are fixed weapons, number
0x14 + k is entry k of the extra weapon list (symbol 0x9e354807).

- Numbers 0..18: the running PS4 game holds exactly these ids at image 0x13a6170 (19 consecutive u32; number n is
  the u32 at 0x13a6170 + 4·n; number 0 is id 0). They are read at run time; nothing is copied into the source.
- Number 19: the PS4 game (PS3 1.10's table) does not have it. It is the one weapon PS3 1.19 added to the fixed
  list: id 0x2daa4324 (a single id of the wire numbering, like the other ids named in this document).

So "fixed id of number n" = the image word for n ≤ 18, 0x2daa4324 for n = 19. No other table is needed.

### 3.6 Numeric constants

| Pattern | Value | Meaning |
|---|---|---|
| 0x3b5a740e | 1/300 | seconds per clock tick |
| 0x00800000 | smallest normal float | below or equal: squared distance counts as 0 |
| 0x41700000 | 15.0 | near distance (metres) and near rate (sends per second) |
| 0x3d888889 | 1/15 | scale of the distance band |
| 0x41000000 | 8.0 | far rate (sends per second) |
| 0x3f800000 | 1.0 | |
| 0x3f7fffff | the float just below 1.0 | "on screen" threshold |
| 0x7fcccccc | (a NaN pattern) | the game's "unset" mark in the message |
| 900 | ticks | how long a target stays marked (3 s) |
| 2400 | bits | capacity of the animation bit stream (300 bytes) |

### 3.7 The message object (0x240 bytes)

This is the layout the game's own code (writer of class 0x16d5b90, reader 0x71a010, apply 0x683670) expects. Bytes
not listed are not written by this group (they keep what the allocator returned).

| Offset | Type | Initial value (4.5) | Filled by 4.7 / 4.8 with |
|---|---|---|---|
| +0 | u64 | `base` + 0x16d5b90 | - |
| +8 | u8 | 0x10 (message type) | - |
| +9 | u8 | 2 | - |
| +0xa | u8 | 1 (reference count) | - |
| +0xb, +0xc | u8 | 1, 1 | - |
| +0xd | u8 | 0 | - |
| +0x14 | float | not initialised | time in seconds |
| +0x18 .. +0x143 | 300 bytes | not initialised | cleared, then the animation bit stream |
| +0x144 | u32 | not initialised | bit count of the stream |
| +0x148 | u32 | 0 | flags (below) |
| +0x14c | u32 | not initialised | sender's net player id |
| +0x150 | u32 | not initialised | character +0x3c0 |
| +0x154 | u32 | 0 | character +0x3a8 |
| +0x158 | u32 | 0 | stance word |
| +0x15c | float | 0 | animation-control float |
| +0x160 .. +0x16f | 16 bytes | not initialised | look direction |
| +0x170 .. +0x1cf | 0x60 bytes | u32 0x7fcccccc at +0x170 and +0x174; u64 0 at +0x178; u32 0 at +0x180; +0x184..+0x18f not initialised; +0x190..+0x1cf zero except u32 0x3f800000 at +0x1ac and at +0x1cc | the bound frame (when the pose object exists) |
| +0x1d0 | u32 | 0 | character +0x39c |
| +0x1d4 | u32 | 0 | net player +0x398 |
| +0x1d8 | float | 0 | net player +0x5a4 |
| +0x1dc | u16 | 0 | weapon pose id |
| +0x1de | u8 | 0 | net player +0x430 |
| +0x1e0 | u8 | 0 | 1 when there is a lock target |
| +0x1e4, +0x1e8, +0x1ec | u32 | 0 | lock target +0x98, +0x9ac, +0x9b0 |
| +0x1f0, +0x1f4, +0x1f8, +0x1fc | u32 | 0 | weapon numbers of slots 0, 1, 2, 5 |
| +0x200 | u32 | 0 | weapon mode (0, 1, 3, 4) |
| +0x204 | u32 | 1 | weapon controller method 0x198 |
| +0x208 | u32 | 0 | name id of the carried object |
| +0x20c | u32 | 0 | cover state 0 / 1 / 2 |
| +0x210 | u32 | 0 | id of the net player aimed at |
| +0x220 .. +0x22f | 16 bytes | 0 | - |
| +0x230, +0x231 | u8 | 0, 0 | first ammunition byte of slot 1, of slot 0 |
| +0x232, +0x233 | u8 | 0xff, 0xff | second ammunition byte of slot 1, of slot 0 |
| +0x234, +0x235 | u8 | 0, 0 | weapon state of slot 0, of slot 1 |

Flag bits of +0x148: 1 = "this is a player" (always set); 2 = move mode 1; 4 = net session up and the character
is in state 3; 8 = (with 4) the test of method 0xa8 answered 0; 0x20 = move mode 2; 0x40 = held / frozen.

### 3.8 The wire form (what the peer accepts)

This group does not write wire bytes. It hands the object of 3.7 to the game's sender (0x7336e0); the game's own
writer of the message class turns it into bits, and the peer's game reads them with its reader (image 0x71a010).
The reader fixes what travels and how wide. Its stream is a sequence of 32-bit words, bits taken from the most
significant bit of each word downward; a field of n bits is an unsigned number, first bit most significant. In
order (an object offset names where the reader stores the value):

| # | Bits | Field | Condition |
|---|---|---|---|
| 1 | 11 | time (+0x14), through the game's time coder 0x12751d0 (an 11-bit count scaled to seconds and placed next to the receiver's clock) | always |
| 2 | 11 | animation bit count (+0x144) | always |
| 3 | count | the animation stream (+0x18): whole bytes in buffer order, 8 bits each, then the remaining (count mod 8) bits (game function 0x1273970) | always |
| 4 | 3 | flags (+0x148), values 1 / 2 / 4 | always |
| 5 | 1 | flag 8 | when flag 4 is set |
| 6 | 1 | flag 0x40 | when flag 1 is set |
| 7 | 13 (flag 1 set) or 32 | sender id (+0x14c) | always |
| 8 | 5 | +0x150 | always |
| 9 | 32 | +0x154 | when +0x150 is 1, 2 or 3 |
| 10 | 3 × 16 | look direction (+0x160; the fourth word does not travel), game coder 0x1274f30 | when +0x150 is 1, 2 or 3 |
| 11 | 3 | stance (+0x158) | when +0x150 is 1, 2 or 3 and flag 1 is set |
| 12 | (game coder 0x7bf430) | bound frame (+0x170) | when +0x150 is 10 or 11 |
| 13 | 6, 6, 6, 6 | weapon numbers +0x1f0, +0x1f4, +0x1f8, +0x1fc | flag 1 |
| 14 | 3 | weapon mode (+0x200) | flag 1 |
| 15 | 1 | +0x204 | flag 1 |
| 16 | 2 | cover state (+0x20c) | flag 1 |
| 17 | 8, 8, 8, 8 | +0x230, +0x231, +0x232, +0x233 | flag 1 |
| 18 | 1, then 2 + 2 when it is 1 | weapon states +0x234, +0x235 (both 0 when the bit is 0) | flag 1 |
| 19 | 1, then 32 when it is 1 | carried name id (+0x208) | flag 1 |
| 20 | 3 | +0x15c as a number 0..7 (the float is value / 7.0) | flag 1 |
| 21 | 1, 1 | bit 1, then bit 0 of the byte at +0x1d0 | flag 1 |
| 22 | 8, then 32 | +0x1d8 as a number 0..255 (float = value / 255.0), then +0x1d4 | flag 1 and bit 0 of +0x1d0 |
| 23 | 8 | +0x1de | flag 1 |
| 24 | 1, then 13 when it is 1 | aimed-at player id (+0x210) | always |
| 25 | 1, then 32 + 32 + 32 when it is 1 | lock mark (+0x1e0), then +0x1e4, +0x1e8, +0x1ec | always |

(A message without flag 1 - a non-player character, built by the game's own sender 0x6ae2a0 - carries, in place of
rows 13 to 23: 16 bits +0x1dc, 1 bit +0x1df with the block at +0x220 when set, 1 bit for flag 0x10, 1 bit for bit
3 of +0x1d0.)

Consequences for this group: the bit count must be a multiple of 32 (4.5: the game's packer leaves the bits of a
partial last word in its high bytes, so only whole words survive the byte-wise copy of row 3); a weapon number
travels in 6 bits, +0x150 in 5, the bit count in 11. See section 6, notes 1 to 4, for values that do not travel.

## 4. Behaviour

### 4.1 `u3x_spotter(base, s, spotter)` (part A)

1. ec[50] += 1.
2. The two code checks of 3.4. Either fails: end.
3. R = `u3x_float_call(base + 0x689d20, base + 0x17c3b78, 0x01ca807a, spotter, 0)`. Unless 0.0 < R: end.
   R2 = `f_mul(R, R)`.
4. "both" = bit 7 (0x80) of ec[0] is clear. F1 = `u3h_spot_flags()` (asked once, before the loop).
5. For i = 0 .. 9, T = net player record i:
   a. skip when the net-active byte is 0 (read again for every i);
   b. skip when T is `spotter`;
   c. G = `u3h_netgame(base, s)` (one call for every i that gets here). Unless G is not null and its s32 at +0xc is
      0xe: skip T when its team (s32 +0x21c) equals the spotter's team;
   d. skip when T is dead (s32 +0x39c > s32 +0x3a0);
   e. for j = 0, and also j = 1 when "both": the flag byte is T + 0x9b1 for j = 0 and F1[i] for j = 1.
      - Already non-zero: nothing for this j.
      - dx = `f_sub`(spotter x, T x), dy and dz alike (spotter first); D2 = `f_add(f_mul(dz, dz), f_add(f_mul(dx,
        dx), f_mul(dy, dy)))`. Unless D2 < R2: nothing for this j.
      - game 0x793280(T, 0x41479fa0, 0) non-zero (low byte): nothing for this j.
      - when "both": L = the local net player record of screen j (3.1). L not null and game 0x793280(L,
        0x41479fa0, 0) non-zero: nothing for this j. (L null: no call.)
      - the flag byte = 1; ec[51 + j] += 1.

### 4.2 `u3x_spot_clear(base, np)` (part A)

F1 = `u3h_spot_flags()`. `np` not null: when it has a slot i (3.2), F1[i] = 0; nothing otherwise. `np` null: for
i = 0 .. 9, F1[i] = 0 when the u8 at +0x5e4 of record i is 0 (other entries stay).

### 4.3 `u3x_spot_flag(base, np)` and `u3x_spot_read(base, np)` (part A)

`u3x_spot_flag`: H = the pointer at image 0x1806480. When bit 7 of ec[0] is clear, H is not null and the s32 at
H + 0x1d0 is 1: ec[53] += 1, and when `np` has a slot i the result is F1[i] (`u3h_spot_flags()` is asked only
then). In every other case the result is the u8 at `np` + 0x9b1.

`u3x_spot_read`: ec[54] = `u3x_spot_flag(base, np)` (as u32) when `np` is not null, else 0.

### 4.4 `u3x_distance(a, b)` (part B)

dx = `f_sub`(a x, b x), dy, dz alike; D2 = `f_add(f_add(f_mul(dx, dx), f_mul(dy, dy)), f_mul(dz, dz))`. Result:
`f_root(D2)` when 0x00800000 < D2, else the pattern 0. (So NaN and tiny values give 0; infinity gives infinity.)
Pure. "dist(a, b)" below is this value.

### 4.5 `u3x_send_player_state(base, s, player, mask)` (part B)

Nothing when `player` is null. Otherwise, in this order:

1. game 0xda1590 with the arguments of 3.4 (the u32 0x14 lives in a local).
2. M = game 0xd9e990(0x240, pointer to the local pair {0x14, 0}, 0x10).
3. When M is not null:
   a. the initial values of 3.7 are written (all before the next step; their order among themselves is free);
   b. B = `u3h_player_fill(base, s, M, player)`;
   c. when B is not 0: B is rounded up to the next multiple of 32 and stored as the u32 at M + 0x144;
   d. ec[61] += 1; ec[62] = B; ec[63] = B when B is above ec[63];
   e. when B is not 0: game 0x7336e0(manager, M, mask); ec[64] += 1; ec[65] = mask;
   f. release: the u8 at M + 0xa is decreased by 1; when it becomes 0, method 8 of M is called (argument M only).
4. game 0xda18b0() (also when M was null).

### 4.6 `u3x_replication(base, s, np, player)` (part B)

1. Nothing when bit 8 (0x100) of ec[0] is set; nothing when `player` is null or the pointer at `player` + 0x138
   is null. Otherwise ec[60] += 1.
2. NOW = `f_mul(f_from_int(s32 at image 0x17b4ca4), 0x3b5a740e)`. SCR = s32 at `np` + 0x1e8. mask = 0.
3. For i = 0 .. 9, Q = net player record i. Q is skipped when any of these holds: the net-active byte is 0 (read
   again for every i); Q's u8 at +0xe1 is 0; Q's u8 at +0x5e4 is not 0; Q's s32 at +0x1e8 is 1 (the second
   screen's player of a remote machine is served through its partner); SCR is not 0 or 1. Otherwise:
   a. D = dist(Q, `np`).
   b. Partner: P = (Q's u32 at +0x1e4 AND 0xfff), with bit 0x1000 added when Q's s32 at +0x1e8 is 0. When P is not
      0: the first record j = 0 .. 9 (all ten, whatever their state) whose net player id (`u3h_np_code` rule)
      equals P is the partner; D' = dist(partner, `np`); D becomes D' unless `f_sub(D, D')` < 0. No such record: D
      stays.
   c. W = the float at Q + 0x5f0 + 4·SCR. When NOW < W: Q is not due; next i.
   d. mask gets bit i.
   e. F = `f_mul(f_sub(D, 0x41700000), 0x3d888889)`; F = 0 when F < 0; then F = 1.0 unless F < 1.0.
      RATE = `f_add(f_mul(f_sub(0x3f800000, F), 0x41700000), f_mul(F, 0x41000000))`;
      W = `f_add(NOW, f_div(0x3f800000, RATE))` is stored at Q + 0x5f0 + 4·SCR.
      (15 sends per second up to 15 m, falling linearly to 8 per second at 30 m and beyond.)
4. mask 0: end.
5. The message: exactly 4.5 with (`base`, `s`, `player`, mask).
6. Ride: R = the pointer at `player` + 0x6c0. When R is not null: method 0x58 of R is called; when its result is
   not null it is called a second time, giving O; game 0x6ae2a0(O, mask); ec[73] += 1.

### 4.7 `u3x_player_fill(base, s, msg, pl)` (part C)

In this order (the order of game and method calls is observable; `msg` was prepared by 4.5, so +0x148 starts as 0
and bits are OR-ed into it):

1. Flags. Bit 1 is set. ST = pointer at `pl` + 0x198: when the u64 at image 0x1b28330 is not 0, ST is not null
   and its s32 at +0x50 is 3: bit 4 is set, and method 0xa8 of the object at `pl` + 0x598 is called; when its
   result as u8 is 0, bit 8 is set.
2. Move flags: `u3x_has_eff(pl, 0x5ec9a3e7)`, and when that is 0 `u3x_has_eff(pl, 0xe9be68ac)`; when either is
   non-zero nothing is added and the move controller is not asked. Otherwise method 0x80 of the object at
   `pl` + 0x550; its result as s32: 1 adds bit 2, 2 adds bit 0x20.
3. Held: bit 0x40 when the u8 at `pl` + 0x1338 is not 0, or (asked in this order, stopping at the first hit)
   `u3x_has_eff(pl, 0xd69dcc37)`, `u3x_has_eff(pl, 0x3eaffc07)`, `u3x_has_eff(pl, 0x16b32ade)`.
4. SCR = s32 at `pl` + 0x1c68; NP = the local net player record of screen SCR (3.1) when SCR is 0 or 1, else null.
   Here the net-active byte is not consulted.
5. +0x14c = `u3h_np_code(NP)` (0 and no call when NP is null). +0x150 = u32 at `pl` + 0x3c0.
   +0x14 = `f_mul(f_from_int(u3h_net_now(base)), 0x3b5a740e)`. +0x154 = u32 at `pl` + 0x3a8. The 16 bytes at
   +0x160 = the 16 bytes at `pl` + 0x3b0. +0x158 = method 0x98 of the object at `pl` + 0x548, as u32.
6. Pose object (pointer at `pl` + 0x538) not null: the 0x60 bytes at +0x170 = its 0x60 bytes at +0x140.
7. u8 +0x1de = NP's u8 at +0x430 (0 when NP is null). u16 +0x1dc = method 0x70 of the object at `pl` + 0x310, as
   u16.
8. Lock: method 0x60 of the object at `pl` + 0x668; a non-null result K: u8 +0x1e0 = 1, +0x1e4 = u32 at K + 0x98,
   +0x1e8 = u32 at K + 0x9ac, +0x1ec = u32 at K + 0x9b0.
9. Animation. TAB = pointer at `pl` + 0x1c20. The 300 bytes at +0x18 are cleared. A stream block (3.4) is set
   up: buffer `msg` + 0x18, position 0, capacity 2400. The word at `u3h_repl_error()` is set to 0. BITS =
   `u3h_anim_pack_full(base, msg, stream, pl, TAB, 1, 3)`. When the error word is now not 0: ec[71] += 1 and BITS
   = 0 (the buffer is left as the packer left it). +0x144 = BITS. Then `u3h_repl_loop(base, pl, TAB, msg, BITS)`.
10. +0x1d0 = u32 at `pl` + 0x39c. +0x1d4 = NP's u32 at +0x398 (0 without NP). u16 +0x1dc = method 0x70 of the
    object at `pl` + 0x310 again (a second call). +0x1d8 = NP's u32 at +0x5a4 (0 without NP).
11. `u3h_weapons_fill(base, msg, pl)`.
12. "On": method 0x78 of the weapon controller (`pl` + 0x520) gives a text pointer; when it is not null and its
    first byte is not 0, ON is true and nothing more is asked. Otherwise C = `u3x_char_np(base, pl)`, V =
    `u3x_float_call(base + 0x689d20, base + 0x17c3b78, 0x974e6226, C, 0)`, ON = (0x3f7fffff < V).
13. T = `u3h_netgame_type(base, s)` (always called, after step 12). Only when T is not 0xe and ON: game
    0x4ef790(SCR, pointer to a u64 holding `base` + 0x17b39f0); when that is null, the same call with
    `base` + 0x17b3138. When either call returned non-null, step 14 runs.
14. Target: a 0x40-byte block, 16-byte aligned, all zero; A = method 0xc8 of the aim object (`pl` + 0x528) with
    (block, u32 0). TN = `u3x_char_np(base, A)` when A is not null. A or TN null: nothing more. Otherwise: NOWT =
    s32 at +0xc of the net clock object (pointer at image 0x17c3580, not tested for null); s32 at TN + 0x9a8 =
    NOWT + 900; u32 at TN + 0x9ac = `u3h_np_code(NP)` (0 and no call without NP); +0x210 =
    `u3h_np_code(TN)`; ec[72] += 1.
15. Result: the u32 at `msg` + 0x144 as it is now (read back after the calls above).

### 4.8 `u3x_weapons_fill(base, msg, pl)` (part C)

WC = the weapon controller (pointer at `pl` + 0x520). "Slot n" = method 0x70 of WC with the u64 argument n. In
this order:

1. Slot 0, when not null (W): +0x1f0 = `u3x_weapon_item(base, u32 at W + 0x6ec)`; u8 +0x231 = W's u8 at +0x6f0;
   u8 +0x233 = W's u8 at +0x6f2; then u8 +0x234 = method 0x438 of W, as u8.
2. Slot 1, when not null: +0x1f4 = its number; u8 +0x230 = its u8 at +0x6f0; u8 +0x232 = its u8 at +0x6f2; then
   u8 +0x235 = its method 0x438, as u8.
3. Slot 2, when not null: its method 0x398 is called; when the result as s32 is above 0, +0x1f8 = its number
   (the number is not computed otherwise).
4. Slot 5, when not null: +0x1fc = its number.
5. Carried object: O = method 0x120 of the carry object (pointer at `pl` + 0x688) with u64 0. When O is not null
   and its u32 at +0x30 is 0x310f465e: E = method 0xe8 of O; +0x208 = the u32 at E + 0x28 when E is not null, else
   the u32 at O + 0x440.
6. +0x200 = `u3x_weapon_mode_119`(method 0x60 of WC, as u32).
7. +0x204 = method 0x198 of WC, as u32.
8. +0x15c: P = pointer at `pl` + 0x138; P1 = pointer at P + 0x40 when P is not null; P2 = pointer at P1 + 0x10
   when P1 is not null. P2 not null: +0x15c = the u32 at P2 + 0x120. Otherwise +0x15c = 0 and ec[69] += 1.
9. +0x20c = 0. CV = pointer at `pl` + 0x5f8: method 0x68 of CV; when its result as u8 is not 0, method 0x70 of CV
   is called and +0x20c = 2 when that result as u8 is not 0, else 1.

A slot that is null leaves its message fields as they were (the initial values of 3.7).

### 4.9 `u3x_weapon_item(base, id)` and `u3x_weapon_fixed_id(base, index)` (part C)

`u3x_weapon_fixed_id`: 3.5 (the image word for index ≤ 18, 0x2daa4324 for 19, 0 above). Pure reads.

`u3x_weapon_item`: the smallest n in 0 .. 19 whose fixed id equals `id` is the result (so `id` 0 gives 0, with no
game call). Otherwise X = game 0x11f2e30(0x9e354807) (exactly one call, only in this case); X null: 0. Else with
N = s32 at X + 0, IDS = pointer at X + 8: the smallest k ≥ 0, k < N (signed) with IDS[k] = `id` gives 0x14 + k;
IDS null, N ≤ 0 or no match: 0.

### 4.10 `u3x_has_eff(c, sid)` (part C)

SL = pointer at `c` + 0x80. 0 when SL is null or the pointer SL points at is null. Otherwise REC = the pointer at
+0x10 of that entry (not tested), result 1 when the u32 at REC + 0x18 equals `sid`, else 0. Reads only.

### 4.11 `u3x_char_np(base, c)` (part C)

Null when `c` is null or the net-active byte is 0. By the u32 at `c` + 0x30: 0xa5a63e9b: the local net player
record of screen (s32 at `c` + 0x1c68) when that is 0 or 1, else null; 0xf2c86c22: with ID = u32 at `c` + 0x3f8,
`u3h_np_by_id(base, ID)` when ID is not 0, else null (no call); any other class: null.

## 5. Test scenarios

All three parts can be tested old against new on a fake image (the ten records, the image words of 3.1, fake
objects with recording virtual tables) with call-recording stubs for the game functions and the `u3h_` / other
group functions of 2.4; compare memory (records, F1, message, ec words), results, and the order and arguments of
every call. Float results bit for bit. Nothing here needs the live game except the final check that a remote
player moves and that the spotted icon shows per screen.

Part A (stubs: float call, 0x793280, `u3h_netgame`, `u3h_spot_flags`):
1. Either code check failing: only ec[50] moves. R = 0, negative, NaN: no flag changes, no 0x793280 call.
2. One enemy inside / exactly at / outside the radius (D2 < R2 strict); enemy = the spotter itself; team mate with
   game type not 0xe (skipped) and with type 0xe (spotted); no net game (null: team rule applies); dead enemy.
3. Flag already set for j = 0 only, j = 1 only; counter booster on the target; on the local player of screen 0
   only, screen 1 only; local player pointer null; ec[0] bit 7 set (only j = 0, no local-player test).
4. Net-active byte 0. Random positions and teams, thousands of cases.
5. `u3x_spot_clear`: a record address, an address between records, below the first, past the tenth; null with
   mixed +0x5e4 bytes. `u3x_spot_flag`: H null; H screen 0, 1, 2; bit 7; `np` without a slot. `u3x_spot_read` null.

Part B (stubs: 0xda1590, 0xd9e990, 0x7336e0, 0xda18b0, 0x6ae2a0, `u3h_player_fill`, recording methods):
6. `u3x_distance`: equal points, D2 at 0x00800000 and one step above, large values, NaN, infinity; random.
7. `u3x_send_player_state`: null player; allocation null (push and pop only); fill result 0 (no send, release
   still done), 1, 31, 32, 33, 2400; the whole 0x240 bytes compared after pre-filling the block with a pattern
   (shows which bytes stay untouched); ec[63] maximum; reference count reaching 0 calls method 8 once.
8. `u3x_replication`: ec[0] bit 8; null player; null +0x138; SCR 0, 1, 2, −1; records not joined, leaving, with
   +0x1e8 = 1; due and not due (W above, equal to, below NOW; W NaN); D below 15, 15..30, above 30, NaN; partner
   present and nearer, farther, absent, P = 0; mask 0 (no message); ride null, ride method giving null, non-null
   (two calls, then 0x6ae2a0); net-active 0.

Part C (stubs: `u3h_np_code`, `u3h_np_by_id`, `u3h_net_now`, `u3h_repl_error`, `u3h_anim_pack_full`,
`u3h_repl_loop`, `u3h_weapons_fill`, `u3h_netgame_type`, float call, 0x4ef790, 0x11f2e30, recording methods):
9. Flags: session word 0 / non-zero, state null / 3 / other, method 0xa8 giving 0 / 1; each of the two "no move"
   effects; move mode 0, 1, 2, 3; frozen byte; each of the three hold effects.
10. SCR 0, 1, 2 (NP null: zeros, no id call); pose object null; lock null / present.
11. Packer result 0, 100, with the error word set by the stub (BITS 0, ec[71]); `u3h_repl_loop` stub changing
    +0x144 (the result follows it).
12. ON by name; name null; empty name with V = 1.0, the pattern below 1.0, NaN; type 0xe; no camera in either
    list, camera in the second list only; aim target null; target that is no net player; NP null in step 14.
13. `u3x_weapons_fill`: every slot null / present; slot 2 count 0, 1, −1; carried object null, other class, with
    and without spawner; mode 0, 1, 5, 2, 7; P, P1, P2 null in turn (ec[69]); cover 0, cover with second test 0 / 1.
14. `u3x_weapon_item`: every fixed id (the image words placed at 0x13a6170, including the id that occurs twice:
    the lower number wins), 0x2daa4324 (19), id 0, an id in the extra list at k = 0 and at the end, list null,
    count 0 and negative, ids pointer null, unknown id. `u3x_weapon_fixed_id` 0, 18, 19, 20.
15. `u3x_has_eff`: null slot, null entry, matching / other id. `u3x_char_np`: null, net-active 0, both classes,
    screen 2, id 0, other class.

## 6. Behaviour to keep (looks odd, must stay)

1. Flag 0x20 (move mode 2) is set in the object, but the game's reader (3.8 row 4) carries only the values 1, 2
   and 4 of the flag word plus the separate bits for 8 and 0x40.
2. The rounded bit count can reach 2400, while the wire field has 11 bits (at most 2047). There is no limit
   besides the packer's capacity.
3. The weapon pose id (+0x1dc) is asked twice (steps 7 and 10 of 4.7) and does not travel in a player message.
   The whole u32 of character +0x39c goes to +0x1d0, of which the game sends two bits.
4. A weapon number above 63 (an extra-list entry from k = 44) does not fit the 6 wire bits.
5. 4.8 crosses the ammunition bytes: slot 0 fills +0x231 / +0x233 and slot 1 fills +0x230 / +0x232, while the
   state bytes are +0x234 for slot 0 and +0x235 for slot 1.
6. `u3x_spot_clear(base, null)` clears the entries of the records whose +0x5e4 byte is 0.
7. `u3x_spotter` asks `u3h_netgame` and reads the net-active byte once per target, not once per call; the same
   per-record read of the net-active byte in 4.6.
8. In 4.6 the ride's method 0x58 is called twice; the partner search looks at all ten records.
9. No null tests where section 4 names none: the net clock object in 4.7 step 14, the effect record in 4.10, the
   objects whose methods are called in 4.7 / 4.8 (`pl` + 0x520, 0x528, 0x548, 0x550, 0x598, 0x5f8, 0x668, 0x688,
   0x310), `np` in `u3x_spot_flag`.
10. A packer overflow gives bit count 0, so nothing is sent for that frame (4.5 step e), and the message is still
    allocated and released.
11. The message is built and filled once per frame per local player whenever at least one record is due, with one
    mask for all due records.
12. Dead code: C `call_f32_119` (no caller), C `player_send119` and `dist119` (no caller).

## 7. Open questions and notes for the integrator

1. **Float-result call.** As G44 section 7 note 1: `u3x_float_call` is today the file-private `call_float` of the
   animation lookups group (G51). It must be reachable under that exported name (or the name G51's new code gives
   it).
2. **The table `wtab119`.** After this group it has one more user: C `k27_wstat` (the weapon-state receiver's stat
   index decoder, through `u3x_weapon_stat_id(number, fixed, extra, missing)`), which is in no group of this
   document. `u3x_weapon_fixed_id` gives it the same 20 values without the table (an index below 20 is the fixed
   id, 20 + k is extra entry k); the integrator or that function's describer has to switch it over before
   `wtab119` can be deleted.
3. **Number 19.** Its id (0x2daa4324) is not in the PS4 image's fixed list. Whether the PS4 game holds it in the
   extra list (symbol 0x9e354807) could not be read here (script data, only present in a running game). The old
   code answers 19 for it in every case, and so does this description.
4. **What the fields mean.** Character +0x39c / +0x3a8 / +0x3c0 and net player +0x398 / +0x5a4 / +0x430 are
   described only as "the value sent in field X": their game meaning is not needed to reproduce the behaviour.
   3.8 comes from the PS4 reader; the conditions under which the game's WRITER sets the presence bits of rows 18,
   19, 24 and 25 were not examined (the writer is the game's, not this group's).
5. New C one-line wrappers: `weapons119` -> `u3x_weapons_fill`, `witem119` -> `u3x_weapon_item`; the C helpers
   `vget119` / `vget1_119` stay only if other C code still uses them.

## 8. Clarifications (tester, 2026-11-14)

Facts found or confirmed by the old-against-new test (old = v7.212). Items marked "open point" answer the
implementers' questions.

1. **Float.** The old code of this group runs on single-precision instructions only (the compiler output of v7.212
   for the spotter, the distance, the send loop and the fill holds scalar single-precision subtract, multiply, add,
   divide, square root, compare and integer-to-float; no x87 instruction). The formulas of 4.1, 4.4, 4.6 and 4.7
   step 5 give the old results bit for bit, in exactly the stated grouping.
2. **4.1 step 4, order.** `u3h_spot_flags()` is asked first; bit 7 of ec[0] ("both") is read after that call (and
   after the float call). As `u3h_spot_flags` only returns an address, this order cannot be seen in the helper.
3. **4.7 steps 1 to 3, when the flag bits reach the message (open point).** Each bit is OR-ed into the u32 at
   +0x148 at the moment it is decided: bit 1 before anything else; bit 4 before method 0xa8 is called; bit 8 right
   after that call; the move bit after method 0x80 returned; bit 0x40 last. So method 0xa8 runs while +0x148 holds
   (start value) | 1 | 4, and method 0x80 while it holds (start value) | 1, plus 4 and 8 when they were set. Every
   OR works on the word as it is in memory at that moment. The game's methods do not get the message, so in the
   helper only the final word can be seen, and that is the same whether the bits are stored one by one or together.
4. **4.7 step 13, the camera call (open point).** The arguments are the s32 SCR as read in step 4 (whatever its
   value: 2, negative and large values are passed on unchanged) and the address of a local u64 that holds
   `base` + 0x17b39f0; the second call (only when the first returned null) gets the same SCR and a u64 holding
   `base` + 0x17b3138.
5. **4.7 step 14, order (open point).** Method 0xc8; then `u3x_char_np(A)` (with its `u3h_np_by_id` call for a
   networked character); then the clock time is read and TN + 0x9a8 is stored; then `u3h_np_code(NP)` is called
   (TN + 0x9a8 already holds the new stamp) and TN + 0x9ac is stored; then `u3h_np_code(TN)` is called (TN + 0x9ac
   already holds the new id) and +0x210 is stored; ec[72] last. NOWT + 900 wraps in 32 bits.
6. **4.8, the order inside a weapon slot (open point).** The slot method; then the weapon id is read and the
   number is found (with the one 0x11f2e30 call when the id is not a fixed one) and stored; only then the two
   ammunition bytes are read and stored; method 0x438 is called last and its byte stored after it returned.
7. **4.9, reads (open point).** Id 0 gives 0 with no game call (number 0's fixed id is 0). An id that is not a
   fixed one: exactly one 0x11f2e30 call. With N of 0 or less the ids are never read through (the pointer at +8
   may be anything); a null pointer with N above 0 gives 0.
8. **4.5 step c, rounding.** The rounding is 32-bit arithmetic: a B above 0xffffffe0 rounds to 0, which is then
   stored at +0x144 and in ec[62], and nothing is sent.
9. **4.5 step d (open point).** With B = 0: ec[61] counts, ec[62] becomes 0, ec[63] stays, nothing is sent, the
   message is released.
10. **4.6 step 3, the skip tests.** No call lies between the five tests, the partner search and the due-time test,
    so the order in which they are made cannot be seen; all 32 combinations give "skipped unless none holds".
    ec[60] counts also when SCR is not 0 or 1.
11. **4.6 step 3 b, the partner (open point).** The record with the lowest index whose id equals P is the partner,
    also when a later record with the same id is nearer, and whatever its "joined" and "leaving" bytes say. A
    partner at a NaN position has distance 0 and therefore wins.
12. **4.6 step 6, the ride (open point).** When the second call of method 0x58 returns null, game 0x6ae2a0 is still
    called (with null) and ec[73] counts. The ride step also runs when the fill result was 0 (no message sent).
13. **3.5.** The 19 u32 at image 0x13a6170 of the PS4 image file are equal to the ids the old code held for
    numbers 0 to 18. Number 18's id is the same as number 11's, so `u3x_weapon_item` never answers 18.
