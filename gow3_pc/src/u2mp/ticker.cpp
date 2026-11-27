// u2mp ticker: puts the friends stats line (online.cpp stats_ticker_text) on PAGE_MULTIPLAYER's bottom strip.
//
// The strip control still scrolls and draws the game's one static ticker object every frame PAGE_MULTIPLAYER is
// drawn, and the game's add-a-text function is still in the image, but the PS4 build lost the per-frame refresh
// that called it. Every per-frame update of the multiplayer page (session.cpp page_handler) asks for a refresh:
// when the strip was drawn within the last 0.1 s, every queued text has fully come in and 10 s have passed since
// the last one, a new line is built and handed to the game's add function, on the game's main thread.
//
// Reimplemented from the behaviour description u2mp/docs/spec/parity-ticker-respawn.md (part 1).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "online.h"
#include "overlay_rules.h"
#include "u2mp_internal.h"

namespace u2mp {
namespace {

constexpr uint64_t kTicker = 0x1138e40;          // the static ticker object (.bss)
constexpr uint64_t fTickerAdd = 0x5d6500;        // int add(ticker, text), uncalled on PS4
constexpr uint64_t fTickerDraw = 0x5d6860;       // scroll and draw, called by the strip control's draw
constexpr uint64_t kFrameClock = 0x1116180 + 0x104;
constexpr float kRefreshSeconds = 10.0f;

bool ready = false;
float next_refresh = 0.0f;
bool first_add_logged = false;
bool add_mismatch_logged = false;

struct Check {
    uint64_t offset;
    const char *bytes;
};

bool check_bytes(const Check &c) {
    unsigned char want[64];
    size_t n = 0;
    for (const char *p = c.bytes; *p && n < sizeof want;) {
        while (*p == ' ') ++p;
        if (!p[0] || !p[1]) break;
        auto nibble = [](char h) { return h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10; };
        want[n++] = static_cast<unsigned char>(nibble(p[0]) << 4 | nibble(p[1]));
        p += 2;
    }
    return image.matches(c.offset, want, n);
}

// Diagnostics (owner report 2026-11-27: the strip stays empty although a text is added): while the strip is drawn,
// every 4 s for the first 8 times, the strip rectangle, the last draw time and every entry in use (start, width,
// the text's first characters). Shows whether the entries move, where they are and whether they are freed.
int state_logs = 0;
float next_state_log = 0.0f;

float f32_at(const unsigned char *p) {
    float v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

void log_state(const unsigned char *ticker, float now) {
    if (state_logs >= 8 || now < next_state_log) return;
    const float drawn = f32_at(ticker + 0x10);
    if (now - drawn > 0.5f || drawn - now > 0.5f) return;   // the strip is not on screen
    next_state_log = now + 4.0f;
    ++state_logs;
    std::string entries;
    for (int k = 0; k < 8; ++k) {
        const unsigned char *e = ticker + 0x14 + 0x208 * k;
        if (!e[8]) continue;
        char line[96];
        std::snprintf(line, sizeof line, " [%d] start %.1f width %.1f \"%.24s\"", k, double(f32_at(e)),
                      double(f32_at(e + 4)), reinterpret_cast<const char *>(e + 8));
        entries += line;
    }
    log("ticker: t %.2f strip x %.1f y %.1f w %.1f h %.1f, drawn %.2f;%s", double(now), double(f32_at(ticker)),
        double(f32_at(ticker + 8)), double(f32_at(ticker + 4)), double(f32_at(ticker + 0xc)), double(drawn),
        entries.empty() ? " no entry" : entries.c_str());
}

}  // namespace

void install_ticker(int &installed) {
    // The add function's start and two places inside it, the strip draw's start and its time store, and the one
    // instruction that names the object followed by the draw call (spec 1.4).
    static const Check kChecks[] = {
        {fTickerAdd, "55 48 89 e5 41 57 41 56 41 54 53 48 81 ec d0 00 00 00 49 89 f6 49 89 fc 4c 8b 3d e1 46 a7 00 49"},
        {0x5d6535, "41 8a 44 24 1c 84 c0 74 20"},
        {0x5d66b5, "c5 fa 11 8d 1c ff ff ff 49 8d 9c 24 24 02 00 00"},
        {fTickerDraw, "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec b8 00 00 00 48 89 fb"},
        {0x5d689c, "c5 fa 11 4b 10"},
        {0x59f32a, "48 8d 3d 0f 9b b9 00"},
        {0x59f35b, "e8 00 75 03 00"},
    };
    for (const Check &c : kChecks) {
        if (!check_bytes(c)) {
            log("ticker: bytes at 0x%llx not as expected, the friends stats ticker stays empty",
                (unsigned long long)c.offset);
            return;
        }
    }
    if (kTicker + overlay::kTickerSize > image.size) {
        log("ticker: object 0x%llx outside the image, not installed", (unsigned long long)kTicker);
        return;
    }
    ready = true;
    installed += 1;
    log("ticker: PAGE_MULTIPLAYER stats ticker refresh ready (add 0x%llx, object 0x%llx; PS3 0x447248 / 0x42da98)",
        (unsigned long long)fTickerAdd, (unsigned long long)kTicker);
}

void ticker_refresh() {
    if (!ready) return;
    unsigned char *ticker = image.base + kTicker;
    int32_t clock;
    std::memcpy(&clock, image.base + kFrameClock, sizeof clock);
    const float now = float(clock) * (1.0f / 300.0f);
    if (next_refresh - now > kRefreshSeconds) next_refresh = now;   // the clock started again lower
    log_state(ticker, now);
    if (!overlay::ticker_visible(ticker, clock) || !overlay::ticker_finished(ticker) || !(now > next_refresh))
        return;
    next_refresh = now + kRefreshSeconds;

    const std::string text = online::stats_ticker_text();
    if (text.empty()) return;
    if (text.size() >= overlay::kTickerTextMax) {   // the builder keeps it below 511 characters; never overflow
        log("ticker: text of %zu bytes is too long for an entry, not added", text.size());
        return;
    }
    const int slot = overlay::ticker_free_entry(ticker);
    const float expected = overlay::ticker_next_start(ticker);
    reinterpret_cast<int(U2MP_GUEST *)(void *, const char *)>(image.base + fTickerAdd)(ticker, text.c_str());

    // The add answers nothing; the entry it filled shows whether the text went in where the rules say.
    if (slot < 0) return;
    const unsigned char *entry = ticker + overlay::kTickerEntry + uint32_t(slot) * overlay::kTickerStride;
    float start;
    std::memcpy(&start, entry, sizeof start);
    const bool in = entry[overlay::kTickerTextOff] != 0;
    if (in && start == expected) {
        if (!first_add_logged) {
            first_add_logged = true;
            log("ticker: first text added (entry %d, start %.1f, %zu bytes)", slot, double(start), text.size());
        }
    } else if (!add_mismatch_logged) {
        add_mismatch_logged = true;
        log("ticker: add left entry %d %s, start %.1f (expected %.1f)", slot, in ? "filled" : "EMPTY", double(start),
            double(expected));
    }
}

}  // namespace u2mp
