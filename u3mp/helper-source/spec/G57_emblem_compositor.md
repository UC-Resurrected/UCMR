# G57. Emblem compositor and wall emblems

Behaviour description for a reimplementation (reimpl2 pipeline, describer's document). It says what the functions
do, not how the old ones were written. Offsets are hexadecimal when prefixed with 0x. "Image offset X" means the
address `base + X`, where `base` is the game image base. All multi-byte values are little-endian; "u8 / u16 / u32 /
u64 / s32" are unsigned / signed integers of that width, "pointer" is 8 bytes, "f32" is a float held as its 32-bit
pattern.

Neighbour specs: `G56_*.md` (emblem maths, texture records and CPU rasterizer: it owns the record layout, the
decoded-texture block, the temp memory, the sine / cosine, the fp16 conversion and the two rasterizer entry points)
and `G58_*.md` (emblem manager state machine and character binds: it owns the emblem manager's state changes, the
record slots' contents and the team player lookup). Every fact of theirs this group relies on is repeated in
section 3 so this document can be used alone.

Float arithmetic: all of it is single precision, one rounding per operation, on 32-bit patterns (the `f_*` tools of
CONVENTIONS.h). The old code of this group already computed every float step with single-precision instructions
(scalar SSE add, subtract, multiply, divide and integer-to-float conversion; the "multiply-add" steps as
`f_madd`: both inputs widened to double, product and sum in double, ONE rounding to single). Nothing in this group
ran on the x87 unit, and nothing in this group converts a float to an integer (only integers to floats). The new
results are therefore expected to be bit-identical to the shipped helper, with no accepted deviation.
In every formula: a + b = `f_add(a, b)`, a − b = `f_sub(a, b)`, a · b = `f_mul(a, b)`, a / b = `f_div(a, b)`,
fma(a, b, c) = `f_madd(a, b, c)` = a · b + c with one rounding, int(n) = `f_from_int(n)`, neg(a) = the pattern of a
with bit 31 inverted (not a subtraction). Keep the operand order given (it decides which NaN payload comes out; the
test compares bit for bit).

## 1. Purpose and callers

PS3 1.19 draws each player's emblem (four layers: icon, angle, scale, colour) into two small render targets on the
GPU, shows them on the characters, and shows the leading team's emblem on boards and walls of the map. The PS4 game
has the emblem manager, the layer data and the icon textures, but no such pass. The helper does the pass on the
CPU. This group is two independent pieces of it:

- **Part A, the compositor.** `u3x_emb_render` plans and draws the four layers of the emblem manager's current
  emblem into a pixel buffer and makes it the texture of one emblem record. `u3x_emb_pass` runs it for the two
  record slots. (The per-pixel work, the texture files and the record's texture are G56's functions; this group
  decides what is drawn, with which corner coordinates and colours, and when nothing needs to be redrawn.)
- **Part B, the wall emblems.** `u3x_wall_emblems` walks the map's simple objects once per frame and writes the
  emblem texture of the leading team's player (or of the local player) into the render tables of every object whose
  art name contains "-multi-emblem-".

Callers (all stay as they are):

- `u3x_emb_render(b, s, sl, rec, size)`: the C one-line wrapper `emb_render119` (no caller of its own left) and
  `u3x_emb_pass`.
- `u3x_emb_pass(b, s)`: the C one-line wrapper `emb_pass119`, reached through the export
  `u3h_emb_frame_parts(b, s, 2)`, which G58's per-frame `u3x_emblem_tick` calls once per frame, right after the
  emblem manager update, on the game's main thread, only while the emblem port is on and multiplayer is active.
- `u3x_wall_emblems(b)`: the C one-line wrapper `emb_wall119`, called by the helper's per-frame tick (stub kind 16)
  once per frame on the game's main thread whenever the image base is known, straight after `emb_tick119`.

Everything here runs on the main thread; there is no locking.

## 2. Interface

### 2.1 Functions to deliver

Part A:

| Name | Signature | Notes |
|---|---|---|
| `u3x_emb_render` | `extern "C" void u3x_emb_render(uintptr_t b, void *s, uint32_t sl, void *rec, uint32_t size)` | name and signature kept. `b` image base, `s` the helper's state (only handed on to `u3h_emb_h`), `sl` record slot 0 or 1, `rec` the emblem record to fill (never null, a record of G56's table), `size` the target's edge in pixels (0x80 or 0x100). |
| `u3x_emb_pass` | `extern "C" void u3x_emb_pass(uintptr_t b, void *s)` | name and signature kept (stray, see section 6). |
| `u3x_emb_layer_uv` | `extern "C" void u3x_emb_layer_uv(uint32_t *uv, uint32_t s, uint32_t cs, uint32_t sn)` | NEW exported name for a piece that was file-private: the eight corner values of a layer (4.3). Pure. `uv` receives 8 f32 patterns; `s`, `cs`, `sn` are f32 patterns (scale, cosine, sine). |
| `u3x_emb_drip_quad` | `extern "C" void u3x_emb_drip_quad(uint32_t *q, const uint8_t *point, uint32_t grid, uint32_t s, uint32_t cs, uint32_t sn, uint32_t half)` | NEW exported name for a piece that was file-private: the eight values of one drip quad (4.4). Pure. `q` receives 8 f32 patterns; `point` is one 12-byte decal point; `grid`, `s`, `cs`, `sn`, `half` are f32 patterns. |

The two new exports exist so the old-versus-new test can call the maths alone; nobody outside this group calls them.

Part B:

| Name | Signature | Notes |
|---|---|---|
| `u3x_wall_emblems` | `extern "C" void u3x_wall_emblems(uintptr_t b)` | name and signature kept. |

The C wrappers already are one line each and pass their arguments unchanged; no new C wrapper is needed.

### 2.2 Functions of other groups and of the C part that this group calls

Declare them exactly so (`U3H_IMPORT` functions are exports of the C part; most forward to a G56 or G58 function).

G56 (through the C part's exports; behaviour is G56's):

- `uint8_t *u3h_emb_h(uintptr_t b, void *s, uintptr_t handle)`: resolves the game's data handle at image offset
  `handle` and returns the address of the data item it names, or null when it is not available.
- `uint8_t *u3h_emb_cache(uintptr_t b, const char *name)`: asks the game's texture cache for the texture `name`
  (this also requests its load); returns the texture's info block (3.4) or null.
- `void *u3h_emb_alloc(uintptr_t b, uint64_t n)`: `n` bytes of engine temp memory, or null.
- `void u3h_emb_free(uintptr_t b, void *p)`: gives such memory (or a decoded texture's file memory) back; null is
  accepted by it, but this group only calls it with non-null pointers.
- `int u3h_emb_dds(uintptr_t b, const char *name, void *dds)`: loads the texture file `name` and fills the
  88-byte decoded-texture block `dds` (3.5). Returns 1 on success; on failure 0, and the block's file pointer is
  null (and G56 has counted the failure itself).
- `void u3h_emb_quad(uint8_t *pix, uint32_t size, const void *dds, const uint32_t *uv, const void *mask_dds, const uint32_t *c16)`:
  draws one layer over the whole target from the 8 corner values `uv`, optionally through a mask (null = none),
  with the colour `c16` (three u32: blue, green, red, each a value · 65536).
- `void u3h_emb_decal(uint8_t *pix, uint32_t size, const void *dds, const uint32_t *q, const uint32_t *c16)`:
  draws one axis-aligned quad from the 8 values `q`.
- `int u3h_emb_texcreate(uintptr_t b, void *rec, const uint8_t *pix, uint32_t size)`: makes a texture of the
  `size` × `size` 4-byte pixels and stores it in the record (fields +0, +8, +0x10, +0x14, +0x18, +0x1c; the old
  texture is retired by G56). Returns 1 on success, 0 on failure (G56 has then written its own failure code).
- `uint32_t u3h_sincos(uint32_t x, uint32_t cosine)`: sine (cosine = 0) or cosine (cosine = 1) of the f32 pattern
  `x` (radians), as an f32 pattern. Pure.
- `uint32_t u3h_h16q16(uint32_t f)`: the f32 pattern rounded to a 16-bit float, as value · 65536. Pure.
- `int u3h_emb_is_record(uint64_t p)`: 1 when `p` is the address of one of the 24 emblem records. Pure.
- `uint8_t *u3h_emb_records(uint8_t **flag)`: returns the record table and stores, through `flag`, the address of
  the helper's "render wanted" byte (G58 sets and clears it).

G58:

- `uint8_t *u3h_emb_team_player(uintptr_t b, int32_t team)` (= G58's `u3x_emblem_team_player`): the net player
  record of team `team`'s emblem player, or null.

Functions that stay as they are (trivial accessors in the old C++ file; call them, do not write them again):

- `extern "C" uint8_t *u3x_emb_rec_slot(uintptr_t b, uint32_t sl)`: the record the manager holds for slot `sl`
  (the u64 at manager +0x50 + 8·sl, or when that is 0 the u64 at manager +0x68 + 8·sl), possibly null.
- `extern "C" uint32_t u3x_emb_size(uintptr_t b)`: 0x80 when manager +0x50 or +0x58 is non-zero, else 0x100 when
  manager +0x68 or +0x70 is non-zero, else 0.

C part, own state of the helper:

- `uint32_t *u3h_emb_wall_diag(void)`: four u32 counters `ew[0..3]` of the wall emblems.
- `uint8_t *u3h_ext(void)` (CONVENTIONS.h): the diagnostic and switch words `ec[]` are the u32 array at
  `u3h_ext() + 0x4450` (the old file has a private accessor for it; write `ec[n]` as the n-th u32 there).

Words of `ec[]` used here: `ec[0]` switches (bit 14 = 0x4000: emblem port off; bit 15 = 0x8000: rows upside down,
a test aid, off by default); `ec[113]` composites made; `ec[114]` passes that ended with "try again"; `ec[122]`
last failure code (this group writes 4 = no pixel memory); `ec[124]` composites skipped as unchanged; `ec[125]`
size of the last composite; `ec[147]` wall objects given a player's record; `ec[148]` wall objects given the
default; `ec[149]` / `ec[150]` covered pixels of the last composite of slot 0 / slot 1; `ec[151]`, `ec[152]`
wall diagnostics (4.6).

## 3. Game facts

### 3.1 Game functions called

| Image offset | Convention | Meaning |
|---|---|---|
| 0x66bc30 | `uint8_t *f(uint8_t *manager, int32_t layer)` (rdi, esi; result rax) | the emblem manager's "texture of layer n": picks the layer's icon from the layer data, asks the texture cache for it (requesting the load) and returns the texture's info block (3.4) or null. It reads the same layer data and icon lists as 4.1 by itself. |
| 0x737eb0 | `int32_t f(uint8_t *net_game_manager)` (rdi; result eax) | the team in the lead (−1 on a tie). |
| 0x11a4580 | `uint8_t *f(uint8_t *object, uint8_t *root)` (rdi, rsi; result rax) | the next object of the process tree after `object` in a depth-first walk that stays below `root`; null at the end. Reads only. |
| object virtual slot 0xe8 | `uint8_t *f(uint8_t *object)`: the function pointer is the u64 at (u64 at object +0) + 0xe8 | the object's art (spawner) description, or null. |
| object virtual slot 0x2c0 | `int32_t f(uint8_t *object)`: the function pointer at (u64 at object +0) + 0x2c0 | the number of render tables of the object. |

### 3.2 Game data

| Image offset | Type | Meaning |
|---|---|---|
| 0x17c3440 | block | the emblem manager (3.3) |
| 0x17cb060 | block | the net game manager; the pointer at +0x370 (image 0x17cb3d0) is the net player record of the first local player, or null |
| 0x17d1868 | s32 | net state; 2 = in a match |
| 0x1b37610 | block | the process tree roots; the pointers at +0xd0 and +0xd8 are the two roots the map's simple objects hang under |
| 0x1b67e60 + n · 0x18, n = 0..3 | data handle | the icon list of layer n |
| 0x1b67ec0 | data handle | the colour palette |
| 0x1b67ee0 + n · 0x18, n = 0..3 | data handle | the scale range of layer n |

The handles are passed to `u3h_emb_h` as image offsets; this group never reads a handle's own fields.

### 3.3 The emblem manager (image 0x17c3440), fields used here

| Offset | Type | Meaning |
|---|---|---|
| +2 | u8 | "try again": something a composite needs was not ready |
| +8 + 8·n | 8 bytes | layer n of the emblem editor (used when the s32 at +0x60 is negative) |
| +0x2c + 8·n | 8 bytes | layer n of the player being rendered (used when the s32 at +0x60 is 0 or more) |
| +0x50, +0x58 | u64 | records of slots 0 and 1 in player mode (128 pixels) |
| +0x60 | s32 | player index; negative = editor mode |
| +0x68, +0x70 | u64 | records of slots 0 and 1 in editor mode (256 pixels) |
| +0x78 | u64 | the default (empty) record |

A layer's 8 bytes: byte 0 icon number, byte 1 angle, byte 2 scale, byte 4 colour number; bytes 3, 5, 6, 7 have no
meaning here but belong to the composite's key (4.2).

### 3.4 Texture info block (what 0x66bc30 and `u3h_emb_cache` return)

u8 at +0x10: non-zero when the texture is loaded. Zero-terminated text at +0x20: the texture's name, the one
`u3h_emb_dds` takes. "Loaded" below means: the pointer is not null and the byte at +0x10 is not 0.

### 3.5 Structures owned by G56 (stated here because this group fills or reads them)

Emblem record, 80 bytes: u64 texture +0; u64 map +8 (non-zero = the record owns a texture made by a composite);
u8 loaded +0x10; u32 width +0x14, height +0x18, format +0x1c; u32 size +0x20; u16 pin +0x24; u16 used +0x26;
**40-byte key +0x28**. This group reads +0 (Part B), +8 and the key, and writes only the key; everything else is
written by `u3h_emb_texcreate`.

Decoded-texture block, 88 bytes: pointer to the file memory +0 (null = nothing loaded); u32 width +8, height +0xc,
last level +0x10, block size +0x14; sixteen u32 level offsets +0x18. This group provides the storage (on its
stack), sets the file pointer of the mask's and the drips' block to null before use, reads only the file pointer,
and frees the file memory with `u3h_emb_free`.

### 3.6 Game data items (what `u3h_emb_h` returns for the handles of 3.2)

- Icon list: s32 count +0; pointer +8 to the entries, 0x18 bytes each; the pointer at entry +0x10 is the icon's
  decal set or null.
- Decal set: s32 grid +0; s32 point count +4; pointer +8 to the points, 12 bytes each: u32 cell +0, s32 x +4,
  s32 y +8. The cell names one of the 4 × 4 pictures of the drips texture: column = cell bits 0..1, row counted
  from the bottom = cell >> 2.
- Palette: s32 count +0; pointer +8 to the entries, 12 bytes each: three f32 red, green, blue.
- Scale range: f32 lowest +0, f32 highest +4.

### 3.7 Net player record, map objects

Net player record (0x9c0 bytes, G58): u64 board emblem record +0x7e8; u64 layer emblem record +0x7f0; u32 +0x980
(the first word of the player's emblem layers; copied to a diagnostic only).

Map object (process): pointer to its virtual table +0; u32 type id +0x30; u16 flags +0x40; eight pointers to
render tables from +0x240 (8 bytes each); a render table holds the u64 texture it shows at +8. The art description
(virtual slot 0xe8) has the pointer to its zero-terminated name at +0x48.

### 3.8 Ids, texts and constants

- Object type ids that carry wall emblems: 0x2db62a42 and 0xb95beddb.
- Name mark: the 14 characters `-multi-emblem-`. Board mark: the three characters `boa` straight after it.
- Texture names: `emblems/spray-01` (the mask of slot 1) and `emblems/emblem-16-drips` (the drips).
- Float constants: 0x3f800000 = 1.0; 0x3f000000 = 0.5; 0xbf000000 = −0.5; 0x3e800000 = 0.25; 0x3e000000 = 0.125;
  0x3d800000 = 0.0625; 0x3b808081 = 1/255 (0.003921569); 0x3cc9d9b5 = 2π/255 (0.024639944).
- Key version byte: 1.

This group holds no table of game values (the handle list with the data ids is G56's, the bind table).

## 4. Behaviour

### 4.1 `u3x_emb_render`: planning

M = the emblem manager. When `sl` is 0, M's "try again" byte (+2) is first set to 0 (slot 1 leaves it).

Then, for the layers n = 0, 1, 2, 3 in this order, each layer completely before the next. The calls of one layer
are made in exactly the order of the steps, and the steps 1 to 5 are made for every layer whatever the results:

1. The layer's data D(n): M +8 + 8n when the s32 at M +0x60 is negative, else M +0x2c + 8n.
2. `u3h_emb_h(b, s, 0x1b67e60 + n·0x18)` = the icon list, or null. icon(n) = (byte 0 of D(n)) modulo the list's
   count when there is a list and its count is above 0, else 0.
3. `u3h_emb_h(b, s, 0x1b67ec0)` = the palette, or null (asked again for every layer). Colour number = (byte 4 of
   D(n)) modulo the palette's count when there is a palette and its count is above 0, else 0. The layer's colour
   (red, green, blue) = the three f32 patterns of that palette entry; all three are 0 when there is no palette or
   its entry pointer is null.
4. `u3h_emb_h(b, s, 0x1b67ee0 + n·0x18)` = the scale range, or null; lo and hi are its two patterns, both 0 when
   null. Then
   - scale(n) = fma(hi − lo, int(byte 2 of D(n)) · 0x3b808081, lo): the difference and the product in the first two
     arguments are each rounded to single first;
   - angle(n) = int(byte 1 of D(n)) · 0x3cc9d9b5.
5. T(n) = game 0x66bc30(M, n). Then K = `u3h_emb_cache(b, "emblems/spray-01")` (asked for every layer and for both
   slots; the last answer is the one used in 4.2).
6. The layer's plan starts as 0 and its decal set as "none". When icon(n) is 0, the layer is finished (not drawn).
7. When T(n) is not loaded, or `sl` is 1 and K is not loaded: M +2 = 1 and the layer is finished (not drawn).
8. Otherwise plan(n) = 1 (drawn). Its decal set = the pointer at +0x10 of entry icon(n) of the icon list (entries
   are 0x18 bytes), or none when the list's entry pointer is null. Then R = `u3h_emb_cache(b,
   "emblems/emblem-16-drips")` (asked for every drawn layer, for both slots; the last answer is the one used).
   Only when `sl` is 1 and the layer has a decal set: when R is not loaded, M +2 = 1; else plan(n) = 3 (drawn,
   with drips).

### 4.2 `u3x_emb_render`: key, drawing, texture

**Key.** 40 bytes: bytes 0..31 = the 8 bytes of D(0), D(1), D(2), D(3) as they are now; byte 32 = low byte of
`sl`; byte 33 = low byte of (`size` >> 8); byte 34 = plan(0) | plan(1) << 2 | plan(2) << 4 | plan(3) << 6;
byte 35 = 1; bytes 36..39 = 0.

**Unchanged.** When the record's map field (+8) is not 0 and the record's 40 key bytes (+0x28) equal this key:
`ec[124]` + 1 and return. Nothing else happens (M +2 keeps whatever planning left in it).

**Pixel buffer.** bytes = `size` · `size` · 4 (64-bit). P = `u3h_emb_alloc(b, bytes)`. When null: `ec[122]` = 4,
M +2 = 1, return. Else all its bytes are set to 0. A pixel is 4 bytes; rows are `size` pixels, top row first; byte
3 of a pixel is its alpha.

**Layers.** Two decoded-texture blocks, "mask" and "drips", start with a null file pointer. A flag "complete"
starts as true. For n = 0..3 in order, only layers whose plan has bit 0:

1. `u3h_emb_dds(b, T(n) + 0x20, &t)` into a block t of the layer. On failure: M +2 = 1, complete = false, next
   layer.
2. Only when `sl` is 1 and the mask block has no file yet: `u3h_emb_dds(b, K + 0x20, &mask)`. On failure:
   `u3h_emb_free(b, t's file)`, M +2 = 1, complete = false, next layer. (A later layer tries the mask again.)
3. cs = `u3h_sincos(angle(n), 1)`, then sn = `u3h_sincos(angle(n), 0)` (cosine first).
4. The eight corner values uv = 4.3 with scale(n), cs, sn.
5. The colour c16, three u32, converted in this order: c16[0] = `u3h_h16q16(blue)`, c16[1] =
   `u3h_h16q16(green)`, c16[2] = `u3h_h16q16(red)`.
6. `u3h_emb_quad(P, size, &t, uv, sl == 1 ? &mask : null, c16)`, then `u3h_emb_free(b, t's file)`.
7. Only when the plan has bit 1 (drips):
   - when the drips block has no file yet: `u3h_emb_dds(b, R + 0x20, &drips)`; on failure M +2 = 1,
     complete = false, next layer;
   - from the layer's decal set: count (s32 +4), points (pointer +8), grid = int(s32 +0);
     half = fma(scale(n), 0x3d800000, 0x3d800000);
   - when the points pointer is not null, for each point k = 0 .. count − 1 in order (none when count is 0 or
     negative): q = 4.4 for that point, then `u3h_emb_decal(P, size, &drips, q, c16)`.

After the last layer: when the mask block has a file, `u3h_emb_free(b, mask's file)`; then the same for the drips
block.

**Upside down.** Only when `ec[0]` has bit 15: row y is exchanged with row `size` − 1 − y for every y below
`size` / 2 (whole rows, pixel for pixel).

**Coverage.** `ec[149 + (sl & 1)]` = the number of pixels of P whose alpha byte is not 0.

**Texture.** `u3h_emb_texcreate(b, rec, P, size)`. On success: the record's key (+0x28, 40 bytes) = the key when
complete, else 40 zero bytes (so the next pass draws again); `ec[113]` + 1; `ec[125]` = `size`. On failure:
M +2 = 1 and the key is left as it was. In both cases `u3h_emb_free(b, P)` last.

### 4.3 Layer corners (`u3x_emb_layer_uv`)

Inputs: scale s, cosine cs, sine sn (patterns). Every operation below is one single-precision operation, in the
grouping shown.

A = 0x3f000000 / s; B = 0xbf000000 / s (a second division, not a negation of A).
C = A · cs; S = A · sn; S′ = B · sn; C′ = B · cs.
Ch = C + 0.5; Sh = S + 0.5; S′h = S′ + 0.5; C′h = C′ + 0.5 (the product is the left operand).

| Output | Value |
|---|---|
| uv[0] | S + C′h |
| uv[1] | 1.0 − (C′ + S′h) |
| uv[2] | S′ + C′h |
| uv[3] | 1.0 − (C + S′h) |
| uv[4] | S′ + Ch |
| uv[5] | 1.0 − (C + Sh) |
| uv[6] | S + Ch |
| uv[7] | 1.0 − (C′ + Sh) |

(The four pairs are the texture coordinates of the target's corners for a picture centred on (0.5, 0.5), turned by
the angle and shrunk by the scale, with the vertical coordinate mirrored. For G56's `u3h_emb_quad` pair 1 =
uv[2], uv[3] is the target's origin corner, pair 2 the end of the horizontal edge, pair 0 the end of the vertical
edge; pair 3 is not read by it but must be written.)

### 4.4 One drip quad (`u3x_emb_drip_quad`)

Inputs: a 12-byte point (u32 cell, s32 x, s32 y), and the patterns grid, s, cs, sn, half.

- uc = fma(int(cell & 3), 0.25, 0.125).
- vc = fma(int(3 − (cell >> 2)), 0.25, 0.125); the subtraction is done on unsigned 32-bit values and the result is
  taken as a signed 32-bit integer (the shift is not masked).
- yn = int(y) / grid, then yn = (yn + yn) − 1.0. xn the same from x.
- ys = yn · s; xs = xn · s.
- yr = fma(xs, sn, ys · cs); xr = fma(xs, cs, neg(ys · sn)). The products inside are rounded to single first.
- yr = fma(yr, 0.5, 0.5); xr = fma(xr, 0.5, 0.5).

| Output | Value |
|---|---|
| q[0] | xr − half |
| q[1] | xr + half |
| q[2] | yr − half |
| q[3] | yr + half |
| q[4] | uc − 0.125 |
| q[5] | uc + 0.125 |
| q[6] | 1.0 − (vc + 0.125) |
| q[7] | 1.0 − (vc − 0.125) |

### 4.5 `u3x_emb_pass`

1. `u3h_emb_records(&flag)`; when the byte `flag` points at is 0, return.
2. For sl = 0, then 1: r = `u3x_emb_rec_slot(b, sl)`. Skip the slot when r is null or `u3h_emb_is_record(r)` is 0.
   n = `u3x_emb_size(b)`; skip the slot when n is 0. `u3x_emb_render(b, s, sl, r, n)`. Then, when the manager's
   "try again" byte (+2) is not 0, `ec[114]` + 1.

### 4.6 `u3x_wall_emblems`

1. When `ec[0]` has bit 14, return. ew = `u3h_emb_wall_diag()`; `ew[0]` + 1.
2. The player P whose emblem is shown: when the net state (image 0x17d1868) is 2, P =
   `u3h_emb_team_player(b, game 0x737eb0(b + 0x17cb060))`. When that was not asked or gave null, P = the pointer at
   image 0x17cb3d0. When P is then not null but fails the pointer test (`ptr_ok` of CONVENTIONS.h: at least
   0x10000, below 2^47, a multiple of 8), P = null (no second choice).
3. F = the u64 at manager +0x78 (the default record), read once.
4. For the two roots, the pointer at image 0x1b37610 + 0xd0 and then the one at + 0xd8: skip a root that fails the
   pointer test. Walk: start with the root as the current object and repeat at most 16384 times: current = game
   0x11a4580(current, root); stop this root when the result is null. (The root itself is never looked at.) For
   each object O so obtained, in this order of tests:
   1. Skip unless (u16 at O +0x40) & 5 equals 1.
   2. Skip unless the u32 at O +0x30 is 0x2db62a42 or 0xb95beddb.
   3. `ew[3]` + 0x10000.
   4. A = virtual slot 0xe8 of O. Skip when A is null or fails the pointer test.
   5. N = the pointer at A +0x48. Skip when null.
   6. Look for the mark `-multi-emblem-` in N: try the start positions 0, 1, 2, ... while the position is below 40
      and the character there is not the end of the text; the first position where all 14 characters match wins
      (the match itself may run past position 40). Skip when there is none.
   7. "Board" = the three characters after the mark are `b`, `o`, `a` (read one at a time, stopping at the first
      that differs).
   8. R = 0 when P is null, else the u64 at P +0x7e8 (board) or P +0x7f0 (others). When R is 0, or
      `u3h_emb_is_record(R)` is 0 (asked only when R is not 0): R = F and `ec[148]` + 1. Else `ec[147]` + 1.
   9. When P is not null: `ec[151]` = the u32 at P +0x980; `ec[152]` = the low 32 bits of P's address.
   10. X = the u64 at R +0 when R is not 0 and `u3h_emb_is_record(R)` is not 0 (asked again, for the record chosen
       in step 8, only when it is not 0), else 0. When X is 0: `ew[3]` + 1 and skip.
   11. c = virtual slot 0x2c0 of O (signed). For i = 0 .. min(c, 8) − 1: t = the pointer at O +0x240 + 8i; when
       not null, the u64 at t +8 = X.
   12. `ew[1]` + 1 when at least one table was written, else `ew[2]` + 1.

`ew[3]` so holds two counters: objects without any record in its low 16 bits (as long as it does not overflow) and
candidate objects seen above them.

## 5. Test scenarios

All of this group can be tested without the game: a fake image block (manager, handles' data, net game manager,
roots), recording stubs for the three game functions and the two virtual slots, and recording stubs for the `u3h_`
exports (or G56's real functions for a pixel comparison). Compare memory (record key, manager +2, `ec[]`, `ew[]`,
render tables), and the order and arguments of every call.

Pure maths (bit for bit, old against new, thousands of random patterns plus the special values 0, −0, tiny, huge,
infinity, NaN with different payloads in each argument):

- `u3x_emb_layer_uv`: random s, cs, sn; s = 0 (infinite A and B), s = NaN.
- `u3x_emb_drip_quad`: cells 0..15 and cells above 15 (negative row), x and y negative, 0, above grid; grid 0.

Compositor (`u3x_emb_render`):

1. Editor mode (+0x60 negative) and player mode (0 or more): the layer bytes are taken from +8 / +0x2c.
2. Icon list missing, count 0, count 1 (icon always 0: nothing drawn, yet a texture of zero pixels is made and
   the key stored), counts above the icon byte and below it (modulo).
3. Palette missing, entry pointer null, count 0; range missing (scale 0).
4. Slot 0 with all four layers loaded: four quads, no mask, no decals even when the icons have decal sets; the
   drips texture is still asked from the cache for each drawn layer.
5. Slot 1: mask not loaded (no layer drawn, +2 = 1); mask loaded; an icon with a decal set and the drips loaded
   (plan 3) or not loaded (plan 1, +2 = 1); decal set with count 0, negative count, null points.
6. A layer's texture not loaded: that layer is left out, +2 = 1, the others are drawn, the key carries plan 0 for
   it.
7. Slot 0 resets +2 at entry, slot 1 does not.
8. Second call with the same inputs and a record that owns a texture: `ec[124]` + 1, no allocation, no texture;
   with the record's map 0: drawn again. Any single key input changed (a layer byte, including bytes 3, 5, 6, 7;
   slot; size 0x80 against 0x100; one plan): drawn again.
9. `u3h_emb_alloc` returns null: `ec[122]` = 4, +2 = 1, nothing else.
10. `u3h_emb_dds` fails for a layer's file; for the mask (the layer's file is freed, the next layer asks for the
    mask again); for the drips (the quad was drawn, the decals are not). In each case the texture is still made
    and the stored key is 40 zero bytes.
11. `u3h_emb_texcreate` fails: +2 = 1, key untouched, pixel memory freed.
12. `ec[0]` bit 15: rows reversed, for both sizes. Coverage counter in `ec[149]` / `ec[150]`.
13. Call order per layer in planning (icon list, palette, range, 0x66bc30, mask cache, drips cache) and in
    drawing (file, mask, cosine, sine, blue, green, red, quad, free, drips file, decals), and the frees at the
    end (mask, then drips, then the pixel memory last).

Pass (`u3x_emb_pass`): flag 0 (no call); slot record null; not a record; size 0; both slots rendered;
`ec[114]` counted per slot while +2 is set.

Wall emblems (`u3x_wall_emblems`): switch bit 14; net state 2 with a team player, with none (local player),
other net states; local player pointer null or misaligned; a root null / misaligned; an empty tree; objects with
the wrong flags or type; art null / misaligned; name null; names without the mark, with it at position 0, 39, 40
(not found), a name shorter than the mark; `-multi-emblem-board` against `-multi-emblem-wall`; player record 0,
not a record, a record with texture 0 (and default too); default 0; table counts 0, 3, 8, 12 (only 8 written),
negative; null table pointers (none written: `ew[2]`); more than 16384 objects under one root (the walk stops).

Needs the live game: only that the emblems appear right way up on the characters, the editor preview and the map
boards (the meaning of the corner order and of the table writes).

## 6. Strays, and what stays as it is

Covered here although the inventory does not list them in G57:

- the layer corner maths (old file-private `layer_uv`, in no inventory list) = 4.3, delivered as
  `u3x_emb_layer_uv`;
- the drip quad maths (old file-private `drip_quad`, inventory "read as rewritten: RESTRUCTURED") = 4.4, delivered
  as `u3x_emb_drip_quad`; it is described again so the whole compositor comes from one description;
- `u3x_emb_pass` (exported, named by the inventory only as a caller) = 4.5;
- the old file-private pointer test of the wall code (the same test as `ptr_ok`) and its virtual-call helper: no
  logic of their own; use `ptr_ok` and a call through the table as 3.1 says.

Stay as they are: `u3x_emb_rec_slot`, `u3x_emb_size` (trivial accessors, inventory "B, trivial"; G58 uses the same
manager fields), the C wrappers `emb_render119`, `emb_pass119`, `emb_wall119`, the counters `ew119` in the C part.

## 7. Behaviour that looks odd and must be kept

- The mask texture is asked from the cache four times per call, also for slot 0, which never uses it; the drips
  texture is asked for every drawn layer, also for slot 0. (The cache call requests the load, so this is what
  gets them loaded before slot 1 needs them.)
- Game 0x66bc30 is called for every layer even when the icon is 0 or the helper found no icon list; the game
  function looks the list up on its own.
- "Try again" (+2) can be set by planning and the call can still end as "unchanged" without drawing.
- A composite that could not load one of its files is still made into a texture (with what could be drawn) and
  only its key is cleared.
- The unchanged test needs the record's map to be non-zero; the key's byte 35 is always 1 and bytes 36..39 always 0.
- Slot 1 does not clear "try again"; `u3x_emb_pass` therefore counts `ec[114]` for slot 1 also when only slot 0
  had failed.
- Drip row = 3 − (cell >> 2) without a mask (cells above 15 give a negative row).
- Wall emblems: a misaligned player pointer from the team lookup is dropped without trying the local player;
  `ec[148]` is counted for every object without a usable player record, also when there is no player at all;
  `ec[151]` / `ec[152]` are rewritten for every matching object; `u3h_emb_is_record` is asked twice for a
  player's record; the default record is used only when it is one of the 24 records; the tables are written every
  frame (nothing is remembered between frames); at most 8 tables although the object may report more.
- The upside-down switch (`ec[0]` bit 15) is a test aid and stays.

## 8. Open questions

- The meaning of the second object type id 0xb95beddb (the first, 0x2db62a42, is the game's simple object) and of
  the two root offsets +0xd0 / +0xd8 is taken from the old code, not checked against the game here. They are ids
  and offsets, not expression; the new code uses them as given.
- None about the float maths: the old build used single-precision instructions throughout this group.

## Clarifications (tester, 2026-11-14)

Facts confirmed by the old-versus-new test (behaviour, in words):

- 4.1 step 1 / 4.2 "Key": the choice between the editor's layer bytes (M +8 + 8n) and the player's (M +0x2c + 8n) is
  made once per layer, from the sign of the s32 at M +0x60 as it is at the START of that layer's planning (before
  its icon-list request). The key's bytes 8n .. 8n + 7 are then read, after planning, from that same place chosen
  for layer n. The sign is not looked at again when the key is built. (It only matters when a callee changes the
  sign of M +0x60 during planning; then the key can mix editor bytes for the earlier layers with player bytes for
  the later ones.)
- 4.2 "Layers" step 7: the address of decal point k is `points` + 12·k computed with k widened to 64 bits (no 32-bit
  wrap of 12·k).
- 4.4: confirmed bit for bit against the shipped compiled code of v7.212 (the listing the v7.212 source carries) on
  every combination of special values and millions of random patterns: no further rule is needed.
- Consistency with G56_emblem_raster.md and G58_emblem_manager.md (sections 2.2, 3.3 to 3.7 checked): record
  layout, decoded-texture block, handle offsets, the corner pair meaning of `u3h_emb_quad`, the q order of
  `u3h_emb_decal`, the colour order of c16, the free / dds / texcreate contracts and `u3h_emb_frame_parts(b, s, 2)`
  agree. Differences in wording only: G58 calls M +0x60 "index of the net player, −1 = none" and has its own mode
  byte at M +0; this group's "editor mode" in 3.3 / 4.1 means only "M +0x60 is negative" (the mode byte is not
  read here). G58 3.10 lists G57's diagnostic words as ec[113], [114], [124], [125], [149], [150]; this group also
  writes ec[122] (= 4), ec[147], ec[148], ec[151], ec[152]. G56 3.6 adds that the handles of layer 3 and scale 3
  are byte copies of those of layer 2 / scale 2 (made by G56's bind); this group just passes their offsets.
- Open question 1 (type id 0xb95beddb, roots +0xd0 / +0xd8) cannot be settled without the game; old and new use the
  same values.
