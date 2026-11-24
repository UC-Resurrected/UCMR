# G60. Round screen: slot state machine and page overlay (plus the film HUD fade setter)

Describer's spec (reimpl2). Old code: `u3x_fixes.cpp` v7.212, the block "fourth split, part 12 (the round results
overlay)" (278 code lines) and, as a section of its own (section 7), the leftover `u3x_rp_hud_fade` of the film block
(8 lines). Nothing of this group is left in C except the one-line wrapper `r19_frame`.

## 1. Purpose and callers

In a multiplayer match the game keeps, per local screen, a small "round screen" object with four slots. Each slot
is a menu page that is laid over the running game (scoreboard, RESULTS page, ...), with an on/off flag and an alpha
that fades in and out. The PS4 game still has the object, its constructor and the function that switches one slot,
but no longer the two functions that (a) decide every frame which slots are on and move the alphas and (b) draw the
page of the highest visible slot. This group supplies both, run once per frame for both screens from the game's
menu draw.

Call chain: the hook dispatcher of the C part (hook kind 77, at the game's menu draw call, image 0x571235) calls the
C wrapper `r19_frame(base, s)`, which is `return u3x_r19_frame(base, s);`. `u3x_r19_frame` calls
`u3x_r19_callees`, `u3x_r19_tick` and `u3x_r19_draw`. Nothing else calls the four functions. Everything runs on the
game's main thread; no locking.

`u3x_rp_hud_fade` (section 7) is called only by the C wrapper `rp_hud_fade`, used by the film cameras
(`film_cameras.md`).

## 2. Interface

### 2.1 Functions to deliver (names and signatures unchanged, all `extern "C"`)

| Function | Signature | Caller | Part |
|---|---|---|---|
| callee check | `int u3x_r19_callees(uintptr_t base)` | `u3x_r19_frame` | A |
| slot state machine | `void u3x_r19_tick(uintptr_t base, void *s, uint8_t *obj, int scr)` | `u3x_r19_frame` | A |
| per-frame entry | `int u3x_r19_frame(uintptr_t base, void *s)` | C `r19_frame` (unchanged one-line wrapper) | A |
| HUD fade setter | `void u3x_rp_hud_fade(uintptr_t base, uint32_t a, uint32_t b, uint32_t c, uint32_t i, uint8_t by)` | C `rp_hud_fade` (unchanged one-line wrapper) | A |
| overlay draw | `void u3x_r19_draw(uintptr_t base, void *s, uint8_t *obj, int scr)` | `u3x_r19_frame` | B |

Split (the old code is 278 + 8 = 286 lines, so two implementers can work at once; the parts share no private
piece and no variable):
- Part A, "state": `u3x_r19_callees`, `u3x_r19_tick`, `u3x_r19_frame` and `u3x_rp_hud_fade` (sections 4.1, 4.2,
  4.5, 4.6, 7). Private to it: SWITCH, ALLOFF, CLEAR3, QUERY, STATE, the callee check result byte, the two "slot 3
  page set" bytes. `u3x_r19_frame` calls Part B's `u3x_r19_draw` by its signature only.
- Part B, "draw": `u3x_r19_draw` with PAGE DRAW, CHILDREN and the "valid entry" read (sections 4.3, 4.4, 3.5).
  Private to it: the layout output buffer and the pad command list.
Both parts are tested on their own with call-recording stubs (section 6); Part A's test stubs `u3x_r19_draw`.

`s` is the helper's profile state pointer; this group only passes it on. `obj` is the round-screen object of screen
`scr` (0 or 1). `u3x_r19_draw` does not use `s`.

Private pieces of the old file that are part of this group and go away with it (the new parts write what they need
themselves, from sections 3 to 5): the identity transform, the transform combine, the "valid virtual entry" read, the
children draw, the page draw, the slot switch with its hold-back switch, the "all four off" store, the process state
id read, the query sender, the dead test, the "menu page open" test, the net-game-process resolve, the three literal
lists (pad commands, callee addresses, callee first words) and the private variables of 2.3.

### 2.2 Functions of other groups and of the C part that are called

| Function | Signature | What it does | Owner assumed |
|---|---|---|---|
| `u3h_pm_type` | `int32_t u3h_pm_type(uintptr_t base, void *s)` | C export, forwards to `u3x_pm_type`: the game type number of the current net game (pure query) | G30 (4.2) |
| `u3h_pm_coop` | `int u3h_pm_coop(uintptr_t base, void *s)` | C export, forwards to `u3x_pm_coop`: non-zero when the current net game is a co-op game (pure query) | G30 (4.3) |
| `u3h_ol01_tick` | `void u3h_ol01_tick(uintptr_t base)` | C part: the online layer's per-frame tick | C part (stays C; not in a reimpl2 group) |
| `u3h_rc_auto_tick` | `void u3h_rc_auto_tick(uintptr_t base)` | C part: the automatic sign-in after a drop and a pending Connect, per frame | C part (stays C; private) |
| `u3x_rp_h` | `uint8_t *u3x_rp_h(const uint8_t *handle)` | object of a handle {u64 pointer to an object slot, u32 id at +8}: the object in the slot when slot pointer, id and object are non-zero and the object's u32 at +0x7c equals the id, else null | film helpers (`film_helpers.md`, G02) |
| `u3x_rp_dead` | `int u3x_rp_dead(const uint8_t *record)` | non-zero when the s32 at record + 0x3a0 is less than the s32 at record + 0x39c (the old group had its own identical copy) | film helpers (G02) |
| `u3h_crc_sid` | `uint32_t u3h_crc_sid(const char *name)` | the string id of a name (G23 3.9: 32-bit CRC, polynomial 0x04C11DB7, most significant bit first, start 0, no final inversion); pure | C part; rule in G23 |
| `u3h_ext`, `ext_eb()` | CONVENTIONS.h | the diagnostic / switch words eb[] | shared tools |

The old code read "second local player present" itself (3.3); `u3x_split_screen(base)` of G31's list is the same
read, but the multiplayer-active byte is tested first here and the read must not happen when it is 0 (not
observable; either way is fine).

### 2.3 Private variables (U3X_BSS, zero at load, never reset)

- callee check result: one byte, 0 = not checked yet, 1 = all callees as expected, 2 = at least one differs.
- "slot 3 page set" flag: one byte per screen (two).
- layout output buffer: 0x320 bytes, 16-byte aligned (static because of its size; contents never read back apart
  from its first 16 bytes right after the call that fills it).

## 3. Game facts

All addresses are image offsets (add `base`). Calls use the System V AMD64 convention with integer / pointer
arguments only; no call of this group passes a float in a register.

### 3.1 The round-screen object (PS4 game data, 0x74 bytes per screen)

Object of screen k at image 0x17dd670 + 0x74 · k (k = 0, 1).

| Offset | Type | Meaning |
|---|---|---|
| +0x00 | u8 | "dead" flag of the screen's player (written by this group; the HUD reads it) |
| +0x08 + 24·n | slot n, n = 0..3 | see below |
| +0x68 | s32 | the round state seen by the previous run of the state machine |
| +0x6c | u8 | "results are ready" (read only here) |

Slot (24 bytes): +0 u8 on flag; +4 f32 alpha (0 .. 1); +8 f32 fade speed; +12 u8 "dims the background"; +16 u32 id
of the slot's menu page. So: on flags at object +0x08, +0x20, +0x38, +0x50; alphas at +0x0c, +0x24, +0x3c, +0x54;
speeds at +0x10, +0x28, +0x40, +0x58; dim bytes at +0x14, +0x2c, +0x44, +0x5c; page ids at +0x18, +0x30, +0x48,
+0x60.

Slot 0 is the scoreboard the player toggles with Select, slot 2 the RESULTS page, slot 3 the scoreboard of game type
10; slot 1 is never switched on here. The game's constructor sets the page ids of slots 0 and 2 and leaves slot 3's
page id 0; `u3x_r19_frame` sets it once (4.5) to 0xf650325d, the same page id the constructor gives slot 0.

### 3.2 Game functions

| Image offset | Arguments | Result | Meaning |
|---|---|---|---|
| 0x796640 | (object, u32 slot, u32 on (0/1), u32 sound (0/1)) | none | the game's "switch a slot": does nothing when the slot's on flag already has that value; otherwise sets the on flag and does the page / sound work. Called only through SWITCH (4.1) |
| 0x1197380 | (result box, null, u32 event id, target process, box 1, box 2, stack: box 3, box 4) | none | send an event to a process (same function as in G04 / G40). Called only through QUERY (4.1) |
| 0x658050 | (menu, 0, 0) | s32 | the menu's input state for the page update |
| 0x10dda70 | (pad record, u32 pad index, u32 value, pointer to a list of u32 command ids, u32 count) | none | sets the script override "disable-joypad-<index>-commands" to `value` for a few frames and copies the first min(count, 10) words of the list into the pad record as the commands that stay enabled |
| 0x624500 | (page, draw context, output buffer of 0x320 bytes) | none | lays the page out; writes the output buffer (first 16 bytes = the page rectangle) |
| 0x627520 | (page, null, draw context, address of the context's transform (context + 8), 16-byte-aligned copy of the rectangle) | none | draws the page's own frame |
| 0x63c430 | (null, null) | none | draws the screen fade quad using the float at image 0x16e8110 |

Virtual calls (object's first u64 = table of function pointers):

| Object | Table offset | Arguments | Meaning |
|---|---|---|---|
| menu page | 0x30 | (page, s32 1) | activate |
| menu page | 0x10 | (page, pointer to the update record of 4.4 step 8) | update |
| child of a page | 0x18 | (child, null, draw context) | draw |

"Valid entry E of object O" (used for every virtual call): O passes `ptr_ok`, the u64 at O + 0 passes `ptr_ok`, and
the u64 at that table + E passes `ptr_ok`; the entry is that last value. Otherwise there is no entry.

Callee check list (4.6): the first four bytes, read as one little-endian u32, of the seven game functions
0x624500, 0x627520, 0x658050, 0x10dda70, 0x796640, 0x1197380, 0x63c430 must be 0xe5894855 (the usual
"push rbp; mov rbp, rsp" start), except 0x658050, whose first word is 0x0d8d4853. These are our own guard values
(first bytes of the functions this group calls in the supported image), not a game table.

### 3.3 Game data

| Image offset | Type | Meaning |
|---|---|---|
| 0x17cb060 | object | the net game manager, "NGM" below |
| NGM + 0x370 + 8·k | pointer | the net player record of local screen k (may be null / invalid) |
| NGM + 0x390 + 0x9c0·i | record | net player record i, i = 0..9. Record: s32 +0x39c and +0x3a0 (dead when +0x3a0 < +0x39c), s32 +0x21c, s32 +0x600 |
| NGM + 0x6808 | s32 | round state "R": 2 = the round runs, 3 = round over (results), 4 and 5 = the states after it (5 = match end / leaving) |
| NGM + 0x680c | s32 | the "mode" word of the ended match |
| NGM + 0x6810 | s32 | sub-state used in round state 5 |
| NGM + 0x6a24 | u8 | non-zero = film playback |
| NGM + 0x6cb0 | u8 | non-zero: the overlay must not dim the background |
| 0x17d5c61 | u8 | non-zero while multiplayer is active ("MP") |
| 0x17d7b06 | u8 | (0x17d7a50 + 182) second local player present |
| 0x17f2fc0 | s32 | second-player state; 3 = second local player present |
| 0x17d7ab4 | u8 | (0x17d7a50 + 100) non-zero: the Select toggle is off |
| 0x17d7aa4 | u8 | (0x17d7a50 + 84) non-zero: force the RESULTS slot on (debug setting) |
| 0x17c5b70 | u8 | read together with the film byte for the Select toggle (4.2 step B6) |
| 0x17b4a68 | u64 | must be 0 for the Select toggle |
| 0x17b4a5f | u8 | (0x17b4a48 + 0x17) non-zero: no overlay is drawn at all |
| 0x17b4a90 + 0x114 | s32 | game clock in ticks of 1/300 s |
| 0x17b4a90 + 0x11c | f32 | frame time in seconds (also copied as a 32-bit pattern, 4.4 step 8) |
| 0x1b3ee38 | handle | the net game process ("PROC" = `u3x_rp_h(address of image 0x1b3ee38)`, may be null) |
| 0x16e2fa0 | pointer | the game's pad object. Pad record of pad n = that pointer + 0x7e0 + 0x1d8·n; in a record: u32 +0x94 = buttons pressed this frame (bit 0x100000 = Select), u32 +0x9c = flags (bit 0x2 = input blocked) |
| 0x17bd080, 0x17bfa10 | object | the menu of screen 0 / of screen 1 |
| 0x17c23a0 | u32 | "current user" word of the menu system (written, 4.4 step 4) |
| 0x16e8110 | f32 (as u32) | strength of the screen fade quad (saved, set to 0x3f800000 = 1.0, restored) |

Menu object fields: u32 +0x08 non-zero = a menu page is up; u32 +0x0c = 1 while the menu is open; +0x10 = the part
the draw context points to; u64 +0x390 + 8·i, i = 0..159: the registered pages, the list ends at the first null;
f32 +0xdcc background dim amount; u8 +0xdd4 "dim requested this frame"; u8 +0xdf5 the menu's user index; u32
+0x13fc (copied into the draw context); u32 +0x2694 (written, 4.4 step 4).

Menu page fields: u32 +0x10 page id; s32 +0xac number of children; u64 +0xb0 pointer to the array of child
pointers; child: u8 +8 non-zero = it has the focus.

Process state id of a process P ("STATE(P)"): follow P + 0x80 (pointer), then + 0 (pointer), then + 0x10 (pointer),
and read the u32 at + 0x18 of the last one. When P or any of the three pointers fails `ptr_ok`, STATE is 0.
State ids used: 0x1f574fac (the string id of "Rewind") and 0x9f514949 (the string id of "Leave").

Event ids (queries to PROC, each the string id of the name given): 0x8a87d1da "get-watch-player-index",
0xf498c2b5 "get-show-hud", 0xe68ae001 "get-free-cam".

### 3.4 Float constants

0x00000000 = 0.0, 0x3f800000 = 1.0, 0xbf800000 = -1.0, 0x40000000 = 2.0, 0x40800000 = 4.0, 0x41a00000 = 20.0,
0x43960000 = 300.0, 0x44a00000 = 1280.0, 0x44340000 = 720.0.

### 3.5 Table: the pad command list (inventory section C, `r19_pad_cmds`)

Meaning: the nine joypad commands that stay enabled while an overlay page is drawn, followed by one zero word (ten
u32 in all; the game function 0x10dda70 copies ten words when the count is ten or more, and the count passed is
0x24, so all ten words including the zero are read).

Where the PS4 game holds it: nowhere. The list comes from the PS3 function this code replaces; the PS4 image has
no copy of it (six of the nine ids occur in PS4 code only as scattered single immediates, three do not occur at
all). So the values are generated: each entry is the string id (`u3h_crc_sid`, G23 3.9) of a command name. In list
order:

1. "medal-streak"
2. "look"
3. "look-press"
4. "melee-attack"
5. "melee-attack-hold"
6. "melee-attack-release"
7. "melee-attack-repeat"
8. "buddy-up"
9. Pad command 9, name unknown: 0xc1e54d53. A single id, given by value because its name is not known (the id is
   not in the PS4 image either; section 8, question 1). It is the only word of the list given by value; the other
   eight are generated from their names.
10. zero (terminator).

The names are kept as NUL-separated text in a U3X_RO array; the ten words are built in a 16-byte-aligned local or
U3X_BSS array before the call (how often they are computed is not observable).

### 3.6 Diagnostics and switch (eb[] of CONVENTIONS.h)

eb[180] frames, eb[181] state machine runs, eb[182] pages drawn, eb[183] draws refused, eb[184] id of the last page
drawn, eb[185] Select toggles, eb[186] screen-1 draws skipped, eb[187] dim requests, eb[188] slot switches held
back, eb[189] callee check failures, eb[190] bit pattern of slot 0's alpha, eb[191] slot-3 pages set. All counters
are incremented by 1 (32-bit wrap).

Switch: eb[0] bit 0x80 set (U3MP_RESULTS_SLOT0=1) = only slot 0 may be switched on (4.1 SWITCH).

## 4. Behaviour

### 4.1 Shared small operations

**SWITCH(slot, on, sound).** When slot is not 0, on is true and eb[0] has bit 0x80 set: eb[188] += 1 and nothing
else. Otherwise game call 0x796640(obj, slot, on ? 1 : 0, sound ? 1 : 0). eb[0] is read at every SWITCH.

**ALLOFF.** Four byte stores of 0 to the on flags, in this order: slot 3 (obj + 0x50), slot 0 (obj + 0x08), slot 1
(obj + 0x20), slot 2 (obj + 0x38). No game call (the flags are cleared directly; the alphas then fade out).

**CLEAR3.** Three byte stores of 0 to the on flags, in this order: slot 0, slot 1, slot 3.

**QUERY(id).** A result box of three u64 is set to zero; four argument boxes of three u64 each are zero (G04
describes the same call with 32-byte boxes; zero boxes of 32 bytes are equally good, the game only reads them).
PROC is resolved anew for every query. Game call 0x1197380(result box, null, id, PROC (also when null), box 1,
box 2, box 3, box 4), boxes 3 and 4 being the two stack arguments. Afterwards: the query "has an answer" when the
third u64 of the result box is non-zero; its value is the first u64.

**DEAD(record)** = `u3x_rp_dead(record)` non-zero.

### 4.2 `u3x_r19_tick(base, s, obj, scr)`: the slot state machine

The machine has no state of its own besides obj + 0x68 and the slots; it follows the manager's round state R. It
never writes R.

**Entry.** MP = (u8 at 0x17d5c61) non-zero. SPLIT = MP and (u8 at 0x17d7b06 non-zero or s32 at 0x17f2fc0 equal to
3). When SPLIT is false and scr is not 0: return at once (nothing counted, nothing written). Otherwise eb[181] += 1,
then R = s32 at NGM + 0x6808, read once here and used for the whole run (also for the final store), then
`u3h_pm_type(base, s)` is called (call T1, always).

**State table.** "Rewinding" = STATE(PROC) equals 0x1f574fac. Columns: the row is chosen top to bottom; "phase 1"
is what is done first, in order; then every row continues with the phase 2 of its R (second table), then with the
common tail (4.2.2).

| Row | Condition | Phase 1: calls and stores, in order |
|---|---|---|
| S12 | T1 returned 12 (whatever R is) | ALLOFF |
| S2 | R = 2 | steps B1 to B6 of 4.2.1 (dead flag, slots 3 and 0 off after a state 5, Select toggle) |
| S5 | R = 5 and not Rewinding (PROC resolved and STATE read once for this test) | when the s32 at NGM + 0x6810 is not 3 and `u3h_pm_coop(base, s)` is 0 (asked only when the s32 is not 3): SWITCH(0, on, no sound). Otherwise: `u3h_pm_type(base, s)` again; when it is 10: SWITCH(3, on, no sound); else nothing |
| SX | any other case (R not 2 and not 5, or R = 5 while Rewinding) | PROC resolved (again); md = s32 at NGM + 0x680c; SWITCH(0, on = (PROC is not null and STATE(PROC) = 0x9f514949 and md is 3 or 8), no sound). The call is always made, with on or off |

Phase 2, by the row and R:

| Came from | R | Phase 2: calls and stores, in order |
|---|---|---|
| S2 with a Select toggle this frame | 2 | SWITCH(2, off, no sound); SWITCH(3, off, no sound) |
| S2 without toggle | 2 | SWITCH(2, off, no sound); SWITCH(3, off, no sound) |
| S5 | 5 | SWITCH(2, off, no sound); then `u3h_pm_coop(base, s)`: 0 -> SWITCH(3, off, no sound); non-zero -> END-BOARD |
| S12 or SX | 3, u8 at obj + 0x6c non-zero | CLEAR3; SWITCH(2, on, no sound); SWITCH(3, off, no sound) |
| S12 or SX | 3, u8 at obj + 0x6c zero | SWITCH(2, off, no sound); SWITCH(3, off, no sound) |
| S12 or SX | 4 | SWITCH(2, off, no sound); END-BOARD |
| S12 or SX | 5 (S12, or SX while Rewinding) | SWITCH(2, off, no sound); then `u3h_pm_coop(base, s)`: 0 -> SWITCH(3, off, no sound); non-zero -> END-BOARD |
| S12 or SX | anything else (0, 1, 2 only via S12, 6, ...) | SWITCH(2, off, no sound); SWITCH(3, off, no sound) |

END-BOARD: `u3h_pm_type(base, s)`; when it is 10: SWITCH(3, on, no sound); otherwise ALLOFF.

Notes that follow from the tables and must hold: in row S5 with game type 10 and no co-op, slot 3 is switched on in
phase 1 and off again in phase 2 of the same frame; with co-op and type 10 SWITCH(3, on) is called twice. In row
S12 with R = 2 none of the steps B1 to B6 run (no dead flag update, no Select toggle). The "leave" SWITCH(0, ...) of
row SX is not made in row S12.

#### 4.2.1 Row S2: the running round (steps in order)

B1. trk = the pointer at NGM + 0x370 + 8·scr; when it fails `ptr_ok` it counts as null.

B2. Dead flag (u8 at obj + 0), film playback (u8 at NGM + 0x6a24 non-zero):
   1. store 0;
   2. QUERY(0x8a87d1da). When it has an answer: i = the low 32 bits of the value (unsigned); when MP and i <= 9 and
      DEAD(NGM + 0x390 + 0x9c0·i): store 1;
   3. QUERY(0xf498c2b5). When it has no answer or the low 8 bits of the value are 0: store 0;
   4. PROC resolved again; when STATE(PROC) = 0x1f574fac: store 0.
   All of 1 to 4 are done every time (both queries are always sent, in this order).

B3. Dead flag, no film playback: store 1 when trk is null or DEAD(trk), else store 0. (One store.)

B4. When trk is not null and MP and `u3h_pm_coop(base, s)` is non-zero (asked only when the first two hold) and the
    s32 at trk + 0x21c is 1 and the s32 at trk + 0x600 is not 0: store 0 to the dead flag.

B5. When the s32 at obj + 0x68 (previous state) is 5: SWITCH(3, off, no sound), then SWITCH(0, off, no sound).

B6. Select toggle. Only when the u8 at 0x17d7ab4 is 0 and (the film byte NGM + 0x6a24 is 0 or the u8 at 0x17c5b70
    is 0): menu = the menu of screen scr (0x17bfa10 when scr is not 0, else 0x17bd080); P = the pointer at
    0x16e2fa0; pad = P + 0x7e0 + 0x1d8·scr when P passes `ptr_ok`, else none. The toggle happens when all hold:
    there is a pad; not (menu u32 +0x08 non-zero and menu u32 +0x0c equal to 1); pad u32 +0x9c has bit 0x2 clear;
    pad u32 +0x94 has bit 0x100000 set; the u64 at 0x17b4a68 is 0. Then, in order: store the u32 0x40000000 (2.0)
    at obj + 0x10 (slot 0's speed); SWITCH(0, on = (the u8 at obj + 0x08 is 0, read now), sound); eb[185] += 1.

Phase 2 is the same with and without a toggle (see the table); the old code reached it by two paths.

#### 4.2.2 Common tail (every run that passed the entry test), in order

1. When the u8 at 0x17d7aa4 is non-zero: CLEAR3, then SWITCH(2, on, no sound).
2. QUERY(0xe68ae001). When it has an answer and the low 8 bits of the value are non-zero: ALLOFF.
3. Alpha step for the four slots, slot 0 first (4.2.3).
4. Store R (the value read at entry) as s32 at obj + 0x68.
5. eb[190] = the u32 at obj + 0x0c (slot 0's alpha pattern after step 3).

#### 4.2.3 Alpha step (float)

dt = the f32 at 0x17b4a90 + 0x11c, read once. For each slot n = 0, 1, 2, 3 in this order, with its on flag, alpha
and speed read at that moment:

- step = dt × 4.0; d = step × speed; when the on flag is 0, d is negated (sign bit flipped, also for a NaN);
- v = alpha + d;
- when v >= 0 is false (v negative or NaN): v = +0.0;
- then, when v − 1.0 >= 0 (v at least 1, or +infinity): v = 1.0;
- store v as the slot's alpha.

A v of −0.0 passes both tests and is stored as −0.0 (pattern 0x80000000).

Rounding. New code (PIPELINE.md float rule): every operation single precision on 32-bit patterns, in exactly this
grouping: `f_mul(dt, 4.0)`, `f_mul(that, speed)`, sign flip, `f_add(alpha, d)`, the two comparisons on the rounded
sum (the second one as `f_sub(v, 1.0)` compared with 0, or directly v >= 1.0: the same for every input), one store.
Old build: this whole loop ran on the x87 unit, no single-precision instruction anywhere in the group. dt × 4.0 was
computed once and stayed in a register at extended precision for all four slots; the product with the speed, the
negation, the sum with the alpha and both clamp comparisons were made on unrounded extended values; the only
rounding to 32 bits was the final store of each alpha. (The last store, slot 0's value kept in a register, also fed
eb[190]: same value as the memory.) Checked in the helper's generated assembly (v7.212): the four slot updates are
x87 operations with the 32-bit operands (speed, alpha) taken directly from memory, dt × 4.0 is held in a register
for all four slots, the negation is a sign change selected by the on flag, the two clamp tests compare unrounded
values, and each alpha is rounded only at its store. How wide "extended" really was depends on the x87 precision
control of the game's main thread (the PS4 system's default is a 53-bit mantissa); not measured.

#### 4.2.4 The machine as states and transitions

The state is the round state R the manager holds this frame (the machine never changes it); the machine's own
memory is obj + 0x68 = the R of its previous run, used for one edge only (5 -> 2). Every frame the machine does the
work of the state it finds (level-triggered), so the calls below are made on every frame in that state, except
where the previous state is named. Game type 12 overrides every state (last row). In every row the common tail
(4.2.2) follows and ends with obj + 0x68 = R.

| State (R) | Left when | Calls and stores, in order (game type not 12) |
|---|---|---|
| 2 (round runs) | the manager's R changes | B1-B4 (dead flag); only when obj + 0x68 = 5 (first frame after state 5): SWITCH(3, off), SWITCH(0, off); B6 (Select toggle: may store speed 2.0 and SWITCH(0, toggled, sound)); SWITCH(2, off); SWITCH(3, off) |
| 3 (round over), obj + 0x6c = 0 | R changes, or the game sets obj + 0x6c | SWITCH(0, LEAVE); SWITCH(2, off); SWITCH(3, off) |
| 3, obj + 0x6c != 0 (results ready) | R changes, or obj + 0x6c is cleared | SWITCH(0, LEAVE); CLEAR3; SWITCH(2, on); SWITCH(3, off) |
| 4 | R changes | SWITCH(0, LEAVE); SWITCH(2, off); END-BOARD |
| 5, process not in "Rewind" | R changes, or the process enters "Rewind" | sub-state != 3 and not co-op: SWITCH(0, on); otherwise type 10: SWITCH(3, on); then SWITCH(2, off); co-op: END-BOARD, else SWITCH(3, off) |
| 5, process in "Rewind" | R changes, or "Rewind" ends | SWITCH(0, LEAVE); SWITCH(2, off); co-op: END-BOARD, else SWITCH(3, off) |
| any other (0, 1, 6, ...) | R changes | SWITCH(0, LEAVE); SWITCH(2, off); SWITCH(3, off) |
| any R, game type 12 | the game type changes | ALLOFF; then the R-dependent part of the rows above without SWITCH(0, LEAVE), with "5" taken as the Rewind row (for R = 2: SWITCH(2, off), SWITCH(3, off), nothing of B1-B6) |

LEAVE = on when PROC is not null, STATE(PROC) = 0x9f514949 ("Leave") and the s32 at NGM + 0x680c is 3 or 8; else
off. The exact call order, the queries and every `u3h_pm_type` / `u3h_pm_coop` call (observable by the stubs) are
as in the two tables of 4.2; this table is a view of the same rules and adds none.

### 4.3 `u3x_r19_draw(base, s, obj, scr)`: which slot is drawn

1. When the u8 at 0x17b4a5f is non-zero: return.
2. Look at the slots from 3 down to 0 and take the first whose alpha a is not equal to 0.0 as a float comparison
   (+0.0 and −0.0 are skipped; a NaN alpha is "not equal" and is taken). No such slot: return.
3. When scr is not 0: eb[186] += 1, return (PS4 draws only screen 0's menus).
4. Easing. Slot on flag non-zero: e = 1 − (1 − a)², k = (−1.0) × 20.0. Flag zero: e = a², k = 1.0 × 20.0.
   Vertical offset y = (1 − e) × k. Alpha passed = a × 1.0. Horizontal offset = +0.0.
5. PAGE DRAW (4.4) with menu = the menu of screen 0 (0x17bd080), page id = the slot's u32 at +16, the alpha
   pattern, the pattern of 0.0, the pattern of y.
6. When the slot's dim byte (+12) is 0: return. When the u8 at NGM + 0x6cb0 is non-zero: return. When the menu's u8
   at +0xdd4 is 0: store e as f32 at menu + 0xdcc, store 1 at menu + 0xdd4, eb[187] += 1.
7. Return. Only this one slot is handled per call; step 6 happens also when the page draw was refused.

Rounding. New code, single precision in this grouping: on: `t = f_sub(1.0, a)`, `e = f_sub(1.0, f_mul(t, t))`,
`k = f_mul(0xbf800000, 0x41a00000)`; off: `e = f_mul(a, a)`, `k = f_mul(0x3f800000, 0x41a00000)`; then
`y = f_mul(f_sub(1.0, e), k)`, alpha = `f_mul(a, 0x3f800000)`. Old build: all x87. a was loaded from the slot; e
was kept at extended precision and used unrounded in (1 − e) × k, and only the product was rounded to 32 bits (the
y argument); the same e was rounded to 32 bits separately for the store at menu + 0xdcc; a × 1.0 was rounded on its
store. So the old y can differ from the new one in the last place; that is the accepted deviation of the float
rule. (Confirmed in the helper's generated assembly, v7.212: e is copied to memory rounded for the later dim store
while the register copy goes on unrounded into 1 − e.)

### 4.4 PAGE DRAW (private; menu, page id, alpha pattern, x pattern, y pattern)

1. When the menu's u32 at +0x08 is non-zero (a menu page is up): eb[183] += 1, return.
2. Find the page: for i = 0 .. 159 read the u64 at menu + 0x390 + 8·i; a null ends the search without a page; an
   entry that passes `ptr_ok` and whose u32 at +0x10 equals the page id is the page (first match); other entries
   are skipped.
3. No page, or the page has no valid entry 0x10 or no valid entry 0x30 (3.2): eb[183] += 1, return.
4. g = the menu's u8 at +0xdf5 (as u32). Store g as u32 at image 0x17c23a0, then as u32 at menu + 0x2694.
5. Build the draw context: 0x50 bytes, 16-byte aligned, all zero, then:
   - +0x00 u64: the address menu + 0x10;
   - +0x08: ten f32, the transform. It is the element-wise combination of two transforms. First: {1, 1, 1, 1,
     alpha, 0, 0, 1, 0, 0}. Second: {1, 1, 1, 1, 1, 0, 0, 1, x, y}. Combination rule: elements 0, 1, 2, 3, 4 and 7
     are multiplied, elements 5, 6, 8 and 9 are added. Result written: elements 0..3 = 1.0 × 1.0, element 4 = 1.0 × alpha,
     elements 5 and 6 = 0.0 + 0.0, element 7 = 1.0 × 1.0, element 8 = x + 0.0, element 9 = y + 0.0. The additions
     matter: a y of −0.0 becomes +0.0. Each element is one single-precision operation (`f_mul` / `f_add`); the old
     build did each as one x87 operation rounded at its store;
   - +0x30: four u32: 0, 0, 0x44a00000, 0x44340000 (the screen rectangle 0, 0, 1280, 720);
   - +0x40 f32: (the s32 at 0x17b4a90 + 0x114) / 300.0. New code: `f_div(f_from_int(clock), 0x43960000)`. Old
     build: x87, the integer loaded exactly, divided by 300.0 at extended precision, one rounding at the store (for
     clock values above 2^24 the new code's extra rounding of the integer can change the last place: accepted);
   - +0x44 u8: 0 (the old code first wrote "menu is open" here and cleared it again before any call);
   - +0x48 u32: the menu's u32 at +0x13fc.
6. st = game call 0x658050(menu, 0, 0) as s32; then st becomes 5 when it is 8, stays when it is 1 or 2, and
   becomes 0 for every other value.
7. Pad commands: n = 2 when g is 0, else 3. P = the pointer at 0x16e2fa0. When P passes `ptr_ok`: game call
   0x10dda70(P + 0x7e0 + 0x1d8·n, n, 2, the command list of 3.5, 0x24). Otherwise no call.
8. Virtual call: the page's entry 0x30 (read again now, called without a further check) with (page, 1).
   Update record, 16 bytes: u64 the menu; s32 st; u32 the bit pattern of the frame time (u32 at 0x17b4a90 +
   0x11c). Virtual call: the page's entry 0x10 (read again now) with (page, address of the record).
9. Zero all 0x320 bytes of the layout output buffer. Game call 0x624500(page, context, output buffer).
10. Copy the first 16 bytes of the output buffer into a 16-byte-aligned local. Game call 0x627520(page, null,
    context, context + 8, that local).
11. CHILDREN (below).
12. eb[182] += 1; eb[184] = the page id. (Also when CHILDREN refused.)

**CHILDREN (page, context).** n = the s32 at page + 0xac, taken as 0 when negative; A = the pointer at page + 0xb0.
When n > 64, or n > 0 and A fails `ptr_ok`: eb[183] += 1 and nothing else. Otherwise:
1. focus = the first child A[i] (i ascending) that passes `ptr_ok` and whose u8 at +8 is non-zero; none: null.
2. For i = 0 .. n−1 in order: the child A[i], unless it is the same pointer as focus (a null entry while there is
   no focus is skipped by the same rule), is drawn when it has a valid entry 0x18: call entry(child, null, context).
3. saved = the u32 at image 0x16e8110; store 0x3f800000 there; game call 0x63c430(null, null); when there is a
   focus and it has a valid entry 0x18: call entry(focus, null, context); store saved back at 0x16e8110.
   Step 3 runs also when n is 0.

### 4.5 `u3x_r19_frame(base, s)`

1. eb[180] += 1. `u3h_ol01_tick(base)`. `u3h_rc_auto_tick(base)`. (Every frame, whatever follows.)
2. When `u3x_r19_callees(base)` returns 0: return 0.
3. When the u8 at 0x17d5c61 is 0: return 0.
4. For screen k = 1, then k = 0: obj = image 0x17dd670 + 0x74·k.
   - When the private "slot 3 page set" flag of screen k is 0: set it to 1; then, when the u32 at obj + 0x60 is 0:
     store 0xf650325d there and eb[191] += 1.
   - `u3x_r19_tick(base, s, obj, k)`, then `u3x_r19_draw(base, s, obj, k)`.
5. Return 0 (always).

### 4.6 `u3x_r19_callees(base)`

When the private result is not 0: return 1 when it is 1, else 0 (no reads). Otherwise set it to 1, then compare all
seven functions of the list in 3.2 (no early stop): for each whose first u32 differs from the expected word, set
the result to 2 and eb[189] += 1. Return 1 when the result is 1, else 0.

## 5. State machine summary per slot (derived from 4.2, for orientation)

| Slot | Switched on when | Switched off when |
|---|---|---|
| 0 (scoreboard) | R = 2: Select pressed while it is off (with sound, speed set to 2.0). R = 5, not Rewinding, sub-state not 3, not co-op. Any SX row: the process is in state "Leave" and md is 3 or 8 | R = 2: Select pressed while it is on; first frame of R = 2 after a state 5. Any SX row otherwise. Directly (flag store): ALLOFF, CLEAR3 |
| 1 | never | ALLOFF, CLEAR3 (flag stores) |
| 2 (RESULTS) | R = 3 with obj + 0x6c set (rows S12, SX); the debug byte 0x17d7aa4 | every other frame (SWITCH(2, off) in every phase 2); ALLOFF |
| 3 (type-10 scoreboard) | game type 10 in R = 4, in R = 5 with co-op, and in row S5 phase 1 when sub-state is 3 or co-op | SWITCH(3, off) in every other phase 2; first frame of R = 2 after a state 5; ALLOFF, CLEAR3 |

With the switch eb[0] bit 0x80 the "on" calls for slots 2 and 3 are not made (counted in eb[188]); the "off" calls
and the flag stores are unchanged.

## 6. Test scenarios

Everything can be tested on a fake image with call-recording stubs (no live game needed): stubs for the seven game
functions and the three virtual entries, for `u3h_pm_type`, `u3h_pm_coop`, `u3h_ol01_tick`, `u3h_rc_auto_tick`; the
0x796640 stub should set the slot's on byte to the requested value (the alpha step reads it afterwards); the
0x1197380 stub fills the result box per event id. Compare memory (object, menu, eb[], 0x17c23a0, 0x16e8110), return
values and the order and arguments of all calls, including the contents of the draw context, the update record, the
ten command words and the 16 rectangle bytes at call time.

Part A:
1. Entry: MP 0 with scr 1 (returns, eb[181] unchanged); MP 0 with scr 0 (runs); MP 1 with each of the two
   split-screen conditions and scr 1.
2. Type 12 with R = 0, 2, 3 (obj + 0x6c 0 and 1), 4, 5 (co-op 0 / 1).
3. R = 2: film off: trk null / invalid pointer / alive / dead; co-op exception with +0x21c = 1 and +0x600 non-zero,
   and each condition failing; film on: every combination of query 1 (no answer, index 3 alive, index 3 dead, index
   10, MP 0), query 2 (no answer, 0, 1), Rewinding or not; previous state 5; Select toggle with each of its eight
   conditions failing in turn, with slot 0 on and off, on screen 1 (other menu, other pad record); switch bit 0x80.
4. R = 5: Rewinding (goes to SX); sub-state 3 / not 3 × co-op × type 10 / other.
5. R = 0, 1, 3, 4, 6: PROC null, state "Leave" with md 3, 8, 2, other state; obj + 0x6c 0 / 1 for R = 3; type 10 for
   R = 4.
6. Tail: debug byte; free-cam query answer 0 / 1 / none.
7. Alpha step: random alphas, speeds and frame times; values that cross 0 and 1; exact 0 and 1; −0.0 result; NaN
   alpha, NaN speed, infinities; on and off.
8. Frame: callee check failing on each of the seven functions (and on several: eb[189] counts each); MP 0; first
   and second frame (slot-3 page set once; not when already non-zero); order screen 1 then 0.
9. `u3x_rp_hud_fade`: random arguments (section 7).

Part B:
1. Draw: byte 0x17b4a5f set; all alphas 0 (and −0.0); each slot as the highest visible; scr 1; on / off flag;
   random alphas (bit-for-bit y, e); dim byte 0 / 1; NGM + 0x6cb0; menu + 0xdd4 already 1.
2. Page draw: a menu page up; page absent; list ending in a null before the page; an invalid non-null entry before
   the page; page with invalid entry 0x10 / 0x30; g = 0 and 3; st = 0, 1, 2, 8, 9, −1; pad pointer null / invalid;
   y of −0.0.
3. Children: n negative, 0, 1, 64, 65; invalid array; no focus; focus first / last; children without a valid
   entry; null child pointers.

## 7. `u3x_rp_hud_fade` (film block leftover, described here because no film spec has it)

`void u3x_rp_hud_fade(uintptr_t base, uint32_t a, uint32_t b, uint32_t c, uint32_t i, uint8_t by)`

Game fact: the HUD of screen 0 is at image 0x1805fa0; the u64 at HUD + 0x220 (image 0x18061c0) points to its fade
object. Fade object fields: u32 +0x04, u8 +0x08, u32 +0x0c, u32 +0x10, u32 +0x14 (a, b and c are float bit
patterns; the film cameras pass e.g. a = strength, b = 0, c = 1.0, i = 1, by = 0, see `film_cameras.md`).

Behaviour: read the pointer once (it is not checked: a null pointer faults, as before); then five stores in this
order: c at +0x14, i at +0x04, by at +0x08 (one byte), a at +0x0c, b at +0x10. No call, no result, no float
arithmetic. The C wrapper `rp_hud_fade` stays as it is.

Test: fake image with a fade object; compare the five fields and that nothing else in the object changes.

## 8. Bugs, dead code and oddities to keep; open questions

To keep (the new code must do the same):
1. Row S5 can switch slot 3 on in phase 1 and off in phase 2 of the same frame (type 10, sub-state 3, no co-op),
   and calls SWITCH(3, on) twice with co-op.
2. The count passed to 0x10dda70 is 0x24 (a byte size), not 9 or 10; the game copies ten words.
3. The context byte +0x44 ends as 0.
4. The page counts as drawn (eb[182], eb[184]) even when CHILDREN refused (eb[183] counted too).
5. The background dim (4.3 step 6) is requested even when the page draw was refused.
6. The "slot 3 page set" flags and the callee check result are never reset (not at a new match, not at shutdown).
7. PROC is resolved again for every query and every state test; the virtual entries 0x30 and 0x10 are read again
   at call time.
8. In film playback the dead flag is written up to four times in one run (the HUD runs on the same thread, so only
   the last value is seen).
9. ALLOFF and CLEAR3 write the flags directly and in the orders given; they do not go through the game's switch
   function (no page work, no sound).
10. The Select toggle works on screen 1's pad and menu, but screen 1's overlay is never drawn (eb[186]).

Open questions:
1. Pad command 9 of the list in 3.5: its name is unknown, so its id cannot be generated by the rule, and the PS4
   image does not hold the id. Eight of nine names were found by matching string ids. Until the name is found the
   integrator has to decide how the ninth word is supplied (the value is in the old list, `r19_pad_cmds` entry 9 of
   inventory section C; it is deliberately not written here).
2. The command names of 3.5 and the event / state names of 3.3 were found by matching their string ids against
   candidate names; a different name with the same id would give the same word, so this does not affect behaviour.
3. The argument boxes of 0x1197380 are 24 bytes in this group's old code and 32 bytes in G04's description; the
   game's read size was not checked.
4. What 0x17c5b70, 0x17b4a68 and menu + 0x13fc mean is not known (they are only read / copied).

## 9. Completion check (second describer, 2026-11-14)

The whole spec was checked statement by statement against the old code (v7.212 `u3x_fixes.cpp`, the block "fourth
split, part 12" and `u3x_rp_hud_fade`) and its callers (`r19_frame`, hook kind 77; `rp_hud_fade`, film cameras):
every call, argument, store order, counter, condition and float grouping above agrees. Added: the split into parts
A / B (2.1), the state / transition view (4.2.4), the assembly checks of the float rounding (4.2.3, 4.3).

- The eight command names of 3.5 and the names "Rewind", "Leave", "get-watch-player-index", "get-show-hud",
  "get-free-cam" were recomputed with the string id rule of G23 3.9 and give exactly the old words.
- Command 9 (open question 1) is still unknown: about 250 further candidate names (buttons, pause / start / select,
  scoreboard, camera, weapon, chat and the press / hold / release / repeat forms of the known names) do not give the
  old word. The PS4 image holds no copy of the list: the word of command 9 does not occur in it at all, and the
  first command's word occurs only twice, never followed by the second. So the integrator must decide: either the
  ninth word is supplied as an identified constant with the open question attached, or the owner finds the name.
- The inventory lists five game functions for G60; the old code calls seven (also 0x796640 and 0x1197380, both
  reached only through SWITCH and QUERY). The spec covers all seven.
- No strays: every private helper of the block is part of this spec (2.1 list); `u3x_rp_h` and `u3x_rp_dead` are
  film helpers (`film_helpers.md`), `u3h_crc_sid` is the C export at the C part's string id function.
- `u3x_rp_hud_fade` is described by no film spec (`film_helpers.md` item 9 and `film_cameras.md` item 8 say so), so
  section 7 stays here.

## Clarifications

Added by the tester (2026-11-14). Facts only, confirmed by the old-versus-new test (`reimpl2\G60\test_g60.cpp`,
v7.212 against part_a.cpp / part_b.cpp). Every statement of sections 3 to 8 that the test checks holds; the old code
was built twice for it: with single-precision floats (to compare the logic exactly) and as the shipped helper builds
it (x87, `-mno-sse`, -O2; run with the x87 precision control at 53 and at 64 bits) to measure the float rule.

1. Pad command list (3.5, open question 1). The eight names give exactly the old first eight words under the string
   id rule of G23 3.9; the new code's named ninth id equals the old ninth word; the tenth word is 0. All ten words
   that reach game function 0x10dda70 are the same as before, with the count 0x24.
2. Event and state ids (3.3). The string ids of "Rewind", "Leave", "get-watch-player-index", "get-show-hud" and
   "get-free-cam" equal the old words; the slot-3 page id written by `u3x_r19_frame` is the same word as before.
3. Query boxes (4.1 QUERY, open question 3). The old code passes a result box and four argument boxes of 24 bytes
   each, all zero at the call and not overlapping; the new code the same. What the game reads from them is not
   known here (live game).
4. The second-player bytes (4.2 entry, 2.2) are read only when the MP byte is non-zero, in the old and the new code;
   not observable with stand-ins.
5. SWITCH reads eb[0] at every call (old and new): changing bit 0x80 inside any recorded call changes the next
   SWITCH in both the same way.
6. ALLOFF and CLEAR3 store in the orders of 4.1 in both; no call lies between the stores, so the order is not
   observable.
7. Row S2, the film byte (4.2.1 B2/B3 and B6): the old code reads the film byte NGM + 0x6a24 twice in one run: for
   the dead-flag steps and again at the Select gate of B6, i.e. after the two queries (0x1197380), the
   `u3h_pm_coop` call of B4 and the two SWITCH calls of B5. The new code reads it once at the start of the row and
   uses that value at B6. The results differ only when one of those calls changes the byte; with the gate byte
   0x17c5b70 set, such a change decides whether the Select toggle happens (seen in 20 one-change cases, none
   elsewhere).
8. 4.2.1 B6 after a state 5: B5 has just switched slot 0 off (the stub sets the on flag), so the Select toggle reads
   the flag as 0 and switches slot 0 on ("read now" is after B5).
9. 4.2.3, −0.0: an alpha of −0.0 with the on flag 0 and a step of 0 stores −0.0; with the on flag set (d = +0.0) it
   stores +0.0.
10. 4.2.4: long frame sequences (600 sequences of 20 to 170 frames, both screens, round states 0 to 6, game types 0,
    5, 10, 12, co-op, film, Select presses, free cam, results ready, dead flags, split screen, debug byte, switch
    bit) give the same calls and the same state after every frame in old and new.
11. Callee check (4.6): a wrong guard word on each of the seven functions, and on several, gives the same result in
    both; eb[189] counts each differing word once; once the result is set the words are not read again.
12. Float rule, alpha step (4.2.3), measured against the shipped x87 build, each operation from identical input:
    of 2.07 million alpha results in random worlds 2013 differ, the largest absolute difference among results of the
    same kind is 5.96e-8 (one last place near 0.5 to 1); results close to 0 can differ in many last places (the sum
    cancels) but never by more than that absolute amount. With realistic values (alphas 0 to 1, frame times 1/120
    to 1/20 s, speeds 0.25 to 8, and whole fades from 1 to 0 and 0 to 1) about 0.45 % differ, by at most 5.96e-8,
    and no clamp result differs. A clamp result (+0 / −0 / 1.0 / other) differs only in two situations: (a) the frame time
    × 4, or that × speed, leaves the single-precision range (above 3.4e38 or below 1.2e-38): x87 keeps the value,
    single precision gives infinity, NaN or 0 (e.g. +0.0 against −0.0, or an alpha kept against 0); (b) a fade-end
    residual: when alpha + d cancels to below one last place of d, the new code gets exactly 0 where the old one
    stored a tiny positive alpha (e.g. 2.6e-10); the old code then draws that slot's page for one more frame at an
    invisible alpha (with the page update, the pad command override and the dim request). (b) appeared in 29 of
    558,638 operations with random speeds and alphas and never with the realistic values.
13. Float rule, draw (4.3, 4.4): for alphas 0 to 1 about half of the slide offsets y differ, by at most 1.91e-6 in
    absolute terms (one last place of 20); near y = 0 the relative difference is large because 1 − e cancels (e.g. old −7.1e-14,
    new 0 when the alpha is one last place below 1 with the on flag). The dim value e at menu + 0xdcc differs by at
    most 7.96e-8 for alphas 0 to 1; with the on flag and an alpha below 2^-25, 1 − alpha rounds to 1 in single
    precision, so the new e is 0 where the old one is about 2 × alpha. The clock seconds are equal for clocks below
    2^24 ticks and differ by at most one last place above. The other transform elements are equal. The precision
    control (53 or 64 bits) did not change these figures.
