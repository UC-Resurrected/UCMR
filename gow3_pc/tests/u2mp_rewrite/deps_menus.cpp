// u2mp rewrite tests, group "menus" (menus.cpp, menuimages.cpp): stand-ins for what the sources call in other
// modules (session.cpp, online.cpp, voice.cpp, progression.cpp, dlc.cpp). Compiled once per namespace by run.sh;
// every call is recorded and answered from hx::mn::s (deps_menus.h).
#include "u2mp_internal.h"
#include "online.h"
#include "progression.h"
#include "session_api.h"
#include "voice.h"

#include "deps_menus.h"
#include "harness.h"

#include <cstring>

using hx::mn::s;

namespace {
const char *row_of(const std::vector<std::string> &v, int i) {
    return i >= 0 && size_t(i) < v.size() ? v[size_t(i)].c_str() : "(out of range)";
}
}  // namespace

namespace u2mp {

const uint8_t *package_by_id(uint32_t id) {
    hx::rec("package_by_id 0x%x", id);
    return s.package ? hx::mem + s.package : nullptr;
}

bool dlc_net_game_available(int32_t index) {
    bool off = false;
    for (int32_t i : s.dlc_off) off = off || i == index;
    hx::rec("dlc_net_game_available %d -> %d", index, off ? 0 : 1);
    return !off;
}

std::vector<std::string> progression_recent_players(size_t max) {
    hx::rec("progression_recent_players %zu -> %zu", max, s.recent.size());
    return s.recent;
}

#if !defined(u2mp)   // new build only: menus.cpp's Players Met query asks for the G1 list (f11e480, spec
                     // parity-g-friends.md G1); the old headers lack it. Answers the same rows.
std::vector<std::string> progression_players_met() {
    hx::rec("progression_players_met -> %zu", s.recent.size());
    return s.recent;
}
// menus.cpp's Matchmaking row (cond 135) asks for the imported map packs.
uint32_t dlc_packs() {
    hx::rec("dlc_packs -> %u", s.packs);
    return s.packs;
}
#endif

namespace session {
std::string local_name() {
    hx::rec("session::local_name");
    return s.local_name;
}
int member_count() {
    hx::rec("session::member_count -> %d", s.members);
    return s.members;
}
bool member_at(int row, uint32_t &net_id, std::string &name) {
    const bool ok = row >= 0 && size_t(row) < s.member_ok.size() && s.member_ok[size_t(row)];
    hx::rec("session::member_at %d -> %d", row, ok ? 1 : 0);
    if (!ok) return false;
    net_id = s.member_id[size_t(row)];
    name = s.member_name[size_t(row)];
    return true;
}
bool is_host() {
    hx::rec("session::is_host -> %d", s.host ? 1 : 0);
    return s.host;
}
}  // namespace session

namespace online {
bool active() {
    hx::rec("online::active -> %d", s.online ? 1 : 0);
    return s.online;
}
int friend_count() {
    hx::rec("online::friend_count -> %d", s.friends);
    return s.friends;
}
bool friend_online(int i) {
    const bool on = i >= 0 && size_t(i) < s.friend_on.size() && s.friend_on[size_t(i)];
    hx::rec("online::friend_online %d -> %d", i, on ? 1 : 0);
    return on;
}
const char *friend_row(int i) {
    hx::rec("online::friend_row %d", i);
    return row_of(s.friend_rows, i);
}
int join_rows() {
    hx::rec("online::join_rows -> %d", s.join_rows);
    return s.join_rows;
}
const char *join_row(int row) {
    hx::rec("online::join_row %d", row);
    return row_of(s.join_row_texts, row);
}
const char *join_title() {
    hx::rec("online::join_title");
    return s.join_title.c_str();
}
#ifndef u2mp   // new build only (parity group B friends list / friend card); the old headers lack the types
bool friend_view(int i, FriendView &v) {   // a plain online friend in another game, named by its row text
    hx::rec("online::friend_view %d", i);
    if (i < 0 || i >= s.friends) return false;
    v = FriendView{};
    v.npid = row_of(s.friend_rows, i);
    v.online = i < int(s.friend_on.size()) && s.friend_on[size_t(i)];
    return true;
}
std::string friend_name(int i) {
    hx::rec("online::friend_name %d", i);
    return row_of(s.friend_rows, i);
}
int friend_index(const std::string &name) {
    hx::rec("online::friend_index [%s]", name.c_str());
    for (int i = 0; i < s.friends; ++i)
        if (name == row_of(s.friend_rows, i)) return i;
    return -1;
}
const char *request_state(int i) {
    hx::rec("online::request_state %d", i);
    return "";
}
void refresh_friend_list() { hx::rec("online::refresh_friend_list"); }
#endif
}  // namespace online

namespace voice {
float volume() {
    uint32_t b;
    std::memcpy(&b, &s.volume, 4);
    hx::rec("voice::volume -> %08x", b);
    return s.volume;
}
void set_volume(float v) {
    uint32_t b;
    std::memcpy(&b, &v, 4);
    hx::rec("voice::set_volume %08x", b);
}
}  // namespace voice

}  // namespace u2mp
