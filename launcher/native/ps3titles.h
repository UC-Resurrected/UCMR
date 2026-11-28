// ps3titles.h - the PS3 releases' install folders under RPCS3's dev_hdd0/game, for the Uncharted 2 and Uncharted 3
// imports and the launcher (Windows and Linux).
//
// RPCS3 installs every release's title update and DLC in dev_hdd0/game/<title id> (for example BCES01176/USRDIR with
// dlc-map1..4.psarc). The DLC map pack archives are the same files in every region's install, so an import may take
// them from any release of the game. The folders looked in are the player's PS3 folders and up to five levels above
// each, as they are and with dev_hdd0/game below them. Folder names compare without case and dashes ("bcus-98233" is
// BCUS98233); "<id>DATA2" (game data) is never a title id here. Listings are sorted, so every file system gives the
// same order.
#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace ps3titles {

namespace fs = std::filesystem;

// Uncharted 3: Drake's Deception, every PS3 release (disc and Game of the Year editions of each region), in the order
// a DLC source is looked for after the player's own game folder
inline const std::vector<std::string> &u3_ids() {
    static const std::vector<std::string> ids = {
        "BCUS98233", "BCUS98379", "BCUS90674", "BCUS90675", "BCUS99086", "BCUS99113", "BCUS90918", "BCUS99234",   // North America
        "BCES01175", "BCES01176", "BCES01670", "BCES01692",                                                       // Europe, Australia
        "BCJS37004", "BCJB95501", "BCJS75003",                                                                    // Japan
        "BCAS25009", "BCAS25014",                                                                                 // Asia
        "BCKS15010", "BCKS15011"};                                                                                // South Korea
    return ids;
}

// Uncharted 2: Among Thieves, the releases whose installs carry the map packs (US first)
inline const std::vector<std::string> &u2_ids() {
    static const std::vector<std::string> ids = {"BCUS98123", "BCUS98257", "BCUS98257F", "NPUA80698", "NPUA70051", "BCES00757",
                                                 "BCES00509", "BCES00727", "NPEA00364", "NPEA00365", "BCJS30035", "BCJS70021",
                                                 "BCJS30051", "BCAS20097", "BCAS20142", "NPHA80194", "BCKS10086", "BCKS10084"};
    return ids;
}

// a folder name as a title id: upper case, without '-'
inline std::string key(const std::string &name) {
    std::string k;
    for (char c : name)
        if (c != '-') k += char(std::toupper((unsigned char)c));
    return k;
}

inline std::string utf8(const fs::path &p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

// the folders whose subfolders may be title ids: each root and up to five levels above it, as it is and with
// dev_hdd0/game below it; each once, in this order
inline std::vector<fs::path> game_dirs(const std::vector<fs::path> &roots) {
    std::vector<fs::path> dirs;
    std::error_code ec;
    for (const fs::path &root : roots) {
        if (root.empty()) continue;
        fs::path q = fs::absolute(root, ec).lexically_normal();
        if (ec) continue;
        for (int up = 0; up <= 5; ++up) {
            for (const fs::path &d : {q, q / "dev_hdd0" / "game"}) {
                std::error_code e;
                if (fs::is_directory(d, e) && std::find(dirs.begin(), dirs.end(), d) == dirs.end()) dirs.push_back(d);
            }
            fs::path p = q.parent_path();
            if (p == q || p.empty()) break;
            q = p;
        }
    }
    return dirs;
}

// every <dir>/<title id> folder of the given ids found from the roots: ids in the given order, each id's folders in
// game_dirs order; (title id, folder)
inline std::vector<std::pair<std::string, fs::path>> find(const std::vector<fs::path> &roots, const std::vector<std::string> &ids) {
    std::vector<std::pair<std::string, fs::path>> titles;   // (key, folder) of every subfolder, folder order
    for (const fs::path &d : game_dirs(roots)) {
        std::vector<std::string> names;
        std::error_code ec;
        for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e;
            if (it->is_directory(e)) names.push_back(utf8(it->path().filename()));
        }
        std::sort(names.begin(), names.end());
        for (const std::string &n : names) titles.push_back({key(n), d / fs::path(std::u8string(n.begin(), n.end()))});
    }
    std::vector<std::pair<std::string, fs::path>> out;
    for (const std::string &id : ids)
        for (const auto &[k, folder] : titles)
            if (k == id && std::find_if(out.begin(), out.end(), [&](const auto &o) { return o.second == folder; }) == out.end())
                out.push_back({id, folder});
    return out;
}

}  // namespace ps3titles
