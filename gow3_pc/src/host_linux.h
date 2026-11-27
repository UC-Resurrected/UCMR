/* host_linux.h - the runtime's Linux platform functions (host_linux.cpp, C++): the OS calls the loader and the C
 * runtime files make on Linux where Windows uses its own bodies (probe.c, runtime_file.c). Windows does not build
 * host_linux.cpp; its bodies stay where they are. Declarations only, for the C and the C++ side. */
#ifndef GOW3_HOST_LINUX_H
#define GOW3_HOST_LINUX_H
#ifndef _WIN32
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* What the signal handlers need from the loader: where the guest image is and whether the GPU library tracks guest
 * pages. Pointers to the loader's own variables, read when a signal arrives. */
typedef struct {
    unsigned char *const *image;
    const int *gpu_enabled;
} HostLinuxGuest;

/* The fault, thread-dump and watchdog handlers (the Linux counterpart of the Windows vectored exception handler):
 * SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGTRAP; SIGUSR2 dumps the receiving thread's frames; SIGALRM after
 * timeout_seconds (0: no watchdog, as on Windows) dumps every thread and ends the process. */
void host_linux_install_signals(const HostLinuxGuest *guest, unsigned timeout_seconds);

/* The in-game restart (settings menu): runs GOW3_RESTART_COMMAND through /bin/sh in place of this process. */
void host_linux_restart(void) __attribute__((noreturn));

/* bytes of fresh memory (prot: PROT_* bits) in the free space between lo and hi, the lowest address first, 64 KiB
 * aligned as on Windows; NULL when nothing fits. For stubs that must be within a relative jump of the image. */
unsigned char *host_linux_alloc_between(uintptr_t lo, uintptr_t hi, size_t bytes, int prot);

/* path is a host path whose first fixed_length bytes (a game folder) exist as given. Linux file systems keep case
 * and the PS4 (like Windows) does not: each later component that does not exist as written is looked up in its
 * folder without ASCII case, and the name found there replaces it. path is left as it is when nothing matches.
 * Returns 1 when it changed path. */
int host_linux_fold_case(char *path, size_t fixed_length);

/* In host_linux_shared.cpp, which the runtime's tests link (as they link compat_win.c on Windows) without the signal
 * handlers of host_linux.cpp: */
/* The CPU's time stamp counter frequency in Hz, measured across a 100 ms sleep (GOW3_NATIVE_TSC, runtime_kernel.c). */
uint64_t host_linux_tsc_hz(void);
/* Deletes path and everything below it (symbolic links themselves, not what they point to); 0 when it is gone or was
 * never there, -1 when something could not be deleted. The Windows body is in compat_win.c. */
int compat_remove_tree(const char *path);

#ifdef __cplusplus
}
#endif
#endif
#endif
