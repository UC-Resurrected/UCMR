# G47. HUD: progress rings, ring colours, element icons, mission-complete message

Behaviour description for the clean-room reimplementation (pipeline `reimpl2`, role 1). Old code: helper v7.212.
The work group is 284 code lines (40 still in C: `k32_ring`, `g3_icon`; 244 in the C++ part); with the strays of
section 8 about 350. Everything below is WHAT the code does. All image offsets are offsets from the game image
base (`base`, in some signatures `b`).

Three independent parts, one implementer each:

- **Part A, "segmented rings"** (about 110 old lines): `u3x_marker_ring`, `u3x_marker_ring_site`,
  `u3x_counter_ring`, `u3x_counter_ring_site` (and, private to the part, the segment count and the ring draw).
- **Part B, "element ring and element icons"** (about 110 old lines): `u3x_ring_colour`, `u3x_hudnet_ring` (new,
  takes over the C function `k32_ring`), `u3x_grapple_icon` (new, takes over the C function `g3_icon`),
  `u3x_mode_icon`, `u3x_icon_quad`.
- **Part C, "mission-complete message"** (about 130 old lines): `u3x_ms_text`, `u3x_ms_len`, `u3x_ms_itoa`,
  `u3x_ms_replace`, `u3x_ms_icon_find`, `u3x_ms_icon`, `u3x_ms_hud`, `u3x_mission_done`.

Part B calls Part A only through `u3x_counter_ring` (exported name and the behaviour of A.4.4). Part C shares
nothing with A and B. The section "Common" is for all three.

---------------------------------------------------------------------------------------------------------------------

## Common

### G.1 Numbers

Every float is handled as its 32-bit IEEE single pattern in a `uint32_t` ("float bits"); nothing is passed or
returned as `float` or `double`. The notation is the one of G04_film_hud.md G.1:

- `a + b`, `a - b`, `a * b`, `a / b`: ONE single-precision operation, round to nearest even, the result is a 32-bit
  float before it is used again (`f_add`, `f_sub`, `f_mul`, `f_div`). Division by zero does not fault.
- `PS(a, b, c)`: `a * b + c` with the product exact in double, the sum rounded once to double and then to single
  (`f_madd(a, b, c)`; not a fused multiply-add, not an extended-precision sum).
- `I2F(n)`: signed 32-bit integer to single (`f_from_int`). `F2I(x)`: single to signed 32-bit integer, truncation
  toward zero; NaN or out of range gives 0x80000000 (`f_to_int`).
- `x < y`: ordered less-than, false when either is NaN (`f_less`).

Constants are written as value (pattern). The old build of this group ran none of its float steps on the x87 unit:
every step, in the C functions too, was one of the single-precision operations above. A tester can therefore
expect bit-for-bit equality with the shipped helper, not only with the host build. (The colour decode behind
`u3h_ring_colour` is x87 code of the C part; it belongs to another group and is only called here.)

### G.2 Calling the game

No game function of this group takes a float argument. All are called as plain calls with integer and pointer
arguments (`game_call<Result>(base + offset, ...)`), except the one float-returning function of A.3, which is
called through `call_float1` (G.3). No call block (`u3x_k26_call`) is needed.

### G.3 Things of other groups, of the C part and of the shared tools

Declared by the implementer from these signatures (`extern "C"`; `u3h_` = hidden-visibility exports of the C part):

| Name | Signature | What it does | Part |
|---|---|---|---|
| `u3h_ext` | `uint8_t *u3h_ext(void)` | the helper's state block (CONVENTIONS.h). Word arrays in it: **eb** at 0x4050 (`ext_eb()`), **ec** at 0x4450 (no accessor: `u3h_ext()` + 0x4450 + 4n), **ms** at 0x2da0 (no accessor: `u3h_ext()` + 0x2da0 + 4n, eight u32) | A B C |
| `u3h_sincos` | `uint32_t u3h_sincos(uint32_t x, uint32_t cosine)` | sine (cosine = 0) or cosine (cosine = 1) of the angle x in radians, float bits in and out (G04 G.3) | A |
| `u3h_ring_colour` | `void u3h_ring_colour(uint32_t *col)` | the C part's colour correction for rings drawn into the queued UI pass: unless a build switch is set it replaces col[0..2] (float bits) by their decoded values and counts two diagnostic words of its own; col[3] is never changed | A B |
| `u3h_t009_diag` | `uint32_t *u3h_t009_diag(void)` | the address of the 16 diagnostic words **t9** of the marker ring (always the same address) | A |
| `call_float1` | `uint32_t call_float1(uintptr_t fn, uint64_t a0)` | existing file-private assembly shim of the C++ part (inventory: trivial; it stays): calls the game function at address `fn` with one integer argument on a 16-byte aligned stack and returns the low 32 bits of xmm0 (a float result as float bits). The integrator puts it in scope of Part A | A |
| `ptr_ok` | CONVENTIONS.h | at least 0x10000, below 2^47, a multiple of 8. (The old file-private `aligned_pointer` is the same test; use `ptr_ok`.) | A B |
| `Log` | `void (*)(uintptr_t base, const char *line)` | a log-line function of the C part (CONVENTIONS.h `Log`, `Line`) | B |

Diagnostic words (eb, ec, ms, t9 and the blocks of Part B) are plain 32-bit adds and stores that maintainers read
in the running game's memory; a test compares them. "+= 1" is a 32-bit wrapping add.

### G.4 Game facts used by more than one part

| Image offset | What |
|---|---|
| 0x11b89b0 | game function (u64 key, u32 index) -> 64-bit texture handle, 0 = none. A **sprite descriptor** is 12 bytes: the u64 key at +0 (string id of the texture name in the low half, class 1 in the high half), the u32 index at +8 (0xffffffff = look up by id). The descriptor's two values are passed as they are |
| 0xe42310 | game function, the quad draw (context block, texture block, vertex block), as G04 A.3: context block = 6 quadwords, all 0 here (the quad is queued for the game's normal UI pass); texture block = {texture handle, 0, 0, 0}; vertex block = 16-byte aligned, 0x110 bytes = 68 words: four vertices of 16 words, then 4 words of which the first (word 0x40) is the **depth** (float bits) and the others 0. A vertex: word 0 x, 1 y, 2 = 0, 3 u, 4 v, 5..7 = 0, 8..11 colour r, g, b, a (float bits), 12..15 four times 1.0 (0x3f800000) |
| 0x16eafe0 | two u32 float bits: the screen alpha of screen 0 and of screen 1 |
| 0x1806480 | pointer to the HUD being drawn (may be null); its u32 at +0x1d0 is the screen number |

"Function start check" of an offset: the u32 at that image offset equals 0xe5894855.

---------------------------------------------------------------------------------------------------------------------

## Part A. Segmented rings

### A.1 Purpose and callers

PS3 1.19 draws a progress ring as a row of small textured quads on a circle. The PS4 game kept the code around
the ring calls but not the calls (it has a trap instruction at each place), so the helper draws the rings at
three places: around a world marker (two places in the marker draw; stub kinds 200 and 201, stub sites at image
0xc2a21a and 0xc2a2e3), around the HUD counter (stub kind 161, trap at 0xc37a97), and around a HUD-net element
(Part B). All run on the game's HUD draw thread, once per marker / counter / element per frame.

Callers: C `t009_ring_site` -> `u3x_marker_ring_site`; C `t9_ring` -> `u3x_marker_ring`; C `k24_ringsite` (from
`cinema_kind`, kind 161) -> `u3x_counter_ring_site`; C `k24_ring` and Part B -> `u3x_counter_ring`. The C functions
are already one-line wrappers and stay.

### A.2 Interface

| Function | Signature (all `extern "C"`) |
|---|---|
| marker ring | `void u3x_marker_ring(uintptr_t b, uint32_t scale, uint32_t p, uint32_t depth, uint32_t x, uint32_t y, const uint8_t *desc, const uint32_t *col)` |
| marker sites | `int u3x_marker_ring_site(uintptr_t b, const uint64_t *r, int site)` |
| counter ring | `void u3x_counter_ring(uintptr_t b, uint32_t scale, uint32_t p, uint32_t r1, uint32_t r2, uint32_t depth, uint32_t x, uint32_t y, const uint8_t *desc, const uint32_t *col)` |
| counter site | `int u3x_counter_ring_site(uintptr_t b, const uint64_t *r)` |

`scale`, `p` (progress), `depth`, `x`, `y`, `r1`, `r2` are float bits; `desc` is a sprite descriptor (G.4); `col`
is four float bits r, g, b, a. `r` is the array of saved registers the stub hands over (A.3). Both site functions
always return 0.

The old file-private functions `ring_segments` and `draw_ring` are not delivered as functions; their behaviour is
A.4.1 and A.4.2, and how the part arranges it is free.

### A.3 Game facts

**Marker sites** (`r` = three quadwords: r[0] = the game's r12, r[1] = r15, r[2] = rbp of the marker draw).
Site 0 (kind 200): the marker list is r[0]; the entry offset is the u64 at rbp - 0x1c0. Site 1 (kind 201): the
marker list is r[1]; the entry offset is r[0]. Marker list: entries of 0x60 bytes from list + 0x10, at most 8;
the ring's sprite descriptor is at list + 0x5d0. Entry: +0x34 u32 packed colour, +0x44 progress (float bits).
Frame values: rbp - 0x254 the marker scale (float bits); rbp - 0x1a0 and rbp - 0x19c the screen position x, y.

**Counter site** (`r` = saved registers: r[4] = the game's rbx = the counter's draw object, r[14] = rbp of the
HUD draw). Draw object: +0x120, +0x124 position x, y (float bits); +0x128 pointer to the style, whose four u32 at
+0x18, +0x1c, +0x20, +0x24 are the colour r, g, b, a (float bits). The HUD is the pointer at rbp - 0x11a8; the
ring's sprite descriptor is at HUD + 0x1a90 + 0x5d0 (the HUD's marker list, same descriptor as above).
Game function 0x745240 (one argument: the address of image 0x17cb060, the net game manager; result: a float in
xmm0) gives the counter's progress.

**Constants.** 1.0 (0x3f800000); 10.0 (0x41200000) and 30.0 (0x41f00000): quads of an empty and of a full ring at
scale 1; 0.5 (0x3f000000); 0.45 (0x3ee66666): the sprite's outer radius in texture units; 2 pi (0x40c90fdb);
pi / 2 (0x3fc90fdb); marker ring: radii 24.0 (0x41c00000) and 33.0 (0x42040000), scale factor 1.75 (0x3fe00000),
depth 0.0 at site 0 and 1.0 at site 1; counter ring: scale 0.475 (0x3ef33333), radii 30.0 (0x41f00000) and 50.0
(0x42480000), depth 0.0; 1 / 255 (0x3b808081). At most 512 quads. These are single numbers of the PS3 behaviour,
not a table.

**Diagnostics.** t9 (from `u3h_t009_diag()`): [0] site-0 calls, [1] site-1 calls, [2] rings drawn, [3] no
texture, [4] last packed colour, [5] last progress, [6] last quad count, [7] refused, [8] last scale. eb[210]
counter site calls, eb[211] counter rings drawn, eb[212] refused, eb[213] last counter progress. ec[2] bit 0x20
(a build switch): the packed marker colour is taken in the other byte order.

### A.4 Behaviour

**A.4.1 Quad count N(scale, p).**

1. q = p, except: when p has its sign bit set and is not exactly -0 (0x80000000), q = +0 (pattern 0). So -0 stays
   -0, a NaN with the sign bit clear stays, a NaN with the sign bit set becomes 0.
2. q' = q when (q - 1.0) < 0; otherwise 1.0 (so a NaN q gives 1.0).
3. count = PS(1.0 - q', 10.0, q' * 30.0).
4. m = scale when (1.0 - scale) < 0; otherwise 1.0 (scale below 1 or NaN counts as 1).
5. N = F2I(m * count).

**A.4.2 Ring draw D(b, rin, rout, p, n, depth, x, y, desc, col)**, n from 1 to 512; result "drawn" or "no texture".

1. texture = 0x11b89b0(u64 at desc + 0, u32 at desc + 8). 0: nothing more, result "no texture".
2. ratio = (rin * 0.45) / rout; sweep = p * 2 pi (the raw p, not the clamped one); inv = 1.0 / I2F(n);
   alpha = col[3] * (u32 at image 0x16eafe0; always screen 0's word).
3. For i = 0 .. n - 1, in order:
   - angle0 = PS(inv, I2F(i) * sweep, pi / 2); angle1 = PS(inv, I2F(i + 1) * sweep, pi / 2).
   - Four `u3h_sincos` calls in this order: (angle0, 1), (angle0, 0), (angle1, 1), (angle1, 0). They are made for
     every quad (the values of the previous quad are not reused).
   - A vertex block (G.4), all 68 words 0 first, then four vertices: 0 = inner radius at angle0, 1 = outer radius
     at angle0, 2 = outer radius at angle1, 3 = inner radius at angle1. With c, s the cosine and sine of the
     vertex's angle, R = rin (inner) or rout (outer), T = ratio (inner) or 0.45 (outer):
     x = PS(c, R, x), y = PS(s, R, y), u = PS(c, T, 0.5), v = PS(s, T, 0.5); colour col[0], col[1], col[2], alpha;
     four 1.0. Word 0x40 = depth.
   - One call 0xe42310(context block of six 0, texture block {texture, 0, 0, 0}, the vertex block).
4. Result "drawn".

The ring starts at angle pi / 2 and covers p of a full turn; each quad covers sweep / n.

**A.4.3 `u3x_marker_ring`.** n = N(scale, p). t9[6] = n (as u32); t9[5] = p; t9[8] = scale. n <= 0: return.
n > 512: t9[7] += 1, return. D(b, scale * 24.0, scale * 33.0, p, n, depth, x, y, desc, col): "no texture":
t9[3] += 1; "drawn": t9[2] += 1.

**A.4.4 `u3x_counter_ring`.** n = N(scale, p). n <= 0: return (nothing counted). n > 512: eb[212] += 1, return.
D(b, scale * r1, scale * r2, p, n, depth, x, y, desc, col): "no texture": eb[212] += 1; "drawn": eb[211] += 1.

**A.4.5 `u3x_marker_ring_site`.** `site` is 0 or 1.

1. t9[site] += 1.
2. rbp = r[2]; list = r[0] for site 0, r[1] for site 1. Either fails `ptr_ok`: t9[7] += 1, return 0.
3. k = the u64 at rbp - 0x1c0 for site 0, r[0] for site 1. k >= 0x300 or k not a multiple of 0x60: t9[7] += 1,
   return 0.
4. entry = list + 0x10 + k; p = the u32 at entry + 0x44. p has its sign bit set (also -0), or its exponent bits
   are all ones (infinity, NaN): return 0, nothing more.
5. c = the u32 at entry + 0x34; t9[4] = c. Four values, each I2F(byte) * (1 / 255): v3 from bits 31..24, v2 from
   bits 23..16, v1 from bits 15..8, v0 from bits 7..0. Colour block: (r, g, b, a) = (v2, v1, v0, v3) (the word is
   alpha, red, green, blue from the top); when ec[2] has bit 0x20: (v3, v2, v1, v0) instead.
6. `u3h_ring_colour(colour block)`.
7. `u3x_marker_ring(b, (u32 at rbp - 0x254) * 1.75, p, depth, u32 at rbp - 0x1a0, u32 at rbp - 0x19c,
   list + 0x5d0, colour block)` with depth 0.0 for site 0 and 1.0 for site 1. Return 0.

**A.4.6 `u3x_counter_ring_site`.**

1. eb[210] += 1.
2. object = r[4]; rbp = r[14]. Either fails `ptr_ok`: eb[212] += 1, return 0.
3. hud = the pointer at rbp - 0x11a8; style = the pointer at object + 0x128. Either fails `ptr_ok`: eb[212] += 1,
   return 0.
4. Function start check (G.4) of 0x745240, 0x11b89b0 and 0xe42310; any fails: eb[212] += 1, return 0.
5. p = `call_float1(b + 0x745240, b + 0x17cb060)`; eb[213] = p.
6. Colour block = the four u32 at style + 0x18 .. + 0x24; `u3h_ring_colour(colour block)`.
7. `u3x_counter_ring(b, 0.475, p, 30.0, 50.0, 0.0, u32 at object + 0x120, u32 at object + 0x124,
   hud + 0x1a90 + 0x5d0, colour block)`. Return 0.

`u3h_t009_diag()` returns a fixed address; how often it is called is not observable and not to be compared.

### A.5 Test scenarios

All of Part A runs on a fake image with recording stubs for 0x11b89b0, 0xe42310, 0x745240 (through a stand-in for
`call_float1`), `u3h_sincos` (the real routine or a recording one), `u3h_ring_colour`, and fake t9 / eb / ec words.
Compare the stub calls in order with their arguments, the three blocks of every quad draw bit for bit, and the
diagnostic words.

1. N: p = 0, 0.25, 0.5, 1, 1.5, -0, -0.5, +NaN, -NaN, +infinity, tiny positive; scale = 0, 0.475, 1, 1.75, 3, 40
   (above 512 quads), NaN, negative; thousands of random pairs.
2. D: n = 1, 2, 30, 512; texture 0 (no sincos call, no draw); rout = 0 (ratio infinite or NaN, still drawn);
   col alpha with the screen word 1.0, 0.5, 0.
3. `u3x_marker_ring`: n <= 0 (only t9[5], [6], [8] written), n = 513 (t9[7]), no texture (t9[3]), drawn (t9[2]).
4. `u3x_counter_ring`: the same four ends with eb[211] / eb[212]; n <= 0 counts nothing.
5. Marker site: both sites; bad rbp, bad list; k = 0, 0x60, 0x2a0, 0x300, 0x61, a huge k; p = -0, negative,
   infinity, NaN (return before t9[4]), 0, 0.5; the colour word 0x4080d030 with ec[2] bit 0x20 clear and set;
   depth 0.0 / 1.0.
6. Counter site: each refusal (bad object, bad rbp, bad hud, bad style, each of the three function starts wrong);
   the full path with a progress answer of 0.3.

Needs the live game: that the rings sit on the markers and the counter and have the right colour.

---------------------------------------------------------------------------------------------------------------------

## Part B. Element ring and element icons

### B.1 Purpose and callers

A HUD-net element (the marker over a co-op partner who is knocked down, grabbed, or needs help) is drawn on PS3
1.19 with a coloured progress ring and an icon inside it; PS4 kept only the two sprites before them and has a trap
where the ring call was (image 0xc31074; stub kind 164). The helper draws the ring for element modes 1, 2 and 5 and
the icon for modes 1 (knocked down) and 2 (grabbed). HUD draw thread, once per element per frame.

Callers: C `cinema_kind` (kind 164) -> C `k32_ring` -> `u3x_hudnet_ring`; C `g3_icon` -> `u3x_grapple_icon`.

### B.2 Interface

| Function | Signature (all `extern "C"`) | State |
|---|---|---|
| element ring | `int u3x_hudnet_ring(uintptr_t b, const uint64_t *r, void (*grapple)(uintptr_t, const uint8_t *, const uint8_t *, uint32_t, uint32_t), Log log)` | new |
| grabbed icon | `void u3x_grapple_icon(uintptr_t b, const uint8_t *el, const uint8_t *mod, uint32_t x, uint32_t y, uint32_t *diag, void (*seen)(uintptr_t, const uint8_t *, uint32_t, uint64_t), void (*drawn)(uintptr_t, uint32_t, uint32_t))` | new |
| mode icon | `void u3x_mode_icon(uintptr_t b, const uint8_t *el, const uint8_t *mod, uint32_t x, uint32_t y, uint32_t mode, Log log)` | existing |
| icon quad | `void u3x_icon_quad(uint32_t *v, uint32_t x, uint32_t y, uint32_t half, uint32_t alpha, uint32_t depth)` | existing |
| ring colour | `void u3x_ring_colour(uint32_t *colour, uint32_t mode, uint32_t alpha, uint32_t progress, int32_t ticks)` | existing |

The C wrappers (the C functions keep their names and parameters):

- `k32_ring(b, r)`: `return u3x_hudnet_ring(b, r, g3_icon, ol01_print);`
- `g3_icon(b, el, mod, x, y)`: `u3x_grapple_icon(b, el, mod, x, y, (uint32_t *)g3_d, g6_icon, g6_drawn);`

`g3_d` (16 words, its own named section), `g6_icon`, `g6_drawn` and `ol01_print` are the C part's own diagnostics
and log functions; they stay in C. `seen(b, slot, index, texture)` and `drawn(b, half, alpha)` only write log
lines and counters. `el` = the element, `mod` = the HUD-net module, `x`, `y` = the element's screen position
(float bits).

Also used: `u3x_counter_ring` (Part A, A.2 / A.4.4), `u3h_ring_colour`, `ptr_ok`.

### B.3 Game facts

**Site** (`r` = saved registers: r[3] = the game's r12 = the element, r[14] = rbp of the element draw). Frame
values: rbp - 0x110 pointer to the module; rbp - 0x160 and rbp - 0x15c the element's screen position x, y.

**Element:** +0x08 scale, +0x10 depth, +0x3c alpha, +0x54 progress (float bits); +0x50 u32 mode (1 knocked down,
2 grabbed, 3 no ring, 5 needs help).

**Module sprite descriptors** (G.4, 12 bytes each): +0x10b0 the knocked-down icon, +0x10bc the grapple icon,
+0x10ec the element ring.

**Game data:** image 0x17b4a90 + 0x94: s32 clock in ticks of 1/300 s. Net game manager at image 0x17cb060: ten
player trackers of 0x9c0 bytes from manager + 0x390; tracker +0x10 pointer to a handle (a pointer to the player
object pointer), +0x18 u32 id, +0xe1 u8 "in use"; a player object has its u32 id at +0x7c and a flag byte at
+0x39c (bit 2 = grabbed).

**The three ring colours are read from the game, not held.** The PS4 element draw loads them from its own
constants (its mode test at image 0xc30e33 .. 0xc30e6e): three u32 float bits r, g, b at image **0x1422250** for
mode 1 (red), at image **0x1422240** for mode 2 (light blue), at image **0x1422230** for every other mode (blue).
(Each is a 16-byte vector; its fourth word is not a colour of this ring and is not used.) They are in the game's
read-only data and equal the old helper's nine literals bit for bit (checked in the PS4 image).

**Constants:** 0.25 (0x3e800000) blink threshold; 0x3b5a740e (1 / 300 as a single, 0.00333333...) ticks to
seconds; 3.0 (0x40400000) blinks per second; 0.5 (0x3f000000); 60.0 (0x42700000) icon size at scale 1; ring radii
40.0 (0x42200000) and 50.0 (0x42480000).

**Diagnostics.** eb[250] site calls, eb[251] rings, eb[252] other modes, eb[253] refused, eb[254] last mode.
`diag` of the grabbed icon (the C part's `g3_d`): [0] calls, [1] icons drawn, [2] no texture, [3] last index, [4]
last string id of the descriptor, [5..14] per tracker 0x100 + the player's flag byte (0 = no live player), [15]
calls during which a tracked player had bit 2 set.
The mode icon keeps a block of its own, a zero-initialised 33-word variable of the C++ part named `u3x_icons`
(`U3X_BSS`): words 0..7 calls per mode, 8..15 drawn per mode, 16..23 no texture per mode, 24..31 "line written"
per mode, 32 lines written in all.

### B.4 Behaviour

**B.4.1 `u3x_ring_colour(colour, mode, alpha, progress, ticks)`.** `base` is not a parameter of this function;
see open question 1 for how it gets the three colours (B.3). colour[0..2] = the colour of the mode (mode 1, mode
2, any other); colour[3] = `alpha`. If progress < 0.25 (not for NaN): cycles = (I2F(ticks) * 0x3b5a740e) * 3.0
(two multiplications, in this order); part = cycles - I2F(F2I(cycles)); if part < 0.5: colour[0] = colour[1] =
colour[2] = 1.0 (white). So the ring blinks white three times a second in the first half of each third of a
second. With negative ticks the part is zero or negative, so the ring is white all the time; with |cycles| beyond
the integer range F2I gives 0x80000000 and the formula is applied as written.

**B.4.2 `u3x_icon_quad(v, x, y, half, alpha, depth)`.** Writes all 68 words of the vertex block `v` (G.4): 0
everywhere, then with left = x - half, right = x + half, top = y - half, bottom = y + half:

| Vertex | x | y | u | v |
|---|---|---|---|---|
| 0 | left | top | 0.0 | 0.0 |
| 1 | right | top | 1.0 | 0.0 |
| 2 | right | bottom | 1.0 | 1.0 |
| 3 | left | bottom | 0.0 | 1.0 |

each with colour (1.0, 1.0, 1.0, alpha) and four 1.0; word 0x40 = depth. No call.

**B.4.3 Icon size and alpha** (both icons). half = ((u32 at el + 8) * 0.5) * 60.0. Screen = 0, or, when the
pointer at image 0x1806480 passes `ptr_ok`, the u32 at that HUD + 0x1d0, taken as 0 when it is above 1.
alpha = (u32 at el + 0x3c) * (u32 at image 0x16eafe0 + 4 * screen). Depth = the u32 at el + 0x10. The quad is the
block of B.4.2 at (x, y) and is drawn with one call 0xe42310(context block of six 0, {texture, 0, 0, 0}, block).

**B.4.4 `u3x_mode_icon(b, el, mod, x, y, mode, log)`.** m = mode AND 7. Descriptor = mod + 0x10b0 when mode is 1,
mod + 0x10bc for every other mode.

1. `u3x_icons` calls[m] += 1.
2. texture = 0x11b89b0(u64 at descriptor, u32 index at descriptor + 8).
3. texture 0: no-texture[m] += 1; a line (below) may be written; return.
4. Size, alpha, quad and draw as B.4.3; drawn[m] += 1; a line may be written.

A line is written only when `log` is not null, line-written[m] is 0 and lines-in-all is below 20; then
line-written[m] = 1, lines-in-all += 1, and `log(b, line)` is called. So each mode gets one line at most, either
kind. Texts (the subject is "knocked-down icon" for mode 1, else "icon of mode " and the mode in decimal):

- no texture: "U3MP down: " subject ": no material (the icon texture is not loaded), not drawn"
- drawn: "U3MP down: " subject ": slot sid " the u32 at the descriptor as "0x" and 8 hex digits " index " the index
  as unsigned decimal ", material " the texture handle as "0x" and 12 hex digits (its low 48 bits) ", drawn " the
  new drawn[m] in decimal.

Hex digits are lower case (`Line` of CONVENTIONS.h).

**B.4.5 `u3x_grapple_icon(b, el, mod, x, y, diag, seen, drawn)`.** Descriptor = mod + 0x10bc.

1. diag[0] += 1; diag[3] = the index (u32 at descriptor + 8); diag[4] = the u32 at descriptor + 0.
2. For tracker i = 0 .. 9 (B.3): value = 0; when the tracker's in-use byte is not 0, its handle pointer is not
   null and passes `ptr_ok`, its id is not 0, the pointer the handle holds is not null and passes `ptr_ok`, and
   the u32 at that player + 0x7c equals the id: value = 0x100 + the player's byte at +0x39c, and when that byte
   has bit 2 a mark is set. diag[5 + i] = value. After the ten: with the mark, diag[15] += 1.
3. texture = 0x11b89b0(u64 at descriptor, index).
4. `seen(b, descriptor, index, texture)` (also when the texture is 0).
5. texture 0: diag[2] += 1, return.
6. Size, alpha, quad and draw as B.4.3; diag[1] += 1; `drawn(b, half, alpha)`.

**B.4.6 `u3x_hudnet_ring(b, r, grapple, log)`.** Always returns 0.

1. eb[250] += 1.
2. el = r[3]; rbp = r[14]. Either fails `ptr_ok`: eb[253] += 1, return.
3. mode = the u32 at el + 0x50; eb[254] = mode. mode 3: return. mode not 1, 2 or 5: eb[252] += 1, return.
4. mod = the pointer at rbp - 0x110; fails `ptr_ok`: eb[253] += 1, return.
5. Function start check (G.4) of 0x11b89b0 and 0xe42310; either fails: eb[253] += 1, return.
6. p = the u32 at el + 0x54. Colour block = `u3x_ring_colour(.., mode, u32 at el + 0x3c, p, s32 at image
   0x17b4a90 + 0x94)`. x = the u32 at rbp - 0x160, y = the u32 at rbp - 0x15c. `u3h_ring_colour(colour block)`.
7. `u3x_counter_ring(b, u32 at el + 8, p, 40.0, 50.0, u32 at el + 0x10, x, y, mod + 0x10ec, colour block)`.
8. eb[251] += 1 (whether or not a ring was drawn).
9. mode 2: `grapple(b, el, mod, x, y)`. mode 1: `u3x_mode_icon(b, el, mod, x, y, 1, log)`.

### B.5 Test scenarios

Fake image with recording stubs for 0x11b89b0 and 0xe42310, recording `u3x_counter_ring`, `u3h_ring_colour`,
`grapple`, `seen`, `drawn` and `log` stand-ins; the three colour vectors placed at their image offsets.

1. `u3x_ring_colour`: modes 0, 1, 2, 3, 5, 0xffffffff; progress 0, 0.2499, 0.25, 1, NaN, negative; ticks 0, 49,
   50, 99, 100, 150, 299, 300, -1, -50, 0x7fffffff, 0x80000000; random.
2. `u3x_icon_quad`: random values; all 68 words compared (a pre-filled block must come out fully rewritten).
3. `u3x_mode_icon`: mode 1 and mode 2 (other descriptor, other subject text); texture 0 first and a draw later
   (no second line); a draw first; `log` null; 21 modes-with-lines cannot happen (8 modes), so the limit of 20 is
   tested by presetting the lines word; HUD pointer null, bad, screen 0, 1, 2; exact line texts, with index
   0xffffffff and a texture handle above 48 bits.
4. `u3x_grapple_icon`: trackers empty, in use with a live player with and without bit 2, id mismatch, in-use byte
   0, bad handle; texture 0 (`seen` called, `drawn` not); the draw.
5. `u3x_hudnet_ring`: each refusal and each early return; modes 1, 2, 5; order of the calls
   (`u3h_ring_colour`, ring, then icon); eb words.

Needs the live game: a co-op match with a knocked-down and a grabbed partner (ring colour, blink under a quarter,
icon inside the ring).

---------------------------------------------------------------------------------------------------------------------

## Part C. Mission-complete message

### C.1 Purpose and callers

PS3 1.19 shows a HUD line when a mission is completed (with the cash or with the unlocked booster and its icon),
pays the cash and marks the mission as done. The PS4 game kept the mission tracker but not this step. The helper
runs it from a stub in the tracker (stub kind 50, image 0x77421c: C `ms_34b91c(base, s, tracker, mission entry)`
-> `u3x_mission_done`). The message queue function `u3x_ms_hud` is also called by other groups
(`u3x_content_notice`, G31; `invite_notice`, G20) and `u3x_ms_text` by G31. Game thread.

The C functions `ms_text`, `ms_len`, `ms_rep`, `ms_itoa`, `ms_icon_find`, `ms_icon`, `ms_hud`, `ms_34b91c`
(kr_port.inc) are already one-line wrappers and stay.

### C.2 Interface

| Function | Signature (all `extern "C"`) |
|---|---|
| text | `const char *u3x_ms_text(uintptr_t base, uint32_t id)` |
| length | `int u3x_ms_len(const char *a)` |
| number | `void u3x_ms_itoa(char *b, int32_t v)` |
| replace | `void u3x_ms_replace(char *buf, int n, const char *tag, const char *rep)` |
| icon search | `int32_t u3x_ms_icon_find(const uint8_t *t, uint32_t id)` |
| icon | `void u3x_ms_icon(uintptr_t base, uint32_t id, uint32_t *ic)` |
| queue a line | `int u3x_ms_hud(uintptr_t base, int user, const char *txt, uint32_t type, int32_t level, const uint32_t *ic)` |
| mission done | `void u3x_mission_done(uintptr_t base, const uint8_t *trk, const int32_t *def)` |

`ic` = three u32: icon id, table (1 = second image table, 0 = first), index in that table (0xffffffff = none).
`trk` = the mission tracker, `def` = the mission entry.

### C.3 Game facts

**Game functions** (plain calls):

| Image offset | Arguments | Result | What |
|---|---|---|---|
| 0xf32ae0 | (text system = address of image 0x17026b8, u32 text id) | pointer or null | the localized text |
| 0xf32080 | (char *buffer, s32 size, s32 value) | none | writes the number as text (G22 3) |
| 0xf32bd0 | (text system, char *buffer, s32 size, u32 text id, text argument 1, text argument 2, text argument 3; the seventh argument is on the stack) | none | writes the localized text with its placeholders replaced by the arguments (G20 3) |
| 0x660fb0 | (null, s32 user, s32 amount) | none | gives the user cash (G31 3) |
| 0x687d30 | (address of image 0x17c3b40, u32 booster id) | pointer or null | the booster definition: +8 u32 text id of its name, +0x28 u32 icon id |

**Game data:**

| Image offset | What |
|---|---|
| 0x17026b8 | the text system; the pointer at +0x10 of it is the game's fallback text |
| 0x17bd080, 0x17bfa10 | the menu object of user 0 and of user 1. **HUD message queue** in a menu: four slots; text of slot i at menu + 0x26a8 + 0x80 * i (0x80 bytes, first byte 0 = free); parameters of slot i at menu + 0x28a8 + 0x30 * i; u32 at menu + 0x26a4 = start time of the first slot (float bits, seconds) |
| 0x17b4a90 + 0x114 | s32 game clock in ticks of 1/300 s |
| 0x1b24bc8 | pointer to the UI image directory (may be null): +0x20 pointer to the first image table, +0x28 pointer to the second. A table: u16 entry count at +2, pointer to the entries at +8; an entry is 0x28 bytes with its u32 id at +8 (G06, G33) |
| 0x17dd610 | pointer to the profile object array (0x7b00 bytes per user; G26 3.5). Fields of a profile object used here, as G26 names them: 0xecc .. 0xf4b completed-mission bits (mission `id`: bit `id` AND 7 of the byte at 0xecc + `id` / 8), 0xf4c s32 progress of the active mission, 0x1864 u32 id of the active mission, 0x1868 u32 its start time |
| 0x17c3b40 + 0x2c | u32, non-zero when the booster table is loaded |

**Slot parameters** (0x30 bytes): +0 u32 message type; +8 u64; +0x10 s32 level; +0x14, +0x18, +0x1c the three
icon words; +0x20 u32; +0x24 u32; +0x28 u32 float bits.

**Tracker:** s32 at +0x1004 = the user (0 or 1). **Mission entry** (G26 3.4), as s32 / u32 words: +0 id, +4 text
id of the name, +0x1c a level number, +0x24 cash reward, +0x34 booster id.

**Ids:** text 0xc423e4f5 = the "mission complete" line with two placeholders (name, cash); text 0x065ba916 = the
reward line, a text containing the tags "[A]", "[B]" and "[TEXT]"; message type 0x2e = reward message. The two
text ids are ids of game texts fetched at run time, kept as ids (as G20, G26 and G31 keep theirs); see open
question 2. 300.0 (0x43960000); 1.0 (0x3f800000).

**Diagnostics** ms (G.3): [0] calls, [1] cash lines queued, [2] reward lines queued, [3] missions marked done,
[4] last mission id, [5] cash paid in all, [6] reward mission already done, [7] line not queued (too long or no
free slot).

### C.4 Behaviour

**C.4.1 `u3x_ms_text(base, id)`.** One call 0xf32ae0(base + 0x17026b8, id); its result, or, when that is null, the
pointer stored at image 0x17026b8 + 0x10.

**C.4.2 `u3x_ms_len(a)`.** The number of bytes before the first 0 byte.

**C.4.3 `u3x_ms_itoa(b, v)`.** Writes `v` in decimal at `b`: a '-' first when negative, no leading zeros, "0" for
0, then a 0 byte (-2147483648 is written in full; at most 12 bytes).

**C.4.4 `u3x_ms_replace(buf, n, tag, rep)`.** Looks for the first place in the text `buf` where `tag` stands
(positions from 0 upward; an empty tag matches at 0). None: nothing changes. Found at position i: the text
becomes (the part before i) + `rep` + (the part that followed the tag), written from position i on, where a
character is written only while its position is below n - 1, and a 0 byte ends the text right after the last
character written. Two limits to keep: only the first 255 characters of the part that followed the tag are
kept; and when i itself is n - 1 or more, nothing is added and the 0 byte goes to position i. The lengths of
`buf`, `tag` and `rep` are taken before anything is written; `rep` never overlaps `buf` in the callers.

**C.4.5 `u3x_ms_icon_find(t, id)`.** `t` = an image table or null. -1 when `t` is null, its entry count is 0 or
its entries pointer is null; else the index of the first entry (0 .. count - 1) whose id equals `id`, or -1.
The pointers are tested against null only.

**C.4.6 `u3x_ms_icon(base, id, ic)`.** ic = {id, 1, 0xffffffff}. Directory = the pointer at image 0x1b24bc8; null:
done. Search (C.4.5) the second table (directory + 0x28): found: ic[2] = the index. Not found: ic[1] = 0 and
ic[2] = the result of the search in the first table (directory + 0x20), which may be -1 (0xffffffff).

**C.4.7 `u3x_ms_hud(base, user, txt, type, level, ic)`.** Menu = image 0x17bfa10 when `user` is not 0, else image
0x17bd080.

1. The length of `txt` is 0x80 or more: ms[7] += 1, return 0.
2. The first slot i (0, 1, 2, 3) whose text starts with a 0 byte is taken. None: ms[7] += 1, return 0.
3. The text with its 0 byte is copied into the slot (the bytes after it are left as they are). Parameters:
   +0 = `type`; the u64 at +8 = 0; +0x10 = `level`; +0x14, +0x18, +0x1c = ic[0], ic[1], ic[2]; +0x20 = 0;
   +0x24 = 0xffffffff; +0x28 = 1.0. The bytes at +4 .. +7 and +0x2c .. +0x2f are not written.
4. When the slot is slot 0: the u32 at menu + 0x26a4 = I2F(game clock) / 300.0.
5. Return 1.

**C.4.8 `u3x_mission_done(base, trk, def)`.** id = the mission id (def + 0, as u32).

1. ms[0] += 1; ms[4] = id.
2. user = the s32 at trk + 0x1004; not 0 or 1: return.
3. Profile array pointer (image 0x17dd610) null: return. P = that pointer + 0x7b00 * user. "Done bit" = the
   completed-mission bit of `id` in P; it exists only for id < 0x400.
4. When the mission was not done before (id >= 0x400, or its done bit is 0) and the active mission (P + 0x1864)
   is `id`: the u32 at P + 0x1864, at P + 0x1868 and at P + 0xf4c are set to 0, in this order.
5. **Cash mission (id < 0x100):**
   1. 0xf32080(a 0x20-byte buffer, 0x20, cash reward).
   2. name = `u3x_ms_text(base, name text id)`.
   3. 0xf32bd0(text system, a 0x80-byte line buffer, 0x80, 0xc423e4f5, name, the cash buffer, null).
   4. `u3x_ms_hud(base, user, line, 0, 0, {0, 0, 0xffffffff})`; result not 0: ms[1] += 1.
   5. 0x660fb0(null, user, cash reward); ms[5] += cash reward (as u32).
   6. Continue at step 7.
   There is no "already done" test here: a cash mission that reaches this function again shows its line and pays
   again.
6. **Reward mission (id >= 0x100):** B = null when the u32 at image 0x17c3b40 + 0x2c is 0; else
   0x687d30(base + 0x17c3b40, booster id of the entry).
   - B not null:
     1. id < 0x400 and its done bit is set: ms[6] += 1, return (nothing more, also no step 7).
     2. The line buffer (0x80 bytes) = the first 0x7f characters at most of `u3x_ms_text(base, 0x065ba916)`.
     3. The number text = `u3x_ms_itoa` of (level number of the entry + 2).
     4. `u3x_ms_replace(line, 0x80, "[A]", number text)`; `u3x_ms_replace(line, 0x80, "[B]", "0")`;
        `u3x_ms_replace(line, 0x80, "[TEXT]", u3x_ms_text(base, u32 at B + 8))`, in this order (the text call of
        the third comes after the second replacement).
     5. `u3x_ms_icon(base, u32 at B + 0x28, icon)`.
     6. `u3x_ms_hud(base, user, line, 0x2e, level number + 1, icon)`; result not 0: ms[2] += 1.
   - Then, with or without B: id >= 0x400: return.
7. The done bit of `id` is set in P; ms[3] += 1.

The buffers of steps 5 and 6 are not cleared before use; what the game functions do with them is the game's.

### C.5 Test scenarios

Fake image with recording stubs for the five game functions (0xf32ae0 answering chosen texts or null, 0xf32bd0
writing a chosen line), fake menus, profile array, image directory and ms words. Compare the calls in order with
their arguments, the slot bytes, the profile bytes and ms.

1. `u3x_ms_text`: an answer; null (fallback pointer). `u3x_ms_len`, `u3x_ms_itoa`: 0, 7, -7, 2147483647,
   -2147483648.
2. `u3x_ms_replace`: tag at the start, in the middle, at the end, twice (only the first), absent, empty tag;
   replacement shorter, longer, empty; result cut at n - 1; a buffer longer than n with the tag beyond n - 1; a
   rest longer than 255 characters; n = 1.
3. `u3x_ms_icon_find` / `u3x_ms_icon`: null directory; id in the second table; only in the first; in neither
   (ic = {id, 0, 0xffffffff}); null table, count 0, null entries.
4. `u3x_ms_hud`: user 0 and 1; text of 0, 1, 0x7f, 0x80 characters; slots free 0, 1, 3, none; slot 0 writes the
   start time (clock 0, 300, 12345, negative), other slots do not; the untouched parameter bytes stay.
5. `u3x_mission_done`: user -1, 2; null profile array; cash mission new, cash mission already done (pays again),
   with the queue full (cash still paid, ms[1] not counted); active mission equal / different, with the done bit
   set (not cleared); reward mission with the booster table not loaded, with a null definition, new, already
   done (ms[6]), id 0x3ff and 0x400 and above (no bit written); the line "[A] / [B] [TEXT]" forms.

Needs the live game: that the line appears on the HUD with the icon and the right texts (the two text ids).

---------------------------------------------------------------------------------------------------------------------

## 6. Things that look like bugs or dead code (behaviour to keep)

1. A.4.1: -0 progress stays -0 and a positive NaN is counted as progress 1 (30 quads at scale 1), while the sweep
   uses the raw p. The marker site filters these; the counter site and Part B do not.
2. A.4.2: the alpha of every ring uses screen 0's alpha word, also on screen 1; the icons of Part B use the
   screen's own word.
3. A.4.4: a quad count of 0 or less counts nothing; "above 512" and "no texture" share eb[212].
4. A.4.5: a progress of -0 draws no ring; +infinity draws none either; t9[4] is only written past those tests.
5. B.4.1: with negative ticks the ring is white all the time below a quarter. The clock is multiplied by the
   single nearest to 1 / 300 (it is not divided by 300; C.4.7 divides).
6. B.4.4: one line per mode whichever kind comes first: after a "no material" line a later successful draw of that
   mode is not logged. The old source holds an unused text ", refused " (dead; not delivered).
7. B.4.4: any mode other than 1 uses the grapple descriptor; in v7.212 only mode 1 arrives.
8. B.4.6: eb[251] counts a ring even when `u3x_counter_ring` drew none; modes 1 and 2 draw their icon then too.
9. B.4.6 draws no ring for modes other than 1, 2, 5 although the game draws its sprites for them (counted in
   eb[252]).
10. C.4.4: the 255-character limit of the rest and the write at position i when i >= n - 1.
11. C.4.7: a line that is not queued is lost (only ms[7] tells).
12. C.4.8: a cash mission has no "already done" test; the cash is paid even when the line was not queued; a
    reward mission whose booster definition is missing is still marked done, without a line; step 4 runs before
    the user sees anything and also when the function then returns at step 6.1 (it cannot have cleared anything
    in that case, since the bit was set).
13. C.4.8: the number put in "[A]" is level + 2 while the level handed to the queue is level + 1; "[B]" is always
    "0".

## 7. Open questions

1. **`u3x_ring_colour` has no `base` parameter**, and the colours are now read from the image (B.3). Proposed: the
   signature becomes `void u3x_ring_colour(uintptr_t base, uint32_t *colour, uint32_t mode, uint32_t alpha,
   uint32_t progress, int32_t ticks)`; its only caller is `k32_ring`, whose body moves into `u3x_hudnet_ring`, so
   no C caller remains and only the C prototype (u3_lan_profiles.c line 349) changes or goes. The owner /
   integrator confirms.
2. The text ids 0xc423e4f5 and 0x065ba916 are PS3 1.19 text ids; neither pattern occurs in the PS4 image. They are
   two single ids, kept by value. Whether they are string ids of text names under G23's rule (3.9) is not known
   (no name is recorded), and it was not checked that the PS4 text files hold both texts (without them
   `u3x_ms_text` gives the game's fallback text).
3. The level number of the mission entry at +0x1c: the old notes call it "level"; G26 3.4 lists a required level
   at +0x20 and does not name +0x1c. The behaviour does not depend on the name.
4. `call_float1` must be visible to Part A (G.3). If the integrator prefers, it becomes a shared tool in
   CONVENTIONS.h.

## 8. Strays and notes for other groups

Covered here although in no list of the inventory's group G47:

- `u3x_ring_colour` (C++, held the nine colour literals: now read from the game image).
- `u3x_marker_ring`, `u3x_counter_ring` and the private `ring_segments` (C++; the inventory names the first two
  only as callers).
- `u3x_ms_icon_find` and its private `icon_in`, private `text_length`; `u3x_ms_text`, `u3x_ms_len`, `u3x_ms_itoa`
  (short, described so that the whole block is written from this document).
- The mode-1 icon state `u3x_icons` and its texts.

Stay as they are (the C part's own code, not described): `h08_ringcol` behind `u3h_ring_colour`, `g6_icon`,
`g6_drawn`, `g3_d`, `ol01_print`, `t009d`; the shim `call_float1`.

What must stay: every exported name and signature of A.2, B.2 (but see open question 1) and C.2, since the C
wrappers and G20 / G31 call them; the layout of the diagnostic words; the log texts.

## 9. Clarifications (tester, 2026-11-14)

Facts confirmed by the differential test (`reimpl2\G47\test_g47.cpp`, old v7.212 against the new `part_a.cpp`,
`part_b.cpp`, `part_c.cpp`). Nothing in sections G, A, B, C was found wrong; these items settle the implementers'
open points and make precise what the text leaves open.

**Floats.** Checked in the shipped v7.212 code (the generated assembly of the C++ part and the helper blob for
`g3_icon`): this group's old code has no x87 instruction and no hardware fused multiply-add. Every step is one
single-precision instruction; every product-sum PS is the three-step form of G.1 (both factors and the addend
widened to double, one double multiplication, one double addition, one rounding to single): once in the quad
count, twice for the two angles and sixteen times for the vertex words of a quad. Old and new are equal bit for
bit for every input tried, including NaN, infinities, denormals and -0.

**Part A.**

1. **A site other than 0 or 1** (`u3x_marker_ring_site`): every site that is not 0 behaves as site 1 (list = r[1],
   entry offset = r[0], depth 1.0), and the word counted in step 1 is t9[site] for that number (2, 3, 15, 23, -1,
   -8 tried). Only sites 0 and 1 arrive in v7.212.
2. **The range of the quad count.** Through the two exported ring functions N is never 1 to 9 and never 0: the
   count of A.4.1 step 3 is between 10 and 30 and the factor m is at least 1. "n <= 0" therefore happens only
   when m * count is beyond the integer range (a scale of about 7.2e7 or more, or +infinity), where F2I gives
   0x80000000. Scenario A.5.2 "n = 1, 2" cannot be reached through the exported functions.
3. **Order of the calls** of the two site functions: `u3h_ring_colour` comes before the texture lookup; in the
   counter site the progress call comes first. The frame values (scale, x, y), the position of the draw object and
   the descriptor are read after `u3h_ring_colour` returned; the colour words, col[3] times the screen word (once,
   before the first quad) and col[0..2] (again for every vertex) are read as A.4.2 says.

**Part B.**

4. **`u3x_ring_colour`** (open question 1): the new function has the signature `void u3x_ring_colour(uintptr_t
   base, uint32_t *colour, uint32_t mode, uint32_t alpha, uint32_t progress, int32_t ticks)`. The nine words at
   image 0x1422230 / 0x1422240 / 0x1422250 of the PS4 image equal the old code's nine literals bit for bit (the
   fourth word of each vector is 0x3f30b0b1 and is not used).
5. **A mode above 7** (`u3x_mode_icon`): the counters use mode AND 7, the line holds the whole mode number in
   unsigned decimal ("icon of mode 9", "icon of mode 4294967295"), and every mode other than exactly 1 uses the
   descriptor at module + 0x10bc (mode 9 counts in slot 1 but is not "knocked-down").
6. **The descriptor index of the mode icon is read once**, at the start (before the texture lookup); that value
   goes to 0x11b89b0 and into the "drawn" line. The string id in the line (the u32 at the descriptor) is read
   when the line is written. (A difference shows only when the texture lookup or the quad draw changes the
   descriptor.)
7. **The tracker tests of the grapple icon**: the handle pointer is followed only for a tracker whose in-use
   byte is not 0, whose id is not 0 and whose handle pointer passes the pointer test; the player's id is then read
   when the player pointer passes the test. With an in-use byte of 0 or an id of 0 neither pointer is followed
   (checked with pointers that pass the test but cannot be read). Any non-zero in-use byte counts.
8. **Null function pointers.** `log` null: no line, the line-written word stays 0, the lines-in-all word is not
   counted. `grapple`, `seen` and `drawn` are never null in the helper (the old C functions called the three C
   functions directly); the new code does not test them: a null `grapple` is called in mode 2 (after the ring
   and eb[251]), a null `seen` on every call, a null `drawn` after a draw.
9. **The 33-word state of the mode icon** is a private variable of the part (zero at start), not a field of the
   helper's state block; layout as B.3.

**Part C.**

10. **`u3x_ms_replace`.** The tag is looked for at the positions 0 to (text length - tag length); a tag longer
    than the text is never found; an empty tag matches at position 0, also in an empty text (the result is the
    replacement followed by the old text, cut at n - 1). Nothing is read behind the 0 byte of `buf`, `tag` or
    `rep`, and nothing is written behind position max(n - 1, place of the tag). n of 1, 0 or below: nothing is
    added and the 0 byte goes to the tag's place.
11. **`u3x_ms_hud`.** A text of 0x80 characters or more is refused whatever the slots hold; with a full queue as
    well ms[7] is counted once. An empty text is queued (result 1, parameters written, start time for slot 0);
    the slot's first byte stays 0, so the slot is still free and the next line takes it again. Every non-zero
    `user` means the menu at image 0x17bfa10.
12. **`u3x_mission_done`, mission ids of 0x400 and above** (they are reward missions): no byte of the completed
    bits is read or written for them (checked with the id 0xffffffff, whose byte would be outside the profile);
    the active mission is cleared when it has this id; the line is queued when there is a booster definition;
    ms[3] is not counted.
13. **The seventh argument of 0xf32bd0** is a null pointer: the stack word is 0 in all 64 bits.
14. **Missing texts.** A null answer of 0xf32ae0 gives the fallback pointer in every use. When the fallback
    pointer is null too: the cash line hands the null name to 0xf32bd0 (no fault here); the reward line reads the
    null text and faults (the pattern at step 6.2, the booster's name in the third replacement), in the old code
    as in the new.
15. **Values read more than once** (visible only when a game function changes them during a call):
    - the cash reward of the entry is read three times: for 0xf32080, again for 0x660fb0 (after the line was
      queued) and again for ms[5] (after 0x660fb0 returned);
    - the level number of the entry is read twice: for the number in the line (after the reward line text was
      fetched) and again for the level handed to the queue (after the booster's name text was fetched and the
      icon was looked up);
    - the done bit is read at step 3 (for step 4) and read again at step 6.1, after 0x687d30 returned.
    The mission id, the name text id, the booster id, the user and the profile array pointer are read once, at
    the start.
16. **Order at the start of `u3x_mission_done`**: the mission id (entry) and the user (tracker) are read before
    ms[0] and ms[4] are written. (With a tracker that cannot be read the old code faults before counting ms[0].)
17. **Level + 2 and level + 1** are computed in 32 bits and wrap: a level number of 0x7ffffffe gives the number
    text "-2147483648" and the queue level 0x7fffffff.
