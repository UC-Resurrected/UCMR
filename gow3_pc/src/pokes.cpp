// GOW3_POKE=<entry>[,<entry>...]: values the runtime keeps set in guest memory, rewritten every 250 ms.
//
// An entry is <offset>[=<value>] (hex image offset; the value is 1 when it is left out) or *<offset>+<add>[=<value>]
// (the value lives <add> bytes past the pointer stored at <offset>). A value of one or two hex digits is one byte; a
// longer one (the launcher always writes eight digits) is a 32-bit little-endian word, for example the bits of a float
// such as a clock multiplier. Reads and
// writes go through Read/WriteProcessMemory, so an address that is not mapped (yet) is skipped, and a value is only
// written when it differs. The launcher builds the list (launcher/native/runtime_env.cpp).
#include "pokes.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace pokes {

std::vector<Poke> parse(const char *text) {
    std::vector<Poke> list;
    const char *p = text ? text : "";
    while (*p) {
        Poke k;
        char *end = nullptr;
        if (*p == '*') {
            k.deref = true;
            ++p;
        }
        k.offset = std::strtoull(p, &end, 16);
        p = end;
        if (k.deref && *p == '+') {
            k.add = std::strtoull(p + 1, &end, 16);
            p = end;
        }
        if (*p == '=') {
            k.value = static_cast<uint32_t>(std::strtoul(p + 1, &end, 16));
            k.width = end - (p + 1) > 2 ? 4 : 1;
            if (k.width == 1) k.value &= 0xff;
            p = end;
        }
        list.push_back(k);
        while (*p && *p != ',') ++p;
        if (*p == ',') ++p;
    }
    return list;
}

namespace {

unsigned char *base;
std::vector<Poke> held;

// Reads / writes size bytes of this process's own memory at address; false (nothing done) where it is not mapped.
// Windows: Read/WriteProcessMemory on the own process. Linux: /proc/self/mem, which likewise skips unmapped pages
// (EIO) and writes through a read-only page as WriteProcessMemory does.
#ifdef _WIN32
bool safe_read(uintptr_t address, void *out, size_t size) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), out, size, &got) && got == size;
}
void safe_write(uintptr_t address, const void *data, size_t size) {
    WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(address), data, size, nullptr);
}
#else
int self_memory() {
    static const int fd = open("/proc/self/mem", O_RDWR | O_CLOEXEC);
    return fd;
}
bool safe_read(uintptr_t address, void *out, size_t size) {
    return self_memory() >= 0 && pread(self_memory(), out, size, static_cast<off_t>(address)) == static_cast<ssize_t>(size);
}
void safe_write(uintptr_t address, const void *data, size_t size) {
    if (self_memory() >= 0 && pwrite(self_memory(), data, size, static_cast<off_t>(address)) < 0) {}
}
#endif

void hold() {
    for (;;) {
        for (const Poke &k : held) {
            uintptr_t address = reinterpret_cast<uintptr_t>(base) + static_cast<uintptr_t>(k.offset);
            if (k.deref) {
                uint64_t pointer = 0;
                if (!safe_read(address, &pointer, sizeof pointer) || !pointer) continue;
                address = static_cast<uintptr_t>(pointer) + static_cast<uintptr_t>(k.add);
            }
            uint32_t current = 0;
            if (safe_read(address, &current, k.width) && std::memcmp(&current, &k.value, k.width) != 0)
                safe_write(address, &k.value, k.width);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

}  // namespace

}  // namespace pokes

extern "C" void pokes_start(unsigned char *image) {
    const char *env = std::getenv("GOW3_POKE");
    if (!env || !*env || !image) return;
    pokes::base = image;
    pokes::held = pokes::parse(env);
    std::printf("Runtime: %zu memory switches kept set (GOW3_POKE: %s)\n", pokes::held.size(), env);
    std::fflush(stdout);
    std::thread(pokes::hold).detach();
}
