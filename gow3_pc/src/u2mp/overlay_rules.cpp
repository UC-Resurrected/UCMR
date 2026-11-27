// u2mp: the pure overlay and ticker rules (overlay_rules.h).
//
// Reimplemented from the behaviour descriptions u2mp/docs/spec/parity-m-features.md (M1, M2, section 10),
// u2mp/docs/spec/parity-j-voice-medals.md (J4), u2mp/docs/spec/parity-n-remaining.md (N2) and
// u2mp/docs/spec/parity-ticker-respawn.md (1.2 - 1.6).
#include "overlay_rules.h"

#include <bit>
#include <cmath>
#include <cstring>
#include <utility>

namespace u2mp::overlay {
static_assert(std::bit_cast<uint32_t>(kPerTick) == 0x3c5a740eu, "1/75 as a float (the PS3 overlay's multiplier)");

namespace {

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

float f32_at(const unsigned char *p, uint32_t off) {
    float v;
    std::memcpy(&v, p + off, sizeof v);
    return v;
}
uint32_t entry_at(int k) { return kTickerEntry + uint32_t(k) * kTickerStride; }
bool in_use(const unsigned char *ticker, int k) { return ticker[entry_at(k) + kTickerTextOff] != 0; }
float start_of(const unsigned char *ticker, int k) { return f32_at(ticker, entry_at(k)); }
float width_of(const unsigned char *ticker, int k) { return f32_at(ticker, entry_at(k) + 4); }

}  // namespace

float fade_step(float fade, bool shown, float speed, float ticks) {
    const float step = speed * (ticks * kPerTick);
    return clamp01(shown ? fade + step : fade - step);
}

int top_entry(const float (&fade)[kEntries]) {
    for (int k = kEntries - 1; k >= 0; --k)
        if (fade[k] > 0.0f) return k;
    return -1;
}

float slide_ease(float fade, bool shown) {
    if (shown) return 1.0f - (1.0f - fade) * (1.0f - fade);
    return fade * fade;
}

float slide_y(float fade, bool shown) {
    const float rest = kSlide * (1.0f - slide_ease(fade, shown));
    return shown ? -rest : rest;
}

FrameResult update_entries(Entries &e, const Frame &f) {
    FrameResult r;
    const auto set = [&](int k, bool on) {
        if (on && !e.shown[k]) r.relayout[k] = true;
        e.shown[k] = on;
    };
    const auto clear_all = [&] {
        for (int k = 0; k < kEntries; ++k) set(k, false);
    };
    const auto hide_results = [&] {
        set(2, false);
        set(3, false);
    };

    if (f.cinema) {
        clear_all();
        if (f.state == 3) set(2, true);
        else if (f.state == 4) set(3, true);
        else hide_results();
    } else if (f.state == 2) {
        // Rule 6: the automatic flag, the notification override, every player out, the falling edge.
        const bool now = f.auto_flag && !(f.notice && !e.by_button);
        const bool falling = e.auto_last && !now;
        if (f.out || (!e.auto_last && now)) {
            set(0, true);
            e.speed[0] = 1.0f;
        }
        if (falling) {
            set(0, false);
            e.speed[0] = 1.0f;
            e.by_button = false;
        }
        e.auto_last = now;
        if (e.prev_state == 5) {
            set(3, false);
            set(0, false);
            e.by_button = false;
        }
        hide_results();
        // Rule 7: the button toggles entry 0 with its sound; never while every player is out or a block holds.
        if (!f.out && f.pressed && !f.blocked) {
            const bool on = !e.shown[0];
            e.speed[0] = 2.0f;
            set(0, on);
            e.by_button = on;
            r.sound = on ? Sound::Open : Sound::Close;
            r.toggled = true;
        }
    } else if (f.state == 5) {
        // Sub-state 3 sets entry 3 and the next step clears it again: entry 0 keeps its flag.
        if (f.substate == 3) set(3, true);
        else set(0, true);
        hide_results();
    } else {
        // A match that ended with reason 3 or 8 keeps the scoreboard while its task is leaving.
        set(0, f.leaving && (f.end_reason == 3 || f.end_reason == 8));
        if (f.state == 3) {
            clear_all();
            set(2, true);
        } else if (f.state == 4) {
            clear_all();
            set(3, true);
        } else {
            hide_results();
        }
    }
    if (f.film_clear) clear_all();
    e.prev_state = f.state;
    return r;
}

bool coop_type(int32_t game_type) { return game_type == 4 || game_type == 8 || game_type == 9 || game_type == 12; }

float overlay_alpha(int32_t state, int32_t game_type, int32_t end_reason, float hud_alpha) {
    if (state == 3 || state == 4 || state == 5 || coop_type(game_type) || end_reason == 3 || end_reason == 8)
        return 1.0f;
    return hud_alpha;
}

bool tracker_out(const TrackerTimes &t, int32_t clock) {
    if (!t.in_use) return true;
    // Clock differences wrap as the game's 32-bit subtraction does.
    const auto since = [clock](int32_t at) { return int32_t(uint32_t(clock) - uint32_t(at)) >= 0; };
    return (t.spawn < t.died && since(t.died)) || (t.flag_a && since(t.a_time)) || (t.flag_b && since(t.b_time));
}

bool every_player_out(const TrackerTimes *trackers, int count, int32_t clock) {
    for (int i = 0; i < count; ++i)
        if (!tracker_out(trackers[i], clock)) return false;
    return true;
}

bool answered(const QueryRecord &r) { return r.type != 0; }

bool answer_true(const QueryRecord &r) { return answered(r) && r.value[0] != 0; }

bool film_automatic(const QueryRecord &watched, const QueryRecord &following, const TrackerTimes *trackers, int count) {
    bool on = false;
    if (answered(watched)) {
        uint32_t index;
        std::memcpy(&index, watched.value, sizeof index);
        on = count > 0 && index < uint32_t(count) && trackers[index].spawn < trackers[index].died;
    }
    return on && answer_true(following);
}

bool button_blocked(uint32_t held, uint64_t pause_owner) { return (held & kL3) != 0 || pause_owner != 0; }

bool voice_cell_muted(int32_t row_kind, uint32_t list_page, uint32_t tracker_index, uint32_t column_id, bool muted) {
    return row_kind == kPlayerRow && list_page != kPreviousGameList && tracker_index < kTrackers &&
           column_id == kVoiceColumn && muted;
}

int32_t mute_action_team(uint32_t list, int32_t own_team, bool &known) {
    known = list == kMutePlayersOwnList || list == kMutePlayersOtherList;
    return list == kMutePlayersOwnList ? own_team : 1 - own_team;
}

bool mute_action_previous_game(int32_t ngm_state) { return ngm_state != 1 && ngm_state != 2; }

int team_row_entry(const TeamEntry *entries, uint32_t count, int32_t team, int32_t row) {
    for (uint32_t i = 0; i < count; ++i)
        if (int32_t(entries[i].team) == team && int64_t(entries[i].position) == int64_t(row) + 1) return int(i);
    return -1;
}

bool mute_list_target(uint32_t net_id, uint32_t local_id, const uint32_t *tracker_ids, uint32_t count) {
    if (net_id == local_id) return false;
    for (uint32_t i = 0; i < count; ++i)
        if (tracker_ids[i] == net_id) return true;
    return false;
}

float hint_step(float h, bool on, float ticks) {
    const float step = ticks * kPerTick;
    return clamp01(on ? h + kHintIn * step : h - kHintOut * step);
}

bool hint_wanted(bool menu_up, bool death_camera, int32_t state, bool out) {
    return !menu_up && death_camera && state == 2 && !out;
}

float ticker_next_start(const unsigned char *ticker) {
    float start = f32_at(ticker, kTickerWidth) + kTickerEdgeGap;
    for (int k = 0; k < kTickerSlots; ++k) {
        if (!in_use(ticker, k)) continue;
        const float behind = start_of(ticker, k) + width_of(ticker, k) + kTickerQueueGap;
        if (behind > start) start = behind;
    }
    return start;
}

int ticker_free_entry(const unsigned char *ticker) {
    for (int k = 0; k < kTickerSlots; ++k)
        if (!in_use(ticker, k)) return k;
    return -1;
}

bool ticker_finished(const unsigned char *ticker) {
    const float width = f32_at(ticker, kTickerWidth);
    for (int k = 0; k < kTickerSlots; ++k)
        if (in_use(ticker, k) && start_of(ticker, k) + kTickerQueueGap + width_of(ticker, k) > width) return false;
    return true;
}

bool ticker_visible(const unsigned char *ticker, int32_t frame_clock) {
    const float now = float(frame_clock) * (1.0f / 300.0f);
    return std::fabs(double(now) - double(f32_at(ticker, kTickerDrawn))) < kTickerVisibleSeconds;
}

std::string ticker_arena_colours(const std::string &text) {
    auto hex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); };
    std::string out = text;
    for (size_t i = 0; i + 12 <= out.size(); ++i) {
        if (out.compare(i, 3, "|@C") != 0 || out[i + 11] != '|') continue;
        bool digits = true;
        for (size_t k = i + 3; k < i + 11; ++k) digits = digits && hex(out[k]);
        if (!digits) continue;
        std::swap(out[i + 5], out[i + 9]);   // AA RR GG BB -> AA BB GG RR
        std::swap(out[i + 6], out[i + 10]);
        i += 11;
    }
    return out;
}

}  // namespace u2mp::overlay
