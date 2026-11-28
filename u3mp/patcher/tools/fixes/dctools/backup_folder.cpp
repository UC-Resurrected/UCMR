// dctools/backup_folder.cpp: drop_backup_folder (u3fix.hpp), only for the level and actor steps, so the code id of every
// other step stays as it is
#include "u3fix.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace u3 {
namespace {

// a symbolic link, or on Windows any reparse point (a junction, which std::filesystem takes for an ordinary folder)
bool is_link(const fs::path &p) {
    std::error_code ec;
    if (fs::is_symlink(fs::symlink_status(p, ec))) return true;
#ifdef _WIN32
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    return false;
#endif
}

}  // namespace

uint64_t drop_backup_folder(const fs::path &dir) {
    // only <dir>/<overlay>/<file> regular files and then the empty folders: a link (or a Windows junction) is never
    // entered or followed, so nothing outside the folder can be deleted
    std::error_code ec;
    if (!fs::is_directory(dir, ec) || is_link(dir)) return 0;
    uint64_t freed = 0;
    for (const fs::directory_entry &ov : fs::directory_iterator(dir, ec)) {
        std::error_code e2;
        if (is_link(ov.path()) || !ov.is_directory(e2)) continue;
        for (const fs::directory_entry &f : fs::directory_iterator(ov.path(), e2)) {
            std::error_code e3;
            if (is_link(f.path()) || !f.is_regular_file(e3)) continue;
            const uint64_t n = f.file_size(e3);
            if (fs::remove(f.path(), e3)) freed += n;
        }
        fs::remove(ov.path(), e2);   // only when empty
    }
    fs::remove(dir, ec);
    return freed;
}

}  // namespace u3
