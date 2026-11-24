/* ===== agent C ports (gameplay functions, PS3 1.19 deltas, shortcuts). Helper kinds 100..109, x-stub slots
 * 0x11a00..0x11f80, switch words ExtState.ec[0..3], diagnostics ec[4..255]. Included after kr_port.inc (uses its float
 * helpers f_i2f / f_mul / f_div / f_lt). =====
 *
 * ec[16] kr 1.19 mode-10 doubled kill points (c001), ec[17] late-join fade 1.19 type-10 checks (c002),
 * ec[18..23] late-join closed check (kind 100): [18] calls, [19] type-10 answers, [20] last answer, [21] last lead team,
 * [22] last lead score, [23] last round | rounds << 16. */
#pragma clang section rodata=".rodata.ext"   /* agent C constants go to the ext code segment (main helper area is nearly full) */


EXT static int latejoin_closed119(uintptr_t base,ProfileState *s,uint64_t *regs){return u3x_latejoin_closed(base,s,regs);}   /* v7.213 C++ (G35) */

EXT static int elim_round_end_hold(uintptr_t base,ProfileState *s,uint64_t *regs){(void)s;return u3x_elim_round_end_hold(base,regs);}   /* v7.213 C++ (G35) */
EXT static void elim_wait_text(uintptr_t base){u3x_elim_wait_text(base);}   /* v7.213 C++ (G35) */

/* ---- c004: PS3 NetGameManager::Init (1.10 f_305d2c; 1.19 0x3115e0), called by NetInit f_2901e0 at 0x290714 ----
 * PS4 has no copy, and nothing on PS4 writes these fields before the match code reads them (PS4's NetInit is gone; the
 * helper's NetInit point in startup_tick stands in for f_2901e0). PS3 body (1.10): +0x1d0 = -1 (m_savedRandSeed = PS4 +0x350,
 * asserted == -1 by PushSyncedRand 0x748150 at 0x74815d, reset to -1 by the pop at 0x7482b0), +0x1d4 = 0 (PS4 +0x354, cleared
 * by PS4 round start 0x73afd6 like 1.10 0x31516c), +0x1d8 = 0 (PS4 +0x358), +0x1dc = 0 (PS4 +0x35c), +0x5c08 = 0 (PS4 +0x6a24,
 * the mgr flag camera_tick reads), +0x5c28 = 2 (PS4 +0x6a48: same +0xe20 shift as 0x5c18 -> 0x6a38, 0x5c24 -> 0x6a44,
 * 0x5c88 -> 0x6aa8). Not ported, PS4 offset not proven: +0x198, +0x1b0 = 2, +0x6594 container (f_d642d4), +0x65b4/+0x65b8,
 * +0x5d8c, +0x592c..+0x5934, +0x5af4, the +0x594a memset, +0x5c8c[32]; the calls f_2fb8f8 (= PS4 0x720500, net player slot
 * reset) and f_30573c(mgr,0,0,0xc) (= PS4 0x720610) are left to the helper's lobby flow, which already selects the net game
 * and resets the slots at Start Game. Replaces the v4.x lazy "seed 0 -> -1" fix in loadout_tick (kept as a counted safety
 * net, ec[30]). ec[29] counts runs. */
static void ngm_init_305d2c(uintptr_t base){
 uint8_t *m=(uint8_t*)(base+0x17cb060u);
 u3x_match_defaults(m);   /* v7.113 C++ */
 X.ec[29]++;}

/* ---- c005: PS3 1.19 taunt package wants (f_30af18) ----
 * 1.19 adds a package want for taunt animations. f_30af18(id) (new in 1.19, 0x30af18..0x30b063): if id != 0x0e69839d
 * ("multi-celebrate-none"), fetch the taunt table f_9d2bd4(0xb2b6e512, 0x4240ef2e, 0) (= PS4 0x11f31b0, as the taunt list
 * source 0x75a748 calls it; PS3 also caches it in a file-local global nobody else reads), scan rows 1..count-1 for the first
 * row whose name hashes to id, and want the package of that name: f_8bb2a0(&want, name) + f_8b89b8(level manager, &want)
 * (= 1.10 f_8a1894 + f_89efac(0x1346a48, ..)). PS4 inlines that pair in its own match setup 0x72b570 (e.g. 0x72d12d..0x72d182,
 * "anim-proto-shield"): want = {+0 0x10e95b0(name), +8 (*0x1702720)(0, name, 0), +0xc u16 0, +0xe u16 0, +0x10 u64 0}, then
 * +0xe = 1 (PS3: |= 0x8000 after the strlen < 0x40 assert), and 0x10ebb90(0x1afcf10, &want).
 * PS4's converted table stores sid(name) at +0 instead of the name pointer (checked: all 44 rows of the PS4 net.bin equal
 * sid() of the PS3 patch-18 names, row by row), so the name comes from taunt_rows119 below (PS3 dlc-patch18 net.bin symbol
 * 0xb2b6e512, rows 0..43 in order) and is used only when its sid equals the row's sid. Every row's actor package ships in
 * the overlay (actor33/multi-celebrate-<name>.pak, release-maps-3).
 * Callers in 1.19: (1) match setup f_31ef18 (= 1.10 f_310afc = PS4 0x72b570) at 0x3204a8..0x32054c, between the wants of
 * anim-proto-multiplayer and anim-proto-shield: p = f_41eb00(0) (player of screen 0 = PS4 handle 0x1bb9500 / id 0x1bb9508,
 * object +0x7c == id, the inline every PS4 caller uses); if p: f_30af18(p+0x4bc -> vt6) and, if p+0x906, f_30af18(p+0x4bc ->
 * vt7). Player+0x4bc is the taunt controller (1.10 factory f_54bc40, vtable 0x10f7038; PS4 Player+0x6e8, created at
 * 0xbd8dfa with vtable 0x16a03e0). 1.19 vt6 0x573a78 returns +0x74 (the taunt last started), vt7 0x573a80 returns +0x70, which
 * 1.19 vt5 Start 0x57447c sets to the old +0x74 before storing the new id (1.10 vt5 f_54c198 / PS4 0xb85010 keep only the
 * current id: 1.10 +0x6c = PS4 +0x128, PS4 vt6 0xb85000 returns it). Player+0x906 = PS4 Player+0xcea (PS4 vt7 0xb84f90 reads
 * it where 1.10 vt7 f_54becc reads +0x906; with it set the controller uses the menu screen 0x17c23a0 instead of +0x1c68,
 * i.e. the lobby character). PS4 has no previous-taunt field, so kind 104 at the Start store (0xb85126) records it per
 * controller (keyed by controller and owner process id +0x118 -> +0x7c), as 1.19's +0x70.
 * (2) the per-net-player look want 0x310b98 (1.10 f_308408, inlined three times in PS4 0x72b570) tail-calls f_30af18(np+0x4e4),
 * the taunt of the 1.19 player info message (+0xb8) -> not in c005 (player info port, next release).
 * Switch U3MP_TAUNT119=0 (ec[0] bit3). Diagnostics: ec[31] kind-103 runs, ec[32] packages wanted, ec[33] last id wanted,
 * ec[34] ids matching a row with no known name, ec[35] kind-104 starts recorded, ec[36] last recorded previous id. */
static const char taunt_rows119[]=
 "multi-celebrate-none\0multi-celebrate-laugh\0multi-celebrate-kiss\0multi-celebrate-flex\0multi-celebrate-flurry\0"
 "multi-celebrate-yessss\0multi-celebrate-liz-lemmon-dance\0multi-celebrate-loser\0multi-celebrate-pump\0"
 "multi-celebrate-ohsnap\0multi-celebrate-pump-t-1000\0multi-celebrate-uppercut\0multi-celebrate-heart-you\0"
 "multi-celebrate-kiss-ma-ass\0multi-celebrate-pump-2-point\0multi-celebrate-carlton\0multi-celebrate-fffuuuuu-\0"
 "multi-celebrate-arm-wave\0multi-celebrate-mj-1\0multi-celebrate-throat-slit\0multi-celebrate-mj-2\0multi-celebrate-haka\0"
 "multi-celebrate-heisman\0multi-celebrate-air-guitar\0multi-celebrate-facepalm\0multi-celebrate-take-a-bow\0"
 "multi-celebrate-high-kick\0multi-celebrate-hillbilly-shuffle\0multi-celebrate-mic-drop\0multi-celebrate-school-fight\0"
 "multi-celebrate-come-at-me-bro\0multi-celebrate-backflip-squat\0multi-celebrate-jump-freeze\0multi-celebrate-river-dance\0"
 "multi-celebrate-cossack-dance\0multi-celebrate-six-flags\0multi-celebrate-carlton-2\0multi-celebrate-bolt\0"
 "multi-celebrate-freddie\0multi-celebrate-dougie\0multi-celebrate-cabbage-patch\0multi-celebrate-cheerleader\0"
 "multi-celebrate-gangnam\0multi-celebrate-top-model\0";
static const char *taunt_row_name119(uint32_t row){
 return u3x_nth_name(taunt_rows119,row);}   /* v7.133 C++; read by u3h_taunt_row_name */
static void taunt_want119(uintptr_t base,uint32_t id){u3x_taunt_want(base,id);}   /* PS3 1.19 f_30af18; r2 G54 C++ */
/* kind 104: PS4 vt5 Start 0xb85010 at its store 0xb85126 (mov [r14+0x128],esi), arg = controller (r14): 1.19 0x57447c's
 * "+0x70 = +0x74" before the new id is stored. */
EXT static void taunt_start_rec119(uintptr_t base,uint8_t *tc){u3x_taunt_start_record(base,tc);}   /* r2 G54 C++ */
/* kind 103: PS4 match setup 0x72b570 at 0x72d12d (right after the anim-proto-multiplayer want, before anim-proto-shield) =
 * PS3 1.19 0x3204a8..0x32054c. */
EXT static void match_taunt_wants119(uintptr_t base){u3x_match_taunt_wants(base);}   /* r2 G54 C++ */

/* ---- c006: PS3 1.19 player info message 0x3a additions, per-net-player taunt wants, hat stats ----
 * Player info. 1.19 SendPlayerInfo 0x2fc150 (= 1.10 f_2f4fd8 = PS4 0x6b7b30) builds message 0x3a with 0xc0 bytes (1.10:
 * 0xb8) and adds, from the screen's profile record (1.19 f_3627bc(screen*0x7980 + base); PS4 keeps that record at profile
 * +0x248, profile = *(0x17dd610) + screen*0x7b00; byte fields LE, e.g. rec+0x161b -> PS4 +0x1860):
 *   +0x21 |= 8 when rec+0x1f1c != 0  (PS4 msg+0x25, profile+0x2164)
 *   +0x61 = f_362850(profile), the rank index from rec+0x1ea8 and table 0x0f892719  (PS4 msg+0x69)
 *   +0x62 = (rec+0x1f14 != 0), "Hat Stats" Off  (PS4 msg+0x6a, profile+0x215c = agent A's a007 row)
 *   +0xb8 = rec+0x53c, the equipped taunt  (PS4 msg+0xc0, profile+0x784)
 * PS4 message layout = PS3 + 4 up to +0x2e and + 8 from +0x30 (sender 0x6b7c49..0x6b8355, process 0x709b84..0x709d8c), so
 * +0x61/+0x62 land in PS4's padding at +0x69/+0x6a and +0xb8 at +0xc0: the message grows from 0xc0 to 0xc8 bytes (builder:
 * both allocations, sender 0x6b7c0e and factory 0x6c44e6). Kind 105 fills them before the send 0x7a2ef0 (0x6b846c).
 * +0x61 is sent as 0: table 0x0f892719 is the 1.19 tournament tier/points table and tournaments are removed by the user's
 * decision (a015), so f_362850 is not ported (c009).
 * Serialization: 1.19 read 0x2cd618 / write 0x2cd0ec (= 1.10 f_2c4ae4 / f_2c4fe0... slots 2 / 3, PS4 0x70b3c0 / 0x709dc0)
 * append after the last 1.10 field (+0x60): u8 +0x61, bool +0x62, u32 +0xb8 (write f_a12a8c / f_a12a98 / f_a12a54, read
 * f_a127bc / f_a127f0 / f_a126ec). Kinds 106/107 (value stubs at the entries) run PS4's own function through its
 * trampoline (address from the builder in ec[40] / ec[41]) and then append the same three fields with PS4's bitstream
 * helpers 0x1272b50 (write n bits) / 0x1272c90 (write bool) / 0x1272c00 (read n bits).
 * Process: 1.19 0x2d25c8 (slot 4; 1.10 f_2c9750 = PS4 0x709a90): the +0x21 byte now goes to np+0x110 masked with 7 (1.10:
 * whole byte to np+0x1d0 = PS4 np+0x208) and bit 3 to the new np+0x112; new np+0x620 = +0x61, np+0x621 = +0x62, np+0x4e4 =
 * +0xb8. PS4's net player has no room for the new fields; they live in np119[] by net player slot (np = mgr+0x390+i*0x9c0).
 * Kind 108 at 0x709da0 (the last stores, reached only after PS4 found the player and passed its checks).
 * Per-net-player taunt want: 1.19 0x310b98 (1.10 f_308408, inlined three times in PS4 0x72b570) tail-calls f_30af18(np+0x4e4)
 * after the look want, whether or not the look want ran (np+0xa9 = PS4 np+0xe1 only gates the look). Sites: local net
 * player 0 (mgr+0x370; 1.19 f_3025a0(mgr,0)) at the join 0x72c21c (kind 109), and the two team loops after their look call
 * (0x72c2ba, 0x72c88a, kind 110). This is how 1.19 loads the equipped taunt into a match; it replaces taunt_tick's
 * equipped source (opt-in fallback U3MP_TAUNT_TICK=1, ec[0] bit4).
 * Hat stats: 1.19 modifier rebuild 0x36ccc8 (= 1.10 f_35e808 = PS4 0x78f3b0; list count np+0x698 -> PS4 np+0x84c, 16-byte
 * entries at +0x850 {id, f32 value, source, u8 type, u8 level}) differs from 1.10 in three places:
 *  (a) a new first block: up to four type-5 entries from the netgame record +0xc4..+0xe0 -> dead with the final data (all 271
 *      records of PS3 dlc-patch18 net.bin have zeros there; PS4's 0xd0-byte records do not have the fields): not ported.
 *  (b) booster loop: a booster with id (record +4) 0xdff8181f is skipped in game type 0x13 (1.19: f_36c858 && (!f_36c7a8 ||
 *      f_30e004 != 0x13 || rec+4 != 0xdff8181f)). Kind 113, value stub at 0x78f589 (the level-entry count load): for that
 *      booster the replayed load reads a zero word, so PS4's own "no entries" branch skips it.
 *  (c) a new last block, type 4: if f_2a3eb0 (boosters enabled; PS4 inline: g+5 0x17d5c58+9, mgr+0x6808, netgame record
 *      +0x6c == 0, 0x73fbc0, whose entry stub adds the 1.19 custom bit 0x20) and table 0xb8a3581b exists and !np+0x621:
 *      look = f_36cb54(np,0) (= PS4 0x784b20(np,0)); for slots s = 2..6, item = f_436a6c(.., look[0], look[1], s-2, look[s])
 *      (= 1.10 f_40e5d8 = PS4 0x9952a0); every table row whose item id equals *item appends its {stat, value} pairs as
 *      {stat, value, s, type 4, level 0}. Kind 112 at the function's exit 0x78f8b3.
 *      Table 0xb8a3581b is not in PS4's net.bin yet (queued as asset work); PS4 layout assumed from the converter's rules:
 *      {u32 count, u64 rows}, rows {u32 item, s32 n, u64 pairs}, pairs {u32 stat, f32 value}. Until it exists the block is
 *      skipped, as 1.19 does without the table. 1.19 does not check the 16-entry cap in this block (it would run into the
 *      booster ids at np+0x79c / PS4 +0x950); the port stops at 16 and counts it (ec[49]).
 * Switches: U3MP_PINFO119=0 (ec[0] bit5) leaves the message as 1.10 (no extra fields, size, serialization, process);
 * U3MP_HAT119=0 (ec[0] bit6) leaves (b) and (c) out. Diagnostics: ec[37] sends, ec[38] reads, ec[39] writes, ec[42] processes,
 * ec[43] last processed slot | hatoff<<8 | f112<<9, ec[44] last taunt received, ec[45] kind-109/110 runs, ec[46] boosters
 * skipped (b), ec[47] hat entries added, ec[48] rebuilds that saw the table, ec[49] cap hits, ec[40]/ec[41] trampolines. */
static struct{uint32_t taunt;uint8_t rank,hatoff,f112,set;} np119[10] EXT_BSS;
/* kind 105: PS4 SendPlayerInfo 0x6b7b30 at 0x6b846c (lea rdi,[0x17d6420] before the send 0x7a2ef0), arg = message (r13). */
EXT static void pinfo_send119(uintptr_t base,uint8_t *msg){
 u3x_pinfo_send(base,msg);}   /* v7.159 C++ */
/* kinds 106 / 107: msg 0x3a slot 2 (read) 0x70b3c0 / slot 3 (write) 0x709dc0, value stubs at the entries (rdi msg, rsi bs). */
EXT static int pinfo_serial119(uintptr_t base,uint64_t *regs,int wr){
 return u3x_pinfo_serial(base,regs,wr);}   /* v7.159 C++ */
/* kind 108: process 0x709a90 at 0x709da0, arg = message (r15). The player is found as PS4 does (0x709ab0..0x709b05). */
EXT static void pinfo_process119(uintptr_t base,uint8_t *msg){
 u3x_pinfo_process(base,msg);}   /* v7.159 C++ */
/* kinds 109 / 110: the inlined 1.19 0x310b98 tail, f_30af18(np+0x4e4); arg = net player. */
EXT static void np_taunt_want119(uintptr_t base,uint8_t *np){
 u3x_np_taunt_want(base,np);}   /* v7.159 C++ */
/* kind 113: 0x78f589 in the booster loop (rax = booster record), value stub; returns 0 (replay), rewriting rdx/rcx so the
 * replayed "mov eax,[rdx+rcx+4]" reads zero for the booster 1.19 skips. */
EXT static int booster_skip119(uintptr_t base,ProfileState *s,uint64_t *regs){return u3x_booster_skip(base,s,regs);}   /* r2 G53 C++ */
/* kind 112: modifier rebuild exit 0x78f8b3, arg = net player (r12). */
EXT static void hat_mods119(uintptr_t base,ProfileState *s,uint8_t *np){u3x_hat_mods(base,s,np);}   /* r2 G53 C++ */

/* ---- c007: PS3 1.19 spotter booster per local screen (1.19 0x370bc4 block 0x371894..0x3719d4; 1.10 f_361fc4) ----
 * 1.10 (and PS4 0x789a60, inline loop 0x78ecc7..0x78ee4e, booster 0x1ca807a radius via 0x689d20 = 1.10 f_29e7f4): for each
 * target net player that is not the spotter's ally (f_3081a4; PS4 inline: skip self; team test unless game type 0xe), is
 * alive (np+0x39c <= +0x3a0) and not yet spotted (np+0x8a5 = PS4 np+0x9b1), within the radius (x,y,z distance squared),
 * set the flag unless the target has the counter-booster 0x41479fa0 (PS4 0x793280(np, sid, 0) = 1.19 f_369688; 1.10 also
 * tested the spotter, PS4 only tests the target).
 * 1.19: the flag is one byte per local screen (np+0x7f5 + j, j = 0, 1). For each j the same tests run against flag[j], and the
 * counter-booster is tested on the target and on local net player j (f_3025a0(mgr, j) = PS4 mgr+0x370+8j; f_2a5618 answers
 * 0 for a missing player). Readers index by the screen of the HUD being drawn (hud+0xf0 = PS4 *(0x1806480)+0x1d0):
 * 1.19 0x66b934, 0x675a1c, 0x675abc, 0x677264, 0x6772fc, ... = PS4 0xc329a7 (local np of that screen), 0xbf2aa2, 0xc2cebe,
 * 0xc2d28f, 0xc2d527, 0xc2db20 (targets), and the helper's HUD slot-3 port (hud_spotted_draw = PS3 0x6603f4 -> 1.19
 * 0x66b8b4). Clears: the net task (1.19 0x31e5d8.. clears +0x7f5 and +0x7f6 with +0x7fa/+0x7fc for every player without
 * +0x45c set; 1.10 0x31ba80; PS4 0x7262b4.., unrolled per player, np+0x5e4) and the player reset (1.19 0x368ab4; PS4
 * 0x782ed4).
 * PS4 storage: flag[0] stays at np+0x9b1 (so every native reader is already right for screen 0), flag[1] in spot1_119[slot].
 * Kind 114 replaces the loop (hook 0x78ecc7, continues at 0x78ee54); kind 115 clears flag[1] (net task 0x7262a4 for all
 * players without np+0x5e4, player reset 0x782ed4); kind 116 is the readers' stub: it returns the flag of the screen being
 * drawn in ec[54] (low byte), and the stub replays the reader's instruction on that byte.
 * Switch U3MP_SPOT119=0 (ec[0] bit7): kind 114 runs PS4's own single-flag loop. Diagnostics: ec[50] loop runs, ec[51]
 * flag[0] sets, ec[52] flag[1] sets, ec[53] reads on screen 1. */
static uint8_t spot1_119[10] EXT_BSS;
/* kind 114: 0x78ecc7 (spotter r13 alive, before the radius query); continues at 0x78ee54. */
EXT static void spotter119(uintptr_t base,ProfileState *s,uint8_t *sp){
 u3x_spotter(base,s,sp);}   /* v7.152 C++ */
/* kind 115: clears of flag[1]. arg = player (player reset 0x782ed4) or 0 (net task 0x7262a4: every player without +0x5e4). */
EXT static void spot_clear119(uintptr_t base,uint8_t *np){
 u3x_spot_clear(base,np);}   /* v7.152 C++ */
/* the flag of np for the screen being drawn (1.19 np+0x7f5+hud->screen) */
static uint8_t spot_flag119(uintptr_t base,uint8_t *np){
 return u3x_spot_flag(base,np);}   /* v7.152 C++ */
/* kind 116: reader stubs, arg = the player the reader tests. */
EXT static void spot_read119(uintptr_t base,uint8_t *np){u3x_spot_read(base,np);}   /* v7.152 C++ */
/* ===================================================================================================================
 * c008: PS3 1.19 player state replication sender (agent C)
 * PS3 1.19 f_363ed4 0x373074..0x3732ec (send loop) -> f_2f6910 (send) -> f_2db504 (player filler) + f_2cf964 (weapons)
 * -> f_2d96a4 (anim state packer). PS4 has none of these: the Player state message (type 0x10, vtable 0x16d5b90, 0x240
 * bytes) is never built, so remote players do not move. The receive side (read 0x71a010, process 0x718350, apply 0x683670)
 * is PS4's own. Everything below follows the 1.19 code; each PS4 counterpart is cited. Message offsets PS3 1.19 -> PS4:
 * +0x10->+0x14 time, +0x14->+0x18 bits buffer (300 bytes), +0x140->+0x144 bit count, +0x144->+0x148 flags, +0x148..+0x158
 * -> +0x14c..+0x15c, +0x160 and +0x170 (BoundFrame, PS4 0x60 bytes) unchanged, +0x1c0..+0x1dc -> +0x1d0..+0x1ec,
 * +0x1e0..+0x200 -> +0x1f0..+0x210, +0x210 -> +0x220, +0x220..+0x225 -> +0x230..+0x235 (PS4 ctor 0x6ae308..0x6ae45e = 1.19
 * ctor f_2c5a98 field for field; PS4 NPC sender 0x6ae2a0 = 1.19 f_2f6804 + f_2db1a0 uses the same layout).
 * Switch: U3MP_REPL119=0 (ec[0] bit8) sends nothing (PS4 behaviour). Diagnostics ec[60..79] (NOTES-c008).
 * Not in c008: the NPC bit count (PS4 0x6ae57f): the NPC's anim table holder (1.19 vt+0x344()+0x40; PS4 vt+0x698 returns
 * [char+0x3d8]) is the struct the decoder uses, its PS4 layout is verified with the decoder in c009. */
static uint8_t repl_frame119[3][0x60] EXT_BSS;          /* BoundFrames handed to PS4 code (vmovaps: 16-byte aligned) */
static uint8_t repl_loc119[0x40] EXT_BSS;
static uint8_t *find_art_group(uintptr_t base,const char *name);   /* u3_lan_profiles.c (PS3 f_88668c / 1.19 f_89a45c) */
/* f_a12834(bs, v, n, -): PS3 bit order (MSB first in 32-bit words), as PS4 0x1272b50 writes them. PS3 asserts on overflow
 * (f_2cf8f8 budget checks before every write); here an overflow sets repl_err119 and stops writing. */
static uint32_t repl_err119 EXT_BSS;
static void bsw119(uint8_t *bs,uint32_t val,uint32_t n){u3x_bsw(bs,val,n);}   /* v7.212 C++ */
/* f_a12d50(bs, v): position, 1.19 0xa12d50..0xa12f74 (r30 = 0x1173c90: 255, 127, 512, 256): |x|<255 && |y|<127 && |z|<255
 * -> bit 0, x*512 (18 bits), y*256 (17), z*512 (18) (fctiwz); else bit 1 and the three floats raw (32 each). */
static uint32_t fabs119(uint32_t f){return u3x_fabs(f);}   /* v7.212 C++ */
/* PPC fctiwz: truncation with saturation (>= 2^31 -> 0x7fffffff, <= -2^31 -> 0x80000000, NaN -> 0x80000000); x86 cvttss2si
 * returns 0x80000000 for every out-of-range value, which differs in the low bits the packer keeps (1-frame anims: x/0). */
static uint32_t ppc_ctiwz119(uint32_t f){
 return u3x_ppc_ctiwz(f);}   /* v7.199 C++ */
static void bsw_vec119(uint8_t *bs,const uint32_t *v){u3x_bsw_vec(bs,v);}   /* v7.212 C++ */
/* the BoundFrame writer f_2bbf5c = PS4 0x7befc0(bs, frame) (writes into the zeroed buffer: see anim_pack119) */
static void bsw_frame119(uintptr_t base,uint8_t *bs,uint8_t *f){u3x_bsw_frame(base,bs,f);}   /* v7.212 C++ */
/* quaternion / locator math for the attach branch (Locator = {pos x,y,z,w; quat x,y,z,w}) */
static void q_mul119(uint32_t *o,const uint32_t *a,const uint32_t *b){u3x_q_mul(o,a,b);}   /* v7.212 C++ */
static void q_rot119(uint32_t *o,const uint32_t *q,const uint32_t *v){u3x_q_rot(o,q,v);}   /* v7.212 C++ */
static void loc_mul119(uint32_t *o,const uint32_t *a,const uint32_t *b){u3x_loc_mul(o,a,b);}   /* v7.212 C++ */
static void loc_rel119(uint32_t *o,const uint32_t *a,const uint32_t *b){u3x_loc_rel(o,a,b);}   /* v7.212 C++ */
/* 1.19 f_953e48: world locator of a BoundFrame = parent o local; the parent is the binding's locator when the handle is
 * valid (PS4 frame +8 handle / +0x10 id, binding +0x00 pos / +0x10 quat, as 0x1196230 refreshes it), else the cached
 * parent (+0x20 / +0x30). Local +0x40 / +0x50. */
static void bf_world119(uintptr_t base,uint8_t *f,uint32_t *o){u3x_bf_world(base,f,o);}   /* v7.212 C++ */
/* 1.19 f_854d14(player) as PS4 inlines it (0x3d14eb..0x3d159d): binding = vt+0x248(obj, 0) (PS3 vt+0x120); while obj:
 * if WORD obj+0x128 bit 0x800 (PS3 u64 obj+0xd0 bit 20) return binding; binding = the binding of obj's BoundFrame
 * (obj+0xb8 / +0xc0, PS3 f_85df94 = obj+0x70); obj = 0x10c5010(obj) (PS3 f_85ea6c, the binding's owner). 0 at the end. */
static uint8_t *bind_root119(uintptr_t base,uint8_t *obj){return u3x_bind_root(base,obj);}   /* v7.212 C++ */
/* 1.19 f_9555d0(out, 0, sid, obj, no args) = PS4 0x1197380; the answer value is the first word */
static uint64_t ev_query119(uintptr_t base,uint32_t sid,uint8_t *obj){return u3x_ev_query(base,sid,obj);}   /* v7.212 C++ */
/* 1.19 f_3097d0(mgr, id): the net player with that id (np code f_367278 = np_code), 0 for id 0 */
static uint8_t *np_by_id119(uintptr_t base,uint32_t id){return u3x_np_by_id(base,id);}   /* v7.212 C++ */
/* 1.19 f_8abdc4(table, key): binary search of {u32 count; entries {u32 sid, ptr}} (PS3 8-byte entries, PS4 16-byte entries
 * {u32 sid; u64 ptr} at +8, as PS4 0x6ba9d0 reads them); same loop bounds (64-bit unsigned) as PS3. */
static int32_t tab_find119(uint8_t *t,uint32_t key){return u3x_tab_find(t,key);}   /* v7.212 C++ */
static uint8_t *art_tab119(uint8_t *g){return u3x_art_tab(g);}   /* v7.212 C++ */
static uint32_t sid119(uintptr_t base,const char *s){return u3x_sid(base,s);}   /* v7.212 C++ */
/* 1.19 f_2d9270(&table, &index, anim id, anim, tables): the anim in the 8 player tables (a table counts only when its
 * art group id, PS4 +0x18 / PS3 +4, is the sid of the anim's group name, PS4 anim +0x38 / PS3 +0x30); else in the art
 * group of that name (table = that sid). */
static int anim_lookup119(uintptr_t base,uint32_t *tab,int32_t *idx,uint32_t id,uint8_t *anim,uint8_t **tabs){
 return u3x_anim_lookup(base,tab,idx,id,anim,tabs);}   /* v7.212 C++ */
/* 1.19 f_7f9c80 / f_7f7590: a streaming chunk anim -> the streamed (root) anim and the stream's phase. PS4 reworked the
 * stream records (anim-streaming.cpp, list 0x1d46090 / count 0x1d46088); its forward lookup 0xfd4c60 goes root id -> slot u
 * (ids[u][k] == id, index[u][k] = a) -> chunk table (**rec+0x48)[a][chunk][u], storing the phase at rec+0x20 (PS3 rec+0x10).
 * The inverse walks the same tables; the root anim is PS4 0xfd62a0(chunk skel +0x40, clip id, root id) = PS3 f_7fb034
 * (as PS4's AnimStreamLoginBuffer 0xfd49d4 calls it). Returns 0 for any anim that is not a chunk, as PS3. */
static uint8_t *stream_root119(uintptr_t base,uint8_t *x,uint32_t *phase){return u3x_stream_root(base,x,phase);}   /* v7.212 C++ */
/* PS3 command types -> PS4 (CreateAnimCmds 0xfbe270 / emitters 0xfc68d0, 0xfcdaa0, 0xfd2360): 2->3, 3->4, 6->8 (anim u64 @+4,
 * layer @+0xc, frame @+0x10), 0x16->0x1b (same + @+0x14), 9->0xc (layer @+4), 10->0xd (layer @+4), 7->9 (@+4..+0x10, f @+0x14),
 * 8->0xa (@+4), 0x14->0x19 (layer id @+4), 0x15->0x1a. PS3 0xc (callback-emitted) has no PS4 producer. */
#define C4_LBEGIN 3
#define C4_LEND 4
#define C4_ANIM 8
#define C4_MANIM 0x1b
#define C4_SNAP 0xc
#define C4_NONE 0xd
#define C4_BLEND 9
#define C4_BLEND2 0xa
#define C4_LMARK 0x19
#define C4_LCOUNT 0x1a
/* 1.19 f_2d96a4(msg, bs, actor, tables, is_player, max): the anim state packer. Returns the bit count. */
static void loop_src119(uint8_t *buf,uint32_t n);   /* c009 loopback (U3MP_REPL119_LOOP=1) */
static uint32_t anim_pack_body119(uintptr_t base,uint8_t *msg,uint8_t *bs,uint8_t *actor,uint8_t **tabs,int param5,int param6,uint8_t **buf);
static uint32_t anim_pack119(uintptr_t base,uint8_t *msg,uint8_t *bs,uint8_t *actor,uint8_t **tabs,int param5,int param6){
 uint8_t *buf=0;return anim_pack_body119(base,msg,bs,actor,tabs,param5,param6,&buf);}   /* v7.98: buf is kept */
static uint32_t anim_pack_body119(uintptr_t base,uint8_t *msg,uint8_t *bs,uint8_t *actor,uint8_t **tabs,int param5,int param6,uint8_t **buf){
 return u3x_anim_pack_checked(base,msg,bs,actor,tabs,param5,param6,buf);}   /* v7.212 C++, v7.236 length check */
/* 1.19 f_2cf964(msg, player): weapons. Player+0x520 (PS3 +0x3d8) vt+0x70 (PS3 vt+0x38) = weapon in slot i (as PS4 0x78bbb7);
 * weapon item id +0x6ec (PS3 +0x568, f_6dd758), +0x6f0 (+0x56c), +0x6f2 (+0x56e), vt+0x438 (PS3 vt+0x210), vt+0x398
 * (vt+0x1c4): the gun vtable 0x16af910 lines up with PS3 0x1102218 entry by entry, PS4 inserts one virtual at 0x430; the
 * pair is checked by body: PS3 f_6d1bf8 = 2 or a +0x698/+0x6b8 bit, PS4 0xd49d40 = 2 or a +0x888/+0x8ac bit. */
static void weapons119(uintptr_t base,uint8_t *msg,uint8_t *pl){u3x_weapons_fill(base,msg,pl);}   /* r2 G48 */
/* 1.19 f_958ba8(char, sid) as PS4 inlines it (0x9fb2ff): char+0x80 (PS3 +0x54) -> *p -> +0x10 -> +0x18 == sid */
static int has_eff119(uint8_t *c,uint32_t sid){return u3x_has_eff(c,sid);}   /* v7.212 C++ */
/* 1.19 f_30a2d4(mgr, char): the net player of a character (MP only): Player -> local np of its screen; NetCharacter
 * (0xf2c86c22) -> the np whose code is char+0x3f8 (PS3 +0x2e8; PS4 apply 0x683a00 does the same match) */
static uint8_t *char_np119(uintptr_t base,uint8_t *c){return u3x_char_np(base,c);}   /* v7.212 C++ */
/* Component vtables (PS3 vtable -> PS4 vtable from the Player init factories 1.10 f_580bc0 / PS4 0xbd53f0; each used slot is
 * matched by function body): +0x520 0x10f7a68->0x16a0b70 (0x38->0x70 GetWeapon, 0x3c->0x78, 0x30->0x60, 0xc8->0x198),
 * +0x528 0x10e8c28->0x1689f50 (0x64->0xc8), +0x548 0x10ea708->0x168c8b0 (0x4c->0x98), +0x550 0x1105d28->0x16e1230 (0x3c->0x80),
 * +0x598 0x10eaa08->0x168cbb0 (0x54->0xa8), +0x5f8 0x10ebb48->0x168dd10 (0x34->0x68, 0x38->0x70), +0x688 0x10e8f98->0x168a2b0
 * (0x90->0x120), +0x668 0x10f7128->0x16a04e0 (0x30->0x60), +0x6c0 0x10f2368->0x1698a50 (0x2c->0x58). Component offsets follow
 * PS3 off -> 0x520 + (off-0x3d8)*2 (the factory order matches store for store). */
/* 1.19 f_2db504(msg, player): the player filler. Returns the packer's bit count (the message is sent when non-zero). */
static void loop119(uintptr_t base,uint8_t *actor,uint8_t **tabs,uint8_t *msg,uint32_t bits);   /* c009 */
static uint32_t player_fill119(uintptr_t base,ProfileState *s,uint8_t *msg,uint8_t *pl){
 return u3x_player_fill(base,s,msg,pl);}   /* v7.212 C++ */
/* 1.19 f_2f6910(player, mask): build, fill and send the state message. Allocation as PS4 0x6ae2cf..0x6ae305 (alloc context
 * 0xda1590(&0x14, file) / 0xd9e990(0x240, &0x14, 16), PS3 tag 0x1b), the PS4 inline ctor (0x6ae308..0x6ae45e = 1.19
 * f_2c5a98), send 0x7336e0 (f_30c554), release (+0xa refcount, vt+8), context pop 0xda18b0 (f_725008(0)). The 1.19 debug
 * loop under +0xb7 (copies to each np, f_2cab94) is a stripped debug option on PS4. */
static void player_send119(uintptr_t base,ProfileState *s,uint8_t *pl,uint32_t mask){
 u3x_send_player_state(base,s,pl,mask);}   /* v7.156 C++ */
/* 1.19 f_363ed4 0x373074..0x3732ec: per remote net player a send interval from the distance (15/s within 15 m, down to
 * 8/s at 30 m; the second local screen's np is skipped, a split-screen pair counts its nearer player), then send to the
 * due ones (mask), and the same for what the player rides (Player+0x6c0 vt+0x58 = PS3 +0x4a8 vt+0x2c) through PS4's own
 * NPC sender 0x6ae2a0 (= 1.19 f_2f6804). Called from respawn_tick at 1.19's place (np = this local net player). */
static uint32_t dist119(const uint8_t *a,const uint8_t *b){
 return u3x_distance(a,b);}   /* v7.156 C++ */
EXT static void repl_loop119(uintptr_t base,ProfileState *s,uint8_t *np,uint8_t *pl){
 u3x_replication(base,s,np,pl);}   /* v7.156 C++ */
/* ===================================================================================================================
 * c009 (agent C): the PS3 1.19 net anim state decoder f_377bb8 = 1.19 0x3869cc and the NetAnimCmdGenerator uses of it.
 *
 * PS4 keeps the NetAnimCmdGenerator (NetCharacter+0x400, PS3 +0x2f0; vtable 0x167ec40 = 1.19 0x10f5078) but Bluepoint cut
 * every call of the decoder: the apply 0x683670 (= 1.19 f_2a19c0) clears the snapshot's anim state and never fills it,
 * CreateAnimCmds 0x6847d0 (= f_2a1248) emits only the ice / blend commands, GetLocator 0x684cf0 (= f_2a0fac) blends two
 * locators that are never computed, and the generator init 0x682ea0 (= f_29f874) stores 0 in the 8 net anim tables. The
 * NPC sender 0x6ae2a0 (= f_2f6804 + f_2db1a0) sets the anim bit count to 0 at 0x6ae57f instead of calling the packer.
 *
 * Generator layout, PS3 1.19 -> PS4 (init 1.19 0xcb7f4.. / PS4 0x33b9a0, relocate f_29de14 / 0x683420):
 *  +0x4 + k*0x14 ice snapshots {p0,p1,p2; u8 pending @+0xc; f32 time @+0x10} -> +0x8 + k*0x20 {u64 x3; u8 @+0x18; f32 @+0x1c}
 *  +0x2c/+0x30 handle -> +0x48/+0x50, +0x34 -> +0x58, +0x38 row -> +0x60, +0x3c default index -> +0x64,
 *  +0x40 8 tables -> +0x68, +0x60 count -> +0xa8, snapshots +0x70 + i*0x300 -> +0xb0 + i*0x3f0, bits +0x1274 + j*300 ->
 *  +0x1854 + j*300, current state +0x197c -> +0x1f60 (count +0x1adc -> +0x2170), +0x1ae0 -> +0x2178, +0x1ae4 -> +0x217c,
 *  +0x1ae8 time -> +0x2180.
 * Snapshot (slot) layout: [0] u8 byte count ((bits+7)>>3, PS4 process 0x7185d8), +4 f32 time, +8 bits pointer, anim state
 *  entries PS3 +0xc (16 bytes {u32 key; anim; f32 phase; u8 valid; u8 marked}, count +0x16c) -> +0x10 (0x18 bytes {u32 key;
 *  u64 anim @+8; f32 @+0x10; u8 @+0x14; u8 @+0x15}, count +0x220, as PS4's consumer 0x6864f0 = f_2a20f0 reads them), message
 *  flags PS3 +0x228 -> +0x308.
 * Anim (PS3 -> PS4): +4 id -> +0x18, +0x24 flags, +0x28 clip (frames u16 @+0x22), +0x30 group name -> +0x38, +0x34 skeleton
 *  id -> +0x40 (PS4 emitter 0xfc69d2), +0x38 effect list {u32 key; i32 count; ...} -> +0x48 (PS4 0x6864f0 -> 0xfdd4d0).
 * Handle object vt+0xc4 (an int, the skeleton id compared with anim +0x34) -> vt+0x190: PS4 keeps exactly these calls at
 *  the places where 1.19 calls it before the decoder (0x683c5b, 0x6848d5, 0x68494a, 0x684b0c, 0x684d61, 0x684d87).
 *
 * Engine counterparts used: f_29f1f0 = 0x6844f0 (xmm0 time, gen, &idx, &blend); f_2bbd48 frame reader = 0x7bf430 (bs, frame);
 *  f_a13238 vector reader = 0x1274970; f_8abd94 (index) = LookupAnimByIndex 0xfd7980's table walk (count; 16-byte entries
 *  {sid; u64 slot}; anim = *slot); f_89a3ac (art group by sid) = the package probe of find_art_group (f004) with the sid
 *  given; f_7d6ba0 channel sample = 0xfbaca0 (int, req {+8 anim, +0x10 channel, +0x14 phase, +0x18 ptr, +0x20 u8, +0x21 u8},
 *  out {scale; quat; trans}); f_7eca54 locator lerp = 0xfd3c60 (out, a, b, xmm0 t); f_8067d4 retarget = RetargetAnim
 *  0xdc4240 (xmm0 frame, anim, skeleton, &joint params, &valid bits: PS4 drops PS3's duplicate anim argument);
 *  f_7f8b44 + f_7f8c6c (streamed anim: phase and chunk) = 0xfd4c60 (anim, id, &phase), PS4's single replacement that its
 *  own emitter 0xfc6969 calls for flag-4 anims; the phase given is the stream record's stored phase (PS4 rec+0x20 = PS3
 *  rec+0x10, which f_7f8b44 reads); no record -> phase 0.0 (1.19 0x116444c-0x7ff0) and no anim, as f_7f8b44 / f_7f8c6c.
 * Command buffer (PS4 {u64 buf; u32 cap; u32 pos; u32 max; u64 count slot}): PS3 header type<<16|words -> PS4 u16 type,
 *  u16 words; pointers take 8 bytes; PS3's running crc (f_719954) -> PS4's per-layer max instance index (+0x10), updated
 *  with every instance index a command carries, as PS4's emitters do (0x6847d0, 0xfc6a36). Types (the reverse of c008's
 *  map): 6 [anim][i][f] -> 8 (5 words), 7 [a][b][c][m][f] -> 9 (6), 8 [i] -> 0xa (2), 9 [i][p][p][p] -> 0xc (8),
 *  10 [i] -> 0xd (2), 0xb [i][p][p][p] -> 0xe (8), 0xc [a][b] -> 0xf (3, executor 0xdb5070 case 0xf).
 * PS3 traps (tw 31) become "stop": the decode stops (V119.err), CreateAnimCmds / GetLocator roll back and run PS4's native
 *  function, the apply leaves the state cleared (PS4's result), a reader overrun stops at the message length.
 * Switches (ec[0]): bit9 U3MP_DEC119=0 (all decoder uses off, PS4 natives), bit10 U3MP_NETANIM_TABLES119=0 (PS4's empty
 *  tables), bit11 U3MP_NPC_ANIM119=0 (NPC bit count 0 as PS4), bit12 U3MP_REPL119_LOOP=1 (loopback check, see NOTES-c009).
 * Diagnostics ec[80..104]: see NOTES-c009. */
static int f_eq119(uint32_t a,uint32_t b){return u3x_float_equal(a,b);}   /* v7.175 C++ */
/* call fn(rdi..rcx) with xmm0 = f0 (float bits); returns rax */
static uint64_t call_x0_119(uintptr_t fn,uint32_t f0,uint64_t a0,uint64_t a1,uint64_t a2,uint64_t a3){
 return u3x_call_float_arg(fn,f0,a0,a1,a2,a3);}   /* v7.175 C++ */
/* ---- bit reader: 1.19 f_a123c0 / f_a123d8 (bs, buf, bytes: cap = bytes*8) / f_a12430 (n bits, MSB first in 32-bit words;
 * PS4 keeps the words native, as its writer 0x1272b50 and reader 0x1272c00). The struct starts as PS4's {u64 buf; u32 pos;
 * u32 cap; u32 0} so PS4's frame / vector readers can read from it; there cap is the 300-byte buffer (2400 bits: memory-safe,
 * PS4's readers trap past it) and lim is the message length that f_a123d8 sets; PS3 traps on a read past lim (f_a12430
 * tw after the read), here err stops the decode. A 0-bit read returns 0 without moving (PS3: srw by 32). ---- */
typedef struct{uint32_t *w;uint32_t pos,cap,z;uint32_t lim,err;}BR119;
static void br_init119(BR119 *r,uint8_t *buf,uint32_t nbytes){u3x_bits_init(r,buf,nbytes);}   /* v7.175 C++ */
static uint32_t br119(BR119 *r,uint32_t n){
 return u3x_bits_read(r,n);}   /* v7.175 C++ */
static void br_frame119(uintptr_t base,BR119 *r,uint8_t *f){   /* f_2bbd48 = PS4 0x7bf430 */
 u3x_bits_frame(base,r,f);}   /* v7.175 C++ */
static void br_vec119(uintptr_t base,BR119 *r,uint8_t *v){     /* f_a13238 = PS4 0x1274970 */
 u3x_bits_vector(base,r,v);}   /* v7.175 C++ */
/* BoundFrame ctor 1.19 f_952e28 as PS4 inlines it (0x683b30..: the apply's stack frame): {0x7fcccccc, 0x7fcccccc, u64 0,
 * u32 0, Locator {0, (0,0,0,1)}, Locator {0, (0,0,0,1)}} (0x60 bytes) */
static void frame_init119(uint8_t *f){
 u3x_frame_fresh(f);}   /* v7.175 C++ */
/* ---- lookups ---- */
/* 1.19 f_89a3ac(levels, sid, 0): the art group with that sid in any loaded package (find_art_group's probe, sid given) */
static uint8_t *art_group_sid119(uintptr_t base,uint32_t sid){
 return u3x_art_group_sid(base,sid);}   /* v7.175 C++ */
/* 1.19 f_8abd94(art table, index): entries[index].slot -> anim, 0 past the count (unsigned compare) */
static uint8_t *tab_idx119(uint8_t *g,int32_t i){
 return u3x_anim_by_index(g,i);}   /* v7.175 C++ */
/* 1.19 f_8abfc4(art table, sid) probes the table's hash index (+0xc). The PS4 hash layout is not verified, so the sorted
 * entry array (same entries) is searched with f_8abdc4's loop: same anim for a unique sid. Reached only with the packet's
 * mode bit set (debug "data-independent" packets, never sent) and on the generator's no-snapshot path. */
static uint8_t *tab_sid119(uint8_t *g,uint32_t sid){
 return u3x_anim_by_sid(g,sid);}   /* v7.175 C++ */
/* 1.19 f_3868f0(row, col) = f_89a45c(levels, names[row*8+col], 0); names 1.19 0x10f8100 (6 x 8; inline strings: the helper
 * link has no absolute relocations); row > 5 or col > 7 trap on PS3 (0 here). */
static uint8_t *net_ag119(uintptr_t base,uint32_t row,uint32_t col){
 return u3x_net_art_group(base,row,col);}   /* v7.175 C++ */
/* PS4 0xfd4c60's record search (stream records 0x1d46090 / count 0x1d46088): the record whose id lists hold 'id' */
static uint8_t *stream_rec119(uintptr_t base,uint32_t id){
 return u3x_stream_record(base,id);}   /* v7.175 C++ */
/* generator: handle object (PS3 +0x2c/+0x30 -> +0x48/+0x50) and its vt+0xc4 -> vt+0x190 */
static int32_t gen_skel119(uint8_t *g){return u3x_generator_skeleton(g);}   /* v7.175 C++ */
static int32_t gen_pick119(uintptr_t base,uint8_t *g,uint32_t *bl){   /* f_29f1f0 = 0x6844f0 */
 return u3x_generator_pick(base,g,bl);}   /* v7.175 C++ */
/* ---- visitors (1.19 vtables 0x10f5148 state, 0x10f50e8 commands, 0x10f50b8 locator; base 0x10f80d8: slots 1/2/4..7
 * return, 0 f_386884, 3 / 8 return) ---- */
typedef struct{
 uint8_t frame[0x60];      /* PS3 vis+0x10 (the decoder reads the packet's BoundFrame into it) */
 uint8_t loc[4][0x30];     /* locator: PS3 vis+0x70 + i*0x30 {scale; quat; trans} */
 uintptr_t base;
 uint8_t *ents;            /* state: PS3 vis+0x68 (the snapshot's entries) */
 uint8_t *cb;              /* commands: PS3 vis+0x68 (command buffer) */
 int32_t v1,v2;            /* PS3 vis+4 / +8 (decoder args 4 / 5) */
 uint32_t v60,key,chan;    /* PS3 vis+0x60 (13-bit id), +0x6c (state key), +0x130 (locator channel) */
 uint8_t abort,active;     /* PS3 vis+0x64, +0x134 */
 int kind,err;             /* 0 state, 1 commands, 2 locator; err = the PS3 trap that would have fired (ec[82]) */
}__attribute__((aligned(16))) V119;
/* v7.190 C++: the visitors are u3x_v_* in u3x_fixes.cpp */
static void verr119(V119 *v,int code){u3x_v_error(v,code);}   /* 12: empty */
static void c6_119(V119 *v,uint32_t f,uint8_t *an,uint32_t i){u3x_v_pose(v,f,an,i);}   /* f_21568 -> 8 */
static void c7_119(V119 *v,uint32_t f,uint32_t a,uint32_t b,uint32_t c,uint32_t m){u3x_v_blend_cmd(v,f,a,b,c,m);}   /* f_21458 -> 9 */
static void c10_119(V119 *v,uint32_t i){u3x_v_bind(v,i);}   /* f_29e9b8 -> 0xd */
static void c9b_119(V119 *v,uint16_t ty,uint32_t i,uint64_t p1,uint64_t p2,uint64_t p3){u3x_v_ice(v,ty,i,p1,p2,p3);}   /* f_29ec98 / f_29eae0 */
static uint32_t frames119(uint8_t *an){return u3x_v_frames(an);}
static void vbegin119(V119 *v){u3x_v_begin(v);}
static void vblend119(V119 *v,uint32_t w,uint32_t a,uint32_t b,uint32_t c,uint32_t m){u3x_v_blend(v,w,a,b,c,m);}   /* slots 1 / 2 */
static void vanim119(V119 *v,uint32_t ph,uint8_t *an,uint32_t dep){u3x_v_anim(v,ph,an,dep);}   /* slot 3 */
static void vsub0_119(V119 *v,uint32_t x){u3x_v_sub0(v,x);}   /* slot 4 */
static void vsub3_119(V119 *v,uint32_t dep){u3x_v_sub3(v,dep);}   /* slot 5 */
static void vsub4_119(V119 *v){u3x_v_sub4(v);}   /* slot 6 */
static void vsub2_119(V119 *v){u3x_v_sub2(v);}   /* slot 7 */
static void vmark119(V119 *v,uint32_t ph,uint8_t *an,uint32_t dep,int32_t p7){u3x_v_mark(v,ph,an,dep,p7);}   /* slot 8 */
/* ---- 1.19 0x3869cc (v, snapshot bits, bytes, flags (+0x228), tables, a4 -> v+4, a5 -> v+8, a6 default index, a7) ---- */
static void dec119(V119 *v,uint8_t *bits,uint32_t nbytes,uint32_t flags,uint8_t **tabs,int32_t a4,int32_t a5,int32_t a6,int32_t a7){
 u3x_decode_anim(v,bits,nbytes,flags,tabs,a4,a5,a6,a7);}   /* v7.197 C++ */
/* ---- kind 117: apply 0x683670 at 0x683c61 (after the state clear and the vt+0x190 call; rbx = generator, r12 = snapshot
 * offset): 1.19 f_2a19c0 0x2a1d6c..0x2a1de0, decode the new snapshot into its anim state (state visitor, args 0, 0, +0x3c,
 * vt+0xc4). PS4 discarded vt+0x190's result; it is a getter (PS4 calls it twice in a row in 0x684d61 / 0x684d87), so the
 * helper calls it again for the value. ---- */
EXT static void apply_dec119(uintptr_t base,uint64_t *regs){
 u3x_apply_decode(base,regs);}   /* v7.146 C++ */
/* 1.19 f_29e318(t, other, gen): the ice snapshot that holds neither time, marked pending with time t; both held -> trap */
/* one snapshot of f_2a1248 (0x2a1304..0x2a1490 / 0x2a14a4..0x2a1650): a held, finished ice snapshot -> 9 [i][p1][p2][p0];
 * else decode it (command visitor, args i / a5) and ice it again (0xb), taking the oldest free ice snapshot (f_29e318) or,
 * for a held one, clearing its pending byte on proc group 1. Returns 1 when the held ice was used. */
/* ---- kind 118: CreateAnimCmds 0x6847d0 entry (value stub; rdi gen, rsi command buffer, rdx skeleton (PS4's added arg),
 * ecx proc group, r8d instance base) = 1.19 f_2a1248(gen, cmds, group, base). Returns 0 (PS4's native runs) when off or
 * when a PS3 trap would fire (buffer, ice snapshots restored first). ---- */
EXT static int cac119(uintptr_t base,uint64_t *regs){
 return u3x_anim_commands_safe(base,regs);}   /* v7.147 C++, v7.236 no fall-back to the decoder-less native */
/* ---- kind 119: GetLocator 0x684cf0 entry (value stub; rdi out, rsi gen, edx channel) = 1.19 f_2a0fac(out, gen, channel):
 * locator visitor over the snapshot pair (args 0,0 / 1,1), then f_29904 (pos lerp, quat slerp) through 0xfd3c60 on
 * {pos; quat; 0} (f_29904 and f_7eca54 both lerp the vector and slerp the quat with f_a82dbc). Returns out in rax. ---- */
EXT static int gloc119(uintptr_t base,uint64_t *regs){
 return u3x_blended_locator(base,regs);}   /* v7.146 C++ */
/* ---- kind 120: generator init 0x682ea0 at 0x682f0b (after PS4's zeroing of the tables; r12 = generator) = 1.19 f_29f874
 * 0x29f8a8..0x29f8d8: tables[i] = f_3868f0(+0x38 row, i). NPC generators (0x339ad0 -> 0x33b9a0, stored at char+0x3d8)
 * and NetCharacters (0x68fbe0 -> 0x33b9a0, +0x400) both come here. ---- */
EXT static void gen_tables119(uintptr_t base,uint8_t *g){
 u3x_ice_note(g);   /* v7.236 C++: ice copy sizes noted (diagnostic) */
 if(X.ec[0]&1024)return;
 uint32_t row=*(uint32_t*)(g+0x60),got=u3x_anim_tables(base,g,net_ag119);   /* v7.113 C++ */
 X.ec[88]++;X.ec[89]=(X.ec[89]&0x80000000u)|row<<8|got;}
/* ---- loopback (U3MP_REPL119_LOOP=1): the sender decodes its own packet (state and command visitors, args as the receiver's
 * apply / CreateAnimCmds with base 0) and compares the decoded anim commands with the PS4 commands the packer read. ---- */
static uint8_t *loop_src_an119[64] EXT_BSS;static uint32_t loop_src_fr119[64] EXT_BSS;static uint32_t loop_src_n119 EXT_BSS;
static uint32_t loop_cmd119[0x800] EXT_BSS;static uint8_t loop_ents119[0x220] EXT_BSS;
static void loop_src119(uint8_t *buf,uint32_t n){       /* the packer's PS4 command list: its anim commands (type 8) */
 loop_src_n119=0;uint32_t p=0;
 while(p<n){uint32_t ty=*(uint16_t*)(buf+4*(size_t)p),len=*(uint16_t*)(buf+4*(size_t)p+2);if(!len)break;
  if(ty==C4_ANIM&&len>=5&&loop_src_n119<64){loop_src_an119[loop_src_n119]=*(uint8_t**)(buf+4*(size_t)(p+1));
   loop_src_fr119[loop_src_n119]=*(uint32_t*)(buf+4*(size_t)(p+4));loop_src_n119++;}
  p+=len;}}
static void loop119(uintptr_t base,uint8_t *actor,uint8_t **tabs,uint8_t *msg,uint32_t bits){
 if(!(X.ec[0]&4096)||!bits||!actor||!tabs)return;
 X.ec[93]++;
 uint32_t nb=((bits+7)>>3)&0xff,fl=*(uint32_t*)(msg+0x148);   /* as the PS4 process 0x7185d8 */
 int32_t sk=((int32_t(*)(uint8_t*))(*(uintptr_t**)actor)[0x190/8])(actor);
 V119 v;v.base=base;v.kind=0;v.err=0;v.ents=loop_ents119;v.cb=0;v.chan=0;
 for(int i=0;i<0x220;i++)loop_ents119[i]=0;
 dec119(&v,msg+0x18,nb,fl,tabs,0,0,0,sk);
 X.ec[95]=*(uint32_t*)(loop_ents119+0x210);if(v.err){X.ec[94]++;return;}
 uint8_t cb[0x20] __attribute__((aligned(16)));for(int i=0;i<0x20;i++)cb[i]=0;
 *(uint32_t**)cb=loop_cmd119;*(uint32_t*)(cb+8)=0x800;
 v.kind=1;v.err=0;v.cb=cb;
 dec119(&v,msg+0x18,nb,fl,tabs,0,0,0,sk);
 uint32_t n=*(uint32_t*)(cb+0xc);X.ec[100]=n;if(v.err){X.ec[94]++;return;}
 uint32_t p=0,k=0,nd=0,um=0,mx=X.ec[99];
 while(p<n){uint32_t ty=*(uint16_t*)(loop_cmd119+p),len=*((uint16_t*)(loop_cmd119+p)+1);if(!len)break;
  if(ty==8){uint8_t *an=*(uint8_t**)(loop_cmd119+p+1);uint32_t fr=loop_cmd119[p+4];nd++;
   uint32_t j=k;while(j<loop_src_n119&&loop_src_an119[j]!=an)j++;
   if(j>=loop_src_n119)um++;
   else{uint32_t d=f_sub(fr,loop_src_fr119[j])&0x7fffffffu;if(f_lt(mx,d))mx=d;k=j+1;}}
  p+=len;}
 X.ec[96]=loop_src_n119;X.ec[97]=nd;X.ec[98]+=um;X.ec[99]=mx;}
/* ---- kind 121: NPC sender 0x6ae2a0 at 0x6ae57f (after vt+0x698; rbx = message, r15 = NPC, rax = its table holder, a
 * NetAnimCmdGenerator made by 0x33b9a0): 1.19 f_2db1a0 0x2db2e4..0x2db318: bits = f_2d96a4(msg, bs(msg+0x14 -> +0x18,
 * 300), npc, holder+0x40 -> +0x68, 0, 2); msg+0x140 -> +0x144 = bits (PS4 stores 0). ---- */
EXT static void npc_bits119(uintptr_t base,uint64_t *regs){
 u3x_npc_bits(base,regs);}   /* v7.146 C++ */
/* ===================================================================================================================
 * c010 (agent C): PS3 1.19 hat-stat icons on the killed-by card (1.19 player card 0x66d284; 1.10 f_66255c has none).
 * 1.19 adds two icons after the booster / kickback icons: v = f_36c9dc(killer np, i) for i = 0, 1, each counted in the card
 * width (0x66d5a4..0x66d5f4) and drawn as item type 2 (v != 0) after the last icon (0x66e300..0x66e480).
 * PS4: the card is inlined in HudNet::Draw 0xc32ba0 (Bluepoint layout code). Killer np = [rbp-0x12b0] (PS3 uVar6 =
 * f_3097d0(mgr, np+0x408)), icon count 0xc38989..0xc38999 (ecx = boosters + kickback), icon rect vector (x, y, w, h) in xmm0
 * at 0xc3a945 (the kickback icon's), icon draw = 0x640ac0(xmm0 [rbp-0x450], layout rbp-0x420, item, 0, &size, &zero16, -1)
 * + 0x63d6f0(0x17bd090, ctx [rbp-0x11f8], color rbp-0x450, layout, rect, 0, 0, -1) with rect = slot + (2, 2, -4, -4), and
 * the next slot at x + w + 4 (PS4's per-icon advance, 0xc3a929..0xc3a93d). PS4 item type 2 = {u32 2; u64 texture-cache
 * entry @+8; 0, 0, 0, -1, 0, -1, 1.0} (the np+0xb78 item at 0xc3a2f3: PS3 np+0x660), drawn when entry+0x10 (loaded) is set.
 *  f_36c9dc(np, i): look = f_36c900(np, 0) (= PS4 0x784b20); table 0xb8a3581b and !np+0x621 (c006's np119 hatoff); item =
 *   f_436a6c(.., look[0], look[1], 0, look[2]) (= PS4 0x9952a0(0, ...)); the first row with that item; i < npairs ->
 *   f_207398(pair multiplier, pair stat).
 *  f_207398(mult, stat, -, buf): the stat's row in 1.19 table 0x53ed596a (last match; batch49 adds it), plus or minus
 *   texture by stat class and mult vs 1.0 (1/mult for 0x7da763c9 / 0x2d57beb9 / 0x8b73121a), f_940fec(cache 0x13781b0,
 *   name) = PS4 0x1189430(0x1b31128, name) (returns the cache entry + 0x10 once loaded, else 0 and queues the load, as
 *   PS3 returns the handle only in state 3). Not ported: its percent text into the caller's buffer (f_7aac60 / f_d793d4),
 *   which f_36c9dc discards.
 * Kind 122: count site (ec[105] = PS4's sum + the 1.19 icons; the stub loads it into ecx and tests it for the je). Kind 123: draw
 * at 0xc3aa8d (the join after the kickback icon); the rect is saved by the stub at 0xc3a945 into ec[106..109].
 * Switch U3MP_HATICON119=0 (ec[0] bit13). Diagnostics: ec[110] cards counted, ec[111] icons drawn, ec[112] last icon count. */
static uint64_t call8_119(uintptr_t fn,uint32_t f0,uint64_t a0,uint64_t a1,uint64_t a2,uint64_t a3,uint64_t a4,uint64_t a5,uint64_t s0,uint64_t s1){
 return u3x_call8(fn,f0,a0,a1,a2,a3,a4,a5,s0,s1);}   /* v7.173 C++ */
/* 1.19 f_207398 (texture part): the icon for (stat, multiplier) */
static uint8_t *stat_icon119(uintptr_t base,ProfileState *s,uint32_t stat,uint32_t m){
 return u3x_stat_icon(base,s,stat,m);}   /* v7.173 C++ */
/* 1.19 f_36c9dc(np, i) */
static uint8_t *hat_icon119(uintptr_t base,ProfileState *s,uint8_t *np,int32_t i){
 return u3x_hat_icon(base,s,np,i);}   /* v7.173 C++ */
/* kind 122: 0xc38989 (regs stub), the 1.19 count of the two icons (0x66d5a4) -> ec[105] */
EXT static void hat_icon_count119(uintptr_t base,ProfileState *s,uint64_t *regs){
 u3x_hat_icon_count(base,s,regs);}   /* v7.173 C++ */
/* kind 123: 0xc3aa8d (regs stub), after the kickback icon: 1.19 0x66e300..0x66e480 */
EXT static void hat_icon_draw119(uintptr_t base,ProfileState *s,uint64_t *regs){
 u3x_hat_icon_draw(base,s,regs);}   /* v7.173 C++ */

/* ===== c011 (agent C): M3 player emblem. The PS3 1.19 emblem render pass as a CPU COMPOSITOR (replaces PS3's GPU pass).
 *
 * PS3 1.19 data flow (all ported 1:1 except the GPU part, which is replaced by the CPU compositor below):
 *  EmblemMgr (PS3 object at module base -0x7f00; PS4 0x17c3440, layout widened for 64-bit pointers):
 *   PS3 +0 editor-mode byte, +1 frame toggle, +2 render-failed, +4/+8 editor layers (4 x 8 bytes), +0x28/+0x2c player layers,
 *   +0x4c local user, +0x50/+0x54 player RT records, +0x58 player index (-1), +0x5c/+0x60 editor RT records, +0x64 default
 *   record, +0x68 editor selection, +0x6c editor countdown  =  PS4 +0, +1, +2, +4/+8, +0x28/+0x2c, +0x4c, +0x50/+0x58, +0x60,
 *   +0x68/+0x70, +0x78, +0x80, +0x84 (PS4 0x66b510 = f_27dd7c, 0x66bc30 = f_27e47c, 0x66cc70 / 0x66c580 use +0x80, static
 *   init 0x66ce80 sets +0 = 1 as f_27e08c does).
 *  f_27daa8 (update, called by the MP frame f_292b98 at 0x292edc while MP is active)  -> emb_daa8_119
 *  f_27d050 (player mode: pick a player without records, copy np+0x7c4, allocate two 128 records, render one frame, then hand
 *            them to the player with f_3687d0 or free them if the render failed)  -> emb_d050_119
 *  f_27d790 (editor re-render request: local profile emblem, two 256 records, countdown 4)  -> emb_d790_119
 *  f_64f288 0x64f300..0x64f3fc (render frame: while (base-0x7ff0)+0x4a8 is set, for slots 0xa4/0xa5 record f_27d29c, RT size
 *            f_27c888, f_744f78 + f_7423fc + f_2800c8)  -> emb_pass119 (the RT becomes a PS4 texture made from CPU pixels)
 *  f_2800c8 (the layer pass)  -> emb_render119, see below.
 *  f_2ba184 (NetCharacter look, vtable slot 18 = PS4 0x6a5c50, called every frame by 0x10c381a after obj+0x258 = 0, as PS3
 *            0x85a050 after obj+0x194 = 0): records np+0x660/+0x664 (team mode) or the editor records (tag 0x32f2d823), default
 *            record +0x64; f_854140 per instance  -> emb_look119 (kind 124, value stub at 0x6a5c50).
 *  Records: PS3 VRAM pool objects (+0 memory, +4 u16 pin, +0x10 GCM texture descriptor). PS4 record = EmbRec119 below (the
 *  Ndgi texture is the instance-table value, as runtime texture tables hold texture-manager entry+0x10, e.g. 0xed78c2).
 *  np records PS3 np+0x660/+0x664 = PS4 player array 0x17cb3f0 + i*0x9c0, +0x7e8/+0x7f0 (= 0x17cb060 + 0xb78/0xb80: the store
 *  0x709da0 / 0x710adf / 0x78fa20 zeroes, as PS3 SetEmblem f_368840 does after f_27cc50). PS3 frees records where it zeroes
 *  them (f_27cc50, f_27cbe4, f_27cf6c, f_27dd7c); PS4 kept those functions without the frees (0x78fa20, 0x66b090, 0x66b510, the
 *  player-info store), so emb_sweep119 frees every record no slot references any more, once per frame before the update.
 *
 * f_2800c8 (PS3 1.19, read from its asm and the hud.pak shader data, see M3-FINDINGS / NOTES-c011):
 *  clear the RT to 0 (f_a96f28(0) CLEAR_COLOR 0x1d90, f_a96d30(0xf0) CLEAR_SURFACE 0x1d94), scissor (1,1,size-2,size-2)
 *  (f_98d330, 2D matrix identity), 4 layers i: data L = +0x2c+8i (player) or +8+8i (editor, +0x58 < 0); icon = L[0] % list
 *  count (lists 0xc7f8567c / 0xe2311588 / 0x03ffae77 / 0x03ffae77), icon 0 = no layer; colour = palette 0xbcbbdfbd entry
 *  L[4] % count, (r, g, b, 1.0); scale s = min + (max - min) * L[2] / 255 (ranges 0x88f9bd5c / 0x83d670b8 / 0x4cfe4557 x2);
 *  angle = L[1] * 2pi/255, cos/sin = f_d574b4; full-target quad, UV = 0.5 + (0.5/s) R(angle) corner, V flipped by f_98d58c;
 *  slot 0 shader 0xd2e394f5 "general/hud/alpha-only-icon" (fp 0x2fd0): rgb = col.rgb (fp16 H reg) * white, a = col.a * layer.r;
 *  slot 1 shader 0xa0cf6221 "general/hud/alpha-mask-icon" (fp 0x2df0): a = layer.r * col.a * mask.r, mask = emblems/spray-01
 *  over the full quad; blend SRC_ALPHA / ONE_MINUS_SRC_ALPHA (rgb), ONE / ONE_MINUS_SRC_ALPHA (alpha), ADD.
 *  slot 1 also draws the icon's decals (entry+8 -> {grid, count, {p, x, y}}) with alpha-only and emblems/emblem-16-drips:
 *  half size 0.0625 s + 0.0625, centre ((R (x', y') + 1) / 2), x' = (2x/grid - 1) s, atlas cell (p & 3, p >> 2) of 4x4.
 *  Sampler of every layer/mask/drips texture (PS3 DDS loader f_9416cc): min LINEAR_MIPMAP_LINEAR, mag LINEAR, LOD bias -1.0
 *  (0x1f00), max LOD = DDS mip count, clamp to edge. Texture missing or not loaded (f_940fec returns 0) -> +2 = 1.
 * CPU compositor: per quad the PS3 float math (SSE scalar = the PPC single ops; fmadds as an exact-product double FMA);
 *  per pixel integer fixed point: pixel centres, affine UV, LOD = log2(max texels per pixel) + bias (8-bit fraction),
 *  trilinear on 8-bit bilinear weights, DXT1/DXT5 R channel (r2 = (2r0+r1)/3, r3 = (r0+2r1)/3 / (r0+r1)/2, 0), blend on the
 *  8-bit target rounded to nearest. Output: an A8R8G8B8 texture (ndgi format 0x57 = GNM 8_8_8_8 swizzle ZYXW = bytes B,G,R,A)
 *  made with OrbisCreateTextureMap2d 0xe76cb0 (memory context 0xe, linear initial data, tiled by the device) and
 *  NdgiCreateTexture 0xe77c50, as CreateDdsTextureFromMemory 0xe4fb80 does; the replaced texture is released like a texture
 *  cache eviction (0xe73490, 0xe4e8f0(0x18c8960, map, frame)). Source textures: the texture cache 0x1189430 entry (as PS3
 *  f_940fec: only a successfully loaded entry counts) and the same file read with 0xf42fa0 under PushAllocator(0x30) as 0xe500d0.
 *  A record whose last composite had the same inputs (layers, slot, size, layers drawn) keeps its texture (identical pixels).
 *
 * Timing: emb_tick119 = the per-frame helper tick (kind 16) while 0x17d5c61 (PS3 MP-active byte (+5), the condition of the
 *  f_292b98 call at 0x3ffa0); the render pass runs right after the update, in the same frame as PS3's f_64f288.
 *  NetInit f_295c14 0x2962d4 f_27d610 (default 4x4 record + part tables) and its worker f_2966f0 0x297e14 f_27d5b8: emb_netinit119
 *  at the helper's NetInit point (replaces the s003 call of 0x66b510 = f_27dd7c, which PS3 does only at match setup).
 *  NetShutdown f_280f68 0x281038 f_27cb54: emb_shutdown119. Editor datasources f_33afa0 (PS4 0x76a610, case 9 dropped) and
 *  f_33ee48 (PS4 0x7699e0) call f_27d790 on event 9: kinds 125 / 126 (value stubs at the entries). Match setup f_31ef18 0x31fad4
 *  (PS4 0x72b570: r23 = 0 and r18 != 0 -> f_27d790): kind 127 at 0x72bdf1.
 *  Not hooked: f_28f800 0x290248 (online connect end, not ported in dev), the server notice in f_298e5c (a012).
 * Switch U3MP_EMBLEM119=0 (ec[0] bit14): everything off, NetInit back to s003. U3MP_EMBLEM119_FLIPV=1 (ec[0] bit15): rows
 * reversed (orientation test aid). Diagnostics ec[113..130], c013: ec[133..146] (see emb_look119 / emb_netinit119). ===== */
#define EMB119 0x17c3440u
#define EMB_OFF119 (X.ec[0]&0x4000u)
/* c014: a record is a texture-info handle, the layout every PS4 reader of a UI "type 2" value uses (the texture cache
 * entry+0x10 that 0x1189430 returns): +0 Ndgi texture, +8 map, u8 +0x10 loaded, +0x14 width, +0x18 height, +0x1c format.
 * Readers: UI layout 0x640dd2 (+0x10, +0, +0x14, +0x18), UI draw 0x642152 (+0x10), quad 0x63dc40 (+0x1c: 0x47 = DXT1 shader
 * 0xc, else 0xb), packet executor 0xe41c7a (+0x10, +0). c011's record had the size at +0x10, so the editor preview 0x76acc0
 * (type 2, EmblemMgr+0x68) read loaded = 0 (size 0x100) and was never drawn. Like a PS3 VRAM pool record, a record always
 * holds a valid texture: until its first composite (and after it is freed) that is the shared 4x4 transparent texture
 * er119_fb (never released), so no reader can see texture 0. map != 0 = the record owns its texture. */
typedef struct { uint64_t tex,map; uint8_t ok,pad0[3]; uint32_t w,h,fmt; uint32_t size; uint16_t pin,used; uint8_t key[40]; } EmbRec119;
static EmbRec119 er119_fb EXT_BSS;   /* c014: permanent fallback texture (4x4, zero pixels) */
static EmbRec119 er119[24] EXT_BSS;
static uint8_t er119_flag EXT_BSS;   /* PS3 render flag (base-0x7ff0)+0x4a8 */
static const uint8_t er119_zero[64]={0};
/* t007 (test agent): no direct gs:0 reads in the helper. The PC port runs the guest on Windows, where gs is the host TEB; guest
 * TLS reads only work in PS4 code the port has prepared. So a fixed "gs:0 - 0x28 / - 0x20" from helper code read address
 * 0xffffffffffffffd8 (v5.81 fault in emb_release119+0x30 after a match, via emb_tick119; image TLS offset variables
 * 0x16e3020 = -0x28, 0x16e2f28 = -0x20).
 *  - allocator stack (*(TLS + [0x16e2f28])): PS4's own getter 0xda0d60 (push rbp; mov rax,gs:0; add rax,[0x16e2f28]; mov rax,[rax]).
 *  - job-manager word (*(TLS + [0x16e3020]), used only to choose the frame counter 0x1c1f300 vs 0x1c1f308): PS4 has no getter.
 *    The helper's tick and look calls run on the main thread, which is not a job worker, so the native test (0x56e7c0..0x56e801)
 *    answers "not a worker" -> 0x1c1f308. */
/* v7.209 C++: sin / cos, the number conversions, engine temp memory, DDS files, the DC binds, the records and the CPU
 * rasterizer are u3x_* in u3x_fixes.cpp; the record arrays stay here while the C emblem code uses them */
static uint32_t sincos119(uint32_t x,uint32_t flag){return u3x_sincos119(x,flag);}
EXT static uint32_t h16q16_119(uint32_t f){return u3x_h16q16(f);}
EXT static void *emb_alloc119(uintptr_t b,uint64_t n){return u3x_emb_alloc(b,n);}
EXT static void emb_free119(uintptr_t b,void *p){u3x_emb_free(b,p);}
EXT static uint8_t *emb_cache119(uintptr_t b,const char *name){return u3x_emb_cache(b,name);}
EXT static uint8_t *emb_h119(uintptr_t b,ProfileState *s,uintptr_t ha){return u3x_emb_h(b,s,ha);}
EXT static void emb_dcbind119(uintptr_t b,ProfileState *s){u3x_emb_dcbind(b,s);}
static int emb_isrec119(uint64_t p){return u3x_emb_isrec(p);}
typedef struct { uint64_t tex,map; uint32_t age,pad; } EmbRet119;
static EmbRet119 erq119[16] EXT_BSS;
static void emb_retick119(uintptr_t b){u3x_emb_retick(b);}
EXT static EmbRec119 *emb_ralloc119(uint32_t size){return (EmbRec119*)u3x_emb_ralloc(size);}
EXT static void emb_rfree119(uintptr_t b,EmbRec119 *r){u3x_emb_rfree(b,r);}
EXT static int emb_texcreate119(uintptr_t b,EmbRec119 *r,const uint8_t *pix,uint32_t size){return u3x_emb_texcreate(b,r,pix,size);}
EXT static void emb_fb119(uintptr_t b){u3x_emb_fb(b);}
typedef struct { uint8_t *file; uint32_t w,h,maxl,bs; uint32_t off[16]; } Dds119;
EXT static int emb_dds119(uintptr_t b,const char *name,Dds119 *t){return u3x_emb_dds(b,name,t);}
typedef struct { uint32_t l0,l1,f; } Lod119;
EXT static void emb_quad119(uint8_t *pix,uint32_t size,const Dds119 *t,const uint32_t *uv,const Dds119 *mk,const uint32_t *c16){u3x_emb_quad(pix,size,t,uv,mk,c16);}
EXT static void emb_decal119(uint8_t *pix,uint32_t size,const Dds119 *t,const uint32_t *q,const uint32_t *c16){u3x_emb_decal(pix,size,t,q,c16);}
static uint8_t *emb_rec_slot119(uintptr_t b,uint32_t sl){return u3x_emb_rec_slot(b,sl);}   /* f_27d29c, v7.188 C++ */
static uint32_t emb_size119(uintptr_t b){return u3x_emb_size(b);}   /* f_27c888, v7.188 C++ */
/* PS3 f_2800c8(emb, sl) */
EXT static void emb_render119(uintptr_t b,ProfileState *s,uint32_t sl,EmbRec119 *rec,uint32_t size){
 u3x_emb_render(b,s,sl,rec,size);}   /* v7.188 C++ */
/* PS3 f_64f288 0x64f300..0x64f3fc (render frame) */
EXT static void emb_pass119(uintptr_t b,ProfileState *s){
 u3x_emb_pass(b,s);}   /* v7.188 C++ */
static uint8_t *emb_np119(uintptr_t b,uint32_t i){return u3x_emb_np(b,i);}   /* f_301a58, v7.188 C++ */
static int emb_valid119(const uint8_t *np){return u3x_emb_valid(np);}   /* f_368308(np, 0), v7.188 C++ */
static uint32_t emb_local119(uintptr_t b){return u3x_emb_local(b);}   /* v7.188 C++ */
static uint8_t *emb_prof119(uintptr_t b,uint32_t u){return u3x_emb_prof(b,u);}   /* f_363810, v7.188 C++ */
/* PS3 f_3687d0(np, k, rec): f_27cbe4 frees the old record, then the store (PS4 setter 0x78fa70, no caller left on PS4) */
EXT static void emb_setrec119(uintptr_t b,uint8_t *np,int32_t k,uint64_t r){
 u3x_emb_setrec(b,np,k,r);}   /* v7.188 C++ */
EXT static void emb_newrecs119(uintptr_t b,uint32_t off,uint32_t size){
 u3x_emb_newrecs(b,off,size);}   /* v7.188 C++ */
/* PS3 f_27d050 (player mode) */
EXT static void emb_d050_119(uintptr_t b,ProfileState *s){
 u3x_emblem_players(b,s);}   /* v7.179 C++ */
/* PS3 f_27d790 (editor re-render request) */
EXT static void emb_d790_119(uintptr_t b,ProfileState *s){
 u3x_emblem_editor(b,s);}   /* v7.179 C++ */
/* PS3 f_27daa8 (update) */
EXT static void emb_daa8_119(uintptr_t b,ProfileState *s){
 u3x_emblem_update(b,s);}   /* v7.179 C++ */
/* records no slot references any more: the frees PS3 does where PS4 only zeroes (see the header) */
EXT static void emb_sweep119(uintptr_t b){
 u3x_emblem_sweep(b);}   /* v7.179 C++ */
EXT static void emb_tick119(uintptr_t b,ProfileState *s){
 u3x_emblem_tick(b,s);}   /* v7.179 C++ */
/* NetInit: f_27d610 (default record 4x4, zeroed by f_27d5b8, + table binds) then the worker's f_27d5b8 (memset, pin 0, binds) */
EXT static int emb_netinit119(uintptr_t b,ProfileState *s){
 return u3x_emblem_netinit(b,s);}   /* v7.179 C++ */
/* PS3 f_302bd0(mgr, team, 1) as PS4 inlines it in 0x6a5c50 (f_3885c8 = the 0x17e2864 / 0x17e384c lookup, default 0x1b7f114) */
EXT static uint8_t *emb_teamnp119(uintptr_t b,int32_t team){
 return u3x_emblem_team_player(b,team);}   /* v7.160 C++ */
/* PS3 f_30a2d4(mgr, obj) (PS4 inline e.g. 0x35967d) */
EXT static uint8_t *emb_objnp119(uintptr_t b,uint8_t *obj){
 return u3x_emblem_object_player(b,obj);}   /* v7.160 C++ */
/* PS3 f_a17d68(spawner, sid): tag table 0x1b3efd8 (PS4: 16-byte buckets, entry sid +0x10, bit +0x14, spawner bits +0x20) */
EXT static int emb_tag119(uintptr_t b,uint8_t *sp,uint32_t sid){
 return u3x_emblem_tag(b,sp,sid);}   /* v7.160 C++ */
/* kind 124: value stub at 0x6a5c50 (NetCharacter look, PS3 f_2ba184); regs[4] = this, regs[5] = object */
EXT static int emb_look119(uintptr_t b,ProfileState *s,uint64_t *regs){
 (void)s;return u3x_emblem_look(b,regs);}   /* v7.160 C++ */
/* 1.19 f_217000 stat lines. Kind 249 0x61a3f1: ds/row; kind 250 0x61a638: draw. PS3 draws them whatever the Hat Stats setting is. */
/* v7.199 C++: the stat lines are u3x_hs_* in u3x_fixes.cpp (with their text buffer) */
EXT static void hs_save119(uintptr_t base,ProfileState *s,uint64_t *regs){u3x_hs_save(base,s,regs);}   /* x097: kind 249, 0x61a402 */
EXT static void hs_draw119(uintptr_t base,ProfileState *s,uint64_t *regs){u3x_hs_draw(base,s,regs);}   /* x102: kind 250, 0x61a638 */
/* ===== wall emblems as NdSimpleObject actors (prototype: air-multi). Paste into agc_port.inc right after emb_look119.
 * PS3 f_31444c (0x31444c): np = f_302bd0(mgr, f_313fa8(mgr), 1), else f_3025a0(mgr, 0) when mgr+0x5534 == 0 (PS4 offset of that
 * flag not located, so the fallback is unconditional here); instances named *-blackboard take np+0x660, the others np+0x664,
 * empty or no player = default mgr+0x64. PS4: +0x7e8 / +0x7f0, default EMB119+0x78.
 * PS4's background has no OverlaySampler, so each patch is a loading-menu-cloth actor. Its overlay table is made by the generic
 * draw setup 0x10b8550 (called from 0x10b7fb6, stored at obj+0x240, ri+0x158); nothing calls a look on these objects, hence this tick.
 * Walk as PS4 scans NdSimpleObjects at 0xcb929c..0xcb9340: bucket *(0x1b37610+0x100), next 0x11a4580(p, 0), +0x40 & 5 == 1,
 * +0x30 == 0x2db62a42. Spawner name: vt+0xe8 -> +0x28 (0x6bd727). Stateless per frame: a freed record is never left in a table
 * for more than one frame. ew119[0] ticks, [1] objects filled, [2] objects with no table, [3] objects with no record. Switch: EMB_OFF119. */
static uint32_t ew119[4] EXT_BSS;
EXT static void emb_wall119(uintptr_t b){
 u3x_wall_emblems(b);}   /* v7.146 C++ */
/* ===== end wall emblems ===== */
/* kinds 125 / 126: editor datasource entries 0x76a610 / 0x7699e0, event 9 -> f_27d790 first (then PS4's own handler) */
EXT static int emb_ed119(uintptr_t b,ProfileState *s,uint64_t *regs){
 if(!EMB_OFF119&&regs[4]&&*(int32_t*)regs[4]==9){emb_d790_119(b,s);X.ec[129]++;}
 return 0;}
/* kind 127: match setup 0x72bdf1 (r23 == 0, r18 != 0): f_27d790 (PS3 0x31fad4) */
EXT static void emb_ms119(uintptr_t b,ProfileState *s){if(!EMB_OFF119){emb_d790_119(b,s);X.ec[130]++;}}
/* ===== c012 (agent C): unlock condition type 6 "DLC pack installed" for the 1.19 packs (index 0x100..0x3ff) =====
 * PS4 evaluator 0x7bb5b0 (PS3 1.10 f_38b508, 1.19 f_39b238), type 6 (jump table 0x7bba48 entry 6 -> 0x7bb708):
 *  owned = byte (0x17c3930 + 0x110 + value) != 0, with no bound. That is the 1.10 layout (f_29c500: lbzx mgr+272+idx),
 *  a 0x100-byte array 0x17c3a40..0x17c3b3f; BoosterMgr 0x17c3b40 follows.
 * PS3 1.19 f_39b238 0x39b4e0: type 6 -> f_2a2adc(mgr 0x121b6e8-equiv, value) = bit (value & 63) of the u64 at
 *  mgr+0x110+(value>>6)*8. That is a 1024-bit array: the setter f_2a2b00 ignores value > 1023, and 0x2a2b70 clears 16 u64.
 *  The 1.19 unlock table (net.bin 0xe2e8998e) gates 194 items on packs 278..535.
 * Port: packs < 0x100 keep PS4's byte (the helper's pack marking, s010, writes those bytes). Packs 0x100..0x3ff are the
 *  1.19 array's range: installed, the user decision that every player owns all DLC in LAN, co-op and competitive
 *  (the same thing the s010 marking does for the packs below 0x100). Packs >= 0x400 are not installed: 1.19 can never
 *  set them.
 * Not changed: PS4's other readers of 0x17c3a40 (levels/playlists +0x2c, 0x75e45e, 0x7bf800, unlock entry +0x20 at
 *  0x7bb976 / 0x7bba93). Their data stays < 0x100: retail and dc3 write entry +0x20 only when the PS3 value is < 100.
 * Switch U3MP_DLC119=0 (ec[0] bit16): PS4's own read for every value. Diagnostics: ec[131] calls, ec[132] calls with
 *  value >= 0x100. ===== */
EXT static int dlc_cond119(uintptr_t b,const int32_t *cond){
 int32_t v=cond[2];X.ec[131]++;
 if(!(X.ec[0]&0x10000u)&&v>=0x100){X.ec[132]++;return v<0x400;}
 return *(const uint8_t*)(b+0x17c3a40u+(intptr_t)v)!=0;}   /* PS4 0x7bb708: movsxd; cmp byte [0x17c3930+rax+0x110],0 */
#pragma clang section rodata=""
