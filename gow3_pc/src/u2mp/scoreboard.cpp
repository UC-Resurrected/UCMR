// u2mp scoreboard: brings back the in-match scoreboard page (PAGE_MULTIPLAYER_SCOREBOARD).
//
// The net player tracker overlay still owns the scoreboard as its entry 0, but PS4 lost the code
// that updated and drew those entries, so the page never appears. We hang a host function on the
// per-frame frontend 2D draw call: after the draw it toggles the page on a pad button press while a
// match is playing, fades it in and out, and runs the page's update, layout and render itself.
// The call may already be wrapped by another module (loading.cpp), so we chain to whatever it
// reached before.
//
// Reimplemented from the behaviour description u2mp/docs/spec/gameplay.md.
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "u2mp_internal.h"


namespace u2mp {
void install_scoreboard(int &installed);

namespace {

constexpr uint64_t kCallSite = 0x3fc52d;
constexpr uint64_t kDraw2D = 0x554340;
constexpr uint64_t kPageLayout = 0x5a2670;

constexpr uint64_t kTracker = 0x104f544;
constexpr uint64_t kTrackerShown = kTracker + 0x08;
constexpr uint64_t kTrackerFade = kTracker + 0x0c;
constexpr uint64_t kTrackerSpeed = kTracker + 0x10;
constexpr uint64_t kTrackerPage = kTracker + 0x18;
constexpr uint32_t kScoreboardPage = sid("PAGE_MULTIPLAYER_SCOREBOARD");
static_assert(kScoreboardPage == 0xf650325du, "scoreboard page id");
constexpr uint32_t kCoopRoundPage = sid("PAGE_MULTIPLAYER_ROUND_RESULTS");
constexpr uint32_t kCoopResultsPage = sid("PAGE_MULTIPLAYER_RESULTS");
constexpr uint32_t kPostgamePage = sid("PAGE_MULTIPLAYER_POSTGAME");
static_assert(kCoopRoundPage == 0x5dad5a68u && kCoopResultsPage == 0x3d437466u && kPostgamePage == 0x6b7378eau,
              "co-op result page ids");

constexpr uint64_t kPadButtons = 0x14b7fb0;
constexpr uint32_t kDefaultMask = 0x100000;   // touchpad, the PS3 SELECT button's place

constexpr uint64_t kItemFade = 0x1050e58;
constexpr uint64_t kFadeScale = 0x104ed74;
constexpr uint64_t kNetState = 0x14ce97c;
constexpr uint64_t kNgmState = 0x113b760 + 0x1e80;
constexpr uint64_t kMenuFrames = 0x1116180 + 0x104;
constexpr uint64_t kMenuPages = addr::menu_manager + 0x370;
constexpr int kMenuPageSlots = 200;

constexpr uint64_t kNgm = 0x113b760;
constexpr uint64_t kGameIndex = kNgm + 0x1ca0;
constexpr uint64_t kTaskIndex = kNgm + 0x1d6c;
constexpr uint64_t kRoundsPlayed = kNgm + 0x1d74;
constexpr uint64_t kRoundLimit = kNgm + 0x21c0;
constexpr uint64_t kLookupPointer = 0xa15e70;
constexpr uint32_t kNetGames = sid("*net-games*");
static_assert(kNetGames == 0x3b9a067du, "net-games id");
constexpr float kRoundResultsSeconds = 12.0f;   // gone ~8 s before the next objective message

constexpr float kTick = 1.0f / 300.0f;
static_assert(std::bit_cast<uint32_t>(kTick) == 0x3B5A740Eu, "1/300 as a float");

template <typename T> T peek(const unsigned char *p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
template <typename T> void poke(unsigned char *p, T v) { std::memcpy(p, &v, sizeof v); }
template <typename T> T peek_image(uint64_t off) { return peek<T>(image.base + off); }
template <typename T> void poke_image(uint64_t off, T v) { poke<T>(image.base + off, v); }

uint32_t button_mask = kDefaultMask;
uint64_t previous_target = 0;   // absolute address the call reached before we took it

bool shown = false;
uint32_t active_page = kScoreboardPage;   // the page drawn at the current result screen
unsigned char *entered_page = nullptr;
bool user_shown = false;   // the player pinned the board with the button
float fade = 0.0f;
float speed = 1.0f;
uint32_t previous_buttons = 0;
bool first_frame = true;
std::chrono::steady_clock::time_point last_time;
bool page_log_pending = true;

float auto_left = 0.0f;   // seconds the round/match result board stays up by itself
float end_hold = 0.0f;    // after the match ends the game leaves at once; keep the results up
int32_t last_end = 0;

using LookupFn = const unsigned char *(U2MP_GUEST *)(uint32_t);

int32_t game_type() {
    const auto lookup = reinterpret_cast<LookupFn>(image.base + kLookupPointer);
    const unsigned char *table = lookup(kNetGames);
    const int16_t game = peek_image<int16_t>(kGameIndex);
    if (!table || game < 0) return -1;
    const int32_t count = peek<int32_t>(table);
    const unsigned char *records = reinterpret_cast<const unsigned char *>(peek<uint64_t>(table + 8));
    if (!records || game >= count) return -1;
    return peek<int32_t>(records + 0x90 * uint64_t(game) + 0x0c);
}

// How many tasks (rounds) the current net game has; -1 when unknown.
int32_t task_count() {
    const auto lookup = reinterpret_cast<LookupFn>(image.base + kLookupPointer);
    const unsigned char *table = lookup(kNetGames);
    const int16_t game = peek_image<int16_t>(kGameIndex);
    if (!table || game < 0) return -1;
    const int32_t count = peek<int32_t>(table);
    const unsigned char *records = reinterpret_cast<const unsigned char *>(peek<uint64_t>(table + 8));
    if (!records || game >= count) return -1;
    const unsigned char *list = reinterpret_cast<const unsigned char *>(peek<uint64_t>(records + 0x90 * uint64_t(game) + 0x48));
    return list ? peek<int32_t>(list + 8) : -1;
}

// A page among the menu manager's loaded pages (the list stops at the first NULL).
unsigned char *find_page_id(uint32_t id) {
    for (int i = 0; i < kMenuPageSlots; ++i) {
        unsigned char *page = reinterpret_cast<unsigned char *>(peek_image<uint64_t>(kMenuPages + 8ull * i));
        if (!page) break;
        if (peek<uint32_t>(page + 0x10) == id) return page;
    }
    return nullptr;
}

unsigned char *find_page() { return find_page_id(active_page); }

// The local record is built before the local rank is known; take every record's rank from its slot.
void sync_rank_rows() {
    constexpr uint64_t kMatchPlayers = 0x1147bd8;
    constexpr uint64_t kMatchRecords = 0x114b4bc;
    constexpr uint64_t kRecordStride = 0x80;
    constexpr uint64_t kTrackers = 0x113b770;
    constexpr uint64_t kTrackerStride = 0x2c0;
    uint32_t count = peek_image<uint32_t>(kMatchPlayers);
    if (count > 10) count = 10;
    for (uint32_t i = 0; i < count; ++i) {
        unsigned char *record = image.base + kMatchRecords + uint64_t(i) * kRecordStride;
        const uint32_t id = peek<uint32_t>(record);
        if (!id) continue;
        for (int t = 0; t < 10; ++t) {
            const unsigned char *tracker = image.base + kTrackers + uint64_t(t) * kTrackerStride;
            if (peek<uint32_t>(tracker + 0x120) != id) continue;
            const uint32_t rank = peek<uint32_t>(tracker + 0x130);
            if (rank && rank <= 0xff && record[0xb] != uint8_t(rank))
                record[0xb] = uint8_t(rank);
            break;
        }
    }
}

void set_shown(bool on, const char *reason) {
    if (on == shown) return;
    shown = on;
    if (!on) entered_page = nullptr;
    poke_image<uint8_t>(kTrackerShown, on ? 1 : 0);
    if (on) {
        poke_image<uint32_t>(kTrackerPage, active_page);
        // Make the page lay its items out again from their start positions.
        if (unsigned char *page = find_page()) poke<int32_t>(page + 0xf8, 2);
        if (page_log_pending) {
            page_log_pending = false;
            log("scoreboard: page %08x %s", kScoreboardPage, find_page() ? "found" : "NOT FOUND");
            for (int i = 0; i < kMenuPageSlots; ++i) {
                const unsigned char *page = reinterpret_cast<const unsigned char *>(peek_image<uint64_t>(kMenuPages + 8ull * i));
                if (!page) break;
                const uint32_t id = peek<uint32_t>(page + 0x10);
                if (id == kCoopRoundPage) log("scoreboard: PAGE_MULTIPLAYER_ROUND_RESULTS loaded (slot %d)", i);
                if (id == kCoopResultsPage) log("scoreboard: PAGE_MULTIPLAYER_RESULTS loaded (slot %d)", i);
                if (id == kPostgamePage) log("scoreboard: PAGE_MULTIPLAYER_POSTGAME loaded (slot %d)", i);
            }
            int loaded = 0;
            while (loaded < kMenuPageSlots && peek_image<uint64_t>(kMenuPages + 8ull * loaded)) ++loaded;
            log("scoreboard: %d menu pages loaded", loaded);
        }
    }
    log("scoreboard: %s (%s)", on ? "shown" : "hidden", reason);
}

struct UpdateArgs {
    uint64_t menu;
    int32_t zero;
    float dt;
};
static_assert(sizeof(UpdateArgs) == 16, "page update arguments");

using UpdateFn = void(U2MP_GUEST *)(void *, UpdateArgs *);
using LayoutFn = void(U2MP_GUEST *)(void *, void *);
using RenderFn = void(U2MP_GUEST *)(void *, void *, void *);

void draw_page(unsigned char *page, float alpha, float x, float y, float dt) {
    const unsigned char *vptr = reinterpret_cast<const unsigned char *>(peek<uint64_t>(page));
    if (!vptr || !peek<uint64_t>(vptr + 0x10) || !peek<uint64_t>(vptr + 0x18)) return;

    if (entered_page != page) {
        entered_page = page;
        log("scoreboard: page %08x entered", peek<uint32_t>(page + 0x10));
    }

    UpdateArgs args{uint64_t(uintptr_t(image.base + addr::menu_manager)), 0, dt};
    reinterpret_cast<UpdateFn>(peek<uint64_t>(vptr + 0x10))(page, &args);

    // The draw descriptor the menu system hands to layout and render: draw context, scales,
    // alpha, offset, the 1280x720 screen rectangle and the menu time.
    alignas(8) unsigned char desc[0x50] = {};
    poke<uint64_t>(desc + 0x00, uint64_t(uintptr_t(image.base + addr::menu_manager + 0x10)));
    poke<float>(desc + 0x08, 1.0f);
    poke<float>(desc + 0x0c, 1.0f);
    poke<float>(desc + 0x10, 1.0f);
    poke<float>(desc + 0x14, 1.0f);
    poke<float>(desc + 0x18, alpha);
    poke<float>(desc + 0x24, 1.0f);
    poke<float>(desc + 0x28, x);
    poke<float>(desc + 0x2c, y);
    poke<float>(desc + 0x38, 1280.0f);
    poke<float>(desc + 0x3c, 720.0f);
    poke<float>(desc + 0x40, float(peek_image<int32_t>(kMenuFrames)) * kTick);
    poke<uint8_t>(desc + 0x44, 1);
    poke<int32_t>(desc + 0x48, peek_image<int32_t>(addr::menu_manager + 0x17a4));
    const uint32_t flags = peek<uint32_t>(page + 0xb0);
    if (flags & 0x80000) poke<float>(desc + 0x08, 1.0f * 0.75f);

    // Layout and render read the global item fade; give them this page's value and put the
    // global back afterwards so other pages are not affected.
    const float saved = peek_image<float>(kItemFade);
    const float scale = (flags & 0x800) ? 1.0f : peek_image<float>(kFadeScale);
    poke_image<float>(kItemFade, (1.0f - peek<float>(page + 0xf4)) * scale);

    reinterpret_cast<LayoutFn>(image.base + kPageLayout)(page, desc);
    reinterpret_cast<RenderFn>(peek<uint64_t>(vptr + 0x18))(page, nullptr, desc);

    poke_image<float>(kItemFade, saved);
}

using Draw2DFn = uint64_t(U2MP_GUEST *)(uint64_t, uint64_t, uint64_t);

U2MP_GUEST uint64_t frame_hook(uint64_t a, uint64_t b, uint64_t c) {
    const uint64_t result = reinterpret_cast<Draw2DFn>(previous_target)(a, b, c);

    const auto now = std::chrono::steady_clock::now();
    float dt = 0.0f;
    if (!first_frame) {
        // Single precision throughout: nanoseconds as a float, divided by 1e9.
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - last_time).count();
        dt = float(ns) / 1e9f;
        if (dt > 0.1f) dt = 0.1f;   // no big jump after a hitch or a pause
    }
    first_frame = false;
    last_time = now;

    const bool playing = peek_image<int32_t>(kNetState) > 1 && peek_image<int32_t>(kNgmState) == 2;

    const uint32_t buttons = peek_image<uint32_t>(kPadButtons);
    const bool pressed = (buttons & ~previous_buttons & button_mask) != 0;
    previous_buttons = buttons;

    // The round-end flag comes up while Playing is still the state; show the board then and take
    // it down as soon as the next round clears the flag. A final round keeps the match results up.
    if (playing) {
        const int32_t type = game_type();
        const int32_t end = peek_image<int32_t>(kNgm + 0x1e84);
        const int32_t rounds = peek_image<int32_t>(kRoundsPlayed);
        const int32_t limit = peek_image<int32_t>(kRoundLimit);
        const bool round_game = type == 0 || type == 4 || type == 5 || type == 6 || type == 7 || type == 8 ||
                                type == 9 || type == 10 || type == 12;
        if (round_game && end != 0 && last_end == 0) {
            const bool final_round = limit <= 0 || rounds + 1 >= limit;
            if (final_round) {
                const bool scoreboard_page = find_page_id(kScoreboardPage) != nullptr;
                const bool results = find_page_id(kCoopResultsPage) != nullptr;
                active_page = scoreboard_page ? kScoreboardPage : (results ? kCoopResultsPage : kScoreboardPage);
                auto_left = kRoundResultsSeconds;
                end_hold = kRoundResultsSeconds;
                log("scoreboard: match results page %08x %s (full %s, summary %s)", active_page,
                    find_page() ? "selected" : "missing", scoreboard_page ? "loaded" : "missing",
                    results ? "loaded" : "missing");
            } else {
                const bool round_mode = type == 4 || type == 5 || type == 6 || type == 7 || type == 8 ||
                                        type == 9 || type == 12;
                const bool round_page = round_mode && find_page_id(kCoopRoundPage) != nullptr;
                const bool results_page = find_page_id(kCoopResultsPage) != nullptr;
                const bool scoreboard_page = find_page_id(kScoreboardPage) != nullptr;
                if (round_page) active_page = kCoopRoundPage;
                else if (type == 10 && scoreboard_page) active_page = kScoreboardPage;
                else active_page = results_page ? kCoopResultsPage : kScoreboardPage;
                auto_left = 60.0f;   // until the next round clears the flag
                log("scoreboard: round %d results page %08x %s", int(rounds + 1), active_page,
                    find_page() ? "selected" : "missing");
            }
        }
        if (end == 0 && last_end != 0) {
            auto_left = 0.0f;
            end_hold = 0.0f;
            log("scoreboard: round started, board hidden");
        }
        last_end = end;
    } else if (end_hold > 0.0f) {
        end_hold -= dt;   // match over, the game is leaving: let the results board finish
    } else {
        auto_left = 0.0f;
        last_end = 0;
    }
    if (auto_left > 0.0f) auto_left -= dt;

    if (!playing && end_hold <= 0.0f) {
        shown = false;
        user_shown = false;
        set_shown(false, "not playing");
    } else if (pressed) {
        speed = 2.0f;
        poke_image<float>(kTrackerSpeed, 2.0f);
        user_shown = !(user_shown || auto_left > 0.0f);
        auto_left = 0.0f;
    }

    const bool want = user_shown || auto_left > 0.0f;
    if (want) sync_rank_rows();
    const char *reason = want ? (auto_left > 0.0f ? "round results" : "button")
                              : (user_shown ? "button" : "round results");
    set_shown(want, reason);
    fade += (want ? dt * 4.0f : -dt * 2.0f);
    if (fade < 0.0f) fade = 0.0f;
    if (fade > 1.0f) fade = 1.0f;
    poke_image<float>(kTrackerFade, fade);

    if (fade <= 0.0f) return result;
    unsigned char *page = find_page();
    if (!page) return result;

    // Slide in from below when showing, out upwards when hiding.
    const float ease = want ? fade * fade : 1.0f - (1.0f - fade) * (1.0f - fade);
    const float y = (1.0f - ease) * (want ? 20.0f : -20.0f);
    draw_page(page, fade, 0.0f, y, dt);
    return result;
}

}  // namespace

void install_scoreboard(int &installed) {
    if (const char *env = std::getenv("UNCHARTED_U2MP_SCOREBOARD_BUTTON")) {
        // The runtime is Windows x64, where unsigned long is 32 bits: overflow saturates to 0xffffffff.
        button_mask = uint32_t(std::strtoul(env, nullptr, 16));
        if (button_mask == 0) {
            log("scoreboard: off (UNCHARTED_U2MP_SCOREBOARD_BUTTON=0)");
            return;
        }
    }
    static const unsigned char prologue[] = {0x55, 0x48, 0x89, 0xE5};
    if (!image.matches(kPageLayout, prologue, sizeof prologue) || !image.matches(kDraw2D, prologue, sizeof prologue)) {
        log("scoreboard: page layout / 2D draw not as expected, not installed");
        return;
    }
    if (peek_image<uint32_t>(kTrackerPage) != kScoreboardPage || peek_image<float>(kTrackerSpeed) != 1.0f) {
        log("scoreboard: tracker 0x%llx is not as expected, not installed", (unsigned long long)kTracker);
        return;
    }
    if (!image.executable(kCallSite, 5) || image.base[kCallSite] != 0xE8) {
        log("scoreboard: 0x%llx is not a call, not installed", (unsigned long long)kCallSite);
        return;
    }
    const int32_t rel = peek_image<int32_t>(kCallSite + 1);
    const uint64_t current = uint64_t(int64_t(kCallSite + 5) + rel);   // may lie outside the image
    previous_target = uint64_t(uintptr_t(image.base)) + current;
    const char *name = current == kDraw2D ? "scoreboard: 2D draw" : "scoreboard: 2D draw (chained)";
    if (!retarget_call(kCallSite, current, reinterpret_cast<void *>(&frame_hook), name)) return;
    installed += 1;
    log("scoreboard: PAGE_MULTIPLAYER_SCOREBOARD port installed (button mask 0x%x; PS3 0x4cbf00 / 0x4cbc10)",
        button_mask);
}

void show_round_results() {
    const bool full = find_page_id(kScoreboardPage) != nullptr;
    const bool results = find_page_id(kCoopResultsPage) != nullptr;
    const bool round_page = find_page_id(kCoopRoundPage) != nullptr;
    active_page = full ? kScoreboardPage
                       : (results ? kCoopResultsPage : (round_page ? kCoopRoundPage : kScoreboardPage));
    if (!find_page()) active_page = kScoreboardPage;
    auto_left = kRoundResultsSeconds;
    end_hold = kRoundResultsSeconds;
    log("scoreboard: round results requested (%08x%s)", active_page, find_page() ? "" : ", page missing");
}

}  // namespace u2mp
