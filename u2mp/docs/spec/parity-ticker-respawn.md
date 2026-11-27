# Spec: PS3 parity, the PAGE_MULTIPLAYER ticker setter and the "Respawn in [A]..." / "Tries Remaining" line

Clean-room behaviour description for two open points: `parity-d-flow1.md` 2.10 / open question 9 and
`parity-e-flow2.md` 2.10 (the PS4 way to put a text on the friends stats ticker, and its "visible" / "finished
scrolling" tests), and the respawn countdown / co-op tries line (`maintainer-tools/research/u2-parity/zip-survey.md`
section 6). It says what the game does and where, not how to code it. Addresses are PS3 1.10 (`NPUA80698` EBOOT,
TOC `0xe183b8`) unless marked PS4 (PS4 1.02 image offset = ELF virtual address of `big2-ps4_Shipping/eboot.elf`;
file offset = address + `0x4000`). All facts were read from the two executables (PS3 through headless Ghidra, PS4
through llvm-objdump and the ELF's own relocation table) and from our image's `dc1/ps4/menu.bin`. The third-party
snapshot only gave leads; where it was wrong it is said below.

Status words as in `parity-e-flow2.md`: **COVERED**, **PLATFORM**, **MISSING**.

## 1. Friends / lobby stats ticker (PAGE_MULTIPLAYER bottom strip)

### 1.1 One ticker object, not a control field

The strip is drawn by a menu control of kind 24 (PS4 class vptr `0x1007b60`, vtable starting at `0x1007b50`; the
object is only 0xb8 bytes, made by the page builder at PS4 `0x5c6bb5..0x5c6c02` with base constructor `0x57a600`).
That control holds no ticker text. Every frame it is drawn, its draw method (slot +0x18 = PS4 `0x59f0e0`) writes the
strip rectangle into a **single static ticker object** and then calls the ticker's own scroll-and-draw function on
that object:

| | PS3 1.10 | PS4 1.02 |
|---|---|---|
| ticker object | static, reached through the module TOC (flow.cpp TOC slot `-0x79ec` of the data pointer used by `0x447248` / `0x446c60`) | **static at image + `0x1138e40`** (in .bss: zero at start). Only one instruction names it: `lea rdi, [rip+...]` at `0x59f32a` (bytes `48 8d 3d 0f 9b b9 00`), just before `call 0x5d6860` at `0x59f35b` (`e8 00 75 03 00`) |
| scroll + draw entries (every frame the strip is drawn) | `0x42de40` | `0x5d6860` (rdi = ticker object, xmm0 = the page alpha from the draw descriptor `+0x18`) |
| add a text | `0x42da98` (called only by the builder `0x446c60` at `0x4470e8`) | **`0x5d6500`** (no caller left in PS4: the builder was stripped; the function itself is intact) |
| "finished scrolling" | `0x42d870` (called by `0x447248` at `0x447494`) | no function (gone with its only caller); test the fields (1.4) |
| "visible" | `0x42da40` (called by `0x447248` at `0x4475cc`) | no function; test the fields (1.5) |

The object pointer therefore never has to be looked up from the page, the menu or the control: it is the fixed
address image + `0x1138e40`. There is one ticker for the whole game; the menu.bin control only shows it.

### 1.2 Ticker object layout (same on PS3 and PS4, 32-bit floats, offsets from the object)

| Offset | Type | Meaning |
|---|---|---|
| +0x00 | f32 | strip x (screen units), written by the kind-24 control's draw every frame it draws |
| +0x04 | f32 | strip width |
| +0x08 | f32 | strip y |
| +0x0c | f32 | strip height |
| +0x10 | f32 | time of the last scroll-and-draw call, in seconds: the frame clock times 1/300 (PS4 `s32 [0x1116180+0x104]` × `0.0033333334`, constant at `0xe62068`; PS3 `s32 [0xe60a00+0xec]` × the same value) |
| +0x14 + k×0x208, k = 0..7 | f32 | entry k: start, the x of its left edge relative to the strip x |
| +0x18 + k×0x208 | f32 | entry k: text width (measured when added) |
| +0x1c + k×0x208 | char[0x200] | entry k: the text, NUL ended; an entry is in use while its first byte is not 0 |

The object spans `0x14 + 8×0x208 = 0x1054` bytes (the last entry's text ends there; an earlier version of this
line said `0x1c + 8×0x208 = 0x105c`, which counts from the first text instead of the first entry). (The third-party note's "scroll position +4" and "slide position
+0x10" are this strip width and this last-draw time.)

### 1.3 Scroll and draw, every frame the strip is drawn (PS3 `0x42de40`, PS4 `0x5d6860`)

- +0x10 is set to the current frame clock in seconds (as in 1.2).
- For each entry in use: start moves left by 40 units per second (PS4: start += −40 × the frame's delta seconds,
  `[[0x1161ee8]+0x70]+0x14`; constant −40 at `0xe62078`; PS3 +40 subtracted). If start + width is then below 0 the
  entry is freed (its first text byte set to 0). Else the text is drawn at (strip x + start, strip y + 6), clipped to
  the strip rectangle, text scale 0.6, in the ticker colour faded by the page alpha.

So the entries move only while PAGE_MULTIPLAYER (the only page with the kind-24 control) is being drawn; on other
pages they keep their place and +0x10 keeps the time of the last drawn frame.

### 1.4 Add a text: PS4 `0x5d6500` (PS3 `0x42da98`)

- **Call:** System V x86-64 (our `U2MP_GUEST`), `int fn(void *ticker, const char *text)`; rdi = image + `0x1138e40`,
  rsi = the NUL-ended text (the same text the PS3 builder makes: colour codes `|@Cxxxxxxxx|` are passed through to
  the text renderer). It returns 0 in every case (PS3 too), so the answer says nothing; whether the text went in
  can be seen from the entries' first bytes.
- **What it does:** it computes the new start as the largest of (strip width + 10) and, for every entry in use,
  (its start + its width + 150) — so a new text enters at the right edge of the strip, or queues 150 units behind
  the last text still coming in. Constants: 10 at `0xe6205c`, 150 at `0xe62060` (PS3: TOC data `0xdafd14` = 10.0,
  `0xdafcf0` = 150.0). It takes the first entry not in use (lowest k); with all 8 in use nothing is added. It copies
  the text into that entry (an unbounded C string copy, library import through the GOT slot `0x104b0a0`: the caller
  must keep the text below 0x200 bytes including the NUL; the PS3 builder's 511-character limit, `0x43b290`, keeps
  it there), stores the start, and stores the width measured with the ticker's text style (scale 0.6).
- **Thread:** it measures text with the game's font system and writes the object the per-frame draw reads, so it must
  run on the game's main thread (where the menu update / draw runs), like every other menu-side call of ours.
- **Verification bytes at `0x5d6500`** (32 bytes; the rip displacement at +0x1b is part of the image and fixed):
  `55 48 89 e5 41 57 41 56 41 54 53 48 81 ec d0 00 00 00 49 89 f6 49 89 fc 4c 8b 3d e1 46 a7 00 49`.
  A second check inside it, at `0x5d6535` (entry 0 in-use test): `41 8a 44 24 1c 84 c0 74 20`; and at `0x5d66b5`
  (the 0x208 entry walk starts at +0x224): `c5 fa 11 8d 1c ff ff ff 49 8d 9c 24 24 02 00 00`.
- **Verification of the object address:** the 7 bytes at `0x59f32a` above (rip+0x00b99b0f from `0x59f331` =
  `0x1138e40`) and the call at `0x59f35b` to `0x5d6860`, whose entry bytes are
  `55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec b8 00 00 00 48 89 fb`, and which stores +0x10 at `0x5d689c`
  (`c5 fa 11 4b 10`).

### 1.5 "Finished a full scroll" (PS3 `0x42d870`; on PS4 read the fields)

True when, for every entry in use, start + 150 + width is not greater than the strip width (+0x04): every queued
text, plus its 150-unit gap, has fully come in from the right edge, so the next text added would enter at the edge
and not wait behind an older one. An empty ticker counts as finished. (It does not wait for the texts to leave on
the left; those keep scrolling out by themselves.)

### 1.6 "Visible" (PS3 `0x42da40`; on PS4 read the fields)

True when |(current frame clock × 1/300) − (+0x10)| < 0.1 (PS3 constant `0xdafd08` = 0.1 as a double; clock and
scale as in 1.2). Meaning: the strip's scroll-and-draw ran within the last 0.1 s, i.e. PAGE_MULTIPLAYER with the
strip is on screen. A test made before the strip's draw in the same frame sees the previous frame's time (about
1/30 s old): still well inside 0.1.

### 1.7 How the refresh uses them (PS3 `0x447248`, rest as `parity-d-flow1.md` 2.10 and `parity-e-flow2.md` 2.10)

In the per-frame net update, after the PAGE_MULTIPLAYER page lookup succeeds: if finished (1.5) and visible (1.6)
and the clock (seconds) is past the next-refresh time, set the next-refresh time to now + 10 s and run the builder
(`0x446c60`), which ends by adding its one text with the add function (1.4). Ours: `online.cpp stats_ticker_text`
builds the text; the missing piece is only this refresh with the add call. LAN: own entry only (group E 2.10).

Status: **DONE (CHECK)** 2026-11-25. `gow3_pc/src/u2mp/ticker.cpp`: install checks every verification byte of 1.4
(else `ticker: bytes at 0x... not as expected`); `ticker_refresh()` runs from session.cpp's multiplayer page handler
(per-frame update, main thread) while PAGE_MULTIPLAYER is open: visible (1.6), finished (1.5) and the frame clock
/ 300 past the next-refresh time -> next = now + 10 s, `online::stats_ticker_text()` (not added when empty or
0x200 bytes or longer), add through `0x5d6500`. The entry it fills is compared with the placement rule (1.4) and
logged once (`ticker: first text added ...` or `ticker: add left entry ...`). Rules: `overlay_rules.cpp`, test
`u2mp-overlay-test`.

Owner report 2026-11-27: the strip stays empty on PC (PS3 scrolls the results), although the log shows the first text
added at start 770. Not explained from the code: the add places the text as PS3 does and the PS4 draw `0x5d6860` moves
it by the frame delta and draws it clipped to the strip. ticker.cpp now logs the ticker state while the strip is drawn
(every 4 s, 8 times: `ticker: t ... strip x .. y .. w .. h .., drawn ..; [k] start .. width .. "text"`), which shows
whether the entries move, sit outside the strip or are freed.

## 2. "Respawn in [A]..." and the co-op "Tries Remaining" line

### 2.1 Where PS3 shows it

The four texts `0xa251dfd5` "Respawn in [A]...", `0x61f6f20d` "Respawning", `0xb9ce5fc4` "[A] Tries Remaining" and
`0x0a7e5b3a` "Last Try" are used by the PS3 function `0x4bf4a8` (checked for the first three: no other instruction
loads them). It is the menu query for **condition 44** (PS3 query table `0xe15b50`, entry 27 at `0xe15c94`, OPD `0xdf1948`;
PS4 query table `0x105e370`, entry `{44, 0x65b830, 0}` at `0x105e5f8`; the third-party "command 45" is wrong: 45 is
`0x65b1c0`). Condition 44 is asked by the **last row of `PAGE_MULTIPLAYER_SCOREBOARD`**: in our image's
`dc1/ps4/menu.bin` the page record is at file offset `0x17b0` (id `0xf650325d`, name at `0x413c`), its control list
`{6, 0xbeb8}` at `0x4160`, and the six controls are: kind 0 flags 4 query cond 87 (header), kind 10 action 23 col 0
and col 1 (the two team lists), kind 1 (separator), kind 15 query cond 88, and **kind 0 flags 2 query cond 44** at
`0xbeb8 + 5×0x70 = 0xc0e8` (the PS3 menu.bin has the same page, MENUS.md 1). So on PS3 the line is a text row of the
in-match scoreboard, under the team lists.

### 2.2 When PS3 shows the scoreboard by itself (tracker update `0x4cbf00`, entry 0 = the scoreboard)

While the net game state is 2 (playing) and the NGM byte PS3 `+0x1ba4` is 0, PS3 computes every frame an "automatic"
flag = **the local player has no character process, or its character state record has the flag PS3 bit 15**
(`0x6b6b3c`: state property `0x1a9688ee`, the same record PS4 gets from the process's vtable +0x88; by the bit-order
mapping of the A / B flags in 2.3 that flag is PS4 state byte +0x3c bit 0x01, not checked live). Then:

- flag goes 0 -> 1: the scoreboard is shown (fade speed 1.0);
- flag goes 1 -> 0 (the player is back): the scoreboard is hidden and the "shown by button" mark is cleared;
- flag unchanged: nothing; the SELECT button (ours: touchpad) still toggles it (fade speed 2.0, with a sound).

Two more inputs (settled 2026-11-25, PS4 facts in `parity-m-features.md` section 10): when every player is out
(`0x494178`, threshold 0: "out" as soon as the player died after the last spawn or is in the A / B states;
`parity-m-features.md` 10.6) the scoreboard is shown every frame; and while the HUD notification element shows a
message (PS3 `0x8a0348`, PS4 `0x555550(0x1130008)`, 10.8) and the board was not opened by the button, the automatic
flag is forced to 0. With NGM `+0x1ba4` (PS4 u8 `+0x209c`, the film playback flag) set, the automatic flag comes from
the film controller instead: its answers to `8a87d1da` (index of the watched player, whose tracker must have died
after its last spawn) and `f498c2b5` ("following a player", must be true) (10.8).

PS4 has the exact twin of `0x6b6b3c`: `0x9766f0`, `bool fn(void)` (local character `[0x115bd20]`, query `1a9688ee`
through `0x4c4090`, true for no character / no answer, else bit 0x01 of record byte `+0x3c`; verification bytes in
`parity-m-features.md` 10.4). The bit-order mapping above is therefore confirmed by PS4 code, not only by the
bitfield rule. The query answer is the same state record the process vtable `+0x88` returns.

So on PS3 a player who dies sees the scoreboard come up with the countdown row, and it goes away on respawn (also
when the player had opened it by button: the falling edge always hides it and clears the mark).

### 2.3 What the row answers (PS3 `0x4bf4a8`; PS4 `0x65b830` does the same, intact)

Called with the query; only operation 1 (get text) answers. The answer (PS4 query `+0x40`, PS3 query word 12) is
empty by default. Rules, in order:

1. Nothing unless the net clock object exists (PS4 u64 `[0x113a040]` != 0) and there is a local player record (PS4
   `[0x113b760]`, the NGM's first field).
2. Dead test. The record's character: handle `+0x10`, process id `+0x18` (PS4; the process's own id at process
   +0x44 must match). If the character resolves: it counts as dead when its died time `+0x17c` >= its spawn time
   `+0x180`, or when its state record (process vtable +0x88) has flag A (byte +0x3c bit 0x40) or flag B (byte +0x3d
   bit 0x01). Not dead -> empty. A character that does not resolve counts as dead.
3. Without the NGM co-op lives flag (PS4 u8 `NGM+0x21d0`): if flag A or B is set -> empty (in competitive play only
   the plain "died" state shows the line).
4. Game type of the current net game (DC `*net-games*` `0x3b9a067d`, index s16 `NGM+0x1ca0`, entry +0x0c): if it is
   a co-op type (4, 8, 9 or 12) and the co-op lives flag is set: only while the NGM state `NGM+0x1e80` is 2 or 5;
   tries left = s32 `NGM+0x21cc`: 0 -> empty, 1 -> "Last Try" (`0x0a7e5b3a`), 2 or more -> "[A] Tries Remaining"
   (`0xb9ce5fc4`) with [A] = the number.
5. Else: only while the NGM state is 2. Respawn time (seconds) of the net game: its respawn table (entry +0x48), if
   present, gives the value for the player's slot (s32 `NGM+0x1d6c`) when above 0, else the table's default when
   present and above 0; otherwise the entry's own value (+0x24). Respawn time not above 0 (no respawn, e.g.
   Elimination) -> empty. Seconds left = whole part of ((respawn time × 300, truncated) + died time − frame clock
   `[0x1116180+0x204]`) / 300 + 0.5, i.e. rounded to the nearest second. 0 -> "Respawning" (`0x61f6f20d`); below 0 ->
   empty; else "Respawn in [A]..." (`0xa251dfd5`) with [A] = the seconds.

Plain texts come from the text database (`0x1066660`) by id, falling back to its default text; the [A] texts are
formatted into a 0x80-byte buffer and copied to a static string (`0x14cd510`). All four ids are in our
`texts.cpp` (1142, 1230, 1231, 1407), so the lookups find them.

PS3 / PS4 field map: died / spawned `+0x168` / `+0x16c` -> `+0x17c` / `+0x180`; co-op lives flag `+0x1c50` ->
`+0x21d0`; tries `+0x1c4c` -> `+0x21cc`; state `+0x1a8c` -> `+0x1e80`; net game index `+0x18ec` -> `+0x1ca0`; slot
`+0x19b0` -> `+0x1d6c`; net-games entries 0x80 -> 0x90 bytes, slot records 0x20 (value +0x14) -> 0x28 (value
+0x18), default value +0x14 -> +0x18; clock `[0xe60a00+0x1cc]` -> `[0x1116180+0x204]`; flags A / B PS3 bits 9 / 7
of the state halfword -> PS4 +0x3c bit 0x40 / +0x3d bit 0x01.

Verification bytes, PS4 `0x65b830` (24): `55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 88 00 00 00 48 89 fb 4c`;
operation test at `0x65b855`: `83 3b 01 0f 85 f1 02 00 00`; `*net-games*` lookups at `0x65b986`:
`bf 7d 06 9a 3b e8 e0 a4 3b 00`.

### 2.4 Why PS4 never asks it, and what parity needs

`0x65b830` needs no change: it already answers exactly as PS3. It is only asked while its row is drawn, and on PS4
nothing draws `PAGE_MULTIPLAYER_SCOREBOARD` (the tracker update was removed, `gameplay.md` 1.1). Ours
(`scoreboard.cpp`) draws the page on a touchpad press and for the round / match results, so the row already answers
there; what is missing is PS3's automatic show while the local player is dead (2.2): show the scoreboard when the
automatic flag turns on, hide it when it turns off. No HUD drawing of our own is needed (the third-party overlay
drawing is not PS3 behaviour).

Status: automatic show on death **DONE (CHECK)** 2026-11-25 (`scoreboard.cpp`, `gameplay.md` 1.4 steps 5-6): the
automatic flag is PS4 `0x9766f0()` (verified bytes), forced off while a HUD notification shows and the board was not
opened by button; "every player out" (threshold 0, flags A / B from the state record) shows the board every frame;
the falling edge hides it and clears the mark. The stand-in (died / spawn test through the cond 44 fields) is gone.
Film playback: the film controller's query call is not mapped, so the board does not open by itself in a film
(`parity-m-features.md` open question 15). Still to see in a run: co-op "down" (open question 5).
The row's text: **COVERED** (intact PS4 code, our texts). No HUD drawing of our own.

## 3. Test scenarios

1. Ticker add: on PAGE_MULTIPLAYER, add one text: it enters at the right edge, moves left 40 units/s and is freed
   when its right end passes the strip's left edge; the first text byte at +0x1c is 0 afterwards.
2. Two adds in one frame: the second text's start = first start + first width + 150.
3. Finished: false right after an add while the text has not fully entered; true once start + width + 150 <= the
   strip width; true with no entries.
4. Visible: true on PAGE_MULTIPLAYER; false within a few frames of leaving it (no strip draw: +0x10 stops), and false
   in a match.
5. Refresh: with two online friends, the line is rebuilt at most every 10 s and only after the previous text has
   come in fully (no texts piling up off screen).
6. Competitive death (TDM): the scoreboard comes up on death with "Respawn in 5..." (for a 5 s respawn) counting
   down, "Respawning" at 0, and goes away when the player is back.
7. Elimination (no respawn time): the scoreboard may come up but the row stays empty.
8. Co-op with lives: on going down, "[2] Tries Remaining" style text; with one left "Last Try"; none at 0.
9. Touchpad still toggles the board while alive and while dead.

## 4. Unknown / not checked

- Meaning of PS3 state flag bit 15 (PS4 `+0x3c` bit 0x01, now confirmed by the PS4 twin `0x9766f0`): not named in
  either binary; the PS4 competitive killer-row query `0x65b490` uses it as "dead". Calling `0x9766f0` reproduces PS3
  whatever it means; check live that it is set while dead (and whether it is set while down in co-op).
- Settled 2026-11-25 (`parity-m-features.md` section 10): `0x8a0348` (HUD notification element, PS4 `0x555550`), NGM
  `+0x1ba4` (film flag, PS4 `+0x209c`) with `8a87d1da` / `f498c2b5` (film controller answers, handle
  `[0x1164080]`), the threshold of `0x494178` (0).
- The text measure call inside `0x5d6500` (`0xcc9a70`) was taken as "measure"; its exact width units were not
  traced beyond being the same units as start and strip width.
- Whether menu.bin row flags 2 (cond 44 row) / 4 (cond 87 row) change layout: not needed for parity (identical data).

## 5. Status table

| Item | PS3 | PS4 | Status |
|---|---|---|---|
| ticker add | `0x42da98` | `0x5d6500` (uncalled, intact), object image + `0x1138e40` | DONE (`ticker.cpp`), CHECK |
| ticker finished scrolling | `0x42d870` | none: fields +0x04, +0x14/+0x18/+0x1c per entry | DONE (`overlay_rules.cpp`) |
| ticker visible | `0x42da40` | none: field +0x10 vs `[0x1116180+0x104]`/300 | DONE (`overlay_rules.cpp`) |
| ticker refresh (10 s, finished, visible) | `0x447248` | none | DONE (`ticker.cpp`, from session.cpp page_handler), CHECK |
| respawn / tries row (cond 44) | `0x4bf4a8` | `0x65b830` intact | COVERED |
| scoreboard shown by itself while dead | `0x4cbf00` with `0x6b6b3c` | `scoreboard.cpp` calls the exact PS4 twin `0x9766f0`, notification override `0x555550`, "every player out" | DONE, CHECK (film path open, `parity-m-features.md` open question 15) |
