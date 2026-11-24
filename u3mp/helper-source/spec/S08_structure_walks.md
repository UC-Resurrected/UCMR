# S08. Game structure walks: Shade melee moves, solo co-op patch and church-key diagnostic, Havok packfile reuse, UI material default texture, weapon skin materials

Behaviour description for a reimplementation (reimpl2 pipeline, describer's document, sweep group S08). It says
what the functions do, not how the old ones were written. Offsets are hexadecimal when prefixed with 0x. "Image
offset X" means the address `base + X`, where `base` is the game image base. All multi-byte values in memory are
little-endian; "u8 / u32 / s32 / u64" are integers of that width, "pointer" is 8 bytes. Game functions use the
System V AMD64 convention.

Related documents: `film_helpers.md` 4.9 (`u3x_rp_isa`), `G23_boosters.md` 3.9 (the string id rule),
`G61_territory_items_misc.md` (`u3h_crc_sid`, value stub block notation), `G38_custom_settings.md` 3.6 and
`G32_menu_actions.md` (`u3h_nmh`, the custom-game menu table), `G11_game_sockets_time.md` (the `Log` argument).

**Float.** Nothing in this group does float arithmetic and nothing runs on the x87 unit. `x87_apply` is named
after the fixer item "x087"; it has no float at all. The only float values are three constant 16-byte vectors
of Part B that are passed **by pointer** to a game function (B.3); they are data, never computed.

The group is five unrelated parts. They share no code and no state:

| Part | Content | Old code lines | Exported names |
|---|---|---|---|
| A | Shade melee knock-back moves restored | 19 | `u3x_melee_restore` |
| B | Solo co-op / store-unlock switches and the church-key diagnostic | 61 | `u3x_solo_patch_tick`, `u3x_mk02_tick`, `u3x_mk03_process`, `u3x_mk03_edge` |
| C | Havok packfile: answer an already loaded blob's contents | 11 | `u3x_hk21_load` |
| D | UI material default texture (packet level and executor level) | 66 | `u3x_k25_fill`, `u3x_k40_tex`, `u3x_k40_deftex` |
| E | Weapon skin materials set on the submesh slots | 13 | `u3x_skin_material_apply` |

Shared notation:

- X = `u3h_ext()` (CONVENTIONS.h), the helper's state. `eb[]` = u32 words at X + 0x4050 (`ext_eb()`). `mel[0..3]`
  = u32 words at X + 0x2768. `clone[k]` (k = 0, 1) = 0x220 bytes at X + 0x2780 + k * 0x220 (16-byte aligned).
  `sc[0..7]` = u32 words at X + 0x2ff0. These positions are part of the helper's state layout and stay.
- "`ptr_ok`" is CONVENTIONS.h's test (at least 0x10000, below 2^47, multiple of 8).
- "**sg test**": value above 0x100000 (strictly), below 0x7fffffffffff, multiple of 8. Different from `ptr_ok`;
  Part B uses it everywhere it tests a pointer.
- "x += 1" on a diagnostic word is a plain read, add, write.
- **Register frame R15** (Part D): the array `r` handed over by the register-saving stubs of kinds 162 and 165:
  r[0..14] = r15, r14, r13, r12, rbx, r11, r10, r9, r8, rdi, rsi, rdx, rcx, rax, rbp as they were at the hooked
  instruction; the address of r[15] (that is, `(uint8_t *)r + 0x78`) equals the game's rsp at that instruction.
  Writing an element of `r` has no effect on the registers; only memory writes matter.
- **Register frame R9** (Part C): the array `r` of the kind 78 stub: r[0..8] = r11, r10, r9, r8, rdi, rsi, rdx,
  rcx, rax. When the function returns non-zero, the stub skips the hooked call and continues with rax = r[8] as
  the function left it; when it returns 0, the stub performs the hooked call itself.

---

# Part A. Shade melee knock-back moves

## A.1 Purpose and caller

PS3 1.19 builds its melee moves as globals during static init: after the two "armor attack" moves (in move lists
1 and 6) it builds the two Shade knock-back moves, then the brute kick. PS4 keeps the armor pair and the brute kick
but dropped the Shade pair. The Shade pair is the armor pair with only its move ids and animation names changed,
so the helper clones the two armor moves, renames them, and appends the clones to move lists 1 and 6.

Caller: the C function `natives_link` (stays, own glue), at the script manager init (hook kind 44), every time it
runs. The function does its work once (guard `mel[0]`).

## A.2 Interface

| Function | Signature | C wrapper |
|---|---|---|
| restore | `extern "C" void u3x_melee_restore(uintptr_t base)` | `melee_restore(base)` passes base |

Uses `u3h_ext` only.

## A.3 Game facts

| What | Where | Meaning |
|---|---|---|
| armor move, list 1 | image 0x1b90690, 0x220 bytes | source of clone 0 |
| armor move, list 6 | image 0x1b908b0, 0x220 bytes | source of clone 1 |
| move registry | image 0x1b92780 + list * 0x800 | 256 pointer slots per list |
| list counts | u32 at image 0x1b99780 + list * 4 | number of used slots of that list |

A move object holds two parameter blocks, at +0xf8 and at +0x1d8. Each: pointer at +0 = enter animation name
(text), pointer at +8 = animation name (text), u32 at +0x10 = move id A, u32 at +0x14 = move id B. The armor moves
have id A = 0xbb180a56 and id B = 0x5055663b (checked at +0x108, +0x10c and +0x1e8).

Shade values (PS3 1.19; see "Tables" at the end): id A = 0x4b5f37f8, id B = 0xfc353425, enter animation
"sha-melee-shade-attack-knock-back-a-hero-enter", animation "sha-melee-shade-attack-knock-back-a-hero". The game
keeps the text pointers, so the texts live in the helper's read-only data for good. Move objects must be 16-byte
aligned (the game's reset does an aligned 16-byte access at +0x90); the clone areas are.

## A.4 Behaviour

1. If `mel[0]` is non-zero: nothing.
2. Check both sources first: for each, u32 at +0x108 must be 0xbb180a56, at +0x10c 0x5055663b and at +0x1e8
   0xbb180a56. If either fails: `mel[3]` += 1 and nothing else (no copy, no append).
3. For k = 0 (source 0x1b90690, list 1) then k = 1 (source 0x1b908b0, list 6):
   a. Copy the 0x220 source bytes into `clone[k]`.
   b. In both parameter blocks of the clone (+0xf8 and +0x1d8): enter name pointer, name pointer, id A, id B :=
      the Shade values.
   c. If the list count is 256 or more: `mel[3]` += 1 and go on with the next k (the clone stays written).
   d. Otherwise store the address of `clone[k]` in registry slot [count] of the list, count += 1, `mel[0]` += 1,
      `mel[1 + k]` := the new count.

## A.5 Test scenarios (call-recording stubs: a fake image buffer)

- `mel[0]` = 1: nothing read or written.
- One check word wrong (each of the six in turn): `mel[3]` += 1, clones and registry untouched.
- Counts 5 and 7: clones = sources except the eight renamed fields; slot 5 of list 1 and slot 7 of list 6 point
  to the clones; counts 6 and 8; `mel` = {2, 6, 8, 0}.
- List 1 full (256), list 6 count 0: clone 0 written but not appended, `mel[3]` = 1, `mel[0]` = 1, `mel[1]`
  unchanged, `mel[2]` = 1.

---

# Part B. Solo co-op / store-unlock switches and the church-key diagnostic

## B.1 Purpose and caller

`u3x_solo_patch_tick` applies two of the user's switches every frame (they port the user's RPCS3 patches):
solo co-op (debug-options byte; custom-game list mode counts preset as PS3's static init does) and store unlock
(the store's "all items unlocked" flag). With solo co-op on it also runs the church-key diagnostic
`u3x_mk02_tick`, which logs the local player's name, co-op index and team, its state, and an edge test of the
local player and the three co-op heroes, to boot.log.

Caller: the C function `solo_patch_tick` (becomes the one-line wrapper) from the per-frame tick (kind 16), main
thread. `mk02_tick`, `mk03_res`, `mk03_edge`, `mk02_x` and their statics `mk02_l`, `mk02_n`, `mk03_k` have no
other users: delete them from the C part.

## B.2 Interface

| Function | Signature | Notes |
|---|---|---|
| tick | `extern "C" void u3x_solo_patch_tick(uintptr_t base, const uint8_t *s, Log log)` | C wrapper passes base, the profile state `s` and `ol01_print` |
| diagnostic | `extern "C" void u3x_mk02_tick(uintptr_t base, Log log)` | called by the tick only |
| process by name | `extern "C" uint64_t u3x_mk03_process(uintptr_t base, uint32_t name)` | 0 = not found |
| edge test | `extern "C" uint32_t u3x_mk03_edge(uintptr_t base, uint64_t p, uint32_t *owner, uint32_t *edge, uint32_t *handle, uint32_t *up)` | all four out-pointers valid |

Called: `u3h_ext`; `uint8_t *u3h_nmh(uintptr_t base)` (C part, address of the custom-game menu table, no side
effect, G38 3.6); `uint32_t u3h_crc_sid(const char *name)` (C part, string id, G23 3.9, pure);
`int u3x_rp_isa(uintptr_t base, uint8_t *obj, uint32_t hash)` (class test, `film_helpers.md` 4.9); `log(base,
line)`.

## B.3 Game facts

| What | Where | Meaning |
|---|---|---|
| switches | u32 at `s` + 0x3d78 | bit 0x80000 solo co-op, bit 0x200000 "no map" (skip the list preset), bit 0x100000 store unlock |
| debug-options byte | u8 at image 0x17d7a50 + 0x7d | PS3 keeps it 1 (its only reader prints debug text PS4 stripped) |
| custom-game table | `u3h_nmh(base)` | u8 at +0 = number of games (0 = not built yet); game i at +8 + i * 0x308; u8 at game + 4 = mode count. The game's builder fills 11 games |
| store flag | u8 at image 0x17b2a94 | store "all items unlocked" |
| MP active | u8 at image 0x17d5c61 | non-zero while a net game runs |
| local net player | pointer at image 0x17cb3d0 (net manager 0x17cb060 + 0x370) | record T: pointer at +0x10 = handle slot, u32 at +0x18 = handle id, s32 at +0x21c = team, s32 at +0x42c = co-op index |
| process object P | | u32 at +0x7c = handle id; pointer at +0x80 = state machine M (pointer at M + 8 = current state S; u32 at S + 0x18 = state name id); u32 at +0x98 = name id; pointer at +0x538 = edge holder E |
| edge reference | E + 0x128 (call it Rf) | u64 at Rf + 0 (low 32 bits logged), u32 at Rf + 0x10 = handle id |
| edge owner | game 0x1047c90: `uint64_t fn(void *Rf)` | owner object of the edge (null = none); u32 at owner + 0x98 = its name id |
| edge test | game 0x1047d60: `uint64_t fn(void *Rf, const void *dir, const void *limit)` | result's low 8 bits non-zero = passes |
| vectors (16 bytes, 16-byte aligned, read-only) | | dir = (0, 1, 0, 0): patterns 0, 0x3f800000, 0, 0. limit L1 = four times 0x3f34fdf4 (0.707). limit L2 = four times 0xc0000000 (−2.0) |
| process table | image 0x1b37610 | pointer at +0x20 = bucket array, s32 at +0x5c = bucket count c; entry: pointer at +0 = next, u32 at +0x10 = name id, pointer at +0x18 = handle slot, u32 at +0x20 = handle id. Pointer bucket[c] (the one after the last bucket) ends every chain |
| Player class | 0xa5a63e9b | string id of "Player" (for `u3x_rp_isa`) |
| co-op heroes | string ids of "coop-drake", "coop-chloe", "coop-sullivan" | compute with `u3h_crc_sid`; cross-check 0x9ba01002, 0x9ffcfd6c, 0x75d3456f |

A "handle" (slot pointer + id) resolves to the object pointer stored in the slot when the slot passes the sg test,
the id is non-zero, the object passes the sg test and its u32 at +0x7c equals the id.

Process table hash: 32-bit FNV-1a (Fowler/Noll/Vo, public; offset basis 0x811c9dc5, prime 0x01000193) over the
four bytes of the name id, low byte first; bucket index = hash mod c (unsigned).

## B.4 Behaviour

**B.4.1 `u3x_solo_patch_tick`.** m = the switches word.
1. If m has 0x80000: `sc[0]` += 1; run `u3x_mk02_tick(base, log)`; if the debug-options byte is 0: set it to 1,
   `sc[6]` += 1. Then, unless m has 0x200000: T = `u3h_nmh(base)`; if `sc[7]` is 0: `sc[7]` := 1 and, if the u8
   at T + 0 is 0, set the mode count byte of games 0..10 (T + 0xc + i * 0x308) to 1, `sc[2]` += 1 for each. (So
   the preset is tried exactly once per load, on the first such tick, and only into an unbuilt table.)
2. If m has 0x100000 and the store flag is 0: `sc[5]` := 0 (the value seen), flag := 1, `sc[4]` += 1.

**B.4.2 `u3x_mk03_process(name)`.** 0 when c ≤ 0, c > 0x100000 or the bucket array fails the sg test. Otherwise
walk the chain from bucket[hash mod c], at most 256 entries, while the entry passes the sg test and is not
bucket[c]. At the FIRST entry whose name id equals `name`: return the object its handle (+0x18 / +0x20) resolves
to, or 0 when it does not resolve (the walk does not go on). No match: 0.

**B.4.3 `u3x_mk03_edge(p, ...)`.** First all four outputs := 0. E = pointer at p + 0x538; fails the sg test: return
0. edge := low 32 bits of the u64 at Rf; handle := u32 at Rf + 0x10. o = game 0x1047c90(Rf); fails the sg test:
return 0. owner := u32 at o + 0x98. Then u = edge test (Rf, dir, L1), then f = edge test (Rf, dir, L2), in that
order. up := 2 * (u ≠ 0) + (f ≠ 0). Return owner when u ≠ 0, else 0.

**B.4.4 `u3x_mk02_tick`.** State private to the new code (zero at load): lines printed N, remembered name / index /
team, and the remembered record K = {owner, edge, handle, up, digits, state, stamp}.
1. Nothing when N ≥ 120 or MP active is 0. Nothing when the local net player pointer fails the sg test or its
   handle does not resolve (as above) to P.
2. nm = P's name id, ci = T's co-op index, tm = T's team. If any differs from the remembered three: remember them,
   print line 1, N += 1.
3. fr = `sc[0]`. st = state name id through P + 0x80 and M + 8 (each pointer must pass the sg test; else st = 0).
4. res = `u3x_mk03_edge(P)` giving owner eo, edge ed, handle eh, up uf.
5. For the heroes in the order drake, chloe, sullivan: q = `u3x_mk03_process(id)`; digit d = 0 when q is 0; 1 when
   `u3x_rp_isa(base, q, Player)` is 0; else r = res when q equals P, otherwise the result of a new
   `u3x_mk03_edge(q)` (its outputs discarded); d = 2 when r is 0, 3 when eo ≠ 0 and r = eo, else 4. rc = the three
   digits as a decimal number (drake = hundreds).
6. If eo ≠ 0: stamp := fr. armed = eo ≠ 0, or stamp ≠ 0 and (fr − stamp, unsigned 32-bit) < 240.
7. changed = eo, ed, eh, uf or st differs from K; digits changed = rc differs from K's digits. Store all six
   values and keep the stamp.
8. Print line 2 (N += 1) only when digits changed, or armed and changed.

Line 1: `U3MP mk02: local player name ` hex(nm) ` coop-idx ` dec(ci) ` team ` dec(tm).
Line 2: `U3MP mk03 f` dec(fr) ` st ` hex(st) ` obj ` hex(eo) ` e ` hex(ed) ` h ` hex(eh) ` up ` u f ` coop ` d d d
— where "u f" are the two digits of uf (bit 1, bit 0) written without a space, and "d d d" the three hero digits
without spaces. hex = `0x` and 8 lowercase digits; dec = signed decimal (a leading `-` for negative values).

## B.5 Test scenarios (fake image and table; `u3x_rp_isa`, game calls and log recorded)

- Switch combinations of the three bits; table built / unbuilt; `sc[7]` already set; flag already 1.
- Hash walk: match in the first, a later and no entry; chain reaching bucket[c]; 256-entry cap; match whose handle
  fails (returns 0 without going on); c = 0, c = 0x100001.
- Edge: E null; owner null (edge / handle set, owner and up 0); u and f in all four combinations.
- Diagnostic: N = 120; MP off; each hero digit 0..4 including q = P (no second edge call); armed by stamp at
  fr − 239 and fr − 240; line 1 only on change; both lines in one call.

---

# Part C. Havok packfile: answer an already loaded blob's contents

## C.1 Purpose and caller

PS4 keeps level collision as a Havok packfile. The level add (image 0x1133bf0) loads the blob on every add with
the game's loader (image 0xdfcd0), which fixes the pointers in place, sets bit 0 of the header word +0x38, and
returns 0 for a blob that already has that bit. A level still listed when the next physics world is built is
added again and gets contents 0, and the game faults (Start Cinema case). PS3 rebuilds the shape from resident
data on every add. The helper answers the address the loader returned the first time.

Caller: hook kind 78 at the loader call 0x1133c6c (C dispatcher `cinema_kind`, stays) through the C wrapper
`hk21_load(r)`, which becomes `return u3x_hk21_load(r);`.

## C.2 Interface

`extern "C" int u3x_hk21_load(uint64_t *r)`: `r` = register frame R9. Returns 1 = "use r[8] as the loader's
result", 0 = "run the loader". Uses `u3h_ext` only.

## C.3 Game facts (public Havok packfile layout: hkPackfileHeader / hkPackfileSectionHeader)

Blob b = r[4] (rdi), size = low 32 bits of r[5] (rsi). Header: u32 at +0 = 0x57e0e057 and u32 at +4 = 0x10c0c010
(the packfile magic); s32 at +0x14 = number of sections n; s32 at +0x18 = contents section index i; s32 at +0x1c =
contents offset within that section; u32 at +0x38 bit 0 = "already loaded" (set by the game's loader). Section
headers are 0x30 bytes each from +0x40; s32 at section + 0x14 = the section's data start (from the blob start).

## C.4 Behaviour

1. `eb[192]` += 1.
2. b below 0x10000, at or above 2^47, or not a multiple of 4: return 0.
3. Header bit 0 of +0x38 clear (a first load): return 0 (read the word fresh, it is written by the game).
4. Magic wrong, n ≤ 0, n > 64, i < 0 or i ≥ n: `eb[194]` += 1, return 0.
5. off = section i's data start + contents offset (64-bit signed sum of the two s32). If off < 0x40, or size ≠ 0
   and off ≥ size: `eb[194]` += 1, return 0.
6. r[8] := b + off; `eb[193]` += 1; `eb[195]` := low 32 bits of off; `eb[196]` := low 32 bits of b; return 1.

## C.5 Test scenarios (plain buffers)

Bad pointer (0, 0xfff0, 2^47, b + 2); bit clear; each magic word wrong; n = 0, 65; i = −1, n; off = 0x3f, 0x40,
size − 1, size; size 0 with a large off; a negative section start that makes off < 0x40. Counters as stated.

---

# Part D. UI material default texture

## D.1 Purpose and callers

UI image quads with the 2-texture material "general/hud/alpha-icon24:default" (all DXT1 UI images, e.g. the
Profile > Emblem picker cells) supply only the colour map. PS3's material system binds the material's own default
texture to a texture the instance does not supply; PS4's UI path lost that, so the second texture is empty.

- `u3x_k40_deftex` (active fix, packet level): hook kind 165 at 0x63dcbb in the UI quad (image 0x63dc40), right
  after the batch's second handle was cleared. The C dispatcher runs two other functions first (`j10_menu`,
  `j04_cell`, stay) and calls the wrapper `k40_deftex(b, r)` → `return u3x_k40_deftex(r);`.
- `u3x_k25_fill` (old executor-level variant, stub only installed with the builder switch U3MP_UI_MATDEF=1, off by
  default): hook kind 162 at 0xe41d19 in the UI packet executor (image 0xe41a60), when its 3-entry texture array
  is complete. Wrapper `k25_fill(b, r)` → `return u3x_k25_fill(r);`.
- `u3x_k40_tex`: used by `u3x_k40_deftex` only; the C `k40_tex` and `k28_ok` have no other caller (delete them,
  and the statics `k40_h`, `k40_w`, `k40_m`, `k40_d`).

## D.2 Interface

| Function | Signature | Result |
|---|---|---|
| executor fill | `extern "C" int u3x_k25_fill(uint64_t *r)` | always 0; `r` = frame R15 |
| default texture | `extern "C" uint64_t u3x_k40_tex(const uint8_t *mat)` | texture address or 0 |
| packet fix | `extern "C" int u3x_k40_deftex(uint64_t *r)` | always 0; `r` = frame R15 |

Uses `u3h_ext` only.

## D.3 Game facts

- Material `mat`: s32 at +0x18 = number of texture collections nc; pointer at +0x20 = collections, 16 bytes each:
  u32 at +0 = technique (0 = technique 0), u32 at +4 = number of textures nt, pointer at +8 = list of nt pointers;
  each list element points to a texture entry {u64 p0, u64 p1}; the entry's texture is p0 when p0 ≠ 0, else p1.
  "The technique-0 collection" is the first collection with technique 0.
- UI context: pointer at ctx + 0x68 + 8 * 0xc (= ctx + 0xc8) = the material "alpha-icon24".
- Kind 165 frame: ctx = r[9] (rdi), mat = r[12] (rcx), frame pointer fp = r[14] (rbp); the batch's second handle
  is the u64 at fp − 0x70.
- Kind 162 frame: packet pk = r[2] (r13); the texture array A = 3 u64 at (address of r[15]) + 0xe0, valid only
  when r[13] (rax) equals that address. Pointer at pk + 0 = the material; pointer at pk + 8 + 8k = handle k
  (diagnostics only: u32 at +0x20 and +0x28 of the handle are name characters).
- Texture object: u32 at +0 type bits (value & 3); u64 at +8 = descriptor (type 2 / 3) or object (type 1). A
  **handle** is 32 bytes: u64 at +0 = address of the texture object, u64 at +8 = 0, u8 at +0x10 = 1 ("loaded"),
  the rest 0.
- "Writer-safe" texture t (the game's resource writer reads it without faulting): t ≥ 0x10000 and t < 2^47 (no
  alignment test), its type bits are not 0, and the u64 at t + 8 is non-zero.
- The executor's type-2 fix-up: for a type-2 texture with descriptor m, the u32 at m + 4 is ANDed with 0xc3ffffff.

## D.4 Behaviour

**D.4.1 `u3x_k40_tex(mat)`.** 0 when nc ≤ 0, nc > 64 or the collections pointer fails `ptr_ok`. Take the
technique-0 collection (none: 0). 0 when its nt < 2 or its list fails `ptr_ok`; 0 when list element 1 fails
`ptr_ok`; t = that entry's texture; return t when it passes `ptr_ok`, else 0.

**D.4.2 `u3x_k40_deftex(r)`.** Private state (zero at load): four cache slots, each a material address, a 32-byte
handle H[k] and a 16-byte texture object W[k] (both 16-byte aligned); eight diagnostic words D[0..7].
1. D[0] += 1. If `eb[1]` has bit 0x40: return.
2. ctx, mat, fp must pass `ptr_ok`, and the material at ctx + 0xc8 must equal mat; else return.
3. Second handle non-zero: D[6] += 1, return.
4. k = the slot whose material is mat. If none: t = `u3x_k40_tex(mat)`; t = 0: D[3] += 1, return. k = the first
   slot with no material, or slot 3 when all four are used (it is overwritten). W[k] := {u32 2, u32 0, u64 t};
   H[k] u64 at +0 := address of W[k], u8 at +0x10 := 1; slot material := mat; D[2] += 1; D[4] := u32 at t;
   D[5] := u32 at t + 0x20.
5. Second handle := address of H[k]; D[1] += 1.

**D.4.3 `u3x_k25_fill(r)`.**
1. r[13] ≠ address of A: return. `eb[216]` += 1.
2. Clear unsafe entries: for k = 0, 1, 2 with A[k] ≠ 0 and not writer-safe: `eb[237]` += 1; `eb[238]` := u32 at
   A[k] (0xffffffff when A[k] fails `ptr_ok`); `eb[239]` := u32 at A[k] + 8 (same rule); `eb[240]` := k, plus
   (u32 at the material & 0xff) << 8 when pk and the material pass `ptr_ok`; when pk passes `ptr_ok` and handle k
   does too: `eb[241]` := u32 at handle + 0x20, `eb[242]` := u32 at handle + 0x28; then A[k] := 0.
3. A[1] and A[2] both non-zero: return.
4. pk failing `ptr_ok`, the material failing `ptr_ok`, nc ≤ 0, nc > 64, collections failing `ptr_ok`, no
   technique-0 collection, nt = 0 or the list failing `ptr_ok`: `eb[218]` += 1, return. (`eb[219]` := low 32 bits
   of the material, set once the material passed.)
5. `eb[214]` := nt; `eb[215]` := (A[1] ≠ 0) + 2 * (A[2] ≠ 0).
6. For k = 1, then 2, only while k < nt, and only when A[k] is 0: entry pointer = list element k; failing `ptr_ok`:
   `eb[209]` += 1, next. t = the entry's texture. Not writer-safe: `eb[5]` += 1, `eb[6]` := nt + (k << 16),
   `eb[13]`, `eb[14]`, `eb[15]` := low 32 bits of p0, p1, t; when t passes `ptr_ok` also `eb[7]`, `eb[8]`,
   `eb[9]`, `eb[11]`, `eb[12]` := u32 at t + 0, 4, 8, 0xc, 0x20; next. Otherwise: type 2 with a descriptor
   passing `ptr_ok`: apply the type-2 fix-up to it; A[k] := t; `eb[217]` += 1.

## D.5 Test scenarios (plain buffers; frames built by the test)

- k40: switch bit set (only D[0]); each pointer bad; other material; second handle set; texture missing (nc 0, 65,
  no technique 0, nt 1, element 1 bad, p0 = 0 → p1, p1 bad); cache hit (no second resolve, same H); five
  materials (the fifth overwrites slot 3).
- k25: rax ≠ A; unsafe entries in each position (type 0, [+8] = 0, pointer below 0x10000); both filled; every
  refusal of step 4; nt = 2 (only k = 1); entry bad; unsafe default; type 1 / 2 / 3 defaults (fix-up only on 2).

---

# Part E. Weapon skin materials

## E.1 Purpose and caller

PS3 applies a pending weapon skin and the material swap lands; on PS4 the skin apply runs (image 0xd4c510) but
the queued material swap does not reach the submesh slots. The helper sets the new material directly with the
game's own slot setter.

Caller: `u3x_apply_pending_skins` (earlier rewrite, stays) calls it as its `after` function, right after the
game's skin apply, for each valid pending weapon object, every frame (C `x72_skins` from `n25_tick`). The C
`x87_apply` becomes `u3x_skin_material_apply(base, o);`; `x87_eq` and `x87_nm` go with it.

## E.2 Interface

`extern "C" void u3x_skin_material_apply(uintptr_t base, uint8_t *o)` (type `HelperObjectFn`). Game call:
0xe9a350 `void fn(void *slot, void *material, void *resource)`.

## E.3 Game facts

- Object o: pointer at +0x130 = draw holder D. D: pointer at +0x10 = parts, s32 at +0x18 = number of parts. Part
  (0x68 bytes): u8 at +0x60 = number of instances; instance pointers at part + k * 0x18.
- Instance I: u32 at +0xd8 (must be non-zero); pointer at +0x160 = pending swap {pointer old name, pointer new
  name} (texts); pointer at +0x1b0 = material source G; u32 at +0x324 = number of slots, pointer at +0x328 =
  slots, 0x3a0 bytes each; pointer at slot + 0x38 = the slot's current material.
- G: pointer at +0x18 = resource, pointer at +0x10 = material table TB. TB: u32 at +0 = number of materials,
  pointer at +0x10 = materials, 0x70 bytes each.
- Name of a material m: pointer at m + 0x10 → pointer at +0 → pointer at +0 = text. Null at any step = no name.

## E.4 Behaviour

Only null tests, no other pointer validation. For each part (while the parts pointer is non-null) and each
instance pointer that is non-null: skip when the u32 at +0xd8, the swap pointer or G is 0, when the resource or TB
is null, when either name is null or empty, or when the two names are equal. Find the first material of TB whose
name equals the new name (the materials pointer must be non-null); none: skip. Then for each slot (while the slots
pointer is non-null) whose current material's name equals the old name: call 0xe9a350(slot, new material,
resource). Names compare as whole texts; a missing name never equals anything. The swap record is not changed,
so a slot already switched no longer matches on later frames.

## E.5 Test scenarios (fake structures, setter recorded)

No holder; parts null; zero instances; each skip condition; equal names; new name not in the table; several
slots, only those with the old name set (in slot order); a slot whose material has no name.

---

# Behaviour to keep (looks odd, must stay)

1. A: a full list still gets its clone written; a failed check retries on every script manager init.
2. B: `sc[7]` is set even when the table was already built, so the preset never happens later. One diagnostic
   call can print two lines, so N can reach 121. The stamp 0 means "never" (fr is at least 1 when stamped).
3. C: size is the low 32 bits of rsi and is not checked when 0.
4. D: k40's cache is never invalidated (a freed material keeps its slot); slot 3 is overwritten when full, so an
   earlier batch's handle then points at another default. k25 never fills entry 0; `eb[10]` is not written;
   `eb[218]` also counts a bad packet.
5. E: no pointer checks beyond null (as the old code).

# Tables and notes for the integrator

| Values | Resolution |
|---|---|
| three co-op hero name ids (B) | string ids of "coop-drake", "coop-chloe", "coop-sullivan": computed with `u3h_crc_sid` (G23 3.9); checked, they match |
| Player class 0xa5a63e9b (B) | string id of "Player" (checked with the same rule); an id as in G41 / G48 |
| ten process bucket offsets of `h10_root` | not in this spec: see the report (judged own glue) |
| Shade move ids 0x4b5f37f8 / 0xfc353425 and the two animation names (A) | PS3 1.19 values; the PS4 executable holds neither pattern nor the names (searched). Not generated by the string id rule from the names tried. Given as identifiers (one parameter record of the move, not a table); open question for the owner |
| vectors (0,1,0,0), 0.707, −2.0 (B) | three numeric constants of the edge test, given as patterns |
| Havok magic (C) | public packfile format |

No float, no x87. Diagnostic word positions in `eb[]`, `mel[]`, `sc[]` stay; the private words of `u3x_mk02_tick`
and `u3x_k40_deftex` (old EXT_BSS statics) move to the new part's own BSS.

# Clarifications (tester, differential test against v7.212, 2026-11-14)

Confirmed by test (old and new compared on the same inputs) unless marked "by reading".

**Part A**
- The four text pointers written into each clone are the only place the Shade texts reach the game; their contents
  equal the PS3 names given in A.3. Each clone is 16-byte aligned.
- A failed source check leaves `mel[0]` at 0, so the check runs again (and `mel[3]` grows by 1) on every later init.

**Part B**
- The three hero names and "Player" give exactly 0x9ba01002, 0x9ffcfd6c, 0x75d3456f and 0xa5a63e9b with the C part's
  string id function.
- An edge test passes when the low 8 bits of its result are non-zero; higher bits are ignored (results such as 0x100
  or 0xffffffffffffff00 count as "fails").
- The test is called first with the 0.707 limit, then with the −2.0 limit; both limit vectors and the direction vector
  reach it 16-byte aligned, with the bit patterns of B.3.
- The outputs of `u3x_mk03_edge` are cleared before anything is read; the edge word and the handle word are read
  before the owner call; the owner's name is read once, right after the owner call; the return value is the owner word
  as it is in memory after the second test call.
- The diagnostic's private state starts at zero after load (the old state lived in a zero-filled BSS section). Only
  the words listed in B.4.4 are kept (the old code had three more words that were never written).
- `fr` (`sc[0]`) is read after line 1 is printed. fr, co-op index and team are printed as signed 32-bit decimals
  (for example fr 0x80000005 prints as -2147483643, team 0x80000000 as -2147483648). The "up" field is two digits, bit 1
  then bit 0.
- `u3h_nmh`'s result is used without a null test (a null table faults on the first tick that reads it: `sc[7]` 0,
  solo co-op on, "no map" off). It is called before `sc[7]` is tested, once per such tick.
- The hash walk reads the bucket count, then the bucket array pointer, then the end marker bucket[c] once before the
  walk (no call happens during the walk, so a later change cannot be seen). An end marker that equals an entry in the
  chain stops the walk at that entry; a chain that loops ends after 256 entries with 0.

**Part D**
- k25 refused default: `eb[6]` = nt **bitwise OR** (k << 16), not nt + (k << 16). The two differ when nt has bit 16 or
  bit 17 set (for example nt 0x30003, k 2: 0x30003, not 0x50003).
- k40 fill: of the 32-byte handle only bytes 0..7 (address of the texture object) and byte 0x10 (value 1) are written;
  bytes 8..0xf and 0x11..0x1f keep whatever they held. All 16 bytes of the texture object are written {2, 0, t}. Store
  order: object, handle address, loaded byte, slot material, D[2], D[4], D[5]; then the second handle, D[1]. The handle
  and the object stay 16-byte aligned.
- k25: `eb[219]` is written as soon as the material passes `ptr_ok`, before the collection checks, so a refusal at the
  collection step leaves the new `eb[219]` and adds 1 to `eb[218]`.
- k25 clear step order per entry: `eb[237]`, `eb[238]`, `eb[239]`, `eb[240]`, (`eb[241]`, `eb[242]`), then the entry
  := 0. Texture objects placed over the `eb` words give the same results in both, so reads and writes happen in that
  order.
- The writer-safe test has no alignment test: an in-range texture at an odd address is read (u32 at t, u64 at t + 8)
  and, when it passes, stored into the array; the type-2 fix-up then reads its type word at that address too.

**Part E**
- The part count (holder + 0x18) and each part's instance count (part + 0x60) are read **again at every loop step**
  (a setter call that changes them changes how many parts / instances are walked). The parts pointer, the material
  table's materials pointer and count, and the instance's slots pointer and slot count are each read once, before
  their walks.
- Counts: part count signed 32-bit (negative = none); instance count unsigned 8-bit (0x80 walks 128 entries); material
  count and slot count unsigned 32-bit (0x80000000 is a large count).
- A null slot material, or a null at any step of the name chain, is "no name" and never matches.
