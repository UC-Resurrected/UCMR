// Uncharted 3 1.02, 19 Nov 2026: the package manager's "loaded" and "unloaded" messages in our log.
//
// The engine's package manager reports every level package it finishes loading ("Logged in Package '<name>' in <n>ms")
// and every one it unloads ("Unloaded package "<name>" in ..."). It prints them with the game's own printf, whose output
// does not reach the runtime's log. Co-op adventure maps load their areas one after the other and unload the parts the
// players can no longer reach (as on PS3), so these two lines show which parts are in memory when a map runs short of
// it. The two print calls now go to a stub that prints the same text into our log. Found by their bytes.
#include "u3fix.h"
#include "u3fix_hook.h"

#include <cstdio>
#include <cstring>

namespace u3fix {

namespace {

// the guest's call, System V: format, then the time (seconds, milliseconds), the package name and, for loads, the time
// the load took; integers arrive as 64-bit registers, which %d reads correctly
__attribute__((sysv_abi)) int package_log(const char *format, long a, long b, const char *name, long c) {
    if (!format) return 0;
    std::printf("Uncharted 3 packages: ");
    std::printf(format, a, b, name ? name : "?", c);
    const size_t n = std::strlen(format);
    if (n && format[n - 1] != '\n') std::printf("\n");
    std::fflush(stdout);
    return 0;
}

struct Site {
    const char *what;
    unsigned char bytes[24];
    unsigned length;
    unsigned call;   // offset of the e8 call in bytes
};

// xor eax,eax; lea rdi,[fmt]; mov esi,r14d; lea rcx,[rsp+0xc0]; call printf
// lea rcx,[r14+0x40]; lea rdi,[fmt]; xor eax,eax; mov esi,r15d; call printf
const Site kSites[] = {
    {"package loaded", {0x31, 0xc0, 0x48, 0x8d, 0x3d, 0x2d, 0xbe, 0x6a, 0x00, 0x44, 0x89, 0xf6, 0x48, 0x8d, 0x8c, 0x24,
                        0xc0, 0x00, 0x00, 0x00, 0xe8}, 21, 20},
    {"package unloaded", {0x49, 0x8d, 0x4e, 0x40, 0x48, 0x8d, 0x3d, 0x1a, 0xf9, 0x6a, 0x00, 0x31, 0xc0, 0x44, 0x89, 0xfe,
                          0xe8}, 17, 16},
};

// Both messages are printed only while a debug switch of the package manager is on (a byte the debug menu sets). The
// two checks of that switch in front of the messages are opened; nothing else depends on them but the timing they print.
struct Gate {
    unsigned char bytes[16];
    unsigned length;
    unsigned branch, branch_length;
};
const Gate kGates[] = {
    // lea r12,[switch]; cmp byte [r12],0; je +0x51 (unloaded)
    {{0x4c, 0x8d, 0x25, 0xef, 0xef, 0xae, 0x00, 0x41, 0x80, 0x3c, 0x24, 0x00, 0x74, 0x51, 0xff, 0x53}, 16, 12, 2},
    // lea rax,[switch]; cmp byte [rax],0; je rel32 (loaded)
    {{0x48, 0x8d, 0x05, 0x58, 0xb9, 0xae, 0x00, 0x80, 0x38, 0x00, 0x0f, 0x84, 0x61, 0x01, 0x00, 0x00}, 16, 10, 6},
};

}  // namespace

bool install_package_log(unsigned char *image, uint64_t size) {
    for (const Gate &g : kGates) {
        const uint64_t at = find_unique(image, size, g.bytes, g.length);
        if (at != kNotFound) std::memset(image + at + g.branch, 0x90, g.branch_length);
    }
    unsigned char *stub = nullptr;
    unsigned used = 0, installed = 0;
    for (const Site &s : kSites) {
        const uint64_t at = find_unique(image, size, s.bytes, s.length);
        if (at == kNotFound) continue;
        if (!stub && !(stub = stub_page_near(image, size))) return false;
        // the stub: mov rax, handler; jmp rax (the guest's call already pushed its return address)
        unsigned char *entry = stub + used;
        const uint64_t handler = reinterpret_cast<uint64_t>(&package_log);
        entry[0] = 0x48; entry[1] = 0xb8;
        std::memcpy(entry + 2, &handler, 8);
        entry[10] = 0xff; entry[11] = 0xe0;
        used += 16;
        const int64_t delta = static_cast<int64_t>(reinterpret_cast<uintptr_t>(entry)) -
                              static_cast<int64_t>(reinterpret_cast<uintptr_t>(image + at + s.call + 5));
        if (delta < INT32_MIN || delta > INT32_MAX) continue;
        const int32_t rel = static_cast<int32_t>(delta);
        std::memcpy(image + at + s.call + 1, &rel, 4);   // the call now goes to the stub
        std::printf("Uncharted 3: %s messages go to the log (%#llx)\n", s.what,
                    static_cast<unsigned long long>(at + s.call));
        installed++;
    }
    if (stub) finish_stub(stub);
    std::fflush(stdout);
    return installed > 0;
}

}  // namespace u3fix
