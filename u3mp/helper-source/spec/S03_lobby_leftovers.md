# S03. Lobby leftovers: menu retags, lobby buttons, options rows, click registration, lobby stage guard, lobby scene and character side

Behaviour description for a reimplementation (reimpl2 pipeline, describer's document, final sweep group S03). It
says what the functions do, not how the old ones were written. Offsets are hexadecimal when prefixed with 0x,
decimal otherwise. "Image offset X" means the address `base + X`, `base` = the game image base. All values are
little-endian; "u8 / u16 / u32 / s16 / s32 / u64" are unsigned / signed integers of that width, "pointer" is 8 bytes.
"Plausible pointer" = `ptr_ok` of CONVENTIONS.h (at least 0x10000, below 2^47, multiple of 8).

**There is no floating point in this group.** No function reads, computes or passes a float; PIPELINE.md's Float rule
has nothing to apply to.

Related documents, referred to and not repeated: `G31_mp_page_events.md` (the lobby buttons source that calls the
two button rules of part 1), `G39_mp_startup.md` (level manager, its want lists and request block 3.2, natives 3.5,
state object 3.4, `u3x_stage_tick` that calls `u3h_match_stage_cleanup` and `u3h_register_lobby_click`),
`G59_lobby_scene.md` (menu scene at image 0x17bba90, customisation manager CM at image 0x17f9938, 0x5b7040 /
0x5b6e40), `G35_custom_game_start.md` (calls `u3h_cg_commit_selection`), `G38_custom_settings.md` (net game table),
`G23_boosters.md` 3.9 (the name-id rule), `G08_lan_room_client.md` (the frame step that calls `e03_side`).

The group is written as **three independent parts**. No part calls another part's functions.

| Part | Subject | Exported names to deliver |
|---|---|---|
| 1 | Menu data: widget retags, the gate of the game's datasource dispatcher, lobby button rules, options rows, blank-row test, lobby click registration | `u3x_retag_page`, `u3x_retag_party`, `u3x_retag_lobby_box`, `u3x_retag_page_source`, `u3x_retag_treasure_list`, `u3x_retag_cinema_page`, `u3x_native_gate_closed`, `u3x_lobby_button_shown`, `u3x_lobby_button_label`, `u3x_rpm_source`, `u3x_k04_blank`, `u3x_register_lobby_click` |
| 2 | The lobby stage: unwant, heap check, stage guard, match cleanup | `u3x_stage_unwant`, `u3x_stage_wont_fit`, `u3x_stage_guard`, `u3x_match_stage_cleanup` |
| 3 | Lobby scene and character: scene shutdown, live team side, character art name, custom game selection | `u3x_lobby_scene_shutdown`, `u3x_e03_side`, `u3x_lg01_slot0`, `u3x_cg_commit_selection` |

No function of the S03 list was judged own glue: each holds a rule, a call sequence or a list of ids taken from the
game (section 7 says which ones are mostly own design around a small game part).

## 1. Purpose and callers

The PS4 build lost several menu sources, a click registration and PS3 behaviour of the multiplayer lobby. The helper
redirects the stripped widgets to its own datasource handler (enum 0xcf, the "LAN tag" in the parameter word),
reproduces the game dispatcher's gate where it bypasses it, restores the lobby list's click registration, guards the
lobby's 3D stage and replays a few PS3 call sequences. All of it runs on the game's main thread; no locking.

- Part 1 retags: called once per menu page by the C function `prepare_lobby_menu` (hook kind 8, the menu load; stays
  C): `retag_page(page, 4, 0)` and `retag_lobby_box(page, B0)` for PAGE_MULTIPLAYER; `retag_page(page, 9, OPTS)` for
  PAGE_LOBBY; `retag_page(page, 1, 0)` for PAGE_MULTIPLAYER_CINEMA_LIST; `retag_cinema_page` for PAGE_CINEMA;
  `retag_party` for every other page; then for every page `retag_page_source`, `retag_treasure_list` and others.
  OPTS = bit 0 from bit 24 and bit 1 from bit 26 of the u32 at `s` + 0x3d78; B0 = OPTS bit 0. When the
  PAGE_MULTIPLAYER retag answered 1, the hook calls `register_lobby_click(base)`; `u3x_stage_tick` (G39) also calls
  it through `u3h_register_lobby_click`.
- `native_gate_closed`: first step of about 30 helper datasources (C and C++, through the exports
  `u3h_native_gate_closed` and `u3h_gate_closed`).
- `u3x_lobby_button_shown`, `u3x_lobby_button_label`: called by the lobby buttons source (C `lobby_buttons`, G31).
- `rpm_source`: the options rows source; called by the hook dispatcher for kind 2 (every datasource request).
- `k04_blank`: called by the C hook `k_kind` for kinds 240 / 241 / 242 (hooks in the row focus, measure and draw
  functions of the menu); 1 makes the hook skip the row.
- Part 2: `stage_guard(base, s)` once per frame (hook kind 16); `match_stage_cleanup` through the export
  `u3h_match_stage_cleanup` from `u3x_stage_tick` (G39); `stage_unwant` from parts 2, the C functions of the
  multi-all / anim-proto keeps (S02) and through `u3h_stage_unwant` from G39.
- Part 3: `u3x_lobby_scene_shutdown` through C `k34_shutdown` from the NetShutdown body (G39);
  `e03_side(base, s)` from the frame step `u3h_lnb_frame_parts(base, s, 0)` (G08), after the lobby scene step;
  `lg01_slot0` from the C guard `lg01_hold` (hook kind 248, the lobby character placement); `cg_commit_selection`
  through `u3h_cg_commit_selection` from the Custom Game start (G35).

## 2. Interface

### 2.1 Functions to deliver (all `extern "C"`)

| Part | Signature | Result | C function that becomes a one-line wrapper (passes all its arguments) |
|---|---|---|---|
| 1 | `int u3x_retag_page(uint8_t *page, uint32_t count, int opts)` | 1 / 0 | `retag_page(page, count, opts)` |
| 1 | `void u3x_retag_party(uint8_t *page)` | - | `retag_party(page)` |
| 1 | `void u3x_retag_lobby_box(uint8_t *page, int off)` | - | `retag_lobby_box(page, off)` |
| 1 | `void u3x_retag_page_source(uint8_t *page)` | - | `retag_page_source(page)` |
| 1 | `void u3x_retag_treasure_list(uint8_t *page)` | - | `retag_treasure_list(page)` |
| 1 | `void u3x_retag_cinema_page(uint8_t *page)` | - | `retag_cinema_page(page)` |
| 1 | `int u3x_native_gate_closed(uintptr_t base, int32_t *event)` | 1 = answered / 0 | `native_gate_closed(base, event)` (both exports keep calling it) |
| 1 | `int u3x_lobby_button_shown(int setup_button, int guest)` | 1 / 0 | exists in C++ (same name, replaced) |
| 1 | `uint32_t u3x_lobby_button_label(int setup_button, int guest, int custom_game, uint32_t own_label)` | label | exists in C++ (same name, replaced) |
| 1 | `int u3x_rpm_source(uintptr_t base, int32_t *event, void *s, uint8_t *eu_row)` | 1 / 0 | `rpm_source(base, event, s)`, passing also the C variable `eu_row` (a widget pointer that the C function `h21_retag` keeps) |
| 1 | `int u3x_k04_blank(uintptr_t base, uint8_t *row)` | 1 / 0 | `k04_blank(base, row)` |
| 1 | `int u3x_register_lobby_click(uintptr_t base)` | 1 / 0 | `register_lobby_click(base)` (export `u3h_register_lobby_click` stays) |
| 2 | `int u3x_stage_unwant(uintptr_t base, uint32_t sid)` | entries removed | `stage_unwant(base, sid)` (export `u3h_stage_unwant` stays) |
| 2 | `uint32_t u3x_stage_wont_fit(uintptr_t base, uint8_t *level)` | pages or 0 | `stage_wont_fit(base, level)` |
| 2 | `void u3x_stage_guard(uintptr_t base, void *s)` | - | `stage_guard(base, s)` |
| 2 | `void u3x_match_stage_cleanup(uintptr_t base, void *s, void *find, void *show)` | - | `match_stage_cleanup(base, s, find, show)` (export `u3h_match_stage_cleanup` stays) |
| 3 | `void u3x_lobby_scene_shutdown(uintptr_t base)` | - | exists in C++ (same name, replaced; C `k34_shutdown` forwards) |
| 3 | `void u3x_e03_side(uintptr_t base, void *s)` | - | `e03_side(base, s)` (G08 may call `u3x_e03_side` directly instead of its proposed `u3h_e03_side`) |
| 3 | `const char *u3x_lg01_slot0(uintptr_t base, const uint8_t *desc)` | name or null | `lg01_slot0(base, desc)` |
| 3 | `void u3x_cg_commit_selection(uintptr_t base, void *s)` | - | `cg_commit_selection(base, s)` (export `u3h_cg_commit_selection` stays) |

`find` and `show` are native 42 and native 45 of G39 3.5 as plain pointers. No float or double crosses any function.

### 2.2 Called functions of the C part, other groups and the shared tools

| Name | Signature | What it does | Part |
|---|---|---|---|
| `u3h_ext` / `ext_ea` / `ext_eb` | CONVENTIONS.h | the helper's state block and its ea / eb words; ec[n] = u32 at `u3h_ext()` + 0x4450 + 4n | all |
| `u3h_text` | `const char *u3h_text(uintptr_t base, void *s, uint32_t id)` | localized text of a text id; never null | 1 |
| `u3h_crc_sid` | `uint32_t u3h_crc_sid(const char *text)` | the game's name id (G23 3.9 rule) | 2 |
| `u3x_mp_lan_or_online` | `int u3x_mp_lan_or_online(uintptr_t base)` | non-zero while a LAN or online multiplayer session exists (G39 2.4) | 2 |
| `u3x_loc_now` | `int32_t u3x_loc_now(uintptr_t base)` | the local engine clock (s32 at image 0x17b4ba4), 300 ticks per second | 2 |
| `u3x_netgame_cur` | `uint8_t *u3x_netgame_cur(uintptr_t base, void *s)` | the current net game entry (net game table 3.3, index s16 at image 0x17d167c) or null; uses the state object's lookup (s + 656) | 2 |
| `u3x_sym_table` | `uint8_t *u3x_sym_table(void *s, uint32_t id)` | value of a script symbol through the state object's lookup (s + 656), or null | 3 |
| `u3x_pm_ngentry` | `uint8_t *u3x_pm_ngentry(uintptr_t base, void *s)` | the selected net game entry or null (G30) | 3 |
| `u3x_custom_side` | `void u3x_custom_side(int32_t *custom, int side, int inplace, const void *shown, uint32_t shown_id, void (*refresh)(int32_t *custom))` | exists (earlier rewrite, stays): stores side 0 / 1 in CM word 0 and refreshes CM's live preview character unless `inplace` is 0 and that preview is the scene character (`shown`, `shown_id`) | 3 |
| `u3h_cg_log` | `void u3h_cg_log(const char *text, int32_t value)` | one session log line "custom game: " + text + value | 3 |

## 3. Game facts

### 3.1 Name ids: a rule, not a table

Every widget id and page id below is the **name id** of the widget's or page's name in the game's menu data: the
CRC of G23 3.9 (polynomial 0x04C11DB7, most significant bit first, start 0, no final inversion; = `u3h_crc_sid`).
The names are written in the menu data file the game loads (PS4 `menu.bin`), and every id used by the old code was
checked against this rule. The new code computes the ids from the names (once or on every call, not observable)
and holds no literal list of ids.

| Name | used in |
|---|---|
| PAGE_MULTIPLAYER, PAGE_LOBBY, PAGE_MULTIPLAYER_CINEMA_LIST, PAGE_CINEMA, PAGE_BOOSTERS, PAGE_NET_TREASURES, PAGE_EULA, PAGE_CREDITS | callers (section 1), 4.4, 4.5, 4.10 |
| LIST_MAIN_MENU, LIST_PLAYERS, LIST_CINEMA | 4.1 |
| TAB_NET_PROFILE, NET_EMBLEM_LAYER, NET_EMBLEM_TYPE, LIST_BOOSTERS, LIST_MEDAL_STREAKS, LIST_SPOILER_CLASS | 4.2 |
| BUTTON_CINEMA_MPEG, BUTTON_CINEMA_PROTECT, BUTTON_CINEMA_UNPROTECT | 4.6 |
| SELECT_LAN_PROFILE | 4.12 |
| `none` | 4.17 |
| `san-upper-multi-stage` (a level name, the retail lobby stage; its id 0x703ab0f4) | 4.15 |

Single ids that are not computed from a known name: text ids 0x92db2b96 (start label in custom-game mode),
0x63091cd0 (normal start label), 0x54b6e963 (MENU_TITLE_EULA; may be computed from that name too); script symbols
0x0dec97a4 (map table), 0x3b9a067d (net game table), 0xbd3bb3b9 (character table).

### 3.2 Menu data (read at run time)

- Page array: pages of 0x80 bytes; page id u32 at page + 8. Page datasource descriptor at page + 0x48 (its first
  8 bytes are the pointer to the widget collection): type u32 at page + 0x54, enum u32 at page + 0x58, parameter u32
  at page + 0x64.
- Widget collection: u32 count at +0, pointer at +8 to `count` widgets of 0x68 bytes. Widget: id u32 at +8; a style
  word u32 at +0x10; its datasource descriptor starts at +0x24: type u32 at +0x30 (21 = a datasource), enum u32 at
  +0x34, parameter u32 at +0x40 (0 = none); target page id u32 at +0x50 (buttons that open a page).
- **Retag**: enum := 0xcf (the dispatcher's free entry that the helper answers; exempt from the gate 3.3),
  parameter := a tag 0x4c414e00 | e, where e is the low byte the rule names (usually the widget's original enum).
- Enum numbers named in 4.1 / 4.2 / 4.5 / 4.6 are the datasource dispatcher's entry numbers that the PS4 menu data
  gives these widgets (identifiers of game callbacks, like function offsets; see open question 1).
- A datasource request ("event", `int32_t *`): word 0 = operation (1 text, 6 count, 8 visible, ...), word 1 = the
  integer answer, pointer at byte +0x40 = the text answer, pointer at byte +0x98 = the source descriptor (u32 at
  descriptor + 8 = the widget's own text id; u32 at descriptor + 0x1c = the parameter, i.e. the tag).

### 3.3 Game data and functions

| Image offset | Type | Meaning |
|---|---|---|
| 0x13ac250 | text | the game's empty string |
| 0x1678ef0 | vtable | the style-6 menu row object |
| 0x16e7be0, 0x16e7bf0, 0x16e7cac | u32, u64, u32 | loading heap (object at 0x16e7bb8): pages total (+0x28), pages used (+0x38, low 32 bits used), peak (+0xf4) |
| 0x17b4ba4 | s32 | local clock (`u3x_loc_now`) |
| 0x17bba90 | object | menu scene SC (G59): s32 state at +0, s32 phase at +4, character handle {pointer at +0x1460, u32 id at +0x1468} |
| 0x17bd080 | object | the menu: s32 at +8 (non-zero = menu up), s32 at +0xc (0 = not busy) |
| 0x17be5dc, 0x17be5e0 | u32, array | the menu click table: count; entries of 24 bytes {u32 id, 4 bytes 0, pointer callback, pointer context}, capacity 128 |
| 0x17cb060 | object | net game manager NGM: pointer at +0x370 = the first local net player; s16 at +0x661c (= image 0x17d167c) selected net game; 0x18 bytes at +0x6a38 the kept settings; stage name text at +0x74b8 (= image 0x17d2518) |
| 0x17d1868 | u32 | match state; 2 = in a match |
| 0x17d5c61 | u8 | multiplayer active (also the menu-ready byte of the dispatcher, net state 0x17d5c58 + 9) |
| 0x17d603c | u32 | net menu mode; 2 = custom game / party flow |
| 0x17f9938 | s32 array | customisation manager CM (G59); CM word 0 = side |
| 0x18bed94 | u32 | loading heap: most pages one package may take |
| 0x1afcf10 | object | level manager (G39 3.2): level want list count +0x28 / entries +0x30; actor want list +0x38 / +0x40; entries 0x18 bytes, sid u32 at +8 |
| 0x1b68810 | u32 | front-end state machine; 0x2c..0x2e = the Start Game window |

Game functions (plain calls, integer / pointer arguments): 0x672c60 the menu's list click callback (only stored);
0x5b7040(SC, 1) leave the menu scene; 0x5b6e40(SC) the scene's team pass; 0x5b47c0(SC) start the character's exit
(no-op once it plays); 0x5b6c40(SC) destroy the scene character only; 0x7927c0(player) → s32 team of a net player;
0x995d50(CM) the preview refresh (passed to `u3x_custom_side`); 0x11f2bf0(u32 symbol) → pointer to a pointer to the
symbol's value, null when missing (no error message). Virtual calls: the style-6 row's datasource object, 7th entry
(byte 0x30) → `const char *`; the object at character + 0x710, 13th entry (byte 0x60) → bool (low 8 bits).

Level / package: level state u32 at +0x30 (8 loaded, 0xc error); flags byte at +2; package count u32 at +0x78,
package pointers inline at +0x80. Package: status u32 at +0x20 (7 = waiting for pages), header version u16 at
+0x238 (0xa79 when read), pages needed u32 at +0x248.

Map table (symbol 0x0dec97a4, value via native 13 = s + 768 + 13·8): s32 count at +0, pointer at +8 to entries of
0x98 bytes: s32 map id at +0, pointer to the stage name at +0x80, u32 stage sid at +0x88. Net game table (symbol
0x3b9a067d): u32 count at +0, pointer at +8 to entries of 0xd0 bytes, game type u32 at +0xc, map id s32 at +4.
Character table (symbol 0xbd3bb3b9): s32 count at +0, pointer at +8 to entries of 0x70; entry + 0x28 = pointer L,
L + 8 = pointer to the u32 symbol of a part list; part list: s32 count at +0, pointer at +8 to entries of 0x38:
pointer to the art name at +8, u32 name id at +0x10.

### 3.4 Helper state

State object `s` (G39 3.4): native k pointer at s + 768 + 8k (2 = select net game 0x720610: (NGM, s32 index,
pointer to kept settings or null, u32 0xc); 13, 40, 41, 42, 45 as G39 3.5); u32 at s + 0x20c0 bit 6 = stage guard off.

Block `u3h_ext()`: STG = 16 u32 at + 0xb00; BAD = 16 u32 at + 0xb40; SW = 32 u32 at + 0x2130; RB = 8 u32 at
+ 0x31b0; ea / eb / ec at + 0x3c50 / + 0x4050 / + 0x4450. Switches: ea[0] bit 3 (page sources off), eb[0] bit 0
(cinema buttons native), eb[1] bit 3 (scene block off), eb[1] bit 28 (side in place), eb[1] bit 29 (live side off),
eb[2] bit 9 (blank-row rule off), ec[1] bit 14 (Credits shown in MP). Counters: ea[14], eb[26], eb[231], RB[7].
Own diagnostics of the new code (location free, U3X_BSS): GATE (2 u32) for 4.7, SIDE D[0..7] for 4.18.

## 4. Behaviour

### Part 1

#### 4.1 `u3x_retag_page(page, count, opts)`
Collection C = pointer at page + 0x48; null → 0. n = C count. n must equal `count`, except that n = 10 is accepted
when `count` is 9 (PS3 1.19's PAGE_LOBBY has a tenth widget); else 0. Items null → 0. Walk the widgets in order;
for widget w (id I, enum E, parameter P) find its tag T:
- I = LIST_MAIN_MENU: T = 0x4c414eb3; I = LIST_PLAYERS: T = 0x4c414ead; I = LIST_CINEMA: T = 0x4c414e39;
- otherwise, only when `count` is 9: E is 0x1e, 0x32 or 0x31, or E = 0x30 and bit 1 of `opts` is clear:
  T = 0x4c414e00 | E; or E = 0xcf and P >> 8 = 0x4c414e: T = P;
- otherwise the widget is skipped.
Then: type ≠ 21 → return 0 at once (earlier widgets stay changed). E = 0xcf and P = T → next widget. E ≠ (T & 0xff)
or P ≠ 0 → return 0. Else retag with T. After the walk: 1.

#### 4.2 `u3x_retag_party(page)`
C null, items null or count > 64 → nothing. Every widget of type 21 with parameter 0 is retagged with tag
0x4c414e00 | E when one rule holds (an item matches at most one rule; the order of writes is not observable):
(I = LIST_PLAYERS, E = 0xad), (I = TAB_NET_PROFILE, E = 0xc3), (NET_EMBLEM_LAYER, 0xb5), (NET_EMBLEM_TYPE, 0xbe),
(LIST_BOOSTERS, 0xd3 or 0x43), (LIST_MEDAL_STREAKS, 0x44), (LIST_SPOILER_CLASS, 0xbf), any id with E in
{0xa6, 0xa7, 0xd9, 0xd4, 0x89, 0x31}, id 0 with E in {0x29, 0xc2, 0x6e}.

#### 4.3 `u3x_retag_lobby_box(page, off)`
`off` non-zero → nothing. C null, items null, count > 64 → nothing. Each widget with type 21, E = 0x89, P = 0:
retag with 0x4c414e89 and RB[7] + 1.

#### 4.4 `u3x_retag_page_source(page)`
ea[0] bit 3 → nothing. Page id PAGE_BOOSTERS with page enum 0x43: tag 0x4c414e43; page id PAGE_NET_TREASURES with
page enum 0xd4 or 0xd5: tag 0x4c414ed5; else nothing. Page type ≠ 21 or page parameter ≠ 0 → nothing. Write page
enum 0xcf, page parameter = tag; ea[14] + 1.

#### 4.5 `u3x_retag_treasure_list(page)`
Page id ≠ PAGE_NET_TREASURES → nothing. C null, items null, count > 64 → nothing. Widgets with type 21, E = 0xd5,
P = 0: retag with 0x4c414ed5. No switch, no counter.

#### 4.6 `u3x_retag_cinema_page(page)`
eb[0] bit 0 → nothing. C null, items null, count > 64 → nothing. Widgets with type 21, P = 0 and (BUTTON_CINEMA_MPEG,
0x3c), (BUTTON_CINEMA_PROTECT, 0x3d) or (BUTTON_CINEMA_UNPROTECT, 0x3f): retag with 0x4c414e00 | E; eb[26] + 1.

#### 4.7 `u3x_native_gate_closed(base, event)`
The game's dispatcher calls a datasource's callback only while the u8 at image 0x17d5c61 is non-zero; otherwise it
just answers the empty string. A widget retagged to 0xcf skips that gate, so: byte non-zero → 0. Else the text
pointer (event + 0x40) := base + 0x13ac250; GATE[0] + 1; GATE[1] := event word 0; result 1.

#### 4.8 `u3x_lobby_button_shown(setup_button, guest)` and 4.9 `u3x_lobby_button_label(...)`
PS3 rule: only the party leader sets the game up. Shown: 0 when both `setup_button` and `guest` are non-zero, else 1.
Label: set-up button → 0 (no text) for a guest, `own_label` otherwise; start button → 1 (the waiting text) for a
guest; else 0x92db2b96 when `custom_game` is non-zero, 0x63091cd0 when it is 0.

#### 4.10 `u3x_rpm_source(base, event, s, eu_row)`
D = descriptor pointer (event + 0x98); null → 0. Tag = D + 0x1c; not in 0x4c414830..0x4c414835 → 0. Row k = tag −
0x4c414830, op = word 0. EULA mode M = 1 only when k = 5, base ≠ 0, u8 at 0x17d5c61 ≠ 0, u32 at 0x17d1868 ≠ 2 and
ec[1] bit 14 clear. When k = 5 and `eu_row` is not null: u32 at eu_row + 0x50 := PAGE_EULA if M, else PAGE_CREDITS
(written on every request of row 5, any op). Then op 8: word 1 := 1; op 1: text answer := `u3h_text(base, s, M ?
0x54b6e963 : u32 at D + 8)`; op 6: word 1 := −1; other ops: nothing. Result 1.

#### 4.11 `u3x_k04_blank(base, row)` (PS3 rule: a row whose label is empty is not drawn, measured or focused)
0 when: eb[2] bit 9 set; `row` not plausible; u64 at row ≠ base + 0x1678ef0; its datasource object O (pointer at
row + 0x18) not plausible; O's vtable pointer is 0 (only tested for null). Else call O's 7th virtual function (O) →
text t; result 1 when t is null or t[0] is 0, else 0.

#### 4.12 `u3x_register_lobby_click(base)`
n = count (0x17be5dc); n > 128 → 0. Walk entries 0..n−1 in order: id = LIST_MAIN_MENU → return at once 1 when its
callback is base + 0x672c60 and its context 0, else 0 (nothing written). id = SELECT_LAN_PROFILE with callback
base + 0x672c60 → "ready". After the walk: not ready or n = 128 → 0. Else entry n: all 24 bytes zero, then id =
LIST_MAIN_MENU, callback = base + 0x672c60 (context stays 0); then count := n + 1; result 1.

### Part 2

Private test **wanted(sid)**: true when an entry with this sid (u32 at +8) is in the level want list or the actor
want list; a list whose entries pointer is not plausible or whose count is above 4096 is skipped.

#### 4.13 `u3x_stage_unwant(base, sid)`
For the level want list, then the actor want list (skipped like above): walk from index 0; at an entry with this
sid, move every later entry down by one (0x18 bytes each, front to back), lower the count by one, count one removal
and look at the same index again; else go on. The old last slot keeps its bytes. Result = removals.

#### 4.14 `u3x_stage_wont_fit(base, level)`
T = pages total, U = low 32 bits of pages used, peak; STG[8] = T, STG[9] = U, STG[10] = peak; F = T − U when T > U,
else 0; STG[6] = F. n = level's package count. n > 16, T = 0 or T > 4096 → 0. For each package pointer p (index
order): skip when not plausible, status ≠ 7 or header version ≠ 0xa79; else N = pages needed, STG[7] = N; when N ≥
u32 at 0x18bed94 or N + 10 (u32, wrapping) > F → return N. End → 0.

#### 4.15 `u3x_stage_guard(base, s)`
Safety net (own, no PS3 counterpart) around the native lobby stage loader's mode 3 (hide + unwant) and mode 1 (name,
want request, show) sequences. FIND, SHOW, HASH, REQ = natives 42, 45, 40, 41; LM = base + 0x1afcf10;
FS = the helper's constant text "san-upper-multi-stage" (in U3X_RO: map entries keep pointing at it), FSID = its
name id.
1. Any of the four natives null, or `u3x_mp_lan_or_online(base)` = 0, or s + 0x20c0 bit 6 set → nothing.
2. STG[15] + 1. Name = text at 0x17d2518; empty → STG[0] = 0, return. sid = `u3h_crc_sid(name)`.
3. L = FIND(LM, sid), null unless plausible. When STG[13] ≠ 0, sid = FSID and L: SHOW(L, 1), byte L+2 |= 0x10,
   STG[13] = 0.
4. Not wanted(sid) → STG[0] = 0, return. now = `u3x_loc_now(base)`. sid ≠ STG[0] → STG[0] = sid, STG[1] = now,
   STG[2] = 0, STG[7] = 0.
5. st = L's state, 0xff without L; STG[14] = st. st = 8 → STG[1] = now, STG[2] = 0, return. STG[2] + 1.
6. Reason: st = 0xc → 1; else L and 2 ≤ st < 8 and `u3x_stage_wont_fit(base, L)` ≠ 0 → 3; else (u32)(now − STG[1])
   ≥ 6000 or STG[2] ≥ 1500 → 2; else return.
7. STG[4] = sid, STG[5] = reason. sid = FSID → STG[12] = 1, return.
8. L → SHOW(L, 0). `u3x_stage_unwant(base, sid)`. sid not in BAD[0..15] → into the first zero slot (none: nothing).
9. Map table: native 13 (may be null → skip) of 0x0dec97a4 → V; when V and the pointer at V + 8 are plausible and
   0 < count < 512: every entry with stage sid = sid gets name pointer FS and sid FSID; STG[11] + 1 each.
10. Copy FS with its NUL into the text at 0x17d2518. Request block (0x18 zero bytes): +0 = HASH(FS), +8 = FSID,
    +0xe = 3; REQ(LM, block). F = FIND(LM, FSID): plausible → SHOW(F, 1), byte F+2 |= 0x10, STG[13] = 0; else
    STG[13] = 1. STG[0] = FSID, STG[1] = now, STG[2] = 0, STG[3] + 1.

#### 4.16 `u3x_match_stage_cleanup(base, s, find, show)`
In a match only the played map's stage belongs (the native hide path is not reliably reached on the LAN start).
1. SW[16] + 1. G = `u3x_netgame_cur(base, s)`. V = native 13 of 0x0dec97a4 (null native → V null), E = pointer at
   V + 8 when V is plausible.
2. G null, V or E not plausible, count ≤ 0 or ≥ 512 → SW[20] + 1, return.
3. cur = stage sid of the first entry whose map id equals the s32 at G + 4; none or 0 → SW[20] + 1, return.
   SW[18] = cur.
4. Each entry in order with sid ≠ 0, ≠ cur and wanted: L = find(LM, sid); plausible → show(L, 0);
   SW[17] += `u3x_stage_unwant(base, sid)`; SW[19] = sid.

### Part 3

#### 4.17 `u3x_lobby_scene_shutdown(base)`
PS3 NetShutdown's scene part. base = 0 or eb[1] bit 3 → nothing. Else 0x5b7040(SC, 1), then 0x5b6e40(SC); eb[231] + 1.

#### 4.18 `u3x_e03_side(base, s)`
PS3 sets CM's side from the local player's team at placement (online) and re-places the character as its Start Game
state does; this keeps it live in the lobby. D[6] = 0 idle / 1 exiting, D[7] exit frames.
1. eb[1] bit 29 → D[6] = 0, return.
2. OK = menu s32 +8 ≠ 0 and +0xc = 0, SC state = 2 and phase = 2, match state ≠ 2, front-end state not in
   0x2c..0x2e. C = the scene character: handle pointer H at SC + 0x1460 and id at SC + 0x1468 non-zero, H plausible,
   *H plausible and u32 at *H + 0x7c = id; else none.
3. D[6] = 1 and (not OK or no C) → D[6] = 0, and D[4] + 1 when not OK. Not OK → return.
4. P = pointer at NGM + 0x370. P plausible and then `u3x_pm_ngentry(base, s)` non-null: side = 0x7927c0(P). side 0 or
   1 and ≠ CM word 0: D[5] = side | (CM word 0 & 0xff) << 8 | (side & 0xffff) << 16; IP = eb[1] bit 28;
   `u3x_custom_side(CM, side, IP, H, id at SC + 0x1468, base + 0x995d50)`; D[0] + 1; when C, IP = 0 and D[6] = 0:
   D[6] = 1, D[7] = 0, D[1] + 1.
5. D[6] ≠ 1 or no C → return. 0x5b47c0(SC). Busy = 0; O = pointer at C + 0x710; O plausible and its vtable plausible
   → busy = O's 13th virtual (O). Busy and ++D[7] < 240 → return. Busy → D[3] + 1. 0x5b6c40(SC); D[2] + 1; D[6] = 0.
H passed in step 4 is the raw pointer at SC + 0x1460 (read again, not the checked C).

#### 4.19 `u3x_lg01_slot0(base, desc)`
The character Init's slot-0 art name. Here "usable" = at least 0x10000 and below 2^47 (any alignment).
X = 0x11f2bf0(0xbd3bb3b9); X or *X not usable → null. T = *X; i = desc[0], 0 when not below T's count. Entries
pointer not usable → null. Entry = entries + i·0x70; L = pointer at +0x28; L or pointer at L + 8 not usable → null.
Y = 0x11f2bf0(u32 at that pointer); Y or *Y not usable → null. j = desc[2], 0 when not below the list count; list
entries not usable → null. Entry = + j·0x38; name id = id of `none` → null; name pointer usable → it, else null.

#### 4.20 `u3x_cg_commit_selection(base, s)`
1. u32 at 0x17d603c ≠ 2 → nothing. N = `u3x_sym_table(s, 0x3b9a067d)`; SEL = native 2; either null → nothing.
2. Entries = pointer at N + 8, n = u32 at N. want = s16 at 0x17d167c, was = s16 at NGM + 0x661c (the same word).
   want < 0, want ≥ n, want = was or entries null → nothing.
3. same = was in 0..n−1 and the game types (+0xc) of entries was and want are equal. Copy the 0x18 bytes at NGM +
   0x6a38; SEL(NGM, want, same ? the copy : null, 0xc). Then `u3h_cg_log("lobby selection made the match's net
   game: ", want)` and `u3h_cg_log("  ... it was net game ", was)` (texts in U3X_RO).

## 5. Test scenarios

All parts can be tested with call-recording stubs and a fake image (page arrays, collections, tables, lists); none
needs the live game. Live checks worth one run: lobby buttons as guest, PAGE_OPTIONS row 5 in the MP front end and in
a match, a converted stage that fails to load (guard fallback), a team change in the lobby (exit then entrance).
- 4.1: counts 4 / 9 / 10 / 1; a mismatched count; type ≠ 21 at item 0 and at item 3 (partial write); already
  retagged widgets; P ≠ 0; E differing from the tag; opts bit 1 with E = 0x30; E = 0xcf with LAN and non-LAN P.
- 4.2 to 4.6: each rule hit and missed by id, by enum, by type, by parameter; count 64 / 65; null collection; the
  switches; wrong page ids.
- 4.7: byte set / clear. 4.8 / 4.9: all eight input combinations and both custom_game values.
- 4.10: tags below / in / above the range; each op; row 5 with every M condition failing once; eu_row null.
- 4.11: each 0 condition; text null, "", "x".
- 4.12: count 129; main id found first / after ready / with wrong callback or context; ready missing; count 128.
- 4.13: no match, several adjacent matches, match in last slot, bad pointer, count 4097.
- 4.14: each skip; N ≥ limit; N + 10 > F; F = 0 (U ≥ T); N = 0xfffffff8 (wrap).
- 4.15: each early exit; switch from a new sid; st 8; reasons 1, 2 (by time and by frames), 3; failure of the
  fallback itself; BAD full; native 13 null; FIND of the fallback null then a later tick with STG[13].
- 4.16: no net game, bad table, no matching map, other stages wanted / not wanted, show null result.
- 4.17: base 0, switch, normal (call order). 4.18: switch; each OK condition; cancel paths; side 2; same side; IP;
  busy for 239 / 240 frames; no exit object. 4.19: each null step; index clamping; `none`.
- 4.20: mode ≠ 2; missing table / native; (want = was always in the real game) and, with a fake where the two
  reads differ, same / different types.

## 6. Old behaviour that looks like a bug or dead code (to keep)

1. 4.20: the selection and the "selected net game" are one address, so want = was and the select call never runs in
   the game; only mode test, table lookup and native test are observable. Keep the full rule.
2. 4.14: a package needing 0 pages that fails the test returns 0, i.e. "fits".
3. 4.1: a type ≠ 21 aborts after earlier widgets were already retagged.
4. 4.10: the old source holds a second branch that is switched off at compile time (never runs); nothing to deliver.
5. 4.18: D[5]'s top half repeats the side (always 0 or 1 there).
6. 4.15 step 10: map entries point into the helper's own text for the rest of the session.

## 7. What stays as it is

- C glue: `prepare_lobby_menu`, `lobby_buttons` (G31), `h21_retag` and its `eu_row`, `k_kind`, `lg01_hold`,
  `stage_wanted` (C, also used by S02), `stage_is_bad`, `e03_char`, `e03_43a034`, the exports named in 2.1.
- Mostly own design with a small game part (described anyway): the retags (own redirection; the game part is the
  menu layout, the ids and the 10-widget rule), `retag_lobby_box` and `retag_treasure_list` (one enum each),
  `stage_guard` (own decision; replays the loader's sequences), `match_stage_cleanup`.

## 8. Open questions

1. The datasource enum numbers of 4.1 / 4.2 / 4.4 to 4.6 (about 25 small numbers) are dispatcher entry numbers that
   the PS4 menu data already holds on each widget. They are given here as identifiers. If the owner wants them read
   at run time, they can be found through the dispatcher table at image 0x16fb8c0 by the callback each points to;
   that table's entry layout was not checked.
2. Text ids 0x92db2b96 and 0x63091cd0: their key names are not recorded; used as plain ids.
3. G08 proposes `u3h_e03_side`; with 2.1 the frame step can call `u3x_e03_side` directly (integrator's choice).

## Clarifications

Added by the tester (2026-11-14). Facts only, confirmed by the old-versus-new test (`reimpl2\S03\test_s03.cpp`),
where the text above is silent or can be read two ways. Every statement of sections 3 and 4 that the test checks
against v7.212 holds, and so do all six items of section 6; nothing above had to be corrected. "Read point" facts
(items 13 and 14) can only be seen with a stand-in that changes the value inside the call named; the real functions
behind those calls do not change it.

Names (3.1)
1. Every id of 3.1 equals the name id of its name under the rule of 3.1: LIST_MAIN_MENU, LIST_PLAYERS, LIST_CINEMA,
   TAB_NET_PROFILE, NET_EMBLEM_LAYER, NET_EMBLEM_TYPE, LIST_BOOSTERS, LIST_MEDAL_STREAKS, LIST_SPOILER_CLASS,
   PAGE_BOOSTERS, PAGE_NET_TREASURES, PAGE_EULA, PAGE_CREDITS, BUTTON_CINEMA_MPEG / _PROTECT / _UNPROTECT,
   SELECT_LAN_PROFILE, `none`, `san-upper-multi-stage` (0x703ab0f4), and the callers' PAGE_MULTIPLAYER, PAGE_LOBBY,
   PAGE_MULTIPLAYER_CINEMA_LIST, PAGE_CINEMA. The text id 0x54b6e963 is the name id of MENU_TITLE_EULA, so it may be
   computed from that name.

Part 1
2. A retag writes the enum word first and the parameter word second (4.1 to 4.6, also the page descriptor of 4.4);
   no call lies between them, so the order cannot be observed.
3. 4.6: eb[26] grows by one for every widget retagged (three matching buttons on one page: + 3), not once per page.
4. 4.2: a widget that matches a rule is retagged once; after that its parameter is non-zero, so no other rule can
   touch it, and the result does not depend on the order in which the rules are tried. The "any id" enums apply to
   every id, including LIST_PLAYERS, TAB_NET_PROFILE and id 0 (e.g. LIST_PLAYERS with 0xa6, TAB_NET_PROFILE with 0x89,
   id 0 with 0x31 are retagged); the id-bound pairs apply only with their own enum (LIST_PLAYERS with 0xc3,
   TAB_NET_PROFILE with 0xad, LIST_BOOSTERS with 0x44, LIST_MEDAL_STREAKS with 0xd3, LIST_SPOILER_CLASS with 0x44, any
   other id with 0x29 / 0xc2 / 0x6e stay as they are).
5. 4.1: the tenth widget of PS3 1.19's PAGE_LOBBY (enum 0xcf with a parameter whose top three bytes are 0x4c414f) is
   skipped; only parameters with top three bytes 0x4c414e count as "already retagged". `count` 10 with 9 widgets is a
   mismatch (the 9/10 allowance works one way only).
6. 4.10: with `base` = 0 no game byte is read and the row-5 target is PAGE_CREDITS; ops other than 1, 6, 8 change
   nothing in the event and still answer 1.

Part 2
7. The level state (+0x30) is compared as a full u32: 0x108 is not "loaded" and 0x10c is not "error".
8. 4.15 natives: 42, 45, 40, 41 are read once, at the start, before the `u3x_mp_lan_or_online` call; native 13 is
   read at step 9 (after the hide call and the unwant). In 4.16, native 13 is read after the `u3x_netgame_cur` call.
9. 4.15 step 9 stores the name pointer before the sid (not observable; no call between).
10. 4.15 step 8 with sid 0 (a stage name whose name id is 0): an empty BAD slot holds 0, so sid 0 always counts as
    already listed when any slot is empty; with no empty slot nothing can be stored. Sid 0 is never written to BAD.
11. 4.15 step 3: a FIND result that is not plausible (e.g. not a multiple of 8) is treated as no level: state 0xff, no
    SHOW, no flag byte. 4.15 step 6: the time test is (u32)(now − STG[1]) ≥ 6000, i.e. it also fires across the
    wrap of the s32 clock.
12. 4.14: STG[6], [8], [9], [10] are written before the early exits (n > 16, T = 0, T > 4096).
13. 4.16 step 4 (and the search of step 3): the map table count (s32 at V + 0) is read again before every entry, so a
    count that changes during an entry's FIND / SHOW calls changes how far the walk goes. The range
    test of step 2 (0 < count < 512) is made once; the entries pointer (V + 8) is read once.

Part 3
14. 4.18 step 4: IP (eb[1] bit 28) is read after the `0x7927c0` team call, right before `u3x_custom_side`; bit 29 is
    read at step 1. `inplace` is passed as 0 or 1 (not the masked bit).
15. 4.18: C is the object *H (the pointer read from the handle cell), and the exit object is read at C + 0x710; `shown`
    is the raw pointer at SC + 0x1460 (the cell H), read after the team call. The 13th virtual's result counts by its
    low 8 bits only.
16. 4.18 diagnostics: D[0] side changes (each `u3x_custom_side` call), D[1] exits started, D[2] characters destroyed
    (0x5b6c40 calls), D[3] exits ended by the 240-frame cap, D[4] exits cancelled because the lobby was not OK (a missing
    character cancels without counting), D[5] the last change {side, old CM word 0 low byte << 8, side << 16}, D[6]
    0 idle / 1 exiting, D[7] exit frames. A frame with the exit still busy counts D[7] up first: busy on frames
    1..239 of the exit waits; on frame 240 the character is destroyed and D[3] grows. Without an exit object (or with
    an implausible one or its vtable) busy is 0 and the character is destroyed on the first exit frame.
17. 4.20: the table lookup (`u3x_sym_table`) is made first, native 2 is read from `s` after it returns; native 2 is
    called as (NGM, s32 want, pointer to a 0x18-byte copy of NGM + 0x6a38 or null, 0xc). With a fake in which the
    selection is read from another address (so want ≠ was can happen): `same` needs `was` in 0..n−1 and equal game
    types; otherwise null is passed; the two log lines follow the call with want and was.
