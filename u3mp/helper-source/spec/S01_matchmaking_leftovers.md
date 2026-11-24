# S01. Matchmaking leftovers: party count, vote helpers, Community catch, stand-in tick, online driver

Behaviour description for a reimplementation (reimpl2 pipeline, final sweep group S01). It says what the functions
do, not how the old ones were written. Conventions as in `G36_matchmaking.md`: offsets are hexadecimal when
prefixed with 0x; "image offset X" is the address `base` + X; all multi-byte values are little-endian; u8 / u32 /
s32 / u64 / s64 are integers of that width, "pointer" is 8 bytes. Clock values are u32; their subtractions wrap and
each test says whether the difference is taken as unsigned or signed. Counters are plain 32-bit increments
(wrapping). Game functions use the System V x86-64 convention with integer arguments only. There is no float code
in this group.

## 1. Purpose and callers

G36 describes the matchmaking pieces that follow the game (players list, Start Match, online search, end of the map
vote, difficulty vote, start / join step). This group is what the final sweep found left around them:

- **Part A, small rules**: the number of players in a session (the game's party count: a member with a second
  local player counts twice); the winner of the map vote; whether a game type gets a difficulty vote; how a vote
  block is opened; the two Community buttons of the menu's catch-all (Cinema, Leaderboards) as PS3 1.19 handles
  them.
- **Part B, the offline stand-in tick**: the per-frame driver of the offline matchmaking stand-in. It notices that
  the "Starting in" popup's ok ran, then plays PS3 1.19's status sequence (search phases, players found, "Voting
  starts", map vote, co-op difficulty vote, "Game starting in") on the game's own status variables and vote blocks,
  and at the end selects the winning net game and starts the match.
- **Part C, the online driver**: the per-frame driver of the real online matchmaking: a watchdog on the match start,
  following the party leader into its match room (party message 0x113, as PS3 1.19 does), the search, joining the
  first room found or hosting one, waiting for the playlist's minimum of players, merging into another room, and
  the hand-over to the stand-in's vote / countdown; plus its failure step (the game's matchmaking failure path).

| Function | Called by | When |
|---|---|---|
| `u3x_party_players` | G07, G10 (C++), C `lnb_...` member count (u3_lan_profiles.c 5938) | as they need it |
| `u3x_vote_winner` | `u3x_m05_tick` (this group); may be used by G36 5.4 | end of the map vote |
| `u3x_difficulty_vote_for` (replaces `u3x_has_difficulty_vote`) | G36 `u3x_n09_open` | opening the difficulty vote |
| `u3x_vote_open` | G36 `u3x_n09_open` | opening the difficulty vote |
| `u3x_m02_catch` (old C `m02_catch`) | C wrapper `m02_catch`, reached through the C export `u3h_community_catch` from the menu's catch-all `u3x_menu_action` (C++, G32) | a menu button without target page was pressed |
| `u3x_m03_tick` (old C `m03_tick`) | C wrapper, from the per-frame tick (`u3_profile_event` kind 16, non-zero `base`) | every frame |
| `u3x_m05_tick` (old C `m05_tick`) | `u3x_m03_tick` only (the C wrapper is left without a caller) | every frame of a running stand-in in fill mode |
| `u3x_onm_tick` (old C `onm_tick`) | C wrapper, from the per-frame tick right after `m03_tick` | every frame |
| `u3x_onm_fail` (old C `onm_fail`) | `u3x_onm_tick` only (the C wrapper is left without a caller) | - |

Threading: everything runs on the thread that runs the helper's hooks. No thread, no lock.

## 2. Interface

All functions are `extern "C"`. No float is passed or returned. `base` = the game image base; `s` = the helper's
state object (G36 3.2).

### 2.1 Functions to deliver

The group is written as three independent parts; no part calls a function of another part.

**Part A**

| Function | Signature | Result |
|---|---|---|
| party count (existing name) | `int u3x_party_players(const uint8_t *session)` | number of players |
| map vote winner (existing name) | `int32_t u3x_vote_winner(const uint8_t *vote)` | net game index |
| difficulty vote for a game type (NEW name, replaces `u3x_has_difficulty_vote`) | `int u3x_difficulty_vote_for(uintptr_t base, uint32_t game_type)` | 1 / 0 |
| open a vote block (existing name) | `void u3x_vote_open(uint8_t *vote)` | - |
| Community buttons (new) | `int u3x_m02_catch(uintptr_t base, void *s, uint32_t id)` | 1 = handled, 0 = not this button |

**Part B**

| Function | Signature |
|---|---|
| stand-in tick (new) | `void u3x_m03_tick(uintptr_t base, void *s)` |
| stand-in fill sequence (new) | `void u3x_m05_tick(uintptr_t base, void *s, uint32_t now, uint32_t dt)` |

**Part C**

| Function | Signature |
|---|---|
| online driver (new) | `void u3x_onm_tick(uintptr_t base, void *s)` |
| online failure step (new) | `void u3x_onm_fail(uintptr_t base)` |

C wrappers (integrator): `m02_catch`, `m03_tick`, `m05_tick`, `onm_tick`, `onm_fail` each become one line passing
their arguments unchanged to the `u3x_` function of the same name. `u3x_has_difficulty_vote` is deleted with its C
declaration; G36's `u3x_n09_open` (part_b.cpp, one call and one declaration) calls
`u3x_difficulty_vote_for(base, t)` instead (a new name so that a stale declaration cannot link silently).

The C functions `onm_stop`, `onm_host`, `onm_research`, `onm_ready`, `onm_members`, `mm_tell_party` (own glue,
called only from `onm_tick`) are described in 4.9 as private helpers of part C and deleted from the C part by the
integrator. The first part of the old `onm_tick` (own glue and diagnostics, see 4.8 step 2) moves unchanged into a
new C function behind the NEW export `u3h_onm_watch`.

### 2.2 Functions of other groups, of the C part and of the shared tools that the new code calls

"NEW" = a one-line C export the integrator adds (name proposed). Exports already proposed by G36 are marked "G36
NEW"; one copy each is enough.

| Name | Signature | What it does | Part |
|---|---|---|---|
| `u3h_ext`, `ext_ea()`, `ext_eb()` | CONVENTIONS.h | helper words ea[], eb[]; ec[] = `u3h_ext()` + 0x4450 (G36 3.1) | A, B, C |
| `u3x_type_is_coop` | `int u3x_type_is_coop(uintptr_t base, int32_t type)` | G38 4.1: 1 when `type` is a co-op game type by the game's own mask at image 0x721100, else 0. Pure read | A |
| `u3x_cinema` | `void u3x_cinema(uintptr_t base, void *s)` | exists: the menu's Cinema entry (cinema mode, film list page) | A |
| `u3h_show` | `int u3h_show(uintptr_t base, void *s, uint32_t page, int silent)` | shows a menu page | A, C |
| `u3h_m02_diag` (NEW) | `volatile uint32_t *u3h_m02_diag(void)` | address of the C static `m02_d` (8 words) | A |
| `u3h_m03`, `u3h_m03_diag` | G36 2.2 | stand-in state (4.0) and its 8 diagnostic words | B, C |
| `u3h_m03_stop` (NEW) | `void u3h_m03_stop(uintptr_t base, int cancel)` | C `m03_stop` (own): status 0, NET + 0x18 = 0, closes the votes, with `cancel` the net state idle; stand-in armed / running / phase = 0 | B |
| `u3h_m03_pick` (NEW) | `int32_t u3h_m03_pick(uintptr_t base, void *s)` | C `m03_pick` (own): a net game index of the stand-in's playlist by weight, -1 for none | B |
| `u3h_m05_cand` (NEW) | `int32_t u3h_m05_cand(uintptr_t base, void *s, int32_t not_map)` | C `m05_cand` (own): a net game index of the stand-in's playlist whose map id (record + 4) is not `not_map` (-1 = any), -1 for none | B |
| `u3h_m05_vt` | G36 NEW | address of the u32 "last stand-in vote time" | B |
| `u3h_n05_backout` (NEW) | `int u3h_n05_backout(uintptr_t base)` | C `n05_backout` (own): 1 = a "leave Find Game" back-out is under way this frame | B, C |
| `u3h_n05_note` (NEW) | `void u3h_n05_note(int k, uint32_t v)` | C `n05_note`: diagnostic word k AND 7 of `n05_d` = v, or += v when k has bit 8 (0x100) | B |
| `u3h_n02_gone`, `u3h_n02_tarm` (NEW) | `uint32_t *u3h_n02_gone(void)`, `uint32_t *u3h_n02_tarm(void)` | addresses of the C statics `n02_gone` (popup-gone count) and `n02_tarm` (clock when the search was armed); G36's `u3h_n02_arm` writes the same two | B |
| `u3h_n02_diag` (NEW) | `volatile uint32_t *u3h_n02_diag(void)` | address of the C static `n02_d` (8 words) | B |
| `u3h_n04_idx`, `u3h_n04_diag` | G36 NEW | "net game selected at the vote's end" (s32) and the vote end's 8 diagnostic words | B |
| `u3x_n04_close`, `u3x_n09_open`, `u3x_n09_tick` | G36 2.1 | end of the map vote; open the difficulty vote; difficulty vote per tick (1 = closed now) | B |
| `u3x_start_game_now` | `void u3x_start_game_now(uintptr_t base, void *s)` | G35: starts the selected match at once (session reset, countdown done) | B |
| `u3h_active_page` | G36 2.2 | id of the top page; 0 while the menu changes pages | B, C |
| `u3h_active_page_object` | `uint8_t *u3h_active_page_object(uintptr_t base)` | the top page object even while the menu changes pages (null when the top index is 160 or more or the slot is empty); "raw top id" = its u32 at +0x10, 0 for null | B, C |
| `u3x_sym_table`, `u3x_pick_playlist`, `u3x_net_clock` | G36 2.2 | script data lookup; playlist entry; net clock | B, C |
| `u3h_onb_mode` | G36 2.2 | 1 online, 2 offline stand-in, 0 LAN | B, C |
| `u3h_onm` | G36 NEW | online driver state (4.0) | C |
| `u3h_onm_diag` (NEW) | `volatile uint32_t *u3h_onm_diag(void)` | address of the C static `onm_d` (8 words) | C |
| `u3h_onm_state` | G36 2.2 | net state NS = value, NS clock = game clock | C |
| `u3h_mm_bg` | G36 NEW | the u8 background flag | C |
| `u3h_onm_watch` (NEW) | `void u3h_onm_watch(uintptr_t base, void *s)` | the own first part of the old `onm_tick`, moved unchanged into a C function: stores `s` for the online stamp, runs the party follow tick (`lnb_follow_tick(0, base, s)`), publishes "online driver running" for the heartbeat, and the Custom Game start watch and the state-0x30 watch (log lines; at state 0x30 the match host also marks remote players ready, `lnb_np_ready`) | C |
| `u3h_mm_log` (NEW) | `void u3h_mm_log(const char *a, int32_t v, const char *b)` | C `mm_log`: a log line "matchmaking: " + `a`, the number `v` (none for -1), then `b` (may be null) | C |
| `u3h_cg_t0_ptr` | `uint32_t *u3h_cg_t0_ptr(void)` | exists: address of the Custom Game watch's start clock (0 = not watching) | C |
| `u3h_mq_find` | `int32_t u3h_mq_find(uintptr_t base, uint32_t type, uint32_t *idx, int remove)` | exists: index of the first queued party message of `type`; 0 = found | C |
| `u3h_mq_get_np` | `int32_t u3h_mq_get_np(uintptr_t base, uint32_t idx, uint8_t *from, void *data, uint32_t *len)` | exists: game call 0x776530: copies message `idx`'s sender id (0x24 bytes) and data; 0 = success | C |
| `u3h_mq_send` | `int32_t u3h_mq_send(uintptr_t base, const uint8_t *to, uint32_t type, const void *data, uint32_t len)` | exists: sends a party message to one member; result not used | C |
| `u3h_members` | `uint32_t u3h_members(uint8_t *session, uint8_t **out)` | exists: fills `out` (room for 10) with the session's member slots, returns the count | C |
| `u3h_fr_strcmp` | `int u3h_fr_strcmp(uintptr_t base, const void *a, const void *b)` | exists: the game's string compare (image 0x139c710); 0 = equal | C |
| `u3x_party_leader` | `int u3x_party_leader(uintptr_t base)` | exists: non-zero when the party session (image 0x17dc298) has no room or its own member id equals its host's | C |
| `u3x_find_member` | `uint8_t *u3x_find_member(uint8_t *session, uint32_t id)` | exists: the member slot (in use) with that member id, or null | C |
| `u3x_online_search` | G36 5.3 | sends the online search; 0 sent, -1 not | C |
| `u3x_mm_ev6` | G36 5.8 | runs a matchmaking page handler with event 6 | C |

## 3. Game and helper facts

Everything of G36 3.1 to 3.7 holds; additions here.

### 3.1 Helper words

| Word | Meaning |
|---|---|
| eb[2] bit 11 (0x800) | set = Community buttons not handled (m02 off) |
| eb[2] bit 13 (0x2000) | set = stand-in off ("stand-in on" below = clear) |
| eb[2] bit 14 (0x4000) | set = the stand-in does not start the match at the end |
| eb[3] bit 16 (0x10000) | set = fill sequence off ("fill mode" = stand-in on and this bit clear) |
| eb[3] bit 17 (0x20000) | set = n002 off ("n002 on" = clear) |
| ec[2] bit 2 (0x4) | set = n004 off ("n004 on" = clear): no G36 vote end, plain countdown |
| ec[2] bit 4 (0x10) | set = n006 off: the map vote also opens the difficulty block's flag |
| ea[90] | matchmade matches started (plus 1 per start) |
| ea[94] | "the match was matchmade" flag (set to 1 at a start) |

### 3.2 The helper's own state (stays in the C part)

**Stand-in state M** (`u3h_m03()`, u32 words): 0 armed, 1 running, 2 start clock, 3 phase, 4 found, 5 max, 6 selected
row, 7 phase clock; pointer at byte 0x20 = the playlist entry (set by Start Match). Diagnostics `u3h_m03_diag()`:
[1] runs, [2] phase (fill mode: phase OR 0x100), [3] found, [5] matches started, [6] picked index.

**Online driver state O** (`u3h_onm()`, u32 words): 0 armed, 1 running, 2 phase, 3 armed clock, 4 phase clock,
5 selected row, 6 max, 7 rooms in the result, 8 min, 9 re-search clock, 10 in room, 11 merging, 12 watchdog clock.
Diagnostics `u3h_onm_diag()`: [1] searches, [2] phase, [3] rooms / members, [4] joins, [5] hosts, [6] stops, [7]
last net state.

### 3.3 Game data used besides G36 3.5

| Image offset | Type | Meaning |
|---|---|---|
| 0x17b4ba4 | u32 | the game clock, 300 per second ("now" below, one read where said) |
| 0x1b68814 | u32 | the net state's clock (written with NS by the game's ok action and by `u3h_onm_state`) |
| 0x17d79e8 | s32 | search phase index of the status line: 0 Optimal Game, 2 Close Game, 4 Any Game |
| 0x17bd080 + 0x898 | pointer | pending popup / dialog object of the menu (null = none); its u32 at +0x10 = 0x453470dd for the "Starting in" popup |
| 0x17bd080 + 8 | u32 | menu UI state |
| 0x16e7171, 0x16e7172 | u8 | two start flags of the game (cleared on failure) |
| 0x17dc298 | object | the party session PS (layout as MS): u8 at +0xc8 "in a party", u64 +0x18 room, u32 +0x12e4 own id, u32 +0x12e8 host id |
| 0x17d6010 | 0x14 bytes | copied into the 0x113 message (see 4.9; meaning open) |
| NET + 0x28 | u8 | the game's net error flag |
| MS + 0xb0 | u8 | "search result arrived" (set by the backend message 310) |
| MS + 0xb4 | s32 | search result error (negative = failed) |
| MS + 0x218 | u32 | number of rooms in the result; rooms from MS + 0x220, 56 bytes each, u64 room id at +0 |
| map vote V + 4 | s32 | number of candidates (2) |

Net game record (G36 3.5) + 4 = map id (s32). Playlist entry: u32 at +0x18 = minimum players, +0x1c = maximum.

Session manager function table (pointer at image 0x17dd768, first 8 bytes = table; object first, results s64):
entry 4 Create `(mgr, MS, u32 capacity, u32 0, u32 0xffffffff, u32 0)`; entry 5 Search (G36); entry 6 Join `(mgr,
MS, u64 room)`; entry 7 Leave `(mgr, MS)`. "The manager is usable" = the pointer is not null and the table pointer
it holds is not null.

Game function 0x652200 `(menu = image 0x17bd080, int 0)`: menu back one page.

Pages: 0xe35f0f1a matchmaking; "back-out pages" = 0x0b7714b5 (playlists), 0x53cbb5f2 (multiplayer), 0x1e66a744,
0x37656388. Buttons: 0xb85cb12a Cinema, 0xfb59d351 Leaderboard; page 0x8236d02a leaderboard. Party message 0x113 =
"the leader's match room" (0x30 bytes, u64 room at +0x28). Net states: 3, 6 (searching), 0xb / 0xf (join), 0x12
(idle in room), 0x13 (result chosen), 0x14 (20, matchmaking page), 0x28, 0x2e / 0x2f (Starting Game...).

"Set status S" = s32 NET + 0x40 = S (deadline untouched). "Set status S for T seconds" = as G36 3.5.
"Picked playlist" = as G36 3.5. "Clamp max" = P's u32 at +0x1c (10 when P is null), then raised to 2 and lowered to
10 (unsigned). "Clamp min" = P's u32 at +0x18 (2 when P is null), raised to 2, then lowered to max (unsigned).
"n04 due" = `u3x_net_clock(base)` as s32 is greater than or equal to the s32 at NET + 0x2c (a plain signed
comparison, as G36 Clarification 1 found for the same test).

### 3.4 Tables

The co-op game types with a difficulty vote are not a table in the new code: the game's co-op mask (image 0x721100,
G38 3.9 b, via `u3x_type_is_coop`) minus game type 18, which PS3 1.19 excludes by one explicit test. Every other
value is a single id, time or bit. The hash constant 2654435761 (4.6) is Knuth's multiplicative-hashing constant
(2^32 divided by the golden ratio, TAOCP vol. 3, 6.4).

# Part A

## 4. Behaviour

### 4.1 `u3x_party_players(session)`

0 when the u64 at session + 0x18 is 0. Otherwise, over the 10 member slots (0x120 bytes each from session + 0x680):
each slot whose u8 at +0x25 is non-zero adds 1, and 1 more when its u8 at +0x59 is non-zero. Pure read.

### 4.2 `u3x_vote_winner(vote)`

The s32 at +0xc when the s32 at +0x14 is greater than the s32 at +0x10 (signed), else the s32 at +8. Pure read.

### 4.3 `u3x_difficulty_vote_for(base, t)`

1 when `t` is not 18 and `u3x_type_is_coop(base, (int32_t)t)` is 1; else 0. (Gives 1 exactly for 4, 8, 9, 11, 13,
16, 17 with the mask the image holds.)

### 4.4 `u3x_vote_open(vote)`

Writes, in this order: s32 at +4 = -1; u8 at +1 = 0; u8 at +0 = 1; s32 at +8, +0xc, +0x10, +0x14 = 0.

### 4.5 `u3x_m02_catch(base, s, id)`

eb[2] bit 11 set: result 0. id 0xb85cb12a: `u3h_m02_diag()`[0] + 1, then `u3x_cinema(base, s)`, result 1. id
0xfb59d351: `u3h_m02_diag()`[1] + 1, then `u3h_show(base, s, 0x8236d02a, 0)`, result 1. Any other id: result 0.

# Part B

### 4.6 `u3x_m03_tick(base, s)`

1. When n002 on, stand-in on, `u3h_onb_mode()` is 2 (tested in this order) and then `u3h_n05_backout(base)` is
   non-zero: done.
2. M armed is 0: done.
3. Read pg = `u3h_active_page(base)`, ns = NS, now = the game clock (this order).
4. **Not running** (M running = 0):
   - tr = raw top id; okran = ns is 20 and (NS clock − `*u3h_n02_tarm()`), wrapping, as s32 is 0 or more.
   - When okran and (pg or, only if pg is not it, tr is 0xe35f0f1a): M running = 1, start clock = now, phase
     clock = now, phase = 1, found = 1, selected = 0; max = clamp max of M's playlist pointer; s32 at image 0x17d79e8
     = 0; NET + 0x18 = 0; set status 1; m03 diag[1] + 1; `u3h_n05_note(7, now − tarm)`; when pg is not 0xe35f0f1a,
     `u3h_n05_note(0x100, 1)`.
   - Otherwise, P = the menu's pending popup pointer: when P is not null and its u32 at +0x10 is 0x453470dd:
     `*u3h_n02_gone()` = 0 and n02 diag[0] + 1. Else:
     - when gone is 0: `u3h_n05_note(5, ns OR okran << 16)`, `u3h_n05_note(6, tr)`, `u3h_n05_note(4, (menu UI
       state AND 0xff) OR ((menu top index at +0xdb0) AND 0xff) << 8)`;
     - when pg is 0: `u3h_n05_note(0x101, 1)`;
     - when not okran: gone + 1, and when it is now 3 or more: M armed = 0, n02 diag[1] + 1. When okran and gone
       is below 3: gone + 1;
     - when M armed is still non-zero and (now − tarm), unsigned, is above 4500: M armed = 0, n02 diag[1] + 0x10000.
   - In every case of this step: n02 diag[2] = ns, n02 diag[3] = pg when non-zero else tr. Done.
5. **Running.** pg is one of the back-out pages: `u3h_m03_stop(base, 1)`; done.
6. n02 diag[2] = ns, diag[3] = pg, diag[7] = the s32 at NET + 0x40.
7. ns is not 20: n02 diag[5] = ((diag[5] AND 0xffff0000) + 0x10000) OR (ns AND 0xffff); `u3h_m03_stop(base, 0)`;
   done.
8. dt = now − phase clock (unsigned). In fill mode: `u3x_m05_tick(base, s, now, dt)`; done.
9. Short sequence by phase (all dt tests unsigned):
   - 1: s32 at 0x17d79e8 = 0 when dt < 600, 2 when dt < 1200, else 4. When dt ≥ 1800: NET + 0x18 = 1, phase 2,
     phase clock = now.
   - 2: when dt ≥ 360 and found < max: found + 1, phase clock = now. Then when found ≥ max and dt ≥ 360 (the same
     dt): set status 2 for 5 s, phase 3, phase clock = now.
   - 3: dt ≥ 1500: set status 6 for 10 s, phase 4, phase clock = now.
   - 4: dt ≥ 3000: with eb[2] bit 14 set: set status 5, phase 5. Else idx = `u3h_m03_pick(base, s)`; m03 diag[6] =
     idx; native 2 (G36 3.2) null or idx negative: set status 5, phase 5. Else **start** (below); done, step 10
     not made.
   - other phases: nothing.
10. m03 diag[2] = phase, diag[3] = found.

**Start** (shared with 4.7): native 2 (NGM, idx, null, 0xc) — in 4.7 only under its condition; net menu mode
(image 0x17d603c) = 2; `u3h_m03_stop(base, 0)`; m03 diag[5] + 1; ea[94] = 1; ea[90] + 1;
`u3x_start_game_now(base, s)`.

### 4.7 `u3x_m05_tick(base, s, now, dt)`

V and W as G36 3.5. By M phase (dt tests unsigned):

- 1: s32 at 0x17d79e8 = 0 when dt < 600, else 2. When dt ≥ 600: phase 2, phase clock = now.
- 2: when dt ≥ 360 and found < max: found + 1, phase clock = now. Then when found ≥ max and dt ≥ 600 (the same dt):
  set status 2 for 15 s, phase 3, phase clock = now.
- 3: when dt ≥ 4500: one `u3x_sym_table(s, 0x3b9a067d)` lookup (T); c0 = `u3h_m05_cand(base, s, -1)`; c1 = -1; when
  c0 ≥ 0 and T is not null: c1 = `u3h_m05_cand(base, s, map id of record c0 of T)`. c1 < 0: c1 = c0. c0 < 0: set
  status 5, phase 6; nothing else. Otherwise, in this order: V + 8 = c0, V + 0xc = c1, V + 0x10 = 0, V + 0x14 = 0,
  V + 4 = 2, V[1] = 0, V[0] = 1; when ec[2] bit 4 is set: W[0] = 1. Set status 3 for 10 s; phase 4; phase clock =
  now; `*u3h_m05_vt()` = now.
- 4: when (now − last vote time) ≥ 300 and (V + 0x10 + V + 0x14, summed as s32, taken as u32) < max: last vote time
  = now; the count at V + 0x14 + 1 when ((now · 2654435761 mod 2^32) >> 9) mod 3 is 0, else the count at V + 0x10 +
  1. Then when dt ≥ 3000:
  - n004 on: when `u3x_n04_close(base, s)` is 0: set status 5, phase 6, nothing else. Else when `u3x_n09_open(base,
    s)` is non-zero: phase 7, phase clock = now, last vote time = now, nothing else. Else phase 5, phase clock = now.
  - n004 off: V[1] = 1; set status 6 for 5 s; phase 5, phase clock = now.
- 7: when `u3x_n09_tick(base, now)` is non-zero: phase 5, phase clock = now.
- 5: due = "n04 due" (3.3) when n004 on, else dt ≥ 1500. When due: eb[2] bit 14 set: set status 5, phase 6. Else idx
  = `u3x_vote_winner(V)`; m03 diag[6] = idx; native 2 null or idx negative: set status 5, phase 6. Else: V[0] = 0,
  V[1] = 0, W[0] = 0; then **start** (4.6) where the native 2 call is made unless (n004 on and `*u3h_n04_idx()` ==
  idx); in that case `u3h_n04_diag()`[3] + 1 instead. Done (the last step is not made).
- other phases (0, 6): nothing.

Last step: m03 diag[2] = phase OR 0x100, diag[3] = found.

# Part C

### 4.8 `u3x_onm_tick(base, s)`

1. `u3h_onb_mode()` is not 1: done.
2. `u3h_onm_watch(base, s)`.
3. **Watchdog.** ns0 = NS. When ns0 is 0x2e or 0x2f: t = game clock; when O watchdog is 0: O watchdog = t, or 1 when
   t is 0; else when (t − watchdog), unsigned, is above 18000: watchdog = 0, `u3x_onm_fail(base)`. Any other state:
   watchdog = 0.
4. n002 on and `u3h_n05_backout(base)` non-zero: done.
5. **Follow the leader.** When `u3h_mq_find(base, 0x113, &ix, 0)` is 0: from = 0x24 zero bytes, d = 0x30 zero bytes,
   len = 0x30; when `u3h_mq_get_np(base, ix, from, d, &len)` is 0:
   - room = u64 at d + 0x28; host = `u3x_find_member(PS, PS's host id)`; ok = room non-zero, PS + 0xc8 non-zero,
     host non-null, `u3x_party_leader(base)` zero, `u3h_fr_strcmp(base, host, from)` zero (tested in this order,
     stopping at the first failure).
   - `u3h_mm_log` with "party leader's match room received (0x113)" when ok, else "0x113 received but not from this
     party's leader: ignored"; v = -1, b = null.
   - When ok, MS's room is not `room`, and the manager is usable: when `u3h_active_page(base)` is not 0xe35f0f1a,
     `u3h_show(base, s, 0xe35f0f1a, 0)`. P = picked playlist; O max = clamp max(P); O min = clamp min(P); O armed =
     1, running = 1, phase = 2, in room = 0, merging = 0, selected = 0; armed clock = phase clock = re-search clock =
     the game clock (one read). s32 at 0x17d79e8 = 0; NET + 0x18 = 1; set status 1. Leave (mgr, MS); Join (mgr,
     MS, room). `u3h_mm_log("following the party leader: joining its match room (304)", -1, null)`. cg_t0 = game
     clock, or 1 when it is 0.
6. O armed is 0: done. pg = `u3h_active_page`, ns = NS, now = game clock (this order); onm diag[7] = ns.
7. **Not running**: okran = ns is 20 and (NS clock − O armed clock), wrapping, as s32 is 0 or more. When okran and
   (pg or, only if pg is not it, the raw top id is 0xe35f0f1a): running = 1, phase = 1, phase clock = now, selected
   = 0; P = picked playlist; max = clamp max(P); min = clamp min(P); in room = 0; merging = 0; s32 at 0x17d79e8 = 0;
   NET + 0x18 = 0. When `u3x_online_search(base)` is negative: log "search could not be sent" (-1, null),
   `u3x_onm_fail(base)`, done. Else log "search sent (309), playlist minimum " with min and " players"; set status
   1; onm diag[1] + 1. Otherwise, when (now − armed clock), unsigned, is above 4500: armed = 0. Done.
8. pg is a back-out page: stop(cancel) (4.9); done.
9. dt = now − phase clock (unsigned); onm diag[2] = phase.
10. **Phase 1**: ns is not 6 or dt ≥ 9000: log "no search result in 30 s (or the search was replaced), net state "
    with ns, then `u3x_onm_fail`; done. s32 at 0x17d79e8 = 0 when dt < 3000, 2 when dt < 6000, else 4. MS + 0xb0 is
    0: done. rooms = u32 at MS + 0x218; O word 7 = rooms; onm diag[3] = rooms; log "search result (310): rooms found
    " with rooms. s32 at MS + 0xb4 negative: stop(cancel); done. Phase clock = re-search clock = now. rooms non-zero:
    phase 2, NET + 0x18 = 1, background flag = 1, `u3x_mm_ev6(base, 0x6a6830, 0)`, `u3h_onm_state(base, 0xb)`, onm
    diag[4] + 1; done. Else host (4.9) negative: log "hosting failed (303 not sent)" (-1), `u3x_onm_fail`; done. Else
    log "no room found: hosting one (303), up to " with max and " players"; phase 2; NET + 0x18 = 1; onm diag[5] + 1.
11. **Phase 2**: ns is 0xb, 0xf, 0x28, 0x13 or 3: background flag = 1. ns is 0x13: `u3h_onm_state(base, 0xb)`. MS
    room 0: when dt ≥ 9000, log "no room joined / created in 30 s (no 305)" (-1) and `u3x_onm_fail`; done. When in
    room is 0: in room = 1, phase clock = re-search clock = now, background flag = 0, `u3h_onm_state(base, 0x12)`,
    log "in the match room (305), members " with members (4.9), then tell the party (4.9) with MS's room. cnt =
    members; onm diag[3] = cnt. When cnt differs from a private u32 "last count" (starts 0, never reset): last
    count = cnt, log "match room members now " with cnt. cnt ≥ min: log "playlist minimum reached: vote / countdown
    with " with cnt and " players", ready (4.9), done. host = MS own id equals MS host id. When merging and MS +
    0xb0 non-zero: merging = 0; for i from 0 while i < (u32 at MS + 0x218) and i < 20: r = room id i; when r is
    non-zero and not MS's room: when the manager is not usable stop looking; else Leave (mgr, MS), Join (mgr, MS,
    r), in room = 0, phase clock = now, onm diag[4] + 1, stop looking. Done. Else when host and (now − re-search
    clock) ≥ 9000: re-search clock = now; re-search (4.9).
12. Other phases: nothing.

All texts are logged through `u3h_mm_log(text, v, b)`; "with X" = v = X, else v = -1; b = null unless given.

### 4.9 Private helpers of part C (old own C functions moved here)

- **stop(cancel)**: NET + 0x40 = 0; NET + 0x18 = 0; with cancel: `u3h_onm_state(base, 0)`, u8 0x16e7171 = 0, u8
  0x16e7172 = 0. O armed, running, phase = 0; background flag = 0; onm diag[6] + 1.
- **host**: manager not usable: -1. Else Create (mgr, MS, O max, 0, 0xffffffff, 0); result = low 32 bits of its
  s64 result as a signed int.
- **re-search**: manager not usable: nothing. Search (mgr, MS, 0, 0, -1, 0, 0, 0); when its s64 result is 0 or
  more: merging = 1.
- **members**: number of MS's 10 member slots with u8 +0x25 non-zero.
- **ready(cnt)**: now = game clock; M playlist pointer = picked playlist; M max = O max; found = cnt; selected = 0;
  armed = 1; running = 1; phase = 3; start clock = phase clock = now; set status 2 for 15 s; O armed, running,
  phase = 0; onm diag[2] = 9.
- **tell the party(room)**: nothing when PS + 0xc8 is 0, PS room is 0, `u3x_party_leader(base)` is 0 or room is 0
  (in this order). n = `u3h_members(PS, list)`; d = 0x30 zero bytes, its first 0x14 bytes copied from image
  0x17d6010, u64 at d + 0x28 = room; for each listed slot whose u32 at +0x2c is not PS's own id:
  `u3h_mq_send(base, slot, 0x113, d, 0x30)` and count it. Log "told the party (0x113) to follow into the match
  room, members told: " with the count.

### 4.10 `u3x_onm_fail(base)`

In this order: u8 NET + 0x28 = 1; s32 NET + 0x40 = 0; u8 NET + 0x18 = 0; game call 0x652200(menu, 0);
`u3h_onm_state(base, 0)`; u8 0x16e7171 = 0; u8 0x16e7172 = 0; O armed, running, phase = 0; background flag = 0;
onm diag[6] + 1.

## 5. Test scenarios

[fake] = old against new on a zeroed fake image up to 0x1b68818 with call-recording stubs (natives, game calls
0x652200 / 0x776530, the manager table entries 4 to 7, every NEW/G36 export and other-group function of 2.2).
Compare results, image memory, helper words and call order.

- **A [fake, pure]**: party count: no room → 0; slots in use with and without +0x59 (also 0x80) → 1 / 2 each; 10
  slots full with seconds → 20. Vote winner: (2,3) second, (3,3) and (4,1) first, negative counts. Difficulty vote:
  types 0..20 and 0x10004 with the mask 0x72b1 stubbed at 0x721100 (18 → 0). Vote open on a preset block. m02:
  switch set, both ids, another id (counters and the one call).
- **B [fake]**: m03 tick: each step-1 condition alone; not armed; arming with ok ran (pg page, raw top page only,
  clock one behind), popup present, gone counting to 3 without okran, okran with gone < 3, the 4500 timeout,
  diag words; running: each back-out page, ns ≠ 20 (diag[5] pattern), short sequence phases at dt edges (359/360,
  599/600, 1199/1200, 1799/1800, 1499/1500, 2999/3000), eb[2] bit 14, pick -1, native 2 null, start call order.
  m05: each phase at its edges; cand -1, c1 -1 → c0; ec[2] bit 4; fake votes at 299/300, the sum limit, both hash
  outcomes; n004 on / off; n04_close 0; n09_open 1 / 0; n09_tick; phase 5 with n04 due at the signed edge, with
  `*u3h_n04_idx()` equal / unequal; phases 0 and 6 do nothing but the diag words.
- **C [fake]**: mode ≠ 1; watchdog (0x2e first tick with clock 0 → 1; 18000 / 18001); back-out; 0x113 with each ok
  condition failing, same room, manager null, show only off the page; driver arming / timeout; phase 1 each exit
  (ns ≠ 6, 9000, no result, error, rooms > 0, host fail, host ok); phase 2 each branch incl. merging with 0, 1, 25
  rooms (own room skipped, limit 20), manager unusable, re-search at 8999/9000 for host and guest; fail order.
- **[live]**: offline Find Game through to the match (fill and short sequence); online search, host, join, leader
  follow with two PCs; failure paths.

## 6. Things that look like bugs or dead code (keep)

1. 4.6 / 4.7: in "slots filling" the last test uses the dt from before the slot was added, so one tick can add the
   last slot and move on.
2. 4.7 phase 3: with only one map, both candidates are the same net game. Phase 6 is a dead end until backed out.
3. 4.7 phase 4: the vote limit adds both counts as s32 and compares unsigned with max (the own vote is not counted
   separately).
4. 4.8 step 7: the arming timeout runs only while not running; step 10 runs the result handler always for room 0.
5. 4.8 step 11: the "last count" log memory is never reset between searches; a merge leaves merging = 0 also when
   the manager is unusable.
6. host's result is the low 32 bits of the backend result; re-search tests all 64 bits.
7. `u3x_onm_fail` backs the menu one page whatever page is open.
8. `u3x_vote_open` also zeroes +8 .. +0x14 (on the map vote those are the candidates; it is used for W only).

## 7. What stays as it is

The callers; the C state and statics (M, O, `m05_vt`, `n02_*`, `n05_*`, `m02_d`, `onm_d`, `mm_bg`, `cg_t0`); the own
C functions `m03_stop`, `m03_pick`, `m05_cand`, `n05_backout`, `n05_note`, `mm_log`, `lnb_follow_tick`,
`lnb_np_ready` (reached through the exports of 2.2); the watch part of the old `onm_tick` (moved unchanged into the
C function behind `u3h_onm_watch`).

## 8. Open questions

1. The meaning of the 0x14 bytes at image 0x17d6010 that the 0x113 message carries (copied as they are).
2. Whether the running game holds the co-op mask at image 0x721100 (G38 open question 2; the image file does).
3. Whether game call 0x776530 removes the 0x113 message from the queue (`u3h_mq_find` is called with remove 0); the
   new code makes the same calls either way.

## 9. Clarifications (tester, 2026-11-14)

Facts confirmed by the old-against-new test (`reimpl2\S01\test_s01.cpp`, v7.212 against part_a / part_b / part_c, at
-O1 and -O2) and by its directed checks.

1. **6.4 and 4.8 step 10 do not contradict each other.** The result handler (`u3x_mm_ev6(base, 0x6a6830, 0)`, then
   net state 0xb) runs only when the result holds at least one room; with 0 rooms the driver hosts (Create, then
   "no room found: hosting one" or the failure step). "For room 0" in 6.4 means the handler's event always names the
   first room of the result (the third argument, the event's index, is always 0), whatever the number of rooms. 4.8
   step 10 is exact as written.
2. 4.8 step 5: the session manager pointer is read once, after the room test and before `u3h_show` / the picked
   playlist; that pointer is the one passed to Leave and Join even when a call in between changes the pointer at
   image 0x17dd768. The table pointer inside the manager object is read again for each of the two calls (a change of
   it by Leave reaches Join). The MS room compare comes before the manager test (no call lies between them).
3. 4.8 step 11 (merge): the manager pointer and its table are read at the first listed room that is non-zero and not
   MS's room, once; with an unusable manager no call is made and merging stays 0. Only the first 20 rooms are looked
   at: a first other room at index 19 is joined, one at index 20 is not (merging still becomes 0).
4. "Picked playlist" (G36 3.5): v7.212 reads the profile pointer at image 0x17dd610 and its u32 at +0x774 BEFORE the
   `u3x_sym_table(s, 0xc1b0be0d)` lookup, then calls `u3x_pick_playlist(table, id)`.
5. Tell the party (4.9): v7.212 has no limit of its own on the count `u3h_members` returns (that function fills at
   most 10 slots); it compares each listed slot's id with PS's own id (u32 at PS + 0x12e4) read anew for every slot,
   after the sends before it. Tested with 0, 3 and 10 listed members; every listed slot that is not this
   player is sent to and counted, also when the same slot is listed twice.
6. 4.8 step 10: after the "search result (310): rooms found" log line, the "rooms non-zero" test reads the stored O
   word 7 (written just before the log call), not a copy.
7. 3.3 "n04 due" is a plain signed comparison (as G36 Clarification 1): net clock 0x7fffffff with deadline
   0x80000000 is due (the match starts); net clock 0x80000005 with deadline 0x7ffff005 is not; equal values are due.
8. Host (4.9) uses the low 32 bits: Create results 0x100000000 and 0x8000000000000000 host successfully, 0xffffffff
   and 0x80000000 fail. Re-search uses all 64 bits: 0, 0x80000000 and 0xffffffff set merging; results with bit 63
   set do not. Re-search runs only for the room's host at 9000 ticks or more (8999 nothing; a guest never).
9. Watchdog (4.8 step 3): the first tick in state 0x2e at game clock 0 stores 1; 18000 ticks later nothing happens,
   18001 ticks later the failure step runs (watchdog 0). Online arming timeout (step 7): 4500 ticks keeps the driver
   armed, 4501 disarms it; it is tested only while not running.
10. 6.1, 6.2, 6.3, 6.5, 6.7, 6.8 hold as written: one tick can add the last slot and move on (fill: dt 600; short:
    dt 360); with the second candidate lookup failing both candidates are the first; counts -1 and 0 add to
    0xffffffff, which is not below max, so no stand-in vote is cast; a merge leaves merging 0 also without a usable
    manager; the failure step always calls game function 0x652200; `u3x_vote_open` zeroes +8 .. +0x14.
11. 4.3 (planned): the PS4 image holds the bytes b1 72 00 00 at image 0x721100 (file offset 0x725100). With them
    `u3x_difficulty_vote_for` gives the same answer as v7.212's literal list for every type -5 .. 40 and for
    0x10004, 0x10012, 0xfffffffc, 0x80000004. Skipping the `u3x_type_is_coop` call for type 18 changes nothing (that
    function is a pure read).
12. Every time and count test of 4.6 to 4.8 is unsigned where the spec says so; edges tested: 299/300/301,
    359/360/361, 599/600/601, 1199-1201, 1499-1501, 1799-1801, 2999-3001, 4499-4501, 5999/6000, 8999-9001,
    17999-18001 and clock wraps.
13. Accessor call counts (not behaviour): v7.212 asks the raw top page object once per not-running tick of 4.6 and,
    in 4.8 step 7, only when pg is not the matchmaking page; `u3x_find_member` is a pure read in place of v7.212's
    inline search for the same slot.
14. Open question 3 (whether game call 0x776530 removes the message) cannot be settled here: it needs the live game.
