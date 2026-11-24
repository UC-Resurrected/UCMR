// u2mp: net-event serializers for the event types whose Read/Write bodies the PS4 1.02 build
// reduced to a bare `ret`, plus the NpcSpawn Apply that was stripped the same way.
//
// Without these, the affected events travel the network with an empty payload and a received
// NPC spawn does nothing. Each port supplies working bodies for the PS4 payload layout and is
// hooked in by pointing the event's vtable slot at the new host function; the `ret` stub itself
// stays where it is.
//
// Reimplemented from the behaviour description u2mp/docs/spec/events.md.
#include "events.h"

#include <atomic>
#include <climits>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <mutex>

// The wire values must be bit-exact with the other peers: every float operation is rounded on
// its own, never fused.
#pragma clang fp contract(off)

namespace u2mp::events {

Game game;

namespace {

// ---------------------------------------------------------------------------------------------
// Small helpers

template <typename T> T load(const void *base, size_t off) {
    T v;
    std::memcpy(&v, static_cast<const uint8_t *>(base) + off, sizeof v);
    return v;
}
template <typename T> void store(void *base, size_t off, T v) {
    std::memcpy(static_cast<uint8_t *>(base) + off, &v, sizeof v);
}

template <typename Fn> Fn guest(uint64_t offset) { return reinterpret_cast<Fn>(image.base + offset); }

// Image facts (offsets into the PS4 1.02 image).
constexpr uint64_t kFrameClock = 0x1116384;         // s32, 300 ticks per second
constexpr uint64_t kNetGameManager = 0x113b760;
constexpr uint64_t kGameModeIndex = kNetGameManager + 0x1ca0;   // s16
constexpr uint64_t kLookupPointer = 0xa15e70;
constexpr uint32_t kGameModeTableId = 0x3b9a067d;
constexpr uint64_t kLookupProcess = 0x9bbe40;
constexpr uint64_t kSpawnerManager = 0x1130390;
constexpr uint64_t kLookupSpawner = 0x56c7b0;
constexpr uint64_t kSpawnParamsInit = 0x233600;
constexpr uint64_t kSpawnParamsSetSpawner = 0x235360;
constexpr uint64_t kSpawnerLocator = 0x4bdfe0;
constexpr uint64_t kSpawnNpc = 0x4be330;
constexpr uint64_t kAddNetNpcTracker = 0x644fa0;

constexpr size_t kPayload = 0x0c;   // first payload byte; the header before it is never touched

// Float constants of the wire encodings (exact single-precision values).
constexpr float kTimeIn = 0.006666666828095913f;        // 1/150
constexpr float kClockScale = 0.0033333334140479565f;   // 1/300
constexpr float kTimeHalfWrap = 6.826666831970215f;     // 1024/150
constexpr float kTimeWrap = 13.65333366394043f;         // 2048/150
constexpr float kTimeOut = 150.0f;
constexpr float kPosIn = 0.001953125f;                  // 2^-9
constexpr float kPosOut = 512.0f;
constexpr float kQuatIn = 0.00024420025874860585f;      // 1/4095
constexpr float kQuatOut = 4095.0f;
constexpr float kQuatPresent = 9.999999747378752e-05f;  // 1e-4

// x86 cvttss2si semantics: truncation, and the "integer indefinite" value when out of range.
int64_t to_s64(float p) {
    if (!(p >= -9223372036854775808.0f && p < 9223372036854775808.0f)) return INT64_MIN;
    return int64_t(p);
}
int32_t to_s32(float p) {
    if (!(p >= -2147483648.0f && p < 2147483648.0f)) return INT32_MIN;
    return int32_t(p);
}

// ---------------------------------------------------------------------------------------------
// Reading. Once one field does not fit, every later field is skipped; the caller then puts the
// payload back into its factory state.

struct Reader {
    BitStream &bs;
    bool ok = true;

    bool raw(uint32_t &v, unsigned n) {
        if (!ok) return false;
        ok = read_bits(bs, v, n);
        return ok;
    }
    // N bits (or a whole U32 / F32 bit pattern) stored as a u32 at `off`.
    void u32(void *ev, size_t off, unsigned n = 32) {
        uint32_t v;
        if (raw(v, n)) store<uint32_t>(ev, off, v);
    }
    void u16(void *ev, size_t off, unsigned n) {
        uint32_t v;
        if (raw(v, n)) store<uint16_t>(ev, off, uint16_t(v));
    }
    void flag(void *ev, size_t off) {
        uint32_t v;
        if (raw(v, 1)) store<uint8_t>(ev, off, v ? 1 : 0);
    }
    // 11-bit time, moved forward by whole wraps until it sits close behind the frame clock.
    void time(void *ev, size_t off) {
        uint32_t v;
        if (!raw(v, 11)) return;
        float t = float(v) * kTimeIn;
        if (game.frame_clock) {
            const float now = float(*game.frame_clock) * kClockScale;
            // One rounded addition per wrap: a closed form would round differently.
            while (now - t > kTimeHalfWrap) t = t + kTimeWrap;
        }
        store<float>(ev, off, t);
    }
    // Three 18-bit signed components; y is stored a second time in the fourth float.
    void position(void *ev, size_t off) {
        float c[3];
        for (float &f : c) {
            uint32_t v;
            if (!raw(v, 18)) return;
            const int32_t s = int32_t(v << 14) >> 14;
            f = float(s) * kPosIn;
        }
        store<float>(ev, off, c[0]);
        store<float>(ev, off + 4, c[1]);
        store<float>(ev, off + 8, c[2]);
        store<float>(ev, off + 12, c[1]);
    }
    // Position, then a quaternion whose near-zero components were left off the wire.
    void locator(void *ev, size_t off) {
        position(ev, off);
        float q[4];
        for (float &c : q) {
            uint32_t present, v = 4095;   // absent components decode to exactly 0
            if (!raw(present, 1)) return;
            if (present && !raw(v, 13)) return;
            c = float(v) * kQuatIn;
            c = c + (-1.0f);
        }
        const float s = ((q[0] * q[0] + q[1] * q[1]) + q[2] * q[2]) + q[3] * q[3];
        if (s > 0.0f) {
            const float r = 1.0f / std::sqrt(s);
            for (float &c : q) c = c * r;
        } else {
            q[0] = q[1] = q[2] = 0.0f;
            q[3] = 1.0f;
        }
        for (int i = 0; i < 4; ++i) store<float>(ev, off + 0x10 + 4 * i, q[i]);
    }
};

// Factory state of a payload: all zero, except TerritoryOwn whose +0x10 starts as all ones.
void reset_payload(void *ev, int type, uint32_t size) {
    std::memset(static_cast<uint8_t *>(ev) + kPayload, 0, size - kPayload);
    if (type == 49) store<uint32_t>(ev, 0x10, 0xffffffffu);
}

// ---------------------------------------------------------------------------------------------
// Writing. A field past maxBits is dropped by write_bits (the cursor still advances); we say so
// once per process so a too-small stream shows up in the log without flooding it.

bool overflow_reported = false;

struct Writer {
    BitStream &bs;

    void raw(uint32_t v, unsigned n) {
        if (write_bits(bs, v, n) || overflow_reported) return;
        overflow_reported = true;
        log("net event: a Write ran past the BitStream's maxBits (fields dropped)");
    }
    void u32(const void *ev, size_t off, unsigned n = 32) { raw(load<uint32_t>(ev, off), n); }
    void u16(const void *ev, size_t off, unsigned n) { raw(load<uint16_t>(ev, off), n); }
    void flag(const void *ev, size_t off) { raw(load<uint8_t>(ev, off) & 1u, 1); }
    void time(const void *ev, size_t off) {
        const float p = load<float>(ev, off) * kTimeOut;
        raw(uint32_t(uint64_t(to_s64(p))), 11);
    }
    void position(const void *ev, size_t off) {
        for (size_t i = 0; i < 3; ++i) {
            const float p = load<float>(ev, off + 4 * i) * kPosOut;
            raw(uint32_t(to_s32(p)), 18);
        }
    }
    void locator(const void *ev, size_t off) {
        position(ev, off);
        for (size_t i = 0; i < 4; ++i) {
            const float c = load<float>(ev, off + 0x10 + 4 * i);
            const bool present = std::fabs(c) > kQuatPresent;
            raw(present ? 1 : 0, 1);
            if (present) {
                const float a = c + 1.0f;
                raw(uint32_t(to_s32(a * kQuatOut)), 13);
            }
        }
    }
};

// Shared tail of every Read: on a short (or rejected) stream the payload goes back to factory state.
void finish_read(const Reader &in, void *ev, int type, uint32_t size) {
    if (!in.ok) reset_payload(ev, type, size);
}

// ---------------------------------------------------------------------------------------------
// The game-side helpers install_events plugs into `game`.

// Mode of the current game type: index from the NetGameManager into the script game-mode table.
int current_mode_from_game() {
    const uint8_t *table = guest<const uint8_t *(U2MP_GUEST *)(uint32_t)>(kLookupPointer)(kGameModeTableId);
    if (!table) return 0;
    const int16_t index = *image.at<const int16_t>(kGameModeIndex);
    if (index < 0 || index >= load<int32_t>(table, 0)) return 0;
    const uint8_t *entries = load<const uint8_t *>(table, 8);
    if (!entries) return 0;
    return load<int32_t>(entries, size_t(index) * 0x90 + 0xc);
}

void *process_by_id(uint32_t id) {
    return guest<void *(U2MP_GUEST *)(void *, uint32_t)>(kLookupProcess)(nullptr, id);
}

int current_mode() { return game.game_mode ? game.game_mode() : 0; }

// ---------------------------------------------------------------------------------------------
// Per-type Read / Write

// 37 NetEventNpcSpawn: npc id, spawner id, second spawner id, spawn tag.
U2MP_GUEST void read_npc_spawn(void *ev, BitStream *bs) {
    Reader in{*bs};
    in.u32(ev, 0x0c); in.u32(ev, 0x10); in.u32(ev, 0x14); in.u32(ev, 0x18);
    finish_read(in, ev, 37, 0x20);
}
U2MP_GUEST void write_npc_spawn(void *ev, BitStream *bs) {
    Writer out{*bs};
    out.u32(ev, 0x0c); out.u32(ev, 0x10); out.u32(ev, 0x14); out.u32(ev, 0x18);
}

// 38 NetEventNpcKill and 76 NetEventKillEntity: one u32 each.
U2MP_GUEST void read_npc_kill(void *ev, BitStream *bs) {
    Reader in{*bs};
    in.u32(ev, 0x0c);
    finish_read(in, ev, 38, 0x10);
}
U2MP_GUEST void write_one_u32(void *ev, BitStream *bs) {
    Writer out{*bs};
    out.u32(ev, 0x0c);
}
U2MP_GUEST void read_kill_entity(void *ev, BitStream *bs) {
    Reader in{*bs};
    in.u32(ev, 0x0c);
    finish_read(in, ev, 76, 0x10);
}

void apply_npc_damage(void *ev);

U2MP_GUEST void apply_npc_deal_damage(void *ev, void * /*connection*/) {
    apply_npc_damage(ev);
}

// 44 NetEventNpcDealDamage.
U2MP_GUEST void read_npc_deal_damage(void *ev, BitStream *bs) {
    Reader in{*bs};
    in.u32(ev, 0x0c, 12); in.u32(ev, 0x10); in.u32(ev, 0x14); in.time(ev, 0x18); in.u32(ev, 0x1c, 6);
    finish_read(in, ev, 44, 0x20);
}
U2MP_GUEST void write_npc_deal_damage(void *ev, BitStream *bs) {
    Writer out{*bs};
    out.u32(ev, 0x0c, 12); out.u32(ev, 0x10); out.u32(ev, 0x14); out.time(ev, 0x18); out.u32(ev, 0x1c, 6);
}

// 47 NetEventEvent: a script message with up to 15 typed arguments on the wire, 7 kept.
constexpr size_t kArgSlots = 0x38, kArgStride = 0x18, kMaxKeptArgs = 7;

U2MP_GUEST void read_event(void *ev, BitStream *bs) {
    Reader in{*bs};
    uint32_t target = 0, message = 0, sender = 0, count = 0;
    in.raw(target, 32);
    in.raw(message, 32);
    in.raw(sender, 32);
    in.raw(count, 4);
    uint64_t values[16] = {};
    uint32_t types[16] = {};
    for (uint32_t i = 0; in.ok && i < count; ++i) {
        uint32_t type = 0, raw = 0;
        in.raw(type, 4);
        in.raw(raw, 32);
        if (type == 1) {
            values[i] = raw != 0;
            types[i] = 1;
        } else if (type == 2 || type == 3 || type == 6) {
            values[i] = raw;
            types[i] = type;
        }   // anything else decodes to value 0, type 0
    }
    if (!in.ok) {
        reset_payload(ev, 47, 0x100);
        return;
    }
    store<uint32_t>(ev, 0xfc, target);
    store<uint32_t>(ev, 0xf8, sender);
    store<uint64_t>(ev, 0x10, 0);
    store<uint32_t>(ev, 0x18, message);
    uint64_t handle = 0;
    uint32_t field = 0;
    if (game.lookup_process) {
        if (const uint8_t *proc = static_cast<const uint8_t *>(game.lookup_process(sender))) {
            handle = load<uint64_t>(proc, 0x38);
            field = load<uint32_t>(proc, 0x44);
        }
    }
    store<uint64_t>(ev, 0x20, handle);
    store<uint32_t>(ev, 0x28, field);
    const uint32_t kept = count < kMaxKeptArgs ? count : uint32_t(kMaxKeptArgs);
    store<int8_t>(ev, 0x30, int8_t(kept));
    for (uint32_t i = 0; i < 8; ++i) {
        const size_t slot = kArgSlots + i * kArgStride;
        store<uint64_t>(ev, slot, i < kept ? values[i] : 0);
        store<uint64_t>(ev, slot + 8, 0);
        store<uint32_t>(ev, slot + 0x10, i < kept ? types[i] : 0);
    }
}
U2MP_GUEST void write_event(void *ev, BitStream *bs) {
    Writer out{*bs};
    out.u32(ev, 0xfc);
    out.u32(ev, 0x18);
    out.u32(ev, 0xf8);
    int n = load<int8_t>(ev, 0x30);
    if (n > 8) n = 8;
    out.raw(n < 0 ? 0 : uint32_t(n), 4);
    for (int i = 0; i < n; ++i) {
        const size_t slot = kArgSlots + size_t(i) * kArgStride;
        const uint32_t type = load<uint32_t>(ev, slot + 0x10);
        // Booleans travel as 0/1 judged by their first byte only.
        const uint32_t value = type == 1 ? (load<uint8_t>(ev, slot) != 0 ? 1u : 0u) : load<uint32_t>(ev, slot);
        out.raw(type, 4);
        out.raw(value, 32);
    }
}

// 49 NetEventTerritoryOwn: owner data plus up to 10 ids; the float exists only in game mode 12.
constexpr uint32_t kTerritorySlots = 10;
constexpr int kTerritoryFloatMode = 12;

U2MP_GUEST void read_territory_own(void *ev, BitStream *bs) {
    Reader in{*bs};
    in.u32(ev, 0x0c);
    in.u16(ev, 0x10, 16);
    in.u16(ev, 0x12, 16);
    uint32_t count = 0;
    if (in.raw(count, 4) && count > kTerritorySlots) {
        in.ok = false;   // more ids than slots: reject, the cursor stays after the count
        count = 0;
    }
    store<uint32_t>(ev, 0x14, in.ok ? count : 0);
    for (uint32_t i = 0; in.ok && i < count; ++i) in.u32(ev, 0x18 + 4 * i, 12);
    in.u32(ev, 0x40, 12);
    in.u32(ev, 0x44, 12);
    if (in.ok && current_mode() == kTerritoryFloatMode) in.u32(ev, 0x48);
    finish_read(in, ev, 49, 0x50);
}
U2MP_GUEST void write_territory_own(void *ev, BitStream *bs) {
    Writer out{*bs};
    out.u32(ev, 0x0c);
    out.u16(ev, 0x10, 16);
    out.u16(ev, 0x12, 16);
    uint32_t count = load<uint32_t>(ev, 0x14);
    if (count > kTerritorySlots) count = kTerritorySlots;
    out.raw(count, 4);
    for (uint32_t i = 0; i < count; ++i) out.u32(ev, 0x18 + 4 * i, 12);
    out.u32(ev, 0x40, 12);
    out.u32(ev, 0x44, 12);
    if (current_mode() == kTerritoryFloatMode) out.u32(ev, 0x48);
}

// 51 NetEventTerritorySpawn.
U2MP_GUEST void read_territory_spawn(void *ev, BitStream *bs) {
    Reader in{*bs};
    in.u32(ev, 0x0c); in.u32(ev, 0x10);
    finish_read(in, ev, 51, 0x18);
}
U2MP_GUEST void write_territory_spawn(void *ev, BitStream *bs) {
    Writer out{*bs};
    out.u32(ev, 0x0c); out.u32(ev, 0x10);
}

// 52 NetEventTerritorySetTimer: the trailing bit exists only when the flag is set.
U2MP_GUEST void read_territory_set_timer(void *ev, BitStream *bs) {
    Reader in{*bs};
    in.u32(ev, 0x0c);
    in.u32(ev, 0x10);
    uint32_t flag = 0;
    if (in.raw(flag, 1)) store<uint8_t>(ev, 0x14, flag ? 1 : 0);
    if (flag) in.u32(ev, 0x18, 1);
    finish_read(in, ev, 52, 0x20);
}
U2MP_GUEST void write_territory_set_timer(void *ev, BitStream *bs) {
    Writer out{*bs};
    out.u32(ev, 0x0c);
    out.u32(ev, 0x10);
    out.flag(ev, 0x14);
    // The flag goes out as its low bit, but the extra bit follows any non-zero byte (as on PS3).
    if (load<uint8_t>(ev, 0x14) != 0) out.u32(ev, 0x18, 1);
}

// 53 NetEventPlayVox: the flag picks a 12- or 32-bit sound id.
U2MP_GUEST void read_play_vox(void *ev, BitStream *bs) {
    Reader in{*bs};
    uint32_t flag = 0;
    if (in.raw(flag, 1)) store<uint8_t>(ev, 0x0c, flag ? 1 : 0);
    in.u32(ev, 0x10, flag ? 12 : 32);
    in.u32(ev, 0x14); in.u32(ev, 0x18); in.u32(ev, 0x1c); in.u32(ev, 0x20);
    finish_read(in, ev, 53, 0x28);
}
U2MP_GUEST void write_play_vox(void *ev, BitStream *bs) {
    Writer out{*bs};
    out.flag(ev, 0x0c);
    // Same split as type 52: the width follows the whole byte, the flag bit only its low bit.
    out.u32(ev, 0x10, load<uint8_t>(ev, 0x0c) != 0 ? 12 : 32);
    out.u32(ev, 0x14); out.u32(ev, 0x18); out.u32(ev, 0x1c); out.u32(ev, 0x20);
}

// 54 NetEventSimpleSnapshot.
U2MP_GUEST void read_simple_snapshot(void *ev, BitStream *bs) {
    Reader in{*bs};
    in.locator(ev, 0x10); in.u32(ev, 0x30); in.time(ev, 0x34); in.u32(ev, 0x38, 12);
    finish_read(in, ev, 54, 0x40);
}
U2MP_GUEST void write_simple_snapshot(void *ev, BitStream *bs) {
    Writer out{*bs};
    out.locator(ev, 0x10); out.u32(ev, 0x30); out.time(ev, 0x34); out.u32(ev, 0x38, 12);
}

// 55 NetEventStartNpcFight.
U2MP_GUEST void read_start_npc_fight(void *ev, BitStream *bs) {
    Reader in{*bs};
    in.locator(ev, 0x10);
    in.locator(ev, 0x30);
    in.u32(ev, 0x50, 12);
    in.u32(ev, 0x54);
    for (size_t off = 0x58; off <= 0x70; off += 4) in.u32(ev, off);   // five U32, then two F32
    in.u32(ev, 0x74, 6);
    in.flag(ev, 0x78);
    in.flag(ev, 0x79);
    finish_read(in, ev, 55, 0x80);
}
U2MP_GUEST void write_start_npc_fight(void *ev, BitStream *bs) {
    Writer out{*bs};
    out.locator(ev, 0x10);
    out.locator(ev, 0x30);
    out.u32(ev, 0x50, 12);
    out.u32(ev, 0x54);
    for (size_t off = 0x58; off <= 0x70; off += 4) out.u32(ev, off);
    out.u32(ev, 0x74, 6);
    out.flag(ev, 0x78);
    out.flag(ev, 0x79);
}

// 57 NetEventSpawnHiddenTreasure.
U2MP_GUEST void read_spawn_hidden_treasure(void *ev, BitStream *bs) {
    Reader in{*bs};
    in.position(ev, 0x10); in.u32(ev, 0x20);
    finish_read(in, ev, 57, 0x30);
}
U2MP_GUEST void write_spawn_hidden_treasure(void *ev, BitStream *bs) {
    Writer out{*bs};
    out.position(ev, 0x10); out.u32(ev, 0x20);
}

// 61 NetEventEarnMedal.
U2MP_GUEST void read_earn_medal(void *ev, BitStream *bs) {
    Reader in{*bs};
    in.u32(ev, 0x0c); in.u32(ev, 0x10); in.u32(ev, 0x14);
    finish_read(in, ev, 61, 0x18);
}
U2MP_GUEST void write_earn_medal(void *ev, BitStream *bs) {
    Writer out{*bs};
    out.u32(ev, 0x0c); out.u32(ev, 0x10); out.u32(ev, 0x14);
}

// 67 NetEventTurretSnapshot.
U2MP_GUEST void read_turret_snapshot(void *ev, BitStream *bs) {
    Reader in{*bs};
    in.u32(ev, 0x0c); in.u32(ev, 0x10); in.u32(ev, 0x14); in.time(ev, 0x18); in.u32(ev, 0x1c, 12);
    finish_read(in, ev, 67, 0x20);
}
U2MP_GUEST void write_turret_snapshot(void *ev, BitStream *bs) {
    Writer out{*bs};
    out.u32(ev, 0x0c); out.u32(ev, 0x10); out.u32(ev, 0x14); out.time(ev, 0x18); out.u32(ev, 0x1c, 12);
}


// ---------------------------------------------------------------------------------------------
// NPC ownership: the host names and sends every NPC; a joiner binds the sender's name to its own
// copy of that NPC and does not send its own despawns.

constexpr uint64_t kMatchSession = 0x1065b08;
constexpr uint64_t kNpcSendSpawn = 0x6133d0;
constexpr uint64_t kNpcSendDespawn = 0x6135a0;
constexpr uint64_t kNpcKill = 0x60eb90;
constexpr uint64_t kWeaponTable = 0xdef290;
constexpr size_t kMaxNpcs = 256;

struct LocalNpc {
    uint64_t npc;
    uint32_t id;
    uint32_t key;
    uint32_t f0;
    uint32_t f40;
    uint32_t name;
    uint8_t state;
};

LocalNpc g_local[kMaxNpcs];
unsigned g_local_next;
std::mutex g_local_lock;
std::atomic<int> g_bound{0}, g_failed{0}, g_local_logged{0};

bool npc_host() {
    const uint64_t session = uint64_t(uintptr_t(image.base)) + kMatchSession;
    const uint64_t context = load<uint64_t>(reinterpret_cast<const void *>(session), 0x10);
    if (!context) return true;
    return load<uint32_t>(reinterpret_cast<const void *>(session), 0xb00) ==
           load<uint32_t>(reinterpret_cast<const void *>(session), 0xb04);
}

bool npc_alive(uint64_t npc, uint32_t id) {
    return npc && id && load<uint32_t>(reinterpret_cast<const void *>(npc), 0x44) == id;
}

void record_local_npc(uint64_t npc, uint32_t key) {
    if (!npc) return;
    const uint32_t id = load<uint32_t>(reinterpret_cast<const void *>(npc), 0x44);
    if (!id) return;
    std::lock_guard<std::mutex> lock(g_local_lock);
    const unsigned i = g_local_next++ % kMaxNpcs;
    g_local[i] = {npc, id, key,
                  load<uint32_t>(reinterpret_cast<const void *>(npc), 0x2568),
                  load<uint32_t>(reinterpret_cast<const void *>(npc), 0x25a8),
                  load<uint32_t>(reinterpret_cast<const void *>(npc), 0x68), 1};
    if (g_local_logged.fetch_add(1) < 8) log("npc sync: local %08x key %08x", id, key);
}

bool bind_local_npc(uint64_t npc, uint32_t id, uint32_t name) {
    if (!npc_alive(npc, id)) return false;
    store<uint32_t>(reinterpret_cast<void *>(npc), 0x68, name);
    const uint32_t x = load<uint32_t>(reinterpret_cast<const void *>(npc), 0x3708);
    const uint8_t ok = guest<uint8_t(U2MP_GUEST *)(void *, void *, uint32_t)>(kAddNetNpcTracker)(
        image.base + kNetGameManager, reinterpret_cast<void *>(npc), x);
    return ok != 0;
}

bool bind_remote_npc(uint32_t name, uint32_t key, uint32_t f0, uint32_t f40) {
    if (npc_host()) return true;
    uint64_t npc = 0;
    uint32_t id = 0;
    {
        std::lock_guard<std::mutex> lock(g_local_lock);
        for (const LocalNpc &n : g_local)
            if (n.state == 2 && n.name == name) return true;
        for (unsigned k = 0; k < kMaxNpcs; ++k) {
            LocalNpc &n = g_local[(g_local_next + k) % kMaxNpcs];
            if (n.state != 1 || n.key != key) continue;
            if (!key && (n.f0 != f0 || n.f40 != f40)) continue;
            if (!npc_alive(n.npc, n.id)) {
                n.state = 0;
                continue;
            }
            npc = n.npc;
            id = n.id;
            n.state = 2;
            n.name = name;
            break;
        }
    }
    if (!npc) {
        if (g_failed.fetch_add(1) < 8) log("npc sync: no local npc for %08x, spawning it", name);
        return false;
    }
    if (!bind_local_npc(npc, id, name)) {
        g_failed.fetch_add(1);
        return true;
    }
    if (g_bound.fetch_add(1) < 8) log("npc sync: bound %08x to a local npc", name);
    return true;
}

using NpcSendSpawnFn = void(U2MP_GUEST *)(void *, void *, void *);
using NpcSendDespawnFn = void(U2MP_GUEST *)(uint32_t);

U2MP_GUEST void local_npc_init(void *info, void *params, void *npc) {
    guest<NpcSendSpawnFn>(kNpcSendSpawn)(info, params, npc);
    if (npc_host() || !npc) return;
    const uint64_t spawner = info ? load<uint64_t>(info, 0x40) : 0;
    const uint32_t key = spawner ? load<uint32_t>(reinterpret_cast<const void *>(spawner), 0x28) : 0;
    record_local_npc(uint64_t(uintptr_t(npc)), key);
}

U2MP_GUEST void local_npc_despawn(uint32_t name) {
    if (npc_host()) {
        guest<NpcSendDespawnFn>(kNpcSendDespawn)(name);
        return;
    }
    std::lock_guard<std::mutex> lock(g_local_lock);
    for (LocalNpc &n : g_local)
        if (n.state && n.npc && load<uint32_t>(reinterpret_cast<const void *>(n.npc), 0x68) == name) n.state = 0;
}

uint64_t npc_tracker(uint32_t name) {
    const uint64_t ngm = uint64_t(uintptr_t(image.base)) + kNetGameManager;
    const uint64_t buckets = load<uint64_t>(reinterpret_cast<const void *>(ngm), 0x1ba0);
    const int32_t count = load<int32_t>(reinterpret_cast<const void *>(ngm), 0x1bdc);
    if (!buckets || count <= 0) return 0;
    uint32_t h = 0x811c9dc5u;
    for (int i = 0; i < 4; ++i) h = (h ^ ((name >> (8 * i)) & 0xffu)) * 0x1000193u;
    uint64_t node = load<uint64_t>(reinterpret_cast<const void *>(buckets + 8 * (h % uint32_t(count))), 0);
    const uint64_t end = load<uint64_t>(reinterpret_cast<const void *>(buckets + 8 * uint64_t(count)), 0);
    for (int guard = 0; node && node != end && guard < 4096; ++guard) {
        if (load<uint32_t>(reinterpret_cast<const void *>(node), 0x10) == name) return node + 0x18;
        node = load<uint64_t>(reinterpret_cast<const void *>(node), 0);
    }
    return 0;
}

uint64_t player_process(int32_t id) {
    const uint64_t ngm = uint64_t(uintptr_t(image.base)) + kNetGameManager;
    for (int k = 0; k < 10; ++k) {
        const uint64_t slot = ngm + 0x10 + uint64_t(k) * 0x2c0;
        if (!load<uint8_t>(reinterpret_cast<const void *>(slot), 0x20) ||
            load<int32_t>(reinterpret_cast<const void *>(slot), 0x120) != id)
            continue;
        const uint64_t handle = load<uint64_t>(reinterpret_cast<const void *>(slot), 0x10);
        const uint32_t pid = load<uint32_t>(reinterpret_cast<const void *>(slot), 0x18);
        if (!handle || !pid) return 0;
        const uint64_t process = load<uint64_t>(reinterpret_cast<const void *>(handle), 0);
        if (!process || load<uint32_t>(reinterpret_cast<const void *>(process), 0x44) != pid) return 0;
        return process;
    }
    return 0;
}

void apply_npc_damage(void *ev) {
    const int32_t id = load<int32_t>(ev, 0x0c);
    const uint32_t name = load<uint32_t>(ev, 0x10);
    const int32_t damage = load<int32_t>(ev, 0x14);
    const float at = load<float>(ev, 0x18);
    const uint32_t index = load<uint32_t>(ev, 0x1c);
    const uint64_t player = player_process(id);
    if (!player) return;
    const uint64_t tracker = npc_tracker(name);
    if (!tracker) return;
    const uint64_t handle = load<uint64_t>(reinterpret_cast<const void *>(tracker), 0);
    const uint32_t pid = load<uint32_t>(reinterpret_cast<const void *>(tracker), 8);
    const uint64_t npc = handle ? load<uint64_t>(reinterpret_cast<const void *>(handle), 0) : 0;
    if (!npc || !pid || load<uint32_t>(reinterpret_cast<const void *>(npc), 0x44) != pid) return;
    const uint64_t vt = load<uint64_t>(reinterpret_cast<const void *>(npc), 0);
    const uint64_t is_dead = vt ? load<uint64_t>(reinterpret_cast<const void *>(vt), 0x250) : 0;
    const uint64_t take = vt ? load<uint64_t>(reinterpret_cast<const void *>(vt), 0x370) : 0;
    if (!is_dead || !take) return;
    using IsDeadFn = uint8_t(U2MP_GUEST *)(uint64_t);
    using DamageFn = void(U2MP_GUEST *)(uint64_t, int32_t);
    if (reinterpret_cast<IsDeadFn>(is_dead)(npc)) return;
    reinterpret_cast<DamageFn>(take)(npc, damage);
    if (!reinterpret_cast<IsDeadFn>(is_dead)(npc) || !npc_host()) return;
    uint32_t weapon = 0;
    if (index < 46)
        weapon = load<uint32_t>(reinterpret_cast<const void *>(uintptr_t(image.base) + kWeaponTable + 4 * uint64_t(index)), 0);
    using KillFn = void(U2MP_GUEST *)(uint32_t, uint64_t, uint64_t, uint32_t *, uint32_t, uint32_t, uint32_t);
    uint32_t time = at == at && at > 0.0f && at < 1.0e7f ? uint32_t(int32_t(at * 300.0f)) : 0;
    guest<KillFn>(kNpcKill)(weapon, player, npc, &time, 0, 0, 0);
    uint8_t flags = load<uint8_t>(reinterpret_cast<const void *>(npc), 0x262a);
    store<uint8_t>(reinterpret_cast<void *>(npc), 0x262a, uint8_t(flags | 2));
}

// ---------------------------------------------------------------------------------------------
// 37 NetEventNpcSpawn Apply: spawn the npc the sender described and register it for the net.

U2MP_GUEST void apply_npc_spawn(void *ev, void * /*connection*/) {
    const uint32_t npc_id = load<uint32_t>(ev, 0x0c);
    const uint32_t spawner_id = load<uint32_t>(ev, 0x10);
    const uint32_t second_id = load<uint32_t>(ev, 0x14);
    const uint32_t tag = load<uint32_t>(ev, 0x18);
    if (bind_remote_npc(npc_id, spawner_id, second_id, tag)) return;
    void *const manager = image.base + kSpawnerManager;
    const auto lookup_spawner = guest<uint8_t *(U2MP_GUEST *)(void *, uint32_t)>(kLookupSpawner);

    if (game.lookup_process && game.lookup_process(npc_id)) {
        log("NetEventNpcSpawn: npc %u already exists", npc_id);
        return;
    }
    uint8_t *const spawner = lookup_spawner(manager, spawner_id);
    uint8_t *const second = second_id ? lookup_spawner(manager, second_id) : nullptr;
    if (!spawner) {
        log("NetEventNpcSpawn: can't find spawner %08x", spawner_id);
        return;
    }

    alignas(16) uint8_t info[0x60] = {};
    store<uint32_t>(info, 0x00, load<uint32_t>(spawner, 0x40));
    store<uint64_t>(info, 0x10, uint64_t(uintptr_t(spawner)));
    store<uint64_t>(info, 0x40, uint64_t(uintptr_t(spawner)));

    alignas(16) uint8_t params[0xb0] = {};
    guest<void(U2MP_GUEST *)(void *, void *)>(kSpawnParamsInit)(params, info);
    store<uint32_t>(params, 0x00, second_id);
    // The game looks the second spawner up again here, whether or not the id is 0.
    if (uint8_t *s = lookup_spawner(manager, second_id))
        guest<void(U2MP_GUEST *)(void *, void *)>(kSpawnParamsSetSpawner)(params, s);
    store<uint32_t>(params, 0x40, tag);

    alignas(16) uint8_t locator[0x40] = {};
    void *where = nullptr;
    if (second) {
        guest<void(U2MP_GUEST *)(void *, void *)>(kSpawnerLocator)(locator, second);
        where = locator;
    }
    uint8_t *const npc =
        guest<uint8_t *(U2MP_GUEST *)(void *, void *, void *, uint64_t)>(kSpawnNpc)(spawner, params, where, 0);
    if (!npc) {
        log("NetEventNpcSpawn: unable to spawn npc from spawner %08x", spawner_id);
        return;
    }
    store<uint32_t>(npc, 0x68, npc_id);
    // AddNetNpcTracker returns a bool: only its low byte is meaningful.
    const uint8_t added = guest<uint8_t(U2MP_GUEST *)(void *, void *, uint32_t)>(kAddNetNpcTracker)(
        image.base + kNetGameManager, npc, load<uint32_t>(npc, 0x3708));
    if (!added) log("NetEventNpcSpawn: failed to add npc %u to the NetGameManager", npc_id);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// The port table (its order is the install log order).

const Port kPorts[] = {
    {37, "NetEventNpcSpawn", 0x1040f30, 0x20, 0x62f240, 0x62f230, 0x62f220, read_npc_spawn, write_npc_spawn, apply_npc_spawn},
    {38, "NetEventNpcKill", 0x1040760, 0x10, 0x622080, 0x622070, 0, read_npc_kill, write_one_u32, nullptr},
    {44, "NetEventNpcDealDamage", 0x10405d0, 0x20, 0x61fe00, 0, 0, read_npc_deal_damage, write_npc_deal_damage, apply_npc_deal_damage},
    {47, "NetEventEvent", 0x1040580, 0x100, 0x61fa60, 0x61fa50, 0, read_event, write_event, nullptr},
    {49, "NetEventTerritoryOwn", 0x1040440, 0x50, 0x61f030, 0x61f020, 0, read_territory_own, write_territory_own, nullptr},
    {51, "NetEventTerritorySpawn", 0x10403a0, 0x18, 0x61e890, 0x61e880, 0, read_territory_spawn, write_territory_spawn, nullptr},
    {52, "NetEventTerritorySetTimer", 0x1040350, 0x20, 0x61e700, 0x61e6f0, 0, read_territory_set_timer, write_territory_set_timer, nullptr},
    {53, "NetEventPlayVox", 0x1040300, 0x28, 0x61e600, 0x61e5f0, 0, read_play_vox, write_play_vox, nullptr},
    {54, "NetEventSimpleSnapshot", 0x10402b0, 0x40, 0x61e2a0, 0x61e290, 0, read_simple_snapshot, write_simple_snapshot, nullptr},
    {55, "NetEventStartNpcFight", 0x1040260, 0x80, 0x61e1b0, 0x61e1a0, 0, read_start_npc_fight, write_start_npc_fight, nullptr},
    {57, "NetEventSpawnHiddenTreasure", 0x10401c0, 0x30, 0x61d3a0, 0x61d390, 0, read_spawn_hidden_treasure, write_spawn_hidden_treasure, nullptr},
    {61, "NetEventEarnMedal", 0x1040120, 0x18, 0x61d240, 0x61d230, 0, read_earn_medal, write_earn_medal, nullptr},
    {67, "NetEventTurretSnapshot", 0x1040030, 0x20, 0x61bee0, 0x61bed0, 0, read_turret_snapshot, write_turret_snapshot, nullptr},
    {76, "NetEventKillEntity", 0x103fdb0, 0x10, 0x61afd0, 0x61afc0, 0, read_kill_entity, write_one_u32, nullptr},
};
const size_t kPortCount = sizeof kPorts / sizeof kPorts[0];



// Type 13 AttackProjectile: a hit the shooter decided. Log the first few so a missing hit can be
// told apart from a hit that arrives with zero damage.
SerializeFn attack_read_game, attack_write_game;
void(U2MP_GUEST *attack_apply_game)(void *, void *);
unsigned attack_logs;

void log_attack(const char *when, const void *ev) {
    if (++attack_logs > 12) return;
    log("net attack %s: flags %08x attacker %u target %u damage %d", when, load<uint32_t>(ev, 0x0c),
        load<uint32_t>(ev, 0x10), load<uint32_t>(ev, 0x14), load<int32_t>(ev, 0x1c));
}
U2MP_GUEST void read_attack(void *ev, BitStream *bs) {
    attack_read_game(ev, bs);
    log_attack("read", ev);
}
U2MP_GUEST void write_attack(void *ev, BitStream *bs) {
    log_attack("write", ev);
    attack_write_game(ev, bs);
}
U2MP_GUEST void apply_attack(void *ev, void *conn) {
    log_attack("apply", ev);
    attack_apply_game(ev, conn);
}

bool install_attack_log() {
    constexpr uint64_t vtable = 0x1040b70;
    struct Slot { unsigned slot; uint64_t game; void *fn; };
    const Slot slots[] = {
        {2, 0x628be0, reinterpret_cast<void *>(&read_attack)},
        {3, 0x628670, reinterpret_cast<void *>(&write_attack)},
        {4, 0x627da0, reinterpret_cast<void *>(&apply_attack)},
    };
    for (const Slot &s : slots)
        if (*image.at<uint64_t>(vtable + 8 * s.slot) != uint64_t(uintptr_t(image.base + s.game))) return false;
    attack_read_game = reinterpret_cast<SerializeFn>(*image.at<uint64_t>(vtable + 16));
    attack_write_game = reinterpret_cast<SerializeFn>(*image.at<uint64_t>(vtable + 24));
    attack_apply_game = reinterpret_cast<void(U2MP_GUEST *)(void *, void *)>(*image.at<uint64_t>(vtable + 32));
    *image.at<uint64_t>(vtable + 16) = uint64_t(uintptr_t(&read_attack));
    *image.at<uint64_t>(vtable + 24) = uint64_t(uintptr_t(&write_attack));
    *image.at<uint64_t>(vtable + 32) = uint64_t(uintptr_t(&apply_attack));
    log("net attack: AttackProjectile Read/Write/Apply wrapped for logging");
    return true;
}

// Carry-object pickup/drop (types 33 and 34): log the first few so a missing state change can be
// seen when a propane tank is picked up or dropped.
SerializeFn carry33_read_game, carry33_write_game, carry34_read_game, carry34_write_game;
void(U2MP_GUEST *carry33_apply_game)(void *, void *);
void(U2MP_GUEST *carry34_apply_game)(void *, void *);
unsigned carry_logs;

void log_carry(const char *name, const char *when, const void *ev) {
    if (++carry_logs > 16) return;
    log("net carry %s %s: %08x %08x %08x", name, when, load<uint32_t>(ev, 0x0c),
        load<uint32_t>(ev, 0x10), load<uint32_t>(ev, 0x14));
}
U2MP_GUEST void read_carry33(void *ev, BitStream *bs) { carry33_read_game(ev, bs); log_carry("33", "read", ev); }
U2MP_GUEST void write_carry33(void *ev, BitStream *bs) { log_carry("33", "write", ev); carry33_write_game(ev, bs); }
U2MP_GUEST void apply_carry33(void *ev, void *c) { log_carry("33", "apply", ev); carry33_apply_game(ev, c); }
U2MP_GUEST void read_carry34(void *ev, BitStream *bs) { carry34_read_game(ev, bs); log_carry("34", "read", ev); }
U2MP_GUEST void write_carry34(void *ev, BitStream *bs) { log_carry("34", "write", ev); carry34_write_game(ev, bs); }
U2MP_GUEST void apply_carry34(void *ev, void *c) { log_carry("34", "apply", ev); carry34_apply_game(ev, c); }

bool install_carry_log() {
    struct Set {
        uint64_t vtable;
        uint64_t read, write, apply;
        SerializeFn *read_orig, *write_orig;
        void(U2MP_GUEST **apply_orig)(void *, void *);
        SerializeFn read_fn, write_fn;
        void(U2MP_GUEST *apply_fn)(void *, void *);
    };
    Set sets[] = {
        {0x1040800, 0x622bb0, 0x622940, 0x622760, &carry33_read_game, &carry33_write_game, &carry33_apply_game,
         &read_carry33, &write_carry33, &apply_carry33},
        {0x1040fd0, 0x62fc10, 0x62fb50, 0x62f9c0, &carry34_read_game, &carry34_write_game, &carry34_apply_game,
         &read_carry34, &write_carry34, &apply_carry34},
    };
    int done = 0;
    for (Set &set : sets) {
        if (*image.at<uint64_t>(set.vtable + 16) != uint64_t(uintptr_t(image.base + set.read)) ||
            *image.at<uint64_t>(set.vtable + 24) != uint64_t(uintptr_t(image.base + set.write)) ||
            *image.at<uint64_t>(set.vtable + 32) != uint64_t(uintptr_t(image.base + set.apply)))
            continue;
        *set.read_orig = reinterpret_cast<SerializeFn>(*image.at<uint64_t>(set.vtable + 16));
        *set.write_orig = reinterpret_cast<SerializeFn>(*image.at<uint64_t>(set.vtable + 24));
        *set.apply_orig = reinterpret_cast<void(U2MP_GUEST *)(void *, void *)>(*image.at<uint64_t>(set.vtable + 32));
        *image.at<uint64_t>(set.vtable + 16) = uint64_t(uintptr_t(set.read_fn));
        *image.at<uint64_t>(set.vtable + 24) = uint64_t(uintptr_t(set.write_fn));
        *image.at<uint64_t>(set.vtable + 32) = uint64_t(uintptr_t(set.apply_fn));
        ++done;
    }
    if (done) log("net carry: %d carry-object event sets wrapped for logging", done);
    return done > 0;
}
// ---------------------------------------------------------------------------------------------
// 8 NetEventEmitGrenade and 9 NetEventGrenadeExplode keep fields out of their own codecs although
// their Execute bodies use them: type 8's +0x40 projectile settings and type 9's +0x14 attack,
// +0x18 explosion settings and +0x170 float. Append them after the game's fields, as 32-bit words.

using Serialize = void(U2MP_GUEST *)(void *, BitStream *);
Serialize emit_grenade_read_game, emit_grenade_write_game;
Serialize grenade_explode_read_game, grenade_explode_write_game;
Serialize emit_rocket_read_game, emit_rocket_write_game;
using Apply = void(U2MP_GUEST *)(void *, void *);
Apply emit_rocket_apply_game;

// The extra words must not push the cursor past maxBits: write_bits advances even when the
// field does not fit, while read_bits leaves the cursor alone. Skipping the write here keeps
// both sides on the same bit offset.
bool has_room(const BitStream &bs, unsigned bits) {
    return bs.bits <= bs.max_bits && bits <= bs.max_bits - bs.bits;
}
void append_u32(BitStream &bs, uint32_t v) {
    if (!has_room(bs, 32)) return;
    write_bits(bs, v, 32);
}

U2MP_GUEST void read_emit_grenade(void *ev, BitStream *bs) {
    emit_grenade_read_game(ev, bs);
    uint32_t v = 0;
    if (read_bits(*bs, v, 32)) store<uint32_t>(ev, 0x40, v);
}
U2MP_GUEST void write_emit_grenade(void *ev, BitStream *bs) {
    emit_grenade_write_game(ev, bs);
    append_u32(*bs, load<uint32_t>(ev, 0x40));
}
U2MP_GUEST void read_grenade_explode(void *ev, BitStream *bs) {
    grenade_explode_read_game(ev, bs);
    uint32_t v = 0;
    if (read_bits(*bs, v, 32)) store<uint32_t>(ev, 0x14, v);
    if (read_bits(*bs, v, 32)) store<uint32_t>(ev, 0x18, v);
    if (read_bits(*bs, v, 32)) store<uint32_t>(ev, 0x170, v);
}
U2MP_GUEST void write_grenade_explode(void *ev, BitStream *bs) {
    grenade_explode_write_game(ev, bs);
    append_u32(*bs, load<uint32_t>(ev, 0x14));
    append_u32(*bs, load<uint32_t>(ev, 0x18));
    append_u32(*bs, load<uint32_t>(ev, 0x170));
}

// 11 NetEventEmitRocket: the rocket's settings id at +0x50. A rocket whose settings are not
// loaded here cannot be spawned; the game asserts in projectile Init and then faults.
bool settings_loaded(uint32_t id) {
    if (!id) return false;
    auto find = guest<void *(U2MP_GUEST *)(uint32_t)>(0xa15300);
    void *slot = find(id);
    void *object = slot ? load<void *>(slot, 0) : nullptr;
    return object && load<void *>(object, 0xb0);
}
U2MP_GUEST void read_emit_rocket(void *ev, BitStream *bs) {
    emit_rocket_read_game(ev, bs);
    uint32_t v = 0;
    if (read_bits(*bs, v, 32)) store<uint32_t>(ev, 0x50, v);
}
U2MP_GUEST void write_emit_rocket(void *ev, BitStream *bs) {
    emit_rocket_write_game(ev, bs);
    append_u32(*bs, load<uint32_t>(ev, 0x50));
}
U2MP_GUEST void apply_emit_rocket(void *ev, void *conn) {
    const uint32_t id = load<uint32_t>(ev, 0x50);
    if (!settings_loaded(id)) {
        static uint32_t told[8];
        static unsigned count;
        unsigned i = 0;
        while (i < count && told[i] != id) ++i;
        if (i == count && count < 8) {
            told[count++] = id;
            log("net rocket: settings %08x not loaded, left out", id);
        }
        return;
    }
    emit_rocket_apply_game(ev, conn);
}
bool install_rocket_guard() {
    constexpr uint64_t vtable = 0x1041390, apply = 0x63a7c0;
    if (vtable + 0x28 > image.size) return false;
    uint64_t *entry = image.at<uint64_t>(vtable + 0x20);
    if (*entry != uint64_t(uintptr_t(image.base + apply))) {
        log("net rocket: Apply expected 0x%llx, left alone", static_cast<unsigned long long>(apply));
        return false;
    }
    emit_rocket_apply_game = reinterpret_cast<Apply>(*entry);
    *entry = uint64_t(uintptr_t(&apply_emit_rocket));
    log("net rocket: Apply guarded (EmitRocket +0x50)");
    return true;
}

bool install_extra_fields() {
    struct Extra {
        uint64_t vtable;
        unsigned slot;
        uint64_t game;
        Serialize *orig;
        Serialize fn;
        const char *what;
    };
    const Extra extras[] = {
        {0x1040c60, 2, 0x62b650, &emit_grenade_read_game, &read_emit_grenade, "EmitGrenade +0x40 read"},
        {0x1040c60, 3, 0x62b250, &emit_grenade_write_game, &write_emit_grenade, "EmitGrenade +0x40 write"},
        {0x1040c10, 2, 0x62a780, &grenade_explode_read_game, &read_grenade_explode, "GrenadeExplode +0x14/+0x18/+0x170 read"},
        {0x1040c10, 3, 0x62a020, &grenade_explode_write_game, &write_grenade_explode, "GrenadeExplode +0x14/+0x18/+0x170 write"},
        {0x1041390, 2, 0x63b050, &emit_rocket_read_game, &read_emit_rocket, "EmitRocket +0x50 read"},
        {0x1041390, 3, 0x63ad40, &emit_rocket_write_game, &write_emit_rocket, "EmitRocket +0x50 write"},
    };
    int done = 0;
    for (const Extra &e : extras) {
        if (e.vtable + 8 * e.slot + 8 > image.size) continue;
        uint64_t *entry = image.at<uint64_t>(e.vtable + 8 * e.slot);
        if (*entry != uint64_t(uintptr_t(image.base + e.game))) {
            log("net events: %s: expected 0x%llx, left alone", e.what,
                static_cast<unsigned long long>(e.game));
            continue;
        }
        *e.orig = reinterpret_cast<Serialize>(*entry);
        *entry = uint64_t(uintptr_t(e.fn));
        log("net events: %s", e.what);
        ++done;
    }
    return done > 0;
}
// ---------------------------------------------------------------------------------------------
// Message 0x2e's Read (0x62de90) takes its byte buffer from the thread's current allocator scope.
// At the net pump that scope can hold a null entry, and the Read dereferences it at 0x62dfc2 before
// any fallback. Guard that lookup and, when it has nothing, take the buffer from the Network heap
// (context 0x14) so the event's destructor can free it with the game's own free.
static int message_fallbacks;

U2MP_GUEST uint64_t message_buffer(uint64_t size) {
    static const uint32_t context[4] = {0x14};
    using Alloc = uint64_t(U2MP_GUEST *)(uint64_t, const uint32_t *, uint64_t);
    const uint64_t p = guest<Alloc>(0xb332a0)(size, context, 0x10);
    if (++message_fallbacks <= 4) {
        log("net Message: 0x62dfc2 allocator entry is null; its %u bytes from the Network heap (%s)",
            static_cast<unsigned>(uint32_t(size)), p ? "taken" : "NONE");
    }
    return p;
}

static bool install_message_read() {
    constexpr uint64_t kEntry = 0x62dfc2, kResume = 0x62dfdf;
    static const unsigned char want[5] = {0x48, 0x8b, 0x07, 0x48, 0x8d};   // mov rax,[rdi]; lea rcx,...
    if (!image.executable(kEntry, 5)) return false;
    if (!image.matches(kEntry, want, sizeof want)) return image.base[kEntry] == 0xe9;
    unsigned char *s = near_stub(128);
    if (!s) return false;
    const uint64_t stub = uint64_t(uintptr_t(s));
    size_t n = 0;
    auto bytes = [&](std::initializer_list<unsigned char> b) { for (unsigned char c : b) s[n++] = c; };
    auto rel = [&](size_t at, uint64_t target) {
        const int64_t d = int64_t(target) - int64_t(stub + at + 4);
        if (d < INT32_MIN || d > INT32_MAX) return false;
        const int32_t r = int32_t(d);
        std::memcpy(s + at, &r, 4);
        return true;
    };
    bytes({0x48, 0x85, 0xff});                     // test rdi,rdi
    bytes({0x0f, 0x84, 0, 0, 0, 0});               // jz fallback
    const size_t jz_rdi = n - 4;
    bytes({0x48, 0x8b, 0x07});                     // mov rax,[rdi]
    bytes({0x48, 0x85, 0xc0});                     // test rax,rax
    bytes({0x0f, 0x84, 0, 0, 0, 0});               // jz fallback
    const size_t jz_vt = n - 4;
    bytes({0x48, 0x8d, 0x0d, 0, 0, 0, 0});         // lea rcx,[rip+...] (0xeaca4d)
    const size_t lea_at = n - 4;
    bytes({0xba, 0x10, 0x00, 0x00, 0x00});         // mov edx,0x10
    bytes({0x45, 0x31, 0xc0});                     // xor r8d,r8d
    bytes({0x89, 0xde});                           // mov esi,ebx
    bytes({0x49, 0x89, 0xc9});                     // mov r9,rcx
    bytes({0xff, 0x50, 0x48});                     // call [rax+0x48]
    bytes({0x48, 0x89, 0xc6});                     // mov rsi,rax
    bytes({0xe9, 0, 0, 0, 0});                     // jmp kResume
    const size_t jmp_ok = n - 4;
    const size_t fallback = n;
    bytes({0x89, 0xdf});                           // mov edi,ebx
    bytes({0x48, 0xb8});                           // mov rax, message_buffer
    const uint64_t fn = uint64_t(uintptr_t(&message_buffer));
    std::memcpy(s + n, &fn, 8); n += 8;
    bytes({0xff, 0xd0});                           // call rax
    bytes({0x48, 0x89, 0xc6});                     // mov rsi,rax
    bytes({0xe9, 0, 0, 0, 0});                     // jmp kResume
    const size_t jmp_fb = n - 4;
    if (n > 128) return false;
    const uint64_t back = uint64_t(uintptr_t(image.base + kResume));
    if (!rel(lea_at, uint64_t(uintptr_t(image.base + 0xeaca4d))) || !rel(jmp_ok, back) || !rel(jmp_fb, back)) return false;
    const size_t jz_target = fallback;
    const int32_t jz1 = int32_t(jz_target - (jz_rdi + 4)), jz2 = int32_t(jz_target - (jz_vt + 4));
    std::memcpy(s + jz_rdi, &jz1, 4);
    std::memcpy(s + jz_vt, &jz2, 4);
    const int64_t delta = int64_t(stub) - int64_t(uintptr_t(image.base + kEntry + 5));
    if (delta < INT32_MIN || delta > INT32_MAX) return false;
    unsigned char patch[5] = {0xe9, 0, 0, 0, 0};
    const int32_t relative = int32_t(delta);
    std::memcpy(patch + 1, &relative, 4);
    std::memcpy(image.base + kEntry, patch, sizeof patch);
    log("net Message: 0x62dfc2 read guard installed");
    return true;
}

// ---------------------------------------------------------------------------------------------
// Install: point every stubbed vtable slot at its port, but only where the slot still holds the
// expected `ret` stub (anything else means the image is not what we think, so leave it alone).

void install_npcsync(int &installed) {
    if (retarget_call(0x23bb4c, kNpcSendSpawn, reinterpret_cast<void *>(&local_npc_init), "npc local init"))
        ++installed;
    if (retarget_call(0x240fd2, kNpcSendDespawn, reinterpret_cast<void *>(&local_npc_despawn), "npc despawn owner"))
        ++installed;
}

void install_events(int &installed) {
    if (!image.base || kFrameClock + 4 > image.size) return;
    game.frame_clock = image.at<const int32_t>(kFrameClock);
    game.game_mode = current_mode_from_game;
    game.lookup_process = process_by_id;

    int ported = 0;
    for (const Port &p : kPorts) {
        if (p.vtable + 0x28 > image.size) continue;
        const struct {
            unsigned slot;
            uint64_t stub;
            void *fn;
        } slots[] = {
            {2, p.read_stub, reinterpret_cast<void *>(p.read)},
            {3, p.write_stub, reinterpret_cast<void *>(p.write)},
            {4, p.apply_stub, reinterpret_cast<void *>(p.apply)},
        };
        for (const auto &s : slots) {
            if (!s.stub || !s.fn) continue;
            uint64_t *entry = image.at<uint64_t>(p.vtable + 8 * s.slot);
            if (*entry != uint64_t(uintptr_t(image.base + s.stub)) || !is_ret_stub(s.stub)) {
                log("%s slot %u: expected the stub 0x%llx, left alone", p.name, s.slot,
                    static_cast<unsigned long long>(s.stub));
                continue;
            }
            *entry = uint64_t(uintptr_t(s.fn));
            ++ported;
        }
    }
    log("net events: %d stubbed Read/Write/Apply slots ported", ported);
    installed += ported;
    if (install_extra_fields()) ++installed;
    if (install_rocket_guard()) ++installed;
    if (install_attack_log()) ++installed;
    if (install_carry_log()) ++installed;
    if (install_message_read()) ++installed;
}

}  // namespace u2mp::events
