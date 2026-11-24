# G49: animation state packer and the bit-stream writer maths

Behaviour description for a clean-room reimplementation (reimpl2). It says WHAT the functions do; how to write
them is the implementer's choice. Shared tools: `reimpl2\CONVENTIONS.h` (not described again here).

## 1. Purpose and callers

The PS4 game never builds the per-player state message of the PS3 multiplayer, so remote characters would not
animate. The helper builds it. This group is the part that turns an actor's current animation state into the
**animation bit stream** of that message, in the layout the receiving side (the game's own reader and the helper's
decoder, G50) accepts, plus the small tools that packer needs: the bit writer, the vector quantiser, the PowerPC
float-to-integer conversion, quaternion and locator maths, the world locator of a bound frame, and the lookups of
animations in art groups and in the streaming animations.

Callers:

| Function | Called by | When |
|---|---|---|
| `u3x_anim_pack_body` | C `anim_pack_body119` (one line), which C `anim_pack119` calls; exports `u3h_anim_pack_full` (G48's `pack_animation`: players) and `u3h_anim_pack` (the NPC state sender of another group: `param5` 0, `param6` 2) | once per state message built |
| `u3x_bsw` | everything in this group that writes bits; C wrapper `bsw119` (no C caller left) | - |
| `u3x_bsw_vec`, `u3x_bsw_frame`, `u3x_bind_root`, `u3x_ev_query`, `u3x_loc_rel`, `u3x_bf_world`, `u3x_anim_lookup`, `u3x_stream_root`, `u3x_art_tab`, `u3x_tab_find` | the packer; each also has a one-line C wrapper (`bsw_vec119`, `bsw_frame119`, `bind_root119`, `ev_query119`, `loc_rel119`, `bf_world119`, `anim_lookup119`, `stream_root119`, `art_tab119`, `tab_find119`). `stream_root119` and `tab_find119` are exported to other groups as `u3h_stream_root` and `u3h_tab_find` (G50 / G51 use them) | - |
| `u3x_q_mul`, `u3x_q_rot`, `u3x_loc_mul`, `u3x_fabs` | inside this group only (C wrappers `q_mul119`, `q_rot119`, `loc_mul119`, `fabs119` have no caller left); the names stay exported | - |
| `u3x_ppc_ctiwz` | this group; C wrapper `ppc_ctiwz119`; `u3x_hs_text` (hat stat texts, another group) | - |
| `u3x_layer_counts` | C `layer_counts119` only (see 2.2: both go away) | - |

Threading: everything runs on the game's main thread, inside the game's frame. Nothing here blocks or allocates.

## 2. Interface

All functions are `extern "C" U3X_FN`. Floats are always passed as 32-bit patterns or through pointers.

Common parameters: `base` = the game image base. `bs` = a bit stream (3.1). A **locator** is 8 u32 float
patterns: position x, y, z, one pad word, rotation quaternion x, y, z, w. A **quaternion** is 4 patterns x, y, z, w.

### 2.1 Part A: bit writer and maths (no game data except one game call)

| Signature | Result |
|---|---|
| `void u3x_bsw(uint8_t *bs, uint32_t val, uint32_t n)` | appends the low `n` bits of `val` (4.1) |
| `uint32_t u3x_fabs(uint32_t f)` | `f` with bit 31 cleared |
| `uint32_t u3x_ppc_ctiwz(uint32_t f)` | float pattern to signed integer, PowerPC rules (4.2) |
| `void u3x_bsw_vec(uint8_t *bs, const uint32_t *v)` | writes a 3-float vector, compact when small (4.3) |
| `void u3x_bsw_frame(uintptr_t base, uint8_t *bs, uint8_t *f)` | writes a bound frame through the game's writer (4.4) |
| `void u3x_q_mul(uint32_t *o, const uint32_t *a, const uint32_t *b)` | quaternion product a·b into `o` (4.5) |
| `void u3x_q_rot(uint32_t *o, const uint32_t *q, const uint32_t *v)` | vector `v` (3 patterns read) rotated by `q`; 4 patterns written, the fourth 0 (4.5) |
| `void u3x_loc_mul(uint32_t *o, const uint32_t *a, const uint32_t *b)` | locator "a, then b inside a" (4.6) |
| `void u3x_loc_rel(uint32_t *o, const uint32_t *a, const uint32_t *b)` | locator b as seen from a (4.6) |
| `void u3x_bf_world(uintptr_t base, uint8_t *f, uint32_t *o)` | world locator of bound frame `f` (4.7) |

In every function with an output pointer `o`, `o` may be the same memory as an input: all inputs are read before
the first output word is written.

### 2.2 Part B: lookups

| Signature | Result |
|---|---|
| `uint8_t *u3x_bind_root(uintptr_t base, uint8_t *obj)` | the body the object's chain of bindings hangs on, or null (4.8) |
| `uint64_t u3x_ev_query(uintptr_t base, uint32_t sid, uint8_t *obj)` | first result word of the game's event `sid` sent to `obj` (4.9) |
| `int32_t u3x_tab_find(uint8_t *t, uint32_t key)` | index of `key` in a sorted id table, or −1 (4.10) |
| `uint8_t *u3x_art_tab(uint8_t *g)` | the animation table of art group `g`, or null (4.10) |
| `int u3x_anim_lookup(uintptr_t base, uint32_t *tab, int32_t *idx, uint32_t id, uint8_t *anim, uint8_t **tabs)` | 1 and `*tab`, `*idx` when animation `id` is found, else 0 with both untouched (4.11) |
| `uint8_t *u3x_stream_root(uintptr_t base, uint8_t *x, uint32_t *phase)` | the streamed animation that animation chunk `x` belongs to (and `*phase`), or null (4.12) |
| `int u3x_layer_replicated(uintptr_t base, uint32_t id)` | NEW. 1 when the animation layer `id` is sent, 0 when it is one of the layers that are never sent (4.13) |

`u3x_layer_replicated` replaces three things, which are to be REMOVED: the exported
`int u3x_layer_counts(uint32_t u, const uint32_t *ex, uint32_t n)`, the C function `layer_counts119` with its
literal list of 12 ids (`agc_port.inc`), and the C export `u3h_layer_counts` (its only caller is the packer, which
now calls `u3x_layer_replicated(base, id)`). For the tester: `u3x_layer_replicated(base, id)` must equal the old
`u3x_layer_counts(id, list, 12)` called with the old list, for every `id`, on an image that holds the four words of
3.7.

### 2.3 Part C: the packer

`uint32_t u3x_anim_pack_body(uintptr_t base, uint8_t *msg, uint8_t *bs, uint8_t *actor, uint8_t **tabs, int32_t param5, int32_t param6, uint8_t **buf)`

- `msg`: the state message being filled (only its flag word, u32 at +0x148, is read here; G48 owns the layout).
- `bs`: the bit stream to append to (position normally 0, the buffer zero-filled by the caller).
- `actor`: the game object whose animation state is packed (a player or NPC character). May be null.
- `tabs`: the actor's 8 art-group pointers (each may be null). May be null.
- `param5`: which pose stands for a pose snapshot: 0 = the bind pose, anything else = the tense idle pose.
- `param6`: how many commands of one layer are sent at most (callers pass 3 for players, 2 for NPCs).
- `buf`: receives the address of the command buffer (written only once the buffer exists, see 4.14).
- Result: the stream's bit position after packing, or 0 when nothing usable was packed (refusals of 4.14). The
  caller, not this function, looks at the overflow word.

### 2.4 Functions of other groups and of the C part that this group calls

| Function | Signature | What it does |
|---|---|---|
| `u3x_bf_handle_valid` (G42 4.3) | `int u3x_bf_handle_valid(uintptr_t base, const uint8_t *bf)` | 1 when bound frame `bf`'s body handle still names its body |
| `u3x_np_by_id` (G34 4.1) | `uint8_t *u3x_np_by_id(uintptr_t base, uint32_t id)` | the net player record with that id, or null |
| `u3x_sid` | `uint32_t u3x_sid(uintptr_t base, const char *s)` | the game's string id of a name: 0 for null or empty, else the result of the function whose address is stored at image 0x1702720, called with (0, s, 1) |
| `u3h_crc_sid` | `uint32_t u3h_crc_sid(const char *name)` | C export: string id of a name by the rule of G23 3.9 (pure) |
| `u3h_repl_error` | `uint32_t *u3h_repl_error(void)` | C export: address of the overflow word (3.1) |
| `u3h_repl_frame` | `uint8_t *u3h_repl_frame(int i)` | C export: scratch bound frame number `i` (0, 1 or 2), 0x60 bytes, 16-byte aligned, kept between calls |
| `u3h_repl_loc` | `uint8_t *u3h_repl_loc(void)` | C export: a 0x40-byte scratch block, 16-byte aligned |
| `u3h_handle_obj` | `uint8_t *u3h_handle_obj(uint8_t *h)` | C export: the object a 0x10-byte object handle names, or null |
| `u3h_cam_mgr` | `uint8_t *u3h_cam_mgr(uintptr_t base, int32_t screen)` | C export: the camera manager object of screen 0 or 1, or null |
| `u3h_find_art_group_name` | `uint8_t *u3h_find_art_group_name(uintptr_t base, const char *name)` | C export: the loaded art group with that name, or null (null or empty name: null) |
| `u3h_anim_cmd_buffer` | `uint8_t *u3h_anim_cmd_buffer(void)` | C export: the command buffer (0x8000 bytes, allocated on first use; null when the allocation failed) |
| `u3h_loop_src119` | `void u3h_loop_src119(uint8_t *buf, uint32_t n)` | C export: diagnostic loop-back, notes the command list (`n` words) |
| `u3h_ext` | in CONVENTIONS.h | the diagnostic words `ec[]` of this group are the u32 array at `u3h_ext()` + 0x4450 |

## 3. Game facts

### 3.1 The bit stream and the overflow word

| Offset | Type | Meaning |
|---|---|---|
| +0 | pointer | the buffer, used as an array of u32 words |
| +8 | u32 | position: number of bits written so far |
| +0xc | u32 | capacity in bits |

The same structure as the film player's stream (`film_helpers.md`); the game's own writers and readers use it.

Bit and byte order (this is what the peer reads): stream bit number p lives in word p / 32, at bit (31 − p mod 32)
of that word, and the words are ordinary 32-bit integers of the machine (little-endian in memory). So the first
bit of the stream is bit 7 of the buffer's byte 3, bit 8 of the stream is bit 7 of byte 2, bit 32 is bit 7 of byte
7. A field of n bits is stored most significant bit first; it may cross from one word into the next.

The **overflow word** (`*u3h_repl_error()`) is set to 1 by a write that does not fit. It is never cleared in this
group (the caller clears it before packing and reads it afterwards).

### 3.2 Game functions

| Image offset | Arguments | Result | Meaning |
|---|---|---|---|
| 0x7befc0 | (bit stream, bound frame) | none | the game's bound-frame writer: appends the frame to the stream (advances the position itself) |
| 0x1196230 | (bound frame, body or null) | none | binds the frame to that body, or unbinds it (null), keeping its world placement |
| 0x1197380 | (result block, null, u32 event id, object, argument block 1, 2, 3, 4): eight integer arguments, the last two on the stack | none | sends an event to an object; the answer is left in the result block |
| 0x10c5010 | (object) | pointer | the object's parent in the binding chain, or null |
| 0xfd62a0 | (u32, u32, u32) | pointer | the streamed (whole) animation for (skeleton id, clip id, streaming id), or null |
| 0xfbe270 | (animation controller, output block) | none | writes the controller's current command list (3.5) |
| function pointer at image 0x1702720 | (0, text, 1) | u32 | string id of a text (used through `u3x_sid`) |

Virtual calls (the function address is the 8-byte entry at the given byte offset of the table the object's first
8 bytes point to):

| Object, table offset | Arguments | Result | Meaning |
|---|---|---|---|
| game object, 0x248 | (object, u32 0) | pointer | the body the object itself is bound to |
| actor, 0x170 | (output block, actor, u32 0x3a83fc57) | none (fills the output block) | the actor's aim point; the output pointer is the FIRST argument, the actor the second |

### 3.3 Game data

| Image offset | Type | Meaning |
|---|---|---|
| 0x16e79f0 | u8 | "a state is being packed": 1 while the packer works (4.14) |
| 0x1d46088 | s32 | number of streaming animation records |
| 0x1d46090 | 50 pointers | the streaming animation records (entries may be null) |
| 0x52f018, 0x52f098, 0x52f118, 0x52f198 | u32 each, not aligned | four layer ids of 3.7 (immediates inside the game's own code) |

### 3.4 Structures read or written

Bound frame (0x60 bytes): pointer +8 = body handle (points to a slot holding the body pointer), locator at +0x20 =
the last known parent locator, locator at +0x40 = the local locator. A body starts with its world locator (8
words at +0). `u3x_bf_handle_valid` reads further fields (G42).

Game object (character): bound frame at +0xb0 (so its body handle pointer is at +0xb8), u16 flags at +0x128 (bit
0x800 = "is a binding root"), pointer +0x138 = animation controller (may be null), s32 +0x1c68 = screen number of
a local player.

Net player record: object handle of its character at +0x10.

Camera manager: pointer +0xc0 = zoom object (may be null; its u32 float pattern at +0x30 is the zoom value),
locator at +0x4c0 = the camera's locator.

Art group: u32 +0x18 = its id (string id of its name), pointer +0x30 = animation block (may be null), whose
pointer +0x10 = the animation table (may be null). Animation table: u32 +0 = entry count N, pointer +8 = N
entries of 16 bytes sorted ascending by their first u32 (the animation id).

Animation: u32 +0x18 = id, pointer +0x28 = clip data (u32 at clip +4 = clip id; u16 at clip +0x22 = frame count),
pointer +0x38 = name of its art group (text, may be null), u32 +0x40 = skeleton id.

Streaming animation record: u32 +0 = unit count U, u32 +0x20 = phase (float pattern), pointer +0x30 = definition,
pointer +0x48 = pointer to the channel lists. Definition: pointer +0 = U u32 counts; pointer +8 = U pointers, each
to that unit's u32 streaming ids; pointer +0x10 = U pointers, each to that unit's u32 slot numbers; pointer +0x20 =
u32 array indexed by slot number: the last channel index of that slot. The pointer at (record +0x48) +0 = array
indexed by slot number of pointers to that slot's channels; a slot's channels = array of pointers, one per channel,
each to a row of U pointers (one animation chunk per unit).

### 3.5 The command list

The output block handed to 0xfbe270 is 0x20 bytes, 16-byte aligned, zero-filled, then: pointer +0 = the word
buffer, u32 +8 = capacity in 32-bit words. The game stores the number of words written in the u32 at +0xc.

A command starts with u16 kind (+0) and u16 length in 32-bit words (+2); the next command starts length × 4 bytes
later. Fields are not aligned.

| Kind | Name used here | Fields |
|---|---|---|
| 3 | layer begin | none read |
| 4 | layer end | none read |
| 8 | animation | pointer +4 = animation, s32 +0xc = pose slot, u32 +0x10 = phase (float pattern) |
| 0x1b | mirrored animation | as kind 8 |
| 0xc | pose snapshot | s32 +4 = pose slot |
| 0xd | empty pose | s32 +4 = pose slot |
| 9 | blend | u32 +4 = slot A, u32 +8 = slot B, u32 +0xc = result slot, u32 +0x10 = blend mode, u32 +0x14 = weight (float pattern) |
| 0xa | blend with the previous layer | s32 +4 = pose slot |
| 0x19 | layer mark | u32 +4 = layer id |
| 0x1a | counted command mark | none read |

### 3.6 Numbers

| Value | Meaning |
|---|---|
| 0x437f0000 | 255.0: vector limit for x and z; weight scale; phase scale for 8 bits |
| 0x42fe0000 | 127.0: vector limit for y |
| 0x44000000 | 512.0: vector scale for x and z |
| 0x43800000 | 256.0: vector scale for y |
| 0x41700000 | 15.0: phase scale for 4 bits |
| 0x477fff00 | 65535.0: phase scale for 16 bits |
| 0x3f800000 | 1.0 (`F_ONE`) |
| 0x7fcccccc | a not-a-number pattern put into the first two words of a fresh bound frame |
| 0x2000 | capacity of the command list in words |
| 64 | at most this many animation uses are remembered; 36 = at most this many layers have a command count |

Ids: 0x660778e0 = event "is the actor carried / attached?" (name not found; the value is in the PS4 image as an
immediate); 0xfbd8cf95 = event "which player carries it" = the string id of "get-carry-player" (G23 3.9 rule,
checked by computing it); 0x3a83fc57 = the aim point query of virtual call 0x170 (name not found); 0x7339d08c =
layer "base"; 0x1a1c52d3 = animation "bindpose"; 0x86dfee10 = animation "tense-idle" (the last three equal the
string ids of those names; the new code may use the numbers or `u3h_crc_sid` of the names).

### 3.7 The layers that are never sent: rules, not a table

The old code holds a literal list of 12 layer ids and two literal id ranges. They are PS3 1.19 values; the PS4
image holds no such list. They are obtained like this (nothing of it is pasted here):

- **Eight ids by name** (string id rule of G23 3.9, each checked by computing it against the old value): "Hands",
  "Hair", "facial-base", "hands", "breathing-layer", "net-anim-layer", "ragdoll", "ragdoll-snapshot" (upper and
  lower case exactly as written).
- **Four ids read from the running game**: the PS4 function at image 0x52eff0 looks up four layers of a
  character by id, one after the other; the four ids are the 32-bit immediates of its compare instructions, at
  image 0x52f018, 0x52f098, 0x52f118 and 0x52f198 (each directly after the two instruction bytes 0x81 0x3a, the
  last one after 0x81 0x39). Their names were not found. The new code reads these four u32 words from the image
  (unaligned reads) each time or once; which is not observable.
- **First range**: the five ids S, S+1, S+2, S+3, S+4 with S = the string id of "FireAnim" (the game makes the ids
  of its fire animation layers by adding an index to that id).
- **Second range**: 15 consecutive ids. OPEN QUESTION (section 8, Q1): neither its first id nor a name for it is
  in the PS4 image, and no name was found whose string id is its first value.

The new code holds the names as U3X_RO character arrays and gets the ids with `u3h_crc_sid` (or its own
bit-by-bit CRC), as G44 3.8 does.

### 3.8 The wire layout of the animation bit stream

All fields most significant bit first, in this order (the bit numbering of 3.1). "Frame" = whatever the game's
writer 0x7befc0 appends (the game's reader 0x7bf430 on the other side takes it back; the packer only guarantees
512 free bits before calling it).

1. 1 bit: 0 ("animation indexes are 12 bits wide"; the reader would take 32-bit indexes for 1, never sent).
2. 1 bit: attached. When 1: 13 bits = the low 13 bits of the net player id the actor is attached to.
3. Frame: the actor's bound frame (relative to the carrying player's character when attached).
4. Only when bit 0 of the message flags is set (a player): Frame of the camera; 8 bits zoom; the aim point as a
   **vector**: 1 bit form, then for form 0 three two's-complement fixed point numbers of 18, 17 and 18 bits (x ×
   512, y × 256, z × 512, truncated toward zero), for form 1 three raw 32-bit float patterns x, y, z.
5. Animation table: 5 bits = entry count E (0 to 31); 1 bit = "one entry per use, in order"; E × 12 bits = each
   entry's index in its art group's animation table; 1 bit = "all entries have the same art group"; then one art
   group code (same-group bit 1) or E codes (bit 0). An art group code is 3 bits: 0 to 6 = number of the actor's
   own art group; 7 = a 32-bit art group id follows.
6. Commands, each starting with a 2-bit code; code 3 is followed by a 3-bit sub-code:

| Code | Meaning | What follows |
|---|---|---|
| 2 | animation, or pose snapshot | [entry number, W bits, unless "one entry per use"], then the phase: 4, 8 or 16 bits by the animation's frame count (a snapshot: value 0 in the same width, v7.242) |
| 3, 5 | mirrored animation | as code 2 |
| 3, 3 | empty pose | nothing |
| 0 | blend with a weight | [1 bit order], [1 bit which input slot is the result], 2 bits mode, 8 bits weight × 255 |
| 1 | blend with weight 1 | [1 bit order], [1 bit result], 2 bits mode |
| 3, 0 | blend with the previous layer | 2 bits = how many slots back |
| 3, 2 | change between the base layer and another layer | nothing |
| 3, 1 | end of the stream | nothing |

   W = 1 when E ≤ 2, 2 when E ≤ 4, 3 when E ≤ 8, 4 when E ≤ 16, else 5. The receiver knows each animation's frame
   count (it has the same art groups), which tells it the phase width. The bits in square brackets of a blend are
   left out in the inconsistent cases of 4.16 (kept behaviour).

## 4. Behaviour

Float rule for this whole group: the old build did every float step of this group with single-precision
instructions (the `f_*` helpers of CONVENTIONS.h on 32-bit patterns, one rounding per operation); it ran nothing
of this group on the x87 unit, and float-to-integer conversion is the truncating single-precision instruction
(`f_to_int`). New code does the same, in the operation order given. "A × B" etc. below each mean one such
operation; brackets give the grouping. "Equal" for float patterns means the same 32 bits unless a comparison of
values is named.

### 4.1 `u3x_bsw(bs, val, n)`

With p = the stream position: when p + n is greater than the capacity, the overflow word is set to 1 and nothing
else happens (position unchanged, buffer unchanged). Otherwise the low n bits of `val` are stored at stream bits p
to p + n − 1 (3.1), higher bits of `val` are ignored, and the position becomes p + n.

Bits of the buffer after the written field: every bit of the word that holds the field's first bit and comes
after the field is set to 0; when the field continues into the next word, every bit of that next word after the
field is set to 0 too. Bits before the field are kept. The next word is not touched when the field ends in the
first word (also when it ends exactly at its last bit). Callers pass n from 1 to 32. (n = 0: the position stays
and the rest of the current word after p is cleared; never used.)

### 4.2 `u3x_fabs(f)` and `u3x_ppc_ctiwz(f)`

`u3x_fabs`: the pattern with bit 31 cleared.

`u3x_ppc_ctiwz`: the 32-bit signed integer (returned as u32) nearest to zero-ward truncation of the float, with
the PowerPC conversion's special cases:

- not a number (exponent field 0xff, fraction not 0): 0x80000000;
- magnitude 2³¹ or more (exponent field 0x9e or higher, infinities included): 0x7fffffff when positive,
  0x80000000 when negative;
- otherwise the value truncated toward zero (two's complement).

(The plain x86 conversion would give 0x80000000 for every out-of-range value; the low bits differ, and the packer
keeps low bits.)

### 4.3 `u3x_bsw_vec(bs, v)`

Reads the three patterns v[0], v[1], v[2]. The vector is **small** when |v[0]| < 255.0 and |v[1]| < 127.0 and
|v[2]| < 255.0 (ordered comparisons of the values with the sign bit cleared: a not-a-number is not small).

- Small: writes 1 bit 0, then 18 bits of `u3x_ppc_ctiwz(v[0] × 512.0)`, 17 bits of `u3x_ppc_ctiwz(v[1] × 256.0)`,
  18 bits of `u3x_ppc_ctiwz(v[2] × 512.0)` (the low bits of each integer: two's complement).
- Otherwise: writes 1 bit 1, then the three patterns, 32 bits each.

Every write goes through `u3x_bsw` (so an overflow sets the overflow word and the remaining writes are still
attempted, each on its own).

### 4.4 `u3x_bsw_frame(base, bs, f)`

Nothing when the overflow word is already non-zero. When the position + 512 is greater than the capacity: sets
the overflow word to 1, nothing else. Otherwise calls game 0x7befc0(bs, f).

### 4.5 Quaternions: `u3x_q_mul`, `u3x_q_rot`

Product P(a, b) of quaternions a and b (components x, y, z, w), each operation rounded, grouped exactly so:

- x = ((a.w × b.x) + (a.x × b.w)) + ((a.y × b.z) − (a.z × b.y))
- y = ((a.w × b.y) + (a.y × b.w)) + ((a.z × b.x) − (a.x × b.z))
- z = ((a.w × b.z) + (a.z × b.w)) + ((a.x × b.y) − (a.y × b.x))
- w = ((a.w × b.w) − (a.x × b.x)) − ((a.y × b.y) + (a.z × b.z))

Conjugate C(q): x, y and z with bit 31 flipped, w unchanged (a bit operation, also on zero and not-a-number).

Rotation R(q, v) of the vector v = (v0, v1, v2): P(P(q, V), C(q)) with V = the quaternion (v0, v1, v2, pattern 0).

`u3x_q_mul(o, a, b)` stores P(a, b). `u3x_q_rot(o, q, v)` stores the x, y, z of R(q, v) and then the pattern 0 as
the fourth word (the w of the product is dropped).

### 4.6 Locators: `u3x_loc_mul`, `u3x_loc_rel`

With a.pos, a.rot, b.pos, b.rot the position (first three words) and rotation (words 4 to 7) of the inputs:

- `u3x_loc_mul(o, a, b)`: position k = a.pos[k] + R(a.rot, b.pos)[k] for k = 0, 1, 2; rotation = P(a.rot, b.rot).
- `u3x_loc_rel(o, a, b)`: with I = C(a.rot) and d[k] = b.pos[k] − a.pos[k]: position = the x, y, z of R(I, d);
  rotation = P(I, b.rot). (Inside R the conjugate of I is a.rot again, bit for bit.)

Both store position words 0 to 2, the pattern 0 in word 3, the rotation in words 4 to 7. Word 3 of the inputs is
not read.

### 4.7 `u3x_bf_world(base, f, o)`

Parent locator: when `u3x_bf_handle_valid(base, f)` is non-zero and the body pointer (the pointer stored where
the pointer at f + 8 points) is not null, the 8 words at the start of that body; otherwise the 8 words at f + 0x20.
Result: `u3x_loc_mul(o, parent, the 8 words at f + 0x40)`. Both locators are read before `o` is written. The body
pointer is fetched only when the handle test said 1.

### 4.8 `u3x_bind_root(base, obj)`

`obj` must not be null (the virtual call is made first). B = virtual call 0x248 of `obj` with argument 0. Then,
for at most 64 objects, starting with `obj`:

- when the object's u16 at +0x128 has bit 0x800: the result is B;
- otherwise B = the body of the object's own bound frame (object + 0xb0), fetched as in 4.7: the body pointer
  when `u3x_bf_handle_valid(base, object + 0xb0)` is non-zero, else null; and the next object is game
  0x10c5010(object).

The result is null when the chain ends (next object null) or after 64 objects without the flag. Note that B can
be null in the first case too.

### 4.9 `u3x_ev_query(base, sid, obj)`

Calls game 0x1197380 with: a zero-filled result block of 0x20 bytes (16-byte aligned), null, `sid`, `obj`, and
four zero-filled argument blocks of 0x18 bytes each. Returns the first u64 of the result block.

### 4.10 `u3x_tab_find(t, key)` and `u3x_art_tab(g)`

`u3x_art_tab(g)`: null when `g` is null or its pointer at +0x30 is null; otherwise the pointer at +0x10 of that
block (may be null).

`u3x_tab_find(t, key)` (`t` not null): −1 when the entries pointer is null or N is 0 (no entry is read then).
Otherwise a bisection over the N entries: the probed index is always floor((low + high) / 2) with low = 0 and
high = N − 1 at the start; a probe equal to `key` ends the search with that index; a larger probe continues below
it, a smaller one above it; −1 when the range is empty. Only the first u32 of probed entries is read (which
entries are read, and which of several equal keys is found, follows from this probing order).

### 4.11 `u3x_anim_lookup(base, tab, idx, id, anim, tabs)`

Finds animation `id` for the actor whose 8 art groups are `tabs[0..7]`. `anim` (may be null) is the animation
object; when given, its art group name restricts the search.

1. For i = 0 to 7 in order, skipping null groups: when `anim` is not null, the group is skipped unless its id (u32
   at +0x18) equals `u3x_sid(base, anim's group name)` (asked again for every non-null group; a null or empty name
   gives 0). Then K = the index of `id` in the group's animation table (`u3x_art_tab`, then `u3x_tab_find`; −1
   without a table). The first group with K ≠ −1 ends the search: `*idx` = K, `*tab` = i, result 1.
2. Otherwise, when `anim` is null or its group name pointer is null: result 0.
3. Otherwise G = `u3h_find_art_group_name(base, name)` and K as above in G (−1 when G is null or has no table).
   K = −1: result 0. Else `*idx` = K, `*tab` = `u3x_sid(base, name)`, result 1.

### 4.12 `u3x_stream_root(base, x, phase)`

Looks for animation chunk `x` in the streaming animation records (3.3, 3.4): the first min(count, 50) records in
order (a count of 0 or less: none), skipping a null record and any record whose definition, channel-list
pointer, or one of the five arrays reached through them (counts, ids, slot numbers, last channel, channel lists)
is null. Inside a record: units u = 0 to U − 1; inside a unit its entries k = 0 to count[u] − 1; for entry (u, k)
the slot s = slot numbers[u][k]; the slot's channels (skipped when that pointer is null) c = 0 to last channel[s]
inclusive; a channel row that is null is skipped. The first row whose pointer number u equals `x` wins:

- `*phase` = the record's u32 at +0x20;
- result = game 0xfd62a0(u32 at x + 0x40, u32 at (pointer at x + 0x28) + 4, ids[u][k]), which may be null (the
  phase has been written all the same).

No match: null, `*phase` untouched, no game call.

### 4.13 `u3x_layer_replicated(base, id)`

0 when `id` is one of the ids or lies in one of the two ranges of 3.7, else 1. Pure apart from reading the four
image words.

### 4.14 `u3x_anim_pack_body`: frame of the work

`ec` = the diagnostic words (2.4). In this order:

1. `actor` null or `tabs` null: ec[66] += 1, result 0 (nothing else, the image byte is not touched).
2. Image byte 0x16e79f0 = 1. It is set back to 0 at the end of the call, whatever the result of the steps below.
3. Controller = pointer at actor + 0x138. Null: result 0.
4. W = `u3h_anim_cmd_buffer()`. Null: ec[74] += 1, result 0.
5. `*buf` = W.
6. Game 0xfbe270(controller, output block of 3.5 with buffer W and capacity 0x2000). n = the word count it left,
   limited to 0x2000.
7. When ec[0] has bit 0x1000: `u3h_loop_src119(W, n)`.
8. Write 1 bit 0.
9. The actor's frame (4.15).
10. When the message flags (u32 at msg + 0x148) have bit 0: the camera part (4.15); when it fails: result 0.
11. First pass over the commands (4.16). When it fails: result 0.
12. With E = number of table entries and R = number of animation uses noted: when R > 63 or E ≥ 32: ec[70] += 1,
    result 0.
13. Write the table and the commands (4.16), then the end code.
14. Result = the stream position (u32 at bs + 8).

A result 0 after step 8 leaves the bits written so far in the stream. Writes that do not fit only set the
overflow word; the steps go on.

**Reading the command list** (both passes): commands are taken in order from W, as long as the current command
starts before word n. Reading stops for good at the first command whose length is 0 or whose kind is 4 (layer
end); commands after it are never looked at.

### 4.15 The actor's frame and the camera part

**Actor's frame.** F0 = `u3h_repl_frame(0)`; the 0x60 bytes at actor + 0xb0 are copied into it.
A = `u3x_ev_query(base, 0x660778e0, actor)`. When the low 8 bits of A are 0, the actor is not attached and the
second query is not made. Otherwise ID = the low 32 bits of `u3x_ev_query(base, 0xfbd8cf95, actor)`; ID = 0 also
counts as not attached.

- Not attached: write 1 bit 0.
- Attached: write 1 bit 1, then ID in 13 bits. NP = `u3x_np_by_id(base, ID)`; CH = `u3h_handle_obj(NP + 0x10)`
  when NP is not null. When there is no CH: ec[67] += 1 (the frame stays as copied). With CH:
  T = `u3x_bf_world` of the frame at CH + 0xb0; O = `u3x_bf_world` of F0; REL = `u3x_loc_rel(T, O)` (the actor as
  seen from the carrying character); then game 0x1196230(F0, null); then the 8 words of REL are stored at
  F0 + 0x40.

Finally `u3x_bsw_frame(base, bs, F0)`.

**Camera part** (players). S = s32 at actor + 0x1c68. CM = `u3h_cam_mgr(base, S)` when S is 0 or 1, else none.
No CM: ec[68] += 1, the part fails (nothing written). Otherwise:

1. F1 = `u3h_repl_frame(1)`: all 0x60 bytes zero, then u32 +0 = 0x7fcccccc, u32 +4 = 0x7fcccccc, u32 +0x3c =
   1.0, and the 8 words at +0x40 = the 8 words at CM + 0x4c0.
2. ROOT = `u3x_bind_root(base, actor)`; when not null: game 0x1196230(F1, ROOT).
3. `u3x_bsw_frame(base, bs, F1)`.
4. Write 8 bits: 0 when CM's zoom object (pointer at CM + 0xc0) is null, else the low 8 bits of
   `u3x_ppc_ctiwz(u32 at zoom object + 0x30)`.
5. AIM = `u3h_repl_loc()`: its 0x40 bytes zero-filled, then virtual call 0x170 of the actor with (AIM, actor,
   0x3a83fc57).
6. `u3x_bsw_vec(bs, AIM)` (its first three words). The part succeeds.

### 4.16 The commands: two passes

Notions used by both passes, kept while walking the list from its start:

- **Layer number** L: −1 at the start, +1 at every layer mark (kind 0x19).
- **Muted**: false at the start; at every layer mark it becomes "not `u3x_layer_replicated(base, the mark's
  layer id)`" (one call per mark per pass).
- **Count in layer** C: 0 at the start, set to 0 at every layer mark, +1 at every counted command mark (kind
  0x1a).
- A command is **dropped** when muted is true or C ≥ `param6` (signed comparison). The blend has its own rule.

**First pass: which animations are used.** It builds the table (at most 64 entries, each a pair (index, art
group code)), the list of uses (at most 64 numbers) and the per-layer totals T[0..35] (all 0 at first).

- Kind 0x1a: additionally T[L] += 1 when 0 ≤ L < 36 (muted or not).
- Kind 8 or 0x1b, not dropped: X = the command's animation; SH = `u3x_stream_root(base, X, scratch)`, or X itself
  when that is null. `u3x_anim_lookup(base, &code, &index, u32 at SH + 0x18, SH, tabs)`. When it returns 0, or
  index is −1, or index does not fit in 12 bits (any of bits 12 to 31 set): ec[75] += 1, ec[76] = the u32 at
  SH + 0x18, the pass fails (no further command has any effect). Otherwise the pair (index, code) is **noted**.
- Kind 0xc, not dropped: POSE = 0x86dfee10 when `param5` ≠ 0, else 0x1a1c52d3. index = `u3x_tab_find` of POSE in
  `u3x_art_tab(tabs[0])` (−1 without a table). −1: ec[75] += 1, ec[76] = POSE, the pass fails. Otherwise the pair
  (index, 0) is noted (no 12-bit test here).
- Every other kind: nothing.

**Noting a pair**: its entry number is the number of the first equal table entry (both words equal); when there
is none, the pair is appended (entry number = the old entry count) unless the table already has 64 entries, in
which case nothing is appended and the entry number is 64. Then, when fewer than 64 uses are noted, the entry
number is appended to the list of uses.

After the first pass E = table entries, R = noted uses (step 12 of 4.14 refuses R > 63 and E ≥ 32).
**One entry per use** is true exactly when E = R.

**The table** (layout item 5 of 3.8): E in 5 bits; the one-entry-per-use bit; each entry's index in 12 bits (low
12 bits) in table order; the same-group bit = 1 when all entries have the same art group code (also when E is 0 or
1); then the art group code of entry 0 (same-group; for E = 0 that is code 0), or all E codes in order. A code
below 7 is written in 3 bits; any other value as 3 bits 7 followed by the value in 32 bits.

**Second pass: the commands.** Further state: the **last slot** LS (s32, 0 at the start), the **use number** U
(0), the **depth** D (signed, 0), and the id of the previous layer mark PL (0). "Depth check" below means:
when D > 5, ec[69] += 1 (nothing else). "Entry field" means: nothing when one-entry-per-use; otherwise the entry
number of use U (0 when U ≥ 64) written in W bits (W of 3.8), and ec[69] += 1 when that number is ≥ E.

- Kind 3: nothing.
- Kind 8 or 0x1b, not dropped: depth check; LS = the command's pose slot; write code 2 (kind 8) or code 3 and
  sub-code 5 (kind 0x1b); entry field; then the phase:
  - PH = the command's phase pattern; SH = `u3x_stream_root(base, X, &SP)`. When SH is not null:
    PH = SP × (float(frame count of SH) − 1.0). When null: SH = X.
  - F = frame count of SH (u16 at its clip data + 0x22). F < 9: 4 bits, scale 15.0. F > 64: 16 bits, scale
    65535.0. Otherwise 8 bits, scale 255.0.
  - Q = `u3x_ppc_ctiwz((PH × scale) ÷ (float(F) − 1.0))`. (F = 1 divides by zero: the conversion's special cases
    decide the bits. Integer to float is exact here.)
  - D += 1; write the low bits of Q in that width; U += 1.
- Kind 0xc, not dropped: depth check; LS = its pose slot; write code 2; entry field; D += 1; write 0 in the width the reader uses for that entry's animation (v7.242: 4, 8 or 16 bits by its frame count, as for kind 8; the animation is entry E's index in table `tabs[group]` when its group code is below 7, else 4 bits; PS3 wrote a fixed 4, too few when a PS4 pose animation has more than 8 frames);
  U += 1.
- Kind 0xd, not dropped: depth check; LS = its pose slot; write code 3, sub-code 3; D += 1.
- Kind 9 (blend). It is left out when muted, or when D < 2, or when C ≥ `param6` unless (0 ≤ L < 36 and C equals
  T[L]) (a blend after the layer's last counted command still goes out). Otherwise, with A, B, RES, mode and the
  weight pattern WT from the command:
  1. write 2 bits: 1 when WT is the pattern of 1.0, else 0;
  2. B < A: write 1 bit 1. A < B: write 1 bit 0. A = B: ec[69] += 1 and no bit;
  3. RES = A: write 1 bit 1, and D −= 1 when A < B. Else RES = B: write 1 bit 0, and D −= 1 when B < A. Neither:
     ec[69] += 1 and no bit (unsigned comparisons throughout);
  4. LS = RES; depth check;
  5. write 2 bits: 1 for mode 2, 2 for mode 6, 0 for every other mode;
  6. when WT is not the pattern of 1.0: write the low 8 bits of `u3x_ppc_ctiwz(WT × 255.0)`.
- Kind 0xa, not dropped: depth check; BACK = LS − its pose slot (32-bit wrap). When the low 2 bits of BACK are
  above D − 1 (as unsigned 32-bit numbers; D = 0 never is) nothing is written (v7.239: the receiver could not reach
  that slot; PS4 NPC command lists give such a reference after commands beyond the layer limit were left out).
  Else: write code 3, sub-code 0; when any of bits 2 to 31 of BACK is set: ec[69] += 1; write the low 2 bits of
  BACK. (LS and D stay.)
- Kind 0x1a: C += 1.
- Kind 0x19 with layer id ID: when PL or ID is 0x7339d08c ("base"): write code 3, sub-code 2 (also when the new
  layer is muted). Then C = 0, L += 1, muted is set from ID, PL = ID.
- Every other kind: nothing.

After the last command: write code 3, sub-code 1.

## 5. Test scenarios

Parts A and B need no game: plain memory in, memory and results out, with recording stubs for the few calls.
Part C runs on a fake image with fake objects and recording stubs for every game call, virtual call and `u3h_`
export of 2.4 (compare the stream bytes, the position, the overflow word, ec[], the image byte during and after,
`*buf`, and the order and arguments of all calls). Whether the receiving game accepts the stream needs the live
game (two players, a carried player, an NPC).

Part A:
1. `u3x_bsw`: n = 1, 7, 31, 32 at positions 0, 1, 31, 32, 33 on a buffer of all-one bits (shows which bits are
   cleared and that the next word is untouched when the field ends at a word's last bit); a field crossing two
   words; high bits of `val` set; exactly fitting the capacity; one bit too many (overflow word, nothing written);
   capacity not a multiple of 32.
2. `u3x_ppc_ctiwz`: ±0, ±0.99, ±1.5, 2147483520.0, 2³¹, −2³¹, values above, ±infinity, quiet and signalling
   not-a-number, denormals; thousands of random patterns.
3. `u3x_bsw_vec`: all three small; each component in turn at exactly its limit, just below, negative; a
   not-a-number component; negative values (two's complement in 17 / 18 bits); overflow in the middle.
4. `u3x_bsw_frame`: overflow word already set (no call); 511, 512 and 513 bits left.
5. `u3x_q_mul`, `u3x_q_rot`, `u3x_loc_mul`, `u3x_loc_rel`: identity inputs, random unit quaternions, random
   patterns including not-a-number and infinities (bit for bit), output aliasing each input; `u3x_loc_rel` of
   `u3x_loc_mul(a, b)` against a.
6. `u3x_bf_world`: handle invalid; valid with a body; valid with a null body pointer.

Part B:
7. `u3x_bind_root`: flag on the first object (result = the virtual call's value, also null); flag on the third;
   an object with an invalid frame handle before the root (null result with the flag found); chain ending; 64
   objects without flag (exactly 64 parent calls).
8. `u3x_ev_query`: the eight arguments, zeroed blocks, the stub's answer returned.
9. `u3x_tab_find`: N = 0, null entries, N = 1, N = 2, 3, 1000; key below all, above all, each present key,
   absent keys between, duplicate keys (the index found and the entries read must match).
   `u3x_art_tab`: null group, null block, null table.
10. `u3x_anim_lookup`: found in group 0, in group 7; null groups skipped; `anim` given with a matching and a
    non-matching group id; name null, empty; fallback group found / not found / without table; outputs untouched
    on 0; the number of string id calls.
11. `u3x_stream_root`: count 0, negative, 51 (only 50 looked at); null record; each of the seven null-pointer
    skips; match in the first and in a later unit, entry and channel; channel index equal to the last channel;
    null channel row; game call returning null (phase still written); no match.
12. `u3x_layer_replicated`: each of the eight names' ids, the four image words (with other values in the fake
    image too), both ends of each range and the values just outside, random ids; equality with the old function
    as said in 2.2.

Part C:
13. Refusals: null actor; null tabs; no controller; no command buffer; a player without camera manager (screen
    −1, 2, manager null); animation not found (lookup 0, index −1, index 0x1000); snapshot pose missing (both
    values of `param5`); R = 64 with E small; E = 32. The image byte is 0 afterwards in all of them and was 1
    during the game call.
14. Empty list (n = 0), list ending by length 0, by kind 4 with commands after it, by the word count; n above
    0x2000.
15. Not attached (first query 0; first query non-zero with id 0); attached with character (the frame's local
    locator, the unbind call), attached without net player and without character (ec[67]); id above 13 bits.
16. Camera part: with and without binding root, zoom null / a value above 255 / not-a-number, aim vector small
    and large.
17. Table: E = 0, 1, 2, 3, 5, 9, 17, 31; equal animations sharing an entry (one-entry-per-use false) and all
    different (true); all art group codes equal, different; a code of 7 or more (32-bit id form).
18. Commands: every kind; mirrored animation; frame counts 1, 2, 8, 9, 64, 65, 0xffff; streaming animation with
    its phase; snapshot; empty pose; blend with weight 1.0 and others, A < B, B < A, A = B, RES = A, B, neither,
    modes 2, 6, others; D < 2; blend with the previous layer with BACK 0 to 3 and outside; depth above 5;
    use numbers reaching entries ≥ E (only possible with a doctored list).
19. Layers: muted layers (their commands dropped in both passes, marks still counted); `param6` = 2 and 3 with
    more counted commands than that; the blend that follows the layer's last counted command (sent) and one that
    does not (left out); more than 36 layers; the base-layer code when entering and when leaving "base", also
    into a muted layer; commands before the first mark.
20. Overflow: a small capacity so that the frame, a vector and a command each overflow in turn (overflow word 1,
    the result is still the position, later steps still run).

## 6. Odd behaviour to keep

1. `u3x_bsw` clears the rest of the word(s) after the field instead of keeping it (the stream is written front
   to back into a zeroed buffer, so this is invisible there).
2. `u3x_anim_pack_body` returns the position even when the overflow word was set; a refusal after the first bit
   leaves a partly written stream. The caller handles both.
3. The whole list is read only up to the first layer end (kind 4) or empty command.
4. A blend with A = B, or with a result slot that is neither input, writes no order / result bit and only counts
   ec[69]; the stream is then not decodable. Kept as it is.
5. The entry number 64 can be noted when the table is full; step 12 of 4.14 refuses such streams anyway (E would
   be 64). The use list silently stops at 64 uses; step 12 refuses from 64 on.
6. The snapshot pose index is not checked against 12 bits; the animation index is.
7. The 13-bit attached id is cut from the full id, while the player is looked up with the full id.
8. The depth D is not reset at a layer mark and can go below what the commands justify; it only decides whether a
   blend is sent (D ≥ 2) and the ec[69] count.
9. `u3x_bind_root` makes its virtual call before any null test of the object.
10. `u3x_stream_root` writes the phase even when the game then returns no streamed animation; both passes then
    treat the animation as not streamed (the second pass uses the command's own phase).
11. In the camera part the scratch block is 0x40 bytes although only its first three words are sent.
12. The first pass asks `u3x_stream_root` with a scratch phase it never uses; the second pass asks again (two game
    calls per streamed animation).

## 7. What stays as it is

- Every exported name and signature of section 2 except `u3x_layer_counts` (removed with its C caller and the
  literal list, 2.2). The one-line C wrappers of section 1 stay.
- `u3x_sid`, `u3x_np_by_id` (G34), `u3x_bf_handle_valid` (G42), the C exports of 2.4: not part of this group.
- The game's bound-frame writer 0x7befc0 defines the frame bits; the helper does not reproduce it.
- The ids of 3.6 used as single numbers (events, query, poses, the base layer).

## 8. Open questions

- **Q1 (table).** The second id range of the layers that are never sent: 15 consecutive layer ids (the old code
  tests "first ≤ id ≤ last"). Meaning: a family of 15 layers whose ids the PS3 game makes from one base id plus an
  index, like the fire animation layers. The PS4 image contains neither bound as a 32-bit value, the PS3 1.19
  image has no table of them, and no name was found whose string id is the first value (the names in both
  executables, single words and word pairs in dash and capitalised forms were tried). Until a name is found the
  range cannot be generated; the integrator has to decide (a name, or the two bounds as numeric constants like
  the single ids of 3.6).
- **Q2.** The names behind the four layer ids read from image 0x52f018 .. 0x52f198, the event 0x660778e0 and the
  query 0x3a83fc57 are unknown; they are not needed (image words / single ids).
- **Q3.** The names of 3.7 written with capitals ("Hands", "Hair", "FireAnim") give exactly the old values; that
  they are the game's real layer names is an inference from the PS4 executable's texts, not needed for
  correctness.

## Clarifications

Added by the tester (2026-11-14). Facts only, confirmed by the old-versus-new test (reimpl2\G49\test_g49.cpp, -O1 and
-O2) where the text above is silent or can be made more exact. Nothing above was found wrong.

1. Float. Every float step of the old and the new code goes through the `f_*` helpers (single-precision SSE
   instructions in inline assembly), so the results do not depend on the compiler's options; nothing of this group
   runs on the x87 unit. `u3x_ppc_ctiwz` and `u3x_fabs` are equal for all 2^32 patterns; the quaternion and locator
   functions bit for bit on 4 million value sets each (not-a-number and infinities included), also with the output
   on any input and partly overlapping an input.
2. 3.7 / 4.13. `u3x_layer_replicated(base, id)` equals the old `u3x_layer_counts(id, list, 12)` and the old ranges
   for all 2^32 ids, with the real bytes of the PS4 image at 0x52eff0 .. 0x52f200 and `u3h_layer_range_first()`
   giving the old range's first id: exactly 32 ids answer 0 (12 single ids, 5 + 15 in the ranges). Reading the four
   image words at every call (as the new code does) or once is not observable. The new code tests a range as
   "id − first < count" (32-bit wrap): it differs from "first ≤ id ≤ last" only for a first id within 14 of 2^32,
   which the old value is not.
3. 4.1 / 4.4, wrap of the position. The text's "p + n greater than the capacity" holds without 32-bit wrap in the
   new code; the old code added in 32 bits, so with a position within n bits (or 512 bits for `u3x_bsw_frame`) of
   2^32 the old code wrote (or called the game's writer) where the new one sets the overflow word. Positions never
   come near 2^32 (the capacity is the size of a real buffer), so callers cannot see this.
4. 4.1, n above 32 (never used): old and new store different bits. Callers pass 1 to 32 only, as stated.
5. 4.11. The new `u3x_anim_lookup` reads the animation's group-name pointer (anim + 0x38) once at the start; the old
   one read it again for every group and for the fallback. Only visible if a string-id or art-group-name call
   changed that pointer during the lookup, which nothing does.
6. 4.10. Tables whose last entry ends at an unmapped page: old and new read the same entries (82 942 searches).
7. Round trip: the new packer's streams (10 528, of every shape the random cases make) decoded by the old and by the
   new G50 decoder give the same visitor calls and memory in all three visitor kinds.
