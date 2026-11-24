# S04. Film and cinema leftovers

Behaviour description for the clean-room reimplementation (pipeline `reimpl2`, role 1, final sweep group S04). Old
code: helper v7.212, 216 code lines (13 still in C: `sk22_push`; the rest in the C++ part). It says WHAT the code
does. All offsets are hexadecimal. "Image X" means the address `base + X` (`base` = game image base). All values are
little-endian; u8/u16/u32/s32/s64 are integers of that width, "pointer" is 8 bytes. ExtState = the block
`u3h_ext()` returns; eb[n] = the u32 at ExtState + 0x4050 + 4n (`ext_eb()` of CONVENTIONS.h).

Two independent parts (no part calls the other):

- **Part A, "film data helpers"** (about 140 old lines): `u3x_film_entry_copy`, `u3x_sort_films`,
  `u3x_film_delete`, `u3x_film_record_header`, `u3x_clock_reset`, `u3x_render_defaults`, `u3x_stream_read`,
  `u3x_stream_write`, `u3x_bookmark_move`, `u3x_frame_make`, `u3x_frame_parent`.
- **Part B, "film tab bar update and cinema config"** (about 80 old lines): `u3x_k23_update` (with the tab rule,
  old internal `rw::tab_shown` and `rw::update_bar`), `u3x_cinema_config_push` (new, body of the C `sk22_push`).

Related specs (referred to, not repeated): `G03_film_playback.md`, `G04_film_hud.md`, `G05_cinema_directory.md`,
`G06_cinema_rows.md`, `film_helpers.md`, `film_maths.md`, `film_cameras.md`.

## 1. Purpose and callers

| Function | Called by | When |
|---|---|---|
| `u3x_film_entry_copy` | `u3x_sort_films`; G05's scan-and-free (`u3x_cin_scan_free`); C wrapper `cin_entry_copy` | moving film list entries |
| `u3x_sort_films` | G05's film scan (`u3x_cin_scan`) | after a directory scan |
| `u3x_film_delete` | `u3x_film_delete_hook` (only when Cinema is on); C wrapper `cin_delete` (no caller) | the player confirms Delete |
| `u3x_film_record_header` | G05's `u3x_film_open` (record mode, fewer than 20 films); C wrapper `cin_record_header` (no caller) | before a recording starts |
| `u3x_clock_reset` | G03's playback update (jump to a time) | seeking during playback |
| `u3x_render_defaults` | G03's playback update (once per playback) | playback start |
| `u3x_stream_read` | G03's playback update (32-bit times, 8-bit event types) | reading recorded events |
| `u3x_stream_write` | G03 (re-recording), film_helpers' `u3x_bookmark_create` | writing film data |
| `u3x_bookmark_move` | G03's bookmark thinning | when bookmark slots are compacted |
| `u3x_frame_make` | film_cameras / film_helpers (camera spawn); C wrapper `bf_make` | making a local camera frame |
| `u3x_frame_parent` | film_maths / film_cameras (every world / set-locator step); C wrapper `bf_parent` | before a bound frame is used |
| `u3x_k23_update` | G06's `u3x_k23_hud` (stub kind 160, once per HUD draw during film playback, after the callee check); C wrapper `k23_update` (no other caller) | every HUD frame of film playback |
| `u3x_cinema_config_push` | C `sk22_push` (stub kind 79: a value stub at the game's cinema init where it pushes the system module config, image 0x66699f) | Cinema start |

Background for Part B: PS4's HUD draw lost the update of the film tab bar (PS3 1.19 f_664c7c); the helper runs it
from a stub. PS4's cinema init pushes the system module config "multi-player-cinema", but PS4 registers only the
config "single-player", so the push would assert; the helper registers the missing config just before the push.

## 2. Interface

No function takes or returns a float. All are `extern "C"`. Existing names and signatures stay.

| Function | Signature | C wrapper |
|---|---|---|
| A1 | `void u3x_film_entry_copy(uint8_t *to, const uint8_t *from)` | `cin_entry_copy(d, s)`: unchanged |
| A2 | `void u3x_sort_films(uint8_t *list, int count, uint8_t *spare)` | none |
| A3 | `void u3x_film_delete(uintptr_t base, void *s)` | `cin_delete(base, s)`: unchanged |
| A4 | `void u3x_film_record_header(uintptr_t base, uint8_t *st)` | `cin_record_header(base, st)`: unchanged |
| A5 | `void u3x_clock_reset(uint8_t *clock, uint32_t game_time, int32_t t)` | none |
| A6 | `void u3x_render_defaults(uintptr_t base)` | none |
| A7 | `uint32_t u3x_stream_read(uintptr_t base, uint8_t *bs, uint32_t n)` | none |
| A8 | `void u3x_stream_write(uintptr_t base, uint8_t *bs, uint32_t value, uint32_t n)` | none |
| A9 | `void u3x_bookmark_move(uint8_t *d, const uint8_t *s)` | none |
| A10 | `void u3x_frame_make(uint8_t *bf, const uint8_t *local)` | `bf_make(bf, l)`: unchanged |
| A11 | `void u3x_frame_parent(uintptr_t base, uint8_t *bf)` | `bf_parent(base, bf)`: unchanged |
| B1 | `void u3x_k23_update(uintptr_t base, uint8_t *el)` | `k23_update(base, el)`: unchanged |
| B2 | `int u3x_cinema_config_push(uintptr_t base, const uint64_t *r)` (new) | `sk22_push` becomes `return u3x_cinema_config_push(base, r);` |

`r` (B2): the stub's saved registers, r[4] = rdi (first argument of the interrupted call). B2 always returns 0 (the
game's push runs afterwards). After B2 is in, the C statics `k22_cin`, `k22_sp` and `k22_eq` (a byte compare used
only by `sk22_push`; a stray, covered here) have no user and are removed. The old internal `rw::tab_shown`,
`rw::update_bar` and the 0x200-byte static `rw::click_sound` go with B1; the tab rule (4.B1) may be a file-private
helper of Part B.

Functions of the C part and of other groups that the new code calls (declare them; do not reimplement):

| Name | Signature | What it does |
|---|---|---|
| `u3h_ext` | `uint8_t *u3h_ext(void)` | ExtState address (CONVENTIONS.h; `ext_eb()`) |
| `u3h_cin_state` | `uint8_t *u3h_cin_state(uintptr_t base)` | the film state (writer), = image 0x17c3ba8 |
| `u3h_cin_path` | `char *u3h_cin_path(char *out, int size, const char *dir, const char *name)` | `dir`, `/`, `name` into `out`, cut after size - 1 characters, NUL-terminated |
| `u3h_cin_scan` | `void u3h_cin_scan(uintptr_t base, void *s, uint8_t *st)` | rebuilds the local film list from the directory (G05 4.5) |
| `u3h_cin_flags` | `void u3h_cin_flags(uint8_t *st)` | recomputes the Cinema button flags (G05 4.6) |
| `u3h_rp_assert` | `void u3h_rp_assert(uintptr_t base)` | the game's replay-stream assert (calls the game's assert handler, then traps; does not return in practice) |
| `u3x_bits_get` | `uint32_t u3x_bits_get(const uint32_t *words, uint32_t pos, uint32_t n)` | n (0..32) bits at bit `pos` of a 32-bit-word stream, most significant first; n = 0 gives 0 (own glue, stays) |
| `u3x_bits_put` | `void u3x_bits_put(uint32_t *words, uint32_t pos, uint32_t value, uint32_t n)` | writes the low n bits of value there; clears the rest of the word(s) it ends in (own glue, stays) |
| `u3x_k23_rep` | `int u3x_k23_rep(uintptr_t base, uint8_t *pad, uint32_t m)` | non-zero when a button of mask m fires this frame, with auto-repeat (G06 4.6) |
| `u3x_k23_action` | `void u3x_k23_action(uintptr_t base, uint32_t id)` | sends menu page `id` to the game's menu (eb[204] + 1, eb[208] = id, then game 0x64ce90(image 0x17bd080, id, 0)); own glue, stays |
| `u3x_k29_eok` | `int u3x_k29_eok(const void *p)` | 1 when p >= 0x10000, < 2^47 and a multiple of 4, else 0 (own glue, stays) |

## 3. Game facts

**Film list entry** (0x1e90 bytes, G05 3.5): +0x000 date text, +0x100 file name, +0x200 flag byte; +0x201..+0x207
are not part of the entry (they stay with the slot); +0x208 s64 time (seconds since 1970); +0x218 film header.

**Film state** (= writer, image 0x17c3ba8; G05 3.4): +0xa0 u32 header version, +0xb9 film directory text, +0x140
the header area (0x1c78 bytes), +0x1e48 s32 selected local film, +0x1e50 pointer to list A. Further fields used:
+0x31c u32 flags (bit 0 = restricted tab set), +0x1e68 s32 camera mode (0, 1, 2), render default block +0x1e94..
+0x1f13 (0x80 bytes) with its kept copy at +0x1f14..+0x1f93, and +0x1f94..+0x1fab (see A6).

**Record header** (in the header area h = st + 0x140, PS4 / 1.10 layout): h+0 u32 format version, h+4 u32 network
flag, h+8 u32 net game index, h+0x20 s64 time, h+0x28 the 24-byte custom rules block, h+0x1dc u32 matchmaking (2).

**Game data read by A4:** net object image 0x17d5c58 (s32 +0x10 and s32 +0x14: either non-zero = network game;
s32 +0x40 non-zero = matchmade game); u16 at image 0x17d167c (= net game manager 0x17cb060 + 0x661c) = net game
index; 24 bytes at image 0x17d1a98 (= 0x17cb060 + 0x6a38) = custom rules block. ExtState + 0x4060 (eb[4..5]): 8-byte
pointer to the C library `time` function (`int64_t f(int64_t *)`, called with null; may be null), as in G18 3.1.

**Kernel unlink:** the pointer at ExtState + 0x3220 (G05 3.1/3.3): `int f(const char *path)`, result ignored here.

**Playback clock** (A5): +0 u8 stopped flag (0 = running), +4 u32 rate (float bits), +8 u32 game base time, +0xc
s32 current time, +0x14 u32 step (float bits), +0x28 s32 second copy of the time; the other fields named in A5 are
zeroed. Constants: 1.0 = 0x3f800000; the step 1/60 s = 0x3c888889 (0.016666668), the game's frame step.

**Render settings:** pointer at image 0x16e2fb8 (the game's current render variables; not null-checked).

**Bit stream** (A7/A8): +0 pointer to u32 words, +8 u32 bit position, +0xc u32 capacity in bits.
**Bookmark** (0x30 bytes, film_helpers 3): stream {+0 words, +8 position, +0xc capacity, +0x10 u32 length}, +0x18
s32 time, +0x1c u32 read-stream position, +0x20 pointer to its own buffer, +0x28 u64 writer stamp.

**Bound frame** (0x60 bytes, film_maths 3.1): +0 and +4 u32 marker 0x7fcccccc, +8 pointer to a handle slot, +0x10
u32 body id, +0x20 parent position (4 floats), +0x30 parent rotation (x, y, z, w), +0x40 local position, +0x50
local rotation. A rigid body: +0x00..+0x1f its location (position, rotation), +0x50 u32 id, +0x54 u16 slot index.
The handle table base: pointer at image 0x16e2f30 (handle slots are 8 bytes each).

**Tab bar element** (HUD + 0x2408; G06 4.7, G04): +0 s32 cursor, +4 u32 smoothed cursor (float bits), +8 u32 page
waiting to be sent; 14 tabs of 24 bytes at +0xc (tab +0 u32 page id, +4 u32 second page id); +0x15c s32 tab count;
+0x160 14 pointers (the visible tabs); +0x1d0 u32 visible count; +0x1d4 u32 alpha (float bits); +0x1d8 s32 time.

**Data read by B1:** s32 image 0x17b4ba4 (= 0x17b4a90 + 0x114, the game clock in ticks of 1/300 s); u8 image
0x1acab73 (non-zero while a screenshot is taken); pointer image 0x16e2fa0 (pad array; pad 0 = that + 0x7e0, u32
+0x94 buttons pressed this frame, u32 +0x9c buttons held); u32 image 0x17bd088 (= menu 0x17bd080 + 8, non-zero
while a menu page is up); pointer image 0x1b24b80 (system options; u8 +0x39b4 == 1 means circle confirms); s32
image 0x18c6b58 (= 0x18c6b34 + 0x24, display width); u8 image 0x17d1d10 and 0x17d1d11 (= net game manager
0x17cb060 + 0x6cb0 / + 0x6cb1); u8 image 0x17c3571 and 0x17c3572 (two bytes the film actions set); s32 image
0x17c5d00 (= film reader 0x17c5cf8 + 8, its first file state word).
Pad bits: up 0x80, down 0x20, confirm 0x2000, cancel 0x4000.

**Game calls by B1** (plain System V calls with integer arguments, results ignored): sound start image 0x1007610
(sound object, u32 sound id 0xf57d53ce, u32 0); sound play/stop image 0x119b250 (sound object, u32 0). The sound
object is a 0x200-byte zero-initialised static block of the new part (U3X_BSS, 16-aligned), the same block for every
call and kept between calls.

**Tab ids** (B1): the film action ids of G04 A.3, which the game holds only as compare values in its code (no data
table); per the owner's decision G04 "Decisions" 3 they are interface ids used as compare values. Named here:
WIDE = 0xc1f65aeb, CAM1 = 0xff502160, CAM2 = 0x806c9824, CAM0 = 0x887c7d6e, T682 = 0x682c6868, TE81 = 0xe8184ce8,
T3CF = 0x3cf5deed, TA65 = 0xa65c61ba, T382 = 0x38231b34, T516 = 0x516bfbb8, TBD8 = 0xbd8d7974.

**System module config table** (B2; same layout as PS3's): entries at image 0x1809440, 0x30 bytes each {+0 name,
32 bytes, NUL-padded; +0x20 u64 module list pointer; +0x28 u64 value}; u32 count at image 0x1809620; at most 10
entries. The config names are in the game image: "multi-player-cinema" (20 bytes with its NUL) at image 0x145e698
(the push's own argument), "single-player" (14 bytes with its NUL) at image 0x14be998. PS4's single-player module
list is at image 0x16fb760.

## 4. Behaviour

### Part A

**A1 `u3x_film_entry_copy(to, from)`.** Copies bytes 0x000..0x200 and 0x208..0x1e8f of `from` to `to`, byte by
byte in ascending order; `to` bytes 0x201..0x207 are not written.

**A2 `u3x_sort_films(list, count, spare)`.** Entries are 0x1e90 bytes apart; key = s64 at entry + 0x208 (signed).
Result: newest first, entries with equal keys keep their order (stable). The moves are those of a straight insertion
sort, which fixes the observable state of `spare` and of the slot-bound bytes 0x201..0x207: for k = 1 .. count - 1
in order, when the key of slot k - 1 is less than the key of slot k: copy slot k into `spare` (A1); then, from
j = k downwards, while j > 0 and the key of slot j - 1 is less than the key in `spare`, copy slot j - 1 into slot j
(A1); then copy `spare` into the slot j where it stopped (A1). Otherwise slot k is left. count <= 1: nothing.
All copies go through A1 (so bytes 0x201..0x207 of every slot and of `spare` never change).

**A3 `u3x_film_delete(base, s)`.**
1. st = `u3h_cin_state(base)`. If the list pointer st + 0x1e50 is null: return (nothing else happens).
2. path (0x180-byte buffer) = `u3h_cin_path(path, 0x180, st + 0xb9, list + sel * 0x1e90 + 0x100)` where sel is the
   s32 at st + 0x1e48 taken as an unsigned 32-bit number (no range check).
3. Unlink `path` through the pointer at ExtState + 0x3220 (not checked for null; callers ensure Cinema is on).
4. If the s32 at st + 0x1e48 is not 0, decrease it by 1 (also when negative).
5. `u3h_cin_scan(base, s, st)`; then `u3h_cin_flags(st)`; then eb[19] + 1 (ExtState + 0x409c).

**A4 `u3x_film_record_header(base, st)`.** h = st + 0x140. In this order:
1. h+0 = the u32 at st + 0xa0.
2. h+4 = 1 when the s32 at net + 0x14 or the s32 at net + 0x10 is non-zero, else 0 (net = image 0x17d5c58).
3. h+8 = the u16 at image 0x17d167c, zero-extended to 32 bits.
4. If the time pointer (ExtState + 0x4060) is non-zero: h+0x20 (s64) = its result when called with null.
   Otherwise h+0x20 is not written.
5. h+0x28..h+0x3f = the 24 bytes at image 0x17d1a98 (byte copy).
6. h+0x1dc = 2 when the s32 at net + 0x40 is non-zero, else 0.
Nothing else in the header is written; no counter (the caller counts).

**A5 `u3x_clock_reset(clock, game_time, t)`.** Writes: u8 +0 = 0 (bytes +1..+3 untouched); +4 = 0x3f800000; +8 =
game_time; +0xc = t; +0x10 = 0; +0x14 = 0x3c888889; +0x18 = 0; u16 +0x1c = 0 (+0x1e..+0x1f untouched); +0x20 = 0;
+0x24 = 0; +0x28 = t; +0x2c = 0; u64 +0x30 = 0; u32 +0x38 = 0. Nothing else (the clock may be longer). Callers pass
game_time = the u32 at (pointer at image 0x1b24b98) + 0x20 (G03).

**A6 `u3x_render_defaults(base)`.** W = image 0x17c3ba8, R = the pointer at image 0x16e2fb8. 32-bit copies and
stores (plain bit patterns, no arithmetic), destination W + offset:

| W + | value | W + | value | W + | value |
|---|---|---|---|---|---|
| 0x1fa0 | R + 0x0 | 0x1eb8 | R + 0x880 | 0x1ec4 | 0x3f800000 (1.0) |
| 0x1fa4 | R + 0x4 | 0x1ebc | R + 0x884 | 0x1ea4 | 0 |
| 0x1fa8 | R + 0x8 | 0x1ec0 | R + 0x888 | 0x1e9c | 0 |
| 0x1ea8 | R + 0xc | 0x1e98 | 0 | 0x1ea0 | 0 |
| 0x1eac | 0 | 0x1f94 | R + 0x230 | 0x1ec8 | R + 0xba4 |
| 0x1eb0 | 0 | 0x1f98 | R + 0x234 | 0x1ecc | R + 0x4c |
| 0x1eb4 | 0 | 0x1f9c | R + 0x238 | 0x1ed0 | R + 0x8c |
| 0x1e94 | R + 0x898 (saturation) | | | 0x1ed4 | R + 0xcc |

(0x1eb8..0x1ec0 = tint multiply.) Then the 0x80 bytes W + 0x1e94..0x1f13 are copied to W + 0x1f14..0x1f93 (the kept
copy). W + 0x1ed8..0x1f13 are not written but are part of the copy.

**A7 `u3x_stream_read(base, bs, n)`.** P = position (+8). Result = `u3x_bits_get(words (+0), P, n)`. Then the
position = P + n (32-bit wrap). Then, if the capacity (+0xc) < P + n (unsigned, the wrapped sum): `u3h_rp_assert(base)`.
The bits are read before the check (also past the capacity).

**A8 `u3x_stream_write(base, bs, value, n)`.** P = position; `u3x_bits_put(words, P, value, n)`; position = P + n;
then the same capacity check and assert as A7. The write happens before the check.

**A9 `u3x_bookmark_move(d, s)`.** In this order: d+0x18 = s+0x18 (u32); d+0x1c = s+0x1c (u32); d+0x28 = s+0x28
(u64); d+8 = s+8 (u32); d+0xc = s+0xc (u32). Then copy N bytes, N = the u32 at **d** + 0x10 (the target's own length),
from the buffer pointed to by s+0x20 to the buffer pointed to by d+0x20, ascending byte by byte. d+0, d+0x10,
d+0x14, d+0x20, d+0x24 and d+0x2c are not written.

**A10 `u3x_frame_make(bf, local)`.** All 0x60 bytes = 0; then +0 = 0x7fcccccc, +4 = 0x7fcccccc, +0x3c = 0x3f800000
(parent rotation w = 1); then the 0x20 bytes of `local` are copied to +0x40..+0x5f (read after the clearing).

**A11 `u3x_frame_parent(base, bf)`.** H = the pointer at bf + 8, I = the u32 at bf + 0x10. If H is null or I is 0:
return. B = the pointer stored at H; null: return. Same body: the u32 at B + 0x50 equals I, or (I <= 0xfffe and its
low 16 bits equal I). Slot: (H − the pointer at image 0x16e2f30) as an unsigned 64-bit difference, shifted right by 3,
low 32 bits. If same body and the u16 at B + 0x54 (zero-extended) equals the slot: copy B's bytes 0..0x1f to bf +
0x20..0x3f. Otherwise nothing is written.

### Part B

**Tab rule SHOWN(id)** (reads game data each time it is asked; camera = s32 at image 0x17c3ba8 + 0x1e68). In order,
the first rule that applies decides:
1. id = WIDE and the display width (s32 image 0x18c6b58) is not 1280: hidden.
2. If either u8 at image 0x17d1d11 or 0x17d1d10 is non-zero: CAM1 with camera != 2: hidden; CAM2 or T682: hidden.
   Otherwise: CAM1 with camera != 1: hidden; CAM2 with camera != 2: hidden.
3. CAM0 with camera != 0: hidden.
4. If bit 0 of the u32 at image 0x17c3ba8 + 0x31c is set: TE81, CAM0, TA65, CAM2, T382, T516, CAM1 are hidden.
   Otherwise: on = (u8 image 0x17c3571 non-zero) ? (u8 image 0x17c3572 non-zero) : true; T3CF with not on: hidden;
   TE81 with on: hidden.
5. If the s32 at image 0x17c5d00 is non-zero: shown unless id is T682, TBD8 or T382.
6. Else: shown unless id is T516.

**B1 `u3x_k23_update(base, el)`.** In this order:
1. now = s32 image 0x17b4ba4 (read first). el+0x1d0 = 0.
2. If u8 image 0x1acab73 is non-zero: eb[202] + 1; return.
3. Visible list: count = s32 el+0x15c, at most 14 (negative = none). For i = 0 .. count − 1: tab = el + 0xc + 24·i;
   if SHOWN(u32 tab+0): store `tab` at el + 0x160 + 8·v and v = v + 1. Then el+0x1d0 = v; eb[206] = v.
4. pads = pointer image 0x16e2fa0; if not `ptr_ok`: return. pad = pads + 0x7e0.
5. Move (only when bit 0x4000 of pad u32 +0x9c is clear):
   a. If el+8 is non-zero: `u3x_k23_action(base, el+8)`, then el+8 = 0.
   b. old = s32 el+0; c = old. `u3x_k23_rep(base, pad, 0x80)` non-zero: c − 1. Then (always called)
      `u3x_k23_rep(base, pad, 0x20)` non-zero: c + 1. If c < 0: c = 0. Then if c > v − 1 (signed): c = v − 1 (so
      c = −1 when v = 0). el+0 = c.
   c. el+4 = (el+4 × 0.5) + (I2F(c) × 0.5): three single-precision operations, each rounded (`f_mul`, `f_from_int`,
      `f_mul`, `f_add`; 0.5 = 0x3f000000).
   d. If c != old: game 0x1007610(sound block, 0xf57d53ce, 0), then game 0x119b250(sound block, 0).
6. page_up = (u32 image 0x17bd088 != 0), read now. el+0x1d8 = page_up ? now − 3000 : now (32-bit wrap).
   el+0x1d4 = (el+0x1d4 × 0.7) + (target × 0.3), target = page_up ? +0 : 1.0; single precision, each product and the
   sum rounded (0.7 = 0x3f333333, 0.3 = 0x3e99999a, 1.0 = 0x3f800000; target × 0.3 is computed also when 0).
7. If el+0x1d4 < 0.9 (0x3f666666, ordered: NaN is not less): return.
8. confirm = 0x4000 when the pointer at image 0x1b24b80 passes `ptr_ok` and its u8 +0x39b4 is 1; else 0x2000. If
   pad u32 +0x94 AND confirm is 0: return.
9. c = s32 el+0 (read again). tab = (0 <= c < v) ? the pointer at el + 0x160 + 8·c : null. If
   `u3x_k29_eok(tab)` is 0: eb[205] + 1; return.
10. `u3x_k23_action(base, u32 tab+0)`; then el+8 = u32 tab+4.

**B2 `u3x_cinema_config_push(base, r)`.** Result 0 in every case.
1. If r[4] != base + 0x145e698: return.
2. eb[199] + 1. n = u32 image 0x1809620. list = base + 0x16fb760, value = 0.
3. For i = 0 .. while i < n and i < 10, entry E = image 0x1809440 + 0x30·i: if E's first 20 bytes equal the 20 bytes
   at image 0x145e698: return (already registered). If E's first 14 bytes equal the 14 bytes at image 0x14be998:
   list = u64 E+0x20, value = u64 E+0x28 (a later match replaces an earlier one; the loop goes on).
4. If n >= 10: eb[198] + 1; return (the game's assert follows).
5. Entry E = image 0x1809440 + 0x30·n: bytes 0..19 = the 20 bytes at image 0x145e698, bytes 20..31 = 0; E+0x20 =
   list; E+0x28 = value; then count = n + 1; then eb[197] + 1.

## 5. Float

No x87 in this group. Only B1 does float arithmetic (5c and 6), and the old build already did it with single-precision
SSE instructions (the `f_*` helpers of CONVENTIONS.h, one rounding per operation), so the new code is bit-exact with
the shipped helper. A5, A6 and A10 only store float bit patterns; A11 and A9 copy bytes.

## 6. Test scenarios

Part A (all with plain memory and recording stubs for `u3h_cin_*`, unlink, `time`, `u3h_rp_assert`):
- A1: random entries with distinct 0x201..0x207 bytes in `to`: they survive.
- A2: count 0, 1, 2 in order, 2 reversed, all equal keys (nothing moves, spare untouched), negative and large s64
  keys, 20 random entries with duplicates; compare list, spare and slot-bound bytes.
- A3: null list (no calls, no counter); sel 0 (stays 0), sel 3 (becomes 2), sel −1 (name pointer = list + 0xffffffff·0x1e90 + 0x100 in 64 bits, no wrap;
  sel becomes −2); order of calls path, unlink, scan, flags.
- A4: each net flag combination; time pointer null (h+0x20 kept) and set; custom block bytes.
- A5/A6/A10: untouched bytes keep a fill pattern; exact patterns written.
- A7/A8: n = 0, 1, 32; positions across a word boundary; P + n = capacity (no assert), capacity + 1 (assert after
  the bits), wrap of P + n.
- A9: d's length 0 (no bytes), 5, overlapping buffers; fields not written keep a pattern.
- A11: null handle, id 0, null body, id match full / low-16 with I <= 0xfffe / I = 0xffff low-16 match (no copy),
  slot match and mismatch, handle below the table base.
Part B (fake image memory, recording stubs for the two sound calls, `u3x_k23_action`, `u3x_k23_rep`):
- SHOWN: every tab id against every rule branch: width 1280 / other, NGM bytes, camera 0..2, flag bit, the two
  0x17c3571/2 bytes, reader word.
- B1: screenshot byte; tab count 0, 14, 15, −1; pads pointer bad; cancel held (no move, no waiting page); waiting
  page sent; up, down, both; clamp with v = 0 and at the ends; menu up / down alpha sequences crossing 0.9; NaN alpha;
  confirm with each options state; cursor out of range; tab pointer that fails `u3x_k29_eok`.
- B2: other r[4]; table with the cinema name at index 0 / last; single-player present twice; absent (PS4 list);
  n = 9, 10, 11; compare the table, count and eb[197..199].
Live game: B1 and B2 need a film playback / Cinema start; A3, A4 need the Cinema page.

## 7. Odd things in the old code, to be kept

1. A9 copies as many bytes as the **target's** length (+0x10), and `u3x_bookmark_create` sets every length to 0, so
   in practice no buffer bytes are copied.
2. A3 reads the selected index unsigned for the path but decreases it as signed; the unlink result is ignored.
3. A7/A8 read or write before the capacity check.
4. B1 writes el+0x1d0 = 0 before the screenshot test; with a bad pads pointer the visible list is still updated.
5. B1 clamps to 0 first and to v − 1 second, leaving −1 when no tab is visible.
6. A11 compares the full 32-bit slot number with a 16-bit field (G42 note 5 keeps `u3x_bf_handle_valid` different).
7. B2 takes the last single-player entry; an existing cinema entry ends the scan even after single-player was seen.

## 8. What stays as it is

The exported names and signatures above; the C wrappers named in 2; `u3x_bits_get`, `u3x_bits_put`,
`u3x_k23_action` (`rw::send_page`), `u3x_k23_callees` (`rw::callees_there`) and `u3x_k29_eok` (`rw::game_object`):
own glue; all image offsets and ids; the eb counter numbers (maintainers read them in the running game).

## Clarifications (tester, 2026-11-14, settled against v7.212 by the differential test)

1. A9: the field at +0x28 is copied as one 8-byte value, so d+0x2c..d+0x2f receive s+0x2c..s+0x2f. The sentence in
   4.A9 that lists d+0x2c among the bytes not written is wrong; the other bytes listed there (d+0, d+0x10, d+0x14,
   d+0x20, d+0x24) are indeed not written.
2. A6 store order: the table in 4.A6 read column by column (left column top to bottom, then the middle one, then the
   right one) is the order of the stores; then the 0x80 bytes are copied in ascending order. This is observable only
   when the render variables overlap the film writer; with such overlaps the result follows exactly that order.
3. A3 path buffer: 0x180 bytes on the stack, not initialised before `u3h_cin_path`, which receives size 0x180; the
   same buffer is then passed to the unlink. The selected index is read again after the unlink for the step back (a
   value changed by the path call or the unlink is the one decreased).
4. A11: I = 0xffff matches only by the full 32-bit id; a slot number of 0x10000 or more never matches the 16-bit
   field; a handle whose difference to the table base, shifted right by 3, has the right low 32 bits matches even when
   the upper bits are not zero (also when the handle lies below the table base).
5. B1 step 9: the visible count is the one built in step 3 (kept, el+0x1d0 is not read again); a change of el+0x1d0
   made between step 3 and step 9 has no effect. The cursor el+0 is read again at step 9.
6. B1 read points: the clock is read once at step 1, before any call (a later change is not seen); el+0x1d8 is
   now - 3000 modulo 2^32 (now = 0x800003e8 gives 0x7ffffc18). el+8 is read before the waiting-page action and
   cleared after it; el+0 is read after that action; el+4 after both button calls; the menu-page word after the sound
   calls; the options pointer, its byte and pad +0x94 at step 8; tab+0 before the final action and tab+4 after it.
   The tab rule's reads all happen in step 3, before the first call of B1, so their order inside the rule is not
   observable.
7. B1 alpha test: "less than 0.9" is strict; an alpha of exactly 0x3f666666 goes on to the confirm test, and a NaN
   alpha goes on too.
8. B1 float: the shipped helper already did steps 5c and 6 with the single-precision SSE helpers, one rounding per
   operation, so the new code is bit-exact (checked with NaN, infinities, denormals and values near 0.9).
9. A4: the s32 at net + 0x40 and the 24 custom-rules bytes are read after the time call. A7/A8: the capacity is read
   after the bits call (a capacity lowered by that call fires the assert).
10. B2: the 20 bytes at image 0x145e698 and the 14 bytes at image 0x14be998 of the PS4 image are exactly
    "multi-player-cinema" and "single-player", each with its terminating NUL. B2 makes no game call. A single-player
    entry is recognised by its first 14 bytes only (bytes after the NUL are not looked at).
11. Section 7, points 1 to 7: all confirmed as described.
