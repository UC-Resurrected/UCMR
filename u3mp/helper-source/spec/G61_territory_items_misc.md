# G61. Territory re-make, item layout rewrite, slider track, submesh diagnostic, net anim groups

Behaviour description for a reimplementation (reimpl2 pipeline, describer's document). It says what the functions
do, not how the old ones were written. Offsets are hexadecimal when prefixed with 0x. "Image offset X" means the
address `base + X`, where `base` is the game image base. All multi-byte values in memory are little-endian; "u8 /
u16 / u32 / s32 / u64" are integers of that width, "pointer" is 8 bytes. A "float pattern" is the 32-bit pattern of
a single-precision float carried in a u32. Game functions use the System V AMD64 convention.

Related documents: `G51_anim_lookups.md` (art group search 4.6, the net anim package names 3.5 and
`u3x_net_art_group` 4.10: Part D here builds on them), `G22_rank_xp.md` (rank badge: `u3x_rank_tag`,
`u3x_rank_back`, native 20), `G23_boosters.md` 3.9 (the string id rule), `G37_find_game.md` (`u3x_m04_map`), G55
(tab box quads: `u3x_box_item`, the gradient texture; no spec yet when this was written).

**Float.** Nothing in this group ran on the x87 unit and nothing does float arithmetic. Float patterns are copied
as u32. Three places look at a float's value, and the old build did all three with single-precision SSE
instructions on the pattern: the two truncations of B.4.4 step 8 and the truncations and the two "0 < x" tests of
B.4.3. They are `f_to_int` and `f_less` of CONVENTIONS.h, so every result must match the old code bit for bit. No
float is passed or returned by value.

The group is four unrelated pieces. They share no code and no state and are written by different implementers:

| Part | Content | Old lines (about) | Float | Needs |
|---|---|---|---|---|
| A | Territory particle re-make (King of the Hill / Chain Reaction zones) | 105 | none | game function 0x7b2040 |
| B | Menu item rewrite at the item layout, rank badge item, image item, tab icon item, slider track | 115 | `f_to_int`, `f_less` | game functions 0x63d6f0, 0x11b89b0, native 20 |
| C | Submesh draw diagnostic | 86 | none | none (reads, counters, one optional write) |
| D | Net anim groups: art group by name id / by name, the Player's eight groups | 45 | none | the game's name-id function |

Part B may be given to two implementers: B1 = `u3x_item_layout`, `u3x_rank_item`, `u3x_image_item`,
`u3x_tab_icon_item`; B2 = `u3x_slider_track` (it uses nothing of B1).

Shared notation for all parts:

- X = `u3h_ext()` (CONVENTIONS.h), the helper's state. Word arrays of u32: `ea[]` at X + 0x3c50 (`ext_ea()`),
  `eb[]` at X + 0x4050 (`ext_eb()`), `ec[]` at X + 0x4450 (not in CONVENTIONS.h; same pattern), `fsw[]` at
  X + 0x3a70, `pna[]` at X + 0x3c30.
- "`ptr_ok`" is CONVENTIONS.h's test (at least 0x10000, below 2^47, multiple of 8). "**4-aligned pointer test**"
  is the same test with "multiple of 4" instead of "multiple of 8"; Part B needs it in three places and the
  difference is observable.
- A **value stub block** `r` is an array of saved 64-bit values the stub dispatcher hands over: elements 0 to 10 =
  r11, r10, r9, r8, rdi, rsi, rdx, rcx, rax, rbp, the return address, as they were at the entry of the hooked game
  function; elements 11, 12, 13, 14, ... = the 1st, 2nd, 3rd, 4th, ... stack argument of that function.
- "x += 1" on a diagnostics word is a plain read, add, write unless the text says "atomic".

---

# Part A. Territory particle re-make

## A.1 Purpose and callers

A territory (a capture zone of the multiplayer objective modes) shows floor particles in the colour of its owner
(neutral, own team, enemy team). The game makes these particles in its function at image 0x7b2040, which reads the
territory's owner field. On the host the owner changes through an owner event; in the helper's sessions that
event does not always reach the territory, so the particles keep the old colour. The helper therefore watches up to
eight territories and

1. re-makes the particles 0.5 s after an owner change the game itself did not follow with new particles, and
2. finishes a capture 1.5 s after its end time when the owner still is not the capturing team (what the owner
   event would have done locally: old particles killed, owner set, capture time cleared, particles made).

A third, opt-in test aid sets the owner of a still unowned territory once, 10 s after it was first seen.

Callers (they stay):

- `u3x_h08_one`: C one-line wrapper `h08_one`, called by the C function `h08_tick` once per candidate territory,
  every 16th frame while multiplayer is active, on the game's main thread. `h08_tick` passes the local player's
  team (s32 at player object + 0x21c), the game clock (u32 at image 0x17b4a90 + 0x214, 300 ticks per second) and
  a pointer to a u32 "seen" mask it cleared before the scan. After the scan `h08_tick` frees every slot whose
  bit is not in the mask.
- `u3x_h08_handle_ok`: C wrapper `h08_hok` (no C caller left) and `u3x_h08_one`.
- `u3x_h08_kill`: C wrapper `h08_kill` (no C caller left) and the re-make step of `u3x_h08_one`.

## A.2 Interface

| Function | Signature | Kind |
|---|---|---|
| handle valid | `int u3x_h08_handle_ok(const uint8_t *h)` | existing name, new body |
| kill one particle | `void u3x_h08_kill(uintptr_t base, uint8_t *h)` | existing name, new body |
| one territory | `void u3x_h08_one(uintptr_t base, uint8_t *p, int32_t team, uint32_t now, uint32_t *seen)` | existing name, new body |

The file-private "kill the eight particles" step (old name `remake_particles`) is part of `u3x_h08_one` (A.4.3);
how it is organised is free. The C wrappers pass their arguments unchanged.

Called from other places:

| Name | Signature | Effect |
|---|---|---|
| `u3h_ext` | CONVENTIONS.h | state X |
| `u3h_h08_state` | `uint32_t *u3h_h08_state(int which)` | C part: the five slot arrays, eight elements each. which 0 = territory addresses (u64 each; cast the result), 1 = remembered particle generation (u32), 2 = change stamp (u32), 3 = remembered owner (s32), any other value (use 4) = handled capture end time (u32) |
| `u3x_h08_on` | `int u3x_h08_on(void)` | existing one-liner that stays: 1 unless bit 5 of `ec[1]` is set |
| game 0x7b2040 | `void fn(uint8_t *territory)` | the game's "make the owner-coloured particles of this territory"; integer argument only |

## A.3 Game facts

Territory object `p` (only these fields are used):

| Offset | Type | Meaning |
|---|---|---|
| +0x34c | s32 | owner team (−1 = none) |
| +0x350 | s32 | capturing team (−1 = none) |
| +0x374 | u32 | capture end time in clock ticks (0 = none); +0x378 u32 belongs to it and is cleared with it |
| +0x37c | u32 | a seconds value (diagnostic only) |
| +0x3b0 | 4 handles of 16 bytes | particle handles "B" |
| +0x3f0 | 4 handles of 16 bytes | particle handles "A"; the u32 at +0x3f8 (generation word of the first A handle) is the territory's "particle generation" G |
| +0x438 | u32 | diagnostic only (non-zero or zero) |

Particle handle (16 bytes): pointer at +0 to a slot that holds the pointer to the particle object; u32 at +8 =
generation. Particle object: u32 at +0 = its generation; u8 at +4 = flags, bit 0 = "kill me".

Particle manager: at image 0x1a4d910. u64 at manager + 0x10 = a reader/writer lock word: bit 63 set = a writer
holds it, the low 63 bits = number of readers. u8 at manager + 0xfbd = non-zero while the manager is running.

Clock: 300 ticks per second (150 = 0.5 s, 450 = 1.5 s, 3000 = 10 s). Time differences are taken in 32-bit
unsigned arithmetic and then read as signed.

Helper state: the five slot arrays of `u3h_h08_state`; diagnostics D = `ec[216 .. 231]` (D[i] = `ec[216 + i]`);
`ea[147 + k]` = first-seen stamp of slot k; `ea[155]` = bit k set once the test aid acted on slot k; `ea[156]` =
test-aid count; `ea[157]` = last test-aid record. Switches: `ec[1]` bit 5 set = re-make off (`u3x_h08_on`);
`eb[3]` bits 19 and 20 = the test aid (0 = off).

A "stamp" is a clock value with bit 0 forced to 1, so that 0 means "none"; the time a stamp stands for is the
stamp with bit 0 cleared.

## A.4 Behaviour

### A.4.1 `u3x_h08_handle_ok(h)`

1 when the handle's slot pointer is not null, its generation is not 0, the slot holds a non-null object pointer and
the object's generation equals the handle's; else 0. Reads only. No pointer plausibility test.

### A.4.2 `u3x_h08_kill(base, h)`

1. If the handle is not valid (exactly the test of A.4.1): return, nothing changed.
2. Take the manager's lock as a reader: up to 100000 attempts, each one: read the lock word, clear bit 63 of the
   value read, and atomically replace the word by that value + 1 only if the word still equals that value
   (compare-and-swap). An attempt fails while a writer holds the lock or when the word changed in between. If all
   100000 attempts fail: return; the handle is left as it is.
3. If the manager's running byte is non-zero: read the slot's object pointer again; if it is not null and its
   generation still equals the handle's, set bit 0 of the object's flags byte.
4. Release: atomically subtract 1 from the lock word.
5. Clear the handle: the u64 at h + 8 becomes 0, then the u64 at h + 0 becomes 0 (also when step 3 did not set
   the flag).

### A.4.3 `u3x_h08_one(base, p, team, now, seen)`

"Kill the eight particles" = for j = 0, 1, 2, 3 in this order: `u3x_h08_kill` on B handle j (p + 0x3b0 + 16 j),
then on A handle j (p + 0x3f0 + 16 j).

Read once at the start and used with these start values in all later steps: own = owner, cap = capturing team,
end = capture end time, G = particle generation.

1. **Slot.** Look through slots 0 to 7 in order for the one whose address equals `p`; while looking, remember the
   first slot whose address is 0. Found: k = that slot. Not found and an empty slot was seen: k = the first
   empty slot, and it is registered: address = p, remembered owner = own, handled end time = 0, remembered
   generation = G, change stamp = 0, `ea[147 + k]` = stamp of `now`, bit k of `ea[155]` cleared. Not found and no
   empty slot: there is no k.
2. With a k: set bit k in `*seen`.
3. With a k and remembered owner ≠ own (an owner change): D[2] += 1; remembered owner = own; change stamp = stamp
   of `now`; D[13] = (own AND 0xff) + 0x100 when the remembered generation (before any update in this call)
   equals G, else own AND 0xff.
4. With a k and change stamp = 0: remembered generation = G.
5. **Diagnostics, always (also without a k):** D[4] = (own AND 0xff) | (cap AND 0xff) << 8 | (team AND 0xff) << 16
   | 1 << 24; D[5] = end; D[6] = now; D[7] = u32 at p + 0x37c; D[8] = bit 0: first A handle valid (A.4.1), bit 1:
   first B handle valid, bit 2: u32 at p + 0x438 is non-zero; D[9] = the low 32 bits of the address `p`.
6. Return when `u3x_h08_on()` is 0 or there is no k.
7. **Test aid.** T = bits 19..20 of `eb[3]` (0 to 3). When T ≠ 0, own = −1, `team` is 0 or 1, bit k of `ea[155]`
   is clear and (now − time of stamp `ea[147 + k]`) ≥ 3000: new owner = `team` when bit 0 of T is set, else the
   other team (team XOR 1). In this order: set bit k of `ea[155]`; kill the eight particles; owner field = new
   owner; the u64 at p + 0x374 = 0; remembered owner = new owner; call game 0x7b2040(p); remembered generation =
   the particle generation read again now; change stamp = 0; `ea[156]` += 1; `ea[157]` = (new owner AND 0xff) |
   k << 8 | (team AND 0xff) << 16. **Return** (steps 8 and 9 do not run in this call).
8. **Re-make after an owner change.** When the change stamp is not 0 and (now − time of the stamp) ≥ 150: if G
   equals the remembered generation and own ≠ −1: kill the eight particles, call game 0x7b2040(p), D[12] += 1.
   In both cases then: change stamp = 0; remembered generation = the particle generation read again now.
9. **Finish a capture.** When end ≠ 0, cap ≠ −1, own ≠ cap, (now − end) ≥ 450 and the handled end time of slot k
   ≠ end: handled end time = end; kill the eight particles; owner field = cap; the u64 at p + 0x374 = 0;
   remembered owner = cap; call game 0x7b2040(p); remembered generation = the particle generation read again
   now; change stamp = 0; D[3] += 1; D[10] = (cap AND 0xff) | (own AND 0xff) << 8.

Steps 8 and 9 can both act in one call (two calls of 0x7b2040); step 9 still uses own / end as read at the start.

## A.5 Test scenarios

All of Part A runs against a fake image: a block of memory as `base` with the manager at 0x1a4d910, fake
territories and particle objects, a recording stub at 0x7b2040 (which may change the first A handle's generation to
imitate new particles), a fake X, and a stand-in `u3h_h08_state`. Compare memory, `*seen`, and the sequence of
game calls, old against new. No live game needed.

1. Handle valid: null slot pointer; generation 0; slot holds null; generations differ; valid.
2. Kill: invalid handle (untouched); lock free (reader count back to its old value, flag set, handle cleared);
   writer bit set for good (handle untouched, flag not set; the test may shorten nothing: 100000 attempts);
   running byte 0 (flag not set, handle cleared); lock with readers already present.
3. One territory: first sight with free slots (registration values, seen bit); eight other territories registered
   (no k: only D[4..9] written); known territory, nothing changed.
4. Owner change seen: D[2], D[13] with generation equal / different; then calls at now + 149 (nothing) and
   now + 150 with generation unchanged and own ≠ −1 (eight kills, one game call, D[12]); with generation changed
   (no kill, stamp cleared); with own = −1 (no kill).
5. Capture: end 0; cap −1; own = cap; 449 and 450 ticks past the end; a second call with the same end (nothing,
   because of the handled end time); a new end time (acts again). Check the u64 clear at +0x374 clears +0x378.
6. Steps 8 and 9 both due in one call.
7. Switch off (`ec[1]` bit 5): steps 1 to 5 still run, nothing else.
8. Test aid: T = 1, 2, 3; team 0, 1, 2; own ≠ −1; 2999 and 3000 ticks after first sight; second call (bit in
   `ea[155]` set: steps 8 / 9 run instead).
9. Clock wrap: `now` slightly above 0 with a stamp just below 2^32.

## A.6 Behaviour to keep (looks odd, must stay)

1. D[4] to D[9] are written before the switch is tested and also for a territory that got no slot.
2. The kill clears the handle even when it did not set the kill flag (manager not running, or the object changed
   under the lock); it leaves the handle when the lock could not be taken.
3. Neither the handle test nor the kill checks that the pointers are plausible.
4. Clearing the capture time is one 64-bit store (two fields).
5. Step 9 uses the owner read at the start even when step 8 ran before it.
6. The test aid's delay is 3000 ticks from the first-seen stamp; the stamp loses its lowest bit (one tick).

---

# Part B. Item layout rewrite, rank badge item, image item, tab icon item, slider track

## B.1 Purpose and callers

A menu or HUD "layout item" is a small record (44 bytes, 11 u32 words) that says what to draw in a cell: a text,
a texture, a UI icon, a box, a bar. The game lays an item out with its function at image 0x640ac0 and draws it
with 0x63d6f0. PS3 1.19 knows item types the PS4 game lays out but never draws. The helper rewrites such an item
in place, at the entry of the layout function, into a type the PS4 game draws:

- "new" marker, "equipped and new", "prestige badge while new" → a plain UI icon;
- map picture → a texture item (or the loading spinner while the picture is not loaded);
- rank badge (the HUD's rank-up popup) → a texture item with the rank art, tagged for G22's badge drawing;
- tab box → the PS4 box item (flat colour), or optionally a UI icon of the gradient texture.

Separately, at the entry of the item draw, the slider bar (type 4) gets its black track drawn first, because the
PC port does not show the track the game draws.

Callers (they stay):

- `u3x_item_layout`: C wrapper `h09_layout`, called by the dispatcher `k_kind` for stub kind 245 = a value stub
  at the entry of game function 0x640ac0 (an assembly pre-filter lets only item types 0x24 to 0x31 through). Main
  thread. The result is returned to the stub: 0 = the game's function runs (always).
- `u3x_rank_item`: only `u3x_item_layout`. (The C function `n28_rank_item` has no other caller and goes away or
  becomes a wrapper.)
- `u3x_image_item`: `u3x_rank_item`; the declaration in the C part stays.
- `u3x_tab_icon_item`: only `u3x_item_layout`. (C `h13_native`, a stray: see section "Strays".)
- `u3x_slider_track`: C wrapper `w10_track`, called by `k_kind` for stub kind 246 = a value stub at the entry of
  game function 0x63d6f0, after three other fixers and before G55's `u3x_box_item`; only with a non-null block.

## B.2 Interface

| Function | Signature | Kind | C wrapper passes |
|---|---|---|---|
| item rewrite | `int u3x_item_layout(uintptr_t base, void *s, uint64_t *r)` | new entry for C `h09_layout` | `(base, s, r)` |
| rank badge item | `int u3x_rank_item(uintptr_t base, void *s, uint32_t *item)` | new entry for C `n28_rank_item` | `(base, s, o)` |
| image item | `void u3x_image_item(uint32_t *item, void *texture)` | existing name, new body | – |
| tab icon item | `int u3x_tab_icon_item(uintptr_t base, uint32_t *item, const uint32_t *box)` | new entry for C `h13_native` | `(base, o, box)` |
| slider track | `void u3x_slider_track(uintptr_t base, const uint64_t *r)` | existing name, new body | `(base, r)` |

`s` is the helper's profile state (opaque here; only "native 20" is read from it, below).

Called from other places:

| Name | Signature | Effect |
|---|---|---|
| `u3h_ext` | CONVENTIONS.h | state X |
| native 20 | pointer to function at `s` + 928 (G22 3): `void *fn(uintptr_t dictionary, const char *name)` | the game's texture-by-name lookup; first argument = address of image offset 0x1b31128; returns a texture object or null; a miss queues the load. The pointer itself may be null |
| `u3x_rank_icon_name` | `void u3x_rank_icon_name(char *out, uint32_t value, uint32_t prestige_limit)` | existing (G22): writes the rank art name of a rank word, NUL-terminated, at most 24 characters with the NUL |
| `u3x_rank_back` | `void *u3x_rank_back(uintptr_t base, void *s, uint32_t level)` | G22 4.13: asks for the prestige back texture of a level (null for a level outside 1..5); has its own counters |
| `u3x_rank_tag` | `void u3x_rank_tag(uintptr_t base, void *s, int32_t *ev, uint32_t v)` | G22 4.13: when the rank word `v` needs a decorated badge, writes a marker into word 6 and `v` into word 7 of `ev` (at least 8 words) and looks the decoration textures up; otherwise leaves `ev` untouched |
| `u3x_m04_map` | `uint8_t *u3x_m04_map(void *s, uint32_t id)` | G37 4.1: the map entry with that id, or null. Pointer at entry + 0x10 = the name of the map's picture texture (may be null) |
| `u3h_icon_index` | `int32_t u3h_icon_index(uintptr_t base, uint32_t id)` | C part: index of UI icon `id` in the game's UI texture table, or −1; pure read |
| `u3h_crc_sid` | `uint32_t u3h_crc_sid(const char *name)` | C part: string id of a name by the rule of G23 3.9; pure |
| `u3h_h13_mode` | `uint32_t u3h_h13_mode(void)` | C part: the tab box mode 0..3 (2 = default); **also stores the mode in `ec[172]`** each time it is called |
| `u3h_active_page` | `uint32_t u3h_active_page(uintptr_t base)` | C part: id of the menu page on top, 0 when none |
| `u3h_w10_diag` | `volatile uint32_t *u3h_w10_diag(void)` | C part: the slider's four diagnostic words W[0..3] |
| `u3x_k26_call`, `GameCall` | CONVENTIONS.h | game call through a call block (the old code went through the C export `u3h_game_call(void *)`, which only forwards to it) |

## B.3 Game facts

**Layout item** (44 bytes; "word n" = u32 at 4 n):

| Word | Meaning (as far as used) |
|---|---|
| 0 | type: 2 = texture, 3 = UI icon, 4 = bar (slider), 0x10 = loading spinner, 0x1b = flat box; PS3-only types handled here: 0x24 map picture, 0x29 rank badge, 0x2d tab box, 0x2f "new", 0x30 prestige badge while new, 0x31 equipped and new |
| 2, 3 | type 2: the texture pointer (8 bytes at +8) |
| 4 | value: map id (0x24), rank word (0x29), prestige level (0x30) |
| 5 | type 3: icon id |
| 6 | type 3: 1 = icon given by id and index |
| 7 | type 3: icon index; otherwise −1 |
| 9 | −1 = no second index (u32 at +0x24; for a box item this word is the item colour, ARGB) |
| 10 | float pattern, scale (1.0 = 0x3f800000) |

The item handed to the layout function is the caller's per-draw copy, so rewriting it in place is safe.

**Laid-out item** at the item draw 0x63d6f0 (used by the slider track): at least 0x70 bytes; u32 type at +0; u32
colour at +0x24; float patterns width at +0x3c and height at +0x40 (units of the 1280 x 720 layout).

**Game function 0x63d6f0** (item draw): eight arguments, six in the integer registers and two on the stack; the
fourth (rcx) is the laid-out item. No float register is used by the call made here (al = 0).

**Game function 0x11b89b0**: `uint64_t fn(uint32_t icon_id, uint32_t index)`: the UI material of an icon.

**UI texture table** (for `u3x_tab_icon_item`): pointer at image 0x1b24bc8 → UI object; pointer at UI object + 0x20
→ table; u16 at table + 2 = entry count; pointer at table + 8 = entries of 0x28 bytes; u32 at entry + 8 = icon id.

**Ids and constants**

| Value | Meaning | Source |
|---|---|---|
| icon "new" | string id of "general/net/menu-icons/icon-new" | PS3 1.19 value, not in the PS4 image: compute with `u3h_crc_sid` (rule of G23 3.9); cross-check: 0x0c1070e4 |
| icon "check" | string id of "general/net/menu-icons/icon-check" | same; cross-check: 0x08208d0a (G23's check value) |
| prestige badge icons | three u32 icon ids at image 0x13a6670 (entry size 4, count 3; index = prestige level − 1) | read from the running game; never copied |
| 0x6e9d9193 | icon id of the gradient bar texture (the tab box art) | an id the PS4 code itself uses (immediates at image 0x5dafd2 and 0x5db61d) |
| 0xa09ca4ae | the helper's own item colour (ARGB) for a flat tab box | own value: chosen so that the game's two draw colours times it give the PS3 look |
| 0xff000000 | opaque black, the slider track colour | own value |
| 0x85d761b5 | page id of the emblem editor page | page id |
| image 0x17d5c61 | u8, non-zero while multiplayer is active | |

**Switches and counters** (state X): `ea[1]` bit 0 = Legacy 4/5 off (prestige limit 4 instead of 6); `ea[170]`
bit 0 = rank item off; `ea[171]` = rank items converted (low 16 bits) and misses (high 16 bits); `ea[172]` = last
rank word; `ea[240]` bit 2 = tab boxes on; `eb[2]` bit 20 set = decorated badges off; `eb[3]` bit 30 set = no PS3
colour for the flat tab box; `ec[1]` bit 7 set = icon / map rewrite off; `ec[1]` bit 12 set = tab box as UI icon;
`ec[3]` bit 0 set = slider track off, bit 1 set = track on every multiplayer bar; `ec[174]`, `ec[176 .. 197]`,
`ec[199]` = tab box diagnostics (B.4.1, B.4.3); H[i] = `ec[240 + i]` for i = 0 .. 11 = item rewrite diagnostics.

## B.4 Behaviour

### B.4.1 `u3x_item_layout(base, s, r)`; always returns 0

item = the pointer in `r[5]` (rsi). If it fails the 4-aligned pointer test: return. t = word 0. If t < 0x24 or
t > 0x31: return. Then H[10] += 1 and bit (t − 0x24) of H[6] is set.

By type:

- **t = 0x2d (tab box).** Call `u3h_h13_mode()` (always, for its side effect). Only when it returns 2 and bit 2 of
  `ea[240]` is set:
  - `ec[1]` bit 12 set: `u3x_tab_icon_item(base, item, the pointer in r[7])`; its result is not used.
  - else: word 0 = 0x1b; `ec[199]` += 1; and, unless bit 30 of `eb[3]` is set: if word 9 is 0xffffffff it becomes
    0xa09ca4ae (any other colour stays).

  Return.
- **t = 0x29 (rank badge).** If `u3x_rank_item(base, s, item)` returns 1: return. Otherwise continue with the
  next rule (which counts it).
- **t = 0x25, 0x26, 0x27, 0x29, 0x2b or 0x2c** (types nobody draws): H[8] += 1; H[9] = t; return.
- If bit 7 of `ec[1]` is set, or t is none of 0x24, 0x2f, 0x30, 0x31: return.
- **t = 0x24 (map picture).** id = word 4; H[7] = id. map = `u3x_m04_map(s, id)`; name = the pointer at
  map + 0x10, or null when map is null. texture = native 20(image 0x1b31128 address, name) when native 20 and name
  are both non-null, else null. When name is not null: H[11] = the hash of the name's first at most 64 characters
  (start 0; for each character, as an unsigned byte: hash = hash x 31 + character, 32-bit). Then the item is
  rewritten whether or not a map or texture was found: words 1 to 9 = 0, then word 7 = 0xffffffff, word 9 =
  0xffffffff, word 10 = 0x3f800000; with a texture: word 0 = 2, the texture pointer into words 2..3, H[3] += 1;
  without: word 0 = 0x10, H[4] += 1.
- **t = 0x2f, 0x30, 0x31 (icons).** icon = the "new" icon; for t = 0x31 the "check" icon; for t = 0x30 with word
  4 equal to 1, 2 or 3 the prestige badge icon of that level (read from image 0x13a6670). index =
  `u3h_icon_index(base, icon)`. Negative: H[5] += 1, return, item untouched. Otherwise: word 0 = 3, words 1, 2,
  3, 4 = 0, word 5 = icon, word 6 = 1, word 7 = index, word 8 = 0, word 9 = 0xffffffff (word 10 is not written);
  then H[0] += 1 for 0x2f, H[1] += 1 for 0x31, H[2] += 1 for 0x30.

### B.4.2 `u3x_rank_item(base, s, item)` and `u3x_image_item(item, texture)`

`u3x_image_item`: words 1 to 9 = 0, word 7 = 0xffffffff, word 9 = 0xffffffff, word 10 = 0x3f800000, word 0 = 2,
the texture pointer into words 2..3 (the pointer is stored as given, also null). Final memory only; the order of
the stores is free.

`u3x_rank_item`:

1. Bit 0 of `ea[170]` set: return 0. Native 20 null: return 0. (Nothing written.)
2. v = word 4; `ea[172]` = v. level = bits 16..29 of v. limit = 4 when bit 0 of `ea[1]` is set, else 6.
3. When bit 20 of `eb[2]` is clear and level < limit: `u3x_rank_back(base, s, level)`; the result is not used
   (the call gives the back texture its own turn at the loader; level 0 gives null there).
4. name = `u3x_rank_icon_name(buffer of 40 bytes, v, limit)`; texture = native 20(image 0x1b31128 address, name).
   Null: `ea[171]` += 0x10000, return 0, item untouched.
5. `u3x_image_item(item, texture)`; `ea[171]` += 1.
6. A scratch block of eight s32, all 0, is given to `u3x_rank_tag(base, s, block, v)`; then word 4 = block
   element 6 and word 5 = block element 7 (the badge marker and the rank word when G22 tagged it, else 0 and 0).
7. Return 1.

### B.4.3 `u3x_tab_icon_item(base, item, box)` (opt-in route; stray)

1. index = position of the gradient bar icon (id 0x6e9d9193) in the UI texture table: the UI object pointer, the
   table pointer and the entries pointer must each pass `ptr_ok`, the count must be 1 to 4095; the first entry
   with that id. Not found, or any test fails: return 0, nothing written.
2. Once only (while `ec[176]` is 0): `ec[177 .. 186]` = the ten u32 words of that table entry; `ec[176]` = index |
   0x80000000; m = game 0x11b89b0(0x6e9d9193, index) through a call block (two integer arguments, everything else
   0); `ec[187]` = low half of m, `ec[188]` = high half; when m passes `ptr_ok`: `ec[189 .. 196]` = the first
   eight u32 at m.
3. w = h = pattern 0. When `box` passes the 4-aligned pointer test: w = box word 0, h = box word 1 (float
   patterns: the cell's width and height). Unless 0 < w and 0 < h (`f_less`; false for NaN): return 0.
4. `ec[197]` = (`f_to_int`(w) AND 0xffff) | `f_to_int`(h) << 16.
5. Item: word 0 = 3, word 1 = 0, word 2 = w, word 3 = h, word 4 = 0, word 5 = 0x6e9d9193, word 6 = 0, word 7 =
   index; words 8 to 10 are not written. `ec[174]` += 1. Return 1.

### B.4.4 `u3x_slider_track(base, r)`

`r` is the block of the stub at 0x63d6f0: `r[7]` = the laid-out item, `r[4]`, `r[5]`, `r[6]`, `r[3]`, `r[2]` =
the other five register arguments (rdi, rsi, rdx, r8, r9), `r[11]`, `r[12]` = the two stack arguments.

1. W = `u3h_w10_diag()`. item = `r[7]`. If it fails the 4-aligned pointer test or its type is not 4: return
   (nothing counted).
2. W[0] += 1.
3. Skip (W[2] += 1, return) when any of these holds, tested in this order, later ones not evaluated once one
   holds:
   a. bit 0 of `ec[3]` is set;
   b. bit 1 of `ec[3]` is set and the multiplayer byte (image 0x17d5c61) is 0; or bit 1 is clear and
      `u3h_active_page(base)` is not 0x85d761b5 (the page is only asked when bit 1 is clear);
   c. `r[6]` or `r[3]` fails the 4-aligned pointer test;
   d. the u32 at image 0x63d6f0 is 0 (the function is not there).
4. box = a 16-byte aligned copy of the item's first 0x70 bytes with type = 0x1b and the u32 at +0x24 =
   0xff000000.
5. Call game 0x63d6f0 through a call block (`u3x_k26_call`): integer arguments `r[4]`, `r[5]`, `r[6]`, the
   address of box, `r[3]`, `r[2]`; stack words `r[11]`, `r[12]`; float registers 0 and float count 0.
   (This call passes the same stub again; with type 0x1b this function returns at step 1.)
6. W[1] += 1.
7. W[3] = (`f_to_int`(item width pattern) AND 0xffff) | `f_to_int`(item height pattern) << 16, read from the
   original item.

The game then draws the bar itself on top (the dispatcher's business).

## B.5 Test scenarios

All of Part B runs with stubs: a fake image block (prestige icon table at 0x13a6670, UI object at 0x1b24bc8,
multiplayer byte, non-zero bytes at 0x63d6f0), recording stand-ins for native 20, `u3x_rank_back`, `u3x_rank_tag`,
`u3x_rank_icon_name` (or the real one), `u3x_m04_map`, `u3h_icon_index`, `u3h_h13_mode`, `u3h_active_page`,
`u3x_k26_call` (records the block and the 0x70-byte box), 0x11b89b0. Compare the item, X, W and the call sequence,
old against new. The look itself (does the track show, is the badge right) needs the live game.

1. Layout: item pointer 0, odd, 4-aligned but not 8-aligned (accepted); every type 0x23 to 0x32 (H[6] bits,
   H[10]).
2. Tab box: mode 0, 1, 2, 3 (the mode call is made each time); `ea[240]` bit 2 clear; flat route with colour
   0xffffffff and another colour, `eb[3]` bit 30 set; icon route (B.4.3 called with `r[7]`).
3. Rank badge inside the layout: converted (return, H[8] not counted); not converted (H[8], H[9] = 0x29).
4. Types 0x25, 0x26, 0x27, 0x2b, 0x2c; types 0x28, 0x2a, 0x2e (only H[6] / H[10]).
5. `ec[1]` bit 7 set with 0x24 / 0x2f (untouched) and with 0x2d / 0x29 (still handled).
6. Map picture: map not found; name null; native 20 null; texture null (spinner); texture found; a name longer
   than 64 characters (hash stops); characters above 0x7f (unsigned).
7. Icons: 0x2f; 0x31; 0x30 with value 0, 1, 2, 3, 4; icon missing (H[5], untouched); word 10 kept.
8. Rank item: switch off; native 20 null; level 0, 3, 5, 6 with Legacy on / off (back asked or not); `eb[2]`
   bit 20 set; texture null (miss count); tag writes the marker / leaves zeros.
9. Tab icon item: each failing pointer; count 0 and 4096; id absent; first call (once block: with m plausible and
   not) and second call; box failing the test, width 0, negative, NaN; normal.
10. Slider: type 3; type 4 with each skip cause alone (and the page not asked when bit 1 is set); drawn (block
    contents, box bytes, W[1], W[3] with fractional, negative and huge sizes).

## B.6 Behaviour to keep (looks odd, must stay)

1. The three pointer tests named "4-aligned" accept addresses that `ptr_ok` rejects.
2. `u3h_h13_mode` is called for every tab box item, also when tab boxes are off (it writes `ec[172]`).
3. A map picture item is rewritten (to the spinner) even when the map id is unknown.
4. The icon rewrite leaves word 10; the tab icon item leaves words 8 to 10.
5. The switch `ec[1]` bit 7 does not cover the tab box and the rank badge.
6. A rank badge that could not be converted is counted in H[8] / H[9].
7. `u3x_rank_item` asks for the back texture also for level 0 (a null answer) and ignores the answer.
8. In `u3x_tab_icon_item` the once-only diagnostics (and its game call) run before the box is tested.
9. The slider counts W[0] before the switches; the copy is 0x70 bytes whatever the item's real size.

---

# Part C. Submesh draw diagnostic

## C.1 Purpose and callers

A diagnostic for missing background geometry. The game draws a background instance submesh by submesh through its
function at image 0xe99530, which silently skips a submesh for several reasons. `u3x_f4_submesh` repeats the
function's decision without drawing, and counts, per reason, why submeshes are skipped. It changes nothing in the
game, with one opt-in experiment (a submesh that has no shader for the chosen variant borrows another variant's).

Caller (stays): C wrapper `f4_sub`, called by the dispatcher for stub kind 235 = a value stub at the entry of
0xe99530, only with a non-null block. **It runs on the game's render threads, several at once**: every counter
update marked "atomic" must be one atomic operation (relaxed ordering is enough). The result goes back to the
stub: 0 = the game's function runs (always).

## C.2 Interface

`int u3x_f4_submesh(uintptr_t base, uint64_t *r)`: existing name, new body. The C wrapper passes `(base, r)`.

| Name | Signature | Effect |
|---|---|---|
| `u3h_ext` | CONVENTIONS.h | `eb[3]` bit 4 = the shader borrow experiment |
| `u3h_f4_state` | `uint8_t *u3h_f4_state(void)` | C part: the diagnostic record S. Used here: u32 array HDR at S + 0 (16 words), u32 array WHY at S + 0x280 (8 words), u32 array RING at S + 0x2c0 (32 words) |
| `u3h_f4_current` | `uint32_t *u3h_f4_current(void)` | C part: the current frame's counters CUR, 16 rows of 8 u32 (row = pass index AND 15) |
| `u3h_f4_entry` | `volatile uint8_t *u3h_f4_entry(uint8_t *instance)` | C part: the 8-byte record E of this instance in the diagnostic map (it reads the instance's id at +0xd4 and its owner record at +0x120, and may reset the record when the id now belongs to another owner). Called exactly once per accepted call, at the place C.4 says |

## C.3 Game facts

Stub block: `r[4]` (rdi) = the submesh; `r[6]` (edx, low 32 bits) = the pass index; `r[7]` (ecx, low 32 bits) = the
draw type; `r[3]` (r8) = the instance + 0x170; `r[14]` (the 4th stack argument, low 32 bits, signed) = the variant
selector.

Instance: s32 at +0x164 = submesh count; pointer at +0x168 = submesh array (elements of 0x3a0 bytes); u32 at
+0xd4 = id.

Submesh:

| Offset | Type | Meaning |
|---|---|---|
| +0x10, +0x18 | u64 each | vertex and index buffers (0 = none) |
| +0x40 + 8 v | u64 | shader of variant v, v = 0 .. 0x1f (0 = none) |
| +0x240 | bit array of u64 words | pass mask: bit number = pass index (word = pass / 64, bit = pass mod 64) |
| +0x250 | s32 | a mode value |
| +0x255 | u8 | "has override variant" flag |
| +0x256 | u8 | visible flag |
| +0x328 | s32 | LOD index (negative = none) |

Game data: **the variant table at image 0x13a7bf0: 27 entries of 4 bytes (u32), index = draw type**; it is read
from the running game, never copied. u8 at image 0x18c6994 = "no override" switch of the game. u8 at image
0x17d5c61 and 0x17d5c60 = multiplayer active / LAN (either non-zero = multiplayer).

The per-type rules of C.4 step 7 are facts of how 0xe99530 chooses a variant (which submesh fields it tests for
which draw type); the diagnostic is only right when it tests the same fields.

Reason bits: 1 = A no geometry, 2 = B pass not in the mask, 4 = C not visible, 8 = D no shader for the variant,
0x10 = E draw type 0xe in mode 4, 0x20 = Z draw type 0x1a, 0x40 = variant out of range.

## C.4 Behaviour

pass = low 32 bits of `r[6]`; type = low 32 bits of `r[7]`; selector = `r[14]` as s32; P = pass AND 15.

1. HDR[4] += 1 (atomic).
2. sub = `r[4]`; inst = `r[3]` − 0x170. If either fails `ptr_ok`: HDR[5] += 1 (atomic), return 0.
3. array = pointer at inst + 0x168; count = s32 at inst + 0x164. If array fails `ptr_ok`, or count ≤ 0, or
   count > 4096, or sub lies before the array, at or after its end (array + count x 0x3a0), or not on an element
   boundary: HDR[5] += 1 (atomic), return 0.
4. K = the submesh's index in the array. bit = 1 << pass when pass < 7, else 0x80.
5. E = `u3h_f4_entry(inst)`; E[2] |= bit (atomic); CUR[P][5] += 1 (atomic); when the LOD index is ≥ 0:
   WHY[7] += 1 (atomic).
6. Reason w, first rule that holds:
   - either buffer is 0 → 1;
   - type = 0x1a → 0x20;
   - the pass's bit in the pass mask is clear → 2;
   - the visible flag is 0 → 4;
   - otherwise step 7.
7. Variant: v = table[type] when type < 27, else 0. "sel" = selector equals 1. "ovr" = the submesh's override flag
   is non-zero and the game's "no override" byte is 0. S(n) = the shader of variant n is non-zero.

   | Draw type | Rule |
   |---|---|
   | 0 | sel and S(0x1b): v = 8 |
   | 1 | sel and S(0x1b): v = 0x1b |
   | 3 | ovr: v = 2, and with sel and S(3): v = 3. Not ovr: sel and S(1): v = 1. Then, if S(0x1f) and v ≠ 1: v = 0x1f. This type allows the borrow of variant 9 below |
   | 4, 5 | sel and S(0x1c): v = 0x1c |
   | 9 | v = 0x11; with sel and S(0x12): v = 0x12 |
   | 0xe | when the mode value is 4: w = 0x10. v = 0xe |
   | 0xf | ovr: v = 0x14 |
   | any other | v stays |

   Then, when w is still 0:
   - v < 0x20 (v taken as a signed 32-bit number): if S(v) is 0, the type is 3 and S(9): v = 9. If S(v) is
     (still) 0: w = 8; i = the old value of HDR[7], which is incremented atomically (fetch-and-add), AND 31;
     RING[i] = (instance id AND 0x3ff) | (K AND 63) << 10 | P << 16 | (v AND 63) << 20 | (type AND 63) << 26;
     CUR[P][7] += 1 (atomic); and, when multiplayer is active and bit 4 of `eb[3]` is set: the first of the
     variants 0, 1, 2, 3, 9 (in this order; the helper's own choice for the experiment) with a non-zero shader
     gives its shader value to variant v of the submesh (a u64 store into the game's submesh), and w becomes 0.
   - v ≥ 0x20: w = 0x40.
8. If w ≠ 0: E[4] |= w (atomic, 8-bit), and for each bit j = 0 .. 6 set in w: WHY[j] += 1 (atomic). If w = 0:
   E[3] |= bit (atomic), CUR[P][6] += 1 (atomic).
9. Return 0.

## C.5 Test scenarios

Pure memory work: a fake image (table at 0x13a7bf0 with test values, the three bytes), a fake instance with a
submesh array, stand-ins for the three `u3h_f4_*` functions and X. Single-threaded old-against-new comparison of
all counters, E and the submesh; plus one run with several threads calling the new function to check that no
count is lost. No live game needed.

1. Rejections: submesh / instance pointer bad; array bad; count 0, −1, 4097; submesh before the array, at its
   end, off a boundary.
2. Each reason alone: A (each buffer), Z, B (pass 0, 63, 64, 200), C; pass 6, 7, 15, 16 for the bit and row.
3. Every draw type 0 to 27 and one above, with selector 0 / 1, the override flag and the "no override" byte in
   all combinations, each tested shader present / absent; type 3 with variant 0x1f present; type 3 borrowing
   variant 9; type 0xe with mode 4 and other modes; a table value ≥ 0x20 (reason 0x40) and a table value with bit
   31 set.
4. Reason D: ring record and wrap of the ring index after 32 records; experiment off; experiment on outside
   multiplayer; on with each of the five candidates as the first present one; on with none present.
5. LOD index −1, 0.

## C.6 Behaviour to keep (looks odd, must stay)

1. WHY[7] is not a reason: it counts submeshes with an LOD index.
2. For draw type 0 the tested shader is variant 0x1b but the chosen variant is 8.
3. A draw type of 27 or more uses variant 0 (the table is not read).
4. The experiment writes into the game's submesh and then counts the submesh as drawn, but the ring record and
   CUR[P][7] of the failed lookup stay.
5. Reason E leaves v unused (no shader test).

---

# Part D. Net anim groups

## D.1 Purpose and callers

Animation groups ("art groups") are found by name id in the loaded packages; G51 3.3 gives the layouts and G51
4.6 the search. A Player object in multiplayer has an array of eight art group pointers that PS3 1.19 fills by
name when the Player is made; the PS4 game allocates the array and leaves it empty, so every lookup of a death
animation in it failed. The helper fills it.

Callers:

- `u3x_find_art_group(slots, id)`: the C function `find_art_group` and the old `u3x_player_net_anims`. After
  this group it may have no caller left (see D.2); it keeps its name because the old-versus-new test calls it.
- `u3x_art_group_name(base, name)`: C `find_art_group` becomes its one-line wrapper. `find_art_group` is called
  by the C export `u3h_find_art_group_name` (used by G49 and G51) and by C code of the anim port.
- `u3x_player_net_fill(base, player)`: C `player_net_anims` becomes its one-line wrapper; called by the stub
  dispatcher for kind 57 (a hook in the game's Player initialisation at image 0xbd7764, right after the game
  cleared the array), with the Player object, when `base` is not 0. Main thread.

## D.2 Interface

| Function | Signature | Kind | C wrapper passes |
|---|---|---|---|
| group by id | `uint8_t *u3x_find_art_group(const uint8_t *slots, uint32_t id)` | existing name, new body | – |
| group by name | `uint8_t *u3x_art_group_name(uintptr_t base, const char *name)` | new entry for C `find_art_group` (stray) | `(base, name)` |
| Player's groups | `void u3x_player_net_fill(uintptr_t base, uint8_t *player)` | new entry for C `player_net_anims` (stray) | `(base, pl)` |

Removed: `u3x_player_net_anims(groups, slots, name_id, names)` and the C table `player_net_anim_names` (its only
user was C `player_net_anims`).

Called from other places:

| Name | Signature | Effect |
|---|---|---|
| `u3h_ext` | CONVENTIONS.h | `fsw[]`, `pna[]` |
| `u3x_art_group_sid` | `uint8_t *u3x_art_group_sid(uintptr_t base, uint32_t sid)` | G51 4.6: the loaded art group with that name id (null for id 0, a null level manager or a null slot array) |
| `u3x_net_art_group` | `uint8_t *u3x_net_art_group(uintptr_t base, uint32_t row, uint32_t col)` | G51 4.10: the art group of cell (row, col) of the net anim package arrangement; it gets there through `u3h_find_art_group_name`, i.e. through `u3x_art_group_name` |
| name-id function | pointer at image 0x1702720: `uint32_t fn(int zero, const char *name, int one)` | the game's name hash; called as fn(0, name, 1). The pointer may be null |

## D.3 Game facts

G51 3.3 (level manager at the pointer at image 0x1b24b80 + 0x38, slot array at manager + 0x38, 350 slots of
0x2c0 bytes, package hash table) and G51 3.5 (where the PS4 image holds the package names; row 0 = player
characters = the names N1, N2, N3, N4 in columns 0 to 3, columns 4 to 7 empty) are used unchanged.

**Table `player_net_anim_names`** (8 slots, 4 names used): it is exactly row 0 of G51 3.5. The PS4 image holds all
four texts (N1 at image 0x145b8ff, N2 at 0x145f915, N3 at 0x145f953, N4 at 0x145f92c; zero-terminated ASCII, each
referenced by the game's own code). The new code holds no copy: it goes through `u3x_net_art_group(base, 0, col)`,
which reads them from the image.

Player object: pointer at +0x1c20 = its array of eight art group pointers (null outside multiplayer).

u8 at image 0x17d5c61 = multiplayer active. `fsw[0]` bit 3 set = this fix off. `pna[0]` = Players filled,
`pna[1]` = groups found in total, `pna[2]` = found mask of the last Player, `pna[3]` = Players without array,
`pna[4]` = calls while switched off.

## D.4 Behaviour

### D.4.1 `u3x_find_art_group(slots, id)`

Null when `slots` is null. Otherwise exactly the search of G51 4.6 from the slot array on (350 slots; skip rules,
probing from `id` mod n, an empty entry ends a package, a hit with a null group moves on, first package wins).
`id` 0 therefore always gives null. Reads only. The implementer of G51 and this one may share one search; which
of the two functions holds it is free.

### D.4.2 `u3x_art_group_name(base, name)`

Null when `name` is null or empty (the name-id function is not called). Null when the name-id function pointer is
null. Otherwise id = fn(0, name, 1), and the result is the search for that id in the loaded packages: null when
the level manager pointer or its slot array pointer is null, else D.4.1. (`u3x_art_group_sid(base, id)` gives the
same result for every id.) The name-id function is called once, before the manager is looked at or after: not
observable, it only reads its text.

### D.4.3 `u3x_player_net_fill(base, player)`

1. `player` null, or the multiplayer byte is 0: return (nothing counted).
2. Bit 3 of `fsw[0]` set: `pna[4]` += 1, return.
3. array = the pointer at player + 0x1c20. Null: `pna[3]` += 1, return.
4. For col = 0 to 7 in this order: array[col] = `u3x_net_art_group(base, 0, col)` (null for the empty columns and
   for a package that is not loaded; every one of the eight entries is written). found = the mask with bit col
   set for each non-null result.
5. `pna[1]` += the number of bits in found; `pna[0]` += 1; `pna[2]` = found.

Equality with the old code, for the tester: the old code computed the name id of the four names in column order
(only when the name-id function and the level manager exist) and searched the slot array for each; the route
through `u3x_net_art_group` makes the same four name-id calls in the same order and no call for the empty
columns.

## D.5 Test scenarios

Fake image with a level manager, a slot array with a few packages and hash tables, the four texts at their image
offsets, a recording name-id stand-in at the pointer 0x1702720 (for example: id = a simple function of the text),
fake Player and X. No live game needed for the comparison; that death animations are found again needs the game.

1. Search: null slot array; slot not in use; null package; table size 0, negative; null table; hit at the home
   entry; hit after wrapping past the last entry; empty entry before the hit (miss); full table without the id
   (one round, miss); hit with a null group in the first package and a real one in a later package; the same id
   in two packages (first wins); id 0.
2. By name: null name, empty name (no name-id call); name-id pointer null; manager null; slot array null; found;
   not found.
3. Player fill: null player; not multiplayer; switch off; null array; all four packages loaded (mask 0x0f, the
   four name-id calls in order N1 to N4, entries 4 to 7 null); only some loaded; array pre-filled with junk
   (all eight overwritten); name-id pointer null (all eight null, mask 0, `pna[0]` still counted).

## D.6 Behaviour to keep

1. All eight array entries are written on every fill, also with null.
2. `pna[0]` counts a fill that found nothing.

---

# Strays, tables, notes for the integrator

**Strays covered here** (in no work group of INVENTORY.md):

- C `h13_native` (u3_lan_profiles.c about 12924; hand-written, only caller `h09_layout`): B.4.3, new entry
  `u3x_tab_icon_item`. Its icon search is the same fact as G55's gradient texture lookup; G55's describer should
  not describe `h13_native` again.
- C `player_net_anims` (about 3800) with its literal table `player_net_anim_names`, and the C++
  `u3x_player_net_anims` (8 lines): D.4.3, new entry `u3x_player_net_fill`; table and old function removed.
- C `find_art_group` (about 3786): D.4.2, new entry `u3x_art_group_name`.
- C++ private `remake_particles` and the private 4-aligned pointer test `plausible`: described inside A.4.3 and
  in the shared notation.

**Not covered, for the final sweep** (hand-written C around this group, in no work group): `h08_tick`, `h10_root`,
`h10_up` (the territory scan and its process tree walk, with a literal list of ten process bucket offsets),
`f4_ent`, `f4_owner`, `f4_inst`, `f4_tick` (the diagnostic's own bookkeeping), `h13_mode`, `active_page`. One-liners
that stay: `u3x_h08_on`, `u3x_w03_ok`.

**Tables**

| Table | Resolution |
|---|---|
| `player_net_anim_names` (C, 4 names in 8 slots) | removed: row 0 of G51 3.5, texts read from the PS4 image through `u3x_net_art_group` (D.3) |
| variant table of the submesh draw (27 x u32) | already read from the running game at image 0x13a7bf0 (C.3); stays so |
| prestige badge icon ids (3 x u32) | already read from the running game at image 0x13a6670 (B.3); stays so |
| icon ids "new" / "check" (two literals in C) | PS3 values, not in the PS4 image: computed with `u3h_crc_sid` from their texture names (B.3) |
| the five borrow variants 0, 1, 2, 3, 9 of the experiment | the helper's own choice, not a game table (C.4) |

**Open questions**

1. C.4 step 7 is written from the old diagnostic, which was itself written to mirror 0xe99530. Whether the game
   function tests exactly these fields was not re-checked here; the new code must do what the old diagnostic did.
2. Part D changes the route (through `u3x_net_art_group`) to get rid of the name table. If G51's implementation
   of row 0 is not available when Part D is integrated, the four texts are still to be read from the image
   offsets of D.3 and passed to `u3x_art_group_name`; never held as literals.
3. The tick rate of 300 per second in Part A is taken from the old comments (450 ticks = 1.5 s); only the tick
   numbers matter for the behaviour.

---

# Clarifications (tester, 2026-11-14)

Facts settled by the differential test (reimpl2\G61\test_g61.cpp, old v7.212 against part_a..d.cpp).

1. **B.4.2 read order of the Legacy switch.** The old `u3x_rank_item` reads bit 0 of `ea[1]` (prestige limit 4 or 6)
   twice: once for the level test of step 3, and again after `u3x_rank_back` has returned, for the limit passed to
   `u3x_rank_icon_name` in step 4. The new code reads it once before step 3. The two differ only when
   `u3x_rank_back` changes bit 0 of `ea[1]`; the real `u3x_rank_back` (G22 4.13) only counts in `ea[212]` /
   `ea[213]`, so the difference cannot happen in the helper. Accepted as is; to be exact, step 4 takes the limit
   from the switch as it is after step 3.
2. **Icon ids (B.3).** `u3h_crc_sid("general/net/menu-icons/icon-new")` = 0x0c1070e4 and
   `u3h_crc_sid("general/net/menu-icons/icon-check")` = 0x08208d0a, the old literals: the id that reaches
   `u3h_icon_index` is the same in every case.
3. **D.4.2 / D.4.3 name-id calls.** With a null level manager or a null slot array the old code makes no name-id
   call; the new route (through `u3x_art_group_name`) calls the name-id function first. As the spec says this is not
   observable (the function only reads its text); with manager and slot array present the calls are identical in
   number, order and text, and every result and counter is equal.
4. **C.4 step 7, table values with bit 31 set.** v is a signed index: for the values 0xfffffff8 .. 0xffffffff the
   shader word is read (and, in the experiment, written) at submesh + 0x40 + 8 v, i.e. before the shader array;
   old and new agree. Values far below that (around 0x80000000) give a wild address in both and were not run.
5. **Open question 2.** G51's `u3x_net_art_group` exists in reimpl2\G51\part.cpp, so the route of D.4.3 is
   available; the test used a stand-in that does what the old code did for row 0.
6. **Open question 1** (does 0xe99530 test exactly these fields) cannot be settled off the game; the new code
   does exactly what the old diagnostic did in every combination of C.5 scenario 3 and 40000 random cases.
