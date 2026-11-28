// inflate.h - raw deflate (RFC 1951) and gzip (RFC 1952) decoding with CRC-32, part of uncharted_native.dll: the
// launcher update's zip members (updater.cpp) and the game file list (gamefiles.cpp); the imports read their PS3
// archives' zlib blocks with it (u3mp importer, dctools psarc.cpp). A malformed stream throws InflateError.
//
// Huffman codes are looked up in a table of their first ROOT bits (most codes are shorter); a longer code is decoded
// bit by bit from the code counts (the canonical code). Input is loaded up to 7 bytes at a time, and a match is copied
// as a block when it does not overlap its own output. The fixed tables and the CRC table are made once, thread-safe:
// several threads may inflate at the same time.
#pragma once

#include <cstdint>
#include <cstring>

#include <algorithm>
#include <string>
#include <vector>

namespace inflate {

struct InflateError { std::string message; };

inline uint32_t crc32(uint32_t crc, const unsigned char *p, size_t n) {
    struct Crc {
        uint32_t t[256];
        Crc() {
            for (uint32_t i = 0; i < 256; ++i) {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
                t[i] = c;
            }
        }
    };
    static const Crc table;
    crc = ~crc;
    while (n--) crc = table.t[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

// Raw deflate (RFC 1951).
class Inflater {
public:
    Inflater(const unsigned char *in, size_t size, std::string &out) : in_(in), size_(size), out_(out) {}
    void run() {
        int last;
        do {
            last = (int)bits(1);
            const int type = (int)bits(2);
            if (type == 0) stored();
            else if (type == 1) codes(fixed_tables().lit, fixed_tables().dist);
            else if (type == 2) dynamic();
            else fail();
        } while (!last);
    }
    // The input bytes the stream used (after run(): its end, rounded up to the byte; a zlib checksum follows there).
    size_t consumed() const {
        const int whole = count_ / 8 - over_;   // loaded bytes not used, past the last one read from
        return at_ - (whole > 0 ? (size_t)whole : 0);
    }

private:
    static constexpr int ROOT = 10;   // bits looked up at once
    struct Table {
        uint16_t root[1 << ROOT];   // symbol << 4 | length for a code of at most `root_bits` bits, 0 = longer (or none)
        int root_bits = 1, max_len = 0;
        uint16_t count[16] = {0};   // codes of each length
        uint16_t symbol[320];       // the symbols in code order
    };
    struct Fixed {
        Table lit, dist;
        Fixed() {
            unsigned char l[288], d[30];
            for (int i = 0; i < 144; ++i) l[i] = 8;
            for (int i = 144; i < 256; ++i) l[i] = 9;
            for (int i = 256; i < 280; ++i) l[i] = 7;
            for (int i = 280; i < 288; ++i) l[i] = 8;
            for (int i = 0; i < 30; ++i) d[i] = 5;
            build(lit, l, 288);
            build(dist, d, 30);
        }
    };
    static const Fixed &fixed_tables() {
        static const Fixed f;
        return f;
    }

    const unsigned char *in_;
    size_t size_, at_ = 0;
    uint64_t buf_ = 0;
    int count_ = 0;   // bits in buf_
    int over_ = 0;    // zero bytes put in buf_ past the end of the input (only allowed while unused)
    std::string &out_;

    [[noreturn]] static void fail() { throw InflateError{"Error -3 while decompressing data: invalid block"}; }
    void need(int n) {
        if (count_ >= n) return;
        while (count_ <= 56 && at_ < size_) {
            buf_ |= (uint64_t)in_[at_++] << count_;
            count_ += 8;
        }
        while (count_ < n) {   // past the end: zero bytes, allowed only while unused
            ++over_;
            count_ += 8;
        }
    }
    void check() const {   // no bit past the end of the input was used
        if (over_ * 8 > count_) fail();
    }
    void drop(int n) {
        buf_ >>= n;
        count_ -= n;
        check();
    }
    uint32_t bits(int n) {
        if (!n) return 0;
        need(n);
        const uint32_t v = (uint32_t)(buf_ & ((1ull << n) - 1));
        drop(n);
        return v;
    }
    static void build(Table &t, const unsigned char *lengths, int n) {
        std::memset(t.count, 0, sizeof t.count);
        std::memset(t.root, 0, sizeof t.root);
        t.max_len = 0;
        for (int i = 0; i < n; ++i) {
            t.count[lengths[i]]++;
            t.max_len = std::max(t.max_len, (int)lengths[i]);
        }
        t.count[0] = 0;
        t.root_bits = std::max(1, std::min(t.max_len, ROOT));
        int code = 0, next[16] = {0}, offs[16] = {0};
        for (int len = 1; len <= 15; ++len) {
            code = (code + t.count[len - 1]) << 1;
            next[len] = code;
            offs[len] = offs[len - 1] + t.count[len - 1];
        }
        for (int sym = 0; sym < n; ++sym) {
            const int len = lengths[sym];
            if (!len) continue;
            const int c = next[len]++;
            if (c >= (1 << len)) fail();   // over-subscribed
            t.symbol[offs[len]++] = (uint16_t)sym;
            if (len > t.root_bits) continue;
            int rev = 0;
            for (int k = 0; k < len; ++k) rev |= ((c >> k) & 1) << (len - 1 - k);
            for (int fill = rev; fill < (1 << t.root_bits); fill += 1 << len) t.root[fill] = (uint16_t)(sym << 4 | len);
        }
    }
    int decode(const Table &t) {
        need(t.root_bits);
        const uint32_t e = t.root[buf_ & ((1u << t.root_bits) - 1)];
        if (e) {
            drop((int)(e & 15));
            return (int)(e >> 4);
        }
        // a code longer than the root table (or none): the canonical code, one bit at a time
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= t.max_len; ++len) {
            code |= (int)bits(1);
            const int count = t.count[len];
            if (code - first < count) return t.symbol[index + (code - first)];
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        fail();
    }
    void stored() {
        buf_ >>= count_ & 7;   // to the next byte
        count_ -= count_ & 7;
        const uint32_t len = bits(16), nlen = bits(16);
        if ((len ^ 0xffff) != nlen) fail();
        // the bytes still in the bit buffer are the stored data's first ones: step back to them
        const int buffered = count_ / 8 - over_;
        if (buffered < 0) fail();
        at_ -= (size_t)buffered;
        buf_ = 0;
        count_ = 0;
        over_ = 0;
        if (at_ + len > size_) fail();
        out_.append((const char *)in_ + at_, len);
        at_ += len;
    }
    void codes(const Table &lit, const Table &dist) {
        static const uint16_t lbase[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const uint8_t lext[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const uint16_t dbase[] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const uint8_t dext[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        while (true) {
            int sym = decode(lit);
            if (sym < 256) {
                out_.push_back((char)sym);
            } else if (sym == 256) {
                return;
            } else {
                sym -= 257;
                if (sym >= 29) fail();
                const size_t len = lbase[sym] + bits(lext[sym]);
                const int d = decode(dist);
                if (d >= 30) fail();
                const size_t distance = dbase[d] + bits(dext[d]);
                if (distance > out_.size()) fail();
                const size_t at = out_.size();
                out_.resize(at + len);
                char *dst = &out_[at];
                const char *src = dst - distance;
                if (distance >= len) std::memcpy(dst, src, len);
                else
                    for (size_t k = 0; k < len; ++k) dst[k] = src[k];   // overlapping: the run repeats
            }
        }
    }
    void dynamic() {
        static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        const int nlen = (int)bits(5) + 257, ndist = (int)bits(5) + 1, ncode = (int)bits(4) + 4;
        if (nlen > 286 || ndist > 30) fail();
        unsigned char lengths[320] = {0};
        for (int i = 0; i < ncode; ++i) lengths[order[i]] = (unsigned char)bits(3);
        Table lencode;
        build(lencode, lengths, 19);
        std::memset(lengths, 0, sizeof lengths);
        int i = 0;
        while (i < nlen + ndist) {
            int sym = decode(lencode);
            if (sym < 16) {
                lengths[i++] = (unsigned char)sym;
            } else {
                int len = 0, repeat;
                if (sym == 16) {
                    if (!i) fail();
                    len = lengths[i - 1];
                    repeat = 3 + (int)bits(2);
                } else if (sym == 17) {
                    repeat = 3 + (int)bits(3);
                } else {
                    repeat = 11 + (int)bits(7);
                }
                if (i + repeat > nlen + ndist) fail();
                while (repeat--) lengths[i++] = (unsigned char)len;
            }
        }
        if (!lengths[256]) fail();
        Table lit, dist;
        build(lit, lengths, nlen);
        build(dist, lengths + nlen, ndist);
        codes(lit, dist);
    }
};

// A gzip file's content (RFC 1952: one member, its CRC-32 and size checked).
inline std::string gunzip(const std::string &gz) {
    auto byte = [&](size_t at) -> unsigned {
        if (at >= gz.size()) throw InflateError{"Not a gzipped file (truncated header)"};
        return (unsigned char)gz[at];
    };
    if (byte(0) != 0x1f || byte(1) != 0x8b) throw InflateError{"Not a gzipped file (bad magic)"};
    if (byte(2) != 8) throw InflateError{"Unknown compression method"};
    const unsigned flags = byte(3);
    size_t at = 10;
    if (flags & 4) at += 2 + (byte(at) | byte(at + 1) << 8);   // FEXTRA
    if (flags & 8) { while (byte(at)) ++at; ++at; }              // FNAME
    if (flags & 16) { while (byte(at)) ++at; ++at; }             // FCOMMENT
    if (flags & 2) at += 2;                                      // FHCRC
    if (gz.size() < at + 8) throw InflateError{"Compressed file ended before the end-of-stream marker was reached"};
    std::string out;
    Inflater(reinterpret_cast<const unsigned char *>(gz.data()) + at, gz.size() - at - 8, out).run();
    const unsigned char *tail = reinterpret_cast<const unsigned char *>(gz.data()) + gz.size() - 8;
    const uint32_t crc = tail[0] | tail[1] << 8 | tail[2] << 16 | (uint32_t)tail[3] << 24;
    const uint32_t size = tail[4] | tail[5] << 8 | tail[6] << 16 | (uint32_t)tail[7] << 24;
    if (crc != crc32(0, reinterpret_cast<const unsigned char *>(out.data()), out.size()))
        throw InflateError{"CRC check failed"};
    if (size != (uint32_t)out.size()) throw InflateError{"Incorrect length of data produced"};
    return out;
}

}  // namespace inflate
