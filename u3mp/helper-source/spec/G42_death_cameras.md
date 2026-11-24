# G42. Death / spectate cameras: fixed camera spawn, follow camera

Behaviour description for a reimplementation (reimpl2 pipeline, describer's document). It says what the functions
do, not how the old ones were written. All offsets are hexadecimal when prefixed with 0x. "Image offset X" means the
address `base + X`, where `base` is the game image base. All multi-byte values are little-endian; "u8 / u16 / u32 /
s32 / u64" are integers of that width, "pointer" is 8 bytes. Names follow `G40_respawn.md` (net game manager, net
player `p`, state object `s`, natives, handles, rs2[n], ec[n]), `film_maths.md` 3.1 (bound frame, location) and
`G33_publish_prompts.md` (screen menus).

**Float.** The only float arithmetic of this group is the squared horizontal distance of 4.2. The old code already
computed it in single precision, one rounding per operation (scalar SSE instructions in the C version, the `f_*`
tools in the C++ version); nothing of this group ran on the x87 unit. The new code uses the `f_*` tools of
CONVENTIONS.h in the order 4.2 gives and is expected to be bit-identical to the shipped helper. Everything else
that looks like a float (1.0, 75.0, 85.0, -1.0) is a 32-bit PATTERN that is stored or passed, never computed with.

## 1. Purpose and callers

While a local net player is dead (or has not spawned yet), the PS3 game shows something else on that player's
screen: a black fade while the match is not ready, a "follow camera" on another living player, or a "fixed camera"
at one of the level's camera locators; the pad's right / left step through the cameras or the followed players.
The PS4 game kept the piece that picks the player to follow (game function 0x7868a0) and the camera classes, but
lost the per-frame logic around them and the fixed-camera spawn. This group restores both.

Two parts, independent of each other (part B reaches part A's camera spawn only through the C export
`u3h_cam_fixed`, and its frame-handle test by its exported name):

| Part | Content | Exported names |
|---|---|---|
| **A** | the fixed camera: entity name lookup, squared horizontal distance, bound-frame handle test, the spawn | `u3x_entity_sid`, `u3x_xz_dist2`, `u3x_bf_handle_valid`, `u3x_cam_fixed` |
| **B** | the follow camera and the per-frame camera half: damage-effect update, aim, follow update, camera tick | `u3x_follow_fx`, `u3x_follow_aim`, `u3x_follow_update`, `u3x_camera_tick` |

Callers (all stay as they are):

- `u3x_camera_tick`: the C one-line wrapper `camera_tick`, exported as `u3h_camera_tick(base, s, p, idx)`. Its only
  caller is G40's `u3x_respawn_tick` (G40 4.11: once per frame and local net player on every path except its two
  early ends), on the game thread. Everything of this group runs on that thread; nothing is locked.
- `u3x_cam_fixed` (old C `cam_fixed`): the C export `u3h_cam_fixed(base, s, p, idx)`, used by `u3x_follow_update`
  and `u3x_camera_tick` of this group and by G43's `u3x_coop_spectate` (always with index 0).
- `u3x_bf_handle_valid`: `u3x_follow_aim`; the C one-line wrapper `bf_handle_valid` (no C caller left); and two
  file-private functions of the animation group in the same C++ file (the frame-to-world function behind
  `u3x_bf_world`, and the "binding root" search), which call it by this exported name. Name and signature must stay.
- `u3x_follow_fx`, `u3x_follow_aim`: only `u3x_follow_update`. `u3x_follow_update`: only `u3x_camera_tick`. They
  keep their exported names because the old-versus-new test calls them one by one.
- `u3x_entity_sid` (old C `ent_sid`): only `u3x_cam_fixed`. `u3x_xz_dist2`: only `u3x_camera_tick` (the old C
  function `xz_dist2` has no caller left; the old C++ file has a private copy with the same result).

## 2. Interface

### 2.1 Functions to deliver

All `extern "C"`. No float is passed or returned by value.

| Function | Signature | Result | Part |
|---|---|---|---|
| entity name | `uint32_t u3x_entity_sid(uintptr_t base, const uint8_t *e)` | the name id of level entity `e`, or 0 | A |
| distance | `uint32_t u3x_xz_dist2(const uint32_t *a, const uint32_t *b)` | float pattern, sign bit clear (4.2) | A |
| frame handle test | `int u3x_bf_handle_valid(uintptr_t base, const uint8_t *bf)` | 1 or 0 | A |
| fixed camera | `int u3x_cam_fixed(uintptr_t base, void *s, uint8_t *p, int32_t idx)` | 1 when a camera was spawned, else 0 | A |
| damage effect | `void u3x_follow_fx(uintptr_t base, void *s, uint8_t *p)` | none | B |
| aim | `void u3x_follow_aim(uintptr_t base, void *s, uint8_t *p, uint8_t *q)` | none (`s` is not used) | B |
| follow update | `void u3x_follow_update(uintptr_t base, void *s, uint8_t *p)` | none | B |
| camera tick | `void u3x_camera_tick(uintptr_t base, void *s, uint8_t *p, int idx)` | none (`idx` is not used) | B |

C wrappers (integrator): `cam_fixed(base, s, p, idx)` becomes the one line `return u3x_cam_fixed(base, (void *)s,
p, idx);` (`u3h_cam_fixed` keeps forwarding to it). `ent_sid` and `xz_dist2` have no C caller once `cam_fixed` is
a wrapper and disappear from C (or stay as one-line forwards to `u3x_entity_sid(base, e)` / `u3x_xz_dist2(a, b)`).
`bf_handle_valid` and `camera_tick` are one-line wrappers already and do not change.

### 2.2 Functions of other groups, of the C part and shared ones (declare, do not write)

| Name | Signature | What it does | Part |
|---|---|---|---|
| `u3h_ext` | `uint8_t *u3h_ext(void)` | the helper's extension state (CONVENTIONS.h) | A, B |
| `u3h_mp_code_is` | `int u3h_mp_code_is(uintptr_t base, uint32_t off, const char *hex12)` | 1 when the 12 bytes at image offset `off` equal the 24 lower-case hex digits of `hex12`; reads only (G40 2.2) | A, B (optional, see 3.2) |
| `u3h_net_coop_mode` | `int u3h_net_coop_mode(uintptr_t base, void *s)` | G40: non-zero in a co-op game type | A, B |
| `u3h_net_late_joining` | `int u3h_net_late_joining(uintptr_t base)` | G40: non-zero while this machine joins a running match | B |
| `u3h_netgame_type` | `int32_t u3h_netgame_type(uintptr_t base, void *s)` | game type of the current net game, 0 when none (10 = Elimination) | B |
| `u3h_np_alive` | `int u3h_np_alive(void *s, uint8_t *p)` | non-zero when net player `p` is alive (G40 2.2) | B |
| `u3h_np_has_effect` | `int u3h_np_has_effect(void *s, uint8_t *p, uint32_t h)` | non-zero when `p` carries effect `h` (one call of native 56) | B |
| `u3h_spawn_fade` | `void u3h_spawn_fade(uintptr_t base, void *s, uint8_t *p, int on)` | G40 4.6: sends the fade level of `p`'s screen | B |
| `u3h_cam_fixed` | `int u3h_cam_fixed(uintptr_t base, void *s, uint8_t *p, int32_t idx)` | C export forwarding to `u3x_cam_fixed` | B |
| `u3x_entity_tag` | `uint32_t u3x_entity_tag(const uint8_t *e, uint32_t tag, uint32_t fallback)` | G43: the u32 value of tag `tag` of a level entity (its tag table at +0x68), `fallback` when absent | A |
| `u3x_coop_spectate` | `int u3x_coop_spectate(uintptr_t base, void *s, uint8_t *p, uint8_t *menu)` | G43: the co-op hunter's spectate step; non-zero when it handled the cameras of this frame | B |
| `u3x_pad_pressed` | `uint32_t u3x_pad_pressed(uintptr_t base, int32_t screen)` | existing (G43's block): the "pressed this frame" button word of the screen's pad: 0 when the pointer at image 0x16e2fa0 is null, else the u32 at that object + 0x874 (screen not 1) or + 0xa4c (screen 1) | B |
| `u3x_screen_menu` | `uint8_t *u3x_screen_menu(uintptr_t base, int32_t screen)` | G33 4.6: the menu object of a screen (may be null) | B |
| `u3x_menu_page` | `uint32_t u3x_menu_page(const uint8_t *m)` | G33 4.6: id of the top page, 0 for a null menu | B |
| `u3x_menu_back` | `int u3x_menu_back(uintptr_t base, uint8_t *m, int silent)` | G33 4.9 | B |
| `u3x_menu_close` | `void u3x_menu_close(uintptr_t base, uint8_t *m)` | G33 4.10 | B |
| `u3x_call_float_arg` | `uint64_t u3x_call_float_arg(uintptr_t fn, uint32_t f0, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)` | existing (animation group): calls `fn` with the pattern `f0` in the first float register and `a0`..`a3` as the first four integer arguments; returns the integer result | B |
| `u3x_frame_identity` | `void u3x_frame_identity(uint8_t *bf)` | existing (`film_cameras.md` 3.2): writes the identity bound frame of 3.6 into 0x60 bytes; using it is optional | B |

Part B calls the `u3h_` forms named in section 4 (that is what the old code does and what the tester's stand-ins
record). Inside part B the functions call each other by their exported names.

## 3. Game facts

### 3.1 Image data

| Image offset | Type | Meaning |
|---|---|---|
| 0x17cb060 | object | the net game manager ("manager") |
| manager + 0x31e | u8 | non-zero once the match waits for "all players in" (tested together with 0x746ab0) |
| manager + 0x34e | u8 | non-zero: any team may be followed |
| manager + 0x390 + 0x9c0 · i | object | net player record i, i = 0..9 (valid only while the u8 at image 0x17d5c61 is non-zero) |
| manager + 0x6820 + 8 · k | pointer | camera locator entity k of the level (64 slots; filled by 0x73a4e0) |
| manager + 0x6a20 | s32 | number of camera locator entities |
| manager + 0x6a24 | u8 | non-zero: match over / film playback (no death cameras) |
| manager + 0x6a7c | u8 | non-zero: the screen is to stay black |
| 0x17b4ca4 | s32 | the net clock, ticks of 1/300 s ("now") |
| 0x17c3580 | pointer | the clock object in use or null; s32 at +0xc = its time |
| 0x17d5c61 | u8 | non-zero while multiplayer is active |
| 0x17d7b05 | u8 | (0x17d7a50 + 0xb5) non-zero: the damage effect is forced on |
| 0x17d7b10 | u8 | non-zero: a debug camera is on (the follow camera is not aimed) |
| 0x16e2f30 | pointer | start of the game's rigid-body handle table (8-byte slots) |
| 0x16e3188 | pointer | the menu object of screen 0, or null; u32 at +8 = its state (0 = no menu up) |
| 0x16fd020 + 8 · n | pointer | the damage screen-effect object of screen n (n = 0, 1), or null |
| 0x1ae95e0 | object | the game's camera type object used for both death cameras (only its address is used; a camera object holds the address of its type at +0xf0) |
| 0x1afcf10 | object | the level manager ("streamer") |
| 0x1d52b80 + 0x10 · n | handle (G40 3.4) | the camera manager of screen n |

### 3.2 Game functions

Called directly (System V AMD64 convention):

| Image offset | Arguments | Result | Meaning | Expected first 12 bytes |
|---|---|---|---|---|
| 0x72fb00 | (manager) | pointer | the level object of the match, or null | 55 48 89 E5 53 50 48 89 FB BF 7D 06 |
| 0x73a4e0 | (manager) | none | rebuilds the camera locator list (manager + 0x6820 / + 0x6a20) | P |
| 0x101b530 | (camera manager, pointer to a u64 holding the type object's address, pointer to a u64 holding 0x0000000200000000, parameter block of 3.5) | pointer: the new camera object, or null | camera manager "add camera" (`film_helpers.md` 3.1) | P |
| 0x10e6f30 | (out: location of 0x20 bytes, 16-aligned; level entity) | none | writes the entity's world location (position 4 floats, rotation 4 floats) | P |
| 0x7868a0 | (`p`, s32 direction, s32 first) | u8 | the game's "follow the next player": picks a living player in the given direction (1 / -1), spawns the follow camera and stores u8 `p` + 0x381 = 1, the index at `p` + 0x37c and the camera handle at `p` + 0x388; zero when nobody can be followed | P |
| 0x746ab0 | (manager) | u8 | non-zero when all players are in | P |
| 0x685b80 | (character + 0x400, out: bound frame of 0x60 bytes, out: u32 fov pattern) | u8 | a net character's camera frame and fov; zero = none | P |
| 0xc1ccc0 | (effect object, float health in the first float register, u32 flag "still hurt") | none | the damage screen-effect's update | P |

P = the usual function start 55 48 89 E5 41 57 41 56 41 55 41 54. "Code check of X" below means: the 12 bytes at
image offset X are the expected ones. The check only reads; the new code makes it itself or with `u3h_mp_code_is`
and the 24-digit texts `554889e54157415641554154` / `554889e553504889fbbf7d06` (kept in U3X_RO arrays).

0xc1ccc0 is called through `u3x_call_float_arg` (it takes a float register); all others take integers only.

Through the state object's natives (pointer to function at `s` + 768 + 8 · k, called without a null test):

| k | Arguments | Result | Meaning |
|---|---|---|---|
| 57 | (object) | none | kill a process (here: the dead player's body) |
| 58 | (streamer, u32 spawner id) | pointer | find a spawner object, or null |

Virtual call: "slot 0x158 of camera C" = the function whose address is the pointer at (pointer at C + 0) + 0x158,
called with C as its only argument: it ends that camera.

### 3.3 The level's entity name list

The level object returned by 0x72fb00 has at +0x190 a pointer L to its lists, or null (the same L as G40 3.6). The
entity name list: pointer at L + 0x18 to its entries, s32 at L + 0x20 = their number. Entry = 16 bytes: u32 name id
at +0, pointer to the entity at +8.

### 3.4 Net player fields (`p`, `q`)

| Offset | Type | Meaning |
|---|---|---|
| 0x00 | 4 floats | position (x at +0, z at +8) |
| 0x10 | handle (pointer at 0x10, id at 0x18) | the player's character |
| 0xe1 | u8 | joined |
| 0x1e8 | s32 | screen number |
| 0x21c | s32 | team; 1 = the hunters' side in co-op |
| 0x378 | s32 | index of the current fixed camera |
| 0x37c | s32 | index (0..9) of the followed net player; -1 = none |
| 0x380 | u8 | non-zero: the fixed camera steps on by itself after 5 s (this group only clears it) |
| 0x381 | u8 | non-zero: a player is followed |
| 0x384 | s32 | net time of the last fixed-camera spawn |
| 0x388 | handle (pointer at 0x388, id at 0x390) | the death camera |
| 0x39c | s32 | net time of the last death; 0 = never |
| 0x3a0 | s32 | net time of the last spawn |
| 0x5e4 | u8 | leaving / gone |
| 0x600 | s32 | co-op hunter: id of the selected AI (its spawner id); 0 = none |
| 0x7a0 | u8 | non-zero: the fixed camera takes the screen camera's current location instead of a locator's |
| 0x9a0 | s32 | clock time until which the player counts as hurt |

"In game" (a record q): q not null, u8 q + 0xe1 non-zero and u8 q + 0x5e4 zero. "Record i": null when the u8 at
image 0x17d5c61 is 0, else manager + 0x390 + 0x9c0 · i. "Handle valid / object of a handle": G40 3.4 (written out
where needed; the object's own handle is u64 at object + 0x70 and u32 at object + 0x7c). "Camera manager of screen
n": the object of the handle at image 0x1d52b80 + 0x10 · n (n as a signed 64-bit number, no range test), or null.

### 3.5 Camera objects

- Camera manager: +0x4c0 a location (0x20 bytes: position, rotation) = where that screen's camera is now.
- Camera spawn parameter block for the type at 0x1ae95e0: 0x60 bytes, 16-aligned (the layout of `film_helpers.md`
  3.4 with another first word and one more byte): u32 +0 = 0x50000064; u32 +4 = name id of the entity the camera
  sits at (0 = none); u32 +0xc = fov pattern; u32 +0x10 and +0x14 = 0xbf800000 (-1.0); +0x20 location (0x20
  bytes); u8 +0x50 = 1 and u8 +0x51 = 1 ("location given"; the meaning of the second byte is not known); everything
  else zero.
- A camera object of that type: pointer +0xf0 = address of its type object; +0x230 a bound frame (0x60 bytes) the
  camera looks from; u32 +0x294 = fov pattern.
- Character object: u32 +0x30 = class id: 0xf2c86c22 = a net character (another machine's player; +0x400 = the
  sub-object for 0x685b80), 0xa5a63e9b = a local Player.
- Damage screen-effect object: pointer +0 = its function table; the pointer at table + 0x28 is its update function.
- Rigid body (target of a bound frame's handle): u32 +0x50 = its id, u16 +0x54 = its slot number in the handle
  table.

### 3.6 Bound frame

`film_maths.md` 3.1. The identity frame: 0x60 bytes all zero except u32 +0 = u32 +4 = 0x7fcccccc, u32 +0x3c =
0x3f800000 and u32 +0x5c = 0x3f800000 (both rotations (0, 0, 0, 1)). Body handle: pointer at +8 (to a slot of the
handle table), u32 id at +0x10.

### 3.7 Ids and constants

Tag 0x24f280a0 (a spawner's "camera spawner" name); effect 0x116b3043 (hurt); page id 0xd3a938b7 (the co-op class
selection page); pad bits 0x20 = right, 0x80 = left; fov patterns 0x42aa0000 = 85.0 (fixed camera) and 0x42960000 =
75.0 (follow camera default); 0x3f800000 = 1.0 (full health); 0x7f7fffff = the largest finite float (start of the
nearest search). Times: 1500 ticks = 5 s.

### 3.8 Helper state written

| Address | Type | Meaning |
|---|---|---|
| `s` + 0x3d78 | u32 rs2[0..] | rs2[11] = fixed cameras spawned (attempts); rs2[12] = follow cameras started |
| `u3h_ext()` + 0x100 | u32 coop[0..] | coop[11] co-op cameras; coop[14] fixed-camera steps by pad; coop[15] follow steps by pad; coop[16] follow fallbacks |
| `u3h_ext()` + 0x20a0 | u32 | frames faded because not all players are in |
| `u3h_ext()` + 0x20f0 | u32 fcam[0..8] | [0] effect updates, [1] last effect flag, [2] aims at a net character, [3] aims at a local Player, [4] aims given up (no character / no frame / no camera manager), [5] debug camera on, [6] own camera is of another type, [7] last fov pattern, [8] targets of another class |
| `u3h_ext()` + 0x4450 | u32 ec[0] | read only: bit 1 (2) = the Elimination late-join rule is off (G40) |

All counters are plain "+ 1" on the game thread.

## 4. Behaviour

### 4.1 `u3x_entity_sid(base, e)` (part A)

Calls 0x72fb00(manager) (no code check here: the caller made it). Result 0 when it returns null or its list pointer
L (3.3) is null. Otherwise go through the entity name list in order, for as long as the entries pointer is not null
and the index is below the count (s32): the name id of the FIRST entry whose entity pointer equals `e`. 0 when
there is none. Nothing is written.

### 4.2 `u3x_xz_dist2(a, b)` (part A, pure)

`a` and `b` point at positions (float patterns; word 0 = x, word 2 = z; words 1 and 3 are not read). With single
precision operations, one rounding each:

dx = a.x − b.x; dz = a.z − b.z; r = (dx · dx) + (dz · dz); result = the pattern of r with bit 31 cleared.

That is `f_sub(a[0], b[0])`, `f_sub(a[2], b[2])`, the two squares with `f_mul`, then `f_add(square of dx, square of
dz)` (the operand order matters only for NaN payloads; keep it). No fused multiply-add. NaN and infinity are not
treated specially.

### 4.3 `u3x_bf_handle_valid(base, bf)` (part A, reads only)

Tells whether the body handle of bound frame `bf` (3.6) still names its body. 0 when the handle pointer H is null
or the id is 0; 0 when the body pointer B stored at H is null. With I = the body's id (u32 at B + 0x50): 0 unless
I equals the frame's id, or the frame's id is at most 0xfffe and the low 16 bits of I equal it. Finally: 1 when
the body's slot number (u16 at B + 0x54) equals the low 16 bits of ((address H − the pointer stored at image
0x16e2f30) divided by 8, as an unsigned 64-bit computation), else 0.

### 4.4 `u3x_cam_fixed(base, s, p, idx)` (part A)

Puts net player `p`'s screen on a fixed camera at camera locator `idx` of the level, or, for a co-op hunter with a
selected AI, at that AI's spawner. In this order:

1. Code checks of 0x73a4e0, 0x101b530 and 0x72fb00. Any fails: result 0, nothing done.
2. Call 0x73a4e0(manager).
3. AI = 0, unless `u3h_net_coop_mode(base, s)` is non-zero (asked in every case) and the team of `p` is 1: then
   AI = the s32 at `p` + 0x600.
4. N = s32 at manager + 0x6a20. Result 0 when N ≤ 0 and AI is 0.
5. Entity E = null. When N > 0: a negative `idx` gets N added ONCE (this adjusted value is "the index" from here
   on); slot = the remainder of the index divided by N (truncating division, so a still negative index gives a
   negative or zero remainder); result 0 when the slot is negative; E = the pointer at manager + 0x6820 + 8 · slot.
   (The existing `u3x_camera_slot(index, count)` is this remainder rule; the new code does it itself.)
6. Result 0 when E is null and AI is 0.
7. The old camera: C = the object of the handle at `p` + 0x388; when not null, call slot 0x158 of C (3.2).
8. A parameter block (3.5), all zero, then: first word 0x50000064; name id = `u3x_entity_sid(base, E)` when E is
   not null (else 0); fov 0x42aa0000; the two -1.0 words.
9. CM = the camera manager of screen (s32 at `p` + 0x1e8). Place entity A = E.
10. Only when AI is not 0: SP = native 58 (streamer, AI as u32). When SP is not null: CS = `u3x_entity_tag(SP,
    0x24f280a0, AI as u32)`; SP2 = native 58 (streamer, CS); when SP2 is not null: A = SP2 and the block's name id
    = CS; otherwise A = SP and the name id = AI; then coop[11] += 1 (in both cases). When SP is null nothing
    changes (A stays E).
11. "Screen location" = the u8 at `p` + 0x7a0 is non-zero and CM is not null. Result 0 when A is null and "screen
    location" does not hold.
12. The block's location (+0x20, 0x20 bytes): with "screen location", a copy of the 0x20 bytes at CM + 0x4c0;
    otherwise call 0x10e6f30(address of the block's location, A).
13. Block bytes +0x50 = 1 and +0x51 = 1.
14. Result 0 when CM is null.
15. O = 0x101b530(CM, address of a u64 holding `base` + 0x1ae95e0, address of a u64 holding 0x0000000200000000,
    the block).
16. Store, in this order: the handle at `p` + 0x388 = O's own handle (u64 at O + 0x70 into `p` + 0x388, u32 at
    O + 0x7c into `p` + 0x390), or 0 and 0 when O is null; s32 `p` + 0x378 = the index (adjusted as in step 5, NOT
    the slot); s32 `p` + 0x384 = the net clock (image 0x17b4ca4, read now, after the spawn); rs2[11] += 1.
17. Result 1 when O is not null, else 0.

### 4.5 `u3x_follow_fx(base, s, p)` (part B)

Gives the damage screen effect of `p`'s screen its update with full health, so a dead player's blood overlay fades
while spectating.

1. n = s32 at `p` + 0x1e8; return unless 0 ≤ n ≤ 1. O = the pointer at image 0x16fd020 + 8 · n; return when null.
   Return when O's function table pointer is null or the table's pointer at +0x28 is not `base` + 0xc1ccc0. Return
   when the code check of 0xc1ccc0 fails.
2. Flag "still hurt": 1 when the clock object (pointer at image 0x17c3580) is not null and (s32 at `p` + 0x9a0) −
   (s32 at clock + 0xc), as a 32-bit signed difference, is at least 1. Otherwise (asked only then): 1 when
   `u3h_np_has_effect(s, p, 0x116b3043)` is non-zero; otherwise 1 when the u8 at image 0x17d7b05 is non-zero,
   else 0.
3. `u3x_call_float_arg(base + 0xc1ccc0, 0x3f800000, O, flag, 0, 0)`.
4. fcam[0] += 1; fcam[1] = flag.

### 4.6 `u3x_follow_aim(base, s, p, q)` (part B)

Aims `p`'s follow camera at the camera frame of the followed net player `q`.

1. CH = the object of the handle at `q` + 0x10. Null: fcam[4] += 1, return.
2. A local frame F (0x60 bytes, 16-aligned) = the identity frame (3.6); a local fov word = 0x42960000.
3. By the class id of CH (u32 at +0x30):
   - 0xf2c86c22: when the code check of 0x685b80 fails (no call then), or 0x685b80(CH + 0x400, F, address of the
     fov word) returns a zero byte: fcam[4] += 1, return. Otherwise fcam[2] += 1 (the game filled F and the fov).
   - 0xa5a63e9b: CM = the camera manager of screen (s32 at `q` + 0x1e8: the TARGET's screen). Null: fcam[4] += 1,
     return. Otherwise the 0x20 bytes at CM + 0x4c0 become F's local position and rotation (F + 0x40); fcam[3] += 1.
   - any other: fcam[8] += 1 (F stays the identity frame).
4. When the u8 at image 0x17d7b10 is non-zero: fcam[5] += 1, return.
5. CAM = the object of the handle at `p` + 0x388. Null: return (no counter).
6. When the pointer at CAM + 0xf0 is not `base` + 0x1ae95e0: fcam[6] += 1, return.
7. When F's handle pointer (u64 at F + 8) is not 0 and `u3x_bf_handle_valid(base, F)` is 0: set F's handle
   pointer and id (u64 at +8, u32 at +0x10) to 0; the parent position and rotation in F stay as they are.
8. Copy F's bytes 8..0x5f to CAM + 0x230 + 8 (the camera frame's first 8 bytes stay). u32 at CAM + 0x294 = the fov
   word. fcam[7] = the fov word.

### 4.7 `u3x_follow_update(base, s, p)` (part B)

1. Return when the u8 at `p` + 0x381 is 0, or the handle at `p` + 0x388 is not valid. Return when the code check
   of 0x7868a0 fails.
2. `u3x_follow_fx(base, s, p)`.
3. "Any team" = `u3h_netgame_type(base, s)` is 10, or the u8 at manager + 0x34e is non-zero (the type is asked in
   every case).
4. i = s32 at `p` + 0x37c; Q = record i when 0 ≤ i < 10, else null. The target may still be followed when, tested
   in this order and each only when the ones before held: Q is in game; `u3h_np_alive(s, Q)` is non-zero; and one
   of: "any team"; Q's team equals `p`'s team; or (asked last, only when needed) `u3h_net_coop_mode(base, s)` is
   non-zero and `p`'s team is 1. Then: `u3x_follow_aim(base, s, p, Q)`, return.
5. Otherwise: when 0x7868a0(`p`, 1, 0) returns a zero byte: u8 `p` + 0x381 = 0, u8 `p` + 0x380 = 0, then
   `u3h_cam_fixed(base, s, p, s32 at p + 0x378)`. In both cases (a next player was found or not): coop[16] += 1.

### 4.8 `u3x_camera_tick(base, s, p, idx)` (part B)

Once per frame for local net player `p`. Read once at the start and keep: now = s32 at image 0x17b4ca4; death =
s32 at `p` + 0x39c; team = s32 at `p` + 0x21c. "fade" = `u3h_spawn_fade(base, s, p, 1)`. "follow(d, f)" =
0x7868a0(`p`, d, f), its result byte. Time differences are 32-bit signed differences. Screen = s32 at `p` + 0x1e8
(read where used).

**1. Fades.** In this order:
- coop = `u3h_net_coop_mode(base, s)`. late = `u3h_net_late_joining(base)` is non-zero (asked in every case) and
  (coop is 0 or team is not 1).
- When the u8 at manager + 0x6a7c is non-zero: fade.
- When late, and ec[0] has bit 1 clear, and (asked only then) `u3h_netgame_type(base, s)` is 10: late becomes
  false.
- When late: fade.
- When the code check of 0x746ab0 holds, and (the u8 at manager + 0x31e is 0, or else 0x746ab0(manager) returns a
  zero byte): fade, and the u32 at `u3h_ext()` + 0x20a0 += 1.
(Up to three fades in one frame.)

**2. Alive, late or match over.** menu = `u3x_screen_menu(base, screen)`. When `u3h_np_alive(s, p)` is non-zero
(asked in every case), or late, or the u8 at manager + 0x6a24 is non-zero: when `u3x_menu_page(menu)` is
0xd3a938b7: `u3x_menu_back(base, menu, 0)`, then `u3x_menu_close(base, menu)`. Return.

**3. Co-op hunter.** When (death is 0 or now − death > 1500) and coop is non-zero and team is 1: when
`u3x_coop_spectate(base, s, p, menu)` is non-zero, return (it is asked only under those conditions).

**4. Preconditions.** Return when the code check of 0x7868a0 fails. Return when the camera manager of the screen is
null.

**5. Choosing a camera.** gone = now − death > 1500, or the handle at `p` + 0x10 is not valid. Only when the u8 at
`p` + 0x381 is 0:
- (a) Only when gone: when follow(1, 1) is non-zero: u8 `p` + 0x380 = 0 and rs2[12] += 1. Then, whatever it
  returned: when the u8 at `p` + 0x381 (read again now) is non-zero, go on with step 6.
- (b) When the handle at `p` + 0x388 is valid (gone or not): go on with step 6.
- (c) Otherwise (nobody followed, no camera): return when not gone. Count the records i = 0..9 that are in game;
  "someone lives" = at least one of them has (s32 at +0x39c) ≤ (s32 at +0x3a0) and is not (coop non-zero and team
  1). `p`'s own record is among them. Return unless someone lives or the count is exactly 1. Return when the code
  check of 0x73a4e0 or of 0x10e6f30 fails. Call 0x73a4e0(manager). N = s32 at manager + 0x6a20. The nearest
  locator: for k = 0, 1, ... while k < N and k < 65: the entity pointer at manager + 0x6820 + 8 · k; skipped when
  null; else 0x10e6f30(a 16-aligned location of 0x20 bytes, entity) and D = `u3x_xz_dist2(p, that location)`. The
  best k is the one with the smallest D, the D compared as unsigned 32-bit numbers, among those whose D is below
  0x7f7fffff (the first of equal ones); 0 when there is none. Then `u3h_cam_fixed(base, s, p, best)` (called in every case); when it is
  non-zero: u8 `p` + 0x381 = 0; s32 `p` + 0x37c = -1; u8 `p` + 0x380 = 0; and the body is removed: O = the object
  of the handle at `p` + 0x10; when not null: native 57 (O). Return.

When the u8 at `p` + 0x381 was non-zero at the start of step 5, (a) to (c) are skipped.

**6. Stepping.** fol = the u8 at `p` + 0x381 is non-zero (read now).
- When the u8 at `p` + 0x380 is non-zero and now − (s32 at `p` + 0x384) > 1500:
  `u3h_cam_fixed(base, s, p, (s32 at p + 0x378) + 1)`.
- Otherwise, only when the pointer at image 0x16e3188 is not null and the u32 at that object + 8 is 0 (no menu
  up): B = `u3x_pad_pressed(base, screen)`; right = B has 0x20; left = B has 0x80.

  | fol | right | left | effect |
  |---|---|---|---|
  | no | yes | any | u8 `p` + 0x381 = 0, u8 `p` + 0x380 = 0, `u3h_cam_fixed(base, s, p, (s32 at p + 0x378) + 1)`, coop[14] += 1 |
  | no | no | yes | the same with index − 1 |
  | yes | any | yes | step with direction 1 |
  | yes | yes | no | step with direction −1 |
  | any | no | no | nothing |

  "Step with direction d": u8 `p` + 0x380 = 0; when follow(d, 0) is non-zero: u8 `p` + 0x381 = 1; then
  coop[15] += 1 (in both cases).

**7.** `u3x_follow_update(base, s, p)`.

## 5. Tables

No table of game values belongs to this group (none of INVENTORY section C; the old functions hold none). The
twelve function-start bytes of 3.2 are the helper's own safety check, the single ids of 3.7 are interoperability
constants.

## 6. Test scenarios

Off-line, with a fake image, a fake state object whose natives 57 / 58 are recording stand-ins, recording stand-ins
for the game functions of 3.2 (placed with the expected first bytes) and for the functions of 2.2; compare memory,
results, order and arguments of every call.

**Part A**
1. `u3x_entity_sid`: level null; list pointer null; entries pointer null with a positive count; count 0 and
   negative; entity first / last / absent / twice (first wins).
2. `u3x_xz_dist2` (pure, bit for bit): simple values; equal points (+0); large values overflowing to infinity;
   NaN in each of the four inputs, in two at once (payload of the first operand); denormals; negative zero;
   thousands of random patterns; y and w ignored.
3. `u3x_bf_handle_valid`: null handle; id 0; null body; equal ids; id 0xfffe / 0xffff / 0x10000 with a body id
   whose low 16 bits match; slot equal / different; a handle more than 0x10000 slots above the table start (only
   the low 16 bits count); a handle below the table start.
4. `u3x_cam_fixed`: each code check failing (no game call); N = 0 without AI (0 after the list call); N > 0 with
   idx 0, N − 1, N, 2N + 1, −1, −N, −N − 1 (fails), −2N (slot 0) and the value stored at +0x378 for each; null
   slot entity without AI (0, old camera NOT ended); old camera valid / stale (slot 0x158 call or none); byte
   +0x7a0 set with and without a camera manager; no camera manager (0 after the old camera was ended and the
   locator asked; nothing stored, no counter); spawn returning null (handle zeroed, counter, result 0) and an
   object (handle, index, time read after the call, result 1); block contents at the call (every byte) and the
   two u64 arguments. Co-op: mode off; mode on with team 0; team 1 with AI 0; AI with spawner unknown (place stays
   E; with N = 0 that gives result 0 unless "screen location"); spawner without / with the tag naming a known /
   unknown second spawner (place, name id, coop[11]); N = 0 with AI.

**Part B**
5. `u3x_follow_fx`: screen −1, 0, 1, 2; null object; null table; other function in the table; wrong code bytes;
   clock null; timer difference 0, 1, negative, wrapped; effect present / absent with the forced byte 0 / 1; the
   arguments of the float call; fcam[0], fcam[1].
6. `u3x_follow_aim`: target handle stale; net character with wrong code bytes / frame refused / frame given (a
   stand-in that writes the frame, a handle and a fov); local Player with / without camera manager (target's
   screen, not `p`'s); another class; debug byte; own camera stale; camera of another type; frame handle zero /
   valid / stale (handle and id cleared, parent kept); the first 8 bytes of the camera frame untouched; fov; every
   fcam counter.
7. `u3x_follow_update`: +0x381 clear; camera stale; wrong code bytes; index −1, 0, 9, 10; multiplayer byte clear;
   target not joined / leaving / dead; type 10; byte +0x34e; same team; other team with co-op off / on with team
   0 / 1 (order and number of the calls); next player found (flags stay, coop[16]) / not found (flags cleared,
   fixed camera with the stored index, coop[16]).
8. `u3x_camera_tick`, fades: each of the four conditions alone and together; late with co-op team 1; ec[0] bit 1
   with type 10; 0x746ab0 with wrong code bytes (no fade, no call), with byte +0x31e clear (no call), returning
   0 / 1.
9. Alive / late / match-over: page is / is not the class page; null menu.
10. Co-op hunter: death 0; now − death 1500 / 1501; team 0; spectate returning 0 / 1; not asked otherwise.
11. Preconditions: wrong code bytes of 0x7868a0; no camera manager.
12. Choosing: not gone with a body (nothing, return) and with a valid camera (step 6); gone with follow(1, 1)
    succeeding (stand-in sets +0x381) / returning non-zero without setting it / failing with and without a valid
    camera; the count rule (only `p` in game; two in game, the other dead / alive / a living co-op hunter); code
    checks; locators: N = 0 (index 0 asked), null entries, equal distances (first wins), a NaN and an infinite
    distance (never best), N = 70 (65 looked at); fixed camera refused (nothing written) / given (flags, −1, body
    killed or handle stale).
13. Stepping: auto step at 1500 / 1501 ticks with +0x380 set / clear; menu pointer null; menu up; the five rows of
    the table with follow succeeding / failing; counters; `u3x_follow_update` reached in every case of step 6.

Needs the live game: that the camera really appears after a death (follow first, fixed when nobody can be
followed), the pad stepping, the blood overlay fading while spectating, the follow camera tracking a remote and a
split-screen player, the black screen before all players are in, and the co-op hunter's AI camera.

## 7. Must stay as it is

- The exported names and signatures of 2.1 that exist today (`u3x_bf_handle_valid`, `u3x_follow_fx`,
  `u3x_follow_aim`, `u3x_follow_update`, `u3x_camera_tick`), the C wrappers and the exports `u3h_cam_fixed`,
  `u3h_camera_tick` (callers in G40, G43 and the animation group).
- 0x7868a0 is the game's own function: it is called, never rebuilt.
- The counters of 3.8 (the test rigs and the boot log read them) and the meaning of ec[0] bit 1.
- The existing one-liners used here (`u3x_pad_pressed`, `u3x_call_float_arg`, `u3x_frame_identity`, the menu
  accessors of G33, `u3x_handle_obj` / `u3x_handle_obj_valid`, `u3x_net_now`) and the C one-liners `cam_mgr`,
  `np_at`, `np_in_game` (other C code and the exports `u3h_cam_mgr`, `u3h_handle_obj` use them): not delivered
  again; the new parts write the same small tests under names of their own.

## 8. Oddities to keep (the new code must do the same)

1. `u3x_cam_fixed` ends the old camera (step 7) before it knows that a new one can be spawned: the two failures of
   steps 11 and 14 leave `p` without a live camera, the old handle still stored, and no counter. The failures of
   steps 4 to 6 leave the old camera alive.
2. `p` + 0x378 receives the index, not the slot: stepping right past the last locator stores N, N + 1, ... (the
   slot wraps by the remainder); a negative index is raised by N once only, so −N − 1 fails while −2N gives slot 0.
3. rs2[11] counts every spawn call, also one that returned null. coop[11] counts every found AI spawner.
4. `u3x_cam_fixed` makes no code check of 0x10e6f30 and no null test of native 58; with an AI whose spawner is
   unknown the camera silently stays at locator E. PS3 adds a constant offset to the AI spawner's location; the
   helper never did, and the new code does not either.
5. `u3x_bf_handle_valid` compares the slot number in 16 bits (the neighbouring `u3x_frame_parent` of the film
   block compares the full number): keep each as it is.
6. `u3x_follow_fx`: a null clock counts as "timer not running"; the effect is asked only then.
7. `u3x_follow_aim`: no counter when `p`'s own camera handle is stale; a target of an unknown class aims the
   camera at the identity frame (world origin); the Player case uses the target's screen number without a range
   test; a stale body handle is cleared but the parent location the game wrote is kept.
8. `u3x_follow_update` counts coop[16] also when a next player was found. `s` of `u3x_follow_aim` and `idx` of
   `u3x_camera_tick` are unused.
9. `u3x_camera_tick`: the "someone lives" count includes `p` itself; the nearest search asks locator index 0 when
   there is no usable locator; it looks at up to 65 slots although the manager has 64 (with N ≥ 65 the 65th
   "pointer" is the 8 bytes starting at the count); the location buffer is not cleared before 0x10e6f30; distances
   are compared as unsigned patterns (so NaN and infinity never win); right wins over left when no player is
   followed, left wins over right when one is; an alive player closes the class page every frame it is on top;
   the fade may be sent three times in a frame.
10. The body of the dead player is removed (native 57) when the nearest fixed camera is taken, not when a follow
    camera starts.

## 9. Open questions

1. `G03_film_playback.md` 3 calls the type object at image 0x1ae95e0 the camera type "none"; here the same object
   is the type of both death cameras (a camera with a bound frame at +0x230 and a fov at +0x294, checked through
   the object's type pointer at +0xf0). Only its address is used in both groups, so nothing depends on the name;
   the coordinator may align the wording.
2. The second "location given" byte (parameter block +0x51) and the first word 0x50000064 are taken from the old
   code as they are; their meaning inside the game's camera parameters is not known.
3. Whether the tester records the pure helpers (`u3x_xz_dist2`, the code checks, `u3h_ext`): declared free. All
   other call counts and orders are fixed as written (in particular which `u3h_` queries are made "in every case"
   and which only when needed).
4. `u3x_camera_slot` (existing two-line function, in no inventory list) has no caller once `u3x_cam_fixed` does
   the remainder itself: the integrator removes it, or the final sweep classifies it.
5. The meaning of `p` + 0x380 (only cleared here; set by the game's 0x7868a0 or by replication) and of `p` + 0x7a0
   is known by use only.

## Clarifications

Added by the tester (2026-11-14). Facts only, confirmed by the old-versus-new test (`reimpl2\G42\test_g42.cpp`),
where the text above is silent, can be read two ways, or (item 1) does not say what v7.212 does. "Read point" facts
can only be seen with a stand-in that changes the value inside the call named; the real functions behind those
calls do not change it. Everything else of sections 4 and 8 that the test checks against v7.212 holds.

1. 4.5 step 2, the hurt timer (corrects "as a 32-bit signed difference"). v7.212 as shipped, and as compiled for the
   test at -O1 and -O2, does not form a wrapped difference: with a clock object, the flag is 1 without asking the
   effect exactly when (s32 at `p` + 0x9a0) is GREATER than (s32 at clock + 0xc), a plain signed comparison of the
   two values. The two rules differ only when the true difference does not fit in 32 bits: timer 0x7fffffff with
   clock time -10 is "still hurt" in v7.212 (flag 1, `u3h_np_has_effect` not asked), while a wrapped difference is
   negative and asks the effect; timer -0x80000000 with clock time 10 is not hurt in v7.212 (the effect is asked),
   while a wrapped difference is positive. Scenario 5 "wrapped" is to be read this way.
2. 4.7 step 4, read point of the player's own team. v7.212 reads the team of `p` (`p` + 0x21c) for the comparison
   with Q's team after `u3h_np_alive` has returned, and reads it again for the "is 1" test after
   `u3h_net_coop_mode` has returned. Q's team is read after `u3h_np_alive` too. One read of `p`'s team before
   `u3h_np_alive` gives the same result unless one of the two calls changes it.
3. 4.8 step 5 (c), the location buffer of the nearest search. Old and new use one buffer for the whole search (the
   game function gets the same address for every locator). A call of 0x10e6f30 that writes nothing leaves the
   previous locator's location in it, and that locator's distance is computed from it again (with `p`'s position
   as it is then). Before the first call the buffer is not initialised on either side: what a first call that writes
   nothing would give is not defined (the game function always writes). The number of locators N is read once,
   after the 0x73a4e0 call; the slot pointers and `p`'s position are read anew for every locator.
4. 4.8, "now - death" with death 0. Step 3 takes death 0 as "dead long enough" whatever "now" is. "gone" of step 5
   has no special case: with death 0 it is "now > 1500" (or the body handle is not valid); with death 0, now at
   most 1500 (or negative) and a valid body nothing is chosen. The two time differences (now - death, now - the
   s32 at `p` + 0x384) wrap in 32 bits: v7.212 subtracts and compares the result with 1500 (unlike item 1).
5. 4.8 step 6, index + 1 and index - 1 wrap in 32 bits: 0x7fffffff + 1 reaches `u3h_cam_fixed` as -0x80000000,
   -0x80000000 - 1 as 0x7fffffff. The two flag bytes (`p` + 0x381, `p` + 0x380) are 0 at the time of the
   `u3h_cam_fixed` call, coop[14] is counted after it. Whether the index is read before or after the two byte
   writes cannot be observed (other bytes, no call between).
6. 4.4 step 1. The order of the three code checks cannot be observed (they only read; a wrong byte at any of the
   12 places of any of the three gives result 0 without any call).
7. 4.4 step 10. Native 58 gets the streamer's address (64 bits) and the id as an unsigned 32-bit value (an id with
   the top bit set is not sign-extended; the upper half of the id's register is 0 on both sides). The function
   pointer at `s` + 768 + 8 * 58 is read once, before the first lookup, and used for both lookups. The camera
   manager of step 9 is determined before the lookups; the byte at `p` + 0x7a0 is read after them.
8. 4.4 step 16 with a null spawn result is exactly as written: handle 0 and 0, the (adjusted) index and the net time
   stored, rs2[11] counted, result 0.
9. 4.4 step 5 with N above 64: the slot is the plain remainder, so slots 64 .. N - 1 read the 8 bytes at manager +
   0x6820 + 8 * slot behind the 64 pointers (old and new alike; no limit of 64 there).
