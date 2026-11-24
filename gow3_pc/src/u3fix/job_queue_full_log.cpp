// Uncharted 3 1.02, 19 Nov 2026: report jobs the job system drops because their queue is full.
//
// The job system keeps one queue per priority, each with room for 1024 jobs. Adding a job to a queue that already holds
// more than 1023 (image 0xda57fa) gives up and returns false, and the code that runs jobs ignores that result: the job
// never runs and whoever waits for its counter waits for ever. A PS4 never fills a queue. Syria co-op froze at stage 7
// with the audio thread waiting for its mixing job, which no worker ever started; this report shows whether the queue
// was full. The way out stays as it was (the job is still dropped); only a line is printed. Found by its bytes.
#include "u3fix.h"
#include "u3fix_hook.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace u3fix {

namespace {

std::atomic<uint64_t> g_dropped{0};
const uint64_t *g_manager = nullptr;   // the job manager pointer (a global in the image)

__attribute__((sysv_abi)) void job_queue_full(int64_t priority, uint64_t count, const uint64_t *job) {
    const uint64_t n = ++g_dropped;
    if (n <= 20 || n % 1000 == 0) {
        std::printf("Uncharted 3: job dropped, queue of priority %lld is full (%llu jobs); job %p first word %#llx "
                    "(time %llu)\n",
                    static_cast<long long>(priority), static_cast<unsigned long long>(count),
                    static_cast<const void *>(job), static_cast<unsigned long long>(job ? job[0] : 0),
                    static_cast<unsigned long long>(n));
        std::fflush(stdout);
    }
}

// movsxd rax,[rdi+0x4c]; lea r14,[manager]; mov rcx,[r14]; mov rcx,[rcx+0x4f10]; imul r9,rax,0x2078;
// mov rbx,[rcx+r9]; cmp rbx,0x3ff; jbe +7; xor ebx,ebx; jmp exit
constexpr unsigned char kSite[] = {0x48, 0x63, 0x47, 0x4c, 0x4c, 0x8d, 0x35, 0x1b, 0x37, 0xa6, 0x00, 0x49,
                                   0x8b, 0x0e, 0x48, 0x8b, 0x89, 0x10, 0x4f, 0x00, 0x00, 0x4c, 0x69, 0xc8,
                                   0x78, 0x20, 0x00, 0x00, 0x4a, 0x8b, 0x1c, 0x09, 0x48, 0x81, 0xfb, 0xff,
                                   0x03, 0x00, 0x00, 0x76, 0x07, 0x31, 0xdb, 0xe9, 0xfe, 0x01, 0x00, 0x00};
constexpr unsigned kFail = 41;           // xor ebx,ebx; jmp exit
constexpr unsigned kFailLength = 7;
constexpr unsigned kExit = kFail + kFailLength + 0x1fe;

}  // namespace

// the number of jobs in each of the 8 priority queues, as text (empty before the hook is installed)
void job_queue_counts(char *out, size_t size) {
    if (!size) return;
    out[0] = 0;
    if (!g_manager || !*g_manager) return;
    const unsigned char *manager = reinterpret_cast<const unsigned char *>(*g_manager);
    const unsigned char *queues = nullptr;
    std::memcpy(&queues, manager + 0x4f10, sizeof queues);
    if (!queues) return;
    size_t used = 0;
    for (int p = 0; p < 8 && used < size; p++) {
        uint64_t count = 0;
        std::memcpy(&count, queues + p * 0x2078, sizeof count);
        used += std::snprintf(out + used, size - used, "%s%llu", p ? " " : "", static_cast<unsigned long long>(count));
    }
}

bool install_job_queue_full_log(unsigned char *image, uint64_t size) {
    const uint64_t at = find_unique(image, size, kSite, sizeof(kSite));
    if (at == kNotFound) return false;
    unsigned char *stub = stub_page_near(image, size);
    if (!stub) return false;
    {   // lea r14,[rip+disp] at +4 (7 bytes): the job manager pointer
        int32_t disp = 0;
        std::memcpy(&disp, image + at + 7, 4);
        g_manager = reinterpret_cast<const uint64_t *>(image + at + 11 + disp);
    }
    unsigned n = 0;
    auto put = [&](std::initializer_list<unsigned char> bytes) {
        for (unsigned char b : bytes) stub[n++] = b;
    };
    put({0x55, 0x48, 0x89, 0xe5});                                                 // push rbp; mov rbp,rsp
    put({0x50, 0x51, 0x52, 0x56, 0x57, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53});   // push rax..r11
    put({0x48, 0x83, 0xe4, 0xf0});                                                 // and rsp,-16
    put({0x48, 0x89, 0xfa});                                                       // mov rdx,rdi (the job)
    put({0x48, 0x89, 0xc7});                                                       // mov rdi,rax (the priority)
    put({0x48, 0x89, 0xde});                                                       // mov rsi,rbx (the count)
    const uint64_t handler = reinterpret_cast<uint64_t>(&job_queue_full);
    put({0x48, 0xb8});                                                             // mov rax, handler
    std::memcpy(stub + n, &handler, 8);
    n += 8;
    put({0xff, 0xd0});                                                             // call rax
    put({0x48, 0x8d, 0x65, 0xb8});                                                 // lea rsp,[rbp-0x48]
    put({0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5f, 0x5e, 0x5a, 0x59, 0x58});   // pop r11..rax
    put({0x5d});                                                                   // pop rbp
    put({0x31, 0xdb});                                                             // xor ebx,ebx (as before)
    put({0xff, 0x25, 0, 0, 0, 0});                                                 // jmp [rip]: the exit
    const uint64_t back = reinterpret_cast<uint64_t>(image + at + kExit);
    std::memcpy(stub + n, &back, 8);
    n += 8;
    if (!finish_stub(stub) || !jump_to(image, at + kFail, kFailLength, stub)) return false;
    std::printf("Uncharted 3: dropped jobs (full job queue) are reported (%#llx)\n",
                static_cast<unsigned long long>(at + kFail));
    std::fflush(stdout);
    return true;
}

}  // namespace u3fix
