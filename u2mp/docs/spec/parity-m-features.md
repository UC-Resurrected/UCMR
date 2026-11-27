# Spec: PS3 parity, group M (player-visible features: menus, loading screen, spectating, leaderboard tables, assets)

Clean-room gap survey of what a PS3 1.10 multiplayer player sees, measured against the UCMR U2 runtime
(`gow3_pc/src/u2mp`, branch `u2mp-ps3-parity-3`) and the import (`u2mp/patcher/importer`). It states what the
PS3 game shows and what game memory must hold; it does not prescribe code. Conventions as in `parity-f-party-mm.md`:
addresses without "PS3" are PS4 1.02 image offsets; "s" are real seconds; the net clock counts 1/300 s; "NGM" is the
NetGameManager (`0x113b760`, PS3 object read through the module data pointer); "the tracker overlay" is the object
at `0x104f544` (PS3: the object passed to `0x4cbf00` / `0x4cbc10` from the per-frame net display `0x447248`).

Status words: **COVERED** (same visible result already; spec / file named), **PLATFORM** (Sony plumbing replaced by
our design), **MISSING** (visible behaviour we lack or do differently; exact difference named), **CHECK** (code is
present and should match; only a run can tell).

Sources: PS3 `EBOOT.elf` (headless Ghidra, TOC `0xe183b8`, plus llvm-objdump for the parts Ghidra folded), PS4
`eboot.elf`, PS4 `dc1/ps4/menu.bin` and `net.bin`, the imported English text banks
(`recipe/work/u2data/build/main/text1/ps4/2.*`), our docs (`MENUS.md`, `PORT-FRONTEND4/5.md`, `PORT-HUD.md`,
`IMPORT.md`, `ASSET-INVENTORY.md`, `TEST-LOG.md`) and specs (`loading.md`, `hud.md`, `gameplay.md` 1, `menus.md`,
`progression.md`, `parity-a..i`). Third-party notes (`maintainer-tools/research/u2-parity/zip-survey.md`) were used
only as hints; every fact below was checked against the PS3 or PS4 binary or data.

## 1. Area status

| Area | PS3 | Ours | Status | Where |
|---|---|---|---|---|
| MP menu pages (every page of menu.bin) | 40 MP pages; same controls, conditions and labels on PS3 and PS4 (`MENUS.md` 1) | every page reachable on PS3 is reachable, except the four below | COVERED, with M7-M9 | `MENUS.md` 2-9, `parity-b/d/e/f/g` |
| Loading screen | black fill, card table in perspective, fort picture, coins, edges, rosters sliding in, band, map name, description, letter-by-letter tip | all of it, PS3 timings | COVERED (the three PS3 details of M10 settled: bars not needed on PC, `cdf70794` no visible effect, roster gate built) | `loading.md`, `PORT-FRONTEND5.md` |
| Death camera / watching others | native camera kinds (net death, follow teammate, map spot), D-pad cycles | the PS4 code still has them and runs them | COVERED (CHECK) | M5 |
| Dead-player hint line | "\|SELECT\| Scoreboard    \|D-PAD\| Camera" at the bottom while the death camera runs | drawn while the local death camera runs, PS4 facts of section 10 (2026-11-25, `scoreboard.cpp`) | CHECK; glyph = PS4 icon font code 0x0e (10.12), its picture needs one capture | M1, sections 9, 10 |
| Scoreboard / result overlay rules | four overlay entries, priority, PS3 fade and slide, open / close sounds, auto-open rules | four entries, PS3 fade rate / priority / slide, sounds, auto open (`0x9766f0`), result pages from the NGM states, dim, menu gate | CHECK; the film parts, the "Leave" test and the two toggle blocks built 2026-11-25 (10.8, 10.10, 10.11; test scenarios 11-13) | M2, M3, sections 9, 10 |
| Respawn countdown / co-op tries | in the scoreboard header (query cond 44) | the query is intact; the board now opens by itself on death | CHECK | M4 |
| Film spectating HUD | score panel and the watched player's name at the top, boosters on the left, compact camera bar | theater bar only | COVERED (N1: boosters only in the follow camera) | M6 |
| Leaderboard tables | 4 boards, global / friends filter, "my page", 11 rows, Rank / badge / name / score columns, footer Filter / Previous / Next / Go to Top | local + RPCN boards, same list object; start-over as PS3 (1.5.19) | COVERED (footer L1 / R1 pictures built from `MENUS.md` 3.8.1, not yet checked in the game) | `progression.md` 1.5.11-1.5.15, 1.5.19, `parity-a` |
| MP assets (images, icons, sounds, texts, models) | GOTY net.pak, hud.pak, hud.bnk, music stingers, MP vox, DLC voices | net-u2mp.pak (96 images), stingers, DLC voices, all texts | COVERED (two known holes, M11) | `IMPORT.md` audit, `ASSET-INVENTORY.md` |

Ranked list of the work (section 6 repeats it with files).

## 2. Spectating and the dead player's screen

### 2.0 Facts

- PS3 1.10 and PS4 1.02 both carry the camera kinds `CameraControlNetDeath`, `CameraControlFollow2`,
  `CameraControlFixed`, `CameraControlExplosionDeath`, the DC tunable block `death-cam` (PS4 `dc1/ps4/anim-net-player.bin`)
  and the script native `force-death-camera`. The PS3 net player tracker (net-player-tracker.cpp) starts a death
  camera for the local player: either a fixed camera at a spawn spot (PS3 `0x4c830c`) or a camera that follows a
  teammate (PS3 `0x4c9e98`), picked and advanced by its update (PS3 `0x4ca228`); the handle of the running camera is
  kept in the tracker (PS3 tracker `+0x15c`, the process check id at `+0x160`). The same logic is present in the PS4
  binary (camera update called from the tracker frame update `0x664bc0`; teammate camera `0x664550`; spot camera
  `0x664200`; D-pad advance from the update). No PS4 stub was found on that path.
- Loading tip `4fc75197` (net.bin tips): "Help your teammates after death by calling out enemy positions. Use
  |D-PAD| to cycle through the death cameras." The PS4 tips table still carries it.
- Text `e2186a0c`: "|SELECT| Scoreboard    |D-PAD| Camera". Present in every PS4 text bank (greek / czech / turkish
  get ours, `texts.cpp kMenu`), **referenced by no PS4 code or data**. On PS3 its only user is the tracker overlay
  draw `0x4cbc10`.
- PS3 per-frame order (`0x447248`): tracker overlay update `0x4cbf00`, tracker overlay draw `0x4cbc10`, then the
  loading screen (`0x4adaf8`, `0x4ae638`). The PS4 frame has neither tracker call; our `scoreboard.cpp` replaces them
  (`gameplay.md` 1). Correction to `functions-match-gameplay.tsv`: its pairings `0x4cbf00 -> 0x5e8e70` and
  `0x4cbc10 -> 0x5a0620` are wrong. `0x5e8e70` is the theater-camera callback handler (it compares the theater
  camera ids `0x3cf5deed`, `0xff502160`, `0x887c7d6e`, ...); `0x5a0620` is the menu system-message poller
  (`FUNCTIONS-FRONTEND-NET.md`). Both tracker functions are absent on PS4. (The two tsv rows were corrected on
  2026-11-25; a regeneration by `u2_match_report.py` must keep them.)

### M1. Dead-player hint line (PS3 `0x4cbc10` second half, fade in `0x4cbf00` end) - DONE with the section 10 facts (CHECK; glyph open, section 9)

What PS3 shows: while the local player is watching through a death camera, the line
"|SELECT| Scoreboard    |D-PAD| Camera" (text `e2186a0c`, the button glyphs drawn by the game's text printer) fades in
at the bottom of the screen, and fades out when the player respawns, the round ends or a menu takes over.

Behaviour to implement:

1. A hint fade value h in 0..1, process-wide, starts at 0; it lives with the scoreboard overlay (PS3 tracker overlay
   `+0x4`).
2. Every frame (the scoreboard frame hook, after the scoreboard work), with T = the game's own frame step in clock
   ticks (s32 `[0x1116180 + 0x110]`, 1/300 s units; section 10.1), h rises by `1.0 x T / 75` when ALL of the
   following hold, else it falls by `2.0 x T / 75`; then it is clamped to 0..1. (T / 75 is 4 x the frame's
   seconds: the line fades in at 4.0 per second, out at 8.0 per second. An earlier version of this rule said 1.0 /
   2.0 per real second; that was 4 x too slow.)
   - no menu is up: s32 `[0x1136640 + 0x1014]` (the menu manager's state word, PS3 `+0x5b4`) is 0 (section 10.2;
     the same word gates the loading-screen rosters);
   - there is a local player tracker (u64 `[0x113b760]`, not 0) and its death camera runs: PS4 `0x664b90(tracker)`
     answers true (section 10.5);
   - the NGM match state is "playing" (PS3 NGM `+0x1a8c` == 2; PS4 `ngm+0x1e80` == 2, as `scoreboard.cpp` reads);
   - NOT "every player is out" (section 10.6; threshold 0). When every player is out, the hint fades.
3. Drawing, every frame while h > 0, except when the current net game's game type is 11 (the Machinima / cinema
   type: nothing drawn), and nothing at all of the overlay (no page, no dim request, no hint) while the game is
   paused (PS4: f32 `[0x1116180 + 0x4c]` == 0.0, section 10.3):
   - overlay alpha A = 1.0 when the NGM match state is 3, 4 or 5, or the game is co-op (game types 4, 8, 9 or 12),
     or the NGM end reason (s32 `ngm+0x1e84`, PS3 `+0x1a90`, section 10.7) is 3 or 8; otherwise A = the HUD alpha,
     f32 `[0x104ed0c]` (PS3 `0xe1a660`, section 10.3). Correction: this rule used to say "game type 3 or 8" and
     "co-op 4, 8, 9"; and `0x104ed74` is not the HUD alpha but the menu's own fade (section 10.3);
   - the text is drawn white with alpha `A x h`, scale 0.8, colour-transform offset slot [5] = 3.0 (as the loading
     screen's map name, `loading.md` 3.5), centred in the rectangle x 0, y 500, w 1280, h 240 of the 1280 x 720
     menu canvas, with the same text-in-rect call and mode values (1, 1, flags 0, empty clip) the loading screen
     uses (`loading.md`, PS3 `0x4153d0` = PS4 `0x5b5450`). PS3 measures the text first (`0x4157a8`); that has no
     visible effect.
   - When TextDb has no text for `e2186a0c`, nothing is drawn.
4. PC buttons: the line shows the game's own glyphs for SELECT and D-PAD. The PS4 text printer draws `|SELECT|` as
   code 0x0e of its button icon font and `|D-PAD|` as code 0x01 (section 10.12). PS4 has no token for the touchpad
   or OPTIONS, and no PS4 text other than `e2186a0c` uses `|SELECT|`. Our scoreboard button is the touchpad by
   default (`UNCHARTED_U2MP_SCOREBOARD_BUTTON`), the PS4 place of PS3 SELECT (the PS4 menu input mapper answers
   the touchpad where PS3 answers SELECT, 10.10). So the line is right when icon 0x0e shows the touchpad. Whether
   it does is the only part left of open question 3 (one screenshot of the line in a match). Keep the text as it
   is until that capture.
5. Log once per process when the hint first becomes visible: `scoreboard: death camera hint shown`.

### M2. Scoreboard and result overlay rules (PS3 `0x4cbf00` / `0x4cbc10` first half) - rules 1-8 DONE with the section 10 facts (CHECK; the film parts, the "Leave" test and the rule 7 blocks of 10.8 / 10.10 / 10.11 built too, section 9)

The overlay has four entries (PS4 data at `0x104f544 + 8 + 0x14 x k`: u8 shown, f32 fade, f32 speed, u8 dim flag,
u32 page id). PS4 data: entry 0 = PAGE_MULTIPLAYER_SCOREBOARD (`f650325d`, speed 1.0, no dim), entry 1 =
PAGE_MULTIPLAYER_ROUND_RESULTS (`5dad5a68`, dim), entry 2 = PAGE_MULTIPLAYER_RESULTS (`3d437466`, dim), entry 3 =
PAGE_MULTIPLAYER_ROUND_RESULTS again (dim). Our `scoreboard.cpp` draws one page with its own rules.

PS3 rules that differ from ours (each a visible difference):

1. **Fade speed.** Every entry fades toward its shown flag: fade += speed x T / 75 when shown, -= when hidden,
   clamped 0..1, with T = the frame step in ticks (s32 `[0x1116180 + 0x110]`, section 10.1). T / 75 is 4 x the
   frame's seconds, so speed 1.0 = 4.0 per second (0.25 s for a full fade) and the button speed 2.0 = 8.0 per second.
   Speed is 1.0, and entry 0 gets 2.0 from the moment the player toggles it by button (and 1.0 again whenever the
   automatic rule 6 opens or closes it). Correction 2026-11-25: this rule used to say "speed per second"; the PS3
   multiplier is 1/75 per tick, not 1/300 (the built code fades 1.0 / 2.0 per real second: 4 x too slow).
2. **Which page draws.** Only the highest entry with fade > 0 draws (3 before 2 before 1 before 0), with alpha
   `A x fade` (A as in M1 3).
3. **Slide.** While its entry is shown (fading in): ease = 1 - (1 - fade)^2 and the page's y offset = -20 x (1 - ease)
   (it slides down into place from 20 units above). While hidden (fading out): ease = fade^2, y = +20 x (1 - ease)
   (it slides on downward). x is 0. Ours slides in from below and out upward.
4. **Background dim.** When the drawn entry has its dim flag (entries 1..3), the game's menu background dim target
   is requested to be `ease` for that frame (PS3 `0x421b00`). PS4 (section 10.2): if u8 `[0x1136640 + 0x1038]` is 0,
   write f32 `[0x1136640 + 0x1030]` = ease and u8 `[0x1136640 + 0x1038]` = 1; if it is already 1, do nothing (the
   first request of a frame wins). The request is made even when the page itself is not drawn because a menu is up
   (rule 2a). The menu's own per-frame update clears the byte and moves its drawn dim (f32 `+0x1034`) toward the
   target; nothing else is needed from us.
   2a (page gate, PS3 `0x423410`): the page of the top entry is drawn only while the menu state word s32
   `[0x1136640 + 0x1014]` is 0 (no menu up). Fades, slide and the dim request continue regardless.
5. **Sounds.** Toggling entry 0 by button plays a menu sound: `d9b465e1` when it opens, `2f118f31` when it closes.
   An automatic open / close plays nothing. PS4 names: `d9b465e1` = "scoreboard-open", `2f118f31` =
   "scoreboard-close", both sound records of `dc1/ps4/audio.bin`, samples in `sound1/banks/ps4/menu.bnk` (section
   10.9). PS4 still has the PS3 entry setter with this exact sound choice at `0x666b50` (uncalled).
6. **Automatic opening (entry 0) while playing (NGM state 2).** Every frame in state 2, with prev = last frame's
   automatic flag:
   - auto = PS4 `0x9766f0()` (the exact PS4 twin of PS3 `0x6b6b3c`, section 10.4): true when there is no local
     character, or the character gives no answer to state query `1a9688ee`, or its state record has bit 0x01 of
     byte `+0x3c` (PS3 bit 15). In a film (NGM `+0x209c` set, PS3 `+0x1ba4`) auto comes from the film controller
     instead (section 10.8, asked with the call form of section 10.11).
   - If the HUD notification test PS4 `0x555550(0x1130008)` (PS3 `0x8a0348`, section 10.8) is true and the board
     was not opened by button, auto = false.
   - out = "every player is out" (section 10.6).
   - If not out: auto rising (prev false, auto true) -> entry 0 shown with speed 1.0, no sound. Auto falling (prev
     true, auto false) -> entry 0 hidden with speed 1.0 and the "opened by button" mark cleared, **also when the
     player had opened it by button** (correction: this rule used to keep a board opened by button; PS3 does not;
     open question 10 settled in favour of `parity-ticker-respawn.md` 2.2).
   - If out: entry 0 shown with speed 1.0 every frame; if prev was true and auto is false, the falling-edge hide
     above still runs in the same frame (so the board flickers off for that one frame; PS3 does exactly this).
   - If the NGM state of the previous frame was 5: entries 3 and 0 hidden, mark cleared.
   - Then entries 2 and 3 are hidden (always, in state 2).
7. **Button toggle (state 2 only).** Skipped entirely while out. Otherwise, when the scoreboard button was pressed
   this frame and none of the blocks holds: entry 0's speed = 2.0, entry 0 toggled with its sound (rule 5), mark =
   the new shown value. Blocks (section 10.10): L3 held (PS3 held word bit 0x4000 = PS4 bit 0x2 of the held word
   u32 `[0x1116180 + 0x86c]`; the same bit of our raw pad word `0x14b7fb0` may be used), or the engine pause
   object's owner pointer u64 `[0x10595d8 + 0x18]` not 0 (PS3 word `0xe1eb1c + 0x10`). While a block holds, the
   press is ignored (no toggle, no sound, mark unchanged). The toggle is the PS3 SELECT button, ours
   `UNCHARTED_U2MP_SCOREBOARD_BUTTON` (touchpad).
8. **Match states (replaces our 12 s / 60 s timers).** Every frame, before rule 6:
   - Cinema game (type 11): all four shown flags cleared; then state 3 -> entry 2 shown; state 4 -> entry 3 shown;
     other states: entries 2 and 3 hidden. Nothing else.
   - State 2: rules 6 and 7.
   - State 5: entry 0 shown (speed unchanged) unless the round sub-state s32 `ngm+0x1e88` (PS3 `+0x1a94`) is 3; in
     both cases entries 2 and 3 end hidden. (PS3 sets entry 3 when the sub-state is 3 and clears it again in the
     same frame, so with sub-state 3 nothing new is shown; entry 0 keeps its flag.)
   - States 0, 1, 3, 4 (and any other): entry 0 shown only when the current task process (handle `[0x1164080]`)
     is in its state named "Leave" (id `9f514949`, test PS4 `0x9be070`) and the end reason `ngm+0x1e84` is 3 or 8,
     else hidden (section 10.8). Correction: this is not films only. In a film the task is TaskManagerReplay, in a
     net match its online twin TaskManagerOnline; both have a "Leave" state, and the online one writes NGM state 0
     on entry while the end reason stays. So when a match ends with reason 3 or 8, PS3 keeps the scoreboard
     (entry 0) up while the task is leaving (still behind the menu gate, rule 2a). Then:
     state 3 -> all four flags cleared and entry 2 (RESULTS) shown; state 4 -> all four cleared and entry 3
     (ROUND_RESULTS) shown; other states -> entries 2 and 3 hidden.
   - When a shown flag is set from 0 to 1, the entry's page is asked to lay out again (page `+0xf8` = 2, as
     `scoreboard.cpp` does). In states 3 and 4 the flags are cleared and set again every frame, so PS3 repeats that
     request every frame.
   - After the state rules: the three PS3 "request bytes" (`0xf2264c` `+0x54/+0x55/+0x56`) are debug-menu switches
     ("Force Results Scoreboard", "Force Round Results Scoreboard", "Force Wave Results Scoreboard"): always 0 in a
     retail game; do not implement. Then the query `e68ae001` to the current task process (sections 10.8, 10.11),
     every frame in every state (PS3 does not gate it on the film flag): when it is answered (reply type not 0)
     and the answer is true, all four shown flags are cleared (fades run on). Only TaskManagerReplay's RePlaying
     state answers it, so outside films nothing happens.
   - Then the fades (rule 1), the stored "previous state" (for the state-5 rule) and the hint (M1).
   Where the states come from (section 10.7): state 4 when the script event `15f1cf05` arrives with value 3 (its
   value is the round sub-state), state 3 at match end (event `bcc3b05b` or `0x648c30`), state 2 at every round
   start (sub-state and end reason reset to 0), state 5 in the leave sequence, state 0 on the exit paths and at the
   next game start. The timers and the `UNCHARTED_U2MP_SCOREBOARD_STATES` opt-in can go: a stale state 3 cannot
   draw over the lobby because the lobby is a menu (menu state word non-zero, rule 2a) and PS4 resets the state to 0
   on the way out (section 10.7).

Not a difference: the page draw itself (`gameplay.md` 1.4 "Draw the page") matches PS3 `0x423410`.

### M3. Sounds and dim are part of M2 (no separate work).

### M4. Respawn countdown and co-op tries in the scoreboard header (PS3 cond 44 query `0x4bf4a8`, PS4 `0x65b830`) - CHECK (the board now opens on death)

PS3 title text of the scoreboard page header (op 1 only), for the local player:
- competitive, NGM state 2, the net game's respawn delay above 0: "Respawn in [A]..." (`a251dfd5`) with [A] = whole
  seconds left (delay from the net game, counted from the player's death time), "Respawning" (`61f6f20d`) at 0;
- co-op with the tries counter running (NGM states 2 or 5): "[A] Tries Remaining" (`b9ce5fc4`) when 2 or more,
  "Last Try" (`0a7e5b3a`) at 1, nothing at 0;
- shown only while the local player is killed / down, or (co-op) the tries flag is set.

The PS4 query is intact (`MENUS.md` 2, cond 44) and all four texts are in the banks. So the player sees the countdown
exactly when the scoreboard page is up while dead. With our button-only scoreboard it is never seen unless the player
opens the board. M2 rule 6 fixes that. Test: die in Team Deathmatch, the board opens with the countdown and closes
on respawn; co-op: the tries line.

### M5. Death camera and cycling (native) - COVERED (CHECK)

Nothing to implement; the PS4 code is intact. To check in a run on two PCs (and in co-op): after a death in
Deathmatch the net death camera, then respawn; in Elimination and co-op, after death the camera follows a living
teammate, D-pad moves to the next teammate, the camera moves on by itself when the watched teammate dies, and falls
back to a spawn-spot camera when no teammate lives. Names over heads (`hud.md` 4.9) and markers keep working from the
death camera's position (`visibility.cpp` uses the camera position, `gameplay.md` 3). Known risk (not PS3 parity): the
camera process creation can fail when the default process heap is exhausted after long sessions; if a run shows an
assert in CreateControlFromName with the follow / spot camera, treat it as a robustness item, not parity.

### M6. Film spectating HUD (theater) - COVERED (`parity-n-remaining.md` N1)

PS3 in a film shows the normal match score panel and the watched player's name at the top and the booster icons on
the left, besides the compact theater bar. Ours draws the theater bar (`hud.md` 4.11) and the HUD parts whose PS3
gates allow film mode. To do: in a film, compare with a PS3 / RPCS3 capture whether the score panel, the watched
player's name and the boosters appear; if not, read the PS3 HUD Draw `0x8aef68` gates for flag `0x100000` (film)
and give the PS4 port the same gates. A third-party tester report names the same three parts (hint only, verify). Open question 7.

## 3. Menus: pages without a PC route and small items

Every MP page of PS4 `menu.bin` was listed and checked against our docs and code. All pages a PS3 player reaches are
covered by `MENUS.md`, `menus.md`, `parity-b/d/e/f/g` and `CINEMA.md`, except:

### M7. Mute Players (PAGE_MULTIPLAYER_MUTE_PLAYERS, from the in-match pause page) - COVERED (`parity-n-remaining.md` N2)

PAGE_PAUSE_MULTIPLAYER: Options, Mute Players (BUTTON_MP_MUTE_PLAYERS), Leave Game (-> LEAVEGAME_CONFIRM); in a film
also Render Settings, Camera, Fog, Lighting. The Mute Players page has two team lists LIST_MUTEPLAYERS0 / 1 (title
MENU_TITLE_MUTE_PLAYERS); their rows come from PS4 code that is still present (ids at `0x5ed078`, `0x5ed0cc`,
`0x5ed0fa`, in the page handler). What must be true: each list shows the players of that team (the NGM trackers,
names as on the scoreboard, `gameplay.md` 2), selecting a row toggles that player's mute in our voice layer
(`voice.cpp`, same state as the lobby popup Mute, `parity-e-flow2.md` 2.7 and `parity-h-session.md` H7). Check in a
match: rows filled, toggle heard. If rows are empty, the cause is the source list the handler reads (map it, then
spec it here).

### M8. Pages that stay unreachable on PC - owner decisions, nothing to do

- PAGE_FRIENDACTIONS (PSN friend card actions): replaced by View Profile and the popups (`MENUS.md` 3.5).
- PAGE_MULTIPLAYER_EULA, PAGE_MULTIPLAYER_MOTD (server pages; MOTD decided "none").
- PAGE_FRIENDSTATS ("COMPARE FRIENDS"): no control of PS4 menu.bin opens it and its id is in neither binary as code;
  dead on PS3 1.10 as well. The visible friend stats are the ticker (`parity-d-flow1.md` 2.10).
- PAGE_TWITTER, PAGE_TWITTER_MESSAGES: DROPPED (owner, 2026-11-25: the service is gone; `parity-k-twitter.md`).

### M9. PAGE_MULTIMEDALS (BUTTON_MULTIMEDALS, label MENU_MEDALS, title "MULTIPLAYER MEDALS") - COVERED in the import, CHECK in game (`parity-n-remaining.md` N3)

Two controls of PS4 menu.bin open it (records at menu.bin `0x53b0`, `0x5660`, beside the PS4 collection's bonus
pages), neither binary refers to its id in code. Decide after one look in the game: if the button appears on a
multiplayer page (PS3) the medals list must read the MP medals of NetPlayerData; if it is only on the PS4
single-player bonus pages, nothing to do. Open question 8.

## 4. Loading screen and leaderboards

### M10. Loading screen leftovers - COVERED (b: no visible effect, c: built 2026-11-25)

`loading.md` covers every element. Its "not in scope" list: (a) PS3 letterbox bars: only drawn when the display has
fewer than 719 lines: never on PC, nothing to do; (b) the call at the start of the PS3 draw (`0x158ff8`, id
`cdf70794`): identified (`parity-n-remaining.md` N4), it publishes the "loading screen shown" byte that nothing reads: no visible effect, nothing to do; (c) "rosters only while a menu field is 0": the field is
the menu manager word PS3 `+0x5b4` that also gates M1; on PS4 it is s32 `[0x1136640 + 0x1014]` (section 10.2): draw
  the rosters only while it is 0 (PS3 skips the in-place page draw `0x423410` entirely while it is not 0). DONE
  2026-11-25 (`loading.cpp` `draw_rosters` through `menu_up()`, `loading.md` 4.7).

### Leaderboard tables - COVERED (footer L1 / R1 pictures built, not yet checked in the game)

Columns, pages, filter and footer are the game's own list (`0x113d960` object, `progression.md` 1.5.11-1.5.15 and
`parity-a` 2.2-2.4). The rank badge column is image type 0x27 (`menus.md` 2.9). Nothing more is visible on PS3.
Correction 2026-11-25 (owner: the page showed nothing): the table was never drawn, because nothing on PS4 asked the
list update to start over when the page opens (column set 0). Settled from PS3 the same day (`progression.md`
1.5.19): PS3 sets the reset request only in its NetInit leaderboard init `0x4ab854` (board 0, Global, my page, once
per Multiplayer entry) and keeps board / filter / page across visits; ours does the same from the Multiplayer button,
plus a start-over for a list object that never fetched (the first page-shown handler is replaced). The English mode
button reads "Leaderboards" and the footer "Prev" / "Top of List" as on PS3 (`TEXTS.md`). Built 2026-11-25 (not yet
checked in the game): the footer's L1 / R1 pictures. The menu data asks for `|L1-NOSWAP|` / `|R1-NOSWAP|`, tokens the PS4 text printer lacks;
the PS4 icon font has the pictures (codes 8 / 6). Fix in `MENUS.md` 3.8.1 (menus.cpp points the four records at
"|L1| " / "|R1| ", or "|L2| " / "|R2| " while the PS4 shoulder setting is 0, so the pictures are always L1 / R1 as
PS3; no asset or import change).

## 5. Assets

### M11. Asset holes - COVERED except two known

The import audit (`IMPORT.md` "Audit of what MP / co-op still lacks") checked HUD, menu and net images, particles,
hud.bnk sounds, MP banks, music cues, MP vox and DLC voices, per-mode actors, dc modules and texts against GOTY. Left:
- `sniper-towers-multi-ingame` and `train-wreck-multi-tdm-ingame`: named by the registry, in no copy: nothing to
  import.
- `rank-badges/default2` (ranks above 63): absent in PS3 and PS4, so PS3 drew nothing either.
- New with M1 / M2: no asset needed. Text `e2186a0c` is in every bank; the two sounds of M2 (5) must be found in the
  PS4 sound banks by id (open question 9).

## 6. Ranked missing features

Items 1-6 are built (2026-11-25, section 9; run checks in section 7); 7-10 are settled too (2026-11-25, `parity-n-remaining.md` N1-N4, section 9 items 9-12).
New since then (specified 2026-11-25, sections 10.8, 10.10, 10.11; built the same day in `scoreboard.cpp` /
`overlay_rules.cpp`, run checks: test scenarios 11-13): the film automatic flag, the `e68ae001` clear, the "Leave"
entry-0 rule (also in net matches) and the two toggle blocks.

1. M2 rule 6 + M4: scoreboard opens by itself while dead (with the respawn countdown / co-op tries header).
2. M1: dead-player hint line "|SELECT| Scoreboard |D-PAD| Camera".
3. M2 rules 1-3: PS3 fade speeds (4 x the built rate, section 10.1), entry priority, slide direction, page gate 2a.
4. M2 rule 5: open / close sounds on the scoreboard toggle.
5. M2 rule 4: background dim behind the result pages.
6. M2 rule 8: result pages from the NGM states instead of timers (PS4 facts in section 10.7 / 10.8).
7. M6: film spectating HUD parts (check first).
8. M7: Mute Players lists (check).
9. M9: Multiplayer Medals page (check).
10. M10 (b): loading-screen id `cdf70794` (identified: no visible effect, N4).

Files an implementation touches: `gow3_pc/src/u2mp/scoreboard.cpp` (M1, M2, M4), possibly a small helper shared with
`loading.cpp` for the text-in-rect call and the menu-blocking word (M1, M10); `hud.cpp` (M6, only if the check fails);
`remaining.cpp` / `voice.cpp` (M7, only if the check fails); `gow3_pc/src/u2mp/u2mp_internal.h` if shared helpers are
added; docs: `u2mp/docs/spec/gameplay.md` 1 (scoreboard behaviour updated), `functions-match-gameplay.tsv` rows of
`0x4cbf00` / `0x4cbc10` (pairing correction), `MENUS.md` 3.7 / 5.

## 7. Test scenarios

1. Team Deathmatch, two PCs: A kills B. On B: death camera; the board opens by itself (fade 4/s: 0.25 s, slides down from
   above), header "Respawn in 5..." counting down, then "Respawning"; the hint line fades in at y 500 (4/s); on
   respawn both go (board 4/s, hint 8/s), also when B had opened the board by button. No sound on the automatic
   open.
2. Same, B presses the scoreboard button while alive: board opens at 8/s with sound `d9b465e1`; press again: closes
   with `2f118f31`.
3. Elimination: B dies, watches A; D-pad cycles to the next living teammate. The hint stays while any active player
   is still in play and fades only when every active player is out.
4. Round end (Elimination / Plunder): result page drawn over the board (priority), background dimmed.
5. Co-op Arena: down / die: "[A] Tries Remaining" / "Last Try" in the header.
6. Machinima / film: no hint line; M6 comparison.
7. Pause during a match: Mute Players lists both teams; toggling mutes voice.
8. A pause / menu page up while dead: hint fades out (menu state word `+0x1014` not 0) and the board page is not
   drawn while the menu is up; it is back when the menu closes.
9. Elimination / co-op round end: RESULTS / ROUND_RESULTS appear exactly while NGM state is 3 / 4 (log lines
   `scoreboard: NGM state ...`), gone at the next round start; after the match, back in the lobby, nothing of the
   overlay is drawn.
10. A HUD notification on screen while dying (e.g. an objective message): the board does not open by itself until
    the message is gone (M2 rule 6 override).
11. L3 held: pressing the scoreboard button does nothing (no toggle, no sound); release L3, press again: it opens.
12. Film, theater camera following a player (CIRCLE in the film toggles "following"): when the watched player
    dies, the board opens by itself and closes when that player respawns; not following: it never opens by itself.
    Switch the theater camera to the mode that camera event `806c9824` selects (film camera mode 1): the board and
    the result pages close at once (fade out) and stay closed while that mode is on.
13. A match that ends with end reason 3 or 8: the scoreboard stays drawn (full alpha, A = 1.0) while the task
    leaves, as long as no menu is up. Log the current task's "Leave" state test once per change to confirm the
    task process is TaskManagerOnline there.
14. `|SELECT|` glyph: one screenshot of the hint line (scenario 1) shows which icon code 0x0e draws.

## 8. Open questions

Settled on 2026-11-25 by section 10: 1 (10.2), 2 (10.3), 4 (10.2), 6 (10.7, 10.8), 9 (10.9), 10 (M2 rule 6: PS3
hides the board on the falling edge even when it was opened by button), 11 (10.4, 10.5); by static reading in `parity-n-remaining.md`: 7 (N1; the film look is a CHECK, see 14) and 8 (N3); later the same day 12
(10.10: L3, PS4 held bit 0x2; pause owner pointer `[0x10595d8 + 0x18]`), 13 (10.8: state "Leave", PS4 test
`0x9be070`; not films only) and 15 (10.11: call form, records, reply types). Still open:

3. Partly settled (10.12): `|SELECT|` is icon code 0x0e of the PS4 button icon font, a fixed code (START and SHARE
   have a small and a large code, SELECT only the small one), and PS4 has no touchpad token. Left: whether icon
   0x0e shows the touchpad (then the line names our default button and nothing is to do) or the PS3 SELECT
   shape (then it is an owner decision; the only way to name the touchpad would be our own text). Needs one
   screenshot.
5. CHECK in a run (no longer a reading question): PS4 `0x9766f0` is the exact twin of the PS3 auto-open test, so
   calling it gives PS3 behaviour whatever bit 0x01 of the state record means. Evidence that it means "dead, waiting
   to respawn": the PS4 competitive-only query `0x65b490` (next to cond 44) calls it to decide whether to fill the
   killer row. Still to see in a run: TDM death opens the board, respawn closes it; co-op "down" (flags A / B) opens
   it only if PS4 sets that bit while down.
14. M6 / films: the HUD notification element (10.8) and the film controller events are mapped; their visible
    effect in a film still needs one capture.
16. CHECK in a run (new 2026-11-25): that the current task process in a net match is TaskManagerOnline (its
    "Leave" state is what M2 rule 8 tests there). The binary shows the handle is written for every started DC
    task (10.8) and that TaskManagerOnline has a "Leave" state that resets the NGM state; the task name of the net
    match is not read from data here (test scenario 13).
17. Side note, not needed by the overlay: PS4 `0x10595d8` is the engine pause object (twin of PS3 `0xe1eb1c`,
    10.10). PS3's overlay draw tests its byte `+9` ("paused"); 10.3 uses the time scale f32
    `[0x1116180 + 0x4c]` == 0 instead. The PS4 pause update (`0x4b4f30`) moves request bytes `+0xc` -> `+0xd`
    -> `+0xe` (current, 39 readers) and keeps the old value in `+0xb`; which PS4 byte is PS3 `+9` was not paired
    function by function. Net games do not pause, so only films could show a difference.

## 9. Implementation status (2026-11-25, branches `u2mp-scoreboard` and `u2mp-p4-mfacts`)

Code: `gow3_pc/src/u2mp/scoreboard.cpp` (the overlay), `overlay_rules.cpp` / `.h` (pure rules, test
`gow3_pc/tests/test_u2mp_overlay.cpp`), `loading.cpp` `menu_text_in_rect` (the text-in-rect call shared with the
loading screen) and `menu_up()` (the menu state word, shared with the loading-screen rosters). Behaviour as built:
`gameplay.md` 1.4. Every PS4 fact of section 10 the code uses is checked against its verification bytes at install
(all matched in the 1.02 image on 2026-11-25); a part whose bytes differ is logged and left off (`gameplay.md` 1.5).

| Item | Status | What is built |
|---|---|---|
| 1. M2 rule 6 + M4: board opens by itself while dead | DONE (CHECK) | auto = PS4 `0x9766f0()` (10.4); HUD notification override `0x555550(0x1130008)` (10.8); "every player out" with threshold 0 and flags A / B from the state record (10.6); the falling edge also closes a board opened by button; the state-5 rule. The old died / spawn stand-in is gone. Films (built 2026-11-25): the film rule of 10.8, `8a87d1da` / `f498c2b5` asked through `0x4c4310` with the call form of 10.11 every frame in state 2 while the film flag is set; no answer = false (CHECK, test scenario 12). Co-op "down": run check (open question 5). |
| 2. M1: hint line | DONE (CHECK) | 4/s in, 8/s out from the game's frame step (10.1); menu state word `+0x1014` (10.2); death camera `0x664b90` (10.5); A from the HUD alpha `0x104ed0c` with the 1.0 cases (states 3-5, co-op 4 / 8 / 9 / 12, end reason 3 / 8) (10.3); nothing drawn while paused (10.3). `\|SELECT\|` glyph: open question 3. |
| 3. M2 rules 1-3 | DONE | speed x T / 75 (4.0 / 8.0 per second); priority; slide; page drawn only while the menu state word is 0 (rule 2a). |
| 4. M2 rule 5: sounds | DONE (CHECK) | "scoreboard-open" / "scoreboard-close" by id (10.9; the PS4 entry setter's bytes checked). |
| 5. M2 rule 4: dim | DONE (CHECK) | menu `+0x1030` = ease, `+0x1038` = 1 when the byte is 0 (first request wins), also while a menu is up (10.2). |
| 6. M2 rule 8: pages from match states | DONE (CHECK) | from NGM state, sub-state `ngm+0x1e88` and end reason `ngm+0x1e84` incl. the state 5 / sub-state 3 quirk and the per-frame relayout in states 3 / 4. The 12 s / 60 s timers, `show_round_results()` and the `UNCHARTED_U2MP_SCOREBOARD_STATES` opt-in are removed; the debug request bytes are not implemented (entry 1 never shows). Built 2026-11-25: the "Leave" test (10.8, PS4 `0x9be070` on the resolved task handle, also in net matches; CHECK, test scenario 13 and open question 16) and the `e68ae001` clear (10.11, asked every frame in every state while NET_SM > 1; CHECK, test scenario 12). Safety net kept: outside multiplayer (NET_SM <= 1) the rules run as state 0 (the "Leave" answer is then only logged). |
| 7. M2 rule 7: button blocks | DONE (CHECK) | only in state 2, never while every player is out; the two blocks of 10.10 (built 2026-11-25): L3 held = PS4 held bit 0x2 of `[0x1116180 + 0x86c]` (our raw pad word when that word's bytes differ), pause owner pointer `[0x10595d8 + 0x18]` not 0 (test scenario 11). |
| 8. Loading-screen roster gate (M10 c) | DONE (CHECK) | rosters drawn only while `menu_up()` is false (`loading.md` 4.7). |
| 9-12 (M6, M7, M9, M10 b) | DONE (M6, M7, M9: CHECK in game) | Built 2026-11-25 (`parity-n-remaining.md` 10): M6 = N1 (booster condition fixed), M7 = N2 (Mute Players toggle, remaining.cpp), M9 = N3 (overlay version 6 keeps Multiplayer Medals on the no-save main menus), M10 b = N4 (nothing to do). Resolved by static reading in `parity-n-remaining.md`: M6 = N1 (one inverted booster condition, no separate name element), M7 = N2 (rows covered, toggle missing; same item as J5), M9 = N3 (PS3 button on the no-save main menu; our overlay removes it), M10 b = N4 (not a setting, no visible effect). |

Remaining visible differences from PS3: none known from the code (everything of section 10 is built); left are the
`|SELECT|` glyph picture (3) and the run checks 5 and 16 (test scenarios 11-14). Owner rules that touch this overlay: matchmaking
minimum 2 players in every mode, 1 in co-op; custom games with 1 player in every mode (the "every player out" test then covers the lone co-op player: when that player is out, entry 0 stays up and the hint fades);
trophies are out of scope.

## 10. PS4 facts for the overlay (settled 2026-11-25)

Addresses are PS4 1.02 image offsets (`boot-linked.bin` file offset = address + `0x183ce8`; `eboot.elf` file offset
= address + `0x4000`). Each fact names the PS4 code that shows it and the bytes to verify before use. PS3 addresses
are 1.10 (TOC `0xe183b8`). Calls are System V x86-64 (`U2MP_GUEST`), main thread.

### 10.1 Frame step

- s32 `[0x1116180 + 0x110]` = this frame's step of the game clock in ticks of 1/300 s (PS3 `[0xe60a00 + 0xf8]`; most
  PS4 users multiply it by 1/300). The PS3 overlay multiplies it by 1/75 (TOC word `0x3c5a740e`), so every overlay
  rate "r" means r x 4 per second. The PS3 menu dim code multiplies the same field by 1/150 on both consoles (PS3
  `0x3bda740e`, PS4 constant `0xe62000`), which confirms the field is the same on both.
- Verify: `0x5a3c74`: `48 8d 05 05 25 b7 00 c5 fa 2a 88 10 01 00 00` (lea of `0x1116180`, int-to-float of `+0x110`).

### 10.2 Menu manager state word and background dim (menu manager `0x1136640`)

- s32 `+0x1014` = PS3 `+0x5b4`, the menu state: 0 = no menu up; 5 / 6 = closing; 7 / 8 = popping / pushing a page;
  other values 1..15 for the other phases. Set together with s32 `+0x1018` = 0 (state time) and f32 `+0x101c` = 1.0
  (rate), between two calls of `0x5cd5c0` (PS3 setter `0x4298b8`; PS4 inlines it, e.g. `0x5e5b82`:
  `c7 83 14 10 00 00 05 00 00 00 c7 83 18 10 00 00 00 00 00 00 c7 83 1c 10 00 00 00 00 80 3f`). The menu start
  sets -1 then 0 (`0x5c524e`).
  - Same meaning on PS4: the death-camera D-pad advance in the tracker camera update runs only while it is 0
    (`0x665151`: `48 8d 05 e8 14 ad 00 83 b8 14 10 00 00 00`), and the helpers "a menu is open" test it against 0
    (`0x5cc460`, `0x4cbc18`: `48 8d 05 21 aa c6 00 83 b8 14 10 00 00 00`).
  - Users of the PS3 word in the overlay: the hint rule (M1 2), the page draw gate (M2 rule 2a: PS3 `0x423410`
    draws nothing while it is not 0), and the loading-screen roster draw (`loading.md`, same PS3 function).
- Dim: f32 `+0x1030` = requested dim (PS3 `+0x5d0`), u8 `+0x1038` = "requested this frame" (PS3 `+0x5d8`), f32
  `+0x1034` = the dim actually drawn (PS3 `+0x5d4`). The menu's per-frame update clears `+0x1038` (`0x5d06a4`:
  `41 c6 85 38 10 00 00`) and then moves `+0x1034` toward `+0x1030` (`0x5d0704`:
  `c4 c1 7a 10 8d 30 10 00 00 c4 c1 7a 10 85 34 10 00 00`); in a net game with NGM state not 0 and the film flag
  set it forces both to 0. The PS4 setter pattern "only if the byte is 0: write the value, set the byte" is at
  `0x5d1afd`: `80 bb 38 10 00 00 00 75 0f c5 fa 11 83 30 10 00 00 c6 83 38 10 00 00 01` (inside the menu's own
  page-transition dim `0x5d1a00` = PS3 `0x422d68`, which reads `+0x1014` at `0x5d1a55`: `8b 93 14 10 00 00`). PS3's
  stand-alone setter `0x421b00` has no PS4 copy (its only caller was the removed overlay draw): write the fields.

### 10.3 HUD alpha, menu fade, pause

- f32 `[0x104ed0c]` = the HUD alpha (PS3 `0xe1a660`): the PS4 HUD textured quad `0x555c60` (= PS3 `0x89ff3c`, which
  reads `0xe1a660`) multiplies by it (`0x555cd1`: `48 8d 05 34 90 af 00 c4 e2 79 18 30`); also the HUD text draws
  `0x555767`, `0x55e0b6`. This is A of M1 3 / M2 rule 2.
- f32 `[0x104ed74]` is not the HUD alpha: it is the menu's item fade target, moved each frame toward 0 when the top
  page has flag `+0xb1` bit 0x08 and toward 1.0 otherwise (`0x5a3c66..0x5a3cc7`). `scoreboard.cpp`'s use of it as the
  page item-fade scale (`gameplay.md` 1.4) stays as it is; only A changes.
- Paused: PS3 skips the whole overlay draw while the engine pause flag (byte `+9` of PS3 `0xe1eb1c`) is set. PS4 has
  no such byte: its main loop counts "unpaused frames" while f32 `[0x1116180 + 0x4c]` is not 0 where PS3 tests the
  byte (`0x3faf57`: `49 ff 47 28 c5 f8 57 c0 c4 c1 78 2e 47 4c 74 04`). Rule: draw nothing of the overlay while
  that f32 is 0.0. (Net games do not pause; this matters for films.)

### 10.4 Automatic flag and the character state record

- PS4 `0x9766f0`, `bool fn(void)` = PS3 `0x6b6b3c`, intact and uncalled by the overlay: takes the local character
  (u64 slot pointer `[0x115bd20]`, u32 id `[0x115bd28]`; process valid when the slot holds a process whose `+0x44`
  equals the id), sends it query `1a9688ee` through `0x4c4090`, answers true when the reply status (reply `+0x10`) is
  below 2 (no character or no answer), else bit 0x01 of byte `+0x3c` of the returned record (false when the record
  pointer is 0). Verify (the prologue alone also matches its sibling `0x9765d0`, so check all three):
  `0x9766f0`: `55 48 89 e5 53 48 81 ec 88 00 00 00`; `0x9767c9`: `be ee 88 96 1a e8 bd d8 b4 ff b0 01 83 7d e8 02 72 12`;
  `0x9767e4`: `8a 40 3c 24 01`.
- The record is the character's state record: the character answers `1a9688ee` with exactly what its process
  vtable `+0x88` returns (responders `0x7f3520`, `0x24910e`: `call [vtable+0x88]`, reply type 10). The vtable call
  takes only the process (rdi) and returns the record pointer (`0x65b902`: `48 8b 07 ff 90 88 00 00 00 8a 48 3d 80 e1 01`).
  Flags in it: bit 0x01 of `+0x3c` = PS3 bit 15 (auto flag); flag A = bit 0x40 of `+0x3c` (PS3 bit 9); flag B =
  bit 0x01 of `+0x3d` (PS3 bit 7). A / B are the "down" states of cond 44 and of 10.6. PS3 reads them as a
  big-endian halfword at record `+0x38`; PS4 uses bytes at `+0x3c`.
- Meaning of bit 0x01: not named in either binary. Evidence for "dead, waiting to respawn": the PS4
  competitive-only menu query `0x65b490` calls `0x9766f0` to decide whether to fill the killer row. Implementers do
  not need the name: calling `0x9766f0` is the PS3 rule (open question 5 is only a run check).

### 10.5 Death camera handle

- Tracker (NGM `+0x10 + 0x2c0 x i`, local tracker u64 `[0x113b760]`): u64 `+0x168` = slot pointer of the running
  death camera, u32 `+0x170` = its process id (PS3 `+0x15c` / `+0x160`), written by the spot camera `0x664200`
  (`0x6644f5`) and the teammate camera `0x664550` (`0x664b57`).
- PS4 `0x664b90`, `bool fn(tracker *)` = PS3 `0x4c7b90`: true when the slot pointer and id are not 0, the slot holds a
  process, and its `+0x44` equals the id. Verify `0x664b90`:
  `48 8b 87 68 01 00 00 48 85 c0 74 19 8b 8f 70 01 00 00 85 c9 74 12 48 8b 00 48 85 c0 74 0d 39 48 44 0f 94 c0`.

### 10.6 "Every player out" (PS3 `0x494178`, no PS4 copy)

The threshold is 0: both PS3 calls in the overlay store 0 in the threshold word right before the call (in the
automatic rule and in the hint rule). Rule, with clock = s32 `[0x1116180 + 0x204]` (the net clock, as cond 44):
for each of the 10 trackers (NGM `+0x10 + 0x2c0 x i`) whose byte `+0x20` is not 0 (PS3 `+0x18`), the tracker is out
when any of these holds:
1. died after its last spawn: s32 `+0x180` (spawn) < s32 `+0x17c` (died), strictly, and clock - `+0x17c` >= 0;
2. its character's record has flag A and clock - s32 `+0x178` >= 0;
3. its character's record has flag B and clock - s32 `+0x184` >= 0.
The character is the tracker's u64 `+0x10` slot / u32 `+0x18` id (process `+0x44` must match); when it does not
resolve, flags A and B count as not set. "Every player out" is true when every tracker in use is out; a tracker not
in use counts as out, so with no tracker in use the answer is true. Field map: PS3 `+0x164 / +0x168 / +0x16c /
+0x170` = PS4 `+0x178 / +0x17c / +0x180 / +0x184` (the PS4 tracker reset clears these four at `0x6626b5`:
`48 c7 83 80 01 00 00 00 00 00 00 48 c7 83 78 01 00 00 00 00 00 00`). Note the strict "<" here, against ">=" in the
cond 44 dead test.

### 10.7 NGM states, round sub-state, end reason (NGM `0x113b760`)

| PS3 | PS4 | Meaning, writers |
|---|---|---|
| `+0x1a8c` | s32 `+0x1e80` | match state |
| `+0x1a90` | s32 `+0x1e84` | end reason: set during play by `0x64a7c0` (`0x64a7c7`: `89 b7 84 1e 00 00`, with `+0x1e8c` value and `+0x1e90` frame); 3 or 8 make A = 1.0 and feed the film rule |
| `+0x1a94` | s32 `+0x1e88` | round sub-state: the value of script event `15f1cf05` (handlers `0x5e53b0`, `0x5efd62`; `0x5e53f0`: `89 88 88 1e 00 00`); when it is 3 the state becomes 4 (`0x5e5409`: `83 f9 03 75 0b c7 80 80 1e 00 00 04 00 00 00`) |
| `+0x1ba4` | u8 `+0x209c` | film playback flag (set at match setup `0x641518`; the HUD tests it before asking the film controller, `0x577f9f`: `80 b8 9c 20 00 00 00`) |

State writers: 2 with sub-state and end reason 0 at every round start (`0x647a82`:
`48 c7 83 80 1e 00 00 02 00 00 00 c7 83 88 1e 00 00 00 00 00 00`, = PS3 `0x4a85a0`); 3 at match end (`0x648c65`:
`41 c7 87 80 1e 00 00 03 00 00 00`) and on script event `bcc3b05b` (`0x5e5396`); 4 as above; 5 in the leave
sequence `0x641b50` (`0x641f63`: `41 c7 84 24 80 1e 00 00 05 00 00 00`), called right after the menu is closed and
before the local tracker pointer is cleared; 0 on the exit paths (`0x5e8907`: `41 c7 86 80 1e 00 00 00 00 00 00`,
`0x5f0225`) and when the next game starts (`0x649120`, `0x64914b`); 1 during match setup (`0x64150d`).
Stale state 3: it lasts from match end until the leave / exit path; during that time PS3 shows RESULTS (intended),
and on the way back the lobby menu makes the menu state word non-zero, which stops the page draw (M2 rule 2a), and
the exit paths then write 0. Our extra "in the match" gate can stay as a safety net but is not PS3 behaviour.

### 10.8 Film controller, HUD notification override, request bytes

- The handle: u64 slot pointer `[0x1164080]`, u32 id `[0x1164088]` (PS3 `0xfdbc3c`). Correction (2026-11-25): it
  is not a film-only controller. The task system writes it for every DC task process it starts (`0xa5c7f6`:
  `48 8b 43 38 48 8b 48 08 48 8b 40 10 48 8d 15 77 78 70 00 48 89 42 08 48 89 0a`, the new process's handle
  copied from the task node) and clears it on shutdown (`0xa5df75`). In a film it is TaskManagerReplay (CINEMA.md
  4), in a net match its online twin TaskManagerOnline (both names sit side by side in the binary with their
  state names; open question 16 is the run check). "The film controller" below means this process.
- Only TaskManagerReplay's RePlaying state answers the three queries. Its event handler is slot 0 of the RePlaying
  state vtable `0x100a790` (`0x5e8e70`; the other replay states' slot 0 and all TaskManagerOnline states jump to
  the shared base handler `0xa62c90`, which compares none of these ids; the ids appear as immediates nowhere else
  in the binary). The handler ignores every event while the engine pause owner pointer u64 `[0x10595d8 + 0x18]` is
  not 0 (`0x5e8e99`: `48 8d 05 38 07 a7 00 48 83 78 18 00 0f 85 ed 03 00 00`; PS3 `0x4355f0` makes the same test
  on `0xe1eb1c + 0x10`, which is how 10.10 pairs the two objects). Answers (reply records of 10.11):
  - `8a87d1da` -> reply type 2, s32 = the watched player's tracker index, data `+0x168` (`0x5e91d8`:
    `3d da d1 87 8a 0f 85 b5 00 00 00 49 8b 46 08 8b 80 68 01 00 00`; type at `0x5e9206`:
    `41 c7 45 30 02 00 00 00`). RePlaying moves it over the 10 trackers when the camera switches player;
  - `f498c2b5` -> reply type 1, bool = "following a player", data byte `+0x16d` (`0x5e9180`:
    `3d b5 c2 98 f4 0f 85 0d 01 00 00 49 8b 46 08 8a 80`; type at `0x5e91c4`: `41 c7 45 30 01 00 00 00`). The
    film player toggles it with CIRCLE (RePlaying update, PS4 pad bit 0x2000 at `0x5e6d5a`);
  - `e68ae001` -> reply type 1, bool = film camera mode s32 `[0x113a990 + 0x678]` == 1 (`0x5e9336`:
    `3d 01 e0 8a e6 75 7a 48 8d 05 4c 16 b5 00 83 b8 78 06 00 00 01`). Camera event `806c9824` sets the mode to 1
    (`0x5e8ed7`), `ff502160` to 0, `887c7d6e` to 2. Outside films (or in another replay state) no answer comes
    (reply type 0) and nothing happens.
- Film automatic flag (M2 rule 6, NGM `+0x209c` set): auto = false; if `8a87d1da` is answered (reply type not 0)
  and the index is below 10 and that tracker died after its last spawn (10.6 condition 1 without the clock
  part), auto = true; then if `f498c2b5` is not answered or answers false, auto = false. PS3 asks both every frame
  in state 2 while the film flag is set.
- Entry-0 rule (M2 rule 8, states 0 / 1 / 3 / 4): PS3 asks whether the task process is in its state named "Leave":
  id `9f514949` = the game's CRC-32 string id of "Leave" (the same hash as `sids.tsv`; "resetting" =
  `352cd97b` checks the variant). State ids are hashed from the state names at run time, so the constant is in
  neither binary's data; PS3 has it only as an immediate in the overlay. PS4 twin of the PS3 test `0x70db88`:
  `0x9be070`, `bool fn(process *, u32 state_id)`: true when the process has a state object (`+0x50`, then the
  pointer it holds) whose descriptor (`+0x10`) has the name id at `+0x18`. Verify `0x9be070`:
  `48 8b 47 50 48 85 c0 74 13 48 8b 00 48 85 c0 74 0e 48 8b 40 10 39 70 18 0f 94 c0 c3`. Resolve the handle first
  (slot pointer and id not 0, the slot holds a process, process `+0x44` == id; else false). Both tasks have a
  "Leave" state: replay GoLeave `0x5e3e80` and online GoLeave `0x5eaed0` hash the string at `0xe87ee6`
  (`0x5e3eab`: `4c 8d 35 34 40 8a 00`; `0x5eaefb`: `4c 8d 35 e4 cf 89 00`); the online Leave state (vtable
  `0x100a9c0`, Enter `0x5f00d0`) calls the game-start reset `0x649120` and writes NGM state 0 (`0x5f0225`) but
  leaves the end reason `ngm+0x1e84` as it is, so the rule can hold outside films (M2 rule 8).
- HUD notification override (PS3 `0x8a0348` = `0x8a4784` on HUD `+0x118`): PS4 `0x555550`, `bool fn(hud)` with
  rdi = `0x1130008` (the HUD manager), uncalled: true while the HUD notification queue element (HUD `+0x310`, PS3
  `+0x118`, PS3 ctor `0x8a47dc`, PS4 `0x5579a0`; up to 8 queued messages) has an entry in its first or second
  display slot (u64 pointers `+0x138` / `+0x140`, each to a non-zero s32). The PS4 HUD update raises HUD flag 0x400
  while it is true (`0x554251..0x55427e`). Verify `0x555550`:
  `48 8b 8f 10 03 00 00 48 8b 91 38 01 00 00 48 85 d2 74 07 b0 01 83 3a 00 75 15 48 8b 89 40 01 00 00 48 85 c9 74 07 b0 01 83 39 00 75 02 31 c0 c3`.
- Request bytes: PS3 `0xf2264c` `+0x54 / +0x55 / +0x56` are registered (PS3 `0x4b4244`) as the debug-menu switches
  "Force Results Scoreboard" (entry 2), "Force Round Results Scoreboard" (entry 3), "Force Wave Results
  Scoreboard" (entry 1). Always 0 in retail: entry 1 is therefore never shown in a normal PS3 game.

### 10.9 Entry setter and sounds

- PS4 `0x666b50(overlay, k, on, with_sound)` = PS3 `0x4cbac0`, intact and uncalled: when the shown byte changes and
  k = 0 with sound, plays `d9b465e1` on open / `2f118f31` on close through `0x3dd780` / `0x9b5c20`; when it turns on,
  sets `+0xf8` = 2 on the loaded page with the entry's id. Verify `0x666b8b`:
  `85 f6 75 2f 80 f1 01 75 2a 84 db b8 e1 65 b4 d9 be 31 8f 11 2f`.
- The pair, as every PS4 caller uses it (e.g. `0x4ae0cc`, `0x3fb2b8`): `0x3dd780(buffer, id, 0)` (rdi, esi, edx; a
  frameless leaf that fills the buffer, id at `+0x70`), then `0x9b5c20(buffer, 0)` (rdi, esi). The buffer is a
  caller local below `rbp-0x130`, 16-byte aligned (the init stores an xmm at `+0x60`). Verify `0x3dd780`:
  `48 89 f8` (`mov rax, rdi`), `+0x77` `89 90 d8 00 00 00`, `+0x126` `89 70 70`; `0x9b5c20`: `55 48 89 e5`,
  `+0x14` `49 89 f5 48 89 fe`.
- `d9b465e1` = sound record "scoreboard-open", `2f118f31` = "scoreboard-close": PS4 `dc1/ps4/audio.bin` id table
  entries at file `0xf258` / `0x3618`, records at `0x78518` / `0x28820` (each: id, pointer to its name string, then
  the shared field `0xf19e22dc`); the sample names are in `sound1/banks/ps4/menu.bnk` (the menu bank). The ids are
  not the CRC string ids of these names (U2 sound ids use their own hash), so look them up by id.

### 10.10 Button and its blocks (M2 rule 7)

- PS3 reads the toggle from the frame object's "pressed this frame" word (`0xe60a00 + 0x814`, bit 0x2000) and the
  block from its "held" word (`+0x81c`, bit 0x4000). PS4 keeps the two words at `0x1116180 + 0x864` / `+0x86c`
  (pad 0 of an array of 0x130-byte pad records at `0x1116180 + 0x7d0`, cleared in `0x4ca7d0..`; the camera D-pad
  test `0x66516c`: `8b 80 64 08 00 00 a8 20`).
- Bit layouts. PS4 uses the scePad button bits (L3 0x2, R3 0x4, OPTIONS 0x8, UP 0x10, RIGHT 0x20, DOWN 0x40, LEFT
  0x80, L2 0x100, R2 0x200, L1 0x400, R1 0x800, TRIANGLE 0x1000, CIRCLE 0x2000, CROSS 0x4000, SQUARE 0x8000,
  touchpad 0x100000): the debug button sequence table at `0xecf470` holds up, up, down, down, left, right, left,
  right as 0x10 0x10 0x40 0x40 0x80 0x20 0x80 0x20 (PS3 table `0xe130d0`: 1 1 2 2 4 8 4 8). PS3 uses its own
  layout. Pairing by the menu input mapper, PS3 `0x4227c8` = PS4 `0x5d0be0` (both return the same action codes
  in the same order) and by the film RePlaying update (PS3 `0x436220` = PS4 `0x5e5c80`):

  | PS3 bit | button | PS4 bit | evidence |
  |---|---|---|---|
  | 0x1 / 0x2 / 0x4 / 0x8 | UP / DOWN / LEFT / RIGHT | 0x10 / 0x40 / 0x80 / 0x20 | sequence tables, camera D-pad, RePlaying up / down |
  | 0x10 | TRIANGLE | 0x1000 | mapper action 9; RePlaying camera mode |
  | 0x20 | CROSS | 0x4000 | mapper action 5 / 6 (swap flag); RePlaying `953b8f2a` |
  | 0x40 | SQUARE | 0x8000 | mapper action 8; RePlaying HUD flag (CINEMA.md 7) |
  | 0x80 | CIRCLE | 0x2000 | mapper action 6 / 5; RePlaying "following" toggle |
  | 0x100 / 0x400 | L1 / R1 | 0x400 / 0x800 | mapper actions 11 / 10 (swap flag) |
  | 0x800 | R2 | 0x200 | RePlaying (PS4 `0x5e750f`: `41 f6 84 24 65 08 00 00 02`) |
  | 0x1000 | START | 0x8 (OPTIONS) | mapper action 7 |
  | 0x2000 | SELECT | 0x100000 (touchpad) | mapper action 12 (`0x5d0d10`: `b8 0c 00 00 00 f7 c2 00 00 10 00`) |
  | 0x4000 | L3 | 0x2 | mapper action 7 needs START pressed and this bit not pressed: PS3 mask 0x5000 == 0x1000, PS4 mask 0xa == 0x8 (`0x5d0c03`: `89 d6 83 e6 0a b8 07 00 00 00 83 fe 08`) |

  So the PS3 block "held bit 0x4000" is "L3 held": PS4 bit 0x2 of the held word u32 `[0x1116180 + 0x86c]`. The
  PS4 menu update makes the same kind of test (held L3 blocks its own START action, `0x5cf51d`:
  `41 f6 85 6c 08 00 00 02`). Our raw pad word `0x14b7fb0` carries the same scePad bits; either word may be used
  (the held word is the game's own).
- The other block, PS3 word `0xe1eb1c + 0x10`: the PS3 engine pause object `0xe1eb1c` is PS4 `0x10595d8`, and its
  word `+0x10` is PS4 u64 `+0x18`. Pairing: the first thing the film RePlaying event handler does on both consoles
  is to drop the event while that field is not 0 (PS3 `0x4355f0` through its TOC slot `0xdb01cc` -> `0xe1eb1c`;
  PS4 `0x5e8e99`, bytes in 10.8); CINEMA.md 7 notes the same PS3 field gating the theater HUD. On PS4 it is a
  pointer to an engine-level owner object (has a vtable; the shutdown paths `0x4d46d8`, `0x64dcdc`, `0xa5803c`
  call its slot `+0x18` with 1 and then clear the field). While it is set the menu takes no input for 2 frames
  (`0x5d05a4`: `48 8d 05 2d 90 a8 00 48 83 78 18 00 74 0d 41 c7 85 20 10 00 00`, which sets menu `+0x1020` = 2).
  Rule: no toggle while u64 `[0x10595d8 + 0x18]` != 0.
- Our own pad word (`0x14b7fb0`, touchpad) stays the toggle.

### 10.11 Query call form (`0x4c4310`), records and reply types

- `0x4c4310` is the engine's "send a process an event and wait for its answer" call. System V, main thread:
  `rec *fn(rec *reply_out, u32 event_id, u64 slot_ptr, u64 id, rec *a0, rec *a1, rec *a2 /* stack +0 */,
  rec *a3 /* stack +8 */)`; returns `reply_out`. `slot_ptr` / `id` are the handle's two words exactly as stored
  (`[0x1164080]`, `[0x1164088]`; the HUD passes the full u64 at `+8`, only its low 32 bits are compared).
- Record (argument and reply alike): 0x18 bytes, 16 value bytes at `+0` (a bool is the byte at `+0`, an s32 the
  u32 at `+0`, a pointer the u64 at `+0`), u32 type at `+0x10`, 4 bytes padding. Types seen: 0 = empty / no
  answer, 1 = bool, 2 = s32, 10 = pointer (`0x5e8f96..0x5e901c` sends two pointer arguments, type 10 at `+0x10`).
- Arguments: the call copies a0, a1, a2, a3 in that order to the engine's argument stack (`0x1116150`) and stops
  at the first record whose type is 0 (`0x4c4386`: `41 83 78 10 00 0f 84 c5 00 00 00`). For the three overlay
  queries all four are empty: four zeroed 0x18-byte records (the HUD zeroes `+0`, `+4`, `+8` and `+0x10` of each).
- Delivery: the reply starts as the empty record (copied from `0x1116168`, all zero). When `slot_ptr` and `id` are
  not 0, the slot holds a process, its `+0x44` equals `id` and its u16 flags `+0x34` have neither bit 0x4
  (`0x4c4470`: `0f b7 4f 34 f6 c1 04 74 72`) nor bit 0x2, the call makes that process current (`[0x1161ee8]`),
  hands it the event (its state object's slot 0 when it has one at `+0x50`, else its own vtable slot `+0x38`),
  restores the current process and copies the event's reply (event `+0x20`, type at `+0x30`) to `reply_out`.
  Otherwise `reply_out` stays empty.
- Reading the answer: answered when reply type (`+0x10`) is not 0. The HUD's own use (`0x57803f..0x57807d`):
  `48 8d 85 58 ff ff ff 48 89 44 24 08 48 8d 85 70 ff ff ff 48 89 04 24 be 01 e0 8a e6 48 8d 7d b8 4c 8d 45 a0
  4c 8d 4d 88 4d 89 de e8 a1 c2 f4 ff`, then it tests `reply + 0x10` != 0 and the byte at `reply + 0`.
- Verify `0x4c4310`: `55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec` plus the two patterns above.
- Use in the overlay (all on the main thread from the scoreboard frame hook): `e68ae001` every frame;
  `8a87d1da` and `f498c2b5` every frame in NGM state 2 while the film flag `ngm+0x209c` is set. Outside films the
  current task (TaskManagerOnline) gets the events and does not answer them; that is the PS3 behaviour too.

### 10.12 `|SELECT|` in the PS4 text printer

- The PS4 printer (`0xccb270..`) reads a `|NAME|` token, hashes NAME with the game's string id and looks the id up
  in a compare chain that gives an icon code of the button icon font (loaded once into `[0x144e640 + 0x10]`, a
  table of 0x16-byte glyph records searched by code). Codes found: X = 5, D-PAD = 1, D-PAD-UP / -DOWN / -UD / -LR,
  L1, R1, R2, L3, R3, START = 0x0b (0x2c at large text sizes), SHARE = 0x1d (0x31), SELECT = 0x0e (one code at every
  size; `0xccbec2`: `81 fa 18 bc 2c 58 0f 85 c2 00 00 00 41 bd 0e 00 00 00`), BACK / ENTER = the two confirm
  buttons (swapped by the confirm-button setting). No TOUCHPAD or OPTIONS token exists.
- PS4 texts: only `e2186a0c` uses `|SELECT|` (English bank `2.networking`); `2.common` uses `|START|`, `|SHARE|`
  and `|X|`. What icon 0x0e looks like on PS4 is not in the code (open question 3).
