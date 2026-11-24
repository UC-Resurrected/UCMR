# G56. Emblem maths, texture records and CPU rasterizer

Behaviour description for a reimplementation (reimpl2 pipeline, describer's document). It says what the functions
do, not how the old ones were written. Offsets are hexadecimal when prefixed with 0x. "Image offset X" means the
address `b + X`, where `b` is the game image base every function with a `b` parameter receives. All multi-byte values
are little-endian; "u8 / u16 / u32 / u64 / s32 / s64" are unsigned / signed integers of that width, "pointer" is 8
bytes, "f32" is a float held as its 32-bit pattern. "Q32" is a signed 64-bit fixed-point number with 32 fraction
bits (value · 2^32).

Neighbour specs: `G57_emblem_compositor.md` (plans the layers and calls this group's rasterizer and texture
functions) and `G58_emblem_manager.md` (the manager state machine; it takes and frees records). This group OWNS and
gives in full: the emblem record (3.2), the decoded-texture block (3.3), the level-of-detail triple (3.4), the retire
queue (3.5), the data handle (3.6), the temp-memory functions, the sine / cosine, the two number conversions and the
two rasterizer entry points. Both neighbours restate what they need; this document is the reference.

Old code: `u3x_fixes.cpp` v7.212 lines 20892 to 21401 (25 listed functions, 430 code lines) and the C function
`f_fma119` in `agc_port.inc` (3 lines): 433 lines.

## 0. Float rule for this group (read first)

**Nothing in this group ran on the x87 unit.** The old build computed every float step of these functions with
scalar single-precision SSE instructions through the same tools CONVENTIONS.h gives (`f_add`, `f_sub`, `f_mul`,
`f_div`, `f_from_int`, `f_to_int`, `f_less`, `f_madd`), one rounding per operation, on 32-bit patterns. There is no
value kept at extended precision anywhere, no x87 transcendental, and no float is passed by value. The new results
are therefore expected to be **bit-identical to the shipped helper, with no accepted deviation**; a tester compares
pixels and patterns bit for bit.

Notation used in every formula below (each symbol is ONE operation with ONE rounding to single precision):

| Written | Means |
|---|---|
| a + b, a − b, a · b, a / b | `f_add(a, b)`, `f_sub(a, b)`, `f_mul(a, b)`, `f_div(a, b)`, operands in the order written |
| fma(a, b, c) | `f_madd(a, b, c)`: a and b and c widened to double, product in double (exact), sum in double (one rounding to double), then one rounding to single. It is NOT `f_add(f_mul(a, b), c)` |
| int(n) | `f_from_int(n)`: signed 32-bit integer to single, round to nearest even |
| trunc(a) | `f_to_int(a)`: single to signed 32-bit integer, **truncation toward zero**; a value outside the s32 range, an infinity or a NaN gives 0x80000000 |
| a < b | `f_less(a, b)`: ordered compare, false when either is a NaN |

The only float-to-integer conversions of the group are: trunc() once in the sine / cosine (4.1); `u3x_fq32` (4.2:
integer bit work on the pattern, round half away from zero); `u3x_log2q8` (4.3: integer bit work); `u3x_h16q16`
(4.4: integer bit work, round to nearest even, then truncation). Everything per pixel is integer arithmetic (64-bit
unless said otherwise, two's complement, wrapping, ">>" on a signed value is an arithmetic shift = floor).

Clamps: they are listed where they happen (4.2 shift cap, 4.4 saturation, 4.15 level cap, 4.14 texel edge clamp,
4.18 coverage cap and byte cap).

## 1. Purpose and callers

PS3 1.19 draws a player's emblem with the GPU. The PS4 game has no such pass, so the helper composes the emblem on
the CPU and hands the game a texture. This group is the tool box under that:

- **maths**: the sine / cosine PS3 1.19 uses, float to Q32, a base-2 logarithm in 1/256 steps, float to 16-bit
  float;
- **engine temp memory and files**: allocate from the game's temp allocator, read one DDS texture file whole;
- **data handles**: bind and resolve the game's emblem data tables (icon lists, scale ranges, palette);
- **records**: 24 texture records plus one shared 4x4 transparent "fallback" texture; a record always shows a valid
  texture; a replaced texture is released three ticks later;
- **rasterizer**: sample a DXT1 / DXT5 texture's red channel with trilinear filtering and blend one rotated layer
  quad or one axis-aligned decal quad into a B, G, R, A byte target.

Callers in v7.212 (C wrappers are one line each and stay):

| Function | Called by |
|---|---|
| `u3x_sincos119` | C `sincos119` = export `u3h_sincos`, used by G57's compositor, the ring drawing of the hat-stat cards and the menu cursor glow (all C++), and by C code |
| `u3x_h16q16` | C `h16q16_119` = export `u3h_h16q16` (G57) |
| `u3x_fq32`, `u3x_log2q8` | only inside this group (4.15, 4.20, 4.21) |
| `u3x_emb_alloc` | C `emb_alloc119` = export `u3h_emb_alloc` (G57: the pixel buffer) |
| `u3x_emb_pop`, `u3x_emb_file` | only inside this group |
| `u3x_emb_h` | C `emb_h119` = export `u3h_emb_h` (G57), and 4.9 |
| `u3x_emb_bind1` | only `u3x_emb_dcbind` |
| `u3x_emb_dcbind` | C `emb_dcbind119` = export `u3h_emb_bind` (G58) |
| `u3x_emb_isrec` | C `emb_isrec119` = export `u3h_emb_is_record` (G57, G58), and 4.11 |
| `u3x_emb_release`, `u3x_emb_retire`, `u3x_emb_rfb` | only inside this group |
| `u3x_emb_retick` | C `emb_retick119` = export `u3h_emb_frame_parts(b, s, 1)` (G58, once per frame) |
| `u3x_emb_ralloc` | C `emb_ralloc119` = export `u3h_emb_ralloc` (G58's record requests and default record) |
| `u3x_emb_rfree` | C `emb_rfree119` = export `u3h_emb_free_record` (G58) |
| `u3x_emb_texcreate` | C `emb_texcreate119` = export `u3h_emb_texcreate` (G57 after a composite, G58's default record), and 4.12 |
| `u3x_emb_fb` | C `emb_fb119` = export `u3h_emb_frame_parts(b, s, 0)` (G58, every frame, first) |
| `u3x_emb_dds` | C `emb_dds119` = export `u3h_emb_dds` (G57) |
| `u3x_emb_dxt`, `u3x_emb_lod`, `u3x_emb_bil`, `u3x_emb_a32`, `u3x_emb_blend`, `u3x_emb_rho2` | only inside this group |
| `u3x_emb_quad`, `u3x_emb_decal` | C `emb_quad119` / `emb_decal119` = exports `u3h_emb_quad` / `u3h_emb_decal` (G57) |

Threading: everything runs on the game's main thread (the helper's per-frame tick). No locking.

## 2. Interface

### 2.1 Functions to deliver (all `extern "C"`, `U3X_FN`; names and signatures unchanged unless marked NEW)

No float or double is passed or returned by value anywhere: floats travel as u32 patterns.

Part A (pure maths and rasterizer; no game call):

| Signature | Section |
|---|---|
| `uint32_t u3x_sincos119(uint32_t x, uint32_t cosine)` | 4.1 |
| `void u3x_sincos_bind(uintptr_t b)` NEW | 4.1 (takes the six polynomial patterns from the game image once) |
| `int64_t u3x_fq32(uint32_t f)` | 4.2 |
| `int32_t u3x_log2q8(uint32_t f)` | 4.3 |
| `uint32_t u3x_h16q16(uint32_t f)` | 4.4 |
| `uint32_t u3x_emb_dxt(const void *dds, uint32_t level, int32_t x, int32_t y)` | 4.14 |
| `void u3x_emb_lod(const void *dds, uint32_t rho2, void *lod)` | 4.15 |
| `uint32_t u3x_emb_bil(const void *dds, uint32_t level, int64_t U, int64_t V)` | 4.16 |
| `uint64_t u3x_emb_a32(const void *dds, const void *lod, int64_t U, int64_t V)` | 4.17 |
| `void u3x_emb_blend(uint8_t *px, const uint32_t *c16, uint64_t a32)` | 4.18 |
| `uint32_t u3x_emb_rho2(uint32_t du, uint32_t dv, uint32_t tw, uint32_t th, uint32_t size)` | 4.19 |
| `uint32_t u3x_emb_fmax(uint32_t a, uint32_t b)` | 4.19 |
| `void u3x_emb_quad(uint8_t *pix, uint32_t size, const void *dds, const uint32_t *uv, const void *mask, const uint32_t *c16)` | 4.20 |
| `void u3x_emb_decal(uint8_t *pix, uint32_t size, const void *dds, const uint32_t *q, const uint32_t *c16)` | 4.21 |

Part B (engine memory, files, data handles, records; game calls):

| Signature | Section |
|---|---|
| `void u3x_emb_pop(uintptr_t b)` | 4.5 |
| `void *u3x_emb_alloc(uintptr_t b, uint64_t n)` | 4.5 |
| `uint8_t *u3x_emb_file(uintptr_t b, const char *name, uint64_t *size)` | 4.6 |
| `uint8_t *u3x_emb_h(uintptr_t b, void *s, uintptr_t handle)` | 4.7 |
| `void u3x_emb_bind1(uintptr_t b, void *s, uintptr_t handle, uint32_t sid)` | 4.8 |
| `void u3x_emb_dcbind(uintptr_t b, void *s)` | 4.9 |
| `int u3x_emb_isrec(uint64_t p)` (stray) | 4.10 |
| `void u3x_emb_release(uintptr_t b, uint64_t *tex, uint64_t *map)` | 4.10 |
| `void u3x_emb_retire(uintptr_t b, uint64_t *tex, uint64_t *map)` | 4.10 |
| `void u3x_emb_retick(uintptr_t b)` (stray) | 4.10 |
| `void u3x_emb_rfb(uintptr_t b, void *rec)` | 4.11 |
| `void *u3x_emb_ralloc(uint32_t size)` | 4.11 |
| `void u3x_emb_rfree(uintptr_t b, void *rec)` (stray) | 4.11 |
| `int u3x_emb_texcreate(uintptr_t b, void *rec, const uint8_t *pix, uint32_t size)` | 4.12 |
| `void u3x_emb_fb(uintptr_t b)` | 4.12 |
| `int u3x_emb_dds(uintptr_t b, const char *name, void *dds)` | 4.13 |

`s` is the helper's state object; this group only reads one pointer from it (3.7).

The C function `f_fma119(a, b, c)` (the group's only function still in C): it is `f_madd` of CONVENTIONS.h word for
word in effect, and **no code calls it any more** in v7.212 (its comment "still used by C code elsewhere" is stale).
Proposed: delete it. If the integrator wants to keep the C name, deliver
`extern "C" uint32_t u3x_fma32(uint32_t a, uint32_t b, uint32_t c)` = fma(a, b, c) of section 0 and make the C body
`return u3x_fma32(a,b,c);`.

### 2.2 Functions that stay as they are and are called by this group (declare exactly so)

Same file, trivial (inventory "B, trivial"), not delivered again:

- `extern "C" void u3x_emb_push(uintptr_t b)`: selects the game's temp allocator for the calls that follow: game
  0xda1590(pointer to two u32 {0x30, 0}, `b + 0x13ac250`, u32 0, `b + 0x13ac250`). Every push is undone by
  `u3x_emb_pop` (4.5).
- `extern "C" void u3x_emb_free(uintptr_t b, void *p)`: nothing when `p` is null; else game 0xda0dc0(p,
  `b + 0x147d6bb`, u32 0, `b + 0x147d6bb`). Frees memory from `u3x_emb_alloc` or a file from `u3x_emb_file`.
- `extern "C" uint8_t *u3x_emb_cache(uintptr_t b, const char *name)`: game 0x1189430(`b + 0x1b31128`, name), the
  texture cache's block for a texture name (G57 uses it; not used inside this group).

C part exports (`U3H_IMPORT`):

- `void *u3h_emb_fallback(void)`: the address of the fallback record F (one record of 3.2, zero at start).
- `void *u3h_emb_retire_queue(void)`: the address of the retire queue (3.5, zero at start).
- `const uint8_t *u3h_emb_zero(void)`: 64 zero bytes (the pixels of the 4x4 transparent texture).
- `uint8_t *u3h_emb_records(uint8_t **flag)`: returns the address of the record table (24 records of 3.2, zero at
  start) and stores through `flag` the address of the "compose this frame" byte (G58's; this group ignores it but
  must pass a valid pointer).
- `uint8_t *u3h_ext(void)` (CONVENTIONS.h). The diagnostic words `ec[]` are u32 at `u3h_ext() + 0x4450`
  (`ec[i]` at +0x4450 + 4·i); CONVENTIONS.h has no accessor for them, so write one locally.

### 2.3 What the C wrappers pass

Unchanged, one line each: `sincos119(x, flag)` → `u3x_sincos119(x, flag)`; `h16q16_119(f)`; `emb_alloc119(b, n)`;
`emb_free119`; `emb_cache119`; `emb_h119(b, s, ha)`; `emb_dcbind119(b, s)`; `emb_isrec119(p)`; `emb_retick119(b)`;
`emb_ralloc119(size)`; `emb_rfree119(b, r)`; `emb_texcreate119(b, r, pix, size)`; `emb_fb119(b)`;
`emb_dds119(b, name, t)`; `emb_quad119(pix, size, t, uv, mk, c16)`; `emb_decal119(pix, size, t, q, c16)`: each
forwards its arguments unchanged to the `u3x_` function of the same name.

New for the integrator: one call `u3x_sincos_bind(base)` where the C part first learns the image base (the start-up
event that links the native functions, kind 44), before any frame runs. See 4.1 and open question 1.

## 3. Game facts

### 3.1 Game functions (System V x86-64 calling convention, integer arguments only)

| Image offset | Arguments | Result | Meaning |
|---|---|---|---|
| 0xda0d60 | none | pointer | the calling thread's allocator stack (3.8), may be null |
| 0xda1590 | see `u3x_emb_push` | none | push an allocator on that stack |
| 0xda0dc0 | see `u3x_emb_free` | none | free a block |
| 0xf42fa0 | (pointer to two u64 {data pointer, byte count}, const char *path) | bool in the low byte of the result | read a whole file into memory taken from the current allocator; fills the two u64 |
| 0xe73490 | (pointer to a u64 holding a texture) | none | release a texture (the game may change the u64) |
| 0xe4e8f0 | (`b + 0x18c8960`, u64 map, u32 frame number) | none | release a texture map, as the texture cache does when it evicts |
| 0xe76cb0 | (pointer to s32 memory context, pointer to 2 u64 description, pointer to 2 u64 pixel source, pointer to u64) | u64 map, 0 on failure | create a 2D texture map with initial pixels |
| 0xe77c50 | (u64 map, u32 format) | u64 texture, 0 on failure | create the texture of a map |
| native "find module" | (u32 id, int 1) | pointer | the loaded data module that holds the symbol `id`; "none" is the address `b + 0x1b3af40`. Its address is read from the state (3.7); it is the game's 0x11fd4c0 |

Through an object: the allocator on top of the allocator stack is an object whose first 8 bytes point to a table of
function pointers; the pointer at table +0x48 is "allocate": (object, u64 bytes, u64 alignment, const char *file,
u32 line, const char *function) → pointer or null.

### 3.2 Emblem record (80 bytes; 24 of them in the C part's table, one more as the fallback F)

| Offset | Type | Name | Meaning |
|---|---|---|---|
| +0x00 | u64 | tex | the texture the game's readers use; never 0 once F exists |
| +0x08 | u64 | map | the texture map; non-zero = the record owns `tex` (made by a composite); 0 = `tex` is F's |
| +0x10 | u8 | loaded | 1 when `tex` is usable |
| +0x11 | 3 bytes | | padding, 0 |
| +0x14 | u32 | width | pixels |
| +0x18 | u32 | height | pixels |
| +0x1c | u32 | format | always 0x57 (four bytes per pixel in the order B, G, R, A) |
| +0x20 | u32 | size | edge length asked for when the record was taken |
| +0x24 | u16 | pin | G58's; this group writes 0 when a record is freed and clears it with the whole record when taken |
| +0x26 | u16 | used | 1 = taken, 0 = free |
| +0x28 | 40 bytes | key | G57's "inputs of the last composite"; this group only clears it with the whole record when taken |

The first 0x20 bytes are the layout every reader of a UI texture value in the game expects (texture, map, loaded,
width, height, format), which is why a record's address can be given to the game as a texture.

### 3.3 Decoded-texture block (88 bytes; the caller provides the storage, `u3x_emb_dds` fills it)

| Offset | Type | Name | Meaning |
|---|---|---|---|
| +0x00 | pointer | file | the whole DDS file in temp memory, or null |
| +0x08 | u32 | w | width of level 0 |
| +0x0c | u32 | h | height of level 0 |
| +0x10 | u32 | last | number of the last usable level (levels − 1) |
| +0x14 | u32 | bs | bytes per 4x4 block: 8 (DXT1) or 16 (DXT5) |
| +0x18 | 16 × u32 | off | byte offset of each level's first block inside the file |

Level l has width max(1, w >> l) and height max(1, h >> l), in blocks of 4x4 texels stored row by row,
((width + 3) / 4) blocks per row.

### 3.4 Level-of-detail triple (12 bytes): u32 l0 +0, u32 l1 +4, u32 f +8

Sample level l0 and level l1 and mix them with weight f / 256 for l1 (f = 0..255).

### 3.5 Retire queue (16 entries of 0x18 bytes): u64 tex +0, u64 map +8, u32 age +0x10, u32 unused +0x14

An entry is free when tex and map are both 0.

### 3.6 Data handle (0x18 bytes, in the game image): pointer value +0, u32 version +8, u32 id +0xc, pointer module +0x10

Nine handles: icon lists of layers 0..3 at image offset 0x1b67e60 + 0x18·n; palette at 0x1b67ec0; scale ranges of
layers 0..3 at 0x1b67ee0 + 0x18·n. A data module: pointer to its symbol table at +0x48, u32 version at +0x58. A
symbol table: s32 count at +0x30, pointer at +0x38 to entries of 0x10 bytes (u32 id +0, pointer value +8), sorted by
id ascending (unsigned).

The seven data ids (layers 0, 1, 2; scales 0, 1, 2; palette) are NOT written in this document or in the new code.
The running PS4 game holds each as the 4-byte immediate of a "load id" instruction in its own bind function (image
0x66b510, which binds the same handles), and this group reads them from there at run time:

| Handle | Id is the u32 at image offset |
|---|---|
| layer 0 (0x1b67e60) | 0x66b756 |
| layer 1 (0x1b67e78) | 0x66b808 |
| layer 2 (0x1b67e90) | 0x66b8aa |
| scale 0 (0x1b67ee0) | 0x66b976 |
| scale 1 (0x1b67ef8) | 0x66ba1a |
| scale 2 (0x1b67f10) | 0x66bac8 |
| palette (0x1b67ec0) | 0x66bb89 |

Check for each: the byte before the immediate is 0xbf and the five bytes after it are be 01 00 00 00 (verified in
`eboot.elf`, file offset = image offset + 0x4000; see open question 2 for a mismatch).

Layer 3 and scale 3 have no id of their own: PS4's function and the old code both make them byte copies of layer 2
and scale 2 (4.9).

### 3.7 State and globals

| Where | Type | Meaning |
|---|---|---|
| `s` + 0x348 | pointer | the native "find module" function (entry 9 of the state's table of native functions at +0x300, 8 bytes each); may be 0 |
| image 0x1b24b80 | pointer | the game's path object: zero-terminated text at +0x408 (data root folder) and at +0x479c (platform folder name) |
| image 0x1c1f308 | pointer to u32 | the main thread's frame counter (read the pointer, then the u32 it points to) |
| image 0x1cf5330 | u64 | non-zero once the graphics device exists |
| image 0x18c8960 | object | passed to 0xe4e8f0 |
| image 0x13ac250, 0x147d6bb | text | file / function texts the allocator calls take (passed, never read) |
| image 0x13f0760 | 6 rows of 16 bytes | the sine / cosine polynomial patterns (4.1) |

### 3.8 Allocator stack

u32 count at +0x730; entries of 0x28 bytes from +0, entry i at +0x28·i; the first 8 bytes of an entry are the
allocator object. The top entry is number count − 1.

### 3.9 Diagnostic words written by this group (`ec[]`, u32)

| Word | Written |
|---|---|
| ec[115] | + 1 per texture created (4.12) |
| ec[116] | + 1 per record taken (4.11) |
| ec[117] | + 1 per record freed (4.11) |
| ec[122] | last failure: 1 no free record, 2 map creation failed, 3 texture creation failed (4 is G57's) |
| ec[123] | + 1 per texture file that could not be used (4.13) |
| ec[158], ec[159] | low and high half of the fallback texture (4.12) |

### 3.10 DDS file facts used (public format)

u32 at 0 = 0x20534444 ("DDS "); u32 height at 12; u32 width at 16; u32 mip count at 28; u32 four-character code at
84: 0x31545844 ("DXT1") or 0x35545844 ("DXT5"); texel data from 128, level after level, largest first. A DXT colour
block is 8 bytes: u16 colour 0, u16 colour 1 (5 bits red in bits 11..15), u32 of sixteen 2-bit selectors, texel
(x, y) of the block at bits 2·(4·y + x). In DXT5 the colour block is the second 8 bytes of the 16-byte block.

## 4. Behaviour

### 4.1 `u3x_sincos119(x, cosine)` and `u3x_sincos_bind(b)`

Result: the pattern of sin x (`cosine` = 0) or cos x (`cosine` = 1) the way PS3 1.19 computes it. Callers pass only 0
or 1; the value is added as given (see q).

Constants. Mathematical ones, generated by the rule beside them:

| Name | Pattern | Rule |
|---|---|---|
| K | 0x3f22f983 | 2 / pi rounded to nearest single (0.63661975) |
| H | 0xbfc90fda | −(pi / 2 truncated to 24 bits) = −1.57079625; the single just below pi / 2 in size (NOT the nearest, which ends in ...db) |
| W | 0xb3a22169 | −(pi / 2 − 1.57079625129...) rounded to nearest single = −7.5497899e-8, the rest H leaves |
| half | 0x3f000000 | 0.5 |
| T | 0x39800000 | 2^−12; −T = 0xb9800000 |
| one | 0x3f800000 | 1.0 |

Six polynomial coefficients S0, S1, S2 (sine) and C0, C1, C2 (cosine). They are not written here. The running PS4
game holds the same six patterns in its own vector sine / cosine constant block at image offset 0x13f0760: six rows
of 16 bytes, each row four equal f32; take the first f32 of each row:

| Row at image offset | Coefficient | Used as |
|---|---|---|
| 0x13f0760 | C2 | cosine, highest power |
| 0x13f0770 | C1 | cosine, middle |
| 0x13f0780 | S2 | sine, highest power |
| 0x13f0790 | S1 | sine, middle |
| 0x13f07a0 | C0 | cosine, lowest (close to −1/2) |
| 0x13f07b0 | S0 | sine, lowest (close to −1/6) |

`u3x_sincos_bind(b)` copies these six u32 into six private variables (U3X_BSS). `u3x_sincos119` has no base
parameter and uses the copies. (Open question 1: the values look like the published constants of a public library;
if that is confirmed they may be written as named published constants and the bind function dropped.)

Steps, in this order:

1. a = x with bit 31 cleared. a above 0x7f800000 (NaN): return 0x7fc00000. a = 0x7f800000 (infinity): return x
   unchanged (for sine and for cosine). a = 0: return `one` for cosine, x unchanged (signed zero kept) for sine.
2. t = x · K. When 0 < t: t = t + half; otherwise t = t − half. n = trunc(t). (Round to the nearest quadrant, half
   away from zero. For huge x, n is 0x80000000.)
3. q = (n & 3) + cosine (unsigned). nf = int(n).
4. r = fma(nf, H, x); then r = fma(nf, W, r).
5. When −T < r and r < T (both strict): r = `one` if q is odd, otherwise r stays.
   Otherwise r2 = r · r, and
   - q even (sine): r3 = r · r2; p = fma(r2, S2, S1); p = fma(r2, p, S0); r = fma(r3, p, r);
   - q odd (cosine): p = fma(r2, C2, C1); p = fma(r2, p, C0); r = fma(r2, p, one).
6. Return r, with bit 31 inverted when bit 1 of q is set.

Rounding points: exactly the ones the operations above show (12 at most). No intermediate is kept wider than
single except inside one fma.

### 4.2 `u3x_fq32(f)`: f32 to Q32

Result = f · 2^32 as s64, rounded to the nearest integer in size (a tie goes away from zero), with these exact rules
on the pattern (no float instruction):

- exponent field e (bits 23..30) = 0 (zero or denormal): result 0.
- M = mantissa bits with the implied leading 1 (a 24-bit integer); k = e − 118, so |f| · 2^32 = M · 2^k.
- k ≥ 0: k is capped at 38 (so anything of size 2^30 or more, infinities and NaNs included, gives M · 2^38, not a
  saturated value); magnitude = M shifted left by k.
- k < −40: magnitude 0.
- otherwise: magnitude = (M + 2^(−k−1)) shifted right by −k (round half up of the magnitude).
- sign bit set: result = −magnitude.

### 4.3 `u3x_log2q8(f)`: 256 · log2 f, rounded

For a positive normal f the result is round(256 · log2 f) (s32), computed with integers only, and the exact steps
decide the last unit, so they are part of the behaviour:

- E = exponent field − 127. m = the 24-bit mantissa with the implied 1 (a number in [1, 2) with 23 fraction bits).
  The sign bit is ignored and a zero exponent field is treated the same way (callers never pass f ≤ 0).
- Nine fraction bits of log2 m, most significant first: nine times, m = (m · m) with its low 23 bits dropped
  (truncated); when m is now 2^24 or more, the bit is 1 and m is halved (low bit dropped), else the bit is 0.
- R = the nine bits as a number 0..511. Result = 256 · E + ((R + 1) >> 1).

### 4.4 `u3x_h16q16(f)`: f32 through a 16-bit float, as value · 65536

This is what a 16-bit float register does to a colour. Result (u32):

- sign bit set (negative numbers, −0, negative NaN) or f = +0: 0.
- exponent E = exponent field − 127. E < −25: 0. E > 15 (also +infinity and positive NaN): 0x7fffffff.
- otherwise h = f rounded to the nearest IEEE binary16 value, ties to the even mantissa; binary16 has 10 mantissa
  bits for E ≥ −14 and its denormals (multiples of 2^−24) below. Done on the 24-bit mantissa M (implied 1): drop
  13 low bits, plus (−14 − E) more when E < −14; round the kept part by the dropped bits (above half: up; exactly
  half: up only when the kept part is odd). A round-up that carries into the next power of two is kept as it is.
- result = floor(h · 65536), and 0x7fffffff when that exceeds 0x7fffffff (every h of 32768 or more).

### 4.5 Temp memory: `u3x_emb_pop(b)`, `u3x_emb_alloc(b, n)`

`u3x_emb_pop`: st = game 0xda0d60(); null: return. c = u32 at st + 0x730; 0: return. Fill the 0x28 bytes of entry
c − 1 with zero, then store c − 1 at st + 0x730.

`u3x_emb_alloc`: `u3x_emb_push(b)`; result = null; st = game 0xda0d60(); when st is not null and its count c is not
0 and the allocator object A of entry c − 1 is not null: result = A's "allocate" (3.1)(A, n, 0x10,
`b + 0x147d6bb`, 0, `b + 0x147d6bb`). Then `u3x_emb_pop(b)` in every case. Return the result.

### 4.6 `u3x_emb_file(b, name, size)`

1. g = the pointer at image 0x1b24b80. g null or `name` null: return null (nothing else happens, `*size` untouched).
2. Path, in a local buffer of 0x200 bytes: the text at g + 0x408, then "/build/", then the text at g + 0x479c, then
   "/vtex1/ps4/", then `name`, copied character by character and only while fewer than 0x1f8 characters have been
   written (once 0x1f8 is reached everything that follows of these five pieces is dropped); then always ".dds" and
   a terminating zero.
3. Two u64 {0, 0}; `u3x_emb_push(b)`; ok = low byte of game 0xf42fa0(the two u64, path); `u3x_emb_pop(b)`.
4. ok = 0 or the first u64 = 0: return null. Else `*size` = second u64; return the first as a pointer.

The three texts are constants of the helper (U3X_RO character arrays).

### 4.7 `u3x_emb_h(b, s, handle)`: resolve a data handle

H = the handle at `b + handle` (3.6). Result: the handle's value pointer, or null.

1. id = H.id; 0: return null.
2. m = H.module. find = the pointer at `s` + 0x348.
   - m is null: find null → return null. m = find(id, 1); when m is the address `b + 0x1b3af40`, m = null. Store
     H.module = m. m null → return null. Go to step 3 (no version test).
   - m is not null: when H.version equals the u32 at m + 0x58, return H.value (also when that is null).
3. t = pointer at m + 0x48; null → return null.
4. Look the id up in t's entries by bisection (low = 0, high = count − 1, middle = floor((low + high) / 2), ids
   compared unsigned; with count ≤ 0 nothing is read). Found: H.value = the entry's value, H.version = u32 at
   m + 0x58, return H.value. Not found: return null (H.value and H.version stay).

### 4.8 `u3x_emb_bind1(b, s, handle, sid)`

find = pointer at `s` + 0x348; m = find(sid, 1) when find is not 0, else null; the "none" address counts as null.
Store H.module = m, H.value = 0, H.version = 0xffffffff, H.id = sid. Then, when m is not null and the pointer at
m + 0x48 is not null: `u3x_emb_h(b, s, handle)` (result ignored; it fills value and version when the id is found).

### 4.9 `u3x_emb_dcbind(b, s)`

In this order (ids from 3.6): bind1 layer 0; bind1 layer 1; bind1 layer 2; copy the 0x18 bytes of the layer 2
handle over the layer 3 handle; bind1 scale 0; bind1 scale 1; bind1 scale 2; copy the scale 2 handle over the
scale 3 handle; bind1 palette. (Up to seven "find module" calls, in that order.)

### 4.10 Record test, release, retire

`u3x_emb_isrec(p)`: T = `u3h_emb_records(&flag)`. 1 when T ≤ p < T + 24·80 and (p − T) is a multiple of 80; else 0.

`u3x_emb_release(b, tex, map)` (both pointers valid):
1. frame = the u32 the pointer at image 0x1c1f308 points to (read first, always).
2. When `*tex` is not 0 and differs from F.tex: game 0xe73490(tex) (the pointer itself is the argument).
3. When `*map` is not 0: game 0xe4e8f0(`b + 0x18c8960`, `*map`, frame).
4. `*tex` = 0; `*map` = 0.

`u3x_emb_retire(b, tex, map)`: a texture that may still be in use by this frame's draw calls is parked.
1. `*tex` equals F.tex and `*map` is 0: `*tex` = 0; return. (The fallback is shared and never released.)
2. Both 0: return.
3. The first free queue entry (3.5), lowest number first: entry.tex = `*tex`, entry.map = `*map`, entry.age = 0;
   `*tex` = 0, `*map` = 0; return.
4. No free entry: `u3x_emb_release(b, tex, map)` at once.

`u3x_emb_retick(b)`: for the 16 entries in order: when the entry is not free, age + 1 (stored); when the age is now
3 or more, `u3x_emb_release(b, &entry.tex, &entry.map)` (the age is left as it is).

### 4.11 Records: `u3x_emb_rfb`, `u3x_emb_ralloc`, `u3x_emb_rfree`

`u3x_emb_rfb(b, rec)`: remember the record's tex and map; then rec.tex = F.tex, rec.map = 0, rec.loaded = 1 when
F.tex is not 0 else 0, rec.format = 0x57 (width, height, size untouched); only then, when the remembered map is not
0, `u3x_emb_retire(b, the remembered pair)` (on local copies). The new value is in place before the old one is
retired.

`u3x_emb_ralloc(size)`: T = `u3h_emb_records(&flag)`. The first record with used = 0, lowest number first: all 80
bytes zero; used = 1; size = width = height = `size`; tex = F.tex; loaded = 1 when F.tex is not 0 else 0; format =
0x57; ec[116] + 1; return its address. None: ec[122] = 1; return null. No game call.

`u3x_emb_rfree(b, rec)`: nothing when `rec` is null, `u3x_emb_isrec(rec)` is 0, or rec.used is 0. Else
`u3x_emb_rfb(b, rec)`; used = 0; pin = 0; ec[117] + 1.

### 4.12 `u3x_emb_texcreate(b, rec, pix, size)` and `u3x_emb_fb(b)`

`u3x_emb_texcreate`: makes a `size` × `size` texture from `pix` (4 bytes per pixel, rows top to bottom, B, G, R, A)
and puts it into the record. Returns 1, or 0 on failure.

1. Arguments of game 0xe76cb0, all on the stack of this function:
   - s32 memory context = 0xe;
   - description, two u64: first = `size` in bits 0..15, `size` in bits 16..31, 1 in bits 32..39, 1 in bits
     40..47, 0x57 in bits 48..63 (built as size | size << 16 | 1 << 32 | 1 << 40 | 0x57 << 48 with `size` as a
     64-bit value, no masking); second = 0x0000000800010001;
   - pixel source, two u64: first = the address `pix`; second = (size · 4) in the low half and (size · size · 4)
     in the high half, each computed in 32 bits;
   - a u64 0.
   map = game 0xe76cb0(&context, description, source, &that u64).
2. map = 0: ec[122] = 2; return 0.
3. tex = game 0xe77c50(map, 0x57). tex = 0: `u3x_emb_release(b, &zero, &map)` with a local u64 0 as the texture
   (so only the map is released); ec[122] = 3; return 0.
4. Remember the record's tex and map; rec.tex = tex; rec.map = map; width = height = `size`; format = 0x57;
   loaded = 1; ec[115] + 1; then, when the remembered map is not 0, `u3x_emb_retire(b, the remembered pair)`.
   Return 1. (rec.size, pin, used and key are not touched; a remembered texture without a map is not released: it
   was the fallback.)

`u3x_emb_fb`: makes the fallback once. Return when F.tex is not 0 or the u64 at image 0x1cf5330 is 0. Take a local
record, all zero; `u3x_emb_texcreate(b, &local, u3h_emb_zero(), 4)`; on failure return (it is tried again next
frame). On success copy the 80 bytes to F, then F.map = 0 (so nothing ever releases it), ec[158] = low 32 bits of
the texture, ec[159] = high 32 bits.

### 4.13 `u3x_emb_dds(b, name, dds)`

Fills the block D (3.3). Returns 1 when usable, else 0.

1. D.file = `u3x_emb_file(b, name, &n)` with n = 0 before. Null: ec[123] + 1; return 0 (no other field written).
2. Unusable when n < 128, or the u32 at 0 is not "DDS ", or the code at 84 is neither "DXT1" nor "DXT5":
   `u3x_emb_free(b, D.file)`; D.file = null; ec[123] + 1; return 0 (no other field written).
3. D.h = u32 at 12; D.w = u32 at 16; mips = u32 at 28; levels = mips, 1 when mips is 0, at most 16.
   D.bs = 8 for DXT1, 16 for DXT5.
4. Offsets: position = 128 (64-bit). For l = 0 .. levels − 1: D.off[l] = position (low 32 bits); position +=
   ((lw + 3) / 4) · ((lh + 3) / 4) · D.bs with lw = max(1, D.w >> l), lh = max(1, D.h >> l) (the two block counts
   in 32-bit unsigned arithmetic, their product and the sum in 64 bits). Entries from `levels` on are not written.
5. position > n, or D.w = 0, or D.h = 0: free the file, D.file = null, ec[123] + 1, return 0 (the fields of steps
   3 and 4 stay written).
6. D.last = levels − 1. Return 1.

The caller frees D.file with `u3x_emb_free` when it has finished.

### 4.14 `u3x_emb_dxt(dds, level, x, y)`: one texel's red, 0..255

lw = max(1, w >> level), lh = max(1, h >> level) (as s32). x is clamped to 0 .. lw − 1 and y to 0 .. lh − 1 (clamp
to edge). Block number = (y >> 2) · ((lw + 3) >> 2) + (x >> 2) (32-bit unsigned); the colour block is at
file + off[level] + block number · bs, plus 8 when bs is 16. With c0, c1 the two u16 colours and r0, r1 their red
fields (bits 11..15) widened from 5 to 8 bits as (r << 3) | (r >> 2), and sel the texel's 2-bit selector for
(x & 3, y & 3):

| sel | DXT5, or DXT1 with c0 > c1 | DXT1 with c0 ≤ c1 |
|---|---|---|
| 0 | r0 | r0 |
| 1 | r1 | r1 |
| 2 | (2·r0 + r1) / 3 | (r0 + r1) / 2 |
| 3 | (r0 + 2·r1) / 3 | 0 |

Integer divisions truncate. `level` is not checked (callers pass 0 .. last).

### 4.15 `u3x_emb_lod(dds, rho2, lod)`: levels for a footprint

`rho2` = (texels per target pixel)², an f32 pattern. Output triple (3.4), first set to (0, 0, 0). It stays so when
rho2 is ±0 or has its sign bit set. Otherwise lam = ((`u3x_log2q8(rho2)` + 1) >> 1) − 256 (s32, arithmetic shift):
log2 of texels per pixel in 1/256 steps with the PS3 sampler's level bias of −1. lam ≤ 0: stays (0, 0, 0). Else
d = lam, capped at last · 256; l0 = d >> 8; f = d & 255; l1 = l0 + 1; when l1 > last: l1 = last and f = 0.

### 4.16 `u3x_emb_bil(dds, level, U, V)`: bilinear red · 65536

U, V are Q32 texture coordinates (1.0 = the whole texture). lw, lh as in 4.14 (64-bit). tu = U · lw − 2^31,
tv = V · lh − 2^31 (texel space, centres at half). x = tu >> 32 and y = tv >> 32 (floor, then cut to s32);
fx = (tu >> 24) & 255, fy = (tv >> 24) & 255 (8 fraction bits, the rest dropped). With a, c, d, e =
`u3x_emb_dxt` at (x, y), (x + 1, y), (x, y + 1), (x + 1, y + 1), in that order:
result (u32) = (a · (256 − fx) + c · fx) · (256 − fy) + (d · (256 − fx) + e · fx) · fy.

### 4.17 `u3x_emb_a32(dds, lod, U, V)`: filtered red as coverage, 0 .. 2^32

r0 = `u3x_emb_bil`(l0) (64-bit). When f is 0: r = r0 · 256 (level l1 is not sampled). Else r = r0 · (256 − f) +
`u3x_emb_bil`(l1) · f. Result = (r << 8) / 255, unsigned 64-bit division, truncated. (Red 255 everywhere gives
exactly 2^32.)

### 4.18 `u3x_emb_blend(px, c16, a32)`: one pixel

`px` points at 4 bytes B, G, R, A; `c16` at three u32 colour values · 65536 in the order blue, green, red
(`u3x_h16q16` results); `a32` is the coverage. a = min(a32, 2^32); ia = 2^32 − a. For k = 0, 1, 2:
v = (255 · c16[k] · a + px[k] · 65536 · ia + 2^47) >> 48; px[k] = min(v, 255). Then
v = (255 · 65536 · a + px[3] · 65536 · ia + 2^47) >> 48; px[3] = min(v, 255).
All in unsigned 64-bit arithmetic that wraps (a colour value above about 257.0 overflows; keep it). This is "source
over" with the colour weighted by the coverage and the alpha accumulating, rounded to the nearest byte.

### 4.19 `u3x_emb_rho2(du, dv, tw, th, size)` and `u3x_emb_fmax(a, b)`

rho2: fs = int(size); A = (du · int(tw)) / fs; C = (dv · int(th)) / fs; result = (A · A) + (C · C). (tw, th, size
are converted as signed 32-bit values. Grouping as written: per axis the product first, then the division; then
the two squares; then the sum.)

fmax: b when a < b, else a (so a NaN in either place gives a).

### 4.20 `u3x_emb_quad(pix, size, dds, uv, mask, c16)`: one layer over the whole target

`pix`: `size` × `size` pixels of 4 bytes, row after row. `uv`: eight f32, four corner pairs (u, v) as G57's
`u3x_emb_layer_uv` writes them; pair 1 (uv[2], uv[3]) is the texture coordinate of the target's origin corner,
pair 2 (uv[4], uv[5]) of the end of the horizontal edge, pair 0 (uv[0], uv[1]) of the end of the vertical edge;
pair 3 is not read. `mask`: a second decoded texture stretched over the whole target, or null. `c16`: see 4.18.

1. Q32 set-up: U1 = fq32(uv[2]), V1 = fq32(uv[3]); DUX = fq32(uv[4]) − U1; DVX = fq32(uv[5]) − V1;
   DUY = fq32(uv[0]) − U1; DVY = fq32(uv[1]) − V1 (integer differences of converted values, not converted float
   differences).
2. Levels of the layer: rx = rho2(uv[4] − uv[2], uv[5] − uv[3], D.w, D.h, size);
   ry = rho2(uv[0] − uv[2], uv[1] − uv[3], D.w, D.h, size) (float differences here); `u3x_emb_lod(dds,
   fmax(rx, ry), &L)`.
3. Levels of the mask, only when there is one: k = int(mask.w) / int(size); `u3x_emb_lod(mask, k · k, &Lm)`.
4. sh = 9 when `size` is 256, else 8 (the target sizes in use are 256 and 128, where 2^sh = 2 · size; any other
   size also takes 8, keep).
5. For py = 1 .. size − 2 (outer), px = 1 .. size − 2 (inner) (the outermost ring of pixels is never touched: the
   PS3 scissor; nothing at all when size < 3): ox = 2·px + 1, oy = 2·py + 1;
   u = U1 + ((DUX · ox + DUY · oy) >> sh); v = V1 + ((DVX · ox + DVY · oy) >> sh) (sum first, one arithmetic
   shift); a = `u3x_emb_a32(dds, &L, u, v)`. With a mask: m = `u3x_emb_a32(mask, &Lm, ox << (32 − sh),
   oy << (32 − sh))`; a = (a · m) >> 32 with the full 128-bit product. When a is not 0:
   `u3x_emb_blend(pix + (py · size + px) · 4, c16, a)`.

### 4.21 `u3x_emb_decal(pix, size, dds, q, c16)`: one axis-aligned quad

`q`: eight f32: x low, x high, y low, y high (as fractions of the target), u low, u high, v at y low, v at y high.

1. fs = int(size); x0 = q[0] · fs; x1 = q[1] · fs; y0 = q[2] · fs; y1 = q[3] · fs; dx = x1 − x0; dy = y1 − y0.
   Return unless 0 < dx and 0 < dy (a NaN returns).
2. du = q[5] − q[4]; dv = q[7] − q[6]. ru = (du · int(D.w)) / dx; rv = (dv · int(D.h)) / dy;
   `u3x_emb_lod(dds, fmax(ru · ru, rv · rv), &L)`.
3. Q32: X0, X1, Y0, Y1 = fq32 of x0, x1, y0, y1 (pixel units); U0 = fq32(q[4]); V0 = fq32(q[6]);
   GU = fq32(du / dx); GV = fq32(dv / dy) (one float division each, then converted).
4. For py = 1 .. size − 2: cy = (2·py + 1) · 2^31 (the pixel centre; 2·py + 1 in 32-bit unsigned, then widened);
   skip the row unless Y0 ≤ cy < Y1; v = V0 + ((GV · ((cy − Y0) >> 16)) >> 16). For px = 1 .. size − 2:
   cx likewise; skip unless X0 ≤ cx < X1; u = U0 + ((GU · ((cx − X0) >> 16)) >> 16);
   a = `u3x_emb_a32(dds, &L, u, v)`; when a is not 0: `u3x_emb_blend(pix + (py · size + px) · 4, c16, a)`.

## 5. Proposed split for implementers (two independent parts)

- **Part A, maths and rasterizer** (old: about 215 lines): the Part A table of 2.1, sections 0, 3.3, 3.4, 3.10,
  4.1 to 4.4, 4.14 to 4.21. Pure: no game call, no global except the six coefficient copies. Needs from Part B
  nothing (a decoded-texture block is given to it).
- **Part B, memory, files, handles, records** (old: about 215 lines): the Part B table of 2.1, sections 2.2, 3.1,
  3.2, 3.5 to 3.9, 4.5 to 4.13. Needs from Part A nothing.

## 6. Test scenarios

Part A (pure; old against new, bit for bit, no stubs; thousands of random cases plus these):

1. `u3x_sincos119`: ±0, ±infinity, NaNs of both signs, denormals; |r| just inside and just outside 2^−12 around
   multiples of pi / 2 for all four quadrants and both flags; t exactly ±0.5 before the half is added; x where
   x · K lands on n + 0.5; huge x (2^31 and above: n = 0x80000000); every 255th of a turn (the angles G57 uses);
   `cosine` = 0 and 1. With the six patterns loaded from a fake image block by `u3x_sincos_bind`.
2. `u3x_fq32`: zero, denormals, 2^−41 .. 2^−40 (rounding to 0 or 1), ties (x.5 · 2^−32), k = 0, 37, 38, 39,
   infinity, NaN, both signs.
3. `u3x_log2q8`: powers of two, values just under them, 1.0, random; exponent field 0 and sign bit set (same
   bits as the old function).
4. `u3x_h16q16`: 0, −0, negatives, 1.0, 0.5, 1/255 steps (palette colours), ties to even at the 13-bit cut, half
   denormals (E −15 .. −25), E = −26, the largest value below 32768, 32768, 65504 and above, infinity, NaNs.
5. `u3x_emb_dxt`: DXT1 blocks with c0 > c1, c0 = c1, c0 < c1 and all four selectors; DXT5; x, y negative and past
   the edge; non-square textures; level sizes below 4; level where w >> l is 0.
6. `u3x_emb_lod`: rho2 = 0, −0, negative, below 4 (lam ≤ 0), exactly 4, large (cap at last), last = 0, a value
   giving l1 > last.
7. `u3x_emb_bil` / `u3x_emb_a32`: U, V of 0, 1, negative, above 1, fraction 0 and 255, f = 0 and f > 0, a
   texture of red 255 (result 2^32).
8. `u3x_emb_blend`: a32 = 0, 1, 2^32, above 2^32; destination 0 and 255; c16 = 0, 65536, above 65536, 0x7fffffff
   (wrapping).
9. `u3x_emb_quad`: sizes 128 and 256 (and 4, 2, 0); identity corners, a rotated and scaled set from
   `u3x_emb_layer_uv`; with and without mask; compare the whole target and check the outer ring is untouched.
10. `u3x_emb_decal`: quads inside, crossing the edges, dx or dy ≤ 0, NaN in q, boundaries exactly on pixel
    centres (lower bound inclusive, upper exclusive).

Part B (fake image, fake state, recording stubs for the game functions of 3.1 and for "allocate"; compare memory,
results, and the order and arguments of the calls):

11. `u3x_emb_alloc` / `u3x_emb_pop`: stack null; count 0; top allocator null; normal; the entry is zeroed and the
    count lowered.
12. `u3x_emb_file`: path object null; name null; a normal path (check the text handed to 0xf42fa0); texts long
    enough to hit the 0x1f8 limit in each piece; loader returns false; returns true with a null pointer.
13. `u3x_emb_h`: id 0; module null with find 0, find returning "none", a module; cached version equal / different;
    table null; count 0, 1, many; id present, absent, first and last.
14. `u3x_emb_bind1` / `u3x_emb_dcbind`: find 0; "none"; module without table; module with table; the order of the
    seven lookups, the ids read from the fake image's seven places, the two copies.
15. Release / retire / retick: fallback texture, zero pair, own texture; queue empty, partly full, full (release
    at once); three ticks; what 0xe73490 and 0xe4e8f0 receive (frame number).
16. `u3x_emb_ralloc` / `u3x_emb_rfree` / `u3x_emb_rfb` / `u3x_emb_isrec`: before and after the fallback exists;
    24 taken then a 25th; free of null, a non-record, an unused record, a record with and without a map.
17. `u3x_emb_texcreate` / `u3x_emb_fb`: map creation fails; texture creation fails; success on a record with and
    without an old map; the description and source words; device absent; fallback already there.
18. `u3x_emb_dds`: file missing; shorter than 128; wrong magic; wrong code; DXT1 and DXT5; mips 0, 1, 9, 40;
    truncated data; w or h 0; check off[], last, ec[123] and the free calls.

Needs the live game: only that a created texture shows on screen (formats and the description words of 4.12) and
that the temp allocator accepts the calls; all else is covered above.

## 7. Strays and what stays as it is

Covered here although the inventory does not list them in G56: `u3x_emb_isrec`, `u3x_emb_retick`, `u3x_emb_rfree`
(exported, in none of the inventory's lists), `u3x_emb_fmax` (one line, exported), and the private pieces of the old
file that go away with the rewrite (the structure definitions, the bind table, the accessors for F, the queue and
the record table, the "greater" compare).

Stay as they are: `u3x_emb_push`, `u3x_emb_free`, `u3x_emb_cache` (inventory "B, trivial"; contracts in 2.2); the C
part's storage (the 24 records, F, the retire queue, the 64 zero bytes, the compose flag) and all one-line C
wrappers and `u3h_` exports; `u3h_emb_alloc_default` is G58's (its `u3x_emblem_default_record`).

## 8. Behaviour that looks odd and must be kept

- Sine and cosine of an infinity return the infinity itself; cosine's small-angle shortcut returns exactly 1.0 and
  sine's returns the reduced argument.
- `u3x_fq32` does not saturate: the shift is capped (4.2). `u3x_log2q8` ignores the sign and a zero exponent.
- `u3x_emb_release` reads the frame counter before it knows whether anything is to be released.
- A retired entry's age is not reset when it is released; a new entry starts at 0.
- `u3x_emb_rfb` and `u3x_emb_texcreate` drop an old texture that has no map without releasing it (it is the
  fallback); `u3x_emb_ralloc` clears a free record without looking at what it held.
- `u3x_emb_rfb` leaves width and height of the old texture in the record (the fallback is 4 × 4).
- When the fallback does not exist yet, records are taken with texture 0 and "not loaded".
- `u3x_emb_dds` leaves width, height, block size and offsets written when it fails late (4.13 step 5).
- The path always contains "/vtex1/ps4/" whatever the platform text says; an over-long path is cut, not refused.
- The step shift of `u3x_emb_quad` is 8 for every size other than 256.
- `u3x_emb_blend` wraps for very large colour values; the mask product uses 128 bits.
- `f_fma119` is dead code (2.1).

## 9. Open questions

1. The six sine / cosine coefficients: the document that split the old file calls them "public Cephes values".
   They are not (Cephes' single-precision coefficients differ in the low digits). They, the two-part pi / 2 and
   the reduction scheme of 4.1 match, to my knowledge, the published constants of the open-source `simdmath`
   library (file `sincos_c.h`, names `_SINCOS_CC0..2`, `_SINCOS_SC0..2`, `_SINCOS_KC1`, `_SINCOS_KC2`); I could
   not open that source from here to confirm. Until someone confirms it, the new code takes them from the PS4
   image (3.7, 4.1), which needs the one `u3x_sincos_bind(base)` call at start-up (2.3). If confirmed, name them
   as that library's published constants and drop the bind function.
2. The seven data ids are read from instruction immediates of the game's function 0x66b510 (3.6). What should
   happen when a check byte does not match (another game build)? Proposed: `u3x_emb_dcbind` then binds nothing
   and leaves the handles as they are; say if a different reaction is wanted. (The helper calls a relocated copy
   of 0x66b510 elsewhere; the original bytes in the image are not patched as far as the old sources show.)
3. `f_fma119`: delete (proposed) or keep as a wrapper (2.1).

## Clarifications (tester, 2026-11-14)

Facts confirmed by the differential test (reimpl2\G56: old v7.212 against the new part_a.cpp / part_b.cpp; part A also
against the shipped machine code of v7.212, assembled from the listing the helper carries):

1. Open question 1: the six patterns at image 0x13f0760 (eboot.elf of the PS4 game, file offset + 0x4000) are, row
   by row, four equal copies of exactly the six coefficients the old code used, in the row order of 4.1. With them
   bound by `u3x_sincos_bind`, sine and cosine are equal to the old and to the shipped code for all 2^32 patterns and
   both flags. Whether they are the published `simdmath` constants was not checked (no source reachable here); the
   question stays open, the bind call works.
2. Open question 2: the seven ids at the image offsets of 3.6 have the stated check bytes in eboot.elf, and the ids
   equal the old code's ids in the order layer 0, 1, 2, scale 0, 1, 2, palette. With one check byte changed, the new
   `u3x_emb_dcbind` makes no call and changes no byte (planned); the old code binds as always (it never reads the
   image there).
3. Open question 3: `f_fma119` has no caller in v7.212 (it is a file-local function of agc_port.inc and only its
   definition exists); deleting it changes nothing.
4. The ids are compared unsigned in the bisection of 4.7 (a signed compare gives different lookups for ids of
   0x80000000 and above, which the real ids include).
5. `u3x_h16q16`: for exponents −25 .. −18 the result is 0 whatever the rounding does (h · 65536 is at most 1/2), so the
   limit "E < −25" is not observable at −25 or −24 (a changed limit there cannot be told apart by any input).
6. Reading order: `u3x_emb_release` reads the frame number before its first game call; a frame counter changed by
   the texture-release call is not seen (confirmed with a stand-in call changing it).
