// dctools/fixlib.cpp: the helpers u3fix.exe's steps share (arguments, paths, hashes, Python-compatible text and JSON
// output), apart from u3fix.cpp's main so that the Uncharted 2 import (u2mp/patcher/importer) links them too.
#include "u3fix.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include "port.h"   // launcher/native/port.h: the process list
#endif

namespace u3 {

namespace {

fs::path raw_path(const std::string &s) {
#ifdef _WIN32
    return fs::path(std::u8string(s.begin(), s.end()));
#else
    std::u8string u(s.begin(), s.end());
    for (char8_t &c : u)
        if (c == u8'\\') c = u8'/';
    return fs::path(u);
#endif
}

std::string norm_key(std::string s) {
    for (char &c : s)
        if (c == '\\') c = '/';
    while (s.size() > 1 && s.back() == '/') s.pop_back();
#ifdef _WIN32
    for (char &c : s) c = char(std::tolower((unsigned char)c));
#endif
    return s;
}

struct Ps4View {
    std::string root;
    std::map<std::string, std::string> files, dirs;
    bool active = false, loaded = false;

    void load(const fs::path &root0, const fs::path &manifest) {
        root = norm_key(utf8(root0));
        files.clear();
        dirs.clear();
        active = false;
        loaded = true;
        std::error_code ec;
        if (manifest.empty() || !fs::is_regular_file(manifest, ec)) return;
        std::ifstream in(manifest, std::ios::binary);
        if (!in) throw Stop("cannot read " + utf8(manifest));
        std::string line;
        std::getline(in, line);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line != "U3PS4V2") throw Stop(utf8(manifest) + ": not a U3 PS4 manifest");
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            const size_t a = line.find('\t');
            const size_t b = a == std::string::npos ? a : line.find('\t', a + 1);
            if (a == std::string::npos || b == std::string::npos) throw Stop(utf8(manifest) + ": bad manifest line");
            const std::string rel = norm_key(line.substr(a + 1, b - a - 1)), real = line.substr(b + 1);
            if (line[0] == 'F') files[rel] = real;
            else if (line[0] == 'D') dirs[rel] = real;
            else throw Stop(utf8(manifest) + ": bad manifest entry");
        }
        active = true;
    }

    void ensure() {
        if (loaded) return;
        const char *m = std::getenv("U3_PS4_MANIFEST"), *r = std::getenv("U3_PS4_ROOT");
        if (!m || !*m || !r || !*r) {
            loaded = true;
            return;
        }
        load(raw_path(r), raw_path(m));
    }

    bool under(const std::string &key) const {
        if (!active) return false;
        if (key == root) return true;
        return key.size() > root.size() && key.compare(0, root.size(), root) == 0 && key[root.size()] == '/';
    }
    std::string rel(const std::string &key) const { return key == root ? std::string() : key.substr(root.size() + 1); }
    const std::string *find(const std::string &r, bool dir) const {
        const auto &m = dir ? dirs : files;
        auto it = m.find(r);
        return it == m.end() ? nullptr : &it->second;
    }
};

Ps4View &view() {
    static Ps4View v;
    static std::once_flag once;
    std::call_once(once, [&] { v.ensure(); });
    return v;
}

}  // namespace

void ps4_view_set(const fs::path &root, const fs::path &manifest) { view().load(root, manifest); }

fs::path upath(const std::string &s) {
    Ps4View &v = view();
    const std::string key = norm_key(s);
    if (v.under(key)) {
        const std::string r = v.rel(key);
        if (const std::string *p = v.find(r, false)) return raw_path(*p);
        if (const std::string *p = v.find(r, true)) return raw_path(*p);
    }
    return raw_path(s);
}

fs::path resolve_read_path(const fs::path &p) { return upath(utf8(p)); }

bool ps4_exists(const std::string &s) {
    Ps4View &v = view();
    const std::string key = norm_key(s);
    if (v.under(key)) {
        const std::string r = v.rel(key);
        if (v.find(r, false) || v.find(r, true)) return true;
    }
    std::error_code ec;
    return fs::exists(raw_path(s), ec);
}

std::vector<fs::directory_entry> ps4_dir(const std::string &s) {
    std::vector<fs::directory_entry> out;
    Ps4View &v = view();
    const std::string key = norm_key(s);
    if (v.under(key)) {
        const std::string r = v.rel(key), prefix = r.empty() ? std::string() : r + "/";
        std::map<std::string, std::string> kids;
        for (const auto *m : {&v.dirs, &v.files})
            for (auto it = m->lower_bound(prefix); it != m->end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it) {
                const std::string name = it->first.substr(prefix.size());
                if (name.empty() || name.find('/') != std::string::npos) continue;
                kids[name] = it->second;
            }
        for (const auto &[name, p] : kids) out.emplace_back(raw_path(p));
        return out;
    }
    std::error_code ec;
    for (fs::directory_iterator it(raw_path(s), ec), end; it != end; it.increment(ec)) out.emplace_back(*it);
    return out;
}

std::vector<fs::path> ps4_files(const std::string &s) {
    std::vector<fs::path> out;
    Ps4View &v = view();
    const std::string key = norm_key(s);
    if (v.under(key)) {
        const std::string r = v.rel(key), prefix = r.empty() ? std::string() : r + "/";
        for (auto it = v.files.lower_bound(prefix); it != v.files.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it)
            out.push_back(raw_path(it->second));
        return out;
    }
    std::error_code ec;
    for (fs::recursive_directory_iterator it(raw_path(s), ec), end; it != end; it.increment(ec))
        if (it->is_regular_file(ec)) out.push_back(it->path());
    return out;
}

Args::Args(const std::vector<std::string> &argv, const std::set<std::string> &flags, const std::set<std::string> &values) {
    for (size_t i = 0; i < argv.size(); i++) {
        const std::string &a = argv[i];
        if (flags.count(a)) { set.insert(a); continue; }
        if (values.count(a) && i + 1 < argv.size()) { val[a] = argv[++i]; continue; }
        throw Stop("unknown or incomplete argument: " + a);
    }
}

std::string Args::need(const std::string &k) const {
    auto it = val.find(k);
    if (it == val.end() || it->second.empty()) throw Stop("missing " + k);
    return it->second;
}

std::string Args::get(const std::string &k, const std::string &def) const {
    auto it = val.find(k);
    return it == val.end() ? def : it->second;
}

// MD5 (RFC 1321), for the plans that name a pak by its md5
std::string md5_hex(const Bytes &data) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af,
        0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
        0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
        0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97,
        0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                              4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    const uint64_t bits = uint64_t(data.size()) * 8;
    const size_t whole = data.size() - data.size() % 64;
    Bytes tail(data.begin() + std::ptrdiff_t(whole), data.end());
    tail.push_back(0x80);
    while (tail.size() % 64 != 56) tail.push_back(0);
    for (int i = 0; i < 8; i++) tail.push_back(uint8_t(bits >> (8 * i)));
    auto block = [&](const uint8_t *p) {
        uint32_t m[16];
        for (int i = 0; i < 16; i++) m[i] = rd32le(p + 4 * i);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; i++) {
            uint32_t f;
            int g;
            if (i < 16) { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
            else { f = c ^ (b | ~d); g = (7 * i) % 16; }
            const uint32_t t = d;
            d = c;
            c = b;
            const uint32_t x = a + f + K[i] + m[g];
            b = b + ((x << R[i]) | (x >> (32 - R[i])));
            a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    };
    for (size_t o = 0; o < whole; o += 64) block(data.data() + o);
    for (size_t o = 0; o < tail.size(); o += 64) block(tail.data() + o);
    std::string s;
    for (uint32_t v : h)
        for (int i = 0; i < 4; i++) s += fmt("%02x", (v >> (8 * i)) & 0xff);
    return s;
}

void copy2(const fs::path &src, const fs::path &dst) {
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
    fs::last_write_time(dst, fs::last_write_time(src));
}

#ifdef _WIN32
bool game_running(bool name_contains) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;   // as the scripts: no process list, not running
    PROCESSENTRY32W e;
    e.dwSize = sizeof e;
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &e); ok && !found; ok = Process32NextW(snap, &e)) {
        std::wstring n = e.szExeFile;
        for (wchar_t &c : n) c = (c >= L'A' && c <= L'Z') ? wchar_t(c - L'A' + L'a') : c;
        found = name_contains ? n.find(L"uncharted.exe") != std::wstring::npos : n == L"uncharted.exe";
    }
    CloseHandle(snap);
    return found;
}

std::string win_path(std::string p) {
    for (char &c : p)
        if (c == '/') c = '\\';
    return p;
}
#else
// Linux: the game is the runtime's "Uncharted" program here (its Windows build "Uncharted.exe" when run by Wine)
bool game_running(bool name_contains) {
    for (const port::ProcessInfo &p : port::list_processes()) {
        std::string n = p.name;
        for (char &c : n) c = (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
        if (n == "uncharted" || (name_contains ? n.find("uncharted.exe") != std::string::npos : n == "uncharted.exe")) return true;
    }
    return false;
}

std::string win_path(std::string p) { return p; }   // a path as os.path gives it here: "/" stays
#endif

std::string pyhex(uint64_t v) { return fmt("0x%llx", (unsigned long long)v); }

json::Value read_json(const fs::path &p) {
    const Bytes b = read_file(p);
    json::Value v;
    if (!json::parse(std::string(b.begin(), b.end()), v)) throw Stop(utf8(p) + ": not valid JSON");
    return v;
}

namespace {
void dump1(std::string &o, const json::Value &v, int depth) {
    const std::string pad(size_t(depth + 1), ' '), end(size_t(depth), ' ');
    switch (v.kind) {
    case json::Value::Null: o += "null"; break;
    case json::Value::Bool: o += v.b ? "true" : "false"; break;
    case json::Value::Int: o += std::to_string(v.i); break;
    case json::Value::Float: o += json::real(v.f); break;
    case json::Value::String: json::quote(o, v.s); break;
    case json::Value::Array:
        if (v.items.empty()) { o += "[]"; break; }
        o += "[\n";
        for (size_t k = 0; k < v.items.size(); ++k) {
            o += pad;
            dump1(o, v.items[k], depth + 1);
            o += k + 1 < v.items.size() ? ",\n" : "\n";
        }
        o += end + "]";
        break;
    case json::Value::Object:
        if (v.fields.empty()) { o += "{}"; break; }
        o += "{\n";
        for (size_t k = 0; k < v.fields.size(); ++k) {
            o += pad;
            json::quote(o, v.fields[k].first);
            o += ": ";
            dump1(o, v.fields[k].second, depth + 1);
            o += k + 1 < v.fields.size() ? ",\n" : "\n";
        }
        o += end + "}";
        break;
    }
}
void need_bytes(const Bytes &d, uint64_t o, size_t n) {
    if (o + n > d.size())
        throw Stop(fmt("struct.error: %zu bytes at offset %llu need a buffer of %llu bytes, the file has %zu", n, (unsigned long long)o,
                       (unsigned long long)(o + n), d.size()));
}
}  // namespace

std::string json_dump_indent1(const json::Value &v) {
    std::string o;
    dump1(o, v, 0);
    return o;
}

uint16_t rd16le(const Bytes &d, uint64_t o) { need_bytes(d, o, 2); return uint16_t(d[o] | d[o + 1] << 8); }
uint32_t rd32le(const Bytes &d, uint64_t o) { need_bytes(d, o, 4); return rd32le(&d[o]); }
uint64_t rd64le(const Bytes &d, uint64_t o) { need_bytes(d, o, 8); return rd64le(&d[o]); }
uint32_t be32(const Bytes &d, uint64_t o) { need_bytes(d, o, 4); return uint32_t(d[o]) << 24 | uint32_t(d[o + 1]) << 16 | uint32_t(d[o + 2]) << 8 | d[o + 3]; }

bool py_space(unsigned char c) { return c == 0x20 || (c >= 0x09 && c <= 0x0d) || (c >= 0x1c && c <= 0x1f) || c == 0x85 || c == 0xa0; }

std::vector<std::string> py_split(const std::string &s) {
    std::vector<std::string> w;
    size_t i = 0;
    while (true) {
        while (i < s.size() && py_space((unsigned char)s[i])) i++;
        if (i >= s.size()) return w;
        size_t j = i;
        while (j < s.size() && !py_space((unsigned char)s[j])) j++;
        w.push_back(s.substr(i, j - i));
        i = j;
    }
}

std::string py_rstrip(const std::string &s) {
    size_t n = s.size();
    while (n && py_space((unsigned char)s[n - 1])) n--;
    return s.substr(0, n);
}

std::vector<std::string> py_splitlines(const std::string &s) {   // LF, CR, CRLF, VT, FF, FS, GS, RS and NEL (0x85) end a line
    std::vector<std::string> out;
    size_t a = 0, i = 0;
    while (i < s.size()) {
        const unsigned char c = (unsigned char)s[i];
        if (c == 0x0a || c == 0x0d || c == 0x0b || c == 0x0c || c == 0x1c || c == 0x1d || c == 0x1e || c == 0x85) {
            out.push_back(s.substr(a, i - a));
            i += (c == 0x0d && i + 1 < s.size() && s[i + 1] == 0x0a) ? 2 : 1;
            a = i;
        } else {
            i++;
        }
    }
    if (a < s.size()) out.push_back(s.substr(a));
    return out;
}
}  // namespace u3
