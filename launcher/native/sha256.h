// sha256.h - SHA-256 (FIPS 180-4) for the launcher: the game starter checks the multiplayer image's digest before it
// boots the game; the Linux imports hash every file they make with it. Portable, so the same code serves Windows and
// Linux; no bcrypt, no OpenSSL. On an x86-64 CPU with the SHA extensions the blocks are hashed with them (chosen once at
// run time; the same digests), else in plain C++.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#if (defined(__x86_64__) || defined(_M_X64)) && (defined(__clang__) || defined(__GNUC__))
#define SHA256_X86 1
#include <cpuid.h>
#include <immintrin.h>
#endif

namespace sha256 {

#ifdef SHA256_X86
// the CPU has the SHA extensions (CPUID 7.0 EBX bit 29) and SSSE3 / SSE4.1 (CPUID 1 ECX bits 9 and 19)
inline bool has_sha_ni() {
    static const bool yes = [] {
        unsigned a = 0, b = 0, c = 0, d = 0;
        if (!__get_cpuid(1, &a, &b, &c, &d) || !(c & (1u << 9)) || !(c & (1u << 19))) return false;
        if (!__get_cpuid_count(7, 0, &a, &b, &c, &d)) return false;
        return (b & (1u << 29)) != 0;
    }();
    return yes;
}

// `blocks` 64-byte blocks into the state with the SHA extensions: four rounds per step (two SHA256RNDS2), the message
// schedule from SHA256MSG1 / SHA256MSG2
__attribute__((target("sha,sse4.1,ssse3"))) inline void blocks_sha_ni(uint32_t state[8], const uint8_t *p, size_t blocks,
                                                                     const uint32_t *k) {
    const __m128i swap = _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);   // big-endian words
    __m128i t = _mm_loadu_si128(reinterpret_cast<const __m128i *>(state));
    __m128i s1 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(state + 4));
    t = _mm_shuffle_epi32(t, 0xb1);                 // CDAB
    s1 = _mm_shuffle_epi32(s1, 0x1b);               // EFGH
    __m128i s0 = _mm_alignr_epi8(t, s1, 8);         // ABEF
    s1 = _mm_blend_epi16(s1, t, 0xf0);              // CDGH
    for (; blocks; --blocks, p += 64) {
        const __m128i save0 = s0, save1 = s1;
        __m128i w[16];
        for (int i = 0; i < 16; ++i) {
            if (i < 4) w[i] = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i *>(p + 16 * i)), swap);
            else
                w[i] = _mm_sha256msg2_epu32(_mm_add_epi32(_mm_sha256msg1_epu32(w[i - 4], w[i - 3]), _mm_alignr_epi8(w[i - 1], w[i - 2], 4)),
                                            w[i - 1]);
            __m128i m = _mm_add_epi32(w[i], _mm_loadu_si128(reinterpret_cast<const __m128i *>(k + 4 * i)));
            s1 = _mm_sha256rnds2_epu32(s1, s0, m);
            m = _mm_shuffle_epi32(m, 0x0e);
            s0 = _mm_sha256rnds2_epu32(s0, s1, m);
        }
        s0 = _mm_add_epi32(s0, save0);
        s1 = _mm_add_epi32(s1, save1);
    }
    t = _mm_shuffle_epi32(s0, 0x1b);                // FEBA
    s1 = _mm_shuffle_epi32(s1, 0xb1);               // DCHG
    s0 = _mm_blend_epi16(t, s1, 0xf0);              // DCBA
    s1 = _mm_alignr_epi8(s1, t, 8);                 // HGFE
    _mm_storeu_si128(reinterpret_cast<__m128i *>(state), s0);
    _mm_storeu_si128(reinterpret_cast<__m128i *>(state + 4), s1);
}
#endif

class Hash {
public:
    Hash() { reset(); }

    void reset() {
        static const uint32_t start[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        std::memcpy(h_, start, sizeof h_);
        bits_ = 0;
        have_ = 0;
    }

    void update(const void *data, size_t n) {
        const uint8_t *p = (const uint8_t *)data;
        bits_ += (uint64_t)n * 8;
        if (have_) {
            const size_t take = std::min(n, sizeof buf_ - have_);
            std::memcpy(buf_ + have_, p, take);
            have_ += take;
            p += take;
            n -= take;
            if (have_ == sizeof buf_) {
                blocks(buf_, 1);
                have_ = 0;
            }
        }
        if (n >= sizeof buf_) {
            const size_t whole = n / sizeof buf_;
            blocks(p, whole);
            p += whole * sizeof buf_;
            n -= whole * sizeof buf_;
        }
        if (n) {
            std::memcpy(buf_, p, n);
            have_ = n;
        }
    }

    // the 32 digest bytes; the hash is reset for the next message
    void finish(uint8_t out[32]) {
        const uint64_t bits = bits_;
        const size_t rem = (size_t)((bits / 8) % sizeof buf_);
        uint8_t pad[sizeof buf_ * 2];
        size_t n = 0;
        pad[n++] = 0x80;
        const size_t zeros = (rem < sizeof buf_ - 8 ? sizeof buf_ - 8 - rem : 2 * sizeof buf_ - 8 - rem) - 1;
        std::memset(pad + n, 0, zeros);
        n += zeros;
        for (int i = 7; i >= 0; --i) pad[n++] = (uint8_t)(bits >> (8 * i));
        update(pad, n);
        for (int i = 0; i < 8; ++i) {
            out[4 * i] = (uint8_t)(h_[i] >> 24);
            out[4 * i + 1] = (uint8_t)(h_[i] >> 16);
            out[4 * i + 2] = (uint8_t)(h_[i] >> 8);
            out[4 * i + 3] = (uint8_t)h_[i];
        }
        reset();
    }

    // the digest as 64 lowercase hex digits; the hash is reset for the next message
    std::string hex() {
        uint8_t d[32];
        finish(d);
        static const char digits[] = "0123456789abcdef";
        std::string s;
        s.reserve(64);
        for (uint8_t b : d) {
            s += digits[b >> 4];
            s += digits[b & 15];
        }
        return s;
    }

    // the plain C++ path (also what the SHA extensions are checked against)
    static bool &portable_only() {
        static bool only = false;
        return only;
    }

private:
    static uint32_t rotr(uint32_t v, int by) { return (v >> by) | (v << (32 - by)); }

    void blocks(const uint8_t *p, size_t n) {
#ifdef SHA256_X86
        if (!portable_only() && has_sha_ni()) {
            blocks_sha_ni(h_, p, n, K);
            return;
        }
#endif
        for (; n; --n, p += 64) block(p);
    }

    void block(const uint8_t *p) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = h + s1 + ch + K[i] + w[i];
            const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = s0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h_[0] += a;
        h_[1] += b;
        h_[2] += c;
        h_[3] += d;
        h_[4] += e;
        h_[5] += f;
        h_[6] += g;
        h_[7] += h;
    }

    inline static constexpr uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

    uint32_t h_[8];
    uint64_t bits_ = 0;
    uint8_t buf_[64];
    size_t have_ = 0;
};

}  // namespace sha256
