// code_ids.cpp - the code id of every u3fix.exe step: which compiled code a step runs, so the Uncharted 3 import
// reruns a recipe step only when its own code changed (u3mp/patcher/importer/recipe.cpp). A build tool, it does not
// ship; tools/package_windows.sh builds and runs it while it builds u3fix.exe:
//
//   code_ids <dispatch.cpp> <objects.txt> <nm.txt> <toolchain> <out.cpp>
//
//   dispatch.cpp  u3fix.cpp: its lines `if (step == "<name>") return u3::<function>(a);` name the steps
//   objects.txt   one line per object file: "<object> <source>" (sources relative to the repository; each object has
//                 the compiler's dependency file next to it, <object>.d, from -MMD -MF)
//   nm.txt        `llvm-nm -A -P -g <objects>`: the global symbols each object defines and needs
//   toolchain     the compiler's version line (a new compiler or C++ library can change any result: every id changes)
//   out.cpp       written: extern "C" const char u3fix_code_ids[], linked into u3fix.exe
//
// A step's objects: the object defining u3::<function>, then every object defining a symbol that one of them needs
// (all definitions of it), repeated; u3fix.cpp's own object (the dispatcher) is left out. The id is the sha256 of the
// toolchain line, the step's name and function, and each of those objects' source path and sha256 of its bytes: a
// change that alters no compiled byte (a comment, another step's code) leaves the id as it was. The table also lists
// the folders of the sources and headers each step's code is built from (tools/fixes/<job> as "<job>").
//
// Table format: "U3FIX-CODE-IDS 1\n", then "<step> <id> <folder>,<folder>...\n" per step, then "END\n".
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

[[noreturn]] void fail(const std::string &m) {
    std::fprintf(stderr, "code_ids: %s\n", m.c_str());
    std::exit(1);
}

std::string read_file(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) fail("cannot read " + p.string());
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

std::string sha256_hex(const std::string &data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    unsigned char d[32];
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) || BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) ||
        BCryptHashData(h, (PUCHAR)data.data(), ULONG(data.size()), 0) || BCryptFinishHash(h, d, 32, 0))
        fail("sha256 failed");
    BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(alg, 0);
    static const char *hx = "0123456789abcdef";
    std::string o;
    for (unsigned char c : d) o += hx[c >> 4], o += hx[c & 15];
    return o;
}

std::vector<std::string> lines_of(const std::string &t) {
    std::vector<std::string> v;
    std::string l;
    for (char c : t) {
        if (c == '\n') {
            if (!l.empty() && l.back() == '\r') l.pop_back();
            v.push_back(l);
            l.clear();
        } else
            l += c;
    }
    if (!l.empty()) v.push_back(l);
    return v;
}

std::string norm(const std::string &p) {   // a repository-relative path with '/' and no "dir/../"
    std::string s = fs::path(p).lexically_normal().generic_string();
    return s;
}

// the folder a step's code comes from: "<job>" under u3mp/patcher/tools/fixes, else the repository path
std::string folder_of(const std::string &file) {
    static const std::string fixes = "u3mp/patcher/tools/fixes/";
    std::string d = fs::path(file).parent_path().generic_string();
    if (d.compare(0, fixes.size(), fixes) == 0) d = d.substr(fixes.size());
    return d;
}

// a make-style dependency file: "<target>: <dep> <dep> \" ... (paths without spaces)
std::vector<std::string> deps_of(const fs::path &d) {
    std::string t = read_file(d);
    std::vector<std::string> out;
    const size_t colon = t.find(": ");
    if (colon == std::string::npos) fail("no dependencies in " + d.string());
    std::string w;
    for (size_t i = colon + 2; i <= t.size(); i++) {
        const char c = i < t.size() ? t[i] : ' ';
        if (c == '\\' && i + 1 < t.size() && (t[i + 1] == '\n' || t[i + 1] == '\r')) continue;
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            if (!w.empty()) out.push_back(norm(w));
            w.clear();
        } else
            w += c;
    }
    return out;
}

}  // namespace

int wmain(int argc, wchar_t **argv) {
    if (argc != 6) fail("usage: code_ids <dispatch.cpp> <objects.txt> <nm.txt> <toolchain> <out.cpp>");
    auto arg = [&](int i) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(size_t(n > 0 ? n - 1 : 0), '\0');
        WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, s.data(), n, nullptr, nullptr);
        return s;
    };
    const std::string dispatch = norm(arg(1)), toolchain = arg(4);

    // the objects, their sources, their bytes' sha256 and the files they are built from
    struct Obj {
        std::string source, sha;
        std::vector<std::string> files;
    };
    std::map<std::string, Obj> objs;   // object path (as in nm.txt) -> ...
    std::string dispatcher;
    for (const std::string &l : lines_of(read_file(arg(2)))) {
        if (l.empty()) continue;
        const size_t sp = l.find(' ');
        if (sp == std::string::npos) fail("bad line in objects.txt: " + l);
        const std::string o = l.substr(0, sp), src = norm(l.substr(sp + 1));
        Obj &x = objs[o];
        x.source = src;
        x.sha = sha256_hex(read_file(o));
        x.files = deps_of(o + ".d");
        if (src == dispatch) dispatcher = o;
    }
    if (dispatcher.empty()) fail("the dispatcher " + dispatch + " is not among the objects");

    // the symbols: who defines what, who needs what
    std::map<std::string, std::vector<std::string>> defined_by;
    std::map<std::string, std::vector<std::string>> needs;
    for (const std::string &l : lines_of(read_file(arg(3)))) {
        // "<object>: <symbol> <type> [<value> <size>]"
        const size_t c = l.find(": ");
        if (c == std::string::npos) continue;
        const std::string o = l.substr(0, c);
        std::istringstream s(l.substr(c + 2));
        std::string sym, type;
        if (!(s >> sym >> type) || !objs.count(o)) continue;
        if (type == "U") needs[o].push_back(sym);
        else if (type != "w" && type != "v") defined_by[sym].push_back(o);   // weak undefined: not a definition
    }

    // the steps
    struct Step {
        std::string name, function;
    };
    std::vector<Step> steps;
    static const std::regex re(R"rx(if \(step == "([A-Za-z0-9_-]+)"\) return u3::([A-Za-z0-9_]+)\(a\);)rx");
    for (const std::string &l : lines_of(read_file(arg(1)))) {
        std::smatch m;
        if (std::regex_search(l, m, re)) steps.push_back({m[1], m[2]});
    }
    if (steps.empty()) fail("no steps found in " + dispatch);

    std::string table = "U3FIX-CODE-IDS 1\n";
    for (const Step &st : steps) {
        // u3::<function>(...): its mangled name starts with _ZN2u3<length><function>E
        const std::string prefix = "_ZN2u3" + std::to_string(st.function.size()) + st.function + "E";
        std::set<std::string> roots;
        for (const auto &[sym, by] : defined_by)
            if (sym.compare(0, prefix.size(), prefix) == 0)
                for (const auto &o : by) roots.insert(o);
        if (roots.size() != 1) fail("step " + st.name + ": u3::" + st.function + " is defined in " + std::to_string(roots.size()) + " objects");
        std::set<std::string> seen = roots;
        std::vector<std::string> todo(roots.begin(), roots.end());
        while (!todo.empty()) {
            const std::string o = todo.back();
            todo.pop_back();
            for (const std::string &sym : needs[o]) {
                auto it = defined_by.find(sym);
                if (it == defined_by.end()) continue;   // the C++ library or Windows
                for (const std::string &d : it->second)
                    if (d != dispatcher && seen.insert(d).second) todo.push_back(d);
            }
        }
        std::map<std::string, std::string> by_source;   // sorted by source path
        std::set<std::string> folders;
        for (const std::string &o : seen) {
            by_source[objs[o].source] = objs[o].sha;
            for (const std::string &f : objs[o].files) folders.insert(folder_of(f));
        }
        std::string h = toolchain + "\n" + st.name + "=u3::" + st.function + "\n";
        for (const auto &[src, sha] : by_source) h += src + " " + sha + "\n";
        std::string fl;
        for (const std::string &f : folders) fl += (fl.empty() ? "" : ",") + f;
        table += st.name + " " + sha256_hex(h) + " " + fl + "\n";
        std::printf("%-18s %2zu objects  %s\n", st.name.c_str(), seen.size(), fl.c_str());
    }
    table += "END\n";

    std::string out = "// generated by tools/code_ids.cpp: the code id of each u3fix step (u3mp/patcher/importer/recipe.cpp reads it)\n"
                      "extern \"C\" const char u3fix_code_ids[] =\n";
    for (const std::string &l : lines_of(table)) out += "    \"" + l + "\\n\"\n";
    out += "    ;\n";
    std::ofstream f(fs::path(argv[5]), std::ios::binary);
    f << out;
    if (!f) fail("cannot write " + arg(5));
    return 0;
}
