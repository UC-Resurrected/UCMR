// png.h - PNG pictures in and out for the launcher core on Linux, where Windows uses WIC: the Steam artwork
// (steam.cpp) and the game window's icon (starter_files.cpp). Header-only, over zlib. Reads every PNG of the
// specification (bit depths 1 to 16; grey, RGB, palette, grey + alpha, RGBA; Adam7 interlacing) into 8-bit RGBA;
// writes 8-bit RGB or RGBA, each row with the filter that leaves the smallest sum of byte values, at zlib's best level.
#pragma once

#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace png {

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;   // four bytes per pixel, rows from the top down
};

inline uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

// the predictor of the Paeth filter (a: left, b: up, c: up-left)
inline uint8_t paeth(int a, int b, int c) {
    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return (uint8_t)(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

// undoes the filters of `rows` rows of `stride` bytes, each row led by its filter type; bpp: bytes per pixel, at
// least 1. false on an unknown filter type.
inline bool unfilter(uint8_t *data, size_t rows, size_t stride, size_t bpp) {
    std::vector<uint8_t> zero(stride, 0);
    const uint8_t *prior = zero.data();
    for (size_t y = 0; y < rows; ++y) {
        uint8_t *row = data + y * (stride + 1);
        const uint8_t type = row[0];
        uint8_t *x = row + 1;
        for (size_t i = 0; i < stride; ++i) {
            const int a = i >= bpp ? x[i - bpp] : 0, b = prior[i], c = i >= bpp ? prior[i - bpp] : 0;
            switch (type) {
            case 0: break;
            case 1: x[i] = (uint8_t)(x[i] + a); break;
            case 2: x[i] = (uint8_t)(x[i] + b); break;
            case 3: x[i] = (uint8_t)(x[i] + ((a + b) >> 1)); break;
            case 4: x[i] = (uint8_t)(x[i] + paeth(a, b, c)); break;
            default: return false;
            }
        }
        prior = x;
    }
    return true;
}

// a PNG file's bytes as 8-bit RGBA (transparency from an alpha channel or a tRNS chunk); false when it is not a PNG
// this reads (or larger than 16384 pixels a side)
inline bool decode(const std::string &file, Image &image) {
    if (file.size() < 8 + 25 || file.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) != 0) return false;
    const uint8_t *d = (const uint8_t *)file.data();
    uint32_t w = 0, h = 0;
    int depth = 0, type = -1, interlace = 0;
    std::vector<uint8_t> palette, alpha, packed;
    for (size_t at = 8; at + 12 <= file.size();) {
        const uint32_t n = be32(d + at);
        if (n > file.size() - at - 12) return false;
        const std::string kind(file, at + 4, 4);
        const uint8_t *body = d + at + 8;
        if (kind == "IHDR" && n >= 13) {
            w = be32(body);
            h = be32(body + 4);
            depth = body[8];
            type = body[9];
            interlace = body[12];
            if (body[10] != 0 || body[11] != 0 || interlace > 1) return false;
        } else if (kind == "PLTE") {
            palette.assign(body, body + n);
        } else if (kind == "tRNS") {
            alpha.assign(body, body + n);
        } else if (kind == "IDAT") {
            packed.insert(packed.end(), body, body + n);
        } else if (kind == "IEND") {
            break;
        }
        at += 12 + n;
    }
    if (!w || !h || w > 16384 || h > 16384) return false;
    int channels;
    switch (type) {
    case 0: case 3: channels = 1; break;
    case 2: channels = 3; break;
    case 4: channels = 2; break;
    case 6: channels = 4; break;
    default: return false;
    }
    const bool depth_ok = type == 0 ? (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16)
                        : type == 3 ? (depth == 1 || depth == 2 || depth == 4 || depth == 8)
                                    : (depth == 8 || depth == 16);
    if (!depth_ok || (type == 3 && palette.size() < 3)) return false;
    const size_t bits = (size_t)channels * depth, bpp = std::max<size_t>(1, bits / 8);
    // the passes: the whole picture, or Adam7's seven (first column and row, column and row steps)
    static const uint32_t adam7[7][4] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4},
                                         {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};
    struct Pass { uint32_t x0, y0, dx, dy, w, h; };
    std::vector<Pass> passes;
    if (!interlace) {
        passes.push_back({0, 0, 1, 1, w, h});
    } else {
        for (auto &a : adam7)
            passes.push_back({a[0], a[1], a[2], a[3], w > a[0] ? (w - a[0] + a[2] - 1) / a[2] : 0,
                              h > a[1] ? (h - a[1] + a[3] - 1) / a[3] : 0});
    }
    size_t total = 0;
    for (const Pass &p : passes)
        if (p.w && p.h) total += (size_t)p.h * (1 + (p.w * bits + 7) / 8);
    std::vector<uint8_t> raw(total);
    uLongf got = (uLongf)raw.size();
    if (uncompress(raw.data(), &got, packed.data(), (uLong)packed.size()) != Z_OK || got != raw.size()) return false;
    // a tRNS chunk: one grey or one RGB value (16 bits each) that is transparent, or an alpha per palette entry
    auto grey_key = [&](unsigned v) { return alpha.size() >= 2 && ((unsigned)alpha[0] << 8 | alpha[1]) == v; };
    image.w = (int)w;
    image.h = (int)h;
    image.rgba.assign((size_t)w * h * 4, 255);
    size_t offset = 0;
    for (const Pass &p : passes) {
        if (!p.w || !p.h) continue;
        const size_t stride = (p.w * bits + 7) / 8;
        uint8_t *data = raw.data() + offset;
        if (!unfilter(data, p.h, stride, bpp)) return false;
        offset += (size_t)p.h * (stride + 1);
        for (uint32_t y = 0; y < p.h; ++y) {
            const uint8_t *row = data + (size_t)y * (stride + 1) + 1;
            // the raw value of sample k of the row (depth bits, the most significant first)
            auto value = [&](size_t k) -> unsigned {
                if (depth == 16) return (unsigned)row[k * 2] << 8 | row[k * 2 + 1];
                if (depth == 8) return row[k];
                const size_t bit = k * depth;
                return (row[bit / 8] >> (8 - depth - bit % 8)) & ((1u << depth) - 1);
            };
            auto eight = [&](unsigned v) -> uint8_t {   // a raw value as 8 bits
                return depth == 16 ? (uint8_t)(v >> 8) : depth == 8 ? (uint8_t)v : (uint8_t)(v * 255 / ((1u << depth) - 1));
            };
            for (uint32_t x = 0; x < p.w; ++x) {
                uint8_t *o = &image.rgba[(((size_t)(p.y0 + y * p.dy)) * w + (p.x0 + x * p.dx)) * 4];
                const size_t k = (size_t)x * channels;
                if (type == 3) {
                    const unsigned i = value(x);
                    if ((size_t)i * 3 + 2 < palette.size()) {
                        o[0] = palette[i * 3];
                        o[1] = palette[i * 3 + 1];
                        o[2] = palette[i * 3 + 2];
                    } else {
                        o[0] = o[1] = o[2] = 0;
                    }
                    if (i < alpha.size()) o[3] = alpha[i];
                } else if (type == 0 || type == 4) {
                    const unsigned v = value(k);
                    o[0] = o[1] = o[2] = eight(v);
                    if (type == 4) o[3] = eight(value(k + 1));
                    else if (grey_key(v)) o[3] = 0;
                } else {
                    const unsigned r = value(k), g = value(k + 1), b = value(k + 2);
                    o[0] = eight(r);
                    o[1] = eight(g);
                    o[2] = eight(b);
                    if (type == 6) o[3] = eight(value(k + 3));
                    else if (alpha.size() >= 6 && ((unsigned)alpha[0] << 8 | alpha[1]) == r &&
                             ((unsigned)alpha[2] << 8 | alpha[3]) == g && ((unsigned)alpha[4] << 8 | alpha[5]) == b)
                        o[3] = 0;
                }
            }
        }
    }
    return true;
}

inline void put_chunk(std::string &out, const char *kind, const std::string &body) {
    auto u32 = [&](uint32_t v) { for (int s = 24; s >= 0; s -= 8) out += (char)(v >> s); };
    u32((uint32_t)body.size());
    const size_t start = out.size();
    out.append(kind, 4);
    out += body;
    u32((uint32_t)crc32(0, (const Bytef *)out.data() + start, (uInt)(out.size() - start)));
}

// the picture as a PNG file's bytes: RGB when `with_alpha` is false (the alpha bytes are left out), else RGBA; ""
// when zlib fails
inline std::string encode(const Image &image, bool with_alpha) {
    if (image.w <= 0 || image.h <= 0) return "";
    const size_t channels = with_alpha ? 4 : 3, stride = (size_t)image.w * channels;
    std::vector<uint8_t> rows((stride + 1) * image.h), plain(stride), prior(stride, 0), best(stride), trial(stride);
    for (int y = 0; y < image.h; ++y) {
        for (int x = 0; x < image.w; ++x)
            for (size_t c = 0; c < channels; ++c)
                plain[(size_t)x * channels + c] = image.rgba[((size_t)y * image.w + x) * 4 + c];
        unsigned long best_sum = ~0ul;
        uint8_t best_type = 0;
        for (uint8_t type = 0; type < 5; ++type) {
            unsigned long sum = 0;
            for (size_t i = 0; i < stride; ++i) {
                const int a = i >= channels ? plain[i - channels] : 0, b = prior[i], c = i >= channels ? prior[i - channels] : 0;
                const int pred = type == 0 ? 0 : type == 1 ? a : type == 2 ? b : type == 3 ? (a + b) >> 1 : paeth(a, b, c);
                trial[i] = (uint8_t)(plain[i] - pred);
                sum += trial[i] < 128 ? trial[i] : 256 - trial[i];
            }
            if (sum < best_sum) {
                best_sum = sum;
                best_type = type;
                best = trial;
            }
        }
        uint8_t *row = rows.data() + (size_t)y * (stride + 1);
        row[0] = best_type;
        std::memcpy(row + 1, best.data(), stride);
        prior = plain;
    }
    std::vector<uint8_t> packed(compressBound((uLong)rows.size()));
    uLongf size = (uLongf)packed.size();
    if (compress2(packed.data(), &size, rows.data(), (uLong)rows.size(), Z_BEST_COMPRESSION) != Z_OK) return "";
    std::string ihdr;
    for (uint32_t v : {(uint32_t)image.w, (uint32_t)image.h})
        for (int s = 24; s >= 0; s -= 8) ihdr += (char)(v >> s);
    ihdr += (char)8;
    ihdr += (char)(with_alpha ? 6 : 2);
    ihdr.append(3, '\0');
    std::string out("\x89PNG\r\n\x1a\n", 8);
    put_chunk(out, "IHDR", ihdr);
    put_chunk(out, "IDAT", std::string((const char *)packed.data(), size));
    put_chunk(out, "IEND", "");
    return out;
}

}  // namespace png
