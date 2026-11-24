// u2mp matchend: watches what decides the end of a match and drives the per-frame visibility kick.
//
// The PS4 game-over checks were never seen to fire, so this wraps Playing::Update (already wrapped
// by session.cpp) to log the inputs those checks use: every 10 seconds and whenever the NetGameManager's
// state or end state changes. For test runs UNCHARTED_U2MP_TIME_LIMIT forces a short time limit.
// The EndGame and CoopTeamFailed Apply functions are hooked to log their reasons. After every update
// it calls visibility_kick() (visibility.cpp), which needs to run once per frame after the game's own update.
//
// Reimplemented from the behaviour description u2mp/docs/spec/gameplay.md.
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "u2mp_internal.h"

extern "C" int runtime_memory_is_mapped(uintptr_t address, uint64_t size);

namespace u2mp {
void install_matchend(int &installed);
void visibility_kick();

namespace {

constexpr uint64_t kUpdateSlot = 0x100ab98;
constexpr uint64_t kPlayingUpdate = 0x5f14d0;

constexpr uint64_t kNgm = 0x113b760;
constexpr uint64_t kTeams = kNgm + 0x1c9c;
constexpr uint64_t kGameIndex = kNgm + 0x1ca0;
constexpr uint64_t kRoundStart = kNgm + 0x1d5c;
constexpr uint64_t kMatchStart = kNgm + 0x1d60;
constexpr uint64_t kTaskIndex = kNgm + 0x1d6c;
constexpr uint64_t kRoundsPlayed = kNgm + 0x1d74;
constexpr uint64_t kNpcSequence = kNgm + 0x1c8c;
constexpr uint64_t kNpcTrackers = kNgm + 0x1bdc;
constexpr uint64_t kNetFlag = kNgm + 0x209c;
constexpr uint64_t kExpectedPlayers = kNgm + 0x1b90;
constexpr uint64_t kNgmState = kNgm + 0x1e80;
constexpr uint64_t kEndA = kNgm + 0x1e84;
constexpr uint64_t kEndB = kNgm + 0x1e88;
constexpr uint64_t kScoreLimit = kNgm + 0x21b4;
constexpr uint64_t kTimeLimit = kNgm + 0x21b8;
constexpr uint64_t kRoundLimit = kNgm + 0x21c0;
constexpr uint64_t kFailCount = kNgm + 0x21cc;
constexpr uint64_t kFailFlagA = kNgm + 0x21d0;
constexpr uint64_t kFailFlagB = kNgm + 0x21b0;
constexpr uint64_t kFailFlagC = kNgm + 0x21b1;

constexpr uint64_t kEndGameVtable = 0x1040d00;
constexpr uint64_t kEndGameApply = 0x62c9f0;
constexpr uint64_t kCoopFailVtable = 0x103fef0;
constexpr uint64_t kCoopFailApply = 0x61b660;

constexpr uint64_t kNetState = 0x14ce97c;
constexpr uint64_t kRealFrames = 0x1116384;
constexpr uint64_t kScoreTable = 0x104ae70;
constexpr uint64_t kDebugNoLimit = 0x104f4f8;
constexpr uint64_t kDebugMinPlayers = 0x104f52c;
constexpr uint64_t kCash = 0x11475d0;
constexpr uint64_t kLookupPointer = 0xa15e70;
constexpr uint32_t kNetGames = sid("*net-games*");
static_assert(kNetGames == 0x3b9a067du, "net-games id");

constexpr uint64_t kLogEveryMs = 10000;

template <typename T> T peek(const unsigned char *p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
template <typename T> T peek_image(uint64_t off) { return peek<T>(image.base + off); }

float test_time_limit = 0.0f;   // 0 = leave the game's limit alone
uint64_t next_log_ms = 0;
int32_t last_end_a = -1;
int32_t last_end_b = -1;
int32_t last_state = -1;
uint64_t original_update = 0;

using LookupFn = const unsigned char *(U2MP_GUEST *)(uint32_t);
using UpdateFn = uint64_t(U2MP_GUEST *)(void *);
using ApplyFn = void(U2MP_GUEST *)(void *, void *);
using CountFn = int32_t(U2MP_GUEST *)(void *);

ApplyFn original_end_game_apply = nullptr;
ApplyFn original_coop_fail_apply = nullptr;

// Current *net-games* record, null when there is none.
const unsigned char *net_game_record() {
    const unsigned char *table = reinterpret_cast<LookupFn>(image.base + kLookupPointer)(kNetGames);
    const int16_t game = peek_image<int16_t>(kGameIndex);
    if (!table || game < 0) return nullptr;
    const int32_t count = peek<int32_t>(table);
    const unsigned char *records = peek<const unsigned char *>(table + 8);
    if (game >= count || !records) return nullptr;
    return records + 0x90 * uint64_t(game);
}

// Game type of the current record, -1 when there is none.
int32_t game_type() {
    const unsigned char *record = net_game_record();
    return record ? peek<int32_t>(record + 0xc) : -1;
}

const unsigned char *current_task(int32_t &index, int32_t &count) {
    index = peek_image<int32_t>(kTaskIndex);
    count = -1;
    const unsigned char *record = net_game_record();
    const unsigned char *list = record ? peek<const unsigned char *>(record + 0x48) : nullptr;
    if (!list) return nullptr;
    count = peek<int32_t>(list + 8);
    if (index < 0 || index >= count) return nullptr;
    const unsigned char *tasks = peek<const unsigned char *>(list + 0x10);
    return tasks ? tasks + int64_t(index) * 0x28 : nullptr;
}

// Seconds since a 300 Hz start mark (the subtraction wraps like the game's 32-bit counters).
double seconds_since(uint64_t start_offset) {
    const uint32_t frames = peek_image<uint32_t>(kRealFrames) - peek_image<uint32_t>(start_offset);
    return double(int32_t(frames)) / 300.0;
}

int32_t active_players() {
    return reinterpret_cast<CountFn>(image.base + 0x63ffb0)(image.base + kNgm);
}

void arena_log(int32_t type) {
    if (type != 8 && type != 9 && type != 12) return;
    int32_t task_index = 0, task_count = 0;
    const unsigned char *task = current_task(task_index, task_count);
    const uint32_t task_id = task ? peek<uint32_t>(task) : 0;
    const uint32_t task_host = task ? peek<uint32_t>(task + 0x1c) : 0;
    const uint32_t task_script = task ? peek<uint32_t>(task + 0x20) : 0;
    log("co-op arena watch: type %d, task %d/%d id %08x host %08x script %08x, npc seq %d trackers %d netflag %d, "
        "players %d/%d, ngm state %d",
        type, task_index, task_count, task_id, task_host, task_script, peek_image<int32_t>(kNpcSequence),
        peek_image<int32_t>(kNpcTrackers), peek_image<uint8_t>(kNetFlag), active_players(),
        peek_image<int32_t>(kExpectedPlayers), peek_image<int32_t>(kNgmState));
    if (type == 12) {
        const unsigned char *ngm = image.base + kNgm;
        const int32_t handles = peek<int32_t>(ngm + 0x1dd0);
        log("siege zones: handles %d, list 0x%llx", handles, (unsigned long long)peek<uint64_t>(ngm + 0x1d80));
        const uint64_t entry = peek<uint64_t>(ngm + 0x1d80);
        if (handles > 0 && entry && entry >= 0x1000000000ull && runtime_memory_is_mapped(entry, 8)) {
            const uint64_t zone = *reinterpret_cast<const uint64_t *>(entry);
            log("siege zone: entry 0x%llx zone 0x%llx", (unsigned long long)entry, (unsigned long long)zone);
            if (zone && runtime_memory_is_mapped(zone, 0x260)) {
                log("siege zone: vtable 0x%llx id %08x team %d", (unsigned long long)(*reinterpret_cast<const uint64_t *>(zone) - uintptr_t(image.base)),
                    *reinterpret_cast<const uint32_t *>(zone + 0x44), *reinterpret_cast<const int32_t *>(zone + 0x250));
                for (uint64_t off = 0; off < 0x800; off += 8) {
                    const uint64_t p = *reinterpret_cast<const uint64_t *>(zone + off);
                    if (p < 0x1000000000ull || p >= 0x1bad00000ull || (p & 7)) continue;
                    if (!runtime_memory_is_mapped(p, 8)) continue;
                    if (*reinterpret_cast<const uint64_t *>(p) != uintptr_t(image.base) + 0xff90d0) continue;
                    log("siege zone: wave manager 0x%llx at +0x%llx", (unsigned long long)p, (unsigned long long)off);
                }
            }
        }
    }
}

void condition_log(const char *reason) {
    const int32_t type = game_type();
    const int32_t teams = peek_image<int32_t>(kTeams);
    std::string scores;
    const unsigned char *table = reinterpret_cast<const unsigned char *>(peek_image<uint64_t>(kScoreTable));
    if (!table || teams <= 0) {
        scores = "-";
    } else {
        const int shown = teams < 8 ? teams : 8;
        for (int i = 0; i < shown; ++i) {
            if (i) scores += '/';
            scores += std::to_string(peek<uint16_t>(table + 0x3dee + 0x34 * uint64_t(i)));
        }
    }
    log("match end watch (%s): type %d, %.1f s of round (match %.1f s), time limit %.0f s%s, score limit %d, "
        "teams %d scores %s, rounds %d/%d, ngm state %d end %d/%d, NET_SM %d, cash %d",
        reason, type, seconds_since(kRoundStart), seconds_since(kMatchStart), double(peek_image<float>(kTimeLimit)),
        peek_image<uint8_t>(kDebugNoLimit) ? " (debug off)" : "", peek_image<int32_t>(kScoreLimit), teams,
        scores.c_str(), peek_image<int32_t>(kRoundsPlayed), peek_image<int32_t>(kRoundLimit),
        peek_image<int32_t>(kNgmState), peek_image<int32_t>(kEndA), peek_image<int32_t>(kEndB),
        peek_image<int32_t>(kNetState), peek_image<int32_t>(kCash));
    arena_log(type);
}

void event_log(const char *name, int32_t reason, int32_t value) {
    log("match end watch: %s reason %d value %d, type %d, task %d, players %d/%d, debug min %d, ngm state %d "
        "end %d/%d, fail count %d flags %d/%d/%d",
        name, reason, value, game_type(), peek_image<int32_t>(kTaskIndex), active_players(),
        peek_image<int32_t>(kExpectedPlayers), peek_image<uint8_t>(kDebugMinPlayers), peek_image<int32_t>(kNgmState),
        peek_image<int32_t>(kEndA), peek_image<int32_t>(kEndB), peek_image<int32_t>(kFailCount),
        peek_image<uint8_t>(kFailFlagA), peek_image<uint8_t>(kFailFlagB), peek_image<uint8_t>(kFailFlagC));
}

U2MP_GUEST void end_game_apply_hook(void *ev, void *connection) {
    original_end_game_apply(ev, connection);
    const auto *p = static_cast<const unsigned char *>(ev);
    event_log("EndGame", peek<int32_t>(p + 0xc), peek<int32_t>(p + 0x10));
}

U2MP_GUEST void coop_fail_apply_hook(void *ev, void *connection) {
    original_coop_fail_apply(ev, connection);
    const auto *p = static_cast<const unsigned char *>(ev);
    event_log("CoopTeamFailed", peek<uint8_t>(p + 0xc), 0);
}

bool watch_apply(uint64_t vtable, uint64_t expected, ApplyFn hook, ApplyFn &original, const char *name) {
    if (vtable + 0x28 > image.size) return false;
    uint64_t *slot = image.at<uint64_t>(vtable + 0x20);
    const uint64_t stub = uint64_t(uintptr_t(image.base + expected));
    if (*slot != stub) {
        if (*slot)
            log("match end watch: %s vtable 0x%llx slot 4 is not 0x%llx, left alone", name,
                (unsigned long long)vtable, (unsigned long long)expected);
        return false;
    }
    original = reinterpret_cast<ApplyFn>(*slot);
    *slot = uint64_t(uintptr_t(hook));
    log("match end watch: %s Apply 0x%llx hooked", name, (unsigned long long)expected);
    return true;
}

uint64_t now_ms() {
    using namespace std::chrono;
    return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

U2MP_GUEST uint64_t update_hook(void *state) {
    if (test_time_limit > 0.0f) {
        const float current = peek_image<float>(kTimeLimit);
        if (current != test_time_limit) {
            log("match end watch: time limit %.0f s -> %.0f s (UNCHARTED_U2MP_TIME_LIMIT)", double(current),
                double(test_time_limit));
            std::memcpy(image.base + kTimeLimit, &test_time_limit, 4);
        }
    }
    const uint64_t result = reinterpret_cast<UpdateFn>(original_update)(state);
    visibility_kick();

    const uint64_t now = now_ms();
    const int32_t a = peek_image<int32_t>(kEndA);
    const int32_t b = peek_image<int32_t>(kEndB);
    const int32_t s = peek_image<int32_t>(kNgmState);
    if (a != last_end_a || b != last_end_b || s != last_state) {
        last_end_a = a;
        last_end_b = b;
        last_state = s;
        condition_log("end state changed");
        next_log_ms = now + kLogEveryMs;
    } else if (now >= next_log_ms) {
        condition_log("10 s");
        next_log_ms = now + kLogEveryMs;
    }
    return result;
}

}  // namespace

void install_matchend(int &installed) {
    if (!image.base || kUpdateSlot + 8 > image.size) return;
    if (const char *env = std::getenv("UNCHARTED_U2MP_TIME_LIMIT")) {
        const float v = std::strtof(env, nullptr);
        if (v >= 10.0f && v <= 3600.0f) test_time_limit = v;
        // Reports whether a test limit is in force now (one stored by an earlier install counts too).
        const bool used = test_time_limit > 0.0f;
        log("match end watch: UNCHARTED_U2MP_TIME_LIMIT=%s -> %s", env, used ? "used" : "ignored (10..3600)");
    }
    uint64_t *slot = image.at<uint64_t>(kUpdateSlot);
    // session.cpp wraps the slot first; we chain onto its wrapper, never onto the bare game function.
    if (*slot == 0 || *slot == uint64_t(uintptr_t(image.base + kPlayingUpdate))) {
        log("match end watch: Playing::Update slot is not wrapped by session.cpp yet, left alone");
        return;
    }
    original_update = *slot;
    *slot = uint64_t(uintptr_t(&update_hook));
    log("match end watch: Playing::Update slot 0x%llx wrapped", (unsigned long long)kUpdateSlot);
    installed += 1;
    watch_apply(kEndGameVtable, kEndGameApply, &end_game_apply_hook, original_end_game_apply, "EndGame");
    watch_apply(kCoopFailVtable, kCoopFailApply, &coop_fail_apply_hook, original_coop_fail_apply, "CoopTeamFailed");
}

}  // namespace u2mp
