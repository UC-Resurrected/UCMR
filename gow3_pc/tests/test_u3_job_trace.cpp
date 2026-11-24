// The job counter history (src/u3fix/job_trace.cpp) on a small stand-in image: functions with the same bytes at the
// places the trace hooks (queue push, counter update, the two makes, a job's lowering, the two give-backs), run through
// the installed stubs. Checks that the stubs keep the code working and that the counts and troubles come out right.
#include "u3fix/u3fix_hook.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace {

using Fn = uint64_t(__attribute__((sysv_abi)) *)(uint64_t, uint64_t, uint64_t);

unsigned char *g_img = nullptr;
alignas(64) unsigned char g_manager[0x5000];
alignas(128) unsigned char g_counters[8][0x80];

struct Asm {
    unsigned at;
    void put(std::initializer_list<unsigned char> b) { for (unsigned char c : b) g_img[at++] = c; }
    void rel32(unsigned target) {   // the displacement of an instruction that ends after these 4 bytes
        const int32_t d = static_cast<int32_t>(target) - static_cast<int32_t>(at + 4);
        std::memcpy(g_img + at, &d, 4);
        at += 4;
    }
    void imm64(uint64_t v) { std::memcpy(g_img + at, &v, 8); at += 8; }
};

constexpr unsigned kPush = 0x100, kUpdate = 0x200, kMake = 0x300, kInline = 0x400, kLower = 0x500, kRet = 0x5f0,
                   kFree1 = 0x600, kFree2 = 0x700, kManagerPtr = 0x800, kScratch = 0x810, kSize = 0x900;

void build() {
    Asm a{kPush};   // push(list, append): the push's first bytes, then back out
    a.put({0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50, 0x48, 0x63, 0x47, 0x4c, 0x4c, 0x8d, 0x35});
    a.rel32(kScratch);
    a.put({0x48, 0x83, 0xc4, 0x08, 0x5b, 0x41, 0x5e, 0x41, 0x5f, 0x5d, 0xc3});

    a = Asm{kUpdate};   // update(counter, value): the update's first bytes (lea r15 = the job manager), returns value
    a.put({0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x49, 0x89, 0xf6, 0x49, 0x89, 0xfc, 0x4c,
           0x8d, 0x3d});
    a.rel32(kManagerPtr);
    a.put({0x4c, 0x89, 0xf0, 0x5b, 0x41, 0x5c, 0x41, 0x5e, 0x41, 0x5f, 0x5d, 0xc3});

    a = Asm{kMake};   // make(counter, count): rbx counter, r13 count, r14 readable; returns the value before
    a.put({0x55, 0x48, 0x89, 0xe5, 0x41, 0x55, 0x53, 0x41, 0x56, 0x48, 0x83, 0xec, 0x48, 0x48, 0x89, 0xfb, 0x49, 0x89,
           0xf5, 0x4c, 0x8d, 0x35});
    a.rel32(kScratch);
    a.put({0x0f, 0x31, 0x48, 0xc1, 0xe2, 0x20, 0x48, 0x09, 0xc2, 0x48, 0x89, 0x53, 0x18, 0x4c, 0x87, 0x6b, 0x20, 0x4c,
           0x89, 0x6d, 0xc8, 0x48, 0x8b, 0x45, 0xc8, 0x49, 0x8b, 0x3e});
    a.put({0x48, 0x83, 0xc4, 0x48, 0x41, 0x5e, 0x5b, 0x41, 0x5d, 0x5d, 0xc3});

    a = Asm{kInline};   // made inline (counter r13, count r14 -> rax, r12 readable); returns the value before
    a.put({0x55, 0x48, 0x89, 0xe5, 0x41, 0x55, 0x41, 0x56, 0x41, 0x54, 0x48, 0x83, 0xec, 0x48, 0x49, 0x89, 0xfd, 0x49,
           0x89, 0xf6, 0x4c, 0x8d, 0x25});
    a.rel32(kScratch);
    a.put({0x0f, 0x31, 0x48, 0xc1, 0xe2, 0x20, 0x48, 0x09, 0xc2, 0x49, 0x89, 0x55, 0x18, 0x4c, 0x89, 0xf0, 0x49, 0x87,
           0x45, 0x20, 0x48, 0x89, 0x45, 0xc8, 0x48, 0x8b, 0x45, 0xc8, 0x49, 0x8b, 0x3c, 0x24});
    a.put({0x48, 0x83, 0xc4, 0x48, 0x41, 0x5c, 0x41, 0x5e, 0x41, 0x5d, 0x5d, 0xc3});

    a = Asm{kLower};   // lower(counter, entry, arg): [rbp-0x78] counter, rbx entry, [rbp-0x88] arg; returns the value before
    a.put({0x55, 0x48, 0x89, 0xe5, 0x53, 0x48, 0x81, 0xec, 0x98, 0x00, 0x00, 0x00, 0x48, 0x89, 0x7d, 0x88, 0x48, 0x89,
           0xf3, 0x48, 0x89, 0x95, 0x78, 0xff, 0xff, 0xff});
    a.put({0x48, 0xc7, 0xc0, 0xff, 0xff, 0xff, 0xff, 0x48, 0x8b, 0x7d, 0x88, 0xf0, 0x48, 0x0f, 0xc1, 0x47, 0x20, 0x48,
           0x89, 0x45, 0xc0, 0x48, 0x8b, 0x75, 0xc0, 0x48, 0xff, 0xce, 0xe8});
    a.rel32(kRet);
    a.put({0x48, 0x8b, 0x45, 0xc0, 0x48, 0x81, 0xc4, 0x98, 0x00, 0x00, 0x00, 0x5b, 0x5d, 0xc3});
    g_img[kRet] = 0xc3;

    for (unsigned at : {kFree1, kFree2}) {   // give_back(counter, bit, mask): r12 counter, r13 the manager
        a = Asm{at};
        a.put({0x55, 0x48, 0x89, 0xe5, 0x41, 0x54, 0x41, 0x55, 0x49, 0x89, 0xfc, 0x49, 0xbd});
        a.imm64(reinterpret_cast<uint64_t>(g_manager));
        a.put({0x48, 0x89, 0xf1, 0x48, 0x89, 0xd0});
        a.put({0x48, 0xc1, 0xe9, 0x06, 0x49, 0x21, 0x84, 0xcd, 0x00, 0x0b, 0x00, 0x00, 0x0f, 0xae, 0xf8});
        a.put({0x41, 0x5d, 0x41, 0x5c, 0x5d, 0xc3});
    }
    const uint64_t manager = reinterpret_cast<uint64_t>(g_manager);
    std::memcpy(g_img + kManagerPtr, &manager, 8);
    // the counter pool: counters at manager +0xae0 +0x120, capacity at +0x128
    const uint64_t base = reinterpret_cast<uint64_t>(g_counters);
    const uint32_t capacity = 8;
    std::memcpy(g_manager + 0xae0 + 0x120, &base, 8);
    std::memcpy(g_manager + 0xae0 + 0x128, &capacity, 4);
}

uint64_t call(unsigned at, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0) {
    return reinterpret_cast<Fn>(g_img + at)(a, b, c);
}

uint64_t ptr(const void *p) { return reinterpret_cast<uint64_t>(p); }
int64_t value(const unsigned char *counter) {
    int64_t v;
    std::memcpy(&v, counter + 0x20, 8);
    return v;
}

std::string story(const void *counter) {
    char text[1200];
    u3fix::counter_story(counter, text, sizeof text);
    return text;
}

// a list of jobs (0x40 bytes each, the counter at +0x10) with write index, count and capacity as the push reads them
struct List {
    unsigned char head[0x60] = {};
    unsigned char jobs[4][0x40] = {};
    List(std::initializer_list<const void *> counters) {
        int32_t n = 0;
        for (const void *c : counters) std::memcpy(jobs[n++] + 0x10, &c, 8);
        const int32_t capacity = 4;
        const unsigned char *j = jobs[0];
        std::memcpy(head + 0x4, &n, 4);
        std::memcpy(head + 0x8, &n, 4);
        std::memcpy(head + 0x10, &j, 8);
        std::memcpy(head + 0x18, &capacity, 4);
    }
};

}  // namespace

int main() {
#ifdef _WIN32
    g_img = static_cast<unsigned char *>(VirtualAlloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
#else
    void *m = mmap(nullptr, 0x10000, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    g_img = m == MAP_FAILED ? nullptr : static_cast<unsigned char *>(m);
#endif
    assert(g_img);
    std::memset(g_img, 0xcc, 0x10000);
    build();
    assert(u3fix::pending_jobs(g_counters[0]) == -1);   // no trace yet
    assert(u3fix::install_job_trace(g_img, kSize));
    assert(u3fix::job_manager() == g_manager);
    assert(reinterpret_cast<const unsigned char *>(u3fix::counter_update_function()) == g_img + kUpdate);

    unsigned char *c0 = g_counters[0], *c1 = g_counters[1], *c2 = g_counters[2];
    uint32_t life = 0;

    // made for 2 jobs, both submitted, both finish: a clean life
    assert(call(kMake, ptr(c0), 2) == 0 && value(c0) == 2);
    assert(u3fix::counter_trouble(c0, &life) == 0 && life == 1);
    List two{c0, c0};
    call(kPush, ptr(two.head), 1);
    assert(u3fix::pending_jobs(c0) == 2);
    call(kPush, ptr(two.head), 0);   // pushed to the front: released from a wait, counted already
    assert(u3fix::pending_jobs(c0) == 2);
    assert(call(kLower, ptr(c0), ptr(g_img + 0x123), 0x55) == 2 && value(c0) == 1);
    assert(call(kLower, ptr(c0), ptr(g_img + 0x123), 0x56) == 1 && value(c0) == 0);
    assert(u3fix::pending_jobs(c0) == 0 && u3fix::counter_trouble(c0, nullptr) == 0);

    // a third job lowers it: below 0, not counted, reported
    assert(call(kLower, ptr(c0), ptr(g_img + 0x124), 0x57) == 0 && value(c0) == -1);
    assert(u3fix::counter_trouble(c0, nullptr) & u3fix::kCounterBelowZero);
    assert(u3fix::pending_jobs(c0) == 0);
    std::string s = story(c0);
    assert(s.find("job 0x124 arg 0x57 (not counted) lowered it to -1") != std::string::npos);
    assert(s.find("job 0x123 arg 0x56 lowered it to 0") != std::string::npos);
    assert(s.find("made by guest") != std::string::npos);

    // game code changes it and calls the update: the arguments reach the update unchanged
    assert(call(kUpdate, ptr(c0), 5) == 5);
    assert(story(c0).find("set 5 (value -1)") != std::string::npos);

    // given back while a job on it is unfinished, then made again: both are troubles of the new life
    List one{c1};
    call(kPush, ptr(one.head), 1);
    assert(u3fix::pending_jobs(c1) == 1);
    call(kFree1, ptr(c1), 0, ~uint64_t{0});
    assert(u3fix::counter_trouble(c1, nullptr) & u3fix::kCounterFreedBusy);
    assert(call(kMake, ptr(c1), 1) == 0 && value(c1) == 1);
    assert(u3fix::counter_trouble(c1, &life) == u3fix::kCounterReused && life == 1);
    call(kFree2, ptr(c1), 0, ~uint64_t{0});
    assert(story(c1).find("given back by guest") != std::string::npos);

    // made inline (RunJobs) and settled by the runtime
    assert(call(kInline, ptr(c2), 3) == 0 && value(c2) == 3);
    assert(u3fix::counter_trouble(c2, &life) == 0 && life == 1);
    u3fix::note_settled(c2, 3);
    assert(u3fix::counter_trouble(c2, nullptr) == u3fix::kCounterSettled);
    assert(story(c2).find("settled from 3") != std::string::npos);

    // something outside the pool is not traced
    assert(u3fix::pending_jobs(g_manager) == -1);
    std::puts("u3 job trace: ok");
    return 0;
}
