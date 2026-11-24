// Uncharted 3 1.02, 21 Nov 2026 (diagnostic): the steps of a weapon pickup in a net game, in our log.
//
// In a net game a weapon pickup is decided by the match host. When a player presses the pickup button at a weapon on
// the map, the pickup first checks the player may take that weapon; then the player's game asks the host, which looks
// the pickup up in its table of net pickups (kept by the net game manager, one record per pickup: its process, who
// holds a claim on it, whether it is taken, and two times) and answers "approved" or "denied". An approval goes to the
// pickup, which hands the weapon to the player. On Airstrip neither the host nor a guest got power weapons (owner's
// two-PC test, 0.10.127); the same steps work on Syria. Nothing in the game code or the converted spawner data tells the
// two maps apart, so this prints each step once it happens, with the host's record of the pickup, to show which step
// stops. Every call goes on to the game's own function unchanged; only a line is printed. Found by their bytes.
#include "u3fix.h"
#include "u3fix_hook.h"

#include <climits>
#include <cstdio>
#include <cstring>

namespace u3fix {

namespace {

unsigned char *g_image = nullptr;

// image addresses of the game functions the redirected calls go on to
constexpr uint64_t kPressCheck = 0xbd26c0;    // may this player take this weapon (player, weapon definition, weapon id)
constexpr uint64_t kHasWeapon = 0xbd2440;     // the second test when the player already carries that weapon
constexpr uint64_t kAskHost = 0x6b2220;       // the player asks for the pickup (player, pickup net id)
constexpr uint64_t kRecordCheck = 0x734920;   // the record test (manager, pickup net id, player id, asked locally)
constexpr uint64_t kApproveHere = 0x710280;   // an approval for a player on this machine (the approval event)
constexpr uint64_t kSendEvent = 0x1197380;    // sends a game event to a process
constexpr uint64_t kClock = 0x17b4ca4;        // the game clock, 300 ticks a second

using PressFn = uint64_t(__attribute__((sysv_abi)) *)(void *, void *, uint64_t);
using HasFn = uint64_t(__attribute__((sysv_abi)) *)(void *, uint64_t);
using AskFn = uint64_t(__attribute__((sysv_abi)) *)(void *, uint64_t);
using CheckFn = uint64_t(__attribute__((sysv_abi)) *)(unsigned char *, uint64_t, uint64_t, uint64_t);
using ApproveFn = uint64_t(__attribute__((sysv_abi)) *)(unsigned char *, uint64_t);
using SendFn = uint64_t(__attribute__((sysv_abi)) *)(void *, void *, uint64_t, unsigned char *, void *, void *, void *, void *);

int32_t now() { return g_image ? *reinterpret_cast<const int32_t *>(g_image + kClock) : 0; }

// the same line many frames in a row is printed once, then every 300th time
bool worth_printing(uint32_t step, uint64_t a, uint64_t b) {
    static uint32_t last_step = 0;
    static uint64_t last_a = 0, last_b = 0;
    static uint32_t repeats = 0;
    if (step == last_step && a == last_a && b == last_b && ++repeats % 300 != 0) return false;
    if (step != last_step || a != last_a || b != last_b) repeats = 0;
    last_step = step, last_a = a, last_b = b;
    return true;
}

__attribute__((sysv_abi)) uint64_t press_logged(void *player, void *definition, uint64_t weapon) {
    const uint64_t r = reinterpret_cast<PressFn>(g_image + kPressCheck)(player, definition, weapon);
    if (worth_printing(1, weapon, r & 0xff)) {
        std::printf("U3 pickup: press, weapon 0x%08x, definition %s, player may take it %u\n",
                    static_cast<unsigned>(weapon), definition ? "found" : "missing", static_cast<unsigned>(r & 0xff));
        std::fflush(stdout);
    }
    return r;
}

__attribute__((sysv_abi)) uint64_t has_logged(void *player, uint64_t weapon) {
    const uint64_t r = reinterpret_cast<HasFn>(g_image + kHasWeapon)(player, weapon);
    if (worth_printing(2, weapon, r & 0xff)) {
        std::printf("U3 pickup: press, player carries weapon 0x%08x already, second test %u\n", static_cast<unsigned>(weapon),
                    static_cast<unsigned>(r & 0xff));
        std::fflush(stdout);
    }
    return r;
}

__attribute__((sysv_abi)) uint64_t ask_logged(void *player, uint64_t pickup) {
    std::printf("U3 pickup: asks for pickup 0x%08x (clock %d)\n", static_cast<unsigned>(pickup), now());
    std::fflush(stdout);
    return reinterpret_cast<AskFn>(g_image + kAskHost)(player, pickup);
}

// the manager's record of a pickup: a hash table keyed by the pickup's net id (FNV-1a of its 4 bytes)
const unsigned char *find_record(const unsigned char *manager, uint32_t pickup) {
    const unsigned char *const *buckets = *reinterpret_cast<const unsigned char *const *const *>(manager + 0x6590);
    const uint32_t count = *reinterpret_cast<const uint32_t *>(manager + 0x65cc);
    if (!buckets || !count) return nullptr;
    uint32_t h = 0x811c9dc5u;
    for (int i = 0; i < 4; i++) h = (h ^ ((pickup >> (8 * i)) & 0xff)) * 0x1000193u;
    for (const unsigned char *node = buckets[h % count]; node; node = *reinterpret_cast<const unsigned char *const *>(node))
        if (*reinterpret_cast<const uint32_t *>(node + 0x10) == pickup) return node + 0x18;
    return nullptr;
}

__attribute__((sysv_abi)) uint64_t check_logged(unsigned char *manager, uint64_t pickup, uint64_t player, uint64_t local) {
    const uint64_t r = reinterpret_cast<CheckFn>(g_image + kRecordCheck)(manager, pickup, player, local);
    const unsigned char *rec = find_record(manager, static_cast<uint32_t>(pickup));
    if (!rec) {
        std::printf("U3 pickup: %s test of pickup 0x%08x for player 0x%x: no record of it -> %s\n",
                    (local & 0xff) ? "local" : "host", static_cast<unsigned>(pickup), static_cast<unsigned>(player),
                    (r & 0xff) ? "yes" : "no");
    } else {
        const unsigned char *const *handle = *reinterpret_cast<const unsigned char *const *const *>(rec);
        const unsigned char *process = handle ? *handle : nullptr;
        const bool live = process && *reinterpret_cast<const uint32_t *>(process + 0x7c) == *reinterpret_cast<const uint32_t *>(rec + 8);
        std::printf("U3 pickup: %s test of pickup 0x%08x for player 0x%x: record %s, claim 0x%x, taken %u, claimed at %d, "
                    "asked at %d, clock %d -> %s\n",
                    (local & 0xff) ? "local" : "host", static_cast<unsigned>(pickup), static_cast<unsigned>(player),
                    live ? "with its pickup" : "without a pickup", *reinterpret_cast<const uint32_t *>(rec + 0x18),
                    static_cast<unsigned>(rec[0x1c]), *reinterpret_cast<const int32_t *>(rec + 0x10),
                    *reinterpret_cast<const int32_t *>(rec + 0x20), now(), (r & 0xff) ? "yes" : "no");
    }
    std::fflush(stdout);
    return r;
}

__attribute__((sysv_abi)) uint64_t approve_logged(unsigned char *event, uint64_t arg) {
    std::printf("U3 pickup: host approves pickup 0x%08x for its own player 0x%x\n",
                *reinterpret_cast<const uint32_t *>(event + 0x14), *reinterpret_cast<const uint32_t *>(event + 0x1c));
    std::fflush(stdout);
    return reinterpret_cast<ApproveFn>(g_image + kApproveHere)(event, arg);
}

__attribute__((sysv_abi)) uint64_t approved_logged(void *out, void *from, uint64_t id, unsigned char *to, void *a4, void *a5,
                                                   void *a6, void *a7) {
    std::printf("U3 pickup: approval reaches pickup 0x%08x (player %s)\n",
                to ? *reinterpret_cast<const uint32_t *>(to + 0x98) : 0u, from ? "found" : "missing");
    std::fflush(stdout);
    return reinterpret_cast<SendFn>(g_image + kSendEvent)(out, from, id, to, a4, a5, a6, a7);
}

__attribute__((sysv_abi)) uint64_t give_logged(void *out, void *from, uint64_t id, unsigned char *to, void *a4, void *a5, void *a6,
                                               void *a7) {
    std::printf("U3 pickup: the pickup hands its weapon to the player (player %s)\n", to ? "found" : "missing");
    std::fflush(stdout);
    return reinterpret_cast<SendFn>(g_image + kSendEvent)(out, from, id, to, a4, a5, a6, a7);
}

struct Site {
    const char *what;
    unsigned char bytes[16];   // 11 bytes before the call, then the call (e8 rel32)
    const void *handler;
};

// the calls, each found by the 11 bytes before it and its own 5 bytes
const Site kSites[] = {
    {"press check", {0x85, 0xdb, 0x07, 0x00, 0x00, 0x4c, 0x89, 0xe7, 0x44, 0x89, 0xca, 0xe8, 0xf8, 0x07, 0xe4, 0xff},
     reinterpret_cast<const void *>(&press_logged)},
    {"carried weapon test", {0x17, 0x41, 0x8b, 0xb5, 0x38, 0x03, 0x00, 0x00, 0x4c, 0x89, 0xe7, 0xe8, 0x3a, 0x05, 0xe4, 0xff},
     reinterpret_cast<const void *>(&has_logged)},
    {"ask for a pickup", {0xc0, 0x48, 0x89, 0xc7, 0x41, 0x8b, 0xb6, 0x98, 0x00, 0x00, 0x00, 0xe8, 0x13, 0x01, 0x92, 0xff},
     reinterpret_cast<const void *>(&ask_logged)},
    {"record test (local, host's own player)", {0x03, 0x00, 0x00, 0xb9, 0x01, 0x00, 0x00, 0x00, 0x44, 0x89, 0xfe, 0xe8, 0x26, 0x25, 0x08, 0x00},
     reinterpret_cast<const void *>(&check_logged)},
    {"record test (local)", {0x8b, 0x11, 0x01, 0xb9, 0x01, 0x00, 0x00, 0x00, 0x44, 0x89, 0xfe, 0xe8, 0x85, 0x24, 0x08, 0x00},
     reinterpret_cast<const void *>(&check_logged)},
    {"record test (host)", {0x8d, 0x2d, 0xba, 0xbe, 0x0b, 0x01, 0x31, 0xc9, 0x4c, 0x89, 0xef, 0xe8, 0x70, 0x57, 0x02, 0x00},
     reinterpret_cast<const void *>(&check_logged)},
    {"approval for the host's player", {0xe8, 0xf4, 0xf8, 0xe4, 0xff, 0x90, 0x31, 0xf6, 0x4c, 0x89, 0xef, 0xe8, 0x29, 0x0b, 0x00, 0x00},
     reinterpret_cast<const void *>(&approve_logged)},
    {"approval to the pickup", {0x45, 0xc8, 0x4c, 0x8d, 0x4d, 0xb0, 0xba, 0xb5, 0xe9, 0x06, 0xad, 0xe8, 0x19, 0x6f, 0xa8, 0x00},
     reinterpret_cast<const void *>(&approved_logged)},
    {"weapon to the player", {0x8d, 0x8d, 0x50, 0xfc, 0xff, 0xff, 0xba, 0x0d, 0xd4, 0xce, 0x56, 0xe8, 0xe8, 0x4c, 0x40, 0x00},
     reinterpret_cast<const void *>(&give_logged)},
};

}  // namespace

bool install_pickup_log(unsigned char *image, uint64_t size) {
    g_image = image;
    unsigned char *stub = nullptr;
    unsigned used = 0, installed = 0;
    for (const Site &s : kSites) {
        const uint64_t at = find_unique(image, size, s.bytes, sizeof s.bytes);
        if (at == kNotFound) {
            std::printf("Uncharted 3: pickup log: %s not found\n", s.what);
            continue;
        }
        if (!stub && !(stub = stub_page_near(image, size))) return false;
        // the stub: mov rax, handler; jmp rax (the guest's call already pushed its return address)
        unsigned char *entry = stub + used;
        const uint64_t handler = reinterpret_cast<uint64_t>(s.handler);
        entry[0] = 0x48; entry[1] = 0xb8;
        std::memcpy(entry + 2, &handler, 8);
        entry[10] = 0xff; entry[11] = 0xe0;
        used += 16;
        const uint64_t call = at + 11;
        const int64_t delta = static_cast<int64_t>(reinterpret_cast<uintptr_t>(entry)) -
                              static_cast<int64_t>(reinterpret_cast<uintptr_t>(image + call + 5));
        if (delta < INT32_MIN || delta > INT32_MAX) continue;
        const int32_t rel = static_cast<int32_t>(delta);
        std::memcpy(image + call + 1, &rel, 4);
        installed++;
    }
    if (stub) finish_stub(stub);
    std::printf("Uncharted 3: pickup log on, %u of %u steps\n", installed, static_cast<unsigned>(sizeof kSites / sizeof kSites[0]));
    std::fflush(stdout);
    return installed > 0;
}

}  // namespace u3fix
