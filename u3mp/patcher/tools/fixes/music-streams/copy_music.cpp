// Recipe step music-streams: overlay-r3 sound1/streams/music = every PS3 music stream the PS4 game does not ship.
//   u3fix music-streams --project P --ps4 D --ps3-disc D [--apply]
// The PS3 disc's build/main/sound1/streams/music.psarc has 605 streams, the PS4 Collection 532: the 81 missing ones are
// the multiplayer and co-op music (match win / lose and game-over stingers, level-up stingers, the multiplayer edits and
// remixes, Power Play, co-op endings). The game opens them by name (sound1/streams/music/<name>.xvag) and found none,
// so that music was silent. They are byte copies of the PS3 XVAG files (big-endian header, MPEG audio), as the speech
// and effect streams the other copy steps bring over; PS4 has no version of them (PS4 assets first: nothing to prefer).
// Dry run by default; files already present with the same bytes are left alone.
#include "psarc.hpp"
#include "u3fix.hpp"

#include <cstdio>

namespace u3 {

int music_streams(const std::vector<std::string> &argv) {
    const Args a(argv, {"--apply"}, {"--project", "--ps4", "--ps3-disc"});
    const fs::path ps4 = upath(a.need("--ps4")) / "u3data" / "build" / "main" / "sound1" / "streams" / "music";
    const fs::path arc = upath(a.need("--ps3-disc")) / "build" / "main" / "sound1" / "streams" / "music.psarc";
    const fs::path out = upath(a.need("--project")) / "work" / "overlay-r3" / "u3data" / "build" / "main" / "sound1" / "streams" / "music";
    if (!fs::is_directory(ps4)) throw Stop("PS4 music folder not found: " + utf8(ps4));
    if (!fs::is_regular_file(arc)) throw Stop("PS3 music.psarc not found: " + utf8(arc));
    std::set<std::string> have;
    for (const auto &e : fs::directory_iterator(ps4)) have.insert(utf8(e.path().filename()));
    Psarc p(arc);
    int copied = 0, present = 0, retail = 0;
    uint64_t bytes = 0;
    for (size_t i = 0; i < p.names.size(); i++) {
        const std::string &n = p.names[i];
        const size_t s = n.rfind('/');
        const std::string name = s == std::string::npos ? n : n.substr(s + 1);
        if (name.size() < 6 || name.compare(name.size() - 5, 5, ".xvag")) continue;
        if (have.count(name)) { retail++; continue; }
        const fs::path dst = out / upath(name);
        const Bytes d = p.read(i + 1);
        if (fs::is_regular_file(dst) && fs::file_size(dst) == d.size() && read_file(dst) == d) { present++; continue; }
        copied++;
        bytes += d.size();
        if (a.has("--apply")) {
            fs::create_directories(out);
            write_file_replace(dst, d);
        }
    }
    std::printf("music streams: %d on PS4, %d PS3-only already present, %d PS3-only %s (%.1f MB)\n", retail, present, copied,
                a.has("--apply") ? "copied" : "to copy (dry run)", double(bytes) / 1e6);
    return 0;
}

}  // namespace u3
