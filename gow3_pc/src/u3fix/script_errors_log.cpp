// Uncharted 3 1.02, 20 Nov 2026: the game's script and animation error messages in our log.
//
// Many engine and script problems are reported through one printing function (image 0x12444e0; it takes a context
// value, a printf-style format and its arguments), for example "Animation artgroup %s not loaded!", "%s: Anim '%s' not
// found!" or "get-animation-entry: Unable to find entry location from animation '%s' for object '%s'". The retail game
// prints them only while a debug switch is on, so they are lost. In co-op Monastery the AI Drake stood in a bind pose
// after his rescue and the level could not go on (20 Nov 2026); these messages say which animation or art group was
// missing. The function's entry now goes through a stub that prints the message into our log, then runs the game's
// function as before. The text is put together here (strings, integers; floating-point values shown as '?'), never by
// handing the game's format to printf, and each different message is printed at most three times. Found by its bytes.
#include "u3fix.h"
#include "u3fix_hook.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace u3fix {

namespace {

bool readable_text(const char *p) {
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION info;
    if (!p || !VirtualQuery(p, &info, sizeof info) || info.State != MEM_COMMIT) return false;
    if (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return true;
#else
    return p != nullptr;
#endif
}

// a short copy of a guest string (stops at 120 characters or the end of its memory page)
std::string text_of(const char *p) {
    if (!readable_text(p)) return "(?)";
    std::string s;
    const uintptr_t page_end = (reinterpret_cast<uintptr_t>(p) | 0xfff) + 1;
    for (; s.size() < 120 && reinterpret_cast<uintptr_t>(p) < page_end && *p; p++) s += *p;
    return s;
}

constexpr unsigned kSeen = 4096;
std::atomic<uint64_t> g_seen[kSeen];   // hash of each message printed, and how often (low 2 bits)
std::atomic<unsigned> g_lines{0};

__attribute__((sysv_abi)) void script_error(uint64_t context, const char *format, uint64_t a, uint64_t b, uint64_t c,
                                            uint64_t d) {
    if (!readable_text(format) || g_lines.load() > 5000) return;
    const uint64_t args[4] = {a, b, c, d};
    unsigned next = 0;
    std::string out;
    const std::string f = text_of(format);
    for (size_t i = 0; i < f.size(); i++) {
        if (f[i] != '%') { out += f[i]; continue; }
        size_t j = i + 1;
        while (j < f.size() && std::strchr("-+ #0123456789.lhzjtL", f[j])) j++;
        if (j >= f.size()) break;
        const char kind = f[j];
        i = j;
        if (kind == '%') { out += '%'; continue; }
        if (kind == 'f' || kind == 'g' || kind == 'e' || kind == 'F' || kind == 'G' || kind == 'E') { out += '?'; continue; }
        if (next >= 4) { out += "..."; continue; }
        const uint64_t v = args[next++];
        char num[32];
        if (kind == 's') out += text_of(reinterpret_cast<const char *>(v));
        else if (kind == 'c') out += static_cast<char>(v);
        else if (kind == 'x' || kind == 'X' || kind == 'p') { std::snprintf(num, sizeof num, "%#llx", static_cast<unsigned long long>(v)); out += num; }
        else if (kind == 'u') { std::snprintf(num, sizeof num, "%llu", static_cast<unsigned long long>(static_cast<uint32_t>(v))); out += num; }
        else { std::snprintf(num, sizeof num, "%lld", static_cast<long long>(static_cast<int32_t>(v))); out += num; }
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    uint64_t h = 1469598103934665603ull;
    for (char ch : out) h = (h ^ static_cast<unsigned char>(ch)) * 1099511628211ull;
    h &= ~3ull;
    for (unsigned k = 0; k < 16; k++) {   // find or claim its slot, count it, print the first three
        std::atomic<uint64_t> &slot = g_seen[(h >> 2) % kSeen + k < kSeen ? (h >> 2) % kSeen + k : k];
        uint64_t seen = slot.load();
        if (seen == 0 && slot.compare_exchange_strong(seen, h)) seen = h;
        if ((seen & ~3ull) != h) continue;
        if ((seen & 3) == 3) return;
        slot.fetch_add(1);
        break;
    }
    ++g_lines;
    std::printf("Uncharted 3 script: %s (%#llx)\n", out.c_str(), static_cast<unsigned long long>(context));
    std::fflush(stdout);
}

// push rbp; mov rbp,rsp; push r15; push r14; push r12; push rbx; sub rsp,0x2f0; mov r15,rsi; test al,al; je
constexpr unsigned char kSite[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48,
                                   0x81, 0xec, 0xf0, 0x02, 0x00, 0x00, 0x49, 0x89, 0xf7, 0x84, 0xc0, 0x74};
constexpr unsigned kPrologue = 6;   // push rbp; mov rbp,rsp; push r15

}  // namespace

bool install_script_errors_log(unsigned char *image, uint64_t size) {
    const uint64_t at = find_unique(image, size, kSite, sizeof(kSite));
    if (at == kNotFound) return false;
    unsigned char *stub = stub_page_near(image, size);
    if (!stub) return false;
    unsigned n = 0;
    auto put = [&](std::initializer_list<unsigned char> b) { for (unsigned char ch : b) stub[n++] = ch; };
    // keep every argument register (the function is variadic: al and xmm0-7 too), stack aligned for the call
    put({0x50, 0x57, 0x56, 0x52, 0x51, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53});   // push rax..r11 (9)
    put({0x48, 0x81, 0xec, 0x80, 0x00, 0x00, 0x00});                                     // sub rsp,0x80
    for (unsigned x = 0; x < 8; x++) put({0xf3, 0x0f, 0x7f, static_cast<unsigned char>(0x44 | (x << 3)), 0x24,
                                          static_cast<unsigned char>(x * 16)});           // movdqu [rsp+16x],xmmx
    put({0x48, 0xb8});                                                                   // mov rax, handler
    const uint64_t handler = reinterpret_cast<uint64_t>(&script_error);
    std::memcpy(stub + n, &handler, 8);
    n += 8;
    put({0xff, 0xd0});                                                                   // call rax (rdi..r9 as given)
    for (unsigned x = 0; x < 8; x++) put({0xf3, 0x0f, 0x6f, static_cast<unsigned char>(0x44 | (x << 3)), 0x24,
                                          static_cast<unsigned char>(x * 16)});           // movdqu xmmx,[rsp+16x]
    put({0x48, 0x81, 0xc4, 0x80, 0x00, 0x00, 0x00});                                     // add rsp,0x80
    put({0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x59, 0x5a, 0x5e, 0x5f, 0x58});   // pop r11..rax
    std::memcpy(stub + n, image + at, kPrologue);                                        // the prologue
    n += kPrologue;
    put({0xff, 0x25, 0, 0, 0, 0});                                                       // jmp [rip]: back
    const uint64_t back = reinterpret_cast<uint64_t>(image + at + kPrologue);
    std::memcpy(stub + n, &back, 8);
    if (!finish_stub(stub) || !jump_to(image, at, kPrologue, stub)) return false;
    std::printf("Uncharted 3: script and animation error messages go to the log (%#llx)\n",
                static_cast<unsigned long long>(at));
    std::fflush(stdout);
    return true;
}

}  // namespace u3fix
