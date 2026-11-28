// u2mp rewrite tests, group "frontend": the scenario state the stand-ins of deps_frontend.cpp answer from.
// One copy (namespace hx::fe), shared by the old and the new build of the stand-ins.
#pragma once
#include <cstdint>
#include <vector>

namespace hx::fe {

struct State {
    // session.cpp
    bool host = true;
    int members = 1;
    uint32_t clock = 0;                  // net_ticks(): set by the test between calls, still within one call
    bool create_clears_phase = false;    // create_match() writes netInfo+0x34 = 0
    const int *install_ref = nullptr;    // session::install: the counter it must receive
    // online.cpp
    bool select_join = false;
    bool mm_online = false;
    int mm_step = 0;
    // menus.cpp / dlc.cpp
    bool playlist = false;
    std::vector<int32_t> types;
    std::vector<int32_t> dlc_off;        // net game indexes dlc_net_game_available() refuses
    // asked only by the new code (parity D/E/F, J3, 475fba8). The defaults are what online.cpp answers on LAN (the
    // online program not started), the game the old code was written for.
    int32_t playlist_index = -1;         // menus.cpp playlist_index(): -1 = not one of ours
    bool active = false;                 // online::active() (also true while mm_online: matchmaking needs it)
    bool host_in_party = true;           // true when not online or we are the host
    bool party_member = false;
    uint64_t own_party_id = 0;
    uint64_t match_room = 0;
    int party_missing = 0;
    int mm_pending = 0;                  // online::mm_pending_members()
    // record the pure queries too (is_host, member_count, mm_online)
    bool rec_queries = true;
};
extern State s;

}  // namespace hx::fe
