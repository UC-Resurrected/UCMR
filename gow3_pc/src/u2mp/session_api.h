// u2mp: the session layer's party, as other parts of the module see it (session.cpp implements it).
#pragma once
#include <cstdint>
#include <string>

namespace u2mp::session {

// The party member shown in roster row `row` (the order of the PARTY_PLAYERn rows: the host first, then the
// members on the host's team, then the others, each in slot order).
bool member_at(int row, uint32_t &net_id, std::string &name);
// The player shown in matchmaking row `row` (PLAYERn, cond 6..15); false for an open or "Found Player" slot.
bool mm_member_at(int row, uint32_t &net_id, std::string &name);
// The member with this net id (host 1, a client in transport slot s has s + 1).
bool member_name(uint32_t net_id, std::string &name);
// The member's chosen hero and villain actor packages; false when the member is unknown.
bool member_skins(uint32_t net_id, std::string &hero, std::string &villain);
// Registers/loads an actor package by name; NULL when the game cannot create it.
uint8_t *request_package(const char *name);
// This PC's net id; 0 when it is not in a party.
uint32_t local_id();
// The online layer saw the client enter NET_SM 35; the session returns it to the Multiplayer page when
// the game then jumps to state 3 without a frame in 35.
void note_leaving();
// True when this PC leads the party (it hosts the transport).
bool is_host();
// Host only (Kick From Party, PS3 0x439cf8 with the block flag): tells the member with this net id that it
// was kicked and refuses its online id on later joins until this PC stops hosting (the transport closes).
// It leaves the party, and the host drops it (NetEventPlayerLeft, roster) when its goodbye arrives. False
// when not hosting, for the host itself, or for an unknown member.
bool kick(uint32_t net_id);
// Host only, for NetEventKickGriefer: kick(). Naming this PC itself makes it leave the session, as PS3 does.
// False when not hosting or for an unknown member.
bool kick_griefer(uint32_t net_id);
// This PC's player name (UNCHARTED_U2MP_NAME, else "Drake").
std::string local_name();
int rank();   // this PC's rank badge index (profile panel)
// Players in the party, this PC included; 0 when not in one.
int member_count();
// Host: every PC of the session loads the same map for *net-games* record `index` (PS3 0x49bdb8 / Start Match: a map
// pack map only when every member has the pack; the Ruins, which a PC without map pack 1 plays as the base map, when
// every member has map pack 1 or none has). Members that sent no map-pack byte (older PCs) are not counted.
bool net_game_same_map(int32_t index);
// Online: the player with this net id is on our transport but not in our party (a stranger of a matchmaking room).
bool party_stranger(uint32_t net_id);
// The player with this net id is a member of our party (voice mute rules, spec parity-j-voice-medals.md J3): off the
// online link every member is; online only once its NpId is known and in our party room (unknown = not yet).
bool party_member(uint32_t net_id);
// A member leaves its party and hosts a party of its own (Leave Matchmaking as a member, PS3 party "leave" flag):
// online the rooms are left (online::drop_out_of_party), on LAN the transport is closed and opened again as host.
void leave_party();

}  // namespace u2mp::session
