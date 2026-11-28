// u3fix_hook.h: what the u3fix fixes share: finding code by its bytes, stub pages within jump range of the image, jumps.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace u3fix {

constexpr uint64_t kNotFound = ~uint64_t{0};

// offset of the only place the bytes occur in the image; kNotFound when none or more than one
uint64_t find_unique(const unsigned char *image, uint64_t size, const unsigned char *bytes, uint64_t length);
// a read-write page within rel32 range of the image (nullptr when none is free)
unsigned char *stub_page_near(const unsigned char *image, uint64_t size);
// makes the stub page read-execute
bool finish_stub(unsigned char *stub);
// writes jmp rel32 to stub at image+at, the rest of the replaced length as nops (the image is still writable then)
bool jump_to(unsigned char *image, uint64_t at, unsigned length, const unsigned char *stub);

// the general malloc pool (0x15, Wrapped Malloc) raised to 3 MiB; its use printed every 30 s (net_pool.cpp)
bool raise_net_process_pool(unsigned char *image, uint64_t size);
// a null result at the four network allocation sites skips that message instead of crashing (net_alloc_guard.cpp)
constexpr unsigned kNetAllocSites = 4;   // state message, mask node, queue node, retarget node
bool install_net_alloc_guard(unsigned char *image, uint64_t size);
uint64_t net_alloc_skips(unsigned site);
// the package manager's loaded/unloaded messages printed into the runtime log (package_log.cpp)
bool install_package_log(unsigned char *image, uint64_t size);
// a job counter wait that lasts over 5 s is reported with its counter and callers (counter_wait_log.cpp)
bool install_counter_wait_log(unsigned char *image, uint64_t size);
// a parked job's wait that can never end is ended when its work is done (stuck_counter.cpp)
bool install_stuck_counter(unsigned char *image, uint64_t size);
// an async job counter on the start-of-frame list is given back once, by the list (async_counter_once.cpp)
bool install_async_counter_once(unsigned char *image, uint64_t size);
// the physics casts' cleanup does not run in the middle of a cast kick (cast_list_guard.cpp)
bool install_cast_list_guard(unsigned char *image, uint64_t size);
// asks for the counter to be settled when the wait can never end; true when asked
bool consider_stuck_wait(const unsigned char *counter, int64_t target, int mode, int64_t value, int64_t waited_ms,
                         const char *chain);
// sets the counter from seen to 0 and releases its waiting jobs; on a game thread that holds no job system lock
bool settle_counter_now(unsigned char *counter, int64_t seen, const char *chain);
// settles the counters asked for: called on a game thread that holds no job system lock
void settle_stuck_counters();
// a job the job system drops because its priority queue is full is reported (job_queue_full_log.cpp)
bool install_job_queue_full_log(unsigned char *image, uint64_t size);
// the sound driver runs its mixing job's work itself instead of waiting for a job (audio_mix_direct.cpp)
bool install_audio_mix_direct(unsigned char *image, uint64_t size);
// a graphics context takes a free command buffer when its thread's is still in use (cmdbuf_borrow.cpp)
bool install_cmdbuf_borrow(unsigned char *image, uint64_t size);
// the game's script and animation error messages printed into the runtime log (script_errors_log.cpp)
bool install_script_errors_log(unsigned char *image, uint64_t size);
// each skeleton-to-skeleton animation mapping the game looks up is printed once, found or missing (retarget_log.cpp)
bool install_retarget_log(unsigned char *image, uint64_t size);
// skeleton retargets switch to their multiplayer settings while a net game runs, as on PS3 (retarget_mode.cpp)
bool install_retarget_mode(unsigned char *image, uint64_t size);
// every job counter's recent history and the jobs counted on it; reports the moment a count goes wrong (job_trace.cpp)
bool install_job_trace(unsigned char *image, uint64_t size);
void counter_story(const void *counter, char *out, size_t size);
// jobs submitted on the counter that have not finished (-1: not a pool counter or no trace)
int64_t pending_jobs(const void *counter);
// the counter's counted jobs are not coming: counter_keep.cpp gives it back after its wait cap
void forget_pending_jobs(const void *counter);
// a job counter given back while jobs counted on it have not finished stays out of the pool until they have, at most
// 2 s (counter_keep.cpp)
bool install_counter_keep(unsigned char *image, uint64_t size);
// what went wrong in the counter's current life (CounterTrouble bits); life: how many times it was made
enum CounterTrouble : uint32_t {
    kCounterReused = 1,      // made while jobs counted on its last life were unfinished
    kCounterBelowZero = 2,   // a job took it from 0 to -1
    kCounterFreedBusy = 4,   // given back while jobs counted on it were unfinished
    kCounterSettled = 8,     // the runtime settled it
};
uint32_t counter_trouble(const void *counter, uint32_t *life);
void note_settled(const void *counter, int64_t from);
// the job manager (nullptr before the trace is installed) and its counter update (counter, new value): takes the job
// lock and releases the jobs waiting for that value
const unsigned char *job_manager();
using CounterUpdate = void(__attribute__((sysv_abi)) *)(unsigned char *, int64_t);
CounterUpdate counter_update_function();
// the jobs waiting in each of the 8 priority queues, as text (job_queue_full_log.cpp)
void job_queue_counts(char *out, size_t size);
// an NPC copy on a co-op guest starts its base animation at a finite phase (npc_phase.cpp)
bool install_npc_phase_guard(unsigned char *image, uint64_t size);
// a character's part list may hold an empty entry; showing or hiding its parts skips it (part_flag_guard.cpp)
bool install_part_flag_guard(unsigned char *image, uint64_t size);
bool install_improved_tonemap_curve(unsigned char *image, uint64_t size);
// the auto exposure averages at least one frame: luminance filter size 0 as PS3 (exposure_filter.cpp)
bool exposure_filter_at_least_one(unsigned char *image, uint64_t size);
// names and point popups above players use the HUD's 1280x720 canvas, as on PS3 (hudnet_text_canvas.cpp, issue 64)
bool install_hudnet_text_canvas(unsigned char *image, uint64_t size);
// in splitscreen the second local player's pad feeds the game's pad port 1 (split_pad.cpp, issue 31)
bool install_split_pad(unsigned char *image, uint64_t size);
// diagnostic: each step of a weapon pickup in a net game printed into the log (pickup_log.cpp)
bool install_pickup_log(unsigned char *image, uint64_t size);
// a party member takes the host's match settings net game by name, not by the host's index (match_settings_netgame.cpp)
bool install_match_settings_netgame(unsigned char *image, uint64_t size);
// one frame of the game's pad update (image 0x10ddd90, given as update): twice, second player first, while the game
// has a second local player, else once
using SplitPadUpdate = uint32_t(__attribute__((sysv_abi)) *)(unsigned char *ports);
uint32_t split_pad_frame(unsigned char *image, unsigned char *ports, SplitPadUpdate update);
// a word on its own line in u3-switches.txt in the --user folder (tonemap_curve.cpp)
bool user_switch(const char *word);
// the PS3 improved tonemap curve: rewrites the tonemap constants when the render settings ask for it (tonemap_curve.cpp)
bool improved_tonemap_curve(float *constants, const unsigned char *settings, bool t1_curve, bool display = true);
// the default choice: the improved curve where the render settings' exposure maximum is at least 2 (not Borneo co-op)
bool improved_curve_by_default(const unsigned char *settings);
// the PS3 colour grading in net games: the LUT update samples a table baked from the render settings (lut_grading.cpp)
bool install_ps3_lut_grading(unsigned char *image, uint64_t size);
// true when the render settings ask for PS3 grading (lut-enable above one half, no PS4 table named; as on PS3 also when
// all values are zero); lut gets lut-enable and the 35 parameters in their order (+0x740, then +0x760..+0x7e8)
bool ps3_grading_inputs(const unsigned char *settings, float lut[36]);
// the PS3 colour table of those 36 values: 32x32x32 RGBA8, index r + 32 g + 1024 b (the caller's rounding mode is kept)
void bake_ps3_grading_lut(const float lut[36], unsigned char *rgba);

}  // namespace u3fix
