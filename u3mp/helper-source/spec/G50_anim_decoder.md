# G50. Anim state decoder and its entry points

Behaviour description for a reimplementation (reimpl2 pipeline, describer's document). It says what the functions
do, not how the old ones were written. Offsets are hexadecimal when prefixed with 0x. "Image offset X" means the
address `base + X`, where `base` is the game image base. All multi-byte values in memory are little-endian; "u8 /
u16 / u32 / s32 / u64" are integers of that width, "pointer" is 8 bytes. Game functions use the System V AMD64
convention with integer arguments unless a float register is named.

Related documents: `G48_player_state.md` (the player state message that carries the stream; 3.7 there),
`G49_anim_packer.md` (the sender's side: the two documents must agree on section 3.3 here), G51 (bit reader,
lookups, float-argument call, snapshot pick) and G52 (the visitors). Functions of G51 and G52 are named here with
signature and effect only.

**Float.** The only float work of this group is the de-quantisation of phases and blend weights (4.5). It is
single precision on 32-bit patterns: `f_from_int` then `f_mul` (CONVENTIONS.h), one rounding each. The old build
already used these single-precision instructions here; no step of this group ran on the x87 unit, so results must
match bit for bit. Floats are never passed by value: a phase or weight is a u32 pattern everywhere.

## 1. Purpose and callers

PS3 1.19 sends each character's animation state as a compact bit stream inside the player state message (and the
NPC state message). The PS4 game (built from PS3 1.10) neither writes nor reads it. This group is the reading side
plus three hooks:

- the **decoder**: walks one stream and reports what it finds to a "visitor" (state list, command list or locator
  sampler: the visitors are G52);
- **apply**: when the game stores a received snapshot, decode it into the snapshot's anim entries;
- **blended locator**: the game's "get locator of a channel" for a net animated character, answered from the two
  current snapshots;
- **NPC bits**: when the game sends an NPC's state message, pack the NPC's anim state into it (the packer is G49).

| Function | Called from | When |
|---|---|---|
| `u3x_decode_anim` | C `dec119` (behind the export `u3h_decode`, and the C loopback check `loop119`) | each decode of one snapshot or message |
| `u3x_apply_decode` | C `apply_dec119`, hook kind 117: a register stub inside the game's snapshot apply (function 0x683670, at image 0x683c61, after the game cleared the snapshot's state). The dispatcher calls it only with non-null `base` and block; the replaced instruction is run again by the stub | each received player / NPC state |
| `u3x_blended_locator` | C `gloc119`, hook kind 119: a value stub at the entry of game function 0x684cf0 (out, generator, channel). The dispatcher returns this function's result; 0 = the game's own body runs | whenever the game asks for a channel locator |
| `u3x_npc_bits` | C `npc_bits119`, hook kind 121: a register stub in the game's NPC state sender (function 0x6ae2a0, at image 0x6ae57f). The replaced instruction (store 0 to message +0x144) is NOT run again: this function must store that word | each NPC state message built |

Everything runs on the game's main thread; no locking.

## 2. Interface

### 2.1 Functions to deliver (names and signatures unchanged; all `extern "C"`)

| Name | Signature | Meaning |
|---|---|---|
| `u3x_decode_anim` | `void u3x_decode_anim(void *visitor, uint8_t *bits, uint32_t nbytes, uint32_t flags, uint8_t **tabs, int32_t a4, int32_t a5, int32_t a6, int32_t a7)` | decodes the stream of `nbytes` bytes at `bits` (3.3) and drives `visitor` (3.2). `flags`: the message's flag word (bit 0 = a camera block follows the frame). `tabs`: the generator's 8 art group pointers. `a4`, `a5`: stored in the visitor (instance base, depth base). `a6`: index of the default anim. `a7`: skeleton value handed to the marker visitor (4.5) |
| `u3x_apply_decode` | `void u3x_apply_decode(uintptr_t base, const uint64_t *regs)` | 4.1. `regs`: register stub block (3.1) |
| `u3x_blended_locator` | `int u3x_blended_locator(uintptr_t base, uint64_t *regs)` | 4.2. `regs`: value stub block (3.1). Result 1 = answered (the stub returns `regs[8]` to the game), 0 = the game's own function runs |
| `u3x_npc_bits` | `void u3x_npc_bits(uintptr_t base, const uint64_t *regs)` | 4.3. `regs`: register stub block |
| `u3x_v_error` | `void u3x_v_error(void *visitor, int code)` | 4.6 (stray, see 6) |
| `u3x_v_anim` | `void u3x_v_anim(void *visitor, uint32_t phase, uint8_t *anim, uint32_t depth)` | 4.6 (stray) |
| `u3x_v_sub3` | `void u3x_v_sub3(void *visitor, uint32_t depth)` | 4.6 (stray) |

The file-private piece "copy a visitor's first locator as {position, rotation, zero}" (old `take_locator`) is part
of 4.2 and needs no name of its own.

The C wrappers stay as they are (`dec119`, `apply_dec119`, `gloc119`, `npc_bits119`, `verr119`, `vanim119`,
`vsub3_119` each pass their arguments through unchanged).

One part; about 240 old lines. If two implementers are wanted: part A = `u3x_decode_anim` (4.4, 4.5); part B =
the three hooks and the three small visitor functions (4.1 to 4.3, 4.6). They share only the visitor record (3.2).

### 2.2 Functions of other groups and of the C part that are called

| Name | Signature | Effect |
|---|---|---|
| `u3h_ext` | CONVENTIONS.h | the helper's state; the diagnostic words `ec[]` are u32 at `u3h_ext() + 0x4450` |
| `u3h_decode` | `void u3h_decode(void *visitor, uint8_t *snapshot, uint8_t *generator, int32_t a4, int32_t a5)` | C export: calls `u3x_decode_anim(visitor, pointer at snapshot +8, byte at snapshot +0, u32 at snapshot +0x308, generator + 0x68, a4, a5, s32 at generator +0x64, skeleton value of the generator)` |
| `u3h_pick_snapshot` | `int32_t u3h_pick_snapshot(uintptr_t base, uint8_t *generator, uint32_t *blend)` | C export of G51's snapshot pick: the index of the older of the two snapshots to blend at the generator's time (-1 = none) and, in `*blend`, the blend factor (float pattern) |
| `u3h_call_f32` | `uint64_t u3h_call_f32(uintptr_t fn, uint32_t f0, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)` | C export of G51's float-argument call: calls `fn(a0, a1, a2, a3)` with the float pattern `f0` in the first float register; returns the integer result |
| `u3h_anim_pack` | `uint32_t u3h_anim_pack(uintptr_t base, uint8_t *msg, uint8_t *stream, uint8_t *actor, uint8_t **tabs)` | C export of G49's packer (with its two extra arguments 0 and 2): writes the anim state of `actor` into the bit stream block `stream`; returns the number of bits written |
| `u3h_repl_error` | `uint32_t *u3h_repl_error(void)` | the packer's "ran out of room" word (non-zero after a pack that overflowed) |
| `u3h_repl_loop` | `void u3h_repl_loop(uintptr_t base, uint8_t *actor, uint8_t **tabs, uint8_t *msg, uint32_t bits)` | C part's loopback self-check (does nothing unless its switch is on) |
| `u3x_bits_init` | `void u3x_bits_init(void *reader, uint8_t *buf, uint32_t nbytes)` | G51: sets up a bit reader (3.4) over `buf`, limit `nbytes` × 8 bits |
| `u3x_bits_read` | `uint32_t u3x_bits_read(void *reader, uint32_t n)` | G51: next `n` bits (0 to 32) as an unsigned number; on a read past the limit sets the reader's error word, returns 0 and does not move; every later read returns 0 |
| `u3x_bits_frame` | `void u3x_bits_frame(uintptr_t base, void *reader, uint8_t *frame)` | reads one bound frame (0x60 bytes) with the game's own frame reader (image 0x7bf430); nothing when the error word is set; sets it when the read went past the limit. Stays as it is |
| `u3x_bits_vector` | `void u3x_bits_vector(uintptr_t base, void *reader, uint8_t *vec)` | same for one vector (0x10 bytes) with the game's vector reader (image 0x1274970). Stays as it is |
| `u3x_frame_fresh` | `void u3x_frame_fresh(uint8_t *frame)` | fills 0x60 bytes with the "unbound identity" frame. Stays as it is |
| `u3x_art_group_sid` | `uint8_t *u3x_art_group_sid(uintptr_t base, uint32_t sid)` | G51: the loaded art group with that name id, or null |
| `u3x_anim_by_index` | `uint8_t *u3x_anim_by_index(uint8_t *group, int32_t index)` | G51: the anim at that index of the group's table, or null |
| `u3x_anim_by_sid` | `uint8_t *u3x_anim_by_sid(uint8_t *group, uint32_t sid)` | G51: the anim with that name id in the group, or null |
| `u3x_stream_record` | `uint8_t *u3x_stream_record(uintptr_t base, uint32_t id)` | G51: the game's streaming record that lists anim id `id`, or null |
| `u3x_v_frames` | `uint32_t u3x_v_frames(const uint8_t *anim)` | frame count of an anim: u16 at +0x22 of the clip (pointer at anim +0x28), 0 without a clip. Stays as it is |
| `u3x_v_begin` | `void u3x_v_begin(void *visitor)` | G52: start of a decode (fresh frame, abort flag cleared, per-kind reset) |
| `u3x_v_blend` | `void u3x_v_blend(void *visitor, uint32_t weight, uint32_t a, uint32_t b, uint32_t into, uint32_t mode)` | G52: blend of slots a and b into slot `into` |
| `u3x_v_mark` | `void u3x_v_mark(void *visitor, uint32_t phase, uint8_t *anim, uint32_t depth, int32_t a7)` | G52: a marker anim |
| `u3x_v_sub0` | `void u3x_v_sub0(void *visitor, uint32_t x)` | G52 |
| `u3x_v_sub2` | `void u3x_v_sub2(void *visitor)` | marker: only for kind 2: the first one sets `active`, a later one sets `abort`. Stays as it is |
| `u3x_v_sub4` | `void u3x_v_sub4(void *visitor)` | G52 |
| `u3x_v_push` | `void u3x_v_push(void *visitor, uint32_t phase, uint8_t *anim)` | G52: state visitor's entry for an anim |
| `u3x_v_pose` | `void u3x_v_pose(void *visitor, uint32_t frame, uint8_t *anim, uint32_t index)` | G52: command visitor's pose command |
| `u3x_v_bind` | `void u3x_v_bind(void *visitor, uint32_t index)` | command visitor's "empty slot" command. Stays as it is |
| `u3x_v_sample` | `void u3x_v_sample(void *visitor, uint32_t phase, uint8_t *anim, uint32_t depth)` | G52: locator visitor's sample |

## 3. Game facts

### 3.1 Stub blocks

- Register stub (kinds 117, 121): fifteen saved 64-bit registers in the order r15, r14, r13, r12, rbx, r11, r10,
  r9, r8, rdi, rsi, rdx, rcx, rax, rbp (elements 0 to 14).
  - kind 117: element 4 (rbx) = the generator, element 3 (r12) = byte offset of the snapshot inside it.
  - kind 121: element 4 (rbx) = the NPC state message, element 0 (r15) = the NPC, element 13 (rax) = the NPC's
    generator ("table holder").
- Value stub (kind 119): nine saved registers in the order r11, r10, r9, r8, rdi, rsi, rdx, rcx, rax. Element 4
  (rdi) = output locator, element 5 (rsi) = the generator, element 6 (rdx, low 32 bits) = channel id, element 8 =
  the value the game function returns when this group answers.

### 3.2 The visitor record (0x160 bytes, 16-byte aligned; shared with G51, G52 and the C part)

| Offset | Type | Name here | Meaning |
|---|---|---|---|
| +0 | 0x60 bytes | frame | the bound frame read from the stream header |
| +0x60 | 4 × 0x30 bytes | locators | locator visitor: four locators, each {scale 16 bytes, rotation 16, translation 16} |
| +0x120 | u64 | base | image base |
| +0x128 | pointer | entries | state visitor: the snapshot's entry area |
| +0x130 | pointer | commands | command visitor: the command buffer |
| +0x138 | s32 | instance base | decoder argument `a4` |
| +0x13c | s32 | depth base | decoder argument `a5` |
| +0x140 | u32 | attach id | the 13-bit id of the stream header, or 0 |
| +0x144 | u32 | key | (G52) |
| +0x148 | u32 | channel | locator visitor: the channel to sample |
| +0x14c | u8 | abort | set by a visitor to end the decode |
| +0x14d | u8 | active | (G52) |
| +0x150 | s32 | kind | 0 state, 1 commands, 2 locator |
| +0x154 | s32 | error | 0, or the first error recorded (4.6) |

Error codes written by this group: 1 read past the end of the stream, 2 blend with fewer than two slots, 3 art
group missing, 4 anim missing, 5 "back" reference past the depth, 6 depth past the limit, 11 anim index past the
list, 12 empty stream.

### 3.3 The stream (the wire layout; must agree with G49)

Bit order: the stream is a sequence of bits; a field of w bits is an unsigned number sent most significant bit
first. In memory the stream is held as u32 words in host order: stream bit k is bit (31 − k mod 32) of word
k / 32. (`u3x_bits_read` implements exactly this; the group never touches the words itself.) Fields follow each
other with no padding. Length = `nbytes` × 8 bits; anything past it is unreadable.

**Header**

| # | Bits | Field | Use |
|---|---|---|---|
| 1 | 1 | W, "wide ids" | 1: anim ids are 32-bit name ids and every phase has 16 bits. The packer always sends 0 |
| 2 | 1 | attached | 1: field 3 follows |
| 3 | 13 | attach id (only if attached) | stored as the visitor's attach id; 0 when absent |
| 4 | game-coded | bound frame | read with `u3x_bits_frame` into the visitor's frame. Its width is decided by the game's frame coder (writer image 0x7befc0, reader 0x7bf430); the packer reserves at most 0x200 bits for it |
| 5 | game-coded | camera frame, only when `flags` bit 0 | same coding; read into a scratch frame first made fresh with `u3x_frame_fresh`; discarded |
| 6 | 8 | zoom byte, only when `flags` bit 0 | discarded |
| 7 | game-coded | aim vector, only when `flags` bit 0 | read with `u3x_bits_vector` into a 16-byte scratch (16-byte aligned); discarded. (As the packer writes it: 1 bit form; form 0 = 18, 17 and 18 bits; form 1 = three 32-bit float patterns.) |
| 8 | 5 | N | number of anim list entries (0 to 31) |
| 9 | 1 | direct | 1: the anim commands use the list entries in order; 0: each anim command carries an index |
| 10 | N × 12 (N × 32 if W) | ids | per entry: index of the anim in its art group's table (name id if W). 0xfff = "the default anim" |
| 11 | 1 | one type | 1: one type field follows, valid for all entries; 0: N type fields follow |
| 12 | type field(s) | types | a type field is 3 bits t; if t = 7 it is followed by 32 bits that REPLACE t (an art group name id). t below 7 = number of one of the generator's art groups |

**Commands** follow until the end command. Each starts with a 2-bit code; code 3 is followed by a 3-bit sub code.

| Code | Sub | Payload (in order) | Meaning |
|---|---|---|---|
| 2 | - | index (only if not direct), phase | anim |
| 3 | 5 | index (only if not direct), phase | marker anim |
| 0 | - | 1 bit order, 1 bit target, 2 bits mode, 8 bits weight | blend with weight |
| 1 | - | 1 bit order, 1 bit target, 2 bits mode | blend with weight 1.0 |
| 3 | 0 | 2 bits "back" | reference to an earlier slot |
| 3 | 1 | - | end of the stream |
| 3 | 2 | - | marker |
| 3 | 3 | - | empty slot |
| 3 | 4 | - | copy down |
| 3 | 6, 7 | - | nothing |

- index: 1 bit when N ≤ 2, 2 bits when N ≤ 4, 3 when N ≤ 8, 4 when N ≤ 16, else 5.
- phase: with W = 0 its width depends on the frame count F of the anim the receiver found for the entry
  (`u3x_v_frames`): F ≤ 8: 4 bits; F ≤ 64: 8 bits; else 16 bits. With W = 1: 16 bits.
- de-quantisation (the constants are the single-precision values nearest to 1/15, 1/255 and 1/65535; they are
  rule-generated, not a game table):

| Width | Value |
|---|---|
| 4 bits q | `f_mul(f_from_int(q), 0x3d888889)` (× 1/15) |
| 8 bits q | `f_mul(f_from_int(q), 0x3b808081)` (× 1/255) |
| 16 bits q | `f_mul(f_from_int(q), 0x37800080)` (× 1/65535) |

  A blend weight uses the 8-bit rule. Weight 1.0 is the pattern 0x3f800000.
- mode: wire 1 = blend mode 2, wire 2 = blend mode 6, wire 0 and 3 = blend mode 1.

### 3.4 Other layouts

- Bit reader record (G51's; 0x20 bytes, 8-byte aligned, a local of the decoder): this group reads two fields of it
  directly: u32 at +8 = bit position, u32 at +0x18 = error word (non-zero after a failed read).
- Generator: s32 at +0x64 default anim index; 8 art group pointers at +0x68; six snapshots of 0x3f0 bytes at
  +0xb0.
- Snapshot (0x3f0 bytes): u8 at +0 = byte count of its stream; pointer at +8 = the stream; +0x10 = entry area of
  0x214 bytes (22 entries of 0x18 bytes, then the u32 entry count at entry area +0x210 = snapshot +0x220); u32 at
  +0x308 = the message's flag word.
- NPC state message: 300 stream bytes at +0x18; u32 bit count at +0x144.
- Bit stream block for the packer (16 bytes, 16-byte aligned): pointer at +0 = byte buffer, u32 at +8 = bit
  position, u32 at +0xc = capacity in bits.
- Anim: u32 at +0x18 = its id; u32 at +0x24 = flags (bit 2, value 4 = streamed); pointer at +0x28 = clip.
- Streaming record: u32 at +0x20 = its current phase (float pattern).
- Locator as game function 0xfd3c60 takes it here (0x30 bytes, 16-byte aligned): position 16 bytes, rotation 16
  bytes (x, y, z, w), 16 zero bytes.

### 3.5 Game functions

| Image offset | Arguments | Result | Meaning |
|---|---|---|---|
| 0xfd3c60 | (out locator, locator a, locator b), blend factor in the first float register | - | blend of two locators. Called through `u3h_call_f32` with fourth integer argument 0 |
| 0xfd4c60 | (anim, u32 anim id, pointer to a u32 phase pattern) | anim pointer or null | the streamed anim's current chunk for that phase; may change the phase word. Plain integer call |

### 3.6 Switches and diagnostic words (`ec[]`, u32)

`ec[0]` bit 9 (0x200): decoder uses off. `ec[0]` bit 11 (0x800): no NPC anim bits.
Counters: `ec[80]` apply runs; `ec[81]` decode failures other than "empty"; `ec[82]` last error code other than
12; `ec[83]` entry count after the last apply; `ec[86]` locator runs; `ec[87]` locator failures; `ec[90]` NPC
packs; `ec[91]` last NPC bit count; `ec[92]` failed NPC packs; `ec[101]` bit position at the end of the last
decode; `ec[104]` empty streams. Several of these words are also written by unrelated code of the C part; they
must still be written exactly as stated (they are observable).

## 4. Behaviour

### 4.1 `u3x_apply_decode`

1. If `ec[0]` bit 9 is set: return.
2. Generator = element 4, offset = element 3 (as u64). If the generator is null or offset ≥ 6 × 0x3f0: return.
3. Snapshot = generator + 0xb0 + offset. Add 1 to `ec[80]`.
4. A visitor record with base = `base`, kind 0, error 0, entries = snapshot + 0x10, commands null, channel 0 (the
   other fields need no initial value); call `u3h_decode(visitor, snapshot, generator, 0, 0)`.
5. If the visitor's error is non-zero: if it is not 12 add 1 to `ec[81]`; set the 0x214 bytes at snapshot + 0x10
   to zero.
6. `ec[83]` = u32 at snapshot + 0x220.

### 4.2 `u3x_blended_locator`

1. If `ec[0]` bit 9 is set: return 0.
2. Out = element 4, generator = element 5, channel = low 32 bits of element 6. If out or generator is null:
   return 0.
3. Add 1 to `ec[86]`. Index = `u3h_pick_snapshot(base, generator, &blend)`.
4. Index −1: write eight u32 to out: seven zeros and 0x3f800000 as the last (out +0x1c); element 8 = out;
   return 1.
5. First snapshot = generator + 0xb0 + index × 0x3f0 (signed index); second = the next one (+0x3f0).
6. A visitor record with base, kind 2, error 0, commands null, entries null, channel = channel. Call
   `u3h_decode(visitor, first, generator, 0, 0)`. Take locator A from the visitor's locator 0: A's position = its
   translation, A's rotation = its rotation, A's last 16 bytes zero.
7. If the error is still 0: `u3h_decode(visitor, second, generator, 1, 1)` with the same record.
8. If the error is non-zero: add 1 to `ec[87]`; if it is not 12 add 1 to `ec[81]`; return 0 (out and element 8
   untouched).
9. Take locator B the same way. Call
   `u3h_call_f32(base + 0xfd3c60, blend, result, A, B, 0)` with a 0x30-byte, 16-byte aligned result buffer (A and
   B also 16-byte aligned). Copy the first 0x20 bytes of the result to out; element 8 = out; return 1.

### 4.3 `u3x_npc_bits`

1. Message = element 4, NPC = element 0, holder = element 13. If the message is null: return.
2. Bits = 0. If `ec[0]` bit 11 is clear and NPC and holder are both non-null:
   a. add 1 to `ec[90]`;
   b. set the 300 bytes at message + 0x18 to zero;
   c. a stream block (3.4) with buffer = message + 0x18, position 0, capacity 2400;
   d. set the word `*u3h_repl_error()` to 0;
   e. bits = `u3h_anim_pack(base, message, block, NPC, holder + 0x68)`;
   f. if the error word is now non-zero: add 1 to `ec[92]` and bits = 0;
   g. `ec[91]` = bits;
   h. `u3h_repl_loop(base, NPC, holder + 0x68, message, bits)`.
3. u32 at message + 0x144 = bits (always, also on the paths that packed nothing).

### 4.4 `u3x_decode_anim`: start and header

1. `u3x_v_begin(visitor)`; visitor instance base = `a4`, depth base = `a5`.
2. If `nbytes` is 0: `u3x_v_error(visitor, 12)` and return (`ec[101]` is not written).
3. Set up a reader with `u3x_bits_init(reader, bits, nbytes)` and read the header of 3.3 in order: W; attached
   and, if set, the attach id (visitor attach id = it, else 0); the bound frame into the visitor's frame; if
   `flags` bit 0 the camera frame, zoom byte and aim vector (discarded); N; direct; the N ids; the "one type" bit
   and the type field(s), so that every entry i < N has an id and a type (a 32-bit replacement value is taken as it
   is, even when it is below 7). No error is tested during the header: failed reads give 0 and the walk goes on.
4. Read the first command code (2 bits) and, if it is 3, its sub code (3 bits). The "current sub code" starts as
   6 and changes only when a code 3 is read.

### 4.5 `u3x_decode_anim`: the command loop

State: next list entry E = 0 (u32), depth D = 0 (u32, wraps like unsigned arithmetic).

Repeat while the visitor's abort flag is clear and the current sub code is not 1:

1. If the reader's error word is set: `u3x_v_error(visitor, 1)`; stop. Else if the visitor's error is non-zero:
   stop.
2. Handle the command:

   **Anim (code 2) or marker anim (code 3, sub 5)**
   a. If not direct: E = the index field. If E ≥ N: error 11; stop.
   b. t = type of entry E, id = id of entry E. Group = `tabs[t]` when t < 7, else `u3x_art_group_sid(base, t)`.
      Null: error 3; stop.
   c. Anim: if id is 0xfff: `u3x_anim_by_index(tabs[0], a6)` when `tabs[0]` is non-null, else none. Otherwise, W
      clear: `u3x_anim_by_index(group, id)`; W set: `u3x_anim_by_sid(group, id)`. No anim, or an anim whose clip
      pointer (+0x28) is null: error 4; stop.
   d. Phase: read and de-quantise it by 3.3 (width from `u3x_v_frames(anim)` when W is clear).
   e. If the anim's flag bit 2 is set (streamed): id4 = u32 at anim +0x18; record =
      `u3x_stream_record(base, id4)`. With a record: phase = u32 at record +0x20, then anim = game function
      0xfd4c60(anim, id4, pointer to the phase word) (the function may change the phase; the anim may come back
      null). Without a record: phase = 0 and anim = null.
   f. If the reader's error word is set: error 1; stop. (Steps b to e have run with the zeros the failed reads
      returned, including a possible call of 0xfd4c60.)
   g. E += 1. Code 2: `u3x_v_anim(visitor, phase, anim, D)`; marker anim:
      `u3x_v_mark(visitor, phase, anim, D, a7)`. Then D += 1.

   **Blend (code 0 or 1)**
   a. If D < 2: error 2; stop.
   b. Read order. Order 1: a = D − 1, b = D − 2. Order 0: a = D − 2, b = D − 1.
   c. Read target. Target 1: into = a; target 0: into = b. If "into" is the smaller of the two slot numbers
      (D − 2): D −= 1.
   d. Read mode (mapped by 3.3). Code 0: read the 8-bit weight and de-quantise it; code 1: weight = 0x3f800000.
   e. If the reader's error word is set: error 1; stop.
   f. `u3x_v_blend(visitor, weight, a, b, into, mode)` (with the slot numbers computed before the change of D).

   **Code 3, sub 0**: read the 2-bit "back" value d. If D − 1 < d as unsigned 32-bit numbers: nothing (v7.239; the
   PS3 decoder only asserts here, and stopping gave up whole NPC states). Else `u3x_v_sub0(visitor, D − d − 1)`.
   (D = 0 never skips: D − 1 wraps to the largest number.)
   **Sub 2**: `u3x_v_sub2(visitor)`. **Sub 3**: `u3x_v_sub3(visitor, D)`, then D += 1.
   **Sub 4**: D −= 1, then `u3x_v_sub4(visitor)`. **Sub 6, 7** (and sub 5 is handled above): nothing.

3. Read the next command code and, if it is 3, its sub code. This read happens after every handled command,
   before the loop's conditions are tested again (also when the visitor has just set abort).

After the loop, however it ended: if the reader's error word is set and the visitor's error is 0:
`u3x_v_error(visitor, 1)`. Then `ec[101]` = the reader's bit position.

"error k; stop" = `u3x_v_error(visitor, k)` and leave the loop (the after-loop steps still run).

### 4.6 The three small visitor functions (strays)

`u3x_v_error(visitor, code)`: if the visitor's error is already non-zero, nothing. Else error = code; code 12
adds 1 to `ec[104]`; any other code is stored in `ec[82]`.

`u3x_v_anim(visitor, phase, anim, depth)`, by kind:
- 0: `u3x_v_push(visitor, phase, anim)`.
- 1: if depth + depth base (as u32) > 6: error 6, done. Index = depth + instance base (as u32). Null anim:
  `u3x_v_bind(visitor, index)`. Else `u3x_v_pose(visitor, frame, anim, index)` with
  frame = `f_mul(phase, f_sub(f_from_int(F), 0x3f800000))`, F = `u3x_v_frames(anim)` (phase × (F − 1), single
  precision, the subtraction first).
- other: `u3x_v_sample(visitor, phase, anim, depth)`.

`u3x_v_sub3(visitor, depth)`: only for kind 1: if depth + depth base (as u32) > 6: error 6; else
`u3x_v_bind(visitor, depth + instance base)`.

## 5. Test scenarios

All of this group can be tested without the live game: a test bit writer builds streams by 3.3; recording stubs
stand for the visitors (`u3x_v_begin`, `u3x_v_blend`, `u3x_v_mark`, `u3x_v_sub0`, `u3x_v_sub2`, `u3x_v_sub4`,
`u3x_v_push`, `u3x_v_pose`, `u3x_v_bind`, `u3x_v_sample`), the lookups, `u3h_decode`, `u3h_pick_snapshot`,
`u3h_call_f32`, `u3h_anim_pack`, `u3h_repl_loop`, and for game functions 0xfd4c60, 0x7bf430 and 0x1274970 (the
last two as stubs that move the reader's position by a chosen number of bits). Compare visitor memory, the `ec[]`
words, element 8 and the order and arguments of all calls.

Decoder:
1. `nbytes` 0: error 12, `ec[104]` + 1, `ec[101]` unchanged; a second error is ignored.
2. Minimal stream: W 0, not attached, frame, N 0, end command: begin called, no visitor command, `ec[101]` = bits
   read.
3. Attached with a 13-bit id; `flags` bit 0 set and clear (camera block skipped or absent).
4. N = 1, 2, 3, 4, 5, 8, 9, 16, 17, 31 with direct 0 (index widths 1 to 5) and direct 1 (entries in order).
5. One type for all, per-entry types, type 7 with a 32-bit id (also a 32-bit value below 7).
6. Anims with 1, 8, 9, 64, 65 and 65535 frames: phase widths 4, 8, 16; all quantised values at the ends and some
   in between, bit for bit. W = 1: 32-bit ids, lookup by name id, 16-bit phase.
7. Id 0xfff with and without `tabs[0]`; group missing (error 3); anim missing or without clip (error 4); index
   ≥ N (error 11).
8. Streamed anim with a record (phase replaced, 0xfd4c60 called, result null and non-null, phase changed by the
   stub) and without (phase 0, null anim passed on).
9. Blends: code 0 and 1, the four order / target combinations (depth change only for the lower target), modes 0
   to 3, weights 0, 1, 254, 255; blend at depth 0 and 1 (error 2).
10. Sub 0 with d = 0..3 at depths 0 to 4 (skipped when deeper than the stack, and the wrap at depth 0); sub 2, 3, 4 (depth 0 wrap), 6, 7;
    marker anim (a7 passed on).
11. Truncation at every bit position of a longer stream: error 1 exactly once, the calls made before it, the
    final `ec[101]`.
12. Visitor that sets abort during a command: one more command code is read, then the loop ends.
13. Visitor error set by a stub: loop stops at the next test; error on entry non-zero.

Hooks: switch bits on and off; null generator, offset at and past the limit; decode error 0, 12 and another;
pick −1; first decode failing, second failing, both good (locator copy and blend arguments); NPC with null
message, null NPC, null holder, pack overflow, normal pack. `u3x_v_anim` / `u3x_v_sub3`: each kind, depth limit
at 6 and 7, null anim, frame value bit for bit. Thousands of random streams against the old functions.

## 6. Notes

Strays covered here (in no work group of the inventory): `u3x_v_error`, `u3x_v_anim`, `u3x_v_sub3` and the
private "frame of a phase" rule used by `u3x_v_anim` (4.6; G52's marker function uses the same rule).

Tables: this group holds no literal table of game values. The three de-quantisation constants follow from the
field widths (1 / (2^w − 1)).

Behaviour to keep although it looks like a slip or is unreachable:
- The wide form (W = 1) and id 0xfff are never produced by our own packer; they are kept for PS3 senders.
- A 32-bit type value below 7 is used as a table number.
- Unsigned wraps of the depth: "back" at depth 0, "copy down" at depth 0.
- An anim command whose reads failed still runs the lookups (and possibly game function 0xfd4c60) before error 1
  is recorded; with N = 0 it records error 11 instead.
- No error test during the header.
- In 4.2 locator A is taken even when the first decode failed (it is then not used).
- A locator failure counts in `ec[87]` and, when not "empty", also in `ec[81]`.
- `nbytes` 0 leaves `ec[101]` unchanged.

Open questions:
1. The bit layout inside the game-coded bound frame (fields 4 and 5 of the header) is the game's own coder's and
   is not described; both sides call the game for it.
2. `u3x_v_max`, `u3x_v_blend_cmd`, `u3x_v_sub` and `u3x_frame_fresh` / `u3x_frame_identity` are also in no
   inventory list; they belong with G52's functions (not called from this group, except `u3x_frame_fresh`, which
   stays as it is).

## Clarifications (tester, 2026-11-14; old v7.212 against the new code, differential test reimpl2\G50)

Facts confirmed by test on the old functions. Numbers in brackets are the implementer's open choices.

1. (1) Image base inside `u3x_decode_anim`: the old function takes the visitor's base field (+0x120) once, BEFORE it
   calls `u3x_v_begin`, and uses that value for every call that carries a base (both `u3x_bits_frame` calls,
   `u3x_bits_vector`, `u3x_art_group_sid`, `u3x_stream_record`, game function 0xfd4c60). A later change of the field
   during the decode is not seen. Taking it after `u3x_v_begin` gives the same result as long as `u3x_v_begin` does
   not write the base field (G52's does not: fresh frame, abort flag, key for kind 0).
2. (2) The test "E ≥ N: error 11" is applied in direct mode too: with direct = 1 the (N+1)th anim or marker anim
   command gives error 11 (with N = 0 the first one), after N visitor calls.
3. (3) The default-anim test "id = 0xfff" applies to 12-bit and 32-bit ids alike. With W = 1 and id 0xfff the anim
   is `u3x_anim_by_index(tabs[0], a6)` (no lookup by name id); its phase still has 16 bits.
4. (4) For id 0xfff the entry's own group is looked up first (table number or `u3x_art_group_sid`); a missing group
   gives error 3 without any anim lookup. With the group present and `tabs[0]` null: error 4 without a lookup.
5. (5) "One type" with N = 0 still reads the type field (3 bits, plus 32 when it is 7); `ec[101]` shows it.
6. (6) Blend with failed reads: error 1, `u3x_v_blend` is not called. Whether the depth is lowered before the
   read-error test cannot be observed (the decode stops and the depth is not used again).
7. (7) Sub 0 has no read-error test of its own: when the 2-bit "back" read fails, back is 0, the depth test runs
   with it, `u3x_v_sub0(visitor, D − 1)` IS called, and error 1 is recorded at the next loop test.
8. (8) `u3x_apply_decode` and `u3x_blended_locator` give exactly these visitor fields a value before the first
   `u3h_decode`: base, entries, commands, channel, kind, error. Frame, locators, instance base, depth base, attach
   id, key, abort and active have no value there (the decode sets frame, abort, instance base, depth base, attach id
   and, for kind 0, key). Nothing in this group gives "active" a value for the locator visitor.
9. (9) No bounds test on the snapshot index: every result of `u3h_pick_snapshot` other than −1 (negative, 5, larger)
   is used as it is, first = generator + 0xb0 + index × 0x3f0 in signed 64-bit arithmetic, second = first + 0x3f0.
10. Blend depth, said with the wire bits: the depth is lowered by one exactly when the order bit and the target bit
    differ (order 1 / target 0, or order 0 / target 1).
11. After a visitor has set abort (or its error), exactly one more command code is read (2 bits, 3 more when it is
    3) before the loop ends; `ec[101]` includes those bits. With a non-zero visitor error on entry the header and
    the first command code are still read and `ec[101]` is written; no command is handled and `ec[82]` is not
    changed.
12. The current sub code is kept while commands with codes 0 to 2 are read; it has no effect on them.
13. `u3x_v_frames` is called once per anim command when W = 0 (for the phase width) and not at all when W = 1;
    `u3x_v_anim` of kind 1 calls it once more for a non-null anim. An anim with frame count 0 takes the 4-bit
    phase; its frame in `u3x_v_anim` is phase × (0 − 1).
14. `u3x_npc_bits`: the old function calls `u3h_repl_error` once and keeps the address for the clearing and for the
    test after the pack. Calling it once or twice makes no difference as long as it returns the same address.
15. Float: the old source of this group already works on 32-bit patterns with the same single-precision operations
    (`f_from_int`, `f_mul`, `f_sub`); nothing of it ran on the x87 unit. All 16, 256 and 65536 quantised values, the
    blend weights and the frame of a phase for every frame count 0 to 65535 are equal bit for bit, and equal to the
    host's single-precision arithmetic.
16. Loop top with both the reader's error word and the visitor's error set: which of the two tests comes first makes
    no difference (error 1 is then not recorded, because `u3x_v_error` keeps the first error; the loop ends either
    way and the after-loop step records nothing). Rerun 2026-11-14 (second tester): all cases equal at -O1 and -O2.
