# G51. Anim decoder lookups and ice-snapshot commands

Behaviour description for a reimplementation (reimpl2 pipeline, describer's document). It says what the functions
do, not how the old ones were written. Offsets are hexadecimal when prefixed with 0x. "Image offset X" means the
address `base + X`, where `base` is the game image base. All multi-byte values in memory are little-endian; "u8 /
u16 / u32 / s32 / u64" are integers of that width, "pointer" is 8 bytes. A "float pattern" is the 32-bit pattern of
a single-precision float carried in a u32. Game functions use the System V AMD64 convention.

Related documents: `G50_anim_decoder.md` (the decoder that uses the bit reader and the lookups; its section 3.2 is
the visitor record, 3.4 the generator / snapshot layouts, 3.1 the stub blocks: this document uses the same names),
`G49_anim_packer.md` (`u3x_tab_find`, `u3x_art_tab`), G52 (the visitors `u3x_v_*`), G61 (`u3x_find_art_group`),
`G44_kill_rewards.md` and `G48_player_state.md` (users of the float-result call), `G42_death_cameras.md` (a user of
the float-argument call), `film_cameras.md` (`u3x_frame_identity`).

**Float.** This group does no float arithmetic. It compares float patterns for equality (4.3), copies patterns,
and moves patterns into or out of the game's first float register (4.1, 4.2). Nothing here ran on the x87 unit, so
every result must match the old code bit for bit. No float is passed or returned by value.

## 1. Purpose and callers

PS3 1.19 sends each character's animation state as a bit stream and rebuilds animation commands from it on the
receiver. The PS4 game kept the receiving object (the "net anim command generator" of a net character or NPC) but
not the decoding. The helper restores it. This group is the decoder's small tools and one of its three users:

- two **call shims** for game functions that take or return a float in a register (4.1, 4.2);
- the **bit reader** the decoder reads the stream with (4.4, 4.5);
- four **lookups** in the game's animation data: an art group by name id, an anim by index or by name id inside a
  group, the streaming record of an anim id (4.6 to 4.9);
- the **net anim package lookup** (row, column) and the generator's **skeleton** and **snapshot pick** (4.10 to
  4.12; strays, see section 6);
- **CreateAnimCmds** (4.13, 4.14): the replacement for the game's function at image 0x6847d0, which turns the
  generator's two current snapshots into animation commands, reusing a finished "ice" copy of a snapshot when
  there is one.

Callers:

| Function | Called by |
|---|---|
| `u3x_float_call` (old private `call_float`) | C++ only: the Spotter radius and the "on screen" value (G48), the booster value (G44), `u3x_respawn_delay_bits` (a one-liner named in G40) |
| `u3x_call_float_arg` | C `call_x0_119` (and through it the C export `u3h_call_f32`, used by G50 and by 4.13 here); C++: 4.12 here, G52's `u3x_v_blend` and `u3x_v_mark`, G42 (image 0xc1ccc0) |
| `u3x_float_equal` | C `f_eq119`; 4.14 here |
| `u3x_bits_init`, `u3x_bits_read` | C `br_init119`, `br119`; G50's `u3x_decode_anim` |
| `u3x_art_group_sid`, `u3x_anim_by_index`, `u3x_stream_record` | C `art_group_sid119`, `tab_idx119`, `stream_rec119`; G50's `u3x_decode_anim` |
| `u3x_anim_by_sid` | C `tab_sid119` (and through it the C export `u3h_anim_by_sid`, used by 4.13); G50's `u3x_decode_anim` |
| `u3x_net_art_group` | C `net_ag119` (and through it the C export `u3h_net_art_group`, used by 4.13, and the generator's table fill `u3x_anim_tables`, which gets `net_ag119` as its lookup) |
| `u3x_generator_skeleton`, `u3x_generator_pick` | C `gen_skel119`, `gen_pick119` (and through them the C exports `u3h_skeleton`, `u3h_pick_snapshot`, `u3h_decode`) |
| `u3x_anim_commands` | C `cac119`, from the event dispatcher's kind 118: the value stub at the entry of game function 0x6847d0 |

Everything runs on the game thread that updates animation or handles the state message. No locking, no timing
rule, no state of its own (the ice copies live in the generator, the counters in the helper's `ec[]` words).

## 2. Interface

### 2.1 Functions to deliver (all `extern "C"`; names and signatures unchanged except the first)

| Name | Signature | Section |
|---|---|---|
| `u3x_float_call` (new exported name of the old file-private `call_float`) | `uint32_t u3x_float_call(uintptr_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)` | 4.1 |
| `u3x_call_float_arg` | `uint64_t u3x_call_float_arg(uintptr_t fn, uint32_t f0, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)` | 4.2 |
| `u3x_float_equal` | `int u3x_float_equal(uint32_t a, uint32_t b)` | 4.3 |
| `u3x_bits_init` | `void u3x_bits_init(void *reader, uint8_t *buf, uint32_t nbytes)` | 4.4 |
| `u3x_bits_read` | `uint32_t u3x_bits_read(void *reader, uint32_t n)` | 4.5 |
| `u3x_art_group_sid` | `uint8_t *u3x_art_group_sid(uintptr_t base, uint32_t sid)` | 4.6 |
| `u3x_anim_by_index` | `uint8_t *u3x_anim_by_index(uint8_t *group, int32_t index)` | 4.7 |
| `u3x_anim_by_sid` | `uint8_t *u3x_anim_by_sid(uint8_t *group, uint32_t sid)` | 4.8 |
| `u3x_stream_record` | `uint8_t *u3x_stream_record(uintptr_t base, uint32_t id)` | 4.9 |
| `u3x_net_art_group` | `uint8_t *u3x_net_art_group(uintptr_t base, uint32_t row, uint32_t col)` | 4.10 |
| `u3x_generator_skeleton` | `int32_t u3x_generator_skeleton(uint8_t *generator)` | 4.11 |
| `u3x_generator_pick` | `int32_t u3x_generator_pick(uintptr_t base, uint8_t *generator, uint32_t *blend)` | 4.12 |
| `u3x_anim_commands` | `int u3x_anim_commands(uintptr_t base, const uint64_t *regs)` | 4.13 |

The old private helpers (`snapshot_commands`, the two ice-copy choosers, the "anim table of a group" read, the
6 x 8 name table) are not interface: the new code organises 4.13 / 4.14 as it likes. No function is still in C, so
no new C wrapper is needed; the existing one-line C wrappers of section 1 stay. The users of the old private
`call_float` inside the C++ part (G44, G48, `u3x_respawn_delay_bits`) call `u3x_float_call` instead.

Two parts, if two implementers are wanted (about 200 old lines in all, so one is enough): part A = 4.1 to 4.12
(shims, bit reader, lookups); part B = `u3x_anim_commands` (4.13, 4.14), which uses of part A only
`u3x_float_equal` and reaches everything else through the `u3h_` exports of 2.2.

### 2.2 Functions of other groups, of the C part and shared tools that are called

| Name | Signature | Effect | Used in |
|---|---|---|---|
| `u3h_ext` | CONVENTIONS.h | the helper's state; the words `ec[]` are u32 at `u3h_ext() + 0x4450` | 4.10, 4.13 |
| `u3x_tab_find` (old code: through the C export `u3h_tab_find`, same signature, which only forwards) | `int32_t u3x_tab_find(uint8_t *table, uint32_t key)` | G49: binary search of an anim table (3.3) for an entry whose name id is `key`; its index, or -1 (also -1 when the table has no entry array) | 4.8 |
| `u3h_find_art_group_name` | `uint8_t *u3h_find_art_group_name(uintptr_t base, const char *name)` | C part: the loaded art group with that name (null for a null or empty name, or when none is loaded): name id through the game's name-id function, then the search of 4.6 | 4.10 |
| `u3x_handle_obj` | `uint8_t *u3x_handle_obj(uint8_t *handle)` | existing one-liner (G46 2.2): the object a game handle {pointer to an object slot at +0, u32 id at +8} still names, or null. Its use in 4.11 is optional | 4.11 |
| `u3h_pick_snapshot` | `int32_t u3h_pick_snapshot(uintptr_t base, uint8_t *generator, uint32_t *blend)` | C export that forwards to `u3x_generator_pick` (4.12) | 4.13 |
| `u3h_net_art_group` | `uint8_t *u3h_net_art_group(uintptr_t base, uint32_t row, uint32_t col)` | C export that forwards to `u3x_net_art_group` (4.10) | 4.13 |
| `u3h_anim_by_sid` | `uint8_t *u3h_anim_by_sid(uint8_t *group, uint32_t sid)` | C export that forwards to `u3x_anim_by_sid` (4.8) | 4.13 |
| `u3h_skeleton` | `int32_t u3h_skeleton(uint8_t *generator)` | C export that forwards to `u3x_generator_skeleton` (4.11) | 4.13 |
| `u3h_call_f32` | `uint64_t u3h_call_f32(uintptr_t fn, uint32_t f0, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)` | C export that forwards to `u3x_call_float_arg` (4.2) | 4.13 |
| `u3h_decode` | `void u3h_decode(void *visitor, uint8_t *snapshot, uint8_t *generator, int32_t a4, int32_t a5)` | C export (G50 2.2): runs G50's `u3x_decode_anim` on the snapshot's stream with the generator's tables, default index and skeleton value; `a4` / `a5` become the visitor's instance base / depth base. May set the visitor's error | 4.14 |
| `u3h_v_pose` | `void u3h_v_pose(void *visitor, uint32_t frame, uint8_t *anim, uint32_t index)` | C export of G52's `u3x_v_pose`: appends a pose command (anim at a frame pattern, for instance `index`) | 4.13 |
| `u3h_v_ice` | `void u3h_v_ice(void *visitor, uint16_t type, uint32_t index, uint64_t p1, uint64_t p2, uint64_t p3)` | C export of G52's `u3x_v_ice`: appends an ice command of type 0xc (play an ice copy) or 0xe (ice into a copy) for instance `index` with three pointers; error 10 when one of them is not 16-byte aligned | 4.13, 4.14 |
| `u3h_v_bind` | `void u3h_v_bind(void *visitor, uint32_t index)` | C export of `u3x_v_bind`: appends the "empty slot" command for instance `index` | 4.13 |
| `u3h_v_blend` | `void u3h_v_blend(void *visitor, uint32_t factor, uint32_t a, uint32_t b, uint32_t into, uint32_t mode)` | C export of the command visitor's blend command (`u3x_v_blend_cmd`, G52): blend instances `a` and `b` into `into` with a factor pattern | 4.13 |
| `u3h_v_error` | `void u3h_v_error(void *visitor, int code)` | C export of `u3x_v_error` (G50 4.6): records the first error of a visitor | 4.14 |

Each `u3h_v_*` call may set the visitor's error (for example error 7, command buffer full); this group only tests
the error word where section 4 says so.

## 3. Game facts

### 3.1 Game functions called

| Image offset | Arguments | Result | Meaning | Used in |
|---|---|---|---|---|
| 0x6844f0 | (generator, pointer to an s32 index, pointer to a u32 blend pattern), the generator's time in the first float register | unused | picks the two snapshots around that time: writes the index of the older one and the blend factor between them; may leave both words untouched when there is no pair | 4.12 |
| 0xdc4240 | (anim, skeleton id (u32, zero-extended), pointer to a u64 "joint parameters", pointer to a u64 "valid bits"), a frame pattern in the first float register | u8 in the low byte of the result: non-zero = done | retargets an anim made for another skeleton: gives the two pointers an ice command plays | 4.13 |
| virtual, slot at vtable + 0x190 of the generator's handle object | (object) | s32 | the object's skeleton id | 4.11 |

`u3x_float_call` and `u3x_call_float_arg` call whatever address they are given; the addresses their callers use
are in those callers' documents.

### 3.2 The generator and its ice copies

Generator (the net anim command generator of a net character or NPC); fields this group uses:

| Offset | Type | Meaning |
|---|---|---|
| +0x08 | 2 x 0x20 bytes | the two **ice copies** (below) |
| +0x48 | pointer | handle: pointer to an object slot |
| +0x50 | u32 | handle: object id |
| +0x60 | u32 | net anim row (the character class, 3.5) |
| +0x68 | 8 pointers | the row's eight art groups (filled elsewhere) |
| +0xb0 | 6 x 0x3f0 bytes | snapshots (G50 3.4); u32 float pattern at snapshot +4 = the snapshot's time |
| +0x2178 | u32 | pose anim: column of its art group in the row |
| +0x217c | u32 | pose anim: name id |
| +0x2180 | u32 | float pattern: the generator's current time |

Ice copy (0x20 bytes): three u64 pointers at +0 (P0), +8 (P1), +0x10 (P2), which an ice command carries; u8 at
+0x18 = "pending" (1 = the copy has been asked for but is not finished, 0 = finished); u32 float pattern at +0x1c
= the time of the snapshot the copy holds. Bytes +0x19 to +0x1b are not used here.

### 3.3 Art groups, anim tables, packages

- Art group: pointer at +0x30 → an anim block; pointer at anim block +0x10 → the group's **anim table** (either
  may be null).
- Anim table: u32 at +0 = entry count; pointer at +8 = entry array. Entry (16 bytes): u32 name id at +0, pointer
  at +8 = **slot**; the anim is the pointer stored in the slot. The array is sorted by name id.
- Anim: s32 at +0x40 = the skeleton id it was made for.
- Level manager: pointer at image 0x1b24b80 + 0x38. Pointer at manager +0x38 → 0x15e (350) package slots of 0x2c0
  bytes. Slot: u32 at +0x20 non-zero = in use; pointer at +0x18 = package. Package: s32 at +0x2c = size n of its
  art group hash table; pointer at +0x30 = the table of n 16-byte entries {u32 name id at +0 (0 = empty), pointer
  to the art group at +8}.

### 3.4 Streaming records

s32 at image 0x1d46088 = record count; record pointers (8 bytes each) from image 0x1d46090; at most 0x32 (50) are
looked at. Record: u32 at +0 = number of id lists; pointer at +0x30 = its definition. Definition: pointer at +0 =
array of u32 list lengths; pointer at +8 = array of pointers, one per list, each to an array of u32 anim ids.

### 3.5 The net anim packages (the old table `anim_packages`)

A generator's row and a column name one animation package (art group). The column is a value of the wire format
(the 3-bit "type" of an anim in the stream, G50 3.3, and the column word at generator +0x2178), the row is the
character class the game stores at generator +0x60. PS3 1.19 holds the names as a table of 6 rows x 8 columns of
text pointers (19 cells used, the rest empty); the PS4 game has no such table. It still holds six of the nine
different names as zero-terminated ASCII texts, each referenced once by its own code. The new code must not hold
the names as literals where the image has them: it reads the text at run time (pointer = `base` + offset) and
passes it to `u3h_find_art_group_name`.

| Name here | Where the PS4 image holds the text | Meaning |
|---|---|---|
| N1 | image 0x145b8ff (5 characters + 0) | the base player package |
| N2 | image 0x145f915 (22 + 0; the same text G39 3 names) | multiplayer player anims |
| N3 | image 0x145f953 (15 + 0) | multiplayer soldier anims |
| N4 | image 0x145f92c (17 + 0) | shield anims |
| N5 | image 0x145b90e (15 + 0) | the base NPC package |
| N6 | image 0x145f93e (20 + 0) | multiplayer NPC anims |
| C1, C2, C3 | **not in the PS4 image** (no text containing the word occurs in the executable) | three civilian packages: male, female, child. Open question 1 |

Which name stands in which cell (a fact of the wire format: a sender's column number must mean the same package
on every console):

| Row | Meaning | Columns 0, 1, 2, ... (all further columns empty) |
|---|---|---|
| 0 | player characters | N1, N2, N3, N4 |
| 1 | NPC, class 1 | N5, N3, N2, N6, N4 |
| 2 | NPC, class 2 | N5, N3, N4, N6 |
| 3 | NPC, class 3 (civilian male) | N5, N3, C1, N6 |
| 4 | civilian female | C2 |
| 5 | civilian child | C3 |

Row 0 is the same list the Player object's own eight tables are filled from (the C table
`player_net_anim_names`, group G61's `u3x_player_net_anims`); that table is not part of this group.

An empty cell means "no package": the lookup is still made with an empty name and gives null (4.10).

### 3.6 Command buffer, stub block, switches and counters

- Command buffer (G52 describes it): this group reads and restores two u32 words of it: +0xc = write position,
  +0x10 = highest instance index.
- Value stub block of kind 118 (G50 3.1): nine saved 64-bit registers in the order r11, r10, r9, r8, rdi, rsi,
  rdx, rcx, rax (elements 0 to 8). Element 4 (rdi) = the generator; element 5 (rsi) = the command buffer; element
  7 (rcx, low 32 bits) = the processing group; element 3 (r8, low 32 bits) = the first instance index. Element 6
  (rdx, the skeleton argument PS4 added) and element 8 are not used. Result 1 = this group produced the commands,
  the game's own function is skipped; result 0 = the game's own function runs.
- `ec[0]` bit 9 (0x200): decoder uses off.
- Counters (u32, all must be written exactly as stated; some are also written by unrelated code): `ec[84]`
  CreateAnimCmds runs; `ec[85]` runs that fell back to the game's function; `ec[81]` decode failures other than
  "empty" (shared with G50); `ec[103]` pose anims that went through the retarget call (G52 counts in the same
  word); `ec[89]` bit 31 = a net anim package was asked for outside the 6 x 8 range.
- Visitor error codes (G50 3.2) set here: 9 = no free ice copy. Code 12 = "empty stream" (set by the decoder) is
  the only failure that does not count in `ec[81]`.

## 4. Behaviour

### 4.1 `u3x_float_call(fn, a0, a1, a2, a3)`: call a game function that returns a float

Calls the code at address `fn` with the System V integer argument registers loaded as: rdi = `a0`, rsi = `a1`,
rdx = `a2`, rcx = `a3`. The fifth and sixth integer argument registers (r8, r9) and all float argument registers
are not loaded (their content is whatever they held). The stack pointer is a multiple of 16 at the call
instruction (the shim saves the stack pointer, rounds it down to a multiple of 16, calls, restores it). After the
call the result is the low 32 bits of the first float register (xmm0) moved unchanged into an integer: the bit
pattern of the single-precision value the game function returned. No conversion, no rounding, no NaN handling;
the integer result register of the callee is ignored. `fn` is not checked.

This cannot be written with `game_call` (the helper is built without SSE, so a float return value would be read
from the x87 stack) nor with `u3x_k26_call` (which returns the integer register): it is a few lines of assembly
that must declare every caller-saved register (integer and xmm) as changed.

### 4.2 `u3x_call_float_arg(fn, f0, a0, a1, a2, a3)`: call a game function that takes a float

Calls the code at `fn` with the low 32 bits of the first float register (xmm0) = the pattern `f0` (its upper
bits zero), rdi = `a0`, rsi = `a1`, rdx = `a2`, rcx = `a3`, the stack pointer a multiple of 16 at the call.
Returns the callee's full 64-bit integer result register (rax); callers that want a boolean take its low byte.
Other argument registers are not part of the contract (in the old build r8 happened to hold `f0` zero-extended;
no callee reads it). Building this on the shared `u3x_k26_call` block (integer arguments 0 to 3, float register 0
= `f0`, everything else 0, one float register in use) gives the callee the same view and is allowed.

### 4.3 `u3x_float_equal(a, b)`: equality of two float patterns

1 when neither pattern is a NaN and the two values are numerically equal, else 0. So +0.0 and -0.0 are equal, a
NaN equals nothing (not even the same pattern), and otherwise the patterns must be identical.

### 4.4 `u3x_bits_init(reader, buf, nbytes)`

The bit reader record (0x20 bytes, 8-byte aligned, provided by the caller; its first 16 bytes are laid out as
the game's own stream reader record, so that the game's frame and vector readers can read from it):

| Offset | Type | Meaning | Set by init to |
|---|---|---|---|
| +0 | pointer | the stream as u32 words | `buf` |
| +8 | u32 | bit position | 0 |
| +0xc | u32 | capacity in bits, for the game's readers | 2400 (the 300-byte stream buffer) whatever `nbytes` is |
| +0x10 | u32 | (always zero) | 0 |
| +0x14 | u32 | limit: the stream's length in bits | `nbytes` x 8 (32-bit arithmetic) |
| +0x18 | u32 | error word | 0 |

Bytes +0x1c to +0x1f are not written. `buf` is not checked.

### 4.5 `u3x_bits_read(reader, n)`

The stream is a sequence of 32-bit words in memory order (little-endian words); bit k of the stream is bit
(31 - k mod 32) of word k / 32, i.e. the first bit is the most significant bit of the first word. A field of n
bits is an unsigned number whose most significant bit comes first; it may run over two words.

In this order:
1. `n` = 0: return 0. Nothing is read or changed, the error word is neither tested nor set.
2. If the error word is non-zero, or `n` > 32, or position + `n` > limit (the sum in 32-bit unsigned arithmetic):
   set the error word to 1, return 0, leave the position.
3. Otherwise return the n-bit field at the position and add `n` to the position. Only the word(s) the field
   lies in are read: the second word only when the field crosses a word boundary.

The capacity word is not tested here. The limit is at most 255 x 8 bits in practice (the byte count is a u8), so
every word read lies inside the 300-byte buffer.

### 4.6 `u3x_art_group_sid(base, sid)`: a loaded art group by name id

Null when `sid` is 0, when the level manager pointer is null or when its slot array pointer is null (3.3).
Otherwise the 350 package slots are visited in order. A slot is skipped when it is not in use, when its package
pointer is null, when the package's table size n is 0 or negative or its table pointer is null. In a package the
hash table is probed from entry `sid` mod n (unsigned) upwards, wrapping to entry 0 after the last, at most once
round: an entry with name id 0 ends the search in this package; an entry with name id `sid` is the hit. A hit
whose art group pointer is not null is the result (the first such package wins); a hit with a null pointer, an
empty entry or a full round without hit moves on to the next slot. Null when no slot gives one.

(The old code read some entries of a full table twice; only the result is observable. `u3x_find_art_group(const
uint8_t *slots, uint32_t id)` of G61 does the same search from the slot array on and may be used.)

### 4.7 `u3x_anim_by_index(group, index)`

T = the group's anim table (3.3): null group, null anim block or null table → null. If `index`, taken as
unsigned, is not below the entry count → null (so negative indexes give null). Null entry array → null. S = the
slot pointer of entry `index`; null S → null; else the pointer stored in S (may itself be null).

### 4.8 `u3x_anim_by_sid(group, sid)`

T as in 4.7 (null → null). k = `u3x_tab_find(T, sid)`; k < 0 → null. S = the slot pointer of entry k; null S →
null; else the pointer stored in S.

### 4.9 `u3x_stream_record(base, id)`

For i from 0 while i < the record count (signed; a count of 0 or less gives null) and i < 50: R = record pointer
i; skip a null R. L = R's number of lists; D = R's definition; skip the record when D is null or when either of
D's two array pointers is null. For list u from 0 while u < L: c = length u; **c = 0 ends this record** (later
lists are not looked at); a null id-array pointer for list u skips to the next list; if any of the c ids equals
`id`, the result is R. Null when no record lists `id`. Reads only.

### 4.10 `u3x_net_art_group(base, row, col)`

`row` > 5 or `col` > 7 (unsigned): set bit 31 of `ec[89]` (the other bits stay) and return null. Otherwise
return `u3h_find_art_group_name(base, name)` with the name of cell (row, col) of 3.5; for an empty cell the call
is made with a pointer to an empty text (one zero byte), which gives null. The texts N1 to N6 are passed as
pointers into the game image (3.5); C1 to C3: open question 1.

### 4.11 `u3x_generator_skeleton(generator)`

O = the object named by the handle at generator +0x48 / +0x50: null when the slot pointer is null, the id is 0,
the slot holds null or the u32 at object +0x7c differs from the id; otherwise the object in the slot. No object →
0. Otherwise the s32 result of the object's virtual function at vtable +0x190, called with the object as its only
argument. The generator is not null-checked.

### 4.12 `u3x_generator_pick(base, generator, blend)`

Sets a local s32 index to -1 and `*blend` to 0x3f800000 (1.0), then calls game function 0x6844f0 through
`u3x_call_float_arg` with the float register = the u32 at generator +0x2180 and the integer arguments
(generator, address of the index, `blend`, 0). Returns the index (the game's result register is ignored).

### 4.13 `u3x_anim_commands(base, regs)`: CreateAnimCmds

G = element 4, CB = element 5, GROUP = low 32 bits of element 7, FIRST = low 32 bits of element 3 (3.6).

1. If `ec[0]` bit 9 is set: return 0 (nothing else).
2. If G or CB is null, or GROUP > 1: return 0.
3. `ec[84]` += 1.
4. IDX = `u3h_pick_snapshot(base, G, &BLEND)`.
5. Remember the two command buffer words (CB +0xc and +0x10) and the 0x40 bytes of the two ice copies (G +8 ..
   G +0x47).
6. Prepare a visitor record (G50 3.2; 0x160 bytes, 16-byte aligned, a local): base = `base`, kind = 1, error = 0,
   commands = CB, entries = null, channel = 0. Its other fields need no value (the decoder and the visitors set
   what they use).
7. **No snapshot pair** (IDX = -1): the generator's pose anim plays.
   1. AG = `u3h_net_art_group(base, u32 at G +0x60, u32 at G +0x2178)`.
   2. A = `u3h_anim_by_sid(AG, u32 at G +0x217c)` when AG is not null, else null (no call).
   3. SK = `u3h_skeleton(G)` (always called, after the lookups).
   4. If A is not null and the s32 at A +0x40 equals SK: `u3h_v_pose(visitor, 0, A, FIRST)` (frame pattern 0 =
      0.0).
   5. If A is not null and the skeletons differ: two local u64 words O1 = O2 = 0;
      `u3h_call_f32(base + 0xdc4240, 0, A, SK as u32 zero-extended, &O1, &O2)`. If the low byte of the result is
      non-zero: `ec[103]` += 1, then `u3h_v_ice(visitor, 0xc, FIRST, O1, 0, O2)`.
   6. If A is null, or the retarget call answered 0: `u3h_v_bind(visitor, FIRST)`.
8. **A snapshot pair** (any other IDX; not range-checked): S0 = G + 0xb0 + IDX x 0x3f0 (IDX signed), S1 = S0 +
   0x3f0.
   1. Snapshot step (4.14) for S0 with other = S1, instance FIRST, depth argument 0.
   2. If the visitor's error is still 0: snapshot step for S1 with other = S0, instance FIRST + 1, depth
      argument 1.
   3. If the error is still 0: `u3h_v_blend(visitor, BLEND, FIRST, FIRST + 1, FIRST, 1)`.
9. If the visitor's error is non-zero (from either branch): write the two remembered words back to CB, write the
   remembered 0x40 bytes back to G +8, `ec[85]` += 1, and if the error is not 12 `ec[81]` += 1; return 0.
10. Otherwise return 1.

### 4.14 The snapshot step (visitor, G, S, other, instance I, depth argument D, GROUP)

T = the float pattern at S +4. E = the ice copy that holds T: the first copy (G +8) if `u3x_float_equal`(its time,
T), else the second (G +0x28) if `u3x_float_equal`(its time, T), else none.

1. E exists and its pending byte is 0 (a finished copy): `u3h_v_ice(visitor, 0xc, I, E.P1, E.P2, E.P0)`. Done
   (no decode).
2. Otherwise `u3h_decode(visitor, S, G, I as s32, D)`. If the visitor's error is now non-zero: done.
3. If there is no E: take the copy that holds neither T nor the time of `other` (pattern at other +4), by
   `u3x_float_equal` on the copies' times: the first copy unless its time equals one of the two, then the second
   unless its time equals one of the two. If both are taken: `u3h_v_error(visitor, 9)`; done. Otherwise E = that
   copy; set its pending byte to 1 and its time word to T (the pattern, copied), in that order.
4. If there was an E (pending, from the start) and GROUP = 1: set its pending byte to 0. (With GROUP = 0 it stays
   pending.)
5. `u3h_v_ice(visitor, 0xe, I, E.P1, E.P2, E.P0)`.

The pointers P0 to P2 of a copy are never written here. Note the argument order of the ice command: P1, P2, then
P0.

## 5. Test scenarios

Everything except the two call shims can be tested on the host with fake memory and call-recording stand-ins for
the `u3h_` exports, `u3x_tab_find` and the two game functions; old against new: same memory in, compare results,
memory afterwards (generator, command buffer words, reader record, `ec[]`) and the order and arguments of the
calls. The shims are tested on the host against small stand-in callees that record the argument registers and
the stack alignment and return known values; their real use (game functions 0x689d20, 0x6844f0, 0xdc4240,
0xfd3c60, 0xc1ccc0) needs the live game. The texts of 3.5 need a fake image with texts at the six offsets on the
host and the live game for the real ones.

1. `u3x_float_call`: callee returns patterns 0, 0x3f800000, 0x80000000, a NaN pattern, 0xffffffff in xmm0 with
   a different value in rax: the xmm0 pattern comes back unchanged. Arguments arrive in rdi, rsi, rdx, rcx; stack
   16-byte aligned at the call, also when the caller's stack is 8 off.
2. `u3x_call_float_arg`: callee sees `f0` in xmm0 (several patterns, including NaN and -0.0), the four integer
   arguments, aligned stack; a 64-bit result with bits above 32 set comes back whole.
3. `u3x_float_equal`: equal patterns; +0.0 against -0.0 (1); the same NaN pattern twice (0); NaN against a number
   (0); two different numbers (0); denormals.
4. `u3x_bits_init`: record bytes after init for `nbytes` 0, 1, 255, 300 and a value whose x 8 wraps; bytes +0x1c
   .. +0x1f untouched.
5. `u3x_bits_read`: n = 0 (also with the error word set: 0, no change); n = 1, 5, 12, 13, 32 at positions 0, 27,
   31, 32, 63; fields crossing a word boundary; n = 32 at position 0 and at position 1; n = 33 (error); a read
   that ends exactly at the limit (ok) and one bit past it (error, position kept); every read after an error
   returns 0 and keeps the position; a non-crossing read at the last word must not touch the following word
   (guard page or poisoned word). Random sequences against a plain bit-by-bit reference.
6. `u3x_art_group_sid`: sid 0; null manager; null slot array; slots not in use, null package, n = 0, n < 0, null
   table; hit at the home entry; hit after probing; hit after wrapping past the end; empty entry before the hit
   (no hit in that package, a later package has it); full table without hit; hit with null group, a later
   package with a real one; two packages with the group (first wins); the hit in slot 349.
7. `u3x_anim_by_index`: null group / block / table; index = count - 1, count, -1; null entry array; null slot;
   slot holding null.
8. `u3x_anim_by_sid`: null table (no call of the search); search answers -1; answers k (slot null, slot holding
   an anim).
9. `u3x_stream_record`: count 0, negative, 3, 60 (only 50 looked at: the id only in record 55 → null); null
   record pointers; null definition, null arrays; id in list 0, in a later list, behind a zero-length list (not
   found in that record), list pointer null; two records with the id (first wins).
10. `u3x_net_art_group`: row 6 or col 8 (bit 31 of `ec[89]` set, other bits kept, no lookup call); every cell of
    rows 0 to 2: the recorded name pointer equals image + the offset of 3.5, or points to an empty text for an
    empty cell; the result of the stand-in is passed back.
11. `u3x_generator_skeleton`: stale handle (id differs), id 0, null slot pointer, empty slot → 0 without call;
    live handle → the virtual's result.
12. `u3x_generator_pick`: stand-in for 0x6844f0 that writes nothing (result -1, blend 1.0); that writes index 2
    and a blend pattern; the float register carries the word at +0x2180; fourth argument 0.
13. `u3x_anim_commands`, gates: switch bit set (no counter, result 0); null G; null CB; GROUP 2 (no counter).
14. No pair, AG null: no `u3h_anim_by_sid` call, skeleton still asked, bind command; result 1.
15. No pair, A with the generator's skeleton: pose command with frame 0; A with another skeleton and retarget
    answering 1 (`ec[103]`, ice 0xc with O1, 0, O2) and answering 0 (bind); a result whose low byte is 0 but
    higher bits set counts as 0.
16. Pair, both ice copies finished and holding the two times: two 0xc commands (FIRST, FIRST + 1) then the blend;
    no decode; generator unchanged.
17. Pair, no copy holds either time: two decodes (arguments I / D = FIRST / 0 and FIRST + 1 / 1), the first takes
    copy 0, the second copy 1 (copy 0 now holds the other time), both pending with their times; two 0xe commands;
    blend.
18. Pair, a copy holds S0's time but is pending: decode, then with GROUP 1 pending cleared, with GROUP 0 kept;
    0xe command.
19. Pair, S0 has no copy: copy 0 holds S1's time (copy 1 is taken over, also when it was pending for a third
    time); copy 0 holds -0.0 and S1's time is +0.0 (counts as held); both copies hold S1's time (error 9 →
    rollback of step 9).
20. Times that are NaN: a NaN snapshot time never matches a copy; a NaN copy time is always free.
21. Rollback: the decode stand-in sets error 3 on the first step (second step and blend not run; CB words and
    the 0x40 ice bytes restored after the stand-ins changed them; `ec[85]` and `ec[81]` + 1; result 0); error 12
    (`ec[85]` + 1, `ec[81]` unchanged); error set by the second step after the first step changed a copy; error
    set by the blend stand-in; error set by a visitor stand-in in the no-pair branch.
22. Live game: a remote player and a co-op NPC animate (commands produced, `ec[84]` rising, `ec[85]` flat);
    switching bit 9 of `ec[0]` gives the game's own behaviour.

## 6. Notes

**Strays covered here** (in no work group of the inventory, or only in its "trivial" list, but holding logic of
the old port or needed by this group): `u3x_float_equal` and its private form; the two private ice-copy choosers
(4.14); the private "anim table of a group" read (3.3; the exported `u3x_art_tab` of G49 is the same read);
`u3x_net_art_group` with the table `anim_packages` (3.5, 4.10); `u3x_generator_skeleton` and
`u3x_generator_pick` (4.11, 4.12; G50 names G51 as their home). Not covered, they stay as they are:
`u3x_bits_frame`, `u3x_bits_vector`, `u3x_frame_fresh` (one call of a game function or of `u3x_frame_identity`
each; described in G50 2.2), the private `call_float1` (G47), `u3x_handle_obj`.

**Tables.** One: `anim_packages` (6 x 8 texts, 19 cells used, nine different names). Resolved as 3.5: six names
are read from the PS4 image at run time; the cell arrangement is stated as a fact of the wire format; three
names are open (question 1). The constants 2400 (4.4), 350 / 0x2c0 (3.3) and 50 (3.4) are sizes of game
structures, not tables.

**What may stay as it is.** The two call shims (4.1, 4.2) are the helper's own glue between two calling
conventions, not a port of game code; they are described so that they can be rewritten, but keeping their
assembly and only exporting the first under the name `u3x_float_call` is equally correct. The C wrappers of
section 1 stay.

**Behaviour to keep although it looks like a slip or is unreachable:**
- 4.5: a 0-bit read succeeds even after an error and never sets one.
- 4.4: the capacity is always 2400 bits, also for a caller's buffer of another size.
- 4.9: a zero-length list ends a record's search although later lists may be filled.
- 4.13 step 7: the skeleton is asked even when there is no anim; a pose or ice command that fails (error set by
  the visitor) still counts as "produced" (no bind command follows) and leads to the rollback of step 9.
- 4.13: the rollback restores the ice copies even on the no-pair branch, which never changes them.
- 4.13 step 8: IDX is not range-checked; IDX + 1 is assumed to be a valid snapshot.
- 4.14 step 3: a copy that is "pending" for a third, older time is taken over without regard to its pending
  byte; P0 to P2 are reused as they are.
- 4.14 step 1 / 5: the snapshot step's "used a finished copy" answer has no user.
- `u3x_anim_by_sid` is reached only with wide-id streams (never sent by our own packer) and on the no-pair branch
  of 4.13.
- `u3x_call_float_arg` is given a fourth integer argument 0 by callers whose callee takes three.

**Open questions**
1. **Three package names (C1, C2, C3 of 3.5).** Size: three texts (cells row 3 column 2, row 4 column 0, row 5
   column 0). Meaning: the animation packages of civilian men, women and children. The PS4 executable holds none
   of them and no rule generates them; they are known only from the PS3 1.19 table (image 0x10f8100 there). Two
   ways out, for the owner to choose: (a) treat them as file names of the game's data (as the package and stage
   names G39 already writes out) and let the integrator supply the three texts; (b) leave the three cells empty,
   which changes behaviour only if a generator of row 3, 4 or 5 ever exists in multiplayer (row 3 would lose one
   of its four packages, rows 4 and 5 all): whether co-op levels spawn such characters has not been checked.
   Until decided the implementer treats the three cells through one small private function "civilian name of
   cell" that the integrator fills in.
2. **Rows of 3.5 hold image offsets.** The arrangement table of 3.5 becomes, in the new code, a small table of
   image offsets (not of game values); if the owner prefers no table at all, rows 0 to 2 can be written as
   three short rules. Either form gives the same lookups.
3. **Game function 0x6844f0 with no pair**: whether it writes -1 itself or leaves the index untouched is not
   known; 4.12 presets -1 and 1.0, which covers both.

## Clarifications (tester, 2026-11-14; differential test reimpl2\G51\test_g51.cpp, old v7.212 against the new part)

- **The time of a snapshot in the snapshot step (4.14) is read once, before the decode call.** The old code read the
  pattern at S +4 again after `u3h_decode` (for the choice of the free copy and for the time it stores into that
  copy); the new code uses the value read at the start. Both give the same result whenever the decode call does not
  change S +4. It cannot change it: `u3h_decode` hands the decoder only the stream pointer, byte count and the u32 at
  snapshot +0x308 (G50 2.2), and a command-kind visitor (kind 1, entries null) writes no snapshot bytes. The same
  holds for a change of the OTHER snapshot's time: equal results on both sides. Seen only with a test stand-in
  that rewrites S +4 inside the decode call; not a behaviour of the game.
- **Entry offsets of 2^28 and more.** For an entry number of 0x10000000 or above (4.6 probe position, 4.7 index,
  4.8 search answer) the old code formed the byte offset entry x 16 in 32 bits, so it wrapped; the new code does not
  wrap. No real anim table or art group hash table comes near that size (an index this large needs a count above
  2^28), so this changes no reachable behaviour.
- **4.8 with a null entry array and a search answer k >= 0**: the slot pointer is read at address k x 16 + 8 (no
  null test of the entry array) on both sides; the spec's "null entry array" test belongs to 4.7 only.
- **4.13 step 6**: both sides write exactly the visitor fields base, entries, commands, channel, kind and error
  before the first visitor call, in the pair and the no-pair branch.
- **4.2, register r8**: the old build handed `f0` zero-extended in r8 in every run; the new build does not. Not part
  of the contract (stated in 4.2); no caller's game function reads it.
- **Open question 1** (civilian names): the new code reads cells (3, 2), (4, 0), (5, 0) through `u3h_civ_package(0)`,
  `(1)`, `(2)`; with a stand-in serving the old table's texts the text reaching `u3h_find_art_group_name` is equal in
  every cell. The real texts are the integrator's.
- **Open question 2**: rows 0 to 2 are written as rules in the new code; every cell (6 x 8, plus out-of-range rows and
  columns) gives the same text as the old table and the same `ec[89]` effect.
- **Open question 3** (0x6844f0 writing -1 itself): cannot be settled on the host; both sides preset -1 and 1.0, so
  both behaviours of the game function give the same result. Needs the live game to know.
