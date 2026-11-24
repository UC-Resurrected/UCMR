// Uncharted 3 1.02, 16 Nov 2026: an auto exposure whose luminance filter size is 0 averages nothing (Village washed out).
//
// The render settings' tonemap-luminance-filter-size (+0x700) says over how many frames the auto exposure averages the
// measured mean luminance. The tonemap block (image 0xee00c6) hands it to the GPU clamped to 0..128 (constant dword
// 29); cs 04f81a8f keeps that many frames and averages them. With 0 it keeps none, reads a mean it never wrote (on the
// port: 0) and the exposure goes to the top of the level's light range. PS3 1.19 (0x793ec0..0x79453c) takes the
// current frame's mean when the size is 0 (and clamps sizes above 127 to 128). Village's live render settings
// (*village-multi-uc2-village-pre*) have size 0: the owner's exposure log showed frame means of 0.20..0.25, a filtered
// mean of 0 and the EV at 4.875, the top of its 2.375..4.875 range, all match long. The clamp becomes 1..128: one frame
// averaged is the current frame's mean, as on PS3; every other size is unchanged.
#include "u3fix.h"
#include "u3fix_hook.h"

#include <cstdio>
#include <cstring>

namespace u3fix {

namespace {

// mov ecx, [r12+0x700]; xor edx, edx; test ecx, ecx; cmovns edx, ecx; cmp edx, 0x81; mov ecx, 0x80; cmovl ecx, edx;
// mov [rbx+0x74], ecx
constexpr unsigned char kSite[] = {0x41, 0x8b, 0x8c, 0x24, 0x00, 0x07, 0x00, 0x00, 0x31, 0xd2, 0x85, 0xc9, 0x0f, 0x49, 0xd1,
                                   0x81, 0xfa, 0x81, 0x00, 0x00, 0x00, 0xb9, 0x80, 0x00, 0x00, 0x00, 0x0f, 0x4c, 0xca, 0x89,
                                   0x4b, 0x74};
constexpr unsigned kFrom = 8, kLength = 21;   // xor edx, edx .. cmovl ecx, edx
// mov edx, 1; cmp ecx, edx; cmovl ecx, edx; mov edx, 0x80; cmp ecx, edx; cmovg ecx, edx; nop  (ecx = clamp(size, 1, 128))
constexpr unsigned char kClamp[kLength] = {0xba, 0x01, 0x00, 0x00, 0x00, 0x39, 0xd1, 0x0f, 0x4c, 0xca, 0xba,
                                           0x80, 0x00, 0x00, 0x00, 0x39, 0xd1, 0x0f, 0x4f, 0xca, 0x90};

}  // namespace

bool exposure_filter_at_least_one(unsigned char *image, uint64_t size) {
    const uint64_t site = find_unique(image, size, kSite, sizeof(kSite));
    if (site == kNotFound) return false;
    std::memcpy(image + site + kFrom, kClamp, kLength);
    std::printf("Uncharted 3: auto exposure luminance filter size 0 counts as 1 frame, as on PS3 (%#llx)\n",
                static_cast<unsigned long long>(site + kFrom));
    std::fflush(stdout);
    return true;
}

}  // namespace u3fix
