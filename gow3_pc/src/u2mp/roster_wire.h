// u2mp party roster wire format (own design): after the message type byte, one entry per member:
//   slot, name length, rank, name bytes, hero length, hero bytes, villain length, villain bytes.
// Older senders end an entry after the name, or after the hero; the reader accepts that.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace u2mp::roster_wire {

struct Entry {
    uint8_t slot = 0;
    uint8_t rank = 0;
    std::string name;
    std::string hero;
    std::string villain;
};

// Strings longer than 255 bytes are cut (callers cut names to 31 and skins to 47 first).
inline void put(std::vector<uint8_t> &out, const Entry &e) {
    auto text = [&](const std::string &s) {
        const size_t n = s.size() < 255 ? s.size() : 255;
        out.push_back(uint8_t(n));
        out.insert(out.end(), s.begin(), s.begin() + std::ptrdiff_t(n));
    };
    const size_t n = e.name.size() < 255 ? e.name.size() : 255;
    out.push_back(e.slot);
    out.push_back(uint8_t(n));
    out.push_back(e.rank);
    out.insert(out.end(), e.name.begin(), e.name.begin() + std::ptrdiff_t(n));
    text(e.hero);
    text(e.villain);
}

// Reads the entries from d[at] on; stops at a slot above max_slot or a cut entry.
inline std::vector<Entry> read(const uint8_t *d, size_t size, size_t at, uint8_t max_slot) {
    std::vector<Entry> entries;
    while (at < size && size - at >= 3) {
        const uint8_t slot = d[at], len = d[at + 1];
        if (slot > max_slot || at + 3 + len > size) break;
        Entry e;
        e.slot = slot;
        e.rank = d[at + 2];
        e.name.assign(reinterpret_cast<const char *>(d + at + 3), len);
        at += 3 + len;
        for (std::string *s : {&e.hero, &e.villain}) {
            if (at >= size) break;
            const uint8_t n = d[at];
            if (at + 1 + n > size) {
                at = size;
                break;
            }
            s->assign(reinterpret_cast<const char *>(d + at + 1), n);
            at += 1 + n;   // the next string (or member) starts after this one
        }
        entries.push_back(std::move(e));
    }
    return entries;
}

}  // namespace u2mp::roster_wire
