// u2mp netinit: the state the PS3 multiplayer start-up used to create, set up again for PS4 code that still
// reads it.
//
// PS4 1.02 lost NetInit, the connecting page update and NetGameManager::Init, but the remaining code still
// expects what they built: two hash tables inside the NetGameManager (the match-start reset walks their
// bucket arrays and crashes on NULL), an empty recent-game history, the leave status of the match and party
// sessions, and the list of boosters. session.cpp calls in here when Multiplayer is entered and when a match
// starts. It also gives the player-setup event back the slots it used to find (it wiped them on a joiner) and
// sizes NetCharacter's process heap for a single-player actor.
//
// Reimplemented from the behaviour description u2mp/docs/spec/progression.md.
#include <cstdint>
#include <cstring>
#include <windows.h>

#include "u2mp_internal.h"

namespace u2mp {

namespace {

// ---------------------------------------------------------------------------------------------
// Game image offsets (PS4 1.02).

constexpr uint64_t kTableA = 0x113d300;          // NetGameManager +0x1ba0
constexpr uint64_t kTableB = 0x113d370;          // NetGameManager +0x1c10
constexpr uint64_t kGameHistory = 0x113d402;     // 4 x s16
constexpr uint64_t kMatchLeaveStatus = 0x1065bd0;
constexpr uint64_t kPartyLeaveStatus = 0x105f080;
constexpr uint64_t kBoosterList = 0x113a788;     // u32 count, then pointers from +8
constexpr uint32_t kMaxBoosters = 63;
constexpr uint64_t kDcFind = 0xa15300;
constexpr uint32_t kBoostersSid = 0x916e03b0;
constexpr uint64_t kPlayerSetupClear = 0x632148;   // in NetEventPlayerSetup::Apply
constexpr uint64_t kNetCharacterType = 0x14cee28;  // process type node: +0x10 id, +0x18 heap size
constexpr uint32_t kNetCharacterSid = 0xf2c86c22;
constexpr uint32_t kNetCharacterHeap = 0x80000;

// Hash table fields.
constexpr uint64_t kHtBuckets = 0x00;
constexpr uint64_t kHtFree = 0x08;
constexpr uint64_t kHtNodeSize = 0x10;
constexpr uint64_t kHtNodeCount = 0x14;
constexpr uint64_t kHtPool = 0x18;
constexpr uint64_t kHtSentinel = 0x20;
constexpr uint64_t kHtAlloc = 0x28;
constexpr uint64_t kHtBucketsEnd = 0x30;
constexpr uint64_t kHtBucketCount = 0x3c;
constexpr uint64_t kHtReady = 0x40;

// The PS4 code this file stands in for: if any of it differs, the layout assumptions here do not hold.
struct CodeCheck {
    uint64_t offset;
    const char *label;
    unsigned char bytes[16];
    size_t length;
};

const CodeCheck kChecks[] = {
    {0x640fe4, "reset table A", {0x49, 0x8b, 0xbf, 0xa0, 0x1b, 0x00, 0x00}, 7},
    {0x641012, "reset table A buckets", {0x4d, 0x63, 0x87, 0xdc, 0x1b, 0x00, 0x00}, 7},
    {0x6410e0, "table A node size", {0x4c, 0x8d, 0x60, 0x60}, 4},
    {0x64119e, "reset table B", {0x49, 0x8b, 0xbf, 0x10, 0x1c, 0x00, 0x00}, 7},
    {0x6411d2, "reset table B buckets", {0x49, 0x63, 0x8f, 0x4c, 0x1c, 0x00, 0x00}, 7},
    {0x641290, "table B node size", {0x4c, 0x8d, 0x68, 0x38}, 4},
    {0x5d90bb, "hash table layout", {0x41, 0xc7, 0x47, 0x10, 0x58, 0x00, 0x00}, 7},
    {0x5ec590, "session leave status",
     {0x48, 0x8d, 0x0d, 0x71, 0x95, 0xa7, 0x00, 0x31, 0xc0, 0x80, 0xb9, 0xc8, 0x00, 0x00, 0x00, 0x00}, 16},
    {0x5f6750, "booster list", {0x48, 0x8b, 0x44, 0xd7, 0x08, 0x39, 0x70, 0x0c, 0x74}, 9},
    {0x5fd8ef, "booster object", {0x48, 0x8d, 0x05, 0x92, 0xce, 0xb3, 0x00}, 7},
    {0xb332a0, "Alloc", {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50, 0x41, 0x89}, 16},
    {0xa15300, "DcFind", {0x55, 0x48, 0x89, 0xe5, 0x53, 0x48, 0x83, 0xec, 0x18, 0x4c, 0x8b, 0x05, 0xf0, 0x58, 0x63, 0x00}, 16},
    {0x632148, "player-setup slot reset", {0xe8, 0x43, 0xba, 0x00, 0x00}, 5},
};

// ---------------------------------------------------------------------------------------------
// Memory access (little-endian, possibly unaligned).

template <typename T> T get(const uint8_t *p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
template <typename T> void put(uint8_t *p, T v) { std::memcpy(p, &v, sizeof v); }
// The loader maps the image's code segments executable-only after u2mp_install has run. prepare() is
// called later (the Multiplayer button, the match start), so a code patch made from here must first
// make the page writable and put it back: without this, the first write into 0x5e1434 (the first net
// bit-writer site) was an access violation and the game froze entering the multiplayer menu.
bool write_code(uint64_t offset, const void *bytes, size_t length) {
    void *const p = image.base + offset;
    DWORD old = 0;
    if (!VirtualProtect(p, length, PAGE_EXECUTE_READWRITE, &old)) {
        log("netinit: code at 0x%llx cannot be made writable (%lu)", (unsigned long long)offset,
            (unsigned long)GetLastError());
        return false;
    }
    std::memcpy(p, bytes, length);
    FlushInstructionCache(GetCurrentProcess(), p, length);
    DWORD ignored = 0;
    VirtualProtect(p, length, old, &ignored);
    return true;
}

uint64_t address_of(const uint8_t *p) { return uint64_t(reinterpret_cast<uintptr_t>(p)); }
uint8_t *pointer_at(const uint8_t *p) { return reinterpret_cast<uint8_t *>(uintptr_t(get<uint64_t>(p))); }

// ---------------------------------------------------------------------------------------------
// Module state.

bool g_checked = false;
bool g_checks_ok = false;
bool g_history_done = false;
bool g_boosters_done = false;

bool code_matches() {
    if (!g_checked) {
        g_checked = true;
        g_checks_ok = true;
        for (const CodeCheck &c : kChecks) {
            if (image.matches(c.offset, c.bytes, c.length)) continue;
            log("netinit: bytes at %#llx (%s) differ; nothing done", (unsigned long long)c.offset, c.label);
            g_checks_ok = false;
        }
    }
    return g_checks_ok;
}

// Builds a hash table with an empty bucket array and every node on the free list, in one allocation:
// bucket slots (plus one that points at the sentinel), then the node pool, then the sentinel node.
void build_table(uint64_t offset, uint32_t entries, uint32_t node_size, const char *name) {
    uint8_t *table = image.at<uint8_t>(offset);
    if (get<uint64_t>(table + kHtBuckets) != 0 && table[kHtReady] != 0) return;

    uint32_t buckets = entries - 1;
    buckets |= buckets >> 1;
    buckets |= buckets >> 2;
    buckets |= buckets >> 4;
    buckets |= buckets >> 8;
    buckets |= buckets >> 16;
    buckets += 1;
    const uint64_t pool_offset = (uint64_t(buckets) * 8 + 8 + 15) & ~uint64_t(15);
    const uint64_t size = pool_offset + (uint64_t(entries) + 1) * node_size;

    uint8_t *mem = static_cast<uint8_t *>(netheap_alloc(size, 16));
    if (!mem) {
        log("netinit: %s: allocation of %llu bytes failed", name, (unsigned long long)size);
        return;
    }

    uint8_t *pool = mem + pool_offset;
    uint8_t *sentinel = pool + uint64_t(entries) * node_size;
    for (uint32_t i = 0; i < buckets; ++i) put<uint64_t>(mem + 8 * uint64_t(i), 0);
    put<uint64_t>(mem + 8 * uint64_t(buckets), address_of(sentinel));
    put<uint64_t>(sentinel + 0, address_of(sentinel));
    put<uint64_t>(sentinel + 8, address_of(sentinel));
    for (uint32_t i = 0; i < entries; ++i) {
        uint8_t *node = pool + uint64_t(i) * node_size;
        put<uint64_t>(node + 0, i + 1 < entries ? address_of(node + node_size) : 0);
        put<uint64_t>(node + 8, i > 0 ? address_of(node - node_size) : 0);
    }

    put<uint64_t>(table + kHtFree, address_of(pool));
    put<uint32_t>(table + kHtNodeSize, node_size);
    put<uint32_t>(table + kHtNodeCount, entries);
    put<uint64_t>(table + kHtPool, address_of(pool));
    put<uint64_t>(table + kHtSentinel, address_of(sentinel));
    put<uint64_t>(table + kHtAlloc, address_of(mem));
    put<uint64_t>(table + kHtBucketsEnd, address_of(mem + 8 * uint64_t(buckets)));
    put<int32_t>(table + kHtBucketCount, int32_t(buckets));
    put<uint64_t>(table + kHtBuckets, address_of(mem));
    table[kHtReady] = 1;
    log("netinit: %s: %d entries of %#x, %d buckets", name, int(entries), node_size, int(buckets));
}

// Lists the boosters of the *boosters* DC table. When some boosters carry a DLC mask, only those count
// (the PS4 table also holds placeholders without one).
void list_boosters() {
    if (g_boosters_done) return;
    uint8_t *list = image.at<uint8_t>(kBoosterList);
    if (get<uint32_t>(list) != 0) {
        g_boosters_done = true;
        return;
    }
    const uint8_t *slot =
        static_cast<const uint8_t *>(reinterpret_cast<void *(U2MP_GUEST *)(uint32_t)>(image.at<uint8_t>(kDcFind))(kBoostersSid));
    const uint8_t *table = slot ? pointer_at(slot) : nullptr;
    if (!table) {
        log("netinit: *boosters* not loaded yet");
        return;
    }
    const int32_t n = get<int32_t>(table);
    uint8_t *records = pointer_at(table + 8);
    if (n <= 0 || !records) return;

    auto mask = [&](int32_t i) { return get<uint32_t>(records + 0x20 * uint64_t(i) + 0x18); };
    bool any_mask = false;
    for (int32_t i = 0; i < n; ++i)
        if (mask(i) != 0) any_mask = true;

    uint32_t listed = 0;
    for (int32_t i = 0; i < n && listed < kMaxBoosters; ++i) {
        if (any_mask && mask(i) == 0) continue;
        put<uint64_t>(list + 8 + 8 * uint64_t(listed), address_of(records + 0x20 * uint64_t(i)));
        ++listed;
    }
    put<uint32_t>(list, listed);
    g_boosters_done = true;
    log("netinit: %u of %d boosters listed", listed, n);
}

// The net bit writers keep the destination word's bits above the field with
// `mov r32,-1; mov cl,<32-offset>; shl r32,cl`: at offset 0 the count is 32, which x86 masks
// to 0, so the whole old word is kept and the field is ORed into stale bits (the player
// snapshot's locator x becomes the OR of every x sent). A 64-bit shift clears the field first.
unsigned mask_mov(const uint8_t *p, int &reg) {
    static const uint8_t ones[4] = {0xff, 0xff, 0xff, 0xff};
    if (p[0] >= 0xb8 && p[0] <= 0xbf && std::memcmp(p + 1, ones, 4) == 0) {
        reg = p[0] - 0xb8;
        return 5;
    }
    if (p[0] == 0x41 && p[1] >= 0xb8 && p[1] <= 0xbf && std::memcmp(p + 2, ones, 4) == 0) {
        reg = 8 + p[1] - 0xb8;
        return 6;
    }
    return 0;
}

unsigned mask_between(const uint8_t *p, int reg) {
    int rex = 0;
    unsigned n = 0;
    if (p[0] == 0x40 || p[0] == 0x44) {
        rex = p[0];
        n = 1;
    }
    if (p[n] == 0x88 && (p[n + 1] & 0xc7) == 0xc1) {
        const int src = ((p[n + 1] >> 3) & 7) | (rex == 0x44 ? 8 : 0);
        if (src == reg || (!rex && src >= 4 && (src & 3) == reg)) return 0;
        return n + 2;
    }
    if (!rex && p[0] == 0x29 && (p[1] & 0xc0) == 0xc0) {
        if ((p[1] & 7) == reg || ((p[1] >> 3) & 7) == reg) return 0;
        return 2;
    }
    return 0;
}

// Returns the length of the idiom at `at` and its replacement in `patch`, or 0.
unsigned mask_site(uint64_t at, uint8_t *patch) {
    const uint8_t *p = image.base + at;
    int reg = -1;
    const unsigned mov = mask_mov(p, reg);
    if (!mov) return 0;
    unsigned mid = 0;
    for (int step = 0; step < 2; ++step) {
        const unsigned k = mask_between(p + mov + mid, reg);
        if (!k) break;
        mid += k;
    }
    const uint8_t *sh = p + mov + mid;
    const bool shl = reg < 8 ? (sh[0] == 0xd3 && sh[1] == 0xe0 + reg)
                             : (sh[0] == 0x41 && sh[1] == 0xd3 && sh[2] == 0xe0 + (reg - 8));
    if (!shl) return 0;
    const unsigned len = mov + mid + (reg < 8 ? 2 : 3);
    const uint8_t rex = reg < 8 ? 0x48 : 0x49;
    unsigned n = 0;
    std::memcpy(patch, p + mov, mid);
    n = mid;
    patch[n++] = rex;                                       // or r64, -1
    patch[n++] = 0x83;
    patch[n++] = uint8_t(0xc8 + (reg & 7));
    patch[n++] = 0xff;
    patch[n++] = rex;                                       // shl r64, cl
    patch[n++] = 0xd3;
    patch[n++] = uint8_t(0xe0 + (reg & 7));
    while (n < len) patch[n++] = 0x90;
    return len;
}

int fix_bit_writers() {
    constexpr unsigned kSites = 342;
    constexpr uint64_t kFrom = 0x5e0000, kTo = 0x640000;
    uint8_t patch[32];
    unsigned found = 0;
    for (uint64_t at = kFrom; at + 16 <= kTo;) {
        const unsigned len = mask_site(at, patch);
        if (!len) {
            ++at;
            continue;
        }
        ++found;
        at += len;
    }
    if (found != kSites) {
        log("netinit: %u net bit writer sites, not the %u of 1.02: masks left alone", found, kSites);
        return 0;
    }
    // The whole range is one executable segment; make it writable once instead of per site.
    DWORD old = 0;
    if (!VirtualProtect(image.base + kFrom, kTo - kFrom, PAGE_EXECUTE_READWRITE, &old)) {
        log("netinit: the net bit writer range cannot be made writable (%lu); masks left alone",
            (unsigned long)GetLastError());
        return 0;
    }
    unsigned patched = 0;
    for (uint64_t at = kFrom; at + 16 <= kTo;) {
        const unsigned len = mask_site(at, patch);
        if (!len) {
            ++at;
            continue;
        }
        if (std::memcmp(image.base + at, patch, len) != 0) {
            std::memcpy(image.base + at, patch, len);
            ++patched;
        }
        at += len;
    }
    FlushInstructionCache(GetCurrentProcess(), image.base + kFrom, kTo - kFrom);
    DWORD ignored = 0;
    VirtualProtect(image.base + kFrom, kTo - kFrom, old, &ignored);
    log("netinit: net bit writer word masks fixed (%u of %u)", patched, kSites);
    return int(patched);
}

void prepare() {
    if (!code_matches()) return;
    static bool fixes_done = false;
    if (!fixes_done) {
        fixes_done = true;
        fix_bit_writers();
    }
    if (!g_history_done) {
        g_history_done = true;
        std::memset(image.at<uint8_t>(kGameHistory), 0xff, 8);   // four s16 of -1: no recent games
    }
    *image.at<uint8_t>(kMatchLeaveStatus) = 1;
    *image.at<uint8_t>(kPartyLeaveStatus) = 1;
    build_table(kTableA, 16, 0x60, "ngm table 0x1ba0");
    build_table(kTableB, 256, 0x38, "ngm table 0x1c10");
    list_boosters();

    // The player-setup event wipes every slot on a joiner (0x632110 -> 0x63db90) and then fills the ones it
    // finds by NpId. The session layer filled them; leave them to the entries instead.
    uint8_t *clear = image.at<uint8_t>(kPlayerSetupClear);
    if (clear[0] == 0xe8) {
        static const uint8_t nop[5] = {0x90, 0x90, 0x90, 0x90, 0x90};
        if (write_code(kPlayerSetupClear, nop, sizeof nop))
            log("netinit: the player-setup event keeps the joiner's slots");
    }

    // NetCharacter's heap (128 KiB) fits the MP models; a player on a single-player actor (the
    // hero-parka skin) overruns it in NetCharacter::Init and the match dies. Player's is 512 KiB.
    uint8_t *type = image.at<uint8_t>(kNetCharacterType);
    if (get<uint32_t>(type + 0x10) == kNetCharacterSid && get<uint32_t>(type + 0x18) == 0x20000) {
        put<uint32_t>(type + 0x18, kNetCharacterHeap);
        log("netinit: NetCharacter process heap 128 -> 512 KiB");
    }

    // The online task's update stores 1 into the render option "Disable Foreground Meshes" every
    // frame, so no player, NetCharacter, pickup or prop is drawn in a net game.
    static const unsigned char foreground[8] = {0x41, 0xc6, 0x87, 0xbf, 0x00, 0x00, 0x00, 0x01};
    if (image.matches(0x5f228d, foreground, sizeof foreground)) {
        const uint8_t zero = 0;
        if (write_code(0x5f228d + 7, &zero, 1))
            log("netinit: foreground meshes shown (0x5f228d stores 0)");
    }
}

void dump_table(const char *when, const char *label, uint64_t offset) {
    const uint8_t *table = image.at<uint8_t>(offset);
    const uint64_t buckets = get<uint64_t>(table + kHtBuckets);
    const uint64_t end = get<uint64_t>(table + kHtBucketsEnd);
    const int32_t n = get<int32_t>(table + kHtBucketCount);
    int used = 0, minus = 0;
    if (buckets != 0 && n > 0 && n <= 4096) {
        const uint8_t *slots = reinterpret_cast<const uint8_t *>(uintptr_t(buckets));
        for (int32_t i = 0; i <= n; ++i) {
            const uint64_t v = get<uint64_t>(slots + 8 * uint64_t(i));
            if (v != 0) ++used;
            if (v == ~uint64_t(0)) ++minus;
        }
    }
    const bool ok = buckets != 0 && end == buckets + uint64_t(int64_t(n)) * 8;
    log("netinit[%s] table %s: buckets %#llx end %#llx (%s) n %d free %#llx pool %#llx sentinel %#llx ready %u; "
        "non-null slots %d, -1 slots %d",
        when, label, (unsigned long long)buckets, (unsigned long long)end, ok ? "ok" : "MISMATCH", n,
        (unsigned long long)get<uint64_t>(table + kHtFree), (unsigned long long)get<uint64_t>(table + kHtPool),
        (unsigned long long)get<uint64_t>(table + kHtSentinel), unsigned(table[kHtReady]), used, minus);
}

}  // namespace

void netinit_prepare() { prepare(); }

void netinit_match() { prepare(); }

void netinit_dump(const char *when) {
    dump_table(when, "0x1ba0", kTableA);
    dump_table(when, "0x1c10", kTableB);
}

}  // namespace u2mp
