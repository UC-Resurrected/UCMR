#include "grenades.h"

#include <cstring>
#include <mutex>

#include "u2mp_internal.h"

namespace u2mp::grenades {
namespace {

constexpr uint64_t kProcessManager = 0x115f1a0;
constexpr uint64_t kNextProcess = 0x9bcdf0;
constexpr uint64_t kKillTree = 0x9be760;
constexpr uint64_t kNetState = 0x14ce97c;
constexpr uint64_t kNgmState = 0x113b760 + 0x1e80;
constexpr uint32_t kNetGrenade = 0x1caece01u;
constexpr size_t kMost = 32;

struct Entry {
    uint64_t process;
    uint32_t id;
    uint32_t sig;
    uint32_t age;
    uint32_t still;
    bool seen;
};

Entry g_entries[kMost];
unsigned g_count;
uint64_t g_last_ms;
std::mutex g_lock;

template <typename T> T load(const void *base, size_t off) {
    T v;
    std::memcpy(&v, static_cast<const uint8_t *>(base) + off, sizeof v);
    return v;
}
template <typename Fn> Fn guest(uint64_t offset) { return reinterpret_cast<Fn>(image.base + offset); }

bool playing() {
    return load<int32_t>(image.base, kNetState) > 1 && load<int32_t>(image.base, kNgmState) == 2;
}

uint32_t signature(uint32_t count, uint32_t newest, uint8_t state) {
    return count * 0x9e3779b1u ^ newest ^ ((state & 4) ? 0x80000000u : 0u);
}

}  // namespace

void frame(double now_seconds) {
    if (!image.base || !playing()) {
        std::lock_guard<std::mutex> lock(g_lock);
        g_count = 0;
        g_last_ms = 0;
        return;
    }
    const uint64_t now = uint64_t(now_seconds * 1000.0);
    uint32_t step = 0;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (g_last_ms && now - g_last_ms < 100) return;
        if (g_last_ms) step = uint32_t(now - g_last_ms < 250 ? now - g_last_ms : 250);
        g_last_ms = now;
        for (unsigned i = 0; i < g_count; ++i) g_entries[i].seen = false;
    }

    const uint64_t manager = uint64_t(uintptr_t(image.base)) + kProcessManager;
    const uint64_t bucket = load<uint64_t>(reinterpret_cast<const void *>(manager), 0xc58);
    if (!bucket) return;
    using NextFn = uint64_t(U2MP_GUEST *)(uint64_t, uint64_t);
    using KillFn = void(U2MP_GUEST *)(uint64_t);
    const NextFn next = guest<NextFn>(kNextProcess);
    uint64_t due[4];
    unsigned due_count = 0;
    unsigned walked = 0;

    for (uint64_t p = next(bucket, bucket); p && walked < 4096; p = next(p, bucket), ++walked) {
        const uint32_t type = load<uint32_t>(reinterpret_cast<const void *>(p), 0x30);
        const uint16_t flags = load<uint16_t>(reinterpret_cast<const void *>(p), 0x34);
        if (type != kNetGrenade || (flags & 5) != 1) continue;
        const uint32_t id = load<uint32_t>(reinterpret_cast<const void *>(p), 0x44);
        const uint32_t count = load<uint32_t>(reinterpret_cast<const void *>(p), 0x760);
        const uint32_t newest = load<uint32_t>(reinterpret_cast<const void *>(p), 0x5e0);
        const uint8_t state = load<uint8_t>(reinterpret_cast<const void *>(p), 0x5c8);
        const uint32_t sig = signature(count, count ? newest : 0, state);
        bool overdue = false;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            Entry *e = nullptr;
            for (unsigned i = 0; i < g_count && !e; ++i)
                if (g_entries[i].process == p) e = &g_entries[i];
            if (e && e->id != id) {
                *e = {p, id, sig, 0, 0, true};
                continue;
            }
            if (!e) {
                if (g_count >= kMost) continue;
                e = &g_entries[g_count++];
                *e = {p, id, sig, 0, 0, true};
                continue;
            }
            e->seen = true;
            e->age = e->age + step < e->age ? UINT32_MAX : e->age + step;
            if (e->sig != sig) {
                e->sig = sig;
                e->still = 0;
            } else {
                e->still = e->still + step < e->still ? UINT32_MAX : e->still + step;
            }
            overdue = e->age >= 10000 && e->still >= 4000;
        }
        if (overdue && due_count < 4) due[due_count++] = p;
    }

    {
        std::lock_guard<std::mutex> lock(g_lock);
        unsigned k = 0;
        for (unsigned i = 0; i < g_count; ++i)
            if (g_entries[i].seen) g_entries[k++] = g_entries[i];
        g_count = k;
    }
    if (!due_count) return;
    if (!load<uint8_t>(reinterpret_cast<const void *>(manager), 8)) return;
    static int taken;
    for (unsigned k = 0; k < due_count; ++k) {
        guest<KillFn>(kKillTree)(due[k]);
        if (++taken <= 8 || (taken & (taken - 1)) == 0)
            log("grenade watch: removed a remote grenade with no explosion (%d)", taken);
    }
}

}  // namespace u2mp::grenades
