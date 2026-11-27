// u2mp rewrite tests, group "remaining" (remaining.cpp, clantag.cpp): stand-ins for what the sources call in other
// modules (frontend.cpp, session.cpp, texts.cpp, online.cpp, voice.cpp, progression.cpp, the runtime's text input).
// Compiled once per namespace by run.sh; every call is recorded and answered from hx::fk::st (deps_remaining.h).
// The C text-input functions get the OLD_FLAGS renames (..._old) for the old build.
#include <algorithm>

#include "u2mp_internal.h"
#include "online.h"
#include "progression.h"
#include "session_api.h"
#include "texts.h"
#include "voice.h"

#include "deps_remaining.h"
#include "harness.h"

using hx::fk::st;

namespace {
const char *show(const char *s) { return s ? s : "(NULL)"; }
const hx::fk::Friend *fr(int i) { return i >= 0 && size_t(i) < st.friends.size() ? &st.friends[size_t(i)] : nullptr; }
}  // namespace

namespace u2mp {

// frontend.cpp / session.cpp
void show_page(uint32_t page) { hx::rec("show_page 0x%x", page); }
const uint8_t *package_by_id(uint32_t id) {
    hx::rec("package_by_id 0x%x", id);
    auto it = st.packages.find(id);
    return it == st.packages.end() || !it->second ? nullptr : hx::mem + it->second;
}

// texts.cpp
U2MP_GUEST const char *text_find(void *db, uint32_t id) {
    hx::rec("text_find db=%s id=0x%x", hx::where(db).c_str(), id);
    return "PCTEXT";
}
void text_find_original(TextFindFn original) {
    hx::rec("text_find_original %s", hx::where(reinterpret_cast<void *>(original)).c_str());
    st.tramp = reinterpret_cast<void *>(original);
}
const char *own_text(uint32_t id) {
    hx::rec("own_text 0x%x", id);
    if (id == kTextYou) return "(you)";
    if (id == kTextMuted) return "muted";
    if (id == kTextUnmuted) return "unmuted";
    return "own?";
}
void text_set_player(const char *text) { hx::rec("text_set_player [%s]", show(text)); }
void text_set_slot(int slot, const char *text) { hx::rec("text_set_slot %d [%s]", slot, show(text)); }

// session.cpp
namespace session {
bool member_at(int row, uint32_t &net_id, std::string &name) {
    hx::rec("session::member_at %d", row);
    for (const auto &m : st.members)
        if (m.row == row) {
            net_id = m.id;
            name = m.name;
            return true;
        }
    return false;
}
bool member_name(uint32_t net_id, std::string &name) {
    hx::rec("session::member_name %u", net_id);
    for (const auto &m : st.members)
        if (m.id == net_id) {
            name = m.name;
            return true;
        }
    return false;
}
uint32_t local_id() {
    hx::rec("session::local_id");
    return st.local_id;
}
bool is_host() {
    hx::rec("session::is_host");
    return st.is_host;
}
bool kick(uint32_t net_id) {
    hx::rec("session::kick %u", net_id);
    return st.kick_result;
}
bool kick_griefer(uint32_t net_id) {   // only the new build calls it (B7 kicks with block since the parity work)
    hx::rec("session::kick_griefer %u", net_id);
    return st.kick_result;
}
std::string local_name() {
    hx::rec("session::local_name");
    return st.local_name;
}
}  // namespace session

// online.cpp
namespace online {
bool active() {
    hx::rec("online::active");
    return st.active;
}
bool linked() {
    hx::rec("online::linked");
    return st.linked;
}
bool promote(uint32_t net_id) {
    hx::rec("online::promote %u", net_id);
    return st.promote_result;
}
bool open_join_list() {
    hx::rec("online::open_join_list");
    return st.open_join_result;
}
int friend_count() {
    hx::rec("online::friend_count");
    return int(st.friends.size());
}
const char *friend_row(int i) {
    hx::rec("online::friend_row %d", i);
    return fr(i) ? fr(i)->row : nullptr;
}
std::string friend_name(int i) {
    hx::rec("online::friend_name %d", i);
    return fr(i) ? fr(i)->name : std::string();
}
bool friend_online(int i) {
    hx::rec("online::friend_online %d", i);
    return fr(i) && fr(i)->online;
}
bool friend_joinable(int i) {
    hx::rec("online::friend_joinable %d", i);
    return fr(i) && fr(i)->joinable;
}
bool join_friend(int i) {
    hx::rec("online::join_friend %d", i);
    return true;
}
bool invite_friend(int i) {
    hx::rec("online::invite_friend %d", i);
    return true;
}
int invite_count() {
    hx::rec("online::invite_count");
    return st.invite_count;
}
int friend_kind(int i) {
    hx::rec("online::friend_kind %d", i);
    return fr(i) ? fr(i)->kind : -1;
}
bool friend_add(const std::string &npid) {
    hx::rec("online::friend_add [%s]", npid.c_str());
    return st.friend_add_result;
}
bool accept_request(int i) {
    hx::rec("online::accept_request %d", i);
    return true;
}
bool remove_friend(int i) {
    hx::rec("online::remove_friend %d", i);
    return true;
}
std::string profile_text(int i) {
    hx::rec("online::profile_text %d", i);
    return fr(i) ? fr(i)->profile : std::string("no profile");
}
bool send_cinema(const std::string &npid) {
    hx::rec("online::send_cinema [%s]", npid.c_str());
    return true;
}
int cinema_received() {
    hx::rec("online::cinema_received");
    return st.cinema_received;
}
void member_data_changed() { hx::rec("online::member_data_changed"); }   // new build only (clan tag check, parity A 2.1)
#ifndef u2mp   // new build only (parity group B: picks kept by name, friend card); the old headers lack the types
int friend_index(const std::string &name) {
    hx::rec("online::friend_index [%s]", name.c_str());
    for (size_t i = 0; i < st.friends.size(); ++i)
        if (st.friends[i].name == name) return int(i);
    return -1;
}
bool friend_view(int i, FriendView &v) {
    hx::rec("online::friend_view %d", i);
    // no presence in these scenarios (View Profile falls back to profile_text), except a friend made a member of our
    // party (st.party_member_friend)
    if (i < 0 || i != st.party_member_friend) return false;
    v.party_member = true;
    return true;
}
#endif
}  // namespace online
#ifndef u2mp
std::vector<std::string> friend_card_lines(int i) {
    hx::rec("friend_card_lines %d", i);
    return {};
}
namespace session {
bool mm_member_at(int row, uint32_t &net_id, std::string &name) {   // matchmaking rows: same fake roster
    hx::rec("session::mm_member_at %d", row);
    for (const auto &m : st.members)
        if (m.row == row) {
            net_id = m.id;
            name = m.name;
            return true;
        }
    return false;
}
bool party_stranger(uint32_t net_id) {   // Mute All / Unmute All skip them (parity-h-session.md H7)
    const bool r = std::find(st.strangers.begin(), st.strangers.end(), net_id) != st.strangers.end();
    hx::rec("session::party_stranger %u -> %d", net_id, int(r));
    return r;
}
}  // namespace session
namespace online {
bool friend_party(int i, std::vector<std::string> &names) {   // View Profile's party lines (parity-g-friends.md G4)
    hx::rec("online::friend_party %d -> %d", i, int(st.party_known));
    if (st.party_known) names = st.party;
    return st.party_known;
}
}  // namespace online
#endif

// voice.cpp
namespace voice {
bool enabled() {
    hx::rec("voice::enabled");
    return st.voice_enabled;
}
bool toggle_mute(uint32_t net_id) {
    hx::rec("voice::toggle_mute %u", net_id);
    return st.toggle_result;
}
#ifndef u2mp   // new build only: the mute flags the popups read (parity J3) and Mute All / Unmute All (H7)
namespace {
bool is_muted(uint32_t id) { return std::find(st.muted.begin(), st.muted.end(), id) != st.muted.end(); }
std::string ids(const std::vector<uint32_t> &v) {
    std::string s;
    for (uint32_t id : v) s += (s.empty() ? "" : ",") + std::to_string(id);
    return s;
}
}  // namespace
bool muted(uint32_t net_id) {
    hx::rec("voice::muted %u -> %d", net_id, int(is_muted(net_id)));
    return is_muted(net_id);
}
bool all_muted(const std::vector<uint32_t> &others) {
    const bool r = std::all_of(others.begin(), others.end(), is_muted);
    hx::rec("voice::all_muted [%s] -> %d", ids(others).c_str(), int(r));
    return r;
}
bool none_muted(const std::vector<uint32_t> &others) {
    const bool r = std::none_of(others.begin(), others.end(), is_muted);
    hx::rec("voice::none_muted [%s] -> %d", ids(others).c_str(), int(r));
    return r;
}
void set_all_muted(const std::vector<uint32_t> &others, bool on) {
    hx::rec("voice::set_all_muted [%s] %d", ids(others).c_str(), int(on));
    for (uint32_t id : others) {
        if (on && !is_muted(id)) st.muted.push_back(id);
        if (!on) st.muted.erase(std::remove(st.muted.begin(), st.muted.end(), id), st.muted.end());
    }
}
#endif
}  // namespace voice

// progression.cpp
void progression_publish() { hx::rec("progression_publish"); }
void progression_publish_career() { hx::rec("progression_publish_career"); }   // new build only (parity A 2.1)
#ifndef u2mp   // new build: Players Met rows replace the recent players (parity-g-friends.md G1, f11e480)
std::vector<std::string> progression_players_met() {   // at most 50 rows (progression.h, store kMetShown)
    hx::rec("progression_players_met");
    std::vector<std::string> v = st.recent;
    if (v.size() > 50) v.resize(50);
    return v;
}
#endif
std::vector<std::string> progression_recent_players(size_t max) {
    hx::rec("progression_recent_players %zu", max);
    std::vector<std::string> v = st.recent;
    if (v.size() > max) v.resize(max);
    return v;
}
std::string progression_player_summary(const std::string &name) {
    hx::rec("progression_player_summary [%s]", name.c_str());
    return st.summary;
}

}  // namespace u2mp

// gpu/shim/gow3gpu.cpp: the runtime's text input (renamed *_old for the old build by OLD_FLAGS)
extern "C" int gow3gpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8) {
#ifdef u2mp   // the old build (-Du2mp=u2mp_old): (2026-11-27, remaining.md 2.3) the prompt no longer says "letters or digits"
    if (prompt_utf8 && std::strcmp(prompt_utf8, "Clan Tag (up to 4 letters or digits; empty = none)") == 0)
        prompt_utf8 = "Clan Tag (up to 4 characters; empty = none)";
#endif
    hx::rec("text_input_begin [%s] [%s]", show(initial_utf8), show(prompt_utf8));
    return st.begin_result;
}
extern "C" int gow3gpu_text_input_poll(char *out_utf8, uint64_t size) {
    bool zero = true;
    for (uint64_t i = 0; i < size; i++)
        if (out_utf8[i]) zero = false;
    int r = 0;
    if (!st.poll_results.empty()) {
        r = st.poll_results.front();
        st.poll_results.erase(st.poll_results.begin());
    }
    hx::rec("text_input_poll size=%llu zeroed=%d -> %d", (unsigned long long)size, zero ? 1 : 0, r);
    if (r == 1 && size) {
        size_t n = st.poll_text.size() < size - 1 ? st.poll_text.size() : size_t(size - 1);
        std::memcpy(out_utf8, st.poll_text.data(), n);
        out_utf8[n] = 0;
    }
    return r;
}

#ifndef u2mp
// New build only: the Mute Players lists (50a6b69, parity-n-remaining.md N2) take their row and target rules from
// overlay_rules.cpp, pure functions with no other dependency (not in the old build); the real ones are linked so
// the new code behaves as it ships.
#include "overlay_rules.cpp"
#endif
