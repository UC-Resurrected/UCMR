# G58: emblem manager state machine and character binds

Clean-room behaviour description (reimpl2, describer role). It says WHAT the functions do. Everything here is an
interoperability fact or an observable effect. No float arithmetic exists anywhere in this group (see 4.0).

Neighbour specs: `G56_*.md` (emblem maths, texture records, CPU rasterizer: it owns the texture record and the
record store) and `G57_*.md` (compositor and wall emblems). Every fact of theirs that this group relies on is
restated in section 3 so this spec can be used alone.

## 1. Purpose and callers

Uncharted 3 multiplayer shows each player's emblem (four layers: icon, angle, scale, colour) on characters, on
walls and in the emblem editor. The PS4 game kept the emblem manager object but lost the part that asks for emblem
textures and hands them out. This group is that part:

- **Part A, character binds.** For each character the game is about to draw, choose the emblem texture records
  that belong to it (its own player's, its team's emblem player's, or the editor's pair) and write their textures
  into the character's render tables.
- **Part B, manager state machine.** Once per frame: decide which emblem has to be composed next (one net player
  at a time, or the editor's emblem), ask the record store for records, raise the "compose this frame" flag for the
  compositor (G57), one or two frames later hand the finished records to the player, and free records nobody
  references any more. Plus the start-up and shutdown steps.

Callers (all on the game's main thread; nothing here is called from a job worker and nothing needs a lock):

| Function | Called by | When |
|---|---|---|
| `u3x_emblem_look` | C `emb_look119` (hook kind 124: a value stub at the entry of the game's net character look function, image 0x6a5c50) | every frame for every character whose look the game refreshes |
| `u3x_emblem_team_player` | `u3x_emblem_look`; C `emb_teamnp119` = export `u3h_emb_team_player`, used by G57's `u3x_wall_emblems` | see 4.1 |
| `u3x_emblem_object_player`, `u3x_emblem_tag` | `u3x_emblem_look`; C wrappers `emb_objnp119`, `emb_tag119` (no other caller today) | see 4.2, 4.3 |
| `u3x_emblem_tick` | C `emb_tick119`, from the helper's per-frame tick (hook kind 16) | once per game frame |
| `u3x_emblem_update`, `u3x_emblem_sweep`, `u3x_emblem_players` | `u3x_emblem_tick` / `u3x_emblem_update`; C wrappers `emb_daa8_119`, `emb_sweep119`, `emb_d050_119` (no other caller) | inside the tick |
| `u3x_emblem_editor` | `u3x_emblem_update`, `u3x_emblem_netinit`; C `emb_d790_119`, which is called by C `emb_ed119` (hook kinds 125 / 126: the two emblem editor data-source functions of the game, image 0x76a610 and 0x7699e0, when their event number is 9) and C `emb_ms119` (hook kind 127: the game's match setup at image 0x72bdf1) | "the editor's emblem changed, compose it again" |
| `u3x_emblem_netinit` | C `emb_netinit119` = export `u3h_emb_netinit`, called by the multiplayer start-up (G39) | once per network start |
| `u3x_emblem_shutdown` | C `emb_shutdown119`, first step of export `u3h_net_shutdown_parts` (G39's shutdown) | once per network shutdown |

The C callers `emb_ed119` and `emb_ms119` stay as they are (the helper's own switch test, the event-number test and
a counter around one forwarding call).

## 2. Interface

All functions are `extern "C"`, `U3X_FN`. `b` / `base` is the image base. `s` is the helper's state object
(opaque here: only passed on). No function takes or returns a float.

### 2.1 Functions to deliver, part A

| Signature | Result |
|---|---|
| `uint8_t *u3x_emblem_team_player(uintptr_t b, int32_t team)` | the emblem player of a team (4.1) or null |
| `uint8_t *u3x_emblem_object_player(uintptr_t b, const uint8_t *obj)` | the net player record behind a character object (4.2) or null |
| `int u3x_emblem_tag(uintptr_t b, const uint8_t *spawner, uint32_t sid)` | 1 when the spawner carries the tag `sid`, else 0 (4.3) |
| `int u3x_emblem_look(uintptr_t b, const uint64_t *regs)` | 1 = the look is done, the game's own function body must not run; 0 = let the game's own body run (4.4) |

`regs` is the hook's saved-register block: `regs[5]` is the game function's second integer argument (rsi), the
character object. `regs[4]` (rdi) is not used. The C wrappers stay one line each and pass their arguments through
unchanged (`emb_look119` drops its `s`).

### 2.2 Functions to deliver, part B

| Signature | Result |
|---|---|
| `void u3x_emblem_players(uintptr_t b, void *s)` | player mode step (4.6); `s` unused |
| `void u3x_emblem_editor(uintptr_t b, void *s)` | editor compose request (4.7) |
| `void u3x_emblem_update(uintptr_t b, void *s)` | per-frame update (4.8) |
| `void u3x_emblem_sweep(uintptr_t b)` | free unreferenced records (4.9) |
| `void u3x_emblem_shutdown(uintptr_t b)` | 4.12 |
| `void u3x_emblem_tick(uintptr_t b, void *s)` | stray, 4.10 |
| `int u3x_emblem_netinit(uintptr_t b, void *s)` | stray, 4.11; 0 = emblems are switched off, 1 = done |
| `uint8_t *u3x_emb_np(uintptr_t b, uint32_t i)` | stray, 4.5 |
| `int u3x_emb_valid(const uint8_t *np)` | stray, 4.5 |
| `uint32_t u3x_emb_local(uintptr_t b)` | stray, 4.5 |
| `uint8_t *u3x_emb_prof(uintptr_t b, uint32_t u)` | stray, 4.5 |
| `void u3x_emb_setrec(uintptr_t b, uint8_t *np, int32_t k, uint64_t r)` | stray, 4.5 |
| `void u3x_emb_newrecs(uintptr_t b, uint32_t off, uint32_t size)` | stray, 4.5 |
| `uint64_t u3x_emblem_default_record(uintptr_t b)` | NEW name for the body of the C export `u3h_emb_alloc_default` (4.11); the C export becomes the one-line wrapper `return u3x_emblem_default_record(b);` |

All existing C wrappers stay one line and pass their arguments unchanged.

### 2.3 Functions of other groups and of the C part that this group calls

Declare them exactly so. "Record" = the texture record of 3.3.

| Declaration | Owner | What it does |
|---|---|---|
| `int u3h_emb_is_record(uint64_t p)` | C export, forwards to `u3x_emb_isrec` (record store, assumed G56) | 1 when `p` is the address of one of the 24 records of the store (inside the array and on a record boundary), else 0. Pure |
| `uint8_t *u3h_emb_records(uint8_t **flag)` | C export | returns the address of the first of the 24 records (contiguous, 0x50 bytes each) and stores in `*flag` the address of the one-byte "compose this frame" flag (3.4). `flag` must not be null. No other effect |
| `void u3h_emb_free_record(uintptr_t b, uint64_t r)` | C export, forwards to `u3x_emb_rfree` (assumed G56) | frees a record: nothing when `r` is 0, is not a record of the store, or its "used" word is 0; otherwise the record's texture is replaced by the shared empty texture (the old texture is retired by G56), "used" and "pin" become 0, ec[117] plus 1 |
| `void *u3h_emb_ralloc(uint32_t size)` | C export, forwards to `u3x_emb_ralloc` (G56) | takes the first unused record of the store, clears it, marks it used with edge length `size` and the shared empty texture, and returns it; null (and ec[122] = 1) when all 24 are used |
| `int u3x_emb_texcreate(uintptr_t b, void *rec, const uint8_t *pix, uint32_t size)` | G56 | makes a `size` x `size` texture from 4 bytes per pixel at `pix` and puts it into the record |
| `const uint8_t *u3h_emb_zero(void)` | C export | 64 zero bytes (pixels of an empty 4 x 4 texture) |
| `void u3h_emb_bind(uintptr_t b, void *s)` | C export, forwards to `u3x_emb_dcbind` (G56) | binds the game's emblem data tables (icons, scales, palette) |
| `void u3h_emb_frame_parts(uintptr_t b, void *s, int which)` | C export | `which` 0: G56 `u3x_emb_fb(b)` (make sure the shared empty texture exists); 1: `u3x_emb_retick(b)` (age and release retired textures; assumed G56); any other value: G57 `u3x_emb_pass(b, s)` (compose the records that are due when the flag of 3.4 is set) |
| `uint8_t *u3h_emb_np(uintptr_t b, uint32_t i)` | C export, forwards to `u3x_emb_np` (this spec) | 4.5 |
| `void u3h_emb_new_records(uintptr_t b, uint32_t off, uint32_t size)` | C export, forwards to `u3x_emb_newrecs` (this spec) | 4.5 |
| `void u3h_emb_set_record(uintptr_t b, uint8_t *np, int32_t k, uint64_t r)` | C export, forwards to `u3x_emb_setrec` (this spec) | 4.5 |
| `uint8_t *u3h_emb_profile(uintptr_t b, uint32_t user)` | C export, forwards to `u3x_emb_prof` (this spec) | 4.5 |
| `uint64_t u3h_emb_alloc_default(uintptr_t b)` | C export, forwards to `u3x_emblem_default_record` (this spec) | 4.11 |
| `uint8_t *u3h_ext(void)` | CONVENTIONS.h | the helper's extension block; the diagnostic words "ec" are the u32 array at `u3h_ext()` + 0x4450 (ec[n] at + 4·n; CONVENTIONS.h has no accessor for it) |

Call the `u3h_` exports where section 4 names them (not the `u3x_` function behind them), also for the functions
this spec delivers itself: the recorded call sequence of old and new is then the same, and the C part keeps one
place per service. Where section 4 says "calls `u3x_emblem_...`" the call is direct.

Net player id (also `uint32_t u3x_net_player_id(const uint8_t *np)`, an existing shared function, see G12 3.1):
low = (u32 at np + 0x1e4) AND 0xfff; when the byte at np + 0x1ec is non-zero and low is not 0, low plus 0x800; id =
low + 0x1000 · (u32 at np + 0x1e8), modulo 2^32. The old code evaluates this rule in place (a file-private copy);
the new code may call `u3x_net_player_id` (pure, same values).

## 3. Game facts

### 3.1 Game data (image offsets)

| Image offset | Type | Meaning |
|---|---|---|
| 0x17c3440 | object | the emblem manager "EM" (3.2) |
| 0x17cb060 | object | the net game manager. Pointer at +0x370 + 8·n (image 0x17cb3d0 + 8·n): the local net player record of screen n |
| 0x17cb3f0 + 0x9c0·i, i = 0..9 | record | net player record i (3.5) |
| 0x17d1868 | s32 | net state; the value 2 = a match is running ("team pass" of the looks) |
| 0x17d5c61 | u8 | "multiplayer active" (set by the network start, cleared by the shutdown) |
| 0x17bde75 | u8 | non-zero: the menu acts for user 1 |
| 0x17bf714 | u32 | the menu's user index otherwise. "Menu user" = 1 when the u8 at 0x17bde75 is non-zero, else this word (same rule as G22 3.4 / G23 3.5) |
| 0x17dd610 | pointer | array of profile objects, 0x7b00 bytes per user; null while they do not exist. The emblem of a profile: 32 bytes at profile + 0x1510 (four layers of 8 bytes) |
| 0x17dd780 | u32 | member count of the match score object (the object is at image 0x17dd778; G30 3 calls it SM, count at SM + 8) |
| 0x17e2864 + 0xa4·j | u32 | member record j of the score object: the member's net player id (SM + 0x50ec + 0xa4·j) |
| 0x17e384c + 0x130·j | s16 | first word of the stat block of member j (SM + 0x60d4 + 0x130·j): "this member's emblem represents its team" when it is 1 |
| 0x1b7f114 | s16 | the game's default stat block (used when a player has no member record); same first word |
| 0x1b3efd8 | object | the spawner tag table (3.6) |
| 0x1b37530 | pointer | (= 0x1b31128 + 0x6408, a field of the texture cache object at image 0x1b31128) the texture load job in progress, or null. u64 at job + 0x20 non-zero = a texture load is pending |
| 0x17bcf10 | pointer | (= menu / lobby scene object 0x17bba90 + 0x1480) the scene's character slot, or null; the pointer at slot + 0 is the scene's character object. Read for one diagnostic counter only |

### 3.2 The emblem manager EM (image 0x17c3440)

The object is the game's own; the game's static initialisation sets the mode byte to 1. All fields are read and
written in place.

| Offset | Type | Meaning |
|---|---|---|
| +0x00 | u8 | mode: non-zero = editor mode (the emblem being edited is composed), 0 = player mode (net players' emblems are composed) |
| +0x01 | u8 | frame toggle "T" |
| +0x02 | u8 | "compose failed, try again" "F": set by the compositor (G57) when a source texture was not ready; cleared by 4.6 |
| +0x04 | s32 | editor layer count (always written as 4) |
| +0x08 | 32 bytes | editor layers (4 x 8 bytes), a copy of the user's profile emblem |
| +0x28 | s32 | player layer count (always written as 4) |
| +0x2c | 32 bytes | player layers (4 x 8 bytes), a copy of net player record + 0x980 |
| +0x4c | u32 | the user whose emblem the editor fields hold |
| +0x50, +0x58 | u64 each | player record slots 0 and 1: records being composed for a net player (edge 0x80), or 0 |
| +0x60 | s32 | index of the net player the player slots are for; -1 = none |
| +0x68, +0x70 | u64 each | editor record slots 0 and 1 (edge 0x100), or 0 |
| +0x78 | u64 | the default record (an empty 4 x 4 texture), or 0 before the first network start |
| +0x84 | s32 | editor countdown "C" |

Slot 0 is the "board" variant of an emblem, slot 1 the "layer" (sprayed) variant; this group never needs to know
the difference. G57's compositor reads +0x02 (writes it too), +0x08 / +0x2c, +0x50..+0x70 and +0x60: per slot it
takes the player record when that slot is non-zero, else the editor record.

### 3.3 The texture record (owned by G56; the facts this group relies on)

0x50 bytes; 24 of them in one array kept by the C part (`u3h_emb_records`).

| Offset | Type | Meaning |
|---|---|---|
| +0x00 | u64 | the texture (the value a character render table takes); never 0 once the shared empty texture exists |
| +0x24 | u16 | "pin": 1 while the record is being composed or shown by the editor, 0 otherwise |
| +0x26 | u16 | "used": non-zero while the record is allocated |

"Unpin r" below means: when `r` is not 0 and `u3h_emb_is_record(r)` is non-zero, store u16 0 at r + 0x24;
otherwise nothing. "Pin r": the same with the value 1. Each is exactly one `u3h_emb_is_record` call when `r` is not
0 and none when `r` is 0.

### 3.4 The compose flag

One byte owned by the C part; its address is what `u3h_emb_records` stores through its argument. 1 = the
compositor pass of this frame (4.10 step 6) has work. "Flag := v" below means: call `u3h_emb_records` with the
address of a local pointer and store the byte through the pointer it gives (one such call per store).

### 3.5 Net player record (0x9c0 bytes)

| Offset | Type | Meaning |
|---|---|---|
| +0xe1 | u8 | in use |
| +0x1e4, +0x1e8, +0x1ec | u32, u32, u8 | the parts of the net player id (2.3) |
| +0x21c | s32 | team |
| +0x5e4 | u8 | leaving / not to be shown |
| +0x7e8, +0x7f0 | u64 each | the player's emblem records, slot 0 and slot 1 (0 = none). The game itself only ever zeroes them |
| +0x980 | 32 bytes | the player's emblem layers as received from the network |

A net player record is **valid** when the byte at +0xe1 is non-zero and the byte at +0x5e4 is zero.

### 3.6 Spawner tag table (image 0x1b3efd8)

- u8 at table + 0: non-zero when the table is built.
- 0x101 buckets of 0x10 bytes starting at table + 8. A bucket is the head of a ring: the pointer at bucket + 0 is
  the first entry (or the bucket itself, or null, when empty); each entry's pointer at +0 is the next entry; the
  ring ends when it returns to the bucket address.
- Entry: u32 at +0x10 = the tag's id, u32 at +0x14 = the tag's bit number.
- Bucket of an id = id modulo 0x101 (unsigned).
- A spawner holds its tag bits as u64 words starting at spawner + 0x20: bit number n is bit (n AND 63) of the word
  number (n >> 6).

### 3.7 Character object

| Offset | Type | Meaning |
|---|---|---|
| +0x00 | pointer | virtual table. The entry at table + 0x2c0 is a function (object) -> s32: the number of render tables of the object |
| +0x30 | u32 | class id: 0xa5a63e9b = local player, 0xf2c86c22 = networked character (same ids as G41 / G42 / G48) |
| +0x38 | pointer | the object's spawner (may be null) |
| +0x130 | pointer | draw data "M" (may be null): u32 at M + 0x18 = instance count, pointer at M + 0x10 = first instance; instance: pointer at +0 = its mesh, u8 at +0x60 = ready |
| +0x240 + 8·n, n = 0..7 | pointer | render table n (may be null); the u64 at table + 8 is the table's emblem texture |
| +0x3f8 | u32 | (networked character) the id of its net player; 0 = none |
| +0x1c68 | s32 | (local player) screen number |

### 3.8 Game functions called

All use the ordinary integer calling convention (`game_call`); none takes a float.

| Image offset | Arguments | Result | Meaning |
|---|---|---|---|
| 0x737eb0 | (address of image 0x17cb060) | s32 | the local player's team |
| 0xe48ea0 | (mesh pointer) | pointer or null | render info of a mesh; u32 at result + 0xd4 is copied to a diagnostic word only |
| 0x66c190 | (EM) | none | the game's own "store the editor's layers (EM + 8) into the profile of user EM + 0x4c" |
| 0x78fa70 | (net player record, s32 k, u64 record) | none | the game's setter of a net player's emblem record k (it stores the value at record + 0x7e8 + 8·k) |
| object's virtual 0x2c0 | (object) | s32 | 3.7 |

### 3.9 Ids

| Value | Meaning |
|---|---|
| 0x32f2d823 | id of the spawner tag that marks the character of the emblem editor's "board" preview |
| 0xa5a63e9b, 0xf2c86c22 | the two class ids of 3.7 |

These are three single ids, not a table: the PS4 image has them only as immediate operands inside its code (for
example the two class ids side by side in the game's own "player behind an object" code near image 0x35967e /
0x3596a4), not as a data table that could be read at run time. Section C of the inventory lists no table for
this group, and the group contains none.

### 3.10 Switch and diagnostic words (ec, see 2.3)

- ec[0] bit 14 (value 0x4000): emblems are switched off. "Off" below means this bit is set at the moment of the
  test.
- Counters and probes written by this group: ec[118], [119], [120], [121], [126], [127], [128], [133] to [146]
  (section 4 says when). ec[116], [117], [122] belong to G56, ec[113], [114], [124], [125], [149], [150] to G57,
  ec[129], [130] to the C callers. All u32, increments wrap.

## 4. Behaviour

### 4.0 General

- No float arithmetic: neither the x87 unit nor a single-precision instruction is used anywhere in the old code of
  this group (PIPELINE.md's float rule has nothing to apply to). The emblem layers are copied as bytes.
- No null or range checks beyond those stated. Pointers named "may be null" are tested where stated; everything
  else is trusted exactly as the old code trusts it.
- Stores are made in the order given; reads of game memory are made at the point given (a callee of another group
  or of the game may change memory between two steps: where a value is "read again", it must be).

### 4.1 `u3x_emblem_team_player(b, team)`

For i = 0, 1, ... 9 in order, with NP = the net player record i at image 0x17cb3f0 + 0x9c0·i (the address is formed
directly; the "multiplayer active" byte is NOT tested here):

1. Skip i unless NP is valid (3.5) and the s32 at NP + 0x21c equals `team`.
2. Find the state word: start with the default stat block's word (s16 at image 0x1b7f114). When the member count
   (u32 at image 0x17dd780) is not 0: compute NP's net player id and look at member records j = 0 .. count−1 in
   order; the first j whose id word (u32 at image 0x17e2864 + 0xa4·j) equals it gives the state word s16 at image
   0x17e384c + 0x130·j instead. No upper limit other than the count is applied.
3. When the state word equals 1: return NP.

No player qualifies: return null. No memory is written, nothing is called.

### 4.2 `u3x_emblem_object_player(b, obj)`

- Null when `obj` is null or the "multiplayer active" byte (image 0x17d5c61) is 0.
- Class id (u32 at obj + 0x30) 0xa5a63e9b: return the pointer at image 0x17cb060 + 0x370 + 8·n, n = the s32 at
  obj + 0x1c68 taken as a signed 64-bit index (no range check; the result may be null).
- Class id 0xf2c86c22 and the u32 at obj + 0x3f8 not 0: return the first net player record i = 0..9 (image
  0x17cb3f0 + 0x9c0·i, valid or not) whose net player id equals that u32; none: null.
- Anything else: null.

### 4.3 `u3x_emblem_tag(b, spawner, sid)`

0 when the table's built byte (u8 at image 0x1b3efd8) is 0 or `spawner` is null. Otherwise walk the ring of the
bucket of `sid` (3.6) from its first entry while the entry pointer is neither null nor the bucket address; the
first entry whose id equals `sid` decides: the result is the spawner's bit for that entry's bit number (0 or 1) and
the walk ends. No entry: 0. Nothing is written.

### 4.4 `u3x_emblem_look(b, regs)`

EM = image 0x17c3440. In this order:

1. Off (3.10): return 0. Nothing else happens.
2. OBJ = `regs[5]` as a pointer. Null: return 1 (no counter changes).
3. D = the u64 at EM + 0x78 (default record). Choose four record values R0..R3: R2 = D, R3 = 0, and
   - **match** (s32 at image 0x17d1868 equals 2): TEAM = game 0x737eb0(b + 0x17cb060); TP =
     `u3x_emblem_team_player(b, TEAM)`; OWN = `u3x_emblem_object_player(b, OBJ)` (the game call first, then these
     two). When OWN is null: R0 = R1 = D and the team player TP takes OWN's place. When the player so chosen is not
     null: R1 = the u64 at player + 0x7f0, R0 = the u64 at player + 0x7e8 (they replace D; they may be 0). So: the
     object's own player if it has one, else the team's emblem player, else the default record for both.
   - **otherwise**: TAG = `u3x_emblem_tag(b, pointer at OBJ + 0x38, 0x32f2d823)`; R1 = the u64 at EM + 0x70; R0 =
     the u64 at EM + 0x68 when TAG is 1, else R1.
4. ec[133] plus 1; ec[141] = the u32 at OBJ + 0x30; ec[143] = the low 32 bits of OBJ's address; when the s32 at
   image 0x17d1868 equals 2 (read again): ec[135] plus 1.
5. Diagnostic only: M = pointer at OBJ + 0x130. When M is not null and the u32 at M + 0x18 is not 0: I = pointer
   at M + 0x10; when I is not null, the byte at I + 0x60 is not 0 and the pointer at I + 0 is not null: X = game
   0xe48ea0(that pointer); when X is not null: ec[144] = the u32 at X + 0xd4.
6. For i = 0, 1, 2, ...:
   1. N = the object's virtual 0x2c0 (OBJ). The call is made again at the start of EVERY round (so N + 1 calls for
      a constant positive N, one call when N is 0 or negative). When N <= i (signed): the loop ends.
   2. ec[134] = N.
   3. R = R(i) when i < 4, else 0.
   4. ec[145] = 3 when R is 0; else 2 when R equals the u64 at EM + 0x68 or the u64 at EM + 0x70 (read now);
      else 1.
   5. When R is 0: R = the u64 at EM + 0x78 (read now) and ec[142] plus 1.
   6. When R is 0 or `u3h_emb_is_record(R)` is 0: next round.
   7. TBL = the pointer at OBJ + 0x240 + 8·(i AND 7). Null: ec[136] plus 1, next round.
   8. When i > 7: next round (tables beyond the eighth are never written; rounds 8 and up still do steps 1 to 7).
   9. TEX = the u64 at R + 0 (the record's texture). 0: next round.
   10. When the u64 at TBL + 8 is not 0: ec[140] plus 1. Then store TEX at TBL + 8. ec[119] plus 1; ec[138] = low
       32 bits of TEX; ec[139] = high 32 bits.
   11. SC = the pointer at image 0x17bcf10; when SC is not null and the pointer at SC + 0 equals OBJ: ec[137] plus
       1.
7. ec[118] plus 1. Return 1.

Effect in words: render table 0 gets the slot 0 record's texture, table 1 the slot 1 record's, every further table
(up to the eighth) the default record's; a record slot that is 0 falls back to the default record; a value that is
not a record of the store, or a record without texture, leaves the table as it is.

### 4.5 Small services (strays; reached through the `u3h_` exports of 2.3)

- `u3x_emb_np(b, i)`: null when the "multiplayer active" byte (image 0x17d5c61) is 0 or i > 9 (unsigned);
  otherwise the address of net player record i (image 0x17cb3f0 + 0x9c0·i).
- `u3x_emb_valid(np)`: 1 when the record is valid (3.5), else 0. `np` is not tested for null.
- `u3x_emb_local(b)`: the menu user (3.1).
- `u3x_emb_prof(b, u)`: null when the pointer at image 0x17dd610 is null; otherwise that pointer + 0x7b00·u +
  0x1510 (the 32 emblem bytes of user u's profile). `u` is not range-checked.
- `u3x_emb_setrec(b, np, k, r)`: OLD = the u64 at np + 0x7e8 + 8·k. When OLD is not 0 and
  `u3h_emb_is_record(OLD)` is non-zero: store u16 0 at OLD + 0x24, then `u3h_emb_free_record(b, OLD)`. Then, in
  every case, game 0x78fa70(np, k, r). (`r` may be 0.)
- `u3x_emb_newrecs(b, off, size)`: for k = 0, then 1: when the u64 at EM + off + 8·k is not 0, leave it; otherwise
  R = `u3h_emb_ralloc(size)`; when R is not null store u16 1 at R + 0x24; store R (or 0 when the store is full) in
  the slot.

### 4.6 `u3x_emblem_players(b, s)`: player mode step

IDX = the s32 at EM + 0x60, read first. Then Flag := 0 (3.4). Then exactly one of:

**(a) IDX is −1: look for work.**
1. J = the pointer at image 0x1b37530. When J is not null and the u64 at J + 0x20 is not 0: return (a texture load
   is pending; nothing else this frame).
2. For i = 0 .. 9: NP = `u3h_emb_np(b, i)`. Skip i when NP is null, or NP is not valid (3.5), or both of its
   emblem records (u64 at NP + 0x7e8 and u64 at NP + 0x7f0) are non-zero. The first i not skipped is taken, in
   this order:
   1. store s32 i at EM + 0x60;
   2. SRC = `u3h_emb_np(b, i)` (asked a second time, same index);
   3. copy the 32 bytes at SRC + 0x980 to EM + 0x2c;
   4. store s32 4 at EM + 0x28;
   5. Flag := 1;
   6. store u8 0 at EM + 2 (F), then u8 1 at EM + 1 (T);
   7. `u3h_emb_new_records(b, 0x50, 0x80)`;
   8. return.
3. No player taken: return (the flag stays 0).

**(b) IDX is not −1 and T (u8 at EM + 1) is 0: hand over.** For k = 0, then 1:
1. R = the u64 at EM + 0x50 + 8·k. Unpin R (3.3).
2. When F (u8 at EM + 2, read for each k) is 0: NP = `u3h_emb_np(b, IDX as u32)` (asked for each k); when NP is not
   null: `u3h_emb_set_record(b, NP, k, R)` (also when R is 0), then ec[121] plus 1; when NP is null:
   `u3h_emb_free_record(b, R)`.
   When F is not 0: `u3h_emb_free_record(b, R)`.
3. Store u64 0 at EM + 0x50 + 8·k.

After both: store s32 −1 at EM + 0x60.

**(c) IDX is not −1 and T is not 0:** store u8 0 at EM + 1. Nothing else.

### 4.7 `u3x_emblem_editor(b, s)`: editor compose request

Nothing when the mode byte (u8 at EM + 0) is 0. Otherwise ec[120] plus 1, then by the countdown C (s32 at EM +
0x84):

- **C < 1** (a new request), in this order: C := 4; store s32 −1 at EM + 0x60; U = the menu user (3.1); store U at
  EM + 0x4c; P = `u3h_emb_profile(b, U)`; when P is not null copy 32 bytes from P to EM + 8; store s32 4 at EM + 4;
  Flag := 1; store u8 1 at EM + 1; `u3h_emb_new_records(b, 0x68, 0x100)`; `u3h_emb_bind(b, s)`.
- **C >= 1** (a request is already running): C := 4. Nothing else: the layers are NOT read again and the flag is
  not touched.

### 4.8 `u3x_emblem_update(b, s)`: per-frame update

**Player mode** (u8 at EM + 0 is 0): unpin the u64 at EM + 0x68, unpin the u64 at EM + 0x70, then
`u3x_emblem_players(b, s)`. Return.

**Editor mode**, in this order:
1. U = the menu user. When the u32 at EM + 0x4c differs from U: game 0x66c190(EM); store U at EM + 0x4c; P =
   `u3h_emb_profile(b, U)`; when P is not null copy 32 bytes from P to EM + 8; store s32 4 at EM + 4;
   `u3x_emblem_editor(b, s)`.
2. Flag := 0.
3. When the u64 at EM + 0x68 is 0: return.
4. C = s32 at EM + 0x84; when C > 0 store C − 1 there (C below is the new value).
5. A = the u64 at EM + 0x68, B = the u64 at EM + 0x70.
6. When F (u8 at EM + 2) is 0:
   - C >= 2: go on with step 7.
   - C is 0: unpin A, unpin B, return.
   - any other C (1 or negative): return.

   When F is not 0: store s32 4 at EM + 0x84 (the request is kept alive) and go on with step 7.
7. Pin A, pin B. T = the u8 at EM + 1. When T is 0: Flag := 1. Store at EM + 1 the value 1 when T was 0, else 0.

### 4.9 `u3x_emblem_sweep(b)`: free records nobody references

1. RECS = `u3h_emb_records(&flag)` (one call; the flag is not touched).
2. Collect 25 slot values in this order: the u64 at EM + 0x50, + 0x58, + 0x68, + 0x70, + 0x78; then for i = 0..9
   the u64 at net player record i + 0x7e8 and + 0x7f0 (records addressed directly at image 0x17cb3f0 + 0x9c0·i,
   whatever the "multiplayer active" byte or their validity). All 25 are read before the first call of step 3.
3. For each of the 25 in that order: when the value is not 0 and `u3h_emb_is_record(value)` is non-zero, the
   record number (value − RECS) / 0x50 is marked as referenced.
4. For n = 0 .. 23 in order, R = RECS + 0x50·n: when the u16 at R + 0x26 ("used") is not 0 and n is not marked:
   store u16 0 at R + 0x24, then `u3h_emb_free_record(b, R)`.

### 4.10 `u3x_emblem_tick(b, s)`: once per frame (stray)

1. `u3h_emb_frame_parts(b, s, 0)`: always, also when emblems are off.
2. Off (3.10): return.
3. `u3h_emb_frame_parts(b, s, 1)`.
4. `u3x_emblem_sweep(b)`.
5. When the "multiplayer active" byte (image 0x17d5c61) is 0: return.
6. `u3x_emblem_update(b, s)`; `u3h_emb_frame_parts(b, s, 2)`; ec[126] plus 1.

### 4.11 `u3x_emblem_netinit(b, s)` and `u3x_emblem_default_record(b)` (strays)

`u3x_emblem_default_record(b)`: R = `u3h_emb_ralloc(4)`. When R is not null: store u16 1 at R + 0x24, then
`u3x_emb_texcreate(b, R, u3h_emb_zero(), 4)` (result ignored). Return R as a u64 (0 when the store is full).

`u3x_emblem_netinit(b, s)`:
1. Off: return 0 (the caller, G39, then runs the game's own emblem initialisation).
2. When the u64 at EM + 0x78 is 0: store `u3h_emb_alloc_default(b)` there.
3. `u3h_emb_bind(b, s)`; unpin the u64 at EM + 0x78; `u3h_emb_bind(b, s)` a second time.
4. ec[127] plus 1.
5. When the mode byte (u8 at EM + 0) is not 0: `u3x_emblem_editor(b, s)`, then ec[146] plus 1.
6. Return 1.

### 4.12 `u3x_emblem_shutdown(b)`

Off: return. Otherwise Flag := 0; for k = 0, then 1: R = the u64 at EM + 0x68 + 8·k; when R is not 0 and
`u3h_emb_is_record(R)` is non-zero: store u16 0 at R + 0x24, then `u3h_emb_free_record(b, R)`; in every case store
u64 0 at EM + 0x68 + 8·k. Then ec[128] plus 1. The player slots, the default record and the countdown are left
alone.

### 4.13 The state machine as a table

State = (mode, IDX at +0x60, T at +1, C at +0x84, F at +2, the record slots). One row is evaluated per frame by
4.8 (inside 4.10, only while multiplayer is active and emblems are on). "Compose" = G57's pass, which runs right
after the update in the same frame when the flag is 1.

**Player mode (mode byte 0).** Every frame first: unpin both editor records; Flag := 0.

| State | Leaves when | Calls and stores on the transition, in order | Next state |
|---|---|---|---|
| P-idle: IDX = −1 | a texture load job is pending | none | P-idle |
| P-idle: IDX = −1 | no load pending and some valid net player i (lowest i) lacks at least one emblem record | IDX := i; `u3h_emb_np(i)`; player layers := player + 0x980; layer count := 4; Flag := 1; F := 0; T := 1; `u3h_emb_new_records(0x50, 0x80)` (two pinned records of edge 0x80, only for empty slots). Compose runs this frame and may set F | P-wait |
| P-idle: IDX = −1 | no such player | none | P-idle |
| P-wait: IDX >= 0 (any value other than −1), T != 0 | next frame | T := 0 | P-hand |
| P-hand: IDX != −1, T = 0 | next frame, F = 0 and `u3h_emb_np(IDX)` not null | per slot k = 0, 1: unpin record; `u3h_emb_set_record(np, k, record)`; ec[121] + 1; slot := 0. Then IDX := −1 | P-idle |
| P-hand: IDX != −1, T = 0 | next frame, F != 0 or the player is gone (multiplayer inactive) | per slot: unpin record; `u3h_emb_free_record(record)`; slot := 0. Then IDX := −1 | P-idle (the same player is found again later, since it still lacks records) |

**Editor mode (mode byte not 0).** A request (4.7) can arrive at any time from the events of section 1.

| State | Leaves when | Calls and stores on the transition, in order | Next state |
|---|---|---|---|
| any, C < 1 | request | ec[120] + 1; C := 4; IDX := −1; user := menu user; editor layers := profile emblem; layer count := 4; Flag := 1; T := 1; `u3h_emb_new_records(0x68, 0x100)`; `u3h_emb_bind` | E-run, C = 4 |
| any, C >= 1 | request | ec[120] + 1; C := 4 | E-run, C = 4 |
| any | frame, the menu user differs from EM + 0x4c | game 0x66c190(EM); user := menu user; editor layers := profile emblem; layer count := 4; then a request as above; then the row below that applies | as below |
| any | frame, editor slot 0 is 0 | Flag := 0 | unchanged |
| E-run: C >= 3 before the frame (>= 2 after its decrement), F = 0 | frame | Flag := 0; C := C − 1; pin both editor records; when T = 0: Flag := 1; T := not T | E-run |
| E-run: F != 0 | frame | Flag := 0; C := C − 1 when C > 0; C := 4; pin both; when T = 0: Flag := 1; T := not T | E-run (retries every second frame until a compose succeeds; F is cleared only by G57 or by 4.6) |
| E-run: C = 2 before the frame, F = 0 | frame | Flag := 0; C := 1 | E-last |
| E-last: C = 1 before the frame, F = 0 | frame | Flag := 0; C := 0; unpin both editor records | E-idle |
| E-idle: C = 0, F = 0 | frame | Flag := 0; unpin both editor records | E-idle |
| C negative, F = 0 | frame | Flag := 0 | unchanged |

Frame by frame after a fresh request (F staying 0), counting the updates that follow it: update 1: C 4→3, T 1→0,
flag 0; update 2: C 3→2, T 0→1, Flag = 1, compose; update 3: C 2→1; update 4: C 1→0, records unpinned. The
Flag := 1 of the request itself never reaches the compositor: the compose pass runs only right after an update,
and every update clears the flag first (4.8 step 2, 4.6). It must still be stored.

**Mode changes** are made by the game (and by G39's start-up), not here. Shutdown (4.12) empties the editor slots
whatever the state; the sweep (4.9) frees any record that no slot of EM and no net player references.

## 5. Proposed split for implementers

About 315 old code lines with the strays; two independent parts:

- **Part A (character binds, about 115 lines):** `u3x_emblem_team_player`, `u3x_emblem_object_player`,
  `u3x_emblem_tag`, `u3x_emblem_look`. Needs sections 2.1, 2.3 (`u3h_emb_is_record`, `u3h_ext`, the id rule), 3.1,
  3.2 (fields +0x68, +0x70, +0x78 only), 3.3 (+0), 3.5 to 3.10, 4.1 to 4.4.
- **Part B (manager, about 200 lines):** `u3x_emb_np`, `u3x_emb_valid`, `u3x_emb_local`, `u3x_emb_prof`,
  `u3x_emb_setrec`, `u3x_emb_newrecs`, `u3x_emblem_players`, `u3x_emblem_editor`, `u3x_emblem_update`,
  `u3x_emblem_sweep`, `u3x_emblem_tick`, `u3x_emblem_netinit`, `u3x_emblem_default_record`,
  `u3x_emblem_shutdown`. Needs 2.2, 2.3, 3.1 to 3.5, 3.8 (0x66c190, 0x78fa70), 3.10, 4.5 to 4.13.

Part B calls nothing of part A and the reverse. The only thing both use is the validity rule of 3.5 (each part
writes its own).

## 6. Test scenarios

Everything can be tested on a fake image with call-recording stubs (the game functions of 3.8, a fake virtual
table for 0x2c0, the `u3h_` exports of 2.3 as recording stubs or as the real G56 functions over a fake record
array). Compare memory, results, ec words and the order and arguments of calls. Nothing needs the live game for
old-against-new; the live game is only needed to see emblems on characters.

Part A:
1. Team player: no valid player; valid player of another team; valid player of the team with member count 0 and
   default word 1 / not 1; member record found with word 1 / 0; id not among the members (default word used); two
   qualifying players (lowest index wins); guest id rule (+0x1ec set with low bits zero and non-zero).
2. Object player: null object; multiplayer inactive; local player class with screen 0 / 1 / a slot holding null;
   networked class with id 0, with a matching player (also an invalid one), with no match; another class id.
3. Tag: table not built; null spawner; empty bucket (null and self-pointing); the id as first, as later entry;
   absent; bit numbers 0, 63, 64, 200; bit clear and set.
4. Look: off; null object; match with own player (records set, one 0, both 0); match without own player but with
   a team player; with neither; no match with tag set / clear; default record 0; N = 0, 1, 2, 3, 8, 10, negative; N
   changing between rounds; a null table in the middle; a value that is not a record; a record with texture 0; a
   table already holding a texture; the scene character equal / not equal; the diagnostic chain of step 5 broken at
   each link.

Part B:
5. Services: `u3x_emb_np` with i = 0, 9, 10 and the active byte 0 / 1; `u3x_emb_prof` with a null array;
   `u3x_emb_setrec` with old value 0, a record, a non-record; `u3x_emb_newrecs` with both, one, no slot empty and
   with the store full.
6. Players step: each row of the player-mode table of 4.13, including a pending load job (job pointer null, job
   with +0x20 zero / non-zero), a player with exactly one record, all ten players served, F set at hand-over, the
   player gone at hand-over, IDX with an out-of-range value, slots holding 0 at hand-over.
7. Editor request: mode 0; C = 0, negative, 1, 4; profile array null.
8. Update: player mode with editor records set / 0 / non-records; editor mode with user change; slot 0 empty; each
   C from 5 down to −1 with F = 0 and F = 1 and T = 0 / 1; long runs of frames with requests at random frames,
   compared word for word.
9. Sweep: no record used; used and referenced from each of the 25 places; used and unreferenced (pinned and not);
   a slot holding a non-record value.
10. Tick: off; multiplayer inactive; active. Netinit: off; default record missing / present; store full; mode 0 /
    1. Shutdown: off; slots holding records, zeros, non-records.
11. Random sequences of tick / request / netinit / shutdown / look over a random fake world (thousands of frames),
    with a simple model of the compositor that sometimes sets F and of the record store.

## 7. Behaviour that looks odd and must be kept

1. 4.6 (a) asks `u3h_emb_np` twice for the chosen index.
2. 4.6 (b) calls `u3h_emb_set_record` also when the slot value is 0 (the player's record k is then set to 0), and
   calls `u3h_emb_np` and reads F again for each of the two slots.
3. 4.6: a player with one record present and one missing is taken again; `u3h_emb_new_records` fills only the
   empty manager slots, and the hand-over replaces (and frees) the record the player already had.
4. 4.7: a new editor request sets IDX to −1 without touching the player slots. Records waiting there stay (still
   pinned) and are reused by the next player pick; they are not freed by the sweep while the slots reference them.
5. 4.7 with C >= 1 does not read the profile again; 4.8 step 1 copies the profile and then 4.7 (when C < 1) copies
   it a second time.
6. 4.8 step 6: C = 1 and a negative C return without pinning or unpinning; with F set the countdown is first
   decremented and then set to 4.
7. 4.4: the virtual 0x2c0 call is repeated every round; rounds 8 and above still count ec[134], ec[145], ec[142]
   and ec[136] but never write; ec[135] reads the net state a second time; ec[145] compares against the editor
   slots even during a match.
8. 4.4 step 3: when the object has its own player, the team player is still computed (and the game's team call is
   made) although unused.
9. 4.1 and 4.9 read the net player array without testing the "multiplayer active" byte, 4.2 and `u3x_emb_np` do
   test it.
10. 4.11 binds the tables twice around the unpin of the default record; the result of the texture creation is
    ignored.
11. 4.10: the shared-empty-texture step runs even when emblems are off.
12. `u3x_emblem_players` ignores `s`; `u3x_emblem_look` ignores `regs[4]`.

## 8. Open questions

1. Ownership of `u3x_emb_isrec`, `u3x_emb_rfree`, `u3x_emb_retick` (record store, in none of the inventory's
   lists): assumed G56. Their contracts are in 2.3 in full for the first two; if G56's spec does not deliver them,
   that text is their description (`u3x_emb_retick`: each of the 16 entries of the retire queue that holds a
   texture or a map ages by 1 per call and is released by G56's `u3x_emb_release` when its age reaches 3).
2. The meaning of the s16 state word of a member's stat block (4.1) is taken from its use ("this member's emblem
   stands for its team" when 1); the game's own name for it is not known. It does not change the behaviour.

## Clarifications (tester, 2026-11-14)

Old (v7.212) against new on a fake image (`reimpl2\G58\test_g58.cpp`): no missing or wrong fact found; every point
of section 7 holds on both sides. Facts confirmed by test, in words:

1. 4.6 (b): a player that stops being valid between the pick and the hand-over still gets the records; only a null
   answer of `u3h_emb_np` (multiplayer inactive) or F set makes the hand-over free them.
2. 4.6 (b): F is read again for slot 1 after the `u3h_emb_set_record` call of slot 0; when that call sets F, slot 1
   is freed without a `u3h_emb_np` question.
3. 4.7 / 4.13: an editor request that finds the store full leaves both editor slots 0; later updates return at
   4.8 step 3 and later requests (C stays 4, so C >= 1) never ask for records again.
4. 4.11: the pin of the default record (u16 1 at R + 0x24) is stored before `u3x_emb_texcreate` is called; the
   texture creation's result changes nothing.
5. With ten players, the two editor records and the default record in use (23 records), the store does not run
   full.
6. Section 8.1 (ownership of the record store functions) cannot be settled by an old-against-new test; the test
   stands in for them by the contracts of 2.3, the same for both sides.
