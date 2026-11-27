// The host function table of the U3 helper (src/uncharted_host.h, src/runtime_host_api.cpp): read through its
// descriptor as the helper does, then called with Winsock's conventions, which both systems must give alike.
#include "../src/uncharted_host.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <thread>
#include <chrono>

#ifdef _WIN32
#include <io.h>
#define READ _read
#define CLOSE _close
#else
#include <sys/mman.h>
#include <unistd.h>
#define READ read
#define CLOSE close
// the runtime's low-memory map (runtime_memory.c), which the table's alloc uses on Linux
extern "C" void *runtime_low_map(size_t size, int prot) {
    void *p = mmap(nullptr, size, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}
#endif
#include "test_environment.h"   // SetTestEnv

// u3voice's table: not part of this test
extern "C" const void *uncharted_u3_voice(void) {
    static int api;
    return &api;
}

namespace {

struct Sa { uint16_t family, port; uint32_t addr; uint8_t zero[8]; };
Sa loopback(uint16_t port_be) { return Sa{2, port_be, 0x0100007fu, {}}; }
volatile uint32_t g_thread_ran;
uint32_t UNCHARTED_HOST_ABI thread_fn(void *arg) {
    g_thread_ran = *(uint32_t *)arg;
    return 0;
}

}  // namespace

int main() {
    const int fd = runtime_host_api_fd();
    assert(fd >= 0);
    uint64_t address = 0;
    assert(READ(fd, &address, 8) == 8);
    char more;
    assert(READ(fd, &more, 1) == 0);   // exactly the 8 bytes
    CLOSE(fd);
    const UnchartedHost *h = (const UnchartedHost *)(uintptr_t)address;
    assert(h->version == UNCHARTED_HOST_VERSION && h->size == sizeof(UnchartedHost));
    assert(h->startup() == 0);

    // UDP on loopback: bind to an OS-chosen port, find it with getsockname, send to it, receive with the sender's address
    const uint64_t u = h->socket(2, 2, 17);
    assert(u != ~(uint64_t)0);
    Sa any = loopback(0);
    assert(h->bind(u, &any, 16) == 0);
    Sa self{};
    int length = 16;
    assert(h->getsockname(u, &self, &length) == 0 && self.port != 0 && length == 16);
    const uint64_t v = h->socket(2, 2, 17);
    assert(h->sendto(v, "hello", 5, 0, &self, 16) == 5);
    char buf[64] = {};
    Sa from{};
    int from_length = 16;
    assert(h->recvfrom(u, buf, sizeof buf, 0, &from, &from_length) == 5 && !std::memcmp(buf, "hello", 5));
    assert(from.family == 2 && from.addr == 0x0100007fu);
    // a receive timeout (Winsock milliseconds) ends with WSAETIMEDOUT
    uint32_t ms = 200;
    assert(h->setsockopt(u, 0xffff, 0x1006, &ms, 4) == 0);
    const auto start = std::chrono::steady_clock::now();
    assert(h->recvfrom(u, buf, sizeof buf, 0, &from, &from_length) == -1 && h->last_error() == 10060);
    const double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    assert(waited > 0.1 && waited < 2.0);
    // non-blocking: nothing to read is WSAEWOULDBLOCK
    uint32_t on = 1;
    assert(h->nonblocking(u, &on) == 0);
    assert(h->recv(u, buf, sizeof buf, 0) == -1 && h->last_error() == 10035);
    // socket options the helper passes on: SO_BROADCAST, SO_REUSEADDR; unknown ones fail as on Winsock (WSAEINVAL at the
    // socket and TCP levels and for an unknown level, WSAENOPROTOOPT at the IP level)
    int yes = 1;
    assert(h->setsockopt(v, 0xffff, 0x0020, &yes, 4) == 0);
    assert(h->setsockopt(v, 0xffff, 0x0004, &yes, 4) == 0);
    assert(h->setsockopt(v, 0xffff, 0x7777, &yes, 4) == -1 && h->last_error() == 10022);
    assert(h->setsockopt(v, 0xffff, 0x200, &yes, 4) == -1 && h->last_error() == 10022);   // FreeBSD SO_REUSEPORT
    assert(h->setsockopt(v, 0, 0x77, &yes, 4) == -1 && h->last_error() == 10042);
    assert(h->setsockopt(v, 6, 0x77, &yes, 4) == -1 && h->last_error() == 10022);
    assert(h->setsockopt(v, 0x99, 1, &yes, 4) == -1 && h->last_error() == 10022);
    assert(h->close(u) == 0 && h->close(v) == 0);

    // TCP: listen, connect, accept, both ways, MSG_PEEK, orderly close reads 0
    const uint64_t l = h->socket(2, 1, 6);
    Sa lany = loopback(0);
    assert(h->bind(l, &lany, 16) == 0 && h->listen(l, 4) == 0);
    Sa lself{};
    length = 16;
    assert(h->getsockname(l, &lself, &length) == 0);
    const uint64_t c = h->socket(2, 1, 6);
    assert(h->setsockopt(c, 6, 1, &yes, 4) == 0);   // TCP_NODELAY
    assert(h->connect(c, &lself, 16) == 0);
    Sa peer{};
    int peer_length = 16;
    const uint64_t a = h->accept(l, &peer, &peer_length);
    assert(a != ~(uint64_t)0 && peer.addr == 0x0100007fu);
    assert(h->send(c, "abc", 3, 0) == 3);
    assert(h->recv(a, buf, 3, 2) == 3 && !std::memcmp(buf, "abc", 3));   // MSG_PEEK
    assert(h->recv(a, buf, 3, 0) == 3);
    assert(h->send(a, "xy", 2, 0) == 2 && h->recv(c, buf, 2, 0) == 2 && !std::memcmp(buf, "xy", 2));
    assert(h->close(c) == 0);
    assert(h->recv(a, buf, sizeof buf, 0) == 0);
    // a send to a peer that is gone fails instead of ending the process (SIGPIPE on Linux)
    for (int i = 0; i < 64 && h->send(a, "zzzz", 4, 0) == 4;) ++i;
    assert(h->close(a) == 0 && h->close(l) == 0);
    // connect to a port nobody listens on: WSAECONNREFUSED
    const uint64_t r = h->socket(2, 1, 6);
    Sa closed = lself;
    assert(h->connect(r, &closed, 16) == -1 && h->last_error() == 10061);
    h->close(r);

    uint32_t ip = 0;
    assert(h->resolve("localhost", &ip) == 0 && (ip & 0xff) == 127);

    // memory, threads, time, randomness
    uint8_t *m = (uint8_t *)h->alloc(1 << 20);
    assert(m && m[0] == 0 && m[(1 << 20) - 1] == 0);
    m[12345] = 7;
    uint32_t value = 0x1234;
    assert(h->thread(thread_fn, &value, 0x40000) == 0);
    for (int i = 0; i < 200 && g_thread_ran != 0x1234; ++i) h->sleep_ms(10);
    assert(g_thread_ran == 0x1234);
    uint16_t t[8] = {};
    h->utc(t);
    const std::time_t now = std::time(nullptr);
    std::tm g{};
#ifdef _WIN32
    gmtime_s(&g, &now);
#else
    gmtime_r(&now, &g);
#endif
    assert(t[0] == g.tm_year + 1900 && t[1] == g.tm_mon + 1 && t[2] == g.tm_wday && t[3] == g.tm_mday && t[7] < 1000);
    uint8_t random[32] = {};
    assert(h->random(random, sizeof random) == 0);
    int nonzero = 0;
    for (uint8_t b : random) nonzero += b != 0;
    assert(nonzero > 8);

    // the launcher hand-off: read once, then gone; too long for the buffer: removed as well, 0
    SetTestEnv("U3MP_TEST_HANDOFF", "token-123");
    char out[32];
    assert(h->env_take("U3MP_TEST_HANDOFF", out, sizeof out) == 9 && !std::strcmp(out, "token-123"));
    assert(h->env_take("U3MP_TEST_HANDOFF", out, sizeof out) == 0 && out[0] == 0);
    SetTestEnv("U3MP_TEST_HANDOFF", "0123456789");
    assert(h->env_take("U3MP_TEST_HANDOFF", out, 5) == 0 && out[0] == 0);
    assert(h->env_take("U3MP_TEST_HANDOFF", out, sizeof out) == 0);

    // appending from any thread to a file below the working folder
    std::remove("host-api-test.txt");
    assert(h->diag_append("host-api-test.txt", "one\n", 4) == 1 && h->diag_append("host-api-test.txt", "two\n", 4) == 1);
    std::FILE *f = std::fopen("host-api-test.txt", "rb");
    assert(f);
    const size_t got = std::fread(buf, 1, sizeof buf, f);
    std::fclose(f);
    std::remove("host-api-test.txt");
    assert(got == 8 && !std::memcmp(buf, "one\ntwo\n", 8));

    assert(h->voice() == uncharted_u3_voice());
    std::puts("host-api-test ok");
    return 0;
}
