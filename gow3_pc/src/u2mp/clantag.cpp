// u2mp: editing the clan tag on the multiplayer profile page.
//
// On PS3 BUTTON_CLANTAG opened the on-screen keyboard and the page polled it every frame. On PC the
// runtime's in-window text input takes its place (Enter or Cross accepts, Esc or Circle cancels):
// remaining.cpp's button callback opens it with edit_clan_tag(), and the page's per-frame event calls
// clan_tag_frame() until it closes. The tag is kept in NetPlayerData, where the game's label query
// shows it and progression.cpp saves and publishes it.
//
// While the game is online (NET_SM > 1), clan_tag_check_frame() checks every new tag before it counts and then
// publishes the career and the member data, as PS3 did with its score service's comment censor (u2mp/docs/spec/
// parity-a-leaderboards.md 2.1). A refused tag, or one whose check failed, becomes "*". The rules are in
// clantag_rules.h; this file feeds them the tag and the censor's answers and does the writes, publishes and logs.
//
// PLATFORM replacement: PS3 asked the score service's comment censor. Our online program's interface
// (u2_online_abi.h) has no comment censor request, so censor_start() answers "unavailable" and the spec's no-censor
// path runs: the tag is accepted at once and published (log "check unavailable"). The started / running / refused
// paths stay in place for a censor that may come later.
//
// Reimplemented from the behaviour descriptions u2mp/docs/spec/remaining.md and parity-a-leaderboards.md.
#include "clantag_rules.h"
#include "online.h"
#include "progression.h"
#include "u2mp_internal.h"

#include <cstdint>
#include <cstring>
#include <string>

extern "C" int gow3gpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
extern "C" int gow3gpu_text_input_poll(char *out_utf8, uint64_t size);

namespace u2mp {
namespace {

constexpr uint64_t kClanTag = 0x11474f8 + 0x1a8;   // NetPlayerData: 8 bytes, NUL-padded
constexpr size_t kTagBytes = clantag::kTagBytes;
constexpr size_t kTagChars = 4;

constexpr uint64_t kNetSmState = 0x14ce97c;        // s32: > 1 = online

bool g_editing = false;

// The tag check: the last tag checked (kept for the process) and whether a check runs.
clantag::Check g_check;

// The censor service (none yet, see the top of the file). A censor plugs in here: censor_start() takes the tag text
// (<= 63 bytes) and says whether a check started; censor_poll() then answers while it runs.
clantag::Start censor_start(const char *text) {
    (void)text;
    return clantag::Start::Unavailable;
}
clantag::Verdict censor_poll() { return clantag::Verdict::Accepted; }

// The career and the member data go out (a cleared tag, or the verdict is in).
void publish_tag() {
    progression_publish_career();
    online::member_data_changed();
}

// Keeps the tag as typed, as PS3 did: its keyboard (alphabet panel, 4 characters) gave ASCII letters, digits, symbols
// and spaces, case kept, and the page turned "\" and "|" into spaces. Anything else (control or non-ASCII bytes, which
// that keyboard could not type) is skipped.
void store_tag(const char *text, size_t size) {
    char tag[kTagBytes] = {};
    size_t taken = 0;
    for (size_t i = 0; i < size && text[i] && taken < kTagChars; ++i) {
        const char c = text[i];
        if (c < 0x20 || c > 0x7e) continue;
        tag[taken++] = c == '\\' || c == '|' ? ' ' : c;
    }
    std::memcpy(image.at<char>(kClanTag), tag, kTagBytes);
    log("clan tag: [%.4s]", tag);
    progression_publish();
}

}  // namespace

bool edit_clan_tag() {
    if (g_editing) return true;
    const char *stored = image.at<const char>(kClanTag);
    const std::string current(stored, strnlen(stored, kTagBytes));
    if (!gow3gpu_text_input_begin(current.c_str(), "Clan Tag (up to 4 characters; empty = none)")) {
        log("clan tag: no text input available");
        return false;
    }
    g_editing = true;
    log("clan tag: text input open");
    return true;
}

void clan_tag_frame() {
    if (!g_editing) return;
    char text[64] = {};
    const int result = gow3gpu_text_input_poll(text, sizeof text);
    if (result == 0) return;   // still open
    g_editing = false;
    if (result == 1)
        store_tag(text, sizeof text);
    else
        log("clan tag: unchanged");
}

void clan_tag_check_frame() {
    if (!image.base || *image.at<const int32_t>(kNetSmState) <= 1) return;
    char before[kTagBytes];   // the checked tag as it was before this frame's step (for the "refused" log)
    std::memcpy(before, g_check.checked, kTagBytes);
    clantag::Step step;
    if (g_check.running) {
        step = clantag::on_verdict(g_check, censor_poll());
    } else {
        switch (clantag::notice(g_check, image.at<const char>(kClanTag))) {
        case clantag::Change::Same: return;
        case clantag::Change::Cleared: publish_tag(); return;
        case clantag::Change::Check: break;
        }
        char text[kTagBytes + 1] = {};
        std::memcpy(text, g_check.checked, kTagBytes);
        step = clantag::on_start(g_check, censor_start(text));
    }
    switch (step) {
    case clantag::Step::Waiting: return;
    case clantag::Step::Started: log("clan tag: [%.4s] check started", g_check.checked); return;
    case clantag::Step::StartFailed: return;   // kept as typed and not checked again until it changes
    case clantag::Step::Unavailable: log("clan tag: check unavailable, [%.4s] kept", g_check.checked); break;
    case clantag::Step::Kept: log("clan tag: [%.4s] kept", g_check.checked); break;
    case clantag::Step::Refused: log("clan tag: [%.4s] refused, now [*]", before); break;
    }
    // the verdict is in (or there is no censor): the checked tag, "*" when refused, is the tag; then publish
    std::memcpy(image.at<char>(kClanTag), g_check.checked, kTagBytes);
    publish_tag();
}

}  // namespace u2mp
