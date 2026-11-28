// u2mp rewrite tests, group "session" (session.cpp): stand-ins for what the source calls in other modules
// (net::Transport / net::Address of net.cpp, gamenet, voice, online, matchmaking, frontend6, netheap, netinit,
// netpatch, menuimages, menus, dlc, unlocks, progression, remaining's show_notice, frontend's show_page, the
// runtime's file translation). Compiled once per namespace by run.sh; every call is recorded and answered from
// hx::ss::st (deps_session.h). runtime_file_translate gets the OLD_FLAGS rename (..._old) for the old build.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "frontend3.h"
#include "frontend6.h"
#include "gamenet.h"
#include "net.h"
#include "online.h"
#include "progression.h"
#include "session_api.h"
#include "u2mp_internal.h"
#include "unlocks.h"
#include "voice.h"

#include "deps_session.h"
#include "harness.h"

using hx::ss::st;

namespace u2mp {

namespace session {
void install(int &installed);   // declared in frontend.cpp
}

namespace {
struct FakeLink : net::Link {
    void send(const net::Address &, const uint8_t *, size_t) override {}
    int receive(net::Address &, uint8_t *, size_t) override { return 0; }
} fake_link;

const char *lk(const net::Link *l) { return !l ? "null" : l == &fake_link ? "link" : "other"; }
std::string at(const net::Address &a) {
    char b[48];
    std::snprintf(b, sizeof b, "%08x:%u", a.ip, a.port);
    return b;
}
}  // namespace

// ---- net.cpp -------------------------------------------------------------------------------------------
namespace net {
std::string Address::text() const {
    char b[32];
    const uint8_t *p = reinterpret_cast<const uint8_t *>(&ip);
    std::snprintf(b, sizeof b, "%u.%u.%u.%u:%u", p[0], p[1], p[2], p[3], port);
    return b;
}
bool Address::parse(const std::string &text, uint16_t default_port, Address &out) {
    hx::rec("net::Address::parse [%s] %u", text.c_str(), default_port);
    unsigned a, b, c, d, port = default_port;
    if (!st.parse_ok || std::sscanf(text.c_str(), "%u.%u.%u.%u:%u", &a, &b, &c, &d, &port) < 4) return false;
    out.ip = a | b << 8 | c << 16 | d << 24;
    out.port = uint16_t(port);
    return true;
}
Transport::~Transport() {}
Transport *g_transport = nullptr;   // the module's transport (the one it last hosted or joined with)
bool Transport::host(uint16_t port) {
    g_transport = this;
    hx::rec("transport.host %u", port);
    hosting_ = true;
    local_slot_ = 0;
    return st.host_ok;
}
bool Transport::join(const Address &host_address, uint16_t local_port) {
    g_transport = this;
    hx::rec("transport.join %s local %u", at(host_address).c_str(), local_port);
    hosting_ = false;
    local_slot_ = st.local_slot;
    return st.join_ok;
}
bool Transport::host_link(Link *link) {
    g_transport = this;
    hx::rec("transport.host_link %s", lk(link));
    hosting_ = true;
    local_slot_ = 0;
    return st.host_link_ok;
}
bool Transport::join_link(Link *link, const Address &host_address, uint8_t want_slot) {
    g_transport = this;
    hx::rec("transport.join_link %s %s want %u", lk(link), at(host_address).c_str(), want_slot);
    hosting_ = false;
    local_slot_ = st.local_slot;
    return st.join_link_ok;
}
void Transport::close() { hx::rec("transport.close"); }
void Transport::pump(double now) {
    hx::rec("transport.pump %.4f", now);
    local_slot_ = st.local_slot;
    std::vector<hx::ss::Incoming> in;
    in.swap(st.incoming);
    for (const auto &e : in) {
        if (e.peer) {
            hx::rec("  deliver peer %u %s", e.slot, e.joined ? "joined" : "left");
            if (on_peer) on_peer(e.slot, e.joined);
        } else {
            hx::rec("  deliver message from %u [%s]", e.slot, hx::ss::hex(e.data.data(), e.data.size()).c_str());
            if (on_message) on_message(e.slot, e.data.data(), e.data.size());
        }
    }
}
void Transport::send(uint8_t slot, bool reliable, const uint8_t *data, size_t size) {
    hx::rec("transport.send %u rel %d [%s]", slot, reliable ? 1 : 0, hx::ss::hex(data, size).c_str());
}
void Transport::broadcast(bool reliable, const uint8_t *data, size_t size) {
    hx::rec("transport.broadcast rel %d [%s]", reliable ? 1 : 0, hx::ss::hex(data, size).c_str());
}
bool Transport::connected() const { return true; }
bool Transport::peer_address(uint8_t slot, Address &out) const {
    hx::rec("transport.peer_address %u", slot);
    auto it = st.peer_addr.find(slot);
    if (it == st.peer_addr.end()) return false;
    out.ip = it->second.ip;
    out.port = it->second.port;
    return true;
}
}  // namespace net

// ---- gamenet.cpp -----------------------------------------------------------------------------------------
namespace gamenet {
void set_transport(net::Transport *t) { hx::rec("gamenet::set_transport %s", t ? "set" : "null"); }
int attach(uint8_t slot, uint32_t key, bool direct) {
    hx::rec("gamenet::attach %u %u %d", slot, key, direct ? 1 : 0);
    return 0;
}
void detach(uint8_t slot) { hx::rec("gamenet::detach %u", slot); }
uint64_t message_layer(uint8_t) { return 0; }
bool install() {
    hx::rec("gamenet::install");
    return st.gamenet_install;
}
void frame(double now) { hx::rec("gamenet::frame %.4f", now); }
double clock() {
    hx::rec("gamenet::clock");
    return st.clock;
}
void receive(uint8_t slot, const uint8_t *data, size_t size) {
    hx::rec("gamenet::receive %u [%s]", slot, hx::ss::hex(data, size).c_str());
}
}  // namespace gamenet

// ---- voice.cpp -------------------------------------------------------------------------------------------
namespace voice {
void set_transport(net::Transport *t) { hx::rec("voice::set_transport %s", t ? "set" : "null"); }
void frame() { hx::rec("voice::frame"); }
void receive(uint8_t slot, const uint8_t *data, size_t size) {
    hx::rec("voice::receive %u [%s]", slot, hx::ss::hex(data, size).c_str());
}
void stop() { hx::rec("voice::stop"); }
bool talking(uint32_t id) {
    hx::rec("voice::talking %u", id);
    return st.talking.count(id) != 0;
}
bool muted(uint32_t id) {
    hx::rec("voice::muted %u", id);
    return st.muted.count(id) != 0;
}
}  // namespace voice

// ---- online.cpp ------------------------------------------------------------------------------------------
namespace online {
net::Link *start() {
    hx::rec("online::start");
    return st.link ? &fake_link : nullptr;
}
bool active() {
    hx::rec("online::active");
    return st.online_active;
}
void frame() { hx::rec("online::frame"); }
bool create_match_room() {
    hx::rec("online::create_match_room");
    return st.create_room;
}
uint64_t match_room() {
    hx::rec("online::match_room");
    return st.match_room;
}
#if !defined(u2mp)   // new build: follow_match_room reports whether it followed (online.h after the rewrite)
bool follow_match_room(uint64_t room) {
    hx::rec("online::follow_match_room %llx", (unsigned long long)room);
    return true;
}
#else
void follow_match_room(uint64_t room) { hx::rec("online::follow_match_room %llx", (unsigned long long)room); }
#endif
void on_host_reached(uint32_t id) { hx::rec("online::on_host_reached %u", id); }
bool host_change_expected() {
    hx::rec("online::host_change_expected");
    return st.host_change;
}
void on_transport_closed(const char *why) { hx::rec("online::on_transport_closed [%s]", why ? why : "(NULL)"); }
void on_message(uint8_t from, const uint8_t *data, size_t size) {
    hx::rec("online::on_message %u [%s]", from, hx::ss::hex(data, size).c_str());
}
bool on_host_lost() {
    hx::rec("online::on_host_lost");
    if (st.on_host_lost_cb) st.on_host_lost_cb();
    return st.host_lost_taken;
}
std::string my_npid_text() {
    hx::rec("online::my_npid_text");
    return st.my_npid;
}
std::string npid_at(const net::Address &a) {
    hx::rec("online::npid_at %s", at(a).c_str());
    auto it = st.npid_by_ip.find(a.ip);
    return it == st.npid_by_ip.end() ? std::string() : it->second;
}
}  // namespace online

// ---- frontend3.cpp / frontend6.cpp -----------------------------------------------------------------------
namespace matchmaking {
bool installed() {
    hx::rec("matchmaking::installed");
    return st.mm_installed;
}
void leader_start() { hx::rec("matchmaking::leader_start"); }
void on_message(uint8_t from, const uint8_t *data, size_t size) {
    hx::rec("matchmaking::on_message %u [%s]", from, hx::ss::hex(data, size).c_str());
}
void on_peer_joined(uint8_t slot) { hx::rec("matchmaking::on_peer_joined %u", slot); }
void on_migrated(bool host) { hx::rec("matchmaking::on_migrated %d", host ? 1 : 0); }
}  // namespace matchmaking
namespace frontend6 {
bool leader_starting(bool mm) {
    hx::rec("frontend6::leader_starting %d", mm ? 1 : 0);
    return st.leader_starting;
}
}  // namespace frontend6

// ---- the rest ----------------------------------------------------------------------------------------------
void show_notice(const char *text) { hx::rec("show_notice [%s]", text ? text : "(NULL)"); }
void show_page(uint32_t page) { hx::rec("show_page 0x%x", page); }
void netheap_install() { hx::rec("netheap_install"); }
void *netheap_alloc(size_t size, size_t align) {
    hx::rec("netheap_alloc 0x%zx %zu", size, align);
    if (st.netheap_null) return nullptr;
    std::memset(hx::mem + st.netheap_off, 0xcd, size);   // the module must zero it
    return hx::mem + st.netheap_off;
}
void netinit_prepare() { hx::rec("netinit_prepare"); }
void netinit_match() { hx::rec("netinit_match"); }
void netinit_dump(const char *when) { hx::rec("netinit_dump [%s]", when ? when : "(NULL)"); }
int netpatch_load() {
    hx::rec("netpatch_load");
    return 0;
}
const char *extra_net_package() {
    hx::rec("extra_net_package");
    return st.extra_net ? "net-u2mp" : nullptr;
}
int32_t playlist_pick_type() {
    hx::rec("playlist_pick_type");
    return st.playlist_type;
}
bool dlc_net_game_available(int32_t i) {
    hx::rec("dlc_net_game_available %d", i);
    return st.dlc_off.count(i) == 0;
}
uint32_t unlocks_skin_fingerprint() {
    hx::rec("unlocks_skin_fingerprint");
    return st.fingerprint;
}
void progression_publish() { hx::rec("progression_publish"); }

#ifndef u2mp   // new build only: what session.cpp calls since the rewrite (skins, HUD, grenades, parity group B)
namespace grenades {
void frame(double) { hx::rec("grenades::frame"); }
}
void hud_weapon_tick() { hx::rec("hud_weapon_tick"); }
void loading_hide(const char *why) { hx::rec("loading_hide [%s]", why ? why : "(NULL)"); }
void show_round_results() { hx::rec("show_round_results"); }
bool tier2_patch_weapon_damage() {
    hx::rec("tier2_patch_weapon_damage");
    return true;
}
void unlocks_apply_slot_skins() { hx::rec("unlocks_apply_slot_skins"); }
const char *unlocks_local_skin() {
    hx::rec("unlocks_local_skin");
    return "";
}
const char *unlocks_local_villain() {
    hx::rec("unlocks_local_villain");
    return "";
}
int32_t playlist_index(uint32_t id) {
    hx::rec("playlist_index 0x%x", id);
    return -1;
}
namespace online {
std::string room_member_name(uint32_t id) {
    hx::rec("online::room_member_name %u", id);
    return {};
}
bool in_own_party(const std::string &npid) {
    hx::rec("online::in_own_party [%s]", npid.c_str());
    return true;
}
uint64_t own_party_id() {
    hx::rec("online::own_party_id");
    return 0;
}
uint64_t member_party_id(const std::string &npid) {
    hx::rec("online::member_party_id [%s]", npid.c_str());
    return 0;
}
bool peer_rtt(const std::string &npid, uint32_t &, bool &) {
    hx::rec("online::peer_rtt [%s]", npid.c_str());
    return false;
}
int mm_slots() {
    hx::rec("online::mm_slots");
    return 0;
}
// parity group D: Start Match room hide
void hide_for_start(bool on) { hx::rec("online::hide_for_start %d", on ? 1 : 0); }
// parity groups E / F (party up, slot manager, matchmaking host gone, leave matchmaking) and C (link state): the
// answers of a player alone and offline, the case the old code knew (no party room, nobody led, host is ours)
bool drop_out_of_party(const char *why) {
    hx::rec("online::drop_out_of_party [%s]", why ? why : "(NULL)");
    return false;
}
int32_t peer_link_state(const std::string &npid) {
    hx::rec("online::peer_link_state [%s]", npid.c_str());
    return -1;
}
uint64_t party_room_id() {
    hx::rec("online::party_room_id");
    return 0;
}
int party_size() {
    hx::rec("online::party_size");
    return 1;
}
bool party_up_join(uint64_t room) {
    hx::rec("online::party_up_join %llx", (unsigned long long)room);
    return false;
}
bool host_in_party() {
    hx::rec("online::host_in_party");
    return true;
}
void mm_host_gone(const char *why, bool bad_room) {
    hx::rec("online::mm_host_gone [%s] %d", why ? why : "(NULL)", bad_room ? 1 : 0);
}
std::vector<std::string> take_led_party() {
    hx::rec("online::take_led_party");
    return {};
}
void reservation_refused() { hx::rec("online::reservation_refused"); }
}  // namespace online
namespace voice {
void reset_volume() { hx::rec("voice::reset_volume"); }   // parity J6
// parity N5 / N6 (commit 50a6b69): talking indicator table and mouth patch, installed by session::install
void install(int &) { hx::rec("voice::install"); }
}  // namespace voice
// parity N2 (commit 50a6b69): Mute Players list callbacks, once per session frame
void mute_players_frame() { hx::rec("mute_players_frame"); }
// parity group E: matchmaking playlist game types (menus.cpp), menu version text (hud.cpp), stats ticker
// (ticker.cpp), Players Met at match start (progression.cpp)
bool playlist_game_types(std::vector<int32_t> &types) {
    hx::rec("playlist_game_types");
    types = st.playlist_types;
    return !types.empty();
}
void hud_menu_text(float x, float y, float scale, const char *text) {
    hx::rec("hud_menu_text %.1f %.1f %.2f [%s]", x, y, scale, text ? text : "(NULL)");
}
void ticker_refresh() { hx::rec("ticker_refresh"); }
// 2026-11-25: the leaderboard start-over at the Multiplayer button (progression.md 1.5.19) and the help bar
// shoulder icons of every menu frame (menus.cpp, MENUS.md 3.8.1; not recorded: it only rewrites menu data the
// session scenarios do not hold)
void progression_multiplayer_entered() { hx::rec("progression_multiplayer_entered"); }
void menus_help_bar_frame() {}
void progression_note_met(const std::vector<std::string> &names) {
    std::string s;
    for (const auto &n : names) s += (s.empty() ? "" : ",") + n;
    hx::rec("progression_note_met [%s]", s.c_str());
}
// parity group D: Start Match checks, teardown releases, cinema mode, start failure box
void game_message_box(uint32_t body, uint32_t second) { hx::rec("game_message_box 0x%x 0x%x", body, second); }
// since #63 (map packs) and the co-op presets: no base copy of a pack map, no co-op preset character (the scenarios
// predate both, so nothing more is asked of them)
bool dlc_net_game_has_base(int32_t) { return false; }
const char *unlocks_coop_character(int) { return nullptr; }
uint32_t dlc_packs() {
    hx::rec("dlc_packs");
    return st.dlc_packs;
}
int32_t dlc_net_game_pack(int32_t index, bool &local_ok) {
    hx::rec("dlc_net_game_pack %d", index);
    local_ok = st.dlc_off.count(index) == 0;
    const auto it = st.net_game_pack.find(index);
    return it == st.net_game_pack.end() ? 0 : it->second;
}
int32_t playlist_max_party(uint32_t id) {
    hx::rec("playlist_max_party 0x%x", id);
    return 0;
}
// AssignTeams' per-slot values (teams.cpp); not recorded: the old session.cpp has no counterpart
void teams_set_slot(int, uint32_t, uint32_t) {}
namespace cinema {
void start_playback() { hx::rec("cinema::start_playback"); }
}  // namespace cinema
namespace frontend6 {
void preload_net_game(bool request, bool keep_menu, bool flags_only) {
    hx::rec("frontend6::preload_net_game %d %d %d", request ? 1 : 0, keep_menu ? 1 : 0, flags_only ? 1 : 0);
}
}  // namespace frontend6
#endif

// ---- the entry table for test_session.cpp ---------------------------------------------------------------
void hx_session_api(hx::ss::Api &a) {
    a.install = session::install;
    a.member_at = session::member_at;
    a.member_name = session::member_name;
    a.local_id = session::local_id;
    a.is_host = session::is_host;
    a.kick = session::kick;
    a.local_name = session::local_name;
    a.rank = session::rank;
    a.member_count = session::member_count;
    a.send_message = session::send_message;
    a.net_ticks = session::net_ticks;
    a.create_match = session::create_match;
    a.member_rank = session::member_rank;
    a.online_reopen = [](const hx::ss::Addr *o) {
        net::Address owner;
        if (o) owner.ip = o->ip, owner.port = o->port;
        session::online_reopen(&fake_link, o ? &owner : nullptr);
    };
    a.online_match_ready = session::online_match_ready;
    a.set_local_name = session::set_local_name;
    a.member_address = [](uint32_t id, hx::ss::Addr &out) {
        net::Address a;
        a.ip = 0x11111111, a.port = 1111;   // untouched on false
        const bool r = session::member_address(id, a);
        out.ip = a.ip, out.port = a.port;
        return r;
    };
    a.online_migrated = session::online_migrated;
    a.online_host_gone = session::online_host_gone;
    a.match_migration_ok = session::match_migration_ok;
    a.net_id_of = session::net_id_of;
    a.online_rehost = [](const hx::ss::Addr *o, uint32_t id) {
        net::Address owner;
        if (o) owner.ip = o->ip, owner.port = o->port;
        session::online_rehost(&fake_link, o ? &owner : nullptr, id);
    };
    a.package_by_id = package_by_id;
#ifndef u2mp   // new build only: the parity entry points (online.h's u2mp::session part) and the timeout callback
    a.network_error = session::online_network_error;
    a.custom_room_slots = session::custom_room_slots;
    a.playlist_room_slots = session::playlist_room_slots;
    a.mm_pack_mask = session::mm_pack_mask;
    a.peer_timeout = [](uint8_t slot) {
        if (net::g_transport && net::g_transport->on_timeout) net::g_transport->on_timeout(slot);
        else hx::rec("(no transport timeout callback)");
    };
#endif
}

}  // namespace u2mp

// gpu/shim: the runtime's guest path translation (renamed *_old for the old build by flags_session.sh)
extern "C" int runtime_file_translate(const char *guest, char *out, size_t size) {
    hx::rec("runtime_file_translate [%s] %zu", guest ? guest : "(NULL)", size);
    if (size) std::snprintf(out, size, "%s", st.file_path.c_str());
    return st.translate_result;
}
