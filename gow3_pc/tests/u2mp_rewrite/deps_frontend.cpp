// u2mp rewrite tests, group "frontend": stand-ins for what frontend*.cpp call in other modules (session.cpp,
// online.cpp, menus.cpp, dlc.cpp). Compiled for both namespaces; they record the state-changing calls and answer
// from hx::fe::s (deps_frontend.h).
#include <string>
#include <vector>

#include "deps_frontend.h"
#include "frontend3.h"
#include "harness.h"
#include "online.h"
#include "session_api.h"
#include "u2mp_internal.h"
#if !defined(u2mp)
#include "voice.h"   // new build only (the 62fc26b headers have no voice.h)
#endif

namespace u2mp {

bool playlist_game_types(std::vector<int32_t> &types) {
    hx::rec("playlist_game_types -> %d", hx::fe::s.playlist);
    if (hx::fe::s.playlist) types = hx::fe::s.types;
    return hx::fe::s.playlist;
}

bool dlc_net_game_available(int32_t index) {
    bool ok = true;
    for (int32_t i : hx::fe::s.dlc_off)
        if (i == index) ok = false;
    hx::rec("dlc_net_game_available %d -> %d", index, ok);
    return ok;
}

namespace session {

void install(int &installed) {
    hx::rec("session::install same counter %d (value %d)", &installed == hx::fe::s.install_ref, installed);
    installed += 5;
}
bool is_host() {
    if (hx::fe::s.rec_queries) hx::rec("is_host -> %d", hx::fe::s.host);
    return hx::fe::s.host;
}
int member_count() {
    if (hx::fe::s.rec_queries) hx::rec("member_count -> %d", hx::fe::s.members);
    return hx::fe::s.members;
}
uint32_t net_ticks() { return hx::fe::s.clock; }
void send_message(int slot, const std::vector<uint8_t> &body, int except) {
    std::string b;
    char x[4];
    for (uint8_t c : body) {
        std::snprintf(x, sizeof x, "%02x", c);
        b += x;
    }
    hx::rec("send slot %d [%s] except %d", slot, b.c_str(), except);
}
void create_match() {
    hx::rec("create_match");
    if (hx::fe::s.create_clears_phase) hx::put<int32_t>(0x104f1c8 + 0x34, 0);
}

}  // namespace session

namespace online {

bool select_join(int row) {
    hx::rec("online::select_join %d -> %d", row, hx::fe::s.select_join);
    return hx::fe::s.select_join;
}
bool mm_online() {
    if (hx::fe::s.rec_queries) hx::rec("mm_online -> %d", hx::fe::s.mm_online);
    return hx::fe::s.mm_online;
}
void mm_begin(uint32_t playlist) { hx::rec("mm_begin 0x%x", playlist); }
int mm_step() {
    hx::rec("mm_step -> %d", hx::fe::s.mm_step);
    return hx::fe::s.mm_step;
}
void mm_reset() { hx::rec("mm_reset"); }
void mm_research() { hx::rec("mm_research"); }
void mm_leave() { hx::rec("mm_leave"); }

#if !defined(u2mp)
// Only the new headers declare these (parity groups D/E/F: search passes, party leave, host gone; D start hide).
// The pure queries are not recorded: the old code has no such calls, and their effects are in the record.
bool active() { return hx::fe::s.active || hx::fe::s.mm_online; }
bool host_in_party() { return hx::fe::s.host_in_party; }
bool party_member() { return hx::fe::s.party_member; }
uint64_t own_party_id() { return hx::fe::s.own_party_id; }
uint64_t match_room() { return hx::fe::s.match_room; }
int party_missing() { return hx::fe::s.party_missing; }
int mm_pending_members() { return hx::fe::s.mm_pending; }
void mm_begin(uint32_t playlist, uint32_t index, const MmCriteria &c) {
    hx::rec("mm_begin 0x%x index %u pass %d ping %u rank_only %d", playlist, index, c.pass, c.ping_limit_ms, c.rank_only);
}
void mm_create() { hx::rec("mm_create"); }
void mm_host_gone(const char *why, bool bad_room) { hx::rec("mm_host_gone '%s' bad %d", why ? why : "(null)", bad_room); }
void mm_leave_to_leader() { hx::rec("mm_leave_to_leader"); }
void hide_for_start(bool on) { hx::rec("hide_for_start %d", on); }
#endif

}  // namespace online

#if !defined(u2mp)
// New-only (475fba8 / parity F): menus.cpp's playlist index, the session's party leave, voice J3.
int32_t playlist_index(uint32_t) { return hx::fe::s.playlist_index; }
namespace session {
void leave_party() { hx::rec("session::leave_party"); }
// #63: a map pack map only when every member loads the same map; the scenarios have no map packs (every map is the
// same for all), so the vote pool stays the old one
bool net_game_same_map(int32_t) { return true; }
}  // namespace session
namespace voice {
void unmute_strangers() { hx::rec("voice::unmute_strangers"); }
}  // namespace voice
#endif

}  // namespace u2mp
