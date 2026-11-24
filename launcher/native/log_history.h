// log_history.h - logs of earlier runs are kept for LOG_DAYS days, then deleted.
//   keep_log_history(log): before a run starts `log` afresh, the old one moves to <name>.<YYYYMMDD-HHMMSS><ext> (the time
//     it was last written) and that log's earlier copies older than LOG_DAYS go.
//   prune_old_logs(folder, prefix, suffix): deletes the files and folders in `folder` whose names start with `prefix` and
//     end with `suffix` and that were last written more than LOG_DAYS days ago (game runs' attempts, hang reports, ...).
// Used by the launcher (the imports' output logs, the game runs' logs and reports) and both importers (U3
// _recipe/log.txt, U2 import.log). Nothing else of an earlier run is kept.
#pragma once
#include <chrono>
#include <ctime>
#include <cwchar>
#include <filesystem>
#include <string>
#include <system_error>

constexpr int LOG_DAYS = 15;

inline void prune_old_logs(const std::filesystem::path &folder, const std::wstring &prefix, const std::wstring &suffix) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto limit = fs::file_time_type::clock::now() - std::chrono::hours(24 * LOG_DAYS);
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        const std::wstring name = it->path().filename().wstring();
        if (name.size() < prefix.size() + suffix.size() || name.compare(0, prefix.size(), prefix) != 0 ||
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
            continue;
        std::error_code e;
        const auto t = fs::last_write_time(it->path(), e);
        if (!e && t < limit) fs::remove_all(it->path(), e);
    }
}

inline void keep_log_history(const std::filesystem::path &log) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::wstring stem = log.stem().wstring(), ext = log.extension().wstring();
    if (fs::is_regular_file(log, ec)) {
        // the log's own time, as a local date: now minus how long ago it was written
        const auto age = fs::file_time_type::clock::now() - fs::last_write_time(log, ec);
        std::time_t when = std::time(nullptr) - (ec ? 0 : (std::time_t)std::chrono::duration_cast<std::chrono::seconds>(age).count());
        std::tm tm{};
        localtime_s(&tm, &when);
        wchar_t stamp[32];
        std::wcsftime(stamp, 32, L"%Y%m%d-%H%M%S", &tm);
        fs::path to = log.parent_path() / (stem + L"." + stamp + ext);
        for (int n = 2; fs::exists(to, ec); ++n) to = log.parent_path() / (stem + L"." + stamp + L"-" + std::to_wstring(n) + ext);
        fs::rename(log, to, ec);   // a log another program holds open stays where it is (and is overwritten as before)
    }
    prune_old_logs(log.has_parent_path() ? log.parent_path() : fs::path(L"."), stem + L".", ext);   // never the live log
    // (its name has no second dot part)
}
