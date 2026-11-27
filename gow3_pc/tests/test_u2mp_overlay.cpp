// u2mp overlay rules test (no game needed): ninja -C out/gpu u2mp-overlay-test
// The scoreboard / result entry fade, priority and slide rules, the match-state entries, the automatic flag edges
// the button blocks, the "Leave" entry-0 rule, the film query records and film automatic flag (section 10.8 / 10.10 /
// 10.11) and the hint fade (spec parity-m-features.md M1 / M2), and the ticker placement / finished / visible rules on a
// stand-in ticker object (spec parity-ticker-respawn.md 1.2 - 1.6), the team lists' voice column rule (spec
// parity-j-voice-medals.md J4) and the Mute Players list rules (spec parity-n-remaining.md N2).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "../src/u2mp/overlay_rules.h"

using namespace u2mp::overlay;

static int failures = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            ++failures;                                                  \
        }                                                                \
    } while (0)

static bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

static void test_fade() {
    // speed x ticks / 75: speed 1.0 = 4 per second (a full fade in 0.25 s = 75 ticks), speed 2.0 = 8 per second.
    // 60 fps frames are 5 ticks.
    float f = 0.0f;
    for (int i = 0; i < 6; ++i) f = fade_step(f, true, 1.0f, 5.0f);   // 0.1 s
    CHECK(near(f, 0.4f));
    for (int i = 0; i < 9; ++i) f = fade_step(f, true, 1.0f, 5.0f);   // 0.25 s in all
    CHECK(near(f, 1.0f));
    f = fade_step(f, true, 1.0f, 5.0f);
    CHECK(f == 1.0f);
    for (int i = 0; i < 3; ++i) f = fade_step(f, false, 2.0f, 5.0f);  // 15 ticks at 8 per second
    CHECK(near(f, 0.6f));
    for (int i = 0; i < 5; ++i) f = fade_step(f, false, 2.0f, 5.0f);
    CHECK(f == 0.0f);
    CHECK(fade_step(0.0f, true, 1.0f, 0.0f) == 0.0f);   // no frame step (paused): nothing moves
    CHECK(near(fade_step(0.0f, true, 1.0f, 75.0f), 1.0f));
    CHECK(near(kPerTick * 300.0f, 4.0f));
}

static void test_priority() {
    float f[kEntries] = {0.0f, 0.0f, 0.0f, 0.0f};
    CHECK(top_entry(f) == -1);
    f[0] = 1.0f;
    CHECK(top_entry(f) == 0);
    f[1] = 0.01f;
    CHECK(top_entry(f) == 1);
    f[3] = 0.2f;
    CHECK(top_entry(f) == 3);
    f[3] = 0.0f;
    f[2] = 0.5f;
    CHECK(top_entry(f) == 2);
}

static void test_slide() {
    // Fading in: starts 20 above (y -20), ends in place; fading out: in place, then on downward to +20.
    CHECK(near(slide_y(0.0f, true), -20.0f));
    CHECK(near(slide_y(1.0f, true), 0.0f));
    CHECK(near(slide_y(0.5f, true), -20.0f * 0.25f));   // ease 0.75
    CHECK(near(slide_y(1.0f, false), 0.0f));
    CHECK(near(slide_y(0.0f, false), 20.0f));
    CHECK(near(slide_y(0.5f, false), 20.0f * 0.75f));   // ease 0.25
    CHECK(near(slide_ease(0.5f, true), 0.75f));
    CHECK(near(slide_ease(0.5f, false), 0.25f));
}

static bool shown_are(const Entries &e, bool a, bool b, bool c, bool d) {
    return e.shown[0] == a && e.shown[1] == b && e.shown[2] == c && e.shown[3] == d;
}

static Frame playing(bool auto_flag = false, bool pressed = false) {
    Frame f;
    f.state = 2;
    f.auto_flag = auto_flag;
    f.pressed = pressed;
    return f;
}

static void test_auto_open() {
    // Rule 6: rising edge shows entry 0 at speed 1.0 without sound, falling edge hides it.
    Entries e;
    FrameResult r = update_entries(e, playing(true));
    CHECK(shown_are(e, true, false, false, false) && e.speed[0] == 1.0f && r.sound == Sound::None && r.relayout[0]);
    r = update_entries(e, playing(true));
    CHECK(e.shown[0] && !r.relayout[0]);   // no edge: nothing new
    update_entries(e, playing(false));
    CHECK(!e.shown[0] && !e.by_button);

    // The falling edge also hides a board the player opened by button (open question 10 settled).
    Entries b;
    r = update_entries(b, playing(false, true));
    CHECK(b.shown[0] && b.by_button && b.speed[0] == 2.0f && r.sound == Sound::Open && r.toggled);
    update_entries(b, playing(true));
    CHECK(b.shown[0] && b.by_button && b.speed[0] == 1.0f);   // the rising edge shows it again at speed 1.0
    update_entries(b, playing(false));
    CHECK(!b.shown[0] && !b.by_button && b.speed[0] == 1.0f);
}

static void test_auto_notice() {
    // A HUD notification forces the flag off unless the board was opened by button.
    Entries e;
    Frame f = playing(true);
    f.notice = true;
    update_entries(e, f);
    CHECK(!e.shown[0] && !e.auto_last);
    f.notice = false;
    update_entries(e, f);   // the message is gone: now the rising edge
    CHECK(e.shown[0] && e.auto_last);

    Entries b;
    update_entries(b, playing(false, true));   // opened by button
    f = playing(true);
    f.notice = true;
    update_entries(b, f);
    CHECK(b.auto_last && b.shown[0]);
}

static void test_every_player_out_rule() {
    // Out: entry 0 shown every frame at speed 1.0, the button does nothing.
    Entries e;
    Frame f = playing(false, true);
    f.out = true;
    FrameResult r = update_entries(e, f);
    CHECK(e.shown[0] && e.speed[0] == 1.0f && r.sound == Sound::None && !r.toggled);
    // Out with a falling edge: the hide still runs in the same frame (one frame off).
    Entries g;
    update_entries(g, playing(true));
    f = playing(false);
    f.out = true;
    r = update_entries(g, f);
    CHECK(!g.shown[0] && !g.by_button);
    r = update_entries(g, f);
    CHECK(g.shown[0] && r.relayout[0]);
}

static void test_button() {
    Entries e;
    FrameResult r = update_entries(e, playing(false, true));
    CHECK(e.shown[0] && e.speed[0] == 2.0f && r.sound == Sound::Open && e.by_button);
    r = update_entries(e, playing(false, true));
    CHECK(!e.shown[0] && e.speed[0] == 2.0f && r.sound == Sound::Close && !e.by_button);
    // The button only works in state 2.
    Frame f;
    f.state = 3;
    f.pressed = true;
    r = update_entries(e, f);
    CHECK(r.sound == Sound::None && !e.shown[0]);

    // Rule 7 blocks (test 11): a blocked press does nothing (no toggle, no sound, mark and speed unchanged).
    Entries b;
    b.speed[0] = 1.0f;
    f = playing(false, true);
    f.blocked = true;
    r = update_entries(b, f);
    CHECK(!b.shown[0] && !b.by_button && b.speed[0] == 1.0f && r.sound == Sound::None && !r.toggled);
    update_entries(b, playing(false, true));   // released, pressed again: opens
    CHECK(b.shown[0] && b.by_button);
    f = playing(false, true);
    f.blocked = true;
    r = update_entries(b, f);
    CHECK(b.shown[0] && b.by_button && r.sound == Sound::None);   // cannot close it either

    CHECK(!button_blocked(0, 0));
    CHECK(button_blocked(kL3, 0));                       // L3 held (PS4 bit 0x2)
    CHECK(button_blocked(0x100002, 0));                  // touchpad with L3
    CHECK(!button_blocked(0x100000 | 0x4 | 0x1, 0));     // touchpad, R3 and other bits: no block
    CHECK(button_blocked(0, 0x12345678ull));             // pause owner pointer set
}

static QueryRecord reply(uint32_t type, uint32_t value) {
    QueryRecord q{};
    std::memcpy(q.value, &value, sizeof value);
    q.type = type;
    return q;
}

static void test_film_queries() {
    // Section 10.11 records: answered = type not 0; a bool is byte +0.
    const QueryRecord none{};
    CHECK(!answered(none) && !answer_true(none));
    CHECK(answered(reply(1, 0)) && !answer_true(reply(1, 0)));
    CHECK(answer_true(reply(1, 1)));
    CHECK(!answer_true(reply(0, 1)));   // a value without a type is no answer
    CHECK(answer_true(reply(1, 0x100)) == false);   // only the byte at +0 counts

    // 10.8 film automatic flag: watched tracker died after its last spawn, and following a player.
    TrackerTimes t[10];
    t[3].spawn = 100;
    t[3].died = 200;    // dead
    t[4].spawn = 300;
    t[4].died = 200;    // alive again
    t[5].spawn = 200;
    t[5].died = 200;    // died == spawn: not after
    CHECK(film_automatic(reply(2, 3), reply(1, 1), t, 10));
    CHECK(!film_automatic(reply(2, 3), reply(1, 0), t, 10));   // not following
    CHECK(!film_automatic(reply(2, 3), none, t, 10));          // following not answered
    CHECK(!film_automatic(none, reply(1, 1), t, 10));          // watched not answered (outside films)
    CHECK(!film_automatic(reply(2, 4), reply(1, 1), t, 10));
    CHECK(!film_automatic(reply(2, 5), reply(1, 1), t, 10));
    CHECK(!film_automatic(reply(2, 10), reply(1, 1), t, 10));           // index not below 10
    CHECK(!film_automatic(reply(2, 0xffffffffu), reply(1, 1), t, 10));  // negative index: not used
    t[3].in_use = false;   // "in use" plays no part here (10.6 condition 1 only)
    CHECK(film_automatic(reply(2, 3), reply(1, 1), t, 10));
    CHECK(kWatchedPlayer == 0x8a87d1da && kFollowing == 0xf498c2b5 && kFilmCameraMode1 == 0xe68ae001);

    // Test 12: following the watched player, its death opens the board, its respawn closes it; e68ae001 true closes
    // the board and the result pages at once.
    Entries e;
    Frame f = playing(film_automatic(reply(2, 3), reply(1, 1), t, 10));
    update_entries(e, f);
    CHECK(shown_are(e, true, false, false, false) && e.speed[0] == 1.0f);
    t[3].spawn = 300;
    f = playing(film_automatic(reply(2, 3), reply(1, 1), t, 10));
    update_entries(e, f);
    CHECK(shown_are(e, false, false, false, false));
    t[3].spawn = 100;
    f = playing(film_automatic(reply(2, 3), reply(1, 1), t, 10));
    f.film_clear = true;
    FrameResult r = update_entries(e, f);
    CHECK(shown_are(e, false, false, false, false) && r.relayout[0]);   // set by rule 6, then cleared
    f.film_clear = false;
    update_entries(e, f);
    CHECK(shown_are(e, false, false, false, false));   // no new rising edge: stays closed
    Frame s;
    s.state = 4;
    s.film_clear = true;
    update_entries(e, s);
    CHECK(shown_are(e, false, false, false, false));   // ROUND_RESULTS cleared too
}

static void test_states() {
    // States 3 / 4: everything cleared and RESULTS / ROUND_RESULTS shown, asked to lay out again every frame.
    Entries e;
    update_entries(e, playing(false, true));
    Frame f;
    f.state = 3;
    FrameResult r = update_entries(e, f);
    CHECK(shown_are(e, false, false, true, false) && r.relayout[2]);
    r = update_entries(e, f);
    CHECK(shown_are(e, false, false, true, false) && r.relayout[2]);
    f.state = 4;
    r = update_entries(e, f);
    CHECK(shown_are(e, false, false, false, true) && r.relayout[3]);
    // End reason 3 / 8 shows entry 0 in these states only while the task process is in its state "Leave" (test 13).
    f.state = 0;
    f.end_reason = 3;
    update_entries(e, f);
    CHECK(shown_are(e, false, false, false, false));
    f.leaving = true;
    r = update_entries(e, f);
    CHECK(shown_are(e, true, false, false, false) && r.relayout[0] && r.sound == Sound::None);
    f.end_reason = 8;
    f.state = 1;
    r = update_entries(e, f);
    CHECK(shown_are(e, true, false, false, false) && !r.relayout[0]);
    f.end_reason = 1;
    update_entries(e, f);
    CHECK(shown_are(e, false, false, false, false));
    // In states 3 / 4 the results page replaces it (all flags cleared after the entry-0 rule).
    f.end_reason = 3;
    f.state = 3;
    update_entries(e, f);
    CHECK(shown_are(e, false, false, true, false));
    // Leaving with e68ae001 answered true: everything cleared.
    f.state = 0;
    f.film_clear = true;
    update_entries(e, f);
    CHECK(shown_are(e, false, false, false, false));
    f.film_clear = false;
    f.leaving = false;

    // State 5: entry 0 shown (speed unchanged), RESULTS / ROUND_RESULTS hidden.
    Entries s;
    s.speed[0] = 2.0f;
    f = Frame{};
    f.state = 3;
    update_entries(s, f);
    f.state = 5;
    update_entries(s, f);
    CHECK(shown_are(s, true, false, false, false) && s.speed[0] == 2.0f);
    // Sub-state 3: entry 3 set and cleared again in the same frame; entry 0 keeps its flag.
    Entries q;
    f.substate = 3;
    FrameResult rq = update_entries(q, f);
    CHECK(shown_are(q, false, false, false, false) && rq.relayout[3]);
    q.shown[0] = true;
    update_entries(q, f);
    CHECK(shown_are(q, true, false, false, false));

    // Back in state 2 after state 5: entries 0 and 3 hidden, the mark cleared.
    s.by_button = true;
    update_entries(s, playing());
    CHECK(shown_are(s, false, false, false, false) && !s.by_button && s.prev_state == 2);

    // Cinema (type 11): all cleared; 3 -> entry 2, 4 -> entry 3, nothing else (no automatic board, no button).
    Entries c;
    f = playing(true, true);
    f.cinema = true;
    r = update_entries(c, f);
    CHECK(shown_are(c, false, false, false, false) && r.sound == Sound::None);
    f.state = 3;
    update_entries(c, f);
    CHECK(shown_are(c, false, false, true, false));
    f.state = 4;
    update_entries(c, f);
    CHECK(shown_are(c, false, false, false, true));
    f.state = 5;
    update_entries(c, f);
    CHECK(shown_are(c, false, false, false, false));

    // The film query e68ae001 answered true clears every flag.
    Entries m;
    f = Frame{};
    f.state = 3;
    f.film_clear = true;
    update_entries(m, f);
    CHECK(shown_are(m, false, false, false, false));
    // Entry 1 is never shown (its debug request byte is not implemented).
}

static void test_alpha() {
    CHECK(overlay_alpha(2, 0, 0, 0.25f) == 0.25f);   // the HUD alpha
    CHECK(overlay_alpha(3, 0, 0, 0.25f) == 1.0f);
    CHECK(overlay_alpha(4, 0, 0, 0.25f) == 1.0f);
    CHECK(overlay_alpha(5, 0, 0, 0.25f) == 1.0f);
    CHECK(overlay_alpha(2, 4, 0, 0.25f) == 1.0f && overlay_alpha(2, 8, 0, 0.25f) == 1.0f);
    CHECK(overlay_alpha(2, 9, 0, 0.25f) == 1.0f && overlay_alpha(2, 12, 0, 0.25f) == 1.0f);
    CHECK(overlay_alpha(2, 3, 0, 0.25f) == 0.25f);   // game type 3 is no longer a 1.0 case
    CHECK(overlay_alpha(2, 0, 3, 0.25f) == 1.0f && overlay_alpha(2, 0, 8, 0.25f) == 1.0f);
    CHECK(overlay_alpha(2, 0, 1, 0.25f) == 0.25f);
}

static void test_player_out() {
    TrackerTimes t;
    CHECK(tracker_out(t, 1000));   // not in use: out
    t.in_use = true;
    t.spawn = 100;
    t.died = 50;
    CHECK(!tracker_out(t, 1000));   // alive
    t.died = 100;
    CHECK(!tracker_out(t, 1000));   // died == spawn: strictly after is needed
    t.died = 200;
    CHECK(tracker_out(t, 1000) && tracker_out(t, 200));
    CHECK(!tracker_out(t, 199));    // clock before the death time
    t.died = 50;
    t.flag_a = true;
    t.a_time = 500;
    CHECK(tracker_out(t, 500) && !tracker_out(t, 499));
    t.flag_a = false;
    t.flag_b = true;
    t.b_time = 600;
    CHECK(tracker_out(t, 600) && !tracker_out(t, 599));
    // Clock differences wrap like 32-bit integers.
    t.flag_b = false;
    t.spawn = INT32_MIN;
    t.died = INT32_MAX;
    CHECK(tracker_out(t, INT32_MIN));   // INT32_MIN - INT32_MAX wraps to 1

    TrackerTimes all[3];
    CHECK(every_player_out(all, 3, 0));   // none in use
    all[1].in_use = true;
    all[1].died = 10;
    CHECK(every_player_out(all, 3, 10));
    all[2].in_use = true;
    CHECK(!every_player_out(all, 3, 10));   // one still in play
    CHECK(every_player_out(all, 0, 0));
}

static void test_hint() {
    // In at 1 x ticks / 75 (4 per second), out at 2 x ticks / 75 (8 per second), clamped.
    float h = 0.0f;
    for (int i = 0; i < 6; ++i) h = hint_step(h, true, 5.0f);
    CHECK(near(h, 0.4f));
    h = hint_step(h, false, 7.5f);
    CHECK(near(h, 0.2f));
    h = hint_step(h, false, 30.0f);
    CHECK(h == 0.0f);
    for (int i = 0; i < 20; ++i) h = hint_step(h, true, 5.0f);
    CHECK(h == 1.0f);
    CHECK(hint_step(0.5f, true, 0.0f) == 0.5f);

    CHECK(hint_wanted(false, true, 2, false));
    CHECK(!hint_wanted(true, true, 2, false));    // a menu is up
    CHECK(!hint_wanted(false, false, 2, false));  // no death camera
    CHECK(!hint_wanted(false, true, 3, false));   // not playing
    CHECK(!hint_wanted(false, true, 2, true));    // every player out
}

// ---- ticker ----

struct Ticker {
    unsigned char b[kTickerSize] = {};
    void put(uint32_t off, float v) { std::memcpy(b + off, &v, sizeof v); }
    float get(uint32_t off) const {
        float v;
        std::memcpy(&v, b + off, sizeof v);
        return v;
    }
    uint32_t entry(int k) const { return kTickerEntry + uint32_t(k) * kTickerStride; }
    void add(int k, float start, float width, const char *text) {
        put(entry(k), start);
        put(entry(k) + 4, width);
        std::strcpy(reinterpret_cast<char *>(b + entry(k) + kTickerTextOff), text);
    }
    void free_entry(int k) { b[entry(k) + kTickerTextOff] = 0; }
};

// The ticker text's colour codes for the 2D command arena: red and blue trade places, nothing else changes.
static void test_ticker_colours() {
    const std::string ps3 = "|@CFFFFFFFF|Losses:|@CFF999999|      jonaschild: |@CFFF0C828|0|@CFF999999|     ";
    CHECK(ticker_arena_colours(ps3) == "|@CFFFFFFFF|Losses:|@CFF999999|      jonaschild: |@CFF28C8F0|0|@CFF999999|     ");
    CHECK(ticker_arena_colours("|@C80123456|x") == "|@C80563412|x");
    CHECK(ticker_arena_colours("|@c80123456|") == "|@c80123456|");    // lower-case c: not a colour code
    CHECK(ticker_arena_colours("|@CFF12345|x|") == "|@CFF12345|x|");  // seven digits
    CHECK(ticker_arena_colours("|@CFF12G456|") == "|@CFF12G456|");    // not hex
    CHECK(ticker_arena_colours("|@CFF123456") == "|@CFF123456");      // no closing bar
    CHECK(ticker_arena_colours("") == "");
}

static void test_ticker() {
    Ticker t;
    t.put(kTickerWidth, 900.0f);
    // Empty: the next text enters 10 right of the strip's right edge, into entry 0; finished.
    CHECK(near(ticker_next_start(t.b), 910.0f));
    CHECK(ticker_free_entry(t.b) == 0);
    CHECK(ticker_finished(t.b));

    // One text right after its add: not finished; a second one queues 150 behind it.
    t.add(0, 910.0f, 400.0f, "first");
    CHECK(!ticker_finished(t.b));
    CHECK(near(ticker_next_start(t.b), 910.0f + 400.0f + 150.0f));
    CHECK(ticker_free_entry(t.b) == 1);

    // Scrolled until start + 150 + width == strip width: finished (not greater); one unit earlier: not.
    t.put(t.entry(0), 900.0f - 150.0f - 400.0f + 1.0f);
    CHECK(!ticker_finished(t.b));
    t.put(t.entry(0), 900.0f - 150.0f - 400.0f);
    CHECK(ticker_finished(t.b));
    // Once fully in, a new text enters at the edge again, not behind the old one.
    CHECK(near(ticker_next_start(t.b), 910.0f));

    // A free entry in the middle is taken first; free entries count neither for placement nor for finished.
    t.add(1, 2000.0f, 50.0f, "second");
    t.free_entry(1);
    CHECK(ticker_free_entry(t.b) == 1);
    CHECK(ticker_finished(t.b));
    for (int k = 0; k < kTickerSlots; ++k) t.add(k, 0.0f, 10.0f, "x");
    CHECK(ticker_free_entry(t.b) == -1);

    // Visible: the last draw time within 0.1 s of the frame clock / 300.
    Ticker v;
    v.put(kTickerDrawn, 100.0f);
    CHECK(ticker_visible(v.b, 30000));        // 100.0 s
    CHECK(ticker_visible(v.b, 30000 + 10));   // 1/30 s later: the strip draws after the test in a frame
    CHECK(ticker_visible(v.b, 30000 - 29));
    CHECK(!ticker_visible(v.b, 30000 + 31));  // past 0.1 s: no longer
    CHECK(!ticker_visible(v.b, 60000));
    Ticker never;   // the strip never drew (+0x10 zero) and the clock is past 0.1 s
    CHECK(!ticker_visible(never.b, 300));
}

static void test_voice_column() {
    // J4: muted icon only for a muted player's voice cell of a player row that names a tracker.
    CHECK(voice_cell_muted(kPlayerRow, 0x1234, 3, kVoiceColumn, true));
    CHECK(!voice_cell_muted(kPlayerRow, 0x1234, 3, kVoiceColumn, false));      // not muted: the PS4 talking test
    CHECK(!voice_cell_muted(kPlayerRow, 0x1234, 3, 0x2b38314f, true));         // another column
    CHECK(!voice_cell_muted(1, 0x1234, 3, kVoiceColumn, true));                // not a player row
    CHECK(!voice_cell_muted(kPlayerRow, kPreviousGameList, 3, kVoiceColumn, true));   // names no tracker
    CHECK(!voice_cell_muted(kPlayerRow, 0x1234, 10, kVoiceColumn, true));      // index past the ten trackers
    CHECK(voice_cell_muted(kPlayerRow, 0x1234, 9, kVoiceColumn, true));
}

static void test_mute_action() {
    // N2: the own list shows the own team, the other list the other team; other lists are not ours.
    bool known = false;
    CHECK(mute_action_team(kMutePlayersOwnList, 1, known) == 1 && known);
    CHECK(mute_action_team(kMutePlayersOtherList, 1, known) == 0 && known);
    CHECK(mute_action_team(kMutePlayersOtherList, 0, known) == 1 && known);
    mute_action_team(0x649e809a, 0, known);
    CHECK(!known);
    // The current game's copy in NGM states 1 and 2, the previous game's otherwise.
    CHECK(!mute_action_previous_game(1) && !mute_action_previous_game(2));
    CHECK(mute_action_previous_game(0) && mute_action_previous_game(3) && mute_action_previous_game(5) &&
          mute_action_previous_game(-1));
    // Row r of a team's list = the first entry on that team at position r + 1.
    const TeamEntry e[] = {{0, 1}, {1, 1}, {0, 2}, {1, 2}, {0, 2}};
    CHECK(team_row_entry(e, 5, 0, 0) == 0);
    CHECK(team_row_entry(e, 5, 1, 0) == 1);
    CHECK(team_row_entry(e, 5, 0, 1) == 2);   // the first of two entries at position 2
    CHECK(team_row_entry(e, 5, 1, 1) == 3);
    CHECK(team_row_entry(e, 5, 0, 2) == -1);
    CHECK(team_row_entry(e, 3, 1, 1) == -1);  // only `count` entries are searched
    CHECK(team_row_entry(e, 5, 2, 0) == -1);  // no team 2
    CHECK(team_row_entry(e, 5, 0, -1) == -1); // position 0 is no row
    CHECK(team_row_entry(e, 0, 0, 0) == -1);
    // The entry's player: a tracker holds that net id, and it is not the local player.
    const uint32_t trackers[] = {1, 3, 0, 5};
    CHECK(mute_list_target(3, 1, trackers, 4));
    CHECK(!mute_list_target(1, 1, trackers, 4));   // the local player
    CHECK(!mute_list_target(4, 1, trackers, 4));   // gone: no tracker
    CHECK(!mute_list_target(5, 1, trackers, 3));   // only `count` trackers are searched
}

int main() {
    test_fade();
    test_priority();
    test_slide();
    test_auto_open();
    test_auto_notice();
    test_every_player_out_rule();
    test_button();
    test_film_queries();
    test_states();
    test_alpha();
    test_player_out();
    test_hint();
    test_ticker();
    test_ticker_colours();
    test_voice_column();
    test_mute_action();
    std::printf("%s\n", failures ? "u2mp overlay test FAILED" : "u2mp overlay test passed");
    return failures ? 1 : 0;
}
