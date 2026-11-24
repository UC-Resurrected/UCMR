// Uncharted 3 1.02, 20 Nov 2026: the audio driver mixes on its own thread instead of through a job.
//
// For every audio tick the sound driver thread (brbDriver) hands the game's mixing callback to the job system as one job
// (image 0xfe5370: start the job 0xffc4c0 with the callback block, then wait for it) and does nothing until it is done.
// In Syria co-op that job was sometimes never run, the driver thread waited for ever and the game froze behind it
// (20 Nov 2026, four logs). Since the driver only waits, running the job's work right there gives the same result in the
// same order: the start of 0xfe5370 now jumps to the job's own function 0xffc4c0 with the same callback block, which
// calls the mixing callback and releases its lock as the job did. Found by its bytes.
#include "u3fix.h"
#include "u3fix_hook.h"

#include <climits>
#include <cstdio>
#include <cstring>

namespace u3fix {

namespace {

// push rbp; mov rbp,rsp; push rbx; and rsp,-0x40; sub rsp,0xc0; lea rbx,[canary]
constexpr unsigned char kSubmit[] = {0x55, 0x48, 0x89, 0xe5, 0x53, 0x48, 0x81, 0xe4, 0xc0, 0xff, 0xff, 0xff,
                                     0x48, 0x81, 0xec, 0xc0, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x1d, 0x9e, 0x1d};
// push rbp; mov rbp,rsp; mov rax,rdi; test rax,rax; je +0x5a; mov rdi,[rax+8]
constexpr unsigned char kJob[] = {0x55, 0x48, 0x89, 0xe5, 0x48, 0x89, 0xf8, 0x48,
                                  0x85, 0xc0, 0x74, 0x5a, 0x48, 0x8b, 0x78, 0x08};

}  // namespace

bool install_audio_mix_direct(unsigned char *image, uint64_t size) {
    const uint64_t submit = find_unique(image, size, kSubmit, sizeof(kSubmit));
    const uint64_t job = find_unique(image, size, kJob, sizeof(kJob));
    if (submit == kNotFound || job == kNotFound) return false;
    const int64_t rel = static_cast<int64_t>(job) - static_cast<int64_t>(submit + 5);
    if (rel < INT32_MIN || rel > INT32_MAX) return false;
    const int32_t r = static_cast<int32_t>(rel);
    image[submit] = 0xe9;   // jmp job (same rdi, same stack: the job's function returns to the driver)
    std::memcpy(image + submit + 1, &r, 4);
    std::printf("Uncharted 3: audio mixing runs on the sound driver thread (%#llx -> %#llx)\n",
                static_cast<unsigned long long>(submit), static_cast<unsigned long long>(job));
    std::fflush(stdout);
    return true;
}

}  // namespace u3fix
