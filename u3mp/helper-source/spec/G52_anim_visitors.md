# G52. Anim decoder visitors

Behaviour description for a reimplementation (reimpl2 pipeline, describer's document). It says what the functions
do, not how the old ones were written. Offsets are hexadecimal when prefixed with 0x. "Image offset X" means the
address `base + X`, where `base` is the game image base (here always the visitor's own `base` field). All
multi-byte values are little-endian; "u8 / u16 / u32 / s32 / u64" are integers of that width, "pointer" is 8
bytes. Game functions use the System V AMD64 convention with integer arguments unless a float register is named.

Related documents: `G50_anim_decoder.md` (the decoder that drives these functions; its 3.2 is the visitor record,
its 4.6 covers `u3x_v_error`, `u3x_v_anim`, `u3x_v_sub3` and the "frame of a phase" rule), G51 (lookups, the
float-argument call, the ice-snapshot commands: it calls `u3x_v_pose`, `u3x_v_ice`, `u3x_v_blend_cmd` and
`u3x_v_bind` through C exports), G49 (the packer; owns `u3x_stream_root`).

**Float.** This group does no arithmetic of its own except the "frame of a phase" rule in `u3x_v_mark`
(G50 4.6: `f_mul(phase, f_sub(f_from_int(F), 0x3f800000))`, the subtraction first). The old build computed it with
single-precision instructions (integer-to-single conversion, single subtract, single multiply, one rounding
each); no step of this group ran on the x87 unit, so every value must match the old code bit for bit. All
other floats (phases, weights, frames, locator components) are only stored or passed on as 32-bit patterns. No
float is passed by value: a float argument of a game function goes through G51's `u3x_call_float_arg`.

## 1. Purpose and callers

The decoder of G50 walks a received anim-state bit stream and reports what it finds to a "visitor". One record
(G50 3.2) serves three kinds of visitor, selected by its `kind` field:

- kind 0, **state**: collects the anims of the stream into the snapshot's entry list;
- kind 1, **commands**: writes anim commands into a command buffer of the PS4 game;
- kind 2, **locator**: samples one channel of the anims between the stream's two markers and blends the samples.

This group is the visitor's functions (start of a decode, anim for the state list, channel sample, blend, "back"
reference, "copy down", marker anim) and the small writers of the command buffer under them.

| Function | Called from |
|---|---|
| `u3x_v_begin`, `u3x_v_blend`, `u3x_v_sub0`, `u3x_v_sub4`, `u3x_v_mark` | `u3x_decode_anim` (G50 4.4, 4.5); also the C one-line wrappers `vbegin119`, `vblend119`, `vsub0_119`, `vsub4_119`, `vmark119` |
| `u3x_v_push`, `u3x_v_sample` | `u3x_v_anim` (G50 4.6) and `u3x_v_mark` |
| `u3x_v_pose` | `u3x_v_anim` (G50 4.6); C `c6_119` behind the export `u3h_v_pose` (G51's `u3x_anim_commands`) |
| `u3x_v_ice` | `u3x_v_mark`; C `c9b_119` behind the export `u3h_v_ice` (G51's ice-snapshot commands) |
| `u3x_v_blend_cmd` | `u3x_v_blend`; C `c7_119` behind the export `u3h_v_blend` (G51) |
| `u3x_v_sub` | `u3x_v_sub0` |
| `u3x_v_command` | the command writers of this group and the two that stay as they are (`u3x_v_bind`, `u3x_v_copy`) |
| `u3x_v_max` | the same writers |
| `u3x_frame_identity` | `u3x_frame_fresh` (so `u3x_v_begin` and G50's decoder), C `bf_identity` (film cameras, death cameras) |

Everything runs on the game's main thread; no locking.

## 2. Interface

### 2.1 Functions to deliver (names and signatures unchanged; all `extern "C"`)

| Name | Signature | Meaning |
|---|---|---|
| `u3x_v_command` | `uint32_t *u3x_v_command(void *visitor, uint16_t type, uint16_t words)` | 4.1: room for one command; result = its first word, or null |
| `u3x_v_max` | `void u3x_v_max(void *visitor, uint32_t index)` | 4.1: raises the buffer's "highest instance index" |
| `u3x_v_pose` | `void u3x_v_pose(void *visitor, uint32_t frame, uint8_t *anim, uint32_t index)` | 4.2: pose command. `frame`: float pattern |
| `u3x_v_blend_cmd` | `void u3x_v_blend_cmd(void *visitor, uint32_t weight, uint32_t a, uint32_t b, uint32_t into, uint32_t mode)` | 4.2: blend command. `weight`: float pattern |
| `u3x_v_sub` | `void u3x_v_sub(void *visitor, uint32_t index)` | 4.2: "back reference" command |
| `u3x_v_ice` | `void u3x_v_ice(void *visitor, uint16_t type, uint32_t index, uint64_t p1, uint64_t p2, uint64_t p3)` | 4.2: ice command of type 0xc or 0xe with three pointers |
| `u3x_v_begin` | `void u3x_v_begin(void *visitor)` | 4.3 |
| `u3x_v_push` | `void u3x_v_push(void *visitor, uint32_t phase, uint8_t *anim)` | 4.4: state visitor's entry for an anim |
| `u3x_v_sample` | `void u3x_v_sample(void *visitor, uint32_t phase, uint8_t *anim, uint32_t depth)` | 4.5: locator visitor's sample |
| `u3x_v_blend` | `void u3x_v_blend(void *visitor, uint32_t weight, uint32_t a, uint32_t b, uint32_t into, uint32_t mode)` | 4.6 |
| `u3x_v_sub0` | `void u3x_v_sub0(void *visitor, uint32_t x)` | 4.7 |
| `u3x_v_sub4` | `void u3x_v_sub4(void *visitor)` | 4.8 |
| `u3x_v_mark` | `void u3x_v_mark(void *visitor, uint32_t phase, uint8_t *anim, uint32_t depth, int32_t a7)` | 4.9 |
| `u3x_frame_identity` | `void u3x_frame_identity(uint8_t *frame)` | 4.10: the identity bound frame |

The file-private "identity locator" fill (old `locator_identity`) is described in 3.3 and needs no exported name.
The C wrappers stay as they are (each passes its arguments through unchanged).

One part (about 195 old lines). If two implementers are wanted: part A = the command buffer writers and the frame
(`u3x_v_command`, `u3x_v_max`, `u3x_v_pose`, `u3x_v_blend_cmd`, `u3x_v_sub`, `u3x_v_ice`, `u3x_frame_identity`;
4.1, 4.2, 4.10); part B = the visitor functions (`u3x_v_begin`, `u3x_v_push`, `u3x_v_sample`, `u3x_v_blend`,
`u3x_v_sub0`, `u3x_v_sub4`, `u3x_v_mark`; 4.3 to 4.9), which call part A's functions by the signatures above.

### 2.2 Functions of other groups, of the C part and of the shared tools that are called

| Name | Signature | Effect |
|---|---|---|
| `u3h_ext` | CONVENTIONS.h | the helper's state; the diagnostic words `ec[]` are u32 at `u3h_ext() + 0x4450` |
| `u3x_v_error` | `void u3x_v_error(void *visitor, int code)` | G50 4.6: records the first error of a decode (later ones are ignored) |
| `u3x_v_bind` | `void u3x_v_bind(void *visitor, uint32_t index)` | command writer "empty slot": command type 0xd, 2 words, word 1 = index, then `u3x_v_max(visitor, index)`. Stays as it is |
| `u3x_v_copy` | `void u3x_v_copy(void *visitor, uint32_t a, uint32_t b)` | command writer "copy": type 0xf, 3 words, word 1 = a, word 2 = b, then `u3x_v_max` for a and for b. Stays as it is |
| `u3x_v_frames` | `uint32_t u3x_v_frames(const uint8_t *anim)` | frame count of an anim (u16 at +0x22 of its clip; 0 without a clip). Stays as it is |
| `u3x_frame_fresh` | `void u3x_frame_fresh(uint8_t *frame)` | a one-line forward to `u3x_frame_identity`. Stays as it is |
| `u3x_call_float_arg` | `uint64_t u3x_call_float_arg(uintptr_t fn, uint32_t f0, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)` | G51: calls `fn(a0, a1, a2, a3)` with the float pattern `f0` in the first float register; returns the integer result register |
| `u3h_stream_root` | `uint8_t *u3h_stream_root(uintptr_t base, uint8_t *anim, uint32_t *phase)` | C export of G49's `u3x_stream_root`: when `anim` is a chunk played by one of the game's streaming animations, that streaming animation (and its phase in `*phase`); else null |

`u3x_v_bind` and `u3x_v_copy` use `u3x_v_command` and `u3x_v_max` exactly as 4.1 describes them; both must keep
working unchanged.

## 3. Game facts

### 3.1 Visitor record

G50 3.2 (0x160 bytes). Fields used here: frame +0; locators +0x60 (four of 0x30 bytes, number i at
+0x60 + i × 0x30); base +0x120; entries +0x128; commands +0x130; instance base +0x138 (s32, used as u32);
depth base +0x13c (s32, used as u32); key +0x144; channel +0x148; abort +0x14c; active +0x14d; kind +0x150;
error +0x154. The fields G50 left to this document:

- **key** (u32): state visitor: the effect key of the anim entered last (0 = none). 4.4, 4.7.
- **active** (u8): locator visitor: 1 between the stream's first and second marker (set by `u3x_v_sub2`, which
  stays as it is); samples and locator blends happen only while it is 1.

### 3.2 Command buffer of the PS4 game (the visitor's `commands`, kind 1)

| Offset | Type | Meaning |
|---|---|---|
| +0 | pointer | the command words (u32 array) |
| +8 | u32 | capacity in words |
| +0xc | u32 | position: number of words used |
| +0x10 | u32 | highest instance index any command has named so far |

A command is a run of u32 words. Word 0 holds two u16: the command type at byte 0 and the command's length in
words at byte 2. A pointer in a command takes two consecutive words (8 bytes, written at a 4-byte aligned
address). Commands written by this group:

| Type | Words | Contents after word 0 |
|---|---|---|
| 8 pose | 5 | words 1-2 = anim pointer; word 3 = instance index; word 4 = frame (float pattern) |
| 9 blend | 6 | word 1 = a; word 2 = b; word 3 = into; word 4 = mode; word 5 = weight (float pattern) |
| 0xa back reference | 2 | word 1 = instance index |
| 0xc, 0xe ice | 8 | word 1 = instance index; words 2-3 = p1; words 4-5 = p2; words 6-7 = p3 |

(0xd and 0xf are the two writers that stay: 2.2.)

### 3.3 Other layouts

- **Locator of the visitor** (0x30 bytes): scale 4 floats at +0, rotation (x, y, z, w) at +0x10, translation 4
  floats at +0x20. The **identity locator** is: the four scale words 0x3f800000 (1.0), rotation words 0, 0, 0,
  0x3f800000, the four translation words 0.
- **Snapshot entry area** (the visitor's `entries`, kind 0): 22 entries of 0x18 bytes from +0; u32 entry count
  at +0x210. Entry: u32 +0 = effect key; pointer +8 = anim; u32 +0x10 = phase (float pattern); u8 +0x14 = "set"
  flag; u8 +0x15 = "referenced again" flag. The other bytes of an entry are not written by this group.
- **Anim**: pointer at +0x28 = clip; pointer at +0x48 = effect list (may be null). Effect list: u32 at +0 = its
  key; u32 at +4 = its item count.
- **Channel sample request** of game function 0xfbaca0 (0x28 bytes, 16-byte aligned): pointer at +8 = anim; u32
  at +0x10 = channel id; u32 at +0x14 = phase (float pattern); everything else zero (+0, +0x18 pointer, +0x20 and
  +0x21 bytes, padding).
- **Bound frame** (0x60 bytes): u32 +0 and u32 +4 = marker words; rigid body handle +8; id +0x10; parent
  position +0x20; parent rotation +0x30; local position +0x40; local rotation +0x50 (rotations x, y, z, w).

### 3.4 Game functions

| Image offset | Arguments | Result | Meaning |
|---|---|---|---|
| 0xfbaca0 | (0, request, out locator); integer call, first argument the number 0 | success in the low 8 bits of the result | samples the request's channel of the anim at the phase into a locator of 3.3 |
| 0xfd3c60 | (out locator, locator a, locator b, 0), blend factor in the first float register | - | out = blend of a and b. Through `u3x_call_float_arg` |
| 0xdc4240 | (anim, skeleton value as u64 zero-extended from 32 bits, pointer to u64 out 1, pointer to u64 out 2), frame in the first float register | success in the low 8 bits of the result | retargets the anim's pose at that frame to the skeleton; fills the two u64. Through `u3x_call_float_arg` |

### 3.5 Diagnostic words and error codes

`ec[102]` += 1: an anim did not fit the state list (4.4). `ec[103]` += 1: a marker anim gave an ice command
(4.9). Both words are also written by unrelated code; they must still be written exactly as stated.

Error codes recorded by this group through `u3x_v_error`: 4 anim missing; 6 depth past the limit; 7 command
buffer full; 8 locator slot past 3; 10 ice pointer not 16-byte aligned.

Tables: this group holds no literal table of game values.

## 4. Behaviour

### 4.1 `u3x_v_command`, `u3x_v_max`

`u3x_v_command(visitor, type, words)` (only ever called for a visitor whose `commands` is a valid buffer):
1. If the visitor's error is non-zero: result null; nothing written.
2. If position + words (32-bit unsigned sum) > capacity: `u3x_v_error(visitor, 7)`; result null; position
   unchanged.
3. Else: the command starts at word number "position" of the buffer's words. Write its two u16 (type, words);
   the buffer's position += words; result = address of the command's word 0. The command's other words are left
   as they were (the caller fills them).

`u3x_v_max(visitor, index)`: the buffer's u32 at +0x10 becomes the larger (unsigned) of its value and `index`.

### 4.2 The command writers

Each asks for its command with `u3x_v_command`; on a null result it does nothing more. Otherwise it fills the
words by 3.2 and then calls `u3x_v_max` once per instance index it wrote, in the order given.

- `u3x_v_pose(visitor, frame, anim, index)`: type 8, 5 words; `u3x_v_max(index)`.
- `u3x_v_blend_cmd(visitor, weight, a, b, into, mode)`: type 9, 6 words; `u3x_v_max` for a, b, into.
- `u3x_v_sub(visitor, index)`: type 0xa, 2 words; `u3x_v_max(index)`.
- `u3x_v_ice(visitor, type, index, p1, p2, p3)`: first, if any of the low 4 bits of p1, p2 or p3 is set:
  `u3x_v_error(visitor, 10)` and done (no command, also when an earlier error makes that call a no-op). Else
  command `type`, 8 words; `u3x_v_max(index)`. A null pointer (0) counts as aligned.

### 4.3 `u3x_v_begin`

1. The visitor's frame = the identity bound frame (`u3x_frame_fresh` on it).
2. abort = 0.
3. Kind 0: key = 0. Kind 2: each of the four locators = the identity locator (3.3), and active = 0. Kind 1 (and
   any other value): nothing more.

Not touched: error, the entry count, the command buffer, base, instance / depth base, attach id, channel.

### 4.4 `u3x_v_push` (state)

1. key = 0.
2. Done if `anim` is null, or its effect list pointer (+0x48) is null, or the effect list's item count (u32 at
   +4) is 0, or the anim's clip pointer (+0x28) is null.
3. key = the effect list's key (u32 at +0).
4. Root = `u3h_stream_root(base, anim, pointer to a scratch u32)`. If it is non-null, the anim to store is the
   root; else the anim itself. The phase the call reports is not used.
5. Count = u32 at entries + 0x210. If count > 21: `ec[102]` += 1; done (key stays set, nothing stored).
6. Else fill entry number count (3.3): key = the visitor's key; anim = the anim of step 4; phase = the `phase`
   argument; "set" flag = 1; "referenced again" flag = 0; and the count becomes count + 1.

### 4.5 `u3x_v_sample` (locator)

1. If active is 0: nothing.
2. If depth ≥ 4: error 8; done. If `anim` is null: error 4; done. (In this order.)
3. Build a request (3.3) with the anim, the visitor's channel and `phase`; call game function
   0xfbaca0(0, request, locator number depth).
4. If the low 8 bits of the result are 0: locator number depth = the identity locator.

### 4.6 `u3x_v_blend`

By kind, with ib = the instance base as u32 (sums wrap):
- 1: `u3x_v_blend_cmd(visitor, weight, a + ib, b + ib, into + ib, mode)`.
- 2, and active is non-zero: if a ≥ 4 or b ≥ 4 or into ≥ 4: error 8; else
  `u3x_call_float_arg(base + 0xfd3c60, weight, locator number into, locator number a, locator number b, 0)`
  (the locators of the visitor record itself; out may be the same locator as a or b). `mode` is not used.
- anything else (kind 0, kind 2 while not active): nothing.

### 4.7 `u3x_v_sub0`

- Kind 0: if key is 0: nothing. Else look through the entries from the newest (number count − 1) down to number
  0 for the first whose key (u32 +0) equals the visitor's key; set that entry's "referenced again" flag (u8
  +0x15) to 1. No such entry (or count 0): nothing.
- Kind 1: `u3x_v_sub(visitor, x + instance base)` (u32 sum).
- Other kinds: nothing.

### 4.8 `u3x_v_sub4`

With ib = the instance base as u32:
- Kind 1: `u3x_v_copy(visitor, ib + 1, ib)`.
- Kind 2, and active is non-zero: if ib + 1 ≥ 4: error 8; else the 0x30 bytes of locator number ib + 1 are
  copied to locator number ib.
- anything else: nothing.

### 4.9 `u3x_v_mark`

By kind:
- 0: `u3x_v_push(visitor, phase, anim)`.
- 1:
  1. If depth + depth base (u32 sum) > 6: error 6; done.
  2. If `anim` is null: error 4; done.
  3. Two u64 outputs, both 0. Frame = the "frame of a phase" of G50 4.6 for (phase, anim), using
     `u3x_v_frames(anim)`. Call
     `u3x_call_float_arg(base + 0xdc4240, frame, anim, a7 taken as u32 and zero-extended, address of output 1, address of output 2)`.
  4. Index = depth + instance base (u32 sum).
  5. If the low 8 bits of the result are non-zero: `ec[103]` += 1, then
     `u3x_v_ice(visitor, 0xc, index, output 1, 0, output 2)`. Else `u3x_v_bind(visitor, index)`.
- any other kind: `u3x_v_sample(visitor, phase, anim, depth)`.

### 4.10 `u3x_frame_identity`

Sets the 0x60 bytes at `frame` to: all zero, except u32 +0 = u32 +4 = 0x7fcccccc (the game's "not bound" marker
words), u32 +0x3c = 0x3f800000 (parent rotation w = 1.0) and u32 +0x5c = 0x3f800000 (local rotation w = 1.0).
This is the same frame other specs call "the identity frame" (`G42_death_cameras.md` 3.6).

## 5. Test scenarios

Everything here can be tested without the live game: a fake visitor record, a fake command buffer and entry
area, recording stubs for `u3x_v_error` (or the real one with a fake `ec[]`), `u3x_v_bind`, `u3x_v_copy`,
`u3x_v_frames`, `u3h_stream_root`, `u3x_call_float_arg` and game function 0xfbaca0 (through a fake image
base). Compare the record, the buffers, `ec[102]` / `ec[103]`, and the order and arguments of all calls.

1. `u3x_v_command`: error already set; position + words equal to the capacity (fits) and one more (error 7,
   position unchanged); words 0; header bytes and returned address; words after the header untouched.
2. `u3x_v_max`: index below, equal to and above the stored value.
3. Each writer with room and without room: exact words (the pointer halves of pose and ice), the `u3x_v_max`
   calls and their order. `u3x_v_ice`: each pointer misaligned in turn (error 10, no command), all aligned,
   zero pointers, types 0xc and 0xe, an earlier error set.
4. `u3x_v_begin` for kinds 0, 1, 2 and another value, on a record filled with a pattern: exactly the stated
   fields change.
5. `u3x_v_push`: null anim; no effect list; item count 0; no clip (key 0 afterwards in all four); stream root
   found and not found; counts 0, 21 (stored, count 22) and 22 (`ec[102]`, key set, nothing stored); the entry's
   untouched bytes.
6. `u3x_v_sample`: inactive; depth 3 and 4; null anim; depth 4 with null anim (error 8); the request bytes;
   stub result with low byte 0 but higher bits set (identity written) and with low byte non-zero (locator left
   as the stub wrote it).
7. `u3x_v_blend`: kind 0; kind 1 with instance base 0 and 1; kind 2 inactive; kind 2 active with each of a, b,
   into at 3 and 4.
8. `u3x_v_sub0`: kind 0 with key 0, with the key in several entries (only the newest marked), in none, with
   count 0; kind 1; kind 2.
9. `u3x_v_sub4`: kind 1; kind 2 inactive; kind 2 active with instance base 0, 2 and 3; kind 0.
10. `u3x_v_mark`: kind 0; kind 1 with depth + depth base 6 and 7, null anim, call succeeding (outputs aligned;
    output misaligned gives error 10 after `ec[103]` was counted) and failing (bind); the frame argument bit for
    bit for frame counts 0, 1, 2, 65535 and phases 0, 1.0, quantised values in between; negative `a7`
    (zero-extended); kind 2.
11. `u3x_frame_identity` on a buffer filled with a pattern.
12. Whole decodes: G50's decoder with these functions against the old ones over thousands of random streams,
    for the three kinds.

## 6. Notes

Strays covered here (in no work group of the inventory): `u3x_v_max`, `u3x_v_blend_cmd`, `u3x_v_sub`,
`u3x_frame_identity`. `u3x_frame_fresh` stays as it is: it only forwards to `u3x_frame_identity`. The
inventory's trivial list holds `u3x_v_bind`, `u3x_v_copy`, `u3x_v_frames` and `u3x_v_sub2`; they stay as they
are (their effects are given in 2.2 and in G50 2.2).

Behaviour to keep although it looks like a slip or is unreachable:
- `u3x_v_begin` does not reset the state list's count or the error.
- A full state list still sets the key, so a following "back" reference can mark an older entry with that key.
- The entry stores the `phase` argument, never the phase `u3h_stream_root` reports.
- `u3x_v_sample` and the locator blend use the depth / slot numbers as they come, without the instance base;
  `u3x_v_sub4` of the locator visitor uses the instance base alone as the slot number.
- In `u3x_v_sub4` (kind 2) the test is on ib + 1 in 32 bits; an instance base of 0xffffffff would pass it. The
  callers only ever give 0 or 1.
- `u3x_v_blend` (kind 2) gives game function 0xfd3c60 the visitor's own locators (scale, rotation, translation),
  while G50 4.2 gives the same function locators laid out as position, rotation, zero. Keep both as described.
- `u3x_v_mark` counts `ec[103]` before `u3x_v_ice` can still refuse the command (error 7 or 10).
- Kinds other than 0, 1, 2 do not occur; where the text says "any other kind" the old behaviour for them is
  kept.

Open questions: none.

## Clarifications (tester, 2026-11-14)

Facts found or confirmed by the old-against-new test (`reimpl2\G52\test_g52.cpp`: recording stand-ins with a digest
of the whole test memory at every call, one-change cases, overlap probes, a bit-for-bit float sweep). "Not
observable" means: no result, no memory written and no call changes, whatever the other functions do.

1. **4.7, kind 0, entry count of 0x80000000 or more (the new code differed here).** The entry count (u32 at
   entries + 0x210) is taken as a signed 32-bit number for the "back" search: a count of 0x80000000 or more is
   negative and means no search at all (nothing read beyond the count, nothing written). Counts 1 to 0x7fffffff
   are searched from number count - 1 down to 0 as written in 4.7, also past entry 21. The decoder only ever leaves
   counts 0 to 22, so the case cannot happen in the game.
2. **4.9 kind 1, frame count.** The value `u3x_v_frames` returns is converted to a float as a signed 32-bit integer
   (0x80000000 and above are negative). Confirmed bit for bit for frame counts 0 to 65535 and many larger and
   "negative" counts over specials, k/15, k/255, k/65535, every exponent, denormals and NaN phases, and over all
   2^32 phase patterns for selected frame counts.
3. **4.4 step 4, scratch phase word.** Its value before the `u3h_stream_root` call is not observable: the stream
   root (G49 4.12) only writes it. Old left it unset, the new code sets it to 0; both are correct.
4. **4.5 step 3, sample request.** Confirmed: all 0x28 bytes as in 3.3 (+0 pointer 0, +8 anim, +0x10 channel,
   +0x14 phase, +0x18 to +0x27 zero), the request 16-byte aligned, first argument 0, third argument the address of
   locator number depth inside the visitor record.
5. **4.4 step 6, store order.** The old code wrote the new count (count + 1) before the entry's fields (then key,
   anim, "referenced again" = 0, phase, "set" = 1). The order is observable only when the entry area overlaps the
   visitor record or the count word lies inside the entry being written, which never happens; any order is
   correct. Command words (4.1, 4.2): header, then position; then the command's words; then the `u3x_v_max`
   calls in the stated order (confirmed).
6. **4.2 `u3x_v_ice`.** Confirmed: the alignment test comes first; a misaligned pointer gives error 10 without
   asking for command room (so no error 7 even when the buffer is full, and nothing else when an error was
   already set).
7. **4.3 `u3x_v_begin`.** Confirmed: one call `u3x_frame_fresh(visitor)` (frame at +0) first, then abort = 0, then
   the kind-dependent stores.
8. **4.9 kind 1, image base.** Whether the visitor's base is read before or after the `u3x_v_frames` call is not
   observable (`u3x_v_frames` only reads the anim's clip).
