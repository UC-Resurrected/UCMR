// u2mp: the pure rules of the in-match overlay (scoreboard / result entries, the team lists' voice column, the Mute
// Players lists, dead-player hint line) and of the PAGE_MULTIPLAYER stats ticker. No game memory is touched here
// except the ticker object bytes passed in, so the rules can be tested without the game
// (tests/test_u2mp_overlay.cpp). Used by scoreboard.cpp, remaining.cpp and ticker.cpp.
//
// Reimplemented from the behaviour descriptions u2mp/docs/spec/parity-m-features.md (M1, M2, section 10),
// u2mp/docs/spec/parity-j-voice-medals.md (J4), u2mp/docs/spec/parity-n-remaining.md (N2) and
// u2mp/docs/spec/parity-ticker-respawn.md (1.2 - 1.6).
#pragma once

#include <cstdint>
#include <string>

namespace u2mp::overlay {

// ---- scoreboard / result entries (parity-m-features.md M2) ----

constexpr int kEntries = 4;   // 0 scoreboard, 1 round results, 2 results, 3 round results
constexpr float kSlide = 20.0f;
// Every overlay rate r is applied as r x T / 75, T = the game's frame step in 1/300 s ticks (section 10.1):
// r x 4 per second.
constexpr float kPerTick = 1.0f / 75.0f;

// Rule 1: the entry's fade moves toward its shown flag by speed x ticks / 75, clamped to 0..1.
float fade_step(float fade, bool shown, float speed, float ticks);
// Rule 2: the highest entry whose fade is above 0 is the one drawn; -1 when none is.
int top_entry(const float (&fade)[kEntries]);
// Rule 3: fading in, ease = 1 - (1 - fade)^2 and the page sits 20 x (1 - ease) above its place (y < 0);
// fading out, ease = fade^2 and it sits 20 x (1 - ease) below (y > 0). The dim value of rule 4 is this ease.
float slide_ease(float fade, bool shown);
float slide_y(float fade, bool shown);

// Rules 6 - 8: the four entries' shown flags and entry 0's speed, kept from frame to frame.
struct Entries {
    bool shown[kEntries] = {};
    float speed[kEntries] = {1.0f, 1.0f, 1.0f, 1.0f};
    bool by_button = false;    // entry 0 was opened by the button (the "mark")
    bool auto_last = false;    // the automatic flag of the last state-2 frame
    int32_t prev_state = 0;    // the NGM state of the last frame (the state-5 rule)
};
// One frame's inputs.
struct Frame {
    int32_t state = 0;         // NGM match state (ngm +0x1e80)
    int32_t substate = 0;      // round sub-state (ngm +0x1e88)
    int32_t end_reason = 0;    // end reason (ngm +0x1e84)
    bool cinema = false;       // game type 11
    bool auto_flag = false;    // 0x9766f0() (or the film controller's answer in a film)
    bool notice = false;       // the HUD notification queue shows a message (0x555550)
    bool out = false;          // every player is out (section 10.6)
    bool pressed = false;      // the scoreboard button went down this frame
    bool blocked = false;      // a rule 7 block holds (button_blocked): the press is ignored
    bool leaving = false;      // the current task process is in its state "Leave" (9f514949, PS4 0x9be070)
    bool film_clear = false;   // the current task process answered e68ae001 with true (film camera mode 1)
};
enum class Sound { None, Open, Close };
struct FrameResult {
    bool relayout[kEntries] = {};   // the entry's shown flag went from 0 to 1 this frame: page +0xf8 = 2
    Sound sound = Sound::None;      // rule 5 (button toggle only)
    bool toggled = false;           // rule 7 ran
};
// Rule 8 (with 6 and 7 for state 2), in PS3 order; e.prev_state becomes f.state.
FrameResult update_entries(Entries &e, const Frame &f);

// M1 3 / rule 2: the overlay alpha A before the entry's own fade.
bool coop_type(int32_t game_type);   // 4, 8, 9, 12
float overlay_alpha(int32_t state, int32_t game_type, int32_t end_reason, float hud_alpha);

// Section 10.6, one tracker: in use (+0x20), died / spawn times (+0x17c / +0x180), state-record flags A / B (false
// when the character does not resolve) and their times (+0x178 / +0x184).
struct TrackerTimes {
    bool in_use = false;
    int32_t died = 0, spawn = 0, a_time = 0, b_time = 0;
    bool flag_a = false, flag_b = false;
};
// The tracker is out of play at `clock` (threshold 0); a tracker not in use counts as out.
bool tracker_out(const TrackerTimes &t, int32_t clock);
// Every tracker is out (true for no tracker in use).
bool every_player_out(const TrackerTimes *trackers, int count, int32_t clock);

// Section 10.11: an argument / reply record of the engine's event query 0x4c4310: 16 value bytes (a bool is the
// byte at +0, an s32 the u32 at +0), the type at +0x10 (0 = empty / no answer, 1 = bool, 2 = s32, 10 = pointer).
struct QueryRecord {
    unsigned char value[16];
    uint32_t type;
    uint32_t pad;
};
static_assert(sizeof(QueryRecord) == 0x18, "query record size");
constexpr uint32_t kWatchedPlayer = 0x8a87d1da;   // s32: the tracker index the film camera watches
constexpr uint32_t kFollowing = 0xf498c2b5;       // bool: the film camera follows a player
constexpr uint32_t kFilmCameraMode1 = 0xe68ae001; // bool: the film camera mode is 1
// The record was answered (type not 0); a bool answer is answered and its byte +0 is not 0.
bool answered(const QueryRecord &r);
bool answer_true(const QueryRecord &r);
// 10.8, M2 rule 6 in a film: the watched player (an answered index below `count`) died after its last spawn (10.6
// condition 1 without the clock part), and the camera follows a player (answered true).
bool film_automatic(const QueryRecord &watched, const QueryRecord &following, const TrackerTimes *trackers, int count);

// 10.10, M2 rule 7: the button toggle is blocked while L3 is held (PS4 bit 0x2 of the held word, PS3 held bit 0x4000)
// or the engine pause object's owner pointer is set.
constexpr uint32_t kL3 = 0x2;
bool button_blocked(uint32_t held, uint64_t pause_owner);

// ---- team lists' voice column (parity-j-voice-medals.md J4) and the Mute Players lists (parity-n-remaining.md N2) --

constexpr uint32_t kVoiceColumn = 0x4c217b;         // team list column id of the voice column
constexpr uint32_t kMutedIcon = 0x7a5f5bf1;         // general/net/mic/mute
constexpr uint32_t kPreviousGameList = 0x2eb65ce8;  // a team list of this page names no tracker
constexpr int32_t kPlayerRow = 2;                   // team list row kind of a player row
constexpr uint32_t kTrackers = 10;
// PAGE_MULTIPLAYER_MUTE_PLAYERS (pause menu, Mute Players): the ids of its two team lists, sent with the selected row.
constexpr uint32_t kMutePlayersOwnList = 0xb04c828b;    // LIST_MUTEPLAYERS0: the own team
constexpr uint32_t kMutePlayersOtherList = 0xb48d9f3c;  // LIST_MUTEPLAYERS1: the other team

// J4: the cell shows the muted icon (in place of the PS4 talking test): a player row of a list that names a tracker
// (index 0..9, not the previous-game page), the voice column, and that tracker's player is muted. Every team list of
// the vtable 0x100a280 uses it: the scoreboard's and the Mute Players page's.
bool voice_cell_muted(int32_t row_kind, uint32_t list_page, uint32_t tracker_index, uint32_t column_id, bool muted);

// N2: the team a selected row's list shows: the own team for kMutePlayersOwnList, the other one (1 - own) for
// kMutePlayersOtherList; `known` false for any other list.
int32_t mute_action_team(uint32_t list, int32_t own_team, bool &known);
// N2: the NetStatGame copy the row is looked up in: the current game's while the NGM state is 1 or 2, else the
// previous game's.
bool mute_action_previous_game(int32_t ngm_state);
// N2: one NetStatGame player entry as the search reads it: its team byte and its 1-based position in its team list.
struct TeamEntry {
    uint8_t team;
    uint16_t position;
};
// N2: the first of `count` entries on `team` whose position is `row` + 1, -1 when there is none.
int team_row_entry(const TeamEntry *entries, uint32_t count, int32_t team, int32_t row);
// N2: the entry's player gets the mute toggle: some tracker of the `count` (net id at tracker +0x120) is that
// player, and it is not the local player.
bool mute_list_target(uint32_t net_id, uint32_t local_id, const uint32_t *tracker_ids, uint32_t count);

// ---- dead-player hint line (M1) ----

constexpr float kHintIn = 1.0f;    // x ticks / 75 (4 per second), while every condition of M1 2 holds
constexpr float kHintOut = 2.0f;   // x ticks / 75 (8 per second), otherwise
float hint_step(float h, bool on, float ticks);
// M1 2: no menu up, the local death camera runs, NGM state 2, not every player out.
bool hint_wanted(bool menu_up, bool death_camera, int32_t state, bool out);

// ---- PAGE_MULTIPLAYER stats ticker object (parity-ticker-respawn.md 1.2), offsets from the object ----

constexpr uint32_t kTickerWidth = 0x04;      // f32 strip width
constexpr uint32_t kTickerDrawn = 0x10;      // f32 time of the last scroll-and-draw, seconds
constexpr uint32_t kTickerEntry = 0x14;      // entry k at kTickerEntry + k x kTickerStride: f32 start, f32 width, text
constexpr uint32_t kTickerStride = 0x208;
constexpr uint32_t kTickerTextOff = 0x08;    // the text within an entry (first byte 0 = entry free)
constexpr uint32_t kTickerTextMax = 0x200;   // bytes including the NUL
constexpr int kTickerSlots = 8;
constexpr uint32_t kTickerSize = kTickerEntry + kTickerSlots * kTickerStride;
static_assert(kTickerSize == 0x1054, "ticker object size: the last entry's text ends at 0x1054");
constexpr float kTickerEdgeGap = 10.0f;      // a new text enters this far right of the strip's right edge
constexpr float kTickerQueueGap = 150.0f;    // or this far behind the last text still coming in
constexpr double kTickerVisibleSeconds = 0.1;

// 1.4: where the next added text starts (relative to the strip x).
float ticker_next_start(const unsigned char *ticker);
// 1.4: the entry the next text goes into (the lowest free one), -1 when all 8 are in use.
int ticker_free_entry(const unsigned char *ticker);
// 1.5: every queued text plus its 150-unit gap has fully come in from the right edge (true when empty).
bool ticker_finished(const unsigned char *ticker);
// 1.6: the strip was drawn within the last 0.1 s; `frame_clock` is the s32 frame clock (300 per second).
bool ticker_visible(const unsigned char *ticker, int32_t frame_clock);

// The colour codes of a ticker text ("|@CAARRGGBB|": alpha, red, green, blue, as PS3 draws them) for the PS4 2D
// command arena, which draws a code's red and blue swapped (PS3's gold F0C828 came out light blue). Every "|@C"
// followed by eight hex digits and "|" gets AABBGGRR; everything else is copied unchanged.
std::string ticker_arena_colours(const std::string &text);

}  // namespace u2mp::overlay
