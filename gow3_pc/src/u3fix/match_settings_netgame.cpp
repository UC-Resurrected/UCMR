// Uncharted 3 1.02, 21 Nov 2026: a party member takes the host's net game by name from the host's match settings.
//
// When a party starts a Custom Game together, the match host sends its match settings to the other players as a
// network event. The event's apply (image 0x71c780) selects the net game at the index the event carries (+0x18) and
// takes the rules that follow it (+0x1c). That index is the host's position in its own net game table. Each PC builds
// that table from the game data its import was made from, so two players can hold the same net games in another
// order (issue 92, 21 Nov 2026: the host's "Airstrip - No Intro" is 203 on the host and 202 on the member, whose 203
// is London Underground). The member then loaded London while the host played Airstrip, or, when the event came after
// the level was loaded, ran Airstrip with London's settings and stopped at "Failed to even find a default spawner".
//
// The multiplayer helper (v7.232 and later) writes the host's net game name id into the host's party member data
// (+0x28, checked against the 12-bit hash at +0x2e, +0x32 bit 6) and its index (+0x30). When the event's index is
// that advertised index, the member now selects its own entry with that name. Every other case keeps the event's
// index as before: no party, a host without the name (PS3, RPCS3, an older helper), another index, or no such net
// game here. The rules are always the host's. Found by its bytes.
#include "u3fix.h"
#include "u3fix_hook.h"

#include <cstdio>
#include <cstring>

namespace u3fix {

namespace {

unsigned char *g_image = nullptr;

// game data (image offsets)
constexpr uint64_t kPartySession = 0x17dc298;   // +0x18 party id, +0x12e4 our member id, +0x12e8 the host's
constexpr uint64_t kMembers = 0x680, kMemberSize = 0x120, kMemberCount = 10;   // +0x25 used, +0x2c id, +0x3c data length
constexpr uint64_t kMemberData = 0x40;
constexpr uint64_t kSymbolValue = 0x11f2bf0;     // script symbol -> pointer to its value
constexpr uint32_t kNetGames = 0x3b9a067d;      // the net game table: {s32 count, pad, entries of 0xd0, +0 name id}
constexpr uint64_t kEntry = 0xd0;

uint32_t hash12(uint32_t v) { return (v ^ (v >> 12) ^ (v >> 24)) & 0xfffu; }

template <typename T> T at(const unsigned char *p, uint64_t o) {
    T v;
    std::memcpy(&v, p + o, sizeof v);
    return v;
}

// the host's party member data block, or nullptr (no party, we are the host, no full block)
const unsigned char *host_member_data() {
    const unsigned char *ps = g_image + kPartySession;
    if (!at<uint64_t>(ps, 0x18)) return nullptr;
    const uint32_t host = at<uint32_t>(ps, 0x12e8);
    if (host == at<uint32_t>(ps, 0x12e4)) return nullptr;
    for (uint64_t i = 0; i < kMemberCount; i++) {
        const unsigned char *m = ps + kMembers + i * kMemberSize;
        if (m[0x25] && at<uint32_t>(m, 0x2c) == host && at<uint32_t>(m, 0x3c) == 0x40) return m + kMemberData;
    }
    return nullptr;
}

// the host's net game name id from its data block (0 when it carries none)
uint32_t host_net_game_name(const unsigned char *md) {
    const uint32_t name = at<uint32_t>(md, 0x28);
    const uint16_t h = at<uint16_t>(md, 0x2e);
    if (!name || !(md[0x32] & 0x40) || (h & 0xf000u) != 0xa000u) return 0;
    return hash12(name) == (h & 0xfffu) ? name : 0;
}

// this table's index of the net game with that name id, -1 when there is none or more than one
int32_t local_index(uint32_t name) {
    using SymbolValue = void **(__attribute__((sysv_abi)) *)(uint32_t);
    void **value = reinterpret_cast<SymbolValue>(g_image + kSymbolValue)(kNetGames);
    if (!value || !*value) return -1;
    const unsigned char *table = static_cast<const unsigned char *>(*value);
    const int32_t n = at<int32_t>(table, 0);
    const unsigned char *entries = at<const unsigned char *>(table, 8);
    if (!entries || n <= 0) return -1;
    int32_t found = -1;
    for (int32_t i = 0; i < n; i++) {
        if (at<uint32_t>(entries, static_cast<uint64_t>(i) * kEntry) != name) continue;
        if (found >= 0) return -1;
        found = i;
    }
    return found;
}

// the stub's call: the net game index to select for the settings event
__attribute__((sysv_abi)) int32_t match_settings_index(const unsigned char *event) {
    const int32_t sent = at<int16_t>(event, 0x18);
    const unsigned char *md = host_member_data();
    if (!md || at<int16_t>(md, 0x30) != sent) return sent;
    const uint32_t name = host_net_game_name(md);
    if (!name) return sent;
    const int32_t ours = local_index(name);
    if (ours < 0 || ours == sent) return sent;
    std::printf("Uncharted 3: the host's match settings name net game %d; it is %d here (name %08x)\n", sent, ours, name);
    std::fflush(stdout);
    return ours;
}

// mov byte [rax+0x1338],1; lea rdx,[rbx+0x1c]; movsx esi,word [rbx+0x18]; lea r15,[rip+..]
constexpr unsigned char kSite[] = {0xc6, 0x80, 0x38, 0x13, 0x00, 0x00, 0x01, 0x48, 0x8d, 0x53, 0x1c,
                                   0x0f, 0xbf, 0x73, 0x18, 0x4c, 0x8d, 0x3d};
constexpr unsigned kAt = 7;       // lea rdx
constexpr unsigned kLength = 8;   // lea rdx + movsx esi: becomes call stub + 3 nops

}  // namespace

bool install_match_settings_netgame(unsigned char *image, uint64_t size) {
    const uint64_t site = find_unique(image, size, kSite, sizeof(kSite));
    if (site == kNotFound) return false;
    unsigned char *stub = stub_page_near(image, size);
    if (!stub) return false;
    g_image = image;
    // rbx is the event (callee-saved in the game's ABI, so the call keeps it); rdi is set again after the site
    const unsigned char head[] = {0x48, 0x89, 0xdf,                      // mov rdi,rbx
                                  0x48, 0x83, 0xec, 0x08,                // sub rsp,8 (aligned for the call)
                                  0x48, 0xb8};                           // mov rax,imm64
    const unsigned char tail[] = {0xff, 0xd0,                            // call rax
                                  0x48, 0x83, 0xc4, 0x08,                // add rsp,8
                                  0x89, 0xc6,                            // mov esi,eax
                                  0x48, 0x8d, 0x53, 0x1c,                // lea rdx,[rbx+0x1c]
                                  0xc3};                                 // ret
    const uint64_t fn = reinterpret_cast<uint64_t>(&match_settings_index);
    std::memcpy(stub, head, sizeof head);
    std::memcpy(stub + sizeof head, &fn, 8);
    std::memcpy(stub + sizeof head + 8, tail, sizeof tail);
    if (!finish_stub(stub)) return false;
    unsigned char *call = image + site + kAt;
    const int64_t delta = static_cast<int64_t>(reinterpret_cast<uintptr_t>(stub)) -
                          static_cast<int64_t>(reinterpret_cast<uintptr_t>(call + 5));
    if (delta < INT32_MIN || delta > INT32_MAX) return false;
    const int32_t rel = static_cast<int32_t>(delta);
    call[0] = 0xe8;   // call stub
    std::memcpy(call + 1, &rel, 4);
    std::memset(call + 5, 0x90, kLength - 5);
    std::printf("Uncharted 3: party members take the host's match settings net game by name (%#llx)\n",
                static_cast<unsigned long long>(site + kAt));
    std::fflush(stdout);
    return true;
}

}  // namespace u3fix
