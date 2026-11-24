# G55. Hat-stat card lines and tab box quads

Two independent parts, one implementer each:

- **Part A, tab box quads**: `u3x_gradient_texture`, `u3x_box_quad`, `u3x_box_item`.
- **Part B, hat-stat lines**: `u3x_hs_text`, `u3x_hs_save`, `u3x_hs_meas`, `u3x_hs_draw`.

The parts share nothing but the general facts of section G. All float work of this group is single precision on
32-bit patterns (the `f_*` helpers of CONVENTIONS.h), one rounding per operation, in exactly the operation order
given. The old build did the same (it never used the x87 unit in this group), so the new results must be equal to
the old ones bit for bit; no deviation is accepted here.

## G. General

### G.1 Words used below

- "word" = unsigned 32 bits; "float word" = a word holding the bits of a single-precision float.
- `I2F(n)` = `f_from_int(n)`; `TRUNC(f)` = `f_to_int(f)` (truncation; the value 0x80000000 for NaN and for values
  outside the signed 32-bit range); `a < b` on float words = `f_less(a, b)` (false when either is NaN).
- "usable pointer (4)": value >= 0x10000, below 2^47, a multiple of 4. "Usable pointer (8)": the same with a
  multiple of 8 (= `ptr_ok` of CONVENTIONS.h).
- "start check" of a game function: the word at its first byte equals 0xe5894855 (the bytes 55 48 89 E5).
- The helper's state block is `u3h_ext()`. Word arrays in it: **ea** at 0x3c50 (`ext_ea()`), **ec** at 0x4450 (no
  accessor in CONVENTIONS.h: `u3h_ext()` + 0x4450 + 4n).
- A game call "through the call block" = a `GameCall` of CONVENTIONS.h, every byte zero before it is filled, run by
  `u3x_k26_call`.

### G.2 Functions of other groups and of the C part (declare, do not write)

| Name | Signature | What it does | Part |
|---|---|---|---|
| `u3x_k26_call` | `uint64_t u3x_k26_call(void *game_call_block)` | CONVENTIONS.h: runs a game function with 6 integer, 5 float-register and 2 stack arguments | A B |
| `u3h_ext` | `uint8_t *u3h_ext(void)` | the helper's state block | A |
| `u3h_h13_mode` | `uint32_t u3h_h13_mode(void)` | C part: the tab-box experiment mode, 0 to 3 (each call also stores the mode in the C part's own diagnostics) | A |
| `u3h_w03_diag` | `uint32_t *u3h_w03_diag(void)` | C part: address of the eight diagnostic words "d" of the tab boxes | A |
| `u3h_active_page` | `uint32_t u3h_active_page(uintptr_t base)` | C part: id of the menu page shown now, 0 for none | B |
| `u3h_text` | `const char *u3h_text(uintptr_t base, void *s, uint32_t id)` | C part: localized text of a text id; never null in practice (G23), but Part B still tests for null | B |
| `u3x_sym_table` | `uint8_t *u3x_sym_table(void *s, uint32_t id)` | existing C++ function (G25, G27): the data of a script symbol, one call of the state's lookup function (pointer at `s` + 656) with `id`; null when that pointer, the handle or the pointer in the handle is null. No caching: "lookup of X" below is one such call | B |
| `u3x_ppc_ctiwz` | `uint32_t u3x_ppc_ctiwz(uint32_t f)` | G49: float word to signed integer the PS3 way: NaN gives 0x80000000; a value of magnitude 2^31 or more gives 0x80000000 when negative and 0x7fffffff when positive; anything else is truncated | B |
| `u3x_stat_icon` | `uint8_t *u3x_stat_icon(uintptr_t base, void *s, uint32_t stat, uint32_t m)` | G53: the icon texture of a hat stat for its multiplier (float word `m`), null when there is none or it is not loaded yet | B |

`u3x_w03_ok(const void *)` (the "usable pointer (4)" test, exported for the C part) is a one-line function next to
Part A; it stays as it is and is not delivered again.

---

## Part A. Tab box quads

### A.1 Purpose and callers

The PS3 game draws a gradient box behind every tab of a tab strip (menu item type 0x2d). The PS4 game still lays
these items out but its item draw has no case for the type, so nothing shows. Part A draws the box when such an item
reaches the game's "draw a laid-out item" function (image 0x63d6f0).

- `u3x_box_item` is called by the C function `w03_box`, which is the last step of the helper's value stub on the
  entry of image 0x63d6f0 (profile event kind 246; the C part runs four other fixers on the same registers first).
  Result 1 = "drawn here, the game function is skipped"; 0 = "not ours, the game function runs".
- `u3x_gradient_texture` and `u3x_box_quad` are exported for the C part (wrappers `w03_tex`, `w03_quad`, which have
  no caller in v7.212) and are the two steps `u3x_box_item` uses.

All of it runs on the game's UI draw thread; there is no state besides diagnostics.

### A.2 Interface

```
extern "C" uint64_t u3x_gradient_texture(uintptr_t base);
extern "C" void     u3x_box_quad(uintptr_t base, uint64_t context, uint64_t texture, const uint8_t *item,
                                 uint32_t x, uint32_t y, uint32_t argb);
extern "C" int      u3x_box_item(uintptr_t base, const uint64_t *r);
```

- `u3x_gradient_texture`: result = the texture handle of the gradient image, 0 = none.
- `u3x_box_quad`: `context` = the game's draw context (0 = queue for the normal UI pass), `texture` = a texture
  handle, `item` = a laid-out item (A.3), `x`, `y` = float words (top-left corner), `argb` = a colour word
  (alpha in bits 24 to 31, red 16 to 23, green 8 to 15, blue 0 to 7).
- `u3x_box_item`: `r` = the stub's saved values, 13 quadwords: [0] r11, [1] r10, [2] r9, [3] r8, [4] rdi, [5] rsi,
  [6] rdx, [7] rcx, [8] rax, [9] rbp, [10] return address, [11] first stack argument, [12] second stack argument.

The C wrappers stay as they are (`w03_box` passes `(base, r)`).

### A.3 Game facts

| Image offset | What |
|---|---|
| 0x63d6f0 | game function "draw a laid-out item". Arguments as the stub sees them: rsi = draw context, rdx = style block, rcx = laid-out item, r8 = rectangle, r9 (low 32 bits, signed) = horizontal alignment, first stack argument (low 32 bits, signed) = vertical alignment. Alignment values: 0 = left / top, 2 = right / bottom, anything else = centre |
| 0x1b24bc8 | pointer to the UI image directory (may be anything, test it). Directory + 0x20: pointer to the class-0 image table. Table + 2: u16 entry count; table + 8: pointer to the entries; an entry is 0x28 bytes with its u32 image id at +8 (as G06, G33, G47) |
| 0x11b89b0 | game function (u64 key, u32 index) -> 64-bit texture handle, 0 = none. Here the key is the image id with 0 in the high half (class 0), the index is the entry's index in the class-0 table or 0xffffffff when not found. No float register |
| 0xe42310 | game function, the quad draw (context block, texture block, vertex block), exactly as G04 A.3 / G47 G.4: context block = 6 quadwords {context, 0, 0, 0, 0, 0}; texture block = 4 quadwords {texture handle, 0, 0, 0}; vertex block = 16-byte aligned, 0x110 bytes = 68 words: four vertices of 16 words, then 4 zero words. A vertex: word 0 x, 1 y, 2 = 0, 3 u, 4 v, 5 to 7 = 0, 8 to 11 colour r, g, b, a (float words), 12 to 15 four times 1.0. No float register |

Image id of the gradient: 0x6e9d9193 (the PS4 image holds the same id as an immediate at 0x5dafd2 and 0x5db61d).

**Laid-out item** (the block image 0x640ac0 fills, 0x80 bytes): +0 u32 item type (0x2d = tab box); +0x10 u32
flags; +0x24 u32 item colour (same byte layout as `argb`); +0x3c float word width; +0x40 float word height.

**Style block** (10 float words): [0] scale, [1] red, [2] green, [3] blue, [4] alpha, [5] shadow distance,
[8] x offset, [9] y offset ([6], [7] not used here).

**Rectangle**: 4 float words x, y, width, height.

Float constants: 255.0 = 0x437f0000; 1/255 = 0x3b808081; 0.001 = 0x3a83126f; 0.15 = 0x3e19999a; 0.5 = 0x3f000000;
1.0 = 0x3f800000.

Switch: ea[240] bit 2 (value 4) = tab boxes on.

Diagnostics (plain increments and stores, no atomics): d = `u3h_w03_diag()`; d[0] / ec[164] type-0x2d items seen;
d[1] / ec[165] switch off; d[2] / ec[166] refused; d[3] / ec[167] quads issued; d[4] / ec[168] no texture; d[5] /
ec[169] last colour; d[6] / ec[170] last position; d[7] / ec[171] last flags; ec[175] "last draw had a context".

### A.4 Behaviour

**A.4.1 `u3x_gradient_texture(base)`**

1. Find the index of the gradient image: directory = the pointer at image 0x1b24bc8. Only when it is a usable
   pointer (8), and the table pointer at directory + 0x20 is one, and the table's count n is from 1 to 4095, and the
   entries pointer is a usable pointer (8): the index is that of the FIRST entry (0 to n - 1, in order) whose id is
   0x6e9d9193. In every other case, and when no entry matches, the index is "not found".
2. One game call 0x11b89b0 through the call block: integer 1 = 0x6e9d9193 (high half 0), integer 2 = the index as
   an unsigned 32-bit value zero-extended (0xffffffff for "not found"), everything else in the block 0. The call is
   made in every case. Result = what it returns.

**A.4.2 `u3x_box_quad(base, context, texture, item, x, y, argb)`**

No pointer tests (the caller made them).

1. Colour: C = for each of the four bytes of `argb` and the same byte of the item colour (item + 0x24), the product
   of the two bytes shifted right by 8 (so 255 * 255 gives 254), put back at the byte's position. Vertex colour
   (r, g, b, a) = `f_mul(I2F(byte), 1/255)` for the red, green, blue, alpha bytes of C, one multiplication each.
2. Corners, in this order: 0 = (x, y), 1 = (x1, y), 2 = (x1, y1), 3 = (x, y1), with x1 = `f_add(x, width)`,
   y1 = `f_add(y, height)` (item + 0x3c, + 0x40).
3. Texture coordinates: the unit square's corners in the same order, U = {(0,0), (1,0), (1,1), (0,1)} (float
   words 0 and 1.0; a rule, not a game table). rot = (flags OR (flags shifted right by 24)) AND 3, flags = the word
   at item + 0x10. Corner k takes U[(rot + k) AND 3].
4. Vertex block as A.3 (all 68 words zero first), the four corners with their coordinates, the same colour in each,
   four times 1.0 in words 12 to 15.
5. `u3h_h13_mode()` is called once. When it returns 3 (an experiment: opaque magenta, untextured): every vertex
   colour becomes (1.0, 0, 1.0, 1.0) and the texture handle passed is 0.
6. One game call 0xe42310 through the call block: integer 1 = address of the context block {context, 0, 0, 0, 0,
   0}, 2 = address of the texture block {texture (or 0 in mode 3), 0, 0, 0}, 3 = address of the vertex block;
   everything else 0.
7. Diagnostics, after the call: d[3] += 1; ec[167] += 1; d[5] = ec[169] = C (the multiplied colour, also in mode
   3); d[6] = ec[170] = (TRUNC(x) AND 0xffff) OR (TRUNC(y) shifted left by 16, low 32 bits kept); d[7] = ec[171] =
   flags.

**A.4.3 `u3x_box_item(base, r)`**, steps in this order; "return" ends the function:

1. item = r[7]. Not a usable pointer (4), or its type word is not 0x2d: return 0 (nothing counted).
2. d[0] += 1; ec[164] += 1.
3. ea[240] bit 2 clear: d[1] += 1; ec[165] += 1; return 0.
4. style = r[6], rect = r[3]. Either is not a usable pointer (4): d[2] += 1; ec[166] += 1; return 0.
5. Start check of 0xe42310 and of 0x11b89b0; either fails: d[2] += 1; ec[166] += 1; return 0.
6. alpha = style[4]. When the low 8 bits of `TRUNC(f_mul(alpha, 255.0))` are 0, or style[0] < 0.001: return 1
   (nothing to draw, nothing more counted).
7. Dimming factor m = 0.5 when style[1] < 0.15 and style[2] < 0.15 and style[3] < 0.15 (all three), else 1.0.
8. Position, with w, h = the item's width and height, ha = low 32 bits of r[2] as signed, va = low 32 bits of
   r[11] as signed:
   - x: ha = 2: `f_sub(f_add(rect.x, rect.width), w)`; ha = 0: rect.x; otherwise
     `f_add(f_mul(f_sub(rect.width, w), 0.5), rect.x)`.
   - y: the same with va, rect.y, rect.height, h.
   - then x = `f_add(x, style[8])`, y = `f_add(y, style[9])`.
9. texture = the result of A.4.1. 0: d[4] += 1; ec[168] += 1; return 1.
10. context = r[5]. `u3h_h13_mode()` is called once; when it returns 1 (an experiment: the queued UI pass), context
    = 0. ec[175] = 1 when context is not 0 (all 64 bits), else 0.
11. Drop shadow, only when 0.001 < style[5]: s = `f_mul(style[5], 0.5)`, replaced by 1.0 when 1.0 < s. Shadow
    alpha a = `TRUNC(f_mul(f_mul(f_mul(f_mul(m, s), alpha), 0.5), 255.0))` as an unsigned word. One A.4.2 quad with
    the context, the texture, the item, x, the y value `f_add(y, I2F(TRUNC(f_add(style[5], 0.5))))` and the colour
    (a shifted left by 24, low 32 bits kept; red, green, blue 0).
12. The box: colour = (TRUNC(style[1] * 255.0) AND 0xff) shifted left by 16, OR (TRUNC(style[2] * 255.0) AND 0xff)
    shifted left by 8, OR (TRUNC(style[3] * 255.0) AND 0xff), OR (TRUNC(alpha * 255.0) shifted left by 24, low 32
    bits kept); each product is one `f_mul`. One A.4.2 quad with the context, the texture, the item, x, y and this
    colour.
13. Return 1.

Order of the observable calls of a full draw: 0x11b89b0; `u3h_h13_mode`; (shadow: `u3h_h13_mode`, 0xe42310);
`u3h_h13_mode`, 0xe42310.

### A.5 Test scenarios

Everything runs on a fake image with recording stubs for 0x11b89b0 and 0xe42310 (through a stand-in for
`u3x_k26_call`), a settable `u3h_h13_mode`, and fake d / ea / ec arrays. Compare: results, d and ec words, the order
and arguments of the calls, the three blocks of each quad call byte for byte.

1. Item pointer odd / below 0x10000 / type 0x2c: 0, no counter.
2. Switch off: 0, d[0], d[1].
3. Style or rect pointer unusable; either start check failing: 0, d[2].
4. Alpha 0.0, alpha 0.003 (byte 0), alpha 1.004 (TRUNC 256, low byte 0), scale 0.0009: 1, no call at all.
5. Directory null, misaligned table, count 0, count 4096, misaligned entries, id absent, id present twice (first
   index wins): the index argument; the call is always made.
6. Texture 0: 1, d[4], no quad.
7. Alignment 0 / 1 / 2 / 7 / -1 on each axis, with offsets.
8. Shadow distance 0.001 (no shadow), 0.5, 2.0, 5.0 (s clamps); a dark style (all three below 0.15) and one with
   one component at 0.15 (not dark).
9. Flags 0, 1, 2, 3, 0x2000000, 0x3000001 (rotation); item colours 0xffffffff, 0x80808080, 0xa0000000.
10. Mode 1 (context 0, ec[175] = 0), mode 3 (magenta, texture 0, d[5] still the multiplied colour), mode 0 / 2.
11. Random style / rect / item floats including NaN, infinities and values beyond the integer range.

Live game only: that the boxes show behind the tabs (the default mode converts these items earlier in the C part,
so the path is reached only with the experiment switches).

---

## Part B. Hat-stat lines

### B.1 Purpose and callers

On the hats page of the loadout menus the PS3 game shows, under a hat's description, up to two lines such as
"+10% <stat name>" with the stat's icon left of each line. The PS4 game dropped this. Part B adds the lines to the
description text and draws the icons.

Both entry points are register stubs inside the game function at image 0x61a2b0 (it draws the description card of
the selected row):

- `u3x_hs_save`: profile event kind 249, at image 0x61a402, right after the game fetched the row's text of slot 4
  (the result is still in rax). C wrapper `hs_save119(base, s, regs)`.
- `u3x_hs_draw`: kind 250, at image 0x61a638, after the game drew that text. C wrapper `hs_draw119`.

`u3x_hs_text` and `u3x_hs_meas` are exported (declared in the C part) but called only from this part. Everything
runs on the game's UI draw thread, save and draw in the same call of 0x61a2b0; no locking.

### B.2 Interface

```
extern "C" int  u3x_hs_text(uintptr_t base, void *s, uint32_t stat, uint32_t m, char *out, uint32_t cap);
extern "C" void u3x_hs_save(uintptr_t base, void *s, uint64_t *regs);
extern "C" void u3x_hs_meas(uintptr_t base, uint32_t scale, const char *text, uint32_t w, uint32_t h,
                            uint32_t flag, uint32_t *out_w, uint32_t *out_h);
extern "C" void u3x_hs_draw(uintptr_t base, void *s, uint64_t *regs);
```

- `s` = the helper's profile state (only passed on to `u3x_sym_table`, `u3h_text`, `u3x_stat_icon`).
- `u3x_hs_text`: `stat` = a stat id, `m` = its multiplier (float word), `out` / `cap` = a text buffer and its size
  (cap >= 1; the only caller passes 96). Result 1 = a line was written, 0 = unknown stat (buffer untouched).
- `u3x_hs_meas`: `scale`, `w`, `h` float words; `flag` = the layout flag word; the two results are float words.
- `regs` = the register stub's 15 saved quadwords, in the order r15, r14, r13, r12, rbx, r11, r10, r9, r8, rdi,
  rsi, rdx, rcx, rax, rbp (elements 0 to 14, as G50 3.1). The stub loads the registers back from them, so writing
  element 13 changes the game's rax.

The C wrappers stay as they are.

### B.3 Game facts

| Image offset | What |
|---|---|
| 0x1b6a7c0 | u32: number of rows of the hat list shown |
| 0x1b6a7c8 | pointer to the hat table (may be null): +0 u32 entry count, +8 pointer to the entries (may be null); an entry is 0x38 bytes with the hat's u32 item id at +0 |
| 0x2208000 | array of u32, one per list row: the row's index into the hat table |
| 0x640ac0 | game function "lay an item out" (as G22 3.8): integers 1 = address of the laid-out item it fills (0x80 bytes, 16-byte aligned; +0x3c float word width, +0x40 float word height), 2 = address of an item description of 12 words (16-byte aligned), 3 = text or 0, 4 = address of two float words (box width, height; 16-byte aligned), 5 = address of four zero words (16-byte aligned), 6 = 0xffffffff; two stack words 0 and 0; float register 0 = a scale |
| 0x63d6f0 | game function "draw a laid-out item" (A.3): integers 1 = the UI object, 2 = draw context, 3 = address of a style block of 10 float words (16-byte aligned), 4 = address of the laid-out item, 5 = address of the rectangle (4 float words, 16-byte aligned), 6 = horizontal alignment; stack words: vertical alignment, then 0xffffffff; float register 0 = 0.0 |

Both are called through the call block: i[0..5] = the integers (32-bit values zero-extended), x[0] = the float
word (zero-extended), s[0], s[1] = the stack words, al = 1. (The old code set only that one float register and
left al at the low byte of the function address; the callees do not use al or the other float registers: the
same remark as G22 point 8.)

**Item description**, 12 words: [0] kind (1 = text, 2 = image), [2] and [3] = for an image the 64-bit texture
pointer (at byte 8), [4] = layout flag word, [7] = 0xffffffff, [9] = 0xffffffff, [10] = 1.0, all others 0.

**Frame of game function 0x61a2b0** (rbp = regs element 14) at image 0x61a638, valid when the game drew a text:

- rbp - 0x180: the text's rectangle R, 4 float words (x, y, width, height).
- rbp - 0xa8: the style block T the game used for the text; float words T[0], T[4], T[6], T[7] are read.
- rbp - 0x58: 10 words, the caller's style block (copied by the game from the function's argument).
- rbp - 0x118: the function's own laid-out-item block (0x80 bytes, free to reuse at this point).
- rbp - 0x198: quadword, the draw context.
- r13 (regs element 2): address of a block whose first quadword is the UI object (first argument of 0x63d6f0).
- r12 (regs element 3, low 32 bits signed): the list row. rax (regs element 13): the row's slot 4 text (may be
  null or empty; the game skips to 0x61a638 without drawing in that case).

**Script data** (through `u3x_sym_table`):

- Symbol 0x53ed596a, the stat table: +0 s32 count, +8 pointer to entries of 0x28 bytes: +0 u32 stat id, +0x24 u32
  text id of the stat's name. (G53 uses the icon names of the same entries.)
- Symbol 0xb8a3581b, the hat stats: +0 s32 count, +8 pointer to rows of 16 bytes: +0 u32 item id, +4 s32 number
  of pairs, +8 pointer to the pairs; a pair is 8 bytes: u32 stat id, then the multiplier as a float word.

Page id of the hats page: 0x211019f2.

Float constants: 1.0 = 0x3f800000; 100.0 = 0x42c80000; 0.05 = 0x3d4ccccd; 0.5 = 0x3f000000; 2.0 = 0x40000000;
4.0 = 0x40800000; 8.0 = 0x41000000; 24.0 = 0x41c00000; 64.0 = 0x42800000.

**Stat classes (a table of game ids: NOT to be written into the source).** The text form depends on three small
sets of stat ids. They are the PS3 text function's own cases; the PS4 game has no such table, but it holds seven of
the eight ids as the 32-bit immediate of the instruction that loads the stat id before a call of its stat query
(image 0x689d20). The new code reads each id from the image at run time (4 bytes, little-endian, at the offset
given; the byte before it is the instruction's opcode and is checked: when it differs, that id is treated as
matching no stat):

| Class | Members | Where the PS4 image holds each id (offset of the 4-byte immediate; opcode byte before it) |
|---|---|---|
| "inverse" (stored as 1/x) | 3 | 0x68a29a (0xbe); 0x68a295 (0xb8); 0xd55181 (0xbe) |
| "count" (a number, no percent) | 3 | 0xc2b9b6 (0xbe); 0x7ad19d (0xbe); 0xd46457 (0xbe) |
| "flag" (no number) | 2 | 0xc2cb11 (0xbe); the second id is NOT in the PS4 image: open question B.6.1 |

G53's `u3x_stat_icon` needs the same ids (and four more); one shared classifier for both groups is the simplest
result, see the report.

**Private state of the part** (zero at start): the joined text buffer J (384 bytes); two line-start offsets (u32);
two saved pairs (stat id, multiplier word); the line count N (s32); a "had a description" flag (s32, written, never
read).

**Texts** (bytes): "+ ", "+", "-", " ", "% ", the game text's line break = one backslash character (0x5c), an
indent of six spaces, "A", and the indent followed by "A".

### B.4 Behaviour

**B.4.1 `u3x_hs_text(base, s, stat, m, out, cap)`**

1. Lookup of 0x53ed596a. Null table, null entries pointer, count <= 0 or count > 512: return 0.
2. The entry whose stat id equals `stat`; with several, the LAST one. None: return 0.
3. name = `u3h_text(base, s, the entry's name text id)`. Null: return 0.
4. Write the line; at most cap - 1 characters are written in total (further characters are dropped silently), then
   a NUL. The line is, by class:
   - "flag": "+ " then the name.
   - "count": "+", the decimal number `u3x_ppc_ctiwz(m)` taken as a signed 32-bit value, " ", the name.
   - every other stat (percentages): for the "inverse" class first m = `f_div(1.0, m)`. lower = m < 1.0. Then "-"
     when lower, else "+"; the decimal number `u3x_ppc_ctiwz(f_mul(D, 100.0))` as a signed 32-bit value, with
     D = `f_sub(1.0, m)` when lower, else `f_sub(m, 1.0)`; then "% " and the name.
   Decimal numbers: no separators, no padding, a leading "-" for a negative value (e.g. a NaN multiplier of a
   percentage stat gives "+-2147483648% ").
5. Return 1.

**B.4.2 `u3x_hs_save(base, s, regs)`**, in this order; "return" leaves everything untouched:

1. `u3h_active_page(base)` is not 0x211019f2: return.
2. row = low 32 bits of regs[3], signed; count = the word at image 0x1b6a7c0. row < 0, row >= count, or count >
   96: return.
3. index = the word at image 0x2208000 + 4 * row. Hat table pointer (image 0x1b6a7c8) null, or index >= its entry
   count, or its entries pointer null: return. id = the item id of entry `index`.
4. Lookup of 0xb8a3581b. Null table, null rows pointer, count <= 0 or count > 4096: return.
5. description = the pointer in regs[13]. J is rebuilt from empty: when the description is not null and not empty,
   J starts with it. Everything appended to J stops at 383 characters (dropped silently).
6. For every row of the hat stats in order whose item id equals `id` (ALL matching rows, not only the first) and
   whose pairs pointer is not null: for its pairs in order, while fewer than two lines are stored:
   - line text = B.4.1 with a 96-byte buffer; result 0: this pair is skipped (it does not count as a line).
   - when J is not empty so far: append the line break.
   - line-start offset of this line = the length of J now (after the break).
   - append the indent, then the line text.
   - saved pair of this line = (stat id, multiplier word).
7. J is NUL-terminated (also when no line was stored: J then holds just the description, and nothing else changes).
8. When at least one line was stored: regs[13] = address of J; N = the number of lines; the "had a description"
   flag = 1 when the description was not null and not empty, else 0. When none was stored, N and regs[13] are NOT
   touched.

**B.4.3 `u3x_hs_meas(base, scale, text, w, h, flag, out_w, out_h)`**

One call 0x640ac0 (B.3) with: a laid-out-item block of its own (the old code did not clear it before the call;
the game fills it; clear it first), an item description of kind 1 with [4] = `flag`, the text, the size pair
(w, h), four zero words, 0xffffffff, stack words 0 and 0, float register 0 = `scale`. Then `*out_w` = the word at
+0x3c of the laid-out item and `*out_h` = the word at +0x40.

**B.4.4 `u3x_hs_draw(base, s, regs)`**

1. n = N; N = 0 (always, before anything else). n <= 0: return.
2. rbp = regs[14], r13 = regs[2]; R, T as in B.3. Style block S = a copy of the 10 words at rbp - 0x58 with S[1] =
   S[2] = S[3] = 1.0 and S[5] = 0.
3. Text scale sc = `f_mul(f_add(f_mul(f_mul(f_mul(T[6], T[7]), T[4]), 0.05), 1.0), T[0])`.
4. Three measurements (B.4.3) with sc and the box (R.width, R.height), in this order: "A" with flag 1, of which
   the height is the line height lh; "A" with flag 0, of which the width is w0; the indent followed by "A" with
   flag 0, of which the width is w1.
5. indent = `f_sub(w1, w0)`. When lh < 8.0 or 64.0 < lh: lh = 24.0. When indent < 4.0 or 64.0 < indent: indent =
   24.0. (A NaN passes both tests unchanged.)
6. Icon size ic: e = `f_sub(indent, 2.0)`; ic = e when e < lh, else lh. Icon x = `f_add(R.x, f_mul(f_sub(indent,
   ic), 0.5))`.
7. For line i = 0 to n - 1:
   - icon = `u3x_stat_icon(base, s, saved stat id, saved multiplier)`; null: next line.
   - prefix = the first (line-start offset of i) characters of J followed by "A" (the offset is at most 383; the
     old code skips the line when it is above 388, which cannot happen).
   - one measurement of the prefix with sc, the box (R.width, R.height) and flag 1; its height is hp.
   - y = `f_add(f_add(R.y, f_sub(hp, lh)), f_mul(f_sub(lh, ic), 0.5))`.
   - call 0x640ac0: laid-out item = rbp - 0x118; item description of kind 2 with the icon pointer; text 0; size
     pair (ic, ic); four zero words; 0xffffffff; stack words 0, 0; float register 0 = S[0].
   - call 0x63d6f0: UI object = the quadword at r13; draw context = the quadword at rbp - 0x198; style block S;
     laid-out item rbp - 0x118; rectangle (icon x, y, ic, ic); horizontal alignment 0; stack words 0 (vertical
     alignment) and 0xffffffff; float register 0 = 0.0.

The 0x63d6f0 call enters the game function at its start, so the helper's own stub on it (Part A and the C part's
other fixers) sees these icon items like any other item; nothing special is needed.

### B.5 Test scenarios

Call-recording stubs (fake image, fake `s` whose lookup returns test tables, stand-ins for `u3h_text`,
`u3h_active_page`, `u3x_stat_icon`, `u3x_ppc_ctiwz`, and for `u3x_k26_call` a recorder that fills +0x3c / +0x40 of
the laid-out item from a script) cover all of Part B. Compare: results, J, regs, the order and arguments of the
calls and the blocks they point to.

1. `u3x_hs_text`: null table; null entries; count 0, 513; stat absent; stat twice (last wins); null name; each
   class; multipliers 1.0, 1.1, 0.9, 0.0, negative, infinity, NaN, 3e9; an inverse stat with 0.5, 2.0, 0.0; cap 1,
   5, 96 with a long name (truncation, always NUL-terminated).
2. `u3x_hs_save`: wrong page; row -1, row = count, count 97; hat table null, index out of range, entries null;
   stats table null / count 0 / 4097; no matching row (J = description, regs unchanged, N unchanged); one row with
   0, 1, 2, 3 pairs; unknown stats between known ones; two matching rows of one pair each; a null pairs pointer;
   description null, empty, short, 380 characters long (truncation at 383); N left over from an earlier call.
3. `u3x_hs_meas`: the block contents and the two results.
4. `u3x_hs_draw`: N = 0 (no call); N = 1, 2; icon null for line 0 only; lh 7.9 / 8 / 64 / 64.1 / NaN; indent 3.9 /
   4 / 64.1; indent - 2 below and above lh; N is 0 afterwards in every case.

Live game only: that the lines and icons sit where the PS3 game shows them (the frame offsets of B.3).

### B.6 Open questions, bugs and dead code to keep

1. **Open: the second "flag" stat id is not in the PS4 image.** One string id (the checksum of a stat name the old
   notes do not give). Until its name is known (then `u3h_crc_sid(name)` generates it), the integrator has to
   supply it; it must not be written into the part as a literal next to the others. The old code's other user of
   the same id is the "shown on screen" stat query of another group.
2. Keep: step 7 of B.4.2 rewrites J even when no line is stored (J is not used by anyone then).
3. Keep: the "had a description" flag is written and never read.
4. Keep: N is not cleared by `u3x_hs_save` when a row has no lines; `u3x_hs_draw` clears it on every call, so a
   count cannot survive from one card to the next.
5. Keep: a stat with several matching rows contributes from each row until two lines exist.
6. Keep: 255 * 255 >> 8 = 254 in A.4.2 (a full-white box is drawn at 254/255), and the unmasked alpha shift in
   A.4.3 steps 11 and 12 (an alpha product of 256 or more wraps).
7. The laid-out-item block of B.4.3 was passed uncleared by the old code. If the tester's recorder compares its
   contents on entry, they are compared as "cleared" on the new side only.

## Clarifications (tester, 2026-11-14)

1. The seven image immediates of B.3 were checked in the PS4 executable (bytes copied from the file into the test's
   fake image): every opcode byte is as the table gives it, the seven ids are distinct, and each one falls in the
   old code's class the table names (three "inverse", three "count", one "flag"). With a different opcode byte the
   new code writes that stat as a plain percentage and leaves the other seven ids unaffected.
2. B.6.1 stays open: the old code holds exactly one more "flag" id besides the image's one (the test finds it in the
   old code at run time and hands it to the new side through `u3h_stat_flag2_id()`; with it, all eight classes
   agree). Its name is still unknown.
3. Read timing (not fixed by A.4.3 / B.4.2 / B.4.4; both sides equal while the words do not change during the
   calls): in `u3x_box_item` the old code read style[1], [2], [3] once for the dark test (before the 0x11b89b0 call)
   and again for the box colour (after the `u3h_h13_mode` calls), and read style[5] only after the 0x11b89b0 call
   and the first `u3h_h13_mode` call; the new code reads all of them once before the 0x11b89b0 call. In
   `u3x_hs_draw` the old code read R.width / R.height again for every measurement, R.x after the three first
   measurements and R.y per line after that line's icon query and measurement; the new code reads R once at the
   start. In `u3x_hs_save` the old code tested the description's first byte a second time (for the "had a
   description" flag) after the line loop; the new code tests it once before. None of the calls in between writes
   these words in the game (the style block and R belong to the calling game function's frame or its caller; the
   laid-out item at rbp - 0x118 ends at rbp - 0x98 and does not reach R), so this cannot show.
4. The frame item at rbp - 0x118 (0x80 bytes) overlaps T[0..3] (rbp - 0xa8 .. - 0x99). Both sides read T (for the
   scale) before the first call, so an icon layout that overwrites those words does not change the result.
5. The call block's `al` (1 on the new side, the low byte of the function address on the old side) does not reach
   anything the test or the game can observe; the 0x640ac0 / 0x63d6f0 callees do not use it (as B.3 says).
