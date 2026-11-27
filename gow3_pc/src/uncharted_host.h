/* uncharted_host.h - the host function table the runtime offers the Uncharted 3 multiplayer helper's PC platform layer
 * (u3mp/helper-source/plat_pc.inc), the same on Windows and Linux (docs/WINDOWS-DEPENDENCIES.md §4.10, phase 5). The
 * helper reads the table's address from the reserved guest path UNCHARTED_HOST_PATH through the runtime's own
 * sceKernelOpen / sceKernelRead (8 bytes, little-endian), so no import has to be added to the game image.
 *
 * Every function is called from guest code with the System V ABI. The socket functions keep Winsock's shape on both
 * systems, which is what the helper was written against: a 64-bit socket handle (~0 on failure), -1 for a failing
 * call, Winsock error numbers from last_error() (Linux maps its errno values to them), Winsock / FreeBSD option
 * numbers (level 0xffff = socket, 0x1005 / 0x1006 = send / receive timeout in milliseconds), sockaddr_in layout
 * {u16 family, u16 port, u32 address, 8 zero bytes}. Append new members at the end only and raise `size`.
 * Implementation: runtime_host_api.cpp. C-compatible: the helper is C and C++. */
#ifndef UNCHARTED_HOST_H
#define UNCHARTED_HOST_H
#include <stdint.h>

#define UNCHARTED_HOST_PATH "/dev/uncharted_host"
#define UNCHARTED_HOST_VERSION 1u

#if defined(__x86_64__) && !defined(UNCHARTED_HOST_ABI)
#define UNCHARTED_HOST_ABI __attribute__((sysv_abi))
#endif

/* a helper thread's function (System V; its 32-bit result is ignored) */
typedef uint32_t (UNCHARTED_HOST_ABI *UnchartedHostThread)(void *arg);

typedef struct UnchartedHost {
    uint32_t version;   /* UNCHARTED_HOST_VERSION */
    uint32_t size;      /* sizeof(UnchartedHost) of the runtime that filled it */
    /* sockets (IPv4): Winsock's calls and results */
    int (UNCHARTED_HOST_ABI *startup)(void);   /* once before the first socket: 0 ok, else the error */
    uint64_t (UNCHARTED_HOST_ABI *socket)(int family, int type, int protocol);
    int (UNCHARTED_HOST_ABI *close)(uint64_t s);
    int (UNCHARTED_HOST_ABI *bind)(uint64_t s, const void *address, int length);
    int (UNCHARTED_HOST_ABI *connect)(uint64_t s, const void *address, int length);
    uint64_t (UNCHARTED_HOST_ABI *accept)(uint64_t s, void *address, int *length);
    int (UNCHARTED_HOST_ABI *listen)(uint64_t s, int backlog);
    int (UNCHARTED_HOST_ABI *send)(uint64_t s, const void *data, int size, int flags);
    int (UNCHARTED_HOST_ABI *recv)(uint64_t s, void *data, int size, int flags);
    int (UNCHARTED_HOST_ABI *sendto)(uint64_t s, const void *data, int size, int flags, const void *to, int length);
    int (UNCHARTED_HOST_ABI *recvfrom)(uint64_t s, void *data, int size, int flags, void *from, int *length);
    int (UNCHARTED_HOST_ABI *setsockopt)(uint64_t s, int level, int option, const void *value, int length);
    int (UNCHARTED_HOST_ABI *nonblocking)(uint64_t s, uint32_t *on);   /* ioctlsocket(FIONBIO) */
    int (UNCHARTED_HOST_ABI *getsockname)(uint64_t s, void *address, int *length);
    int (UNCHARTED_HOST_ABI *last_error)(void);                     /* WSAGetLastError() numbering */
    int (UNCHARTED_HOST_ABI *resolve)(const char *host, uint32_t *address);   /* 0 ok (network order), -1 not found */
    /* memory, threads, time */
    void *(UNCHARTED_HOST_ABI *alloc)(uint64_t bytes);              /* committed, zeroed, read/write; 0 on failure */
    int (UNCHARTED_HOST_ABI *thread)(UnchartedHostThread fn, void *arg, uint64_t stack);   /* 0 started, -1 not */
    void (UNCHARTED_HOST_ABI *sleep_ms)(uint32_t ms);
    void (UNCHARTED_HOST_ABI *utc)(uint16_t time[8]);               /* {year, month, weekday, day, hour, min, sec, ms} */
    int (UNCHARTED_HOST_ABI *random)(void *out, uint32_t size);     /* 0 ok, -1 none */
    /* the launcher's hand-off: the variable's value into out (cap bytes, NUL-ended) and the variable removed; its
     * length, 0 when it is not set (the out buffer is cleared, as the helper did) */
    uint32_t (UNCHARTED_HOST_ABI *env_take)(const char *name, char *out, uint32_t cap);
    /* n bytes appended to a file below the working folder, from any thread; 1 written, 0 not */
    int (UNCHARTED_HOST_ABI *diag_append)(const char *path, const char *text, uint32_t n);
    /* a box with one text field on its own thread (see plat.h plat_textbox_open); 1 opened */
    int (UNCHARTED_HOST_ABI *textbox)(const char *title, const char *label, const char *initial, int password, char *out,
                                      uint32_t cap, volatile uint8_t *result, volatile int32_t *busy);
    /* the voice chat table (u3voice.h), never null */
    const void *(UNCHARTED_HOST_ABI *voice)(void);
} UnchartedHost;

#ifdef __cplusplus
extern "C" {
#endif
/* runtime_host_api.cpp: a descriptor whose 8 bytes hold the table's address (the read end of a pipe), for
 * runtime_file.c's open of UNCHARTED_HOST_PATH; -1 when no pipe could be made */
int runtime_host_api_fd(void);
#ifdef __cplusplus
}
#endif
#endif
