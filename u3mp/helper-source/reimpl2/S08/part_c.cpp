// S08 part C: Havok packfile, answer an already loaded blob's contents (spec S08_structure_walks.md, Part C).
// The game's loader returns 0 for a blob it fixed up before (header bit 0 of +0x38). For such a blob the contents
// address is recomputed from the packfile header (public hkPackfileHeader / hkPackfileSectionHeader layout) and handed
// back as the loader's result, so the hooked call is skipped.

namespace s08_c {

constexpr uint32_t PACKFILE_MAGIC0 = 0x57e0e057u;
constexpr uint32_t PACKFILE_MAGIC1 = 0x10c0c010u;
constexpr uint32_t HEADER_SIZE = 0x40;          // section headers start here
constexpr uint32_t SECTION_HEADER_SIZE = 0x30;

}  // namespace s08_c

// r = register frame R9: r[4] = rdi (blob), r[5] = rsi (size), r[8] = rax (result when 1 is returned)
extern "C" U3X_FN int u3x_hk21_load(uint64_t *r) {
    using namespace s08_c;
    uint32_t *eb = ext_eb();
    eb[192] += 1;

    const uint64_t b = r[4];
    const uint32_t size = static_cast<uint32_t>(r[5]);
    if (b < 0x10000u || b >= (1ull << 47) || (b & 3)) return 0;

    const uint8_t *h = reinterpret_cast<const uint8_t *>(b);
    // the game's loader writes this word: read it fresh
    const uint32_t flags = *reinterpret_cast<const volatile uint32_t *>(h + 0x38);
    if (!(flags & 1)) return 0;   // first load: the game does it

    const int32_t n = rd<int32_t>(h, 0x14);   // number of sections
    const int32_t i = rd<int32_t>(h, 0x18);   // contents section index
    if (rd<uint32_t>(h, 0) != PACKFILE_MAGIC0 || rd<uint32_t>(h, 4) != PACKFILE_MAGIC1 || n <= 0 || n > 64 || i < 0 ||
        i >= n) {
        eb[194] += 1;
        return 0;
    }

    const int32_t start = rd<int32_t>(h, HEADER_SIZE + uint32_t(i) * SECTION_HEADER_SIZE + 0x14);
    const int64_t off = int64_t(start) + int64_t(rd<int32_t>(h, 0x1c));
    if (off < int64_t(HEADER_SIZE) || (size != 0 && off >= int64_t(size))) {
        eb[194] += 1;
        return 0;
    }

    r[8] = b + uint64_t(off);
    eb[193] += 1;
    eb[195] = static_cast<uint32_t>(off);
    eb[196] = static_cast<uint32_t>(b);
    return 1;
}
