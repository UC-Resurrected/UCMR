// Portable executable memory for the C++ unit tests that run code on a stand-in image.
#ifndef GOW3_TESTS_TEST_MEMORY_H
#define GOW3_TESTS_TEST_MEMORY_H
#include <cstddef>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

// size bytes of zeroed read-write-execute memory, nullptr when there is none. On Linux it is taken below 2 GiB, as
// the runtime's --low-image puts the game: the hook stubs (u3fix/hook.cpp) come from there and must be within a
// relative jump of the image.
inline unsigned char *TestCodeMemory(std::size_t size) {
#ifdef _WIN32
    return static_cast<unsigned char *>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
#else
    void *p = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    return p == MAP_FAILED ? nullptr : static_cast<unsigned char *>(p);
#endif
}

// gives back memory from TestCodeMemory
inline void TestCodeFree(unsigned char *p, std::size_t size) {
#ifdef _WIN32
    (void)size;
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, size);
#endif
}

// The test changed code at p: make sure the CPU runs the new bytes.
inline void TestCodeChanged(void *p, std::size_t size) {
#ifdef _WIN32
    FlushInstructionCache(GetCurrentProcess(), p, size);
#else
    __builtin___clear_cache(static_cast<char *>(p), static_cast<char *>(p) + size);
#endif
}
#endif  // GOW3_TESTS_TEST_MEMORY_H
