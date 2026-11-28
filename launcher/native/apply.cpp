// apply.cpp builds apply_update.exe, the program that swaps in a downloaded launcher version (it replaced
// tools/apply_update.py). It is started from the unpacked copy of the NEW version, as
// update\staged\tools\apply_update.exe, once the running launcher is on its way out, and goes through these stages:
//   1. Wait until the launcher process given by --wait-pid has ended.
//   2. For every top-level program item in the package (anything but analysis\), move the installed one aside into
//      update\old and copy the new one into the root. Inside analysis\uncharted-user only the runtime's defaults file
//      is overwritten; the shader cache, logs, settings and saves are left alone. A folder that Windows will not move
//      (a File Explorer window showing it or a folder inside it, GitHub #67) is updated file by file instead: each
//      file the package brings is set aside into update\old and replaced, and the player's files never move.
//   3. If anything fails, move every item that was set aside back again so the previous version still runs.
//   4. Bring across, into the fresh folders, whatever the old folders held that the package does not ship. That covers
//      the player's own additions: ReShade (ReShade.ini, presets, reshade-shaders\ beside Uncharted.exe), a game folder
//      placed under gow3_pc\, personal notes and so on. The only things left out are files that the previous package
//      itself shipped (recorded in installed-files.txt) and that the new package has dropped (#134); this holds in
//      folders that disappeared from the package too. Program folders that the new package no longer contains at all
//      (an old python\, say) lose those files and are deleted once empty.
//   5. Launch "UCMR_Launcher.exe"; that new launcher deletes update\ on startup.
// usage: apply_update.exe --root <package folder> --wait-pid <launcher pid> [--no-restart] [--quiet]
// The exe is statically linked, so it does not depend on anything in the folder it is about to replace.
//
// Linux (tools/apply_update in the Linux package, docs/WINDOWS-DEPENDENCIES.md §7.6): the same stages from the same
// code. The few file-system and process calls sit in the small functions below, each with its Windows body as before
// and a Linux one beside it; Linux has no progress window (the status lines go to the output, which the launcher
// writes to analysis/update-apply.log) and starts ./UCMR_Launcher at the end.

#ifdef _WIN32
#include <windows.h>
#else
#include "port.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <thread>

#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <set>
#include <string>
#include <vector>

namespace {

#ifdef _WIN32
const wchar_t *const TITLE = L"UCMR";
#endif
const wchar_t *const DEFAULTS[] = {L"analysis/uncharted-user/disabled-optimizations.txt"};
// Written at the end of every update: one line per file the package shipped, relative to the root with "/" between
// folders. When the following update runs, files listed here that its package lacks are left behind, and all other
// files are kept.
const wchar_t *const MANIFEST = L"installed-files.txt";

struct Error { std::wstring message; };

// ------------------------------------------------------------------------------------------------ the platform functions

#ifdef _WIN32
std::string utf8(const std::wstring &w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
std::wstring widen(const std::string &s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
void write_std(DWORD which, const std::wstring &text) {
    std::string o;
    for (char c : utf8(text + L"\n")) { if (c == '\n') o += "\r\n"; else o += c; }
    HANDLE h = GetStdHandle(which);
    DWORD put = 0;
    if (h && h != INVALID_HANDLE_VALUE) WriteFile(h, o.data(), (DWORD)o.size(), &put, nullptr);
}
void print(const std::wstring &text) { write_std(STD_OUTPUT_HANDLE, text); }
void print_error(const std::wstring &text) { write_std(STD_ERROR_HANDLE, text); }

using ErrorCode = DWORD;
ErrorCode last_error() { return GetLastError(); }

// Formats a system error the way Python prints an OSError: "[WinError n] text: 'a' -> 'b'"
std::wstring os_error(DWORD code, const std::wstring &a = L"", const std::wstring &b = L"") {
    wchar_t *text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code,
                   0, (LPWSTR)&text, 0, nullptr);
    std::wstring t = text ? text : L"error";
    if (text) LocalFree(text);
    while (!t.empty() && (t.back() == L'\n' || t.back() == L'\r' || t.back() == L' ' || t.back() == L'.')) t.pop_back();
    std::wstring o = L"[WinError " + std::to_wstring(code) + L"] " + t;
    if (!a.empty()) o += L": '" + a + L"'";
    if (!b.empty()) o += L" -> '" + b + L"'";
    return o;
}

std::wstring join(const std::wstring &a, const std::wstring &b) {
    if (a.empty()) return b;
    return (a.back() == L'\\' || a.back() == L'/') ? a + b : a + L"\\" + b;
}
std::wstring native_rel(std::wstring rel) {
    std::replace(rel.begin(), rel.end(), L'/', L'\\');
    return rel;
}
DWORD attrs(const std::wstring &p) { return GetFileAttributesW(p.c_str()); }
bool exists(const std::wstring &p) {   // like Path.exists: a link counts only if its target is there
    const DWORD a = attrs(p);
    if (a == INVALID_FILE_ATTRIBUTES) return false;
    if (!(a & FILE_ATTRIBUTE_REPARSE_POINT)) return true;
    HANDLE h = CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}
bool is_link(const std::wstring &p) {
    const DWORD a = attrs(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}
bool is_dir(const std::wstring &p) { return exists(p) && (attrs(p) & FILE_ATTRIBUTE_DIRECTORY); }
bool is_file(const std::wstring &p) { return exists(p) && !(attrs(p) & FILE_ATTRIBUTE_DIRECTORY); }

// Names inside a folder in the order sorted(Path.iterdir()) would produce
std::vector<std::wstring> entries(const std::wstring &folder) {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(join(folder, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) throw Error{os_error(GetLastError(), folder)};
    do {
        const std::wstring n = fd.cFileName;
        if (n != L"." && n != L"..") names.push_back(n);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    // case-insensitive, because pathlib lower-cases Windows paths before it compares them
    std::stable_sort(names.begin(), names.end(), [](const std::wstring &a, const std::wstring &b) {
        std::wstring x = a, y = b;
        CharLowerBuffW(&x[0], (DWORD)x.size());
        CharLowerBuffW(&y[0], (DWORD)y.size());
        return x < y;
    });
    return names;
}

std::wstring full_path(const std::wstring &p) {
    std::vector<wchar_t> buf(32768);
    DWORD n = GetFullPathNameW(p.c_str(), 32768, buf.data(), nullptr);
    std::wstring f = n && n < 32768 ? std::wstring(buf.data(), n) : p;
    while (f.size() > 3 && (f.back() == L'\\' || f.back() == L'/')) f.pop_back();
    return f;
}

unsigned long long now_ms() { return GetTickCount64(); }
void sleep_ms(unsigned ms) { Sleep(ms); }

void rename(const std::wstring &from, const std::wstring &to) {
    if (!MoveFileExW(from.c_str(), to.c_str(), 0)) throw Error{os_error(GetLastError(), from, to)};
}
void make_dirs(const std::wstring &p) {
    if (is_dir(p)) return;
    const size_t cut = p.find_last_of(L'\\');
    if (cut != std::wstring::npos && cut > 2) make_dirs(p.substr(0, cut));
    if (!CreateDirectoryW(p.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        throw Error{os_error(GetLastError(), p)};
}
// one new folder: false (the reason in last_error()) when it could not be made, also when it is there already
bool create_dir(const std::wstring &p) { return CreateDirectoryW(p.c_str(), nullptr) != FALSE; }
bool already_exists(ErrorCode code) { return code == ERROR_ALREADY_EXISTS; }
// deletes an empty folder; false when it could not go
bool remove_dir(const std::wstring &p) { return RemoveDirectoryW(p.c_str()) != FALSE; }
// copies one file over whatever is at `to`, with its times; false (the reason in last_error()) on failure
bool copy_file(const std::wstring &from, const std::wstring &to) { return CopyFileW(from.c_str(), to.c_str(), FALSE) != FALSE; }

// Deletes a file or a whole tree (shutil.rmtree / unlink); a link is removed without touching what it points to
void remove_item(const std::wstring &path) {
    const DWORD a = attrs(path);
    if (a == INVALID_FILE_ATTRIBUTES) return;
    if ((a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT))
        for (const std::wstring &n : entries(path)) remove_item(join(path, n));
    if (a & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(path.c_str(), a & ~FILE_ATTRIBUTE_READONLY);
    const BOOL ok = (a & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(path.c_str()) : DeleteFileW(path.c_str());
    if (!ok) throw Error{os_error(GetLastError(), path)};
}

// the whole file as bytes; false when it cannot be opened
bool read_text(const std::wstring &path, std::string &data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[1 << 16];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got) data.append(buf, got);
    CloseHandle(h);
    return true;
}
const char *const NEWLINE = "\r\n";
// writes the bytes as the whole file; false (the reason in last_error()) on failure
bool write_text(const std::wstring &path, const std::string &data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD put = 0;
    const bool ok = h != INVALID_HANDLE_VALUE && WriteFile(h, data.data(), (DWORD)data.size(), &put, nullptr);
    const DWORD code = GetLastError();
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    SetLastError(code);
    return ok;
}

// the folder part and the last name of a path
std::wstring parent(const std::wstring &p) { return p.substr(0, p.find_last_of(L'\\')); }
std::wstring leaf(const std::wstring &p) { return p.substr(p.find_last_of(L'\\') + 1); }

bool same_name_ci(const std::wstring &a, const wchar_t *b) {
    return CompareStringOrdinal(a.c_str(), (int)a.size(), b, -1, TRUE) == CSTR_EQUAL;
}

std::wstring timestamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t b[32];
    swprintf(b, 32, L"%04d%02d%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}
#else   // Linux
namespace fs = std::filesystem;

std::string utf8(const std::wstring &w) { return port::utf8(w); }
std::wstring widen(const std::string &s) { return port::wide(s); }
void write_fd(int fd, const std::wstring &text) {
    const std::string o = utf8(text + L"\n");
    if (::write(fd, o.data(), o.size()) < 0) {}
}
void print(const std::wstring &text) { write_fd(1, text); }
void print_error(const std::wstring &text) { write_fd(2, text); }

using ErrorCode = int;
ErrorCode last_error() { return errno; }

// a system error the way Python prints an OSError on Linux: "[Errno n] text: 'a' -> 'b'"
std::wstring os_error(int code, const std::wstring &a = L"", const std::wstring &b = L"") {
    std::wstring o = L"[Errno " + std::to_wstring(code) + L"] " + widen(std::strerror(code));
    if (!a.empty()) o += L": '" + a + L"'";
    if (!b.empty()) o += L" -> '" + b + L"'";
    return o;
}

std::wstring join(const std::wstring &a, const std::wstring &b) {
    if (a.empty()) return b;
    return a.back() == L'/' ? a + b : a + L"/" + b;
}
std::wstring native_rel(std::wstring rel) { return rel; }
fs::path P(const std::wstring &p) { return fs::path(utf8(p)); }
bool exists(const std::wstring &p) {   // like Path.exists: a link counts only if its target is there
    std::error_code ec;
    return fs::exists(P(p), ec);
}
bool is_link(const std::wstring &p) {
    std::error_code ec;
    return fs::is_symlink(fs::symlink_status(P(p), ec));
}
bool is_dir(const std::wstring &p) {
    std::error_code ec;
    return fs::is_directory(P(p), ec);
}
bool is_file(const std::wstring &p) { return exists(p) && !is_dir(p); }

// Names inside a folder in the order sorted(Path.iterdir()) gives on Linux (plain code point order)
std::vector<std::wstring> entries(const std::wstring &folder) {
    std::vector<std::wstring> names;
    std::error_code ec;
    fs::directory_iterator it(P(folder), ec);
    if (ec) throw Error{os_error(ec.value(), folder)};
    for (fs::directory_iterator end; it != end; it.increment(ec)) {
        if (ec) throw Error{os_error(ec.value(), folder)};
        names.push_back(widen(it->path().filename().string()));
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::wstring full_path(const std::wstring &p) {
    std::error_code ec;
    fs::path a = fs::absolute(P(p), ec);
    if (ec) a = P(p);
    std::wstring f = widen(a.lexically_normal().string());
    while (f.size() > 1 && f.back() == L'/') f.pop_back();
    return f;
}

unsigned long long now_ms() {
    return (unsigned long long)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
void sleep_ms(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// as MoveFileExW without flags: never over something that is there already
void rename(const std::wstring &from, const std::wstring &to) {
    std::error_code ec;
    if (fs::symlink_status(P(to), ec).type() != fs::file_type::not_found) throw Error{os_error(EEXIST, from, to)};
    if (::rename(utf8(from).c_str(), utf8(to).c_str()) != 0) throw Error{os_error(errno, from, to)};
}
void make_dirs(const std::wstring &p) {
    if (is_dir(p)) return;
    std::error_code ec;
    fs::create_directories(P(p), ec);
    if (ec) throw Error{os_error(ec.value(), p)};
}
bool create_dir(const std::wstring &p) { return ::mkdir(utf8(p).c_str(), 0755) == 0; }
bool already_exists(ErrorCode code) { return code == EEXIST; }
bool remove_dir(const std::wstring &p) { return ::rmdir(utf8(p).c_str()) == 0; }
bool copy_file(const std::wstring &from, const std::wstring &to) {
    std::error_code ec;
    fs::copy_file(P(from), P(to), fs::copy_options::overwrite_existing, ec);   // with its mode bits
    if (!ec) {
        const auto time = fs::last_write_time(P(from), ec);
        if (!ec) fs::last_write_time(P(to), time, ec);
    }
    if (ec) errno = ec.value();
    return !ec;
}

void remove_item(const std::wstring &path) {
    std::error_code ec;
    const auto st = fs::symlink_status(P(path), ec);
    if (st.type() == fs::file_type::not_found) return;
    if (st.type() == fs::file_type::directory)
        for (const std::wstring &n : entries(path)) remove_item(join(path, n));
    if (!fs::remove(P(path), ec) || ec) throw Error{os_error(ec ? ec.value() : ENOENT, path)};
}

bool read_text(const std::wstring &path, std::string &data) {
    std::ifstream f(P(path), std::ios::binary);
    if (!f) return false;
    char buf[1 << 16];
    while (f.read(buf, sizeof buf) || f.gcount()) data.append(buf, (size_t)f.gcount());
    return true;
}
const char *const NEWLINE = "\n";
bool write_text(const std::wstring &path, const std::string &data) {
    std::ofstream f(P(path), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data.data(), (std::streamsize)data.size());
    f.close();
    return !f.fail();
}

std::wstring parent(const std::wstring &p) { return p.substr(0, p.find_last_of(L'/')); }
std::wstring leaf(const std::wstring &p) { return p.substr(p.find_last_of(L'/') + 1); }

bool same_name_ci(const std::wstring &a, const wchar_t *b) {
    std::wstring x = a, y = b;
    for (wchar_t &c : x) c = (wchar_t)std::towlower((wint_t)c);
    for (wchar_t &c : y) c = (wchar_t)std::towlower((wint_t)c);
    return x == y;
}

std::wstring timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm t{};
    localtime_r(&now, &t);
    wchar_t b[32];
    swprintf(b, 32, L"%04d%02d%02d-%02d%02d%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    return b;
}
#endif

// ------------------------------------------------------------------------------------------------ file system helpers

// Repeats `action` every half second until it works or `seconds` run out. Right after a process ends its files may
// still be locked briefly, and a virus scanner can hold them as well.
template <typename F> void retry(F action, int seconds = 15) {
    const unsigned long long deadline = now_ms() + (unsigned long long)seconds * 1000;
    while (true) {
        try {
            action();
            return;
        } catch (const Error &) {
            if (now_ms() > deadline) throw;
            sleep_ms(500);
        }
    }
}

// Recursive copy that keeps file times, as shutil.copytree / copy2 do
void copy_item(const std::wstring &source, const std::wstring &target) {
    if (is_dir(source)) {
        if (!create_dir(target)) throw Error{os_error(last_error(), target)};
        for (const std::wstring &n : entries(source)) copy_item(join(source, n), join(target, n));
    } else if (!copy_file(source, target)) {
        throw Error{os_error(last_error(), source, target)};
    }
}

// ------------------------------------------------------------------------------------------------ installing

std::wstring g_root, g_staged;

#ifdef _WIN32
HWND g_window = nullptr, g_label = nullptr;
HFONT g_font = nullptr;
HBRUSH g_brush = nullptr;

void pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void status(const std::wstring &text) {
    print(text);
    if (!g_label) return;
    SetWindowTextW(g_label, text.c_str());
    // resize the window around the text, padded 28 x 22 like the old Tk label
    HDC dc = GetDC(g_label);
    HGDIOBJ previous = SelectObject(dc, g_font);
    RECT r{0, 0, 0, 0};
    DrawTextW(dc, text.c_str(), -1, &r, DT_CALCRECT | DT_NOPREFIX);
    SelectObject(dc, previous);
    ReleaseDC(g_label, dc);
    const UINT dpi = GetDpiForWindow(g_window);
    const int padx = MulDiv(28, (int)dpi, 96), pady = MulDiv(22, (int)dpi, 96);
    RECT frame{0, 0, r.right + 2 * padx, r.bottom + 2 * pady};
    AdjustWindowRectExForDpi(&frame, WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0, dpi);
    SetWindowPos(g_window, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top, SWP_NOMOVE | SWP_NOZORDER);
    MoveWindow(g_label, padx, pady, r.right, r.bottom, TRUE);
    InvalidateRect(g_window, nullptr, TRUE);
    UpdateWindow(g_window);
    pump();
}

LRESULT CALLBACK window_proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_CTLCOLORSTATIC) {
        SetTextColor((HDC)wp, RGB(0xef, 0xe6, 0xd8));
        SetBkColor((HDC)wp, RGB(0x14, 0x11, 0x0d));
        return (LRESULT)g_brush;
    }
    if (m == WM_CLOSE) return 0;   // closing is ignored: the install has to finish
    return DefWindowProcW(w, m, wp, lp);
}

void open_window() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_brush = CreateSolidBrush(RGB(0x14, 0x11, 0x0d));
    WNDCLASSW wc{};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hbrBackground = g_brush;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"UnchartedUpdate";
    RegisterClassW(&wc);
    g_window = CreateWindowExW(0, wc.lpszClassName, TITLE, WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT,
                               CW_USEDEFAULT, 400, 120, nullptr, nullptr, wc.hInstance, nullptr);
    const UINT dpi = GetDpiForWindow(g_window);
    g_font = CreateFontW(-MulDiv(11, (int)dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_label = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 10, 10, g_window, nullptr,
                              wc.hInstance, nullptr);
    SendMessageW(g_label, WM_SETFONT, (WPARAM)g_font, TRUE);
    ShowWindow(g_window, SW_SHOW);
    status(L"The launcher is being updated...");
}

bool wait_for_exit(DWORD pid, int seconds) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return true;   // the process has ended already
    // keep pumping messages so the status window does not freeze during the wait
    const ULONGLONG deadline = GetTickCount64() + (ULONGLONG)seconds * 1000;
    bool gone = false;
    while (!gone) {
        const ULONGLONG now = GetTickCount64();
        if (now > deadline) break;
        const DWORD r = MsgWaitForMultipleObjects(1, &h, FALSE, (DWORD)std::min<ULONGLONG>(deadline - now, 100), QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) gone = true;
        else pump();
    }
    CloseHandle(h);
    return gone;
}
#else   // Linux: no window; the status lines go to the output
void status(const std::wstring &text) { print(text); }

// the launcher has ended (gone, or a zombie its parent has not collected yet)
bool ended(unsigned long pid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/stat");
    std::string stat;
    if (!std::getline(f, stat)) return true;
    const size_t close = stat.rfind(')');
    return close != std::string::npos && close + 2 < stat.size() && (stat[close + 2] == 'Z' || stat[close + 2] == 'X');
}

bool wait_for_exit(unsigned long pid, int seconds) {
    const unsigned long long deadline = now_ms() + (unsigned long long)seconds * 1000;
    while (!ended(pid)) {
        if (now_ms() > deadline) return false;
        sleep_ms(100);
    }
    return true;
}
#endif

// Collects every program file the new package will put in the root, as root-relative paths with "/" separators.
void package_files(const std::wstring &folder, const std::wstring &rel, std::vector<std::wstring> &out) {
    for (const std::wstring &n : entries(folder)) {
        const std::wstring path = join(folder, n), r = rel.empty() ? n : rel + L"/" + n;
        if (rel.empty() && (n == L"analysis" || n == L"update")) continue;
        if (is_dir(path)) package_files(path, r, out);
        else if (is_file(path)) out.push_back(r);
    }
}

std::set<std::wstring> read_manifest() {
    std::set<std::wstring> lines;
    std::string data;
    if (!read_text(join(g_root, MANIFEST), data)) return lines;   // versions up to 0.5.0 never wrote one: keep every file the package lacks
    const std::wstring text = widen(data);
    size_t a = 0;
    while (a <= text.size()) {
        size_t b = text.find_first_of(L"\r\n", a);
        if (b == std::wstring::npos) b = text.size();
        std::wstring line = text.substr(a, b - a);
        while (!line.empty() && iswspace(line.back())) line.pop_back();
        while (!line.empty() && iswspace(line.front())) line.erase(line.begin());
        if (!line.empty()) lines.insert(line);
        a = b + 1;
    }
    return lines;
}

// Folders only the package fills (programs and the helper build): what the new package does not ship there is dropped,
// even when installed-files.txt is missing or incomplete (installs up to 0.5.0, a failed write, an unzip over the
// install). The player's settings and imports never live in them; an older helper build (u3mp/patcher/build/v*) left
// there would otherwise be picked, since the launcher and importer use the highest one on disk. A renamed program goes
// the same way: launcher-qt\UnchartedLauncher.exe (the window's name up to 0.10.158, now UCMRLauncher.exe) is dropped.
const wchar_t *const OWNED[] = {L"u3mp/bin", L"u2mp/bin", L"u3mp/patcher/build", L"launcher/native", L"launcher-qt"};

// Adds to `dropped` every installed file below the package-owned folder `rel` that the new package lacks.
void add_owned(const std::wstring &rel, const std::vector<std::wstring> &new_files, std::set<std::wstring> &dropped) {
    const std::wstring path = join(g_root, native_rel(rel));
    if (!is_dir(path) || is_link(path)) return;
    for (const std::wstring &n : entries(path)) {
        const std::wstring r = rel + L"/" + n, p = join(path, n);
        if (is_link(p)) continue;
        if (is_dir(p)) add_owned(r, new_files, dropped);
        else if (!std::binary_search(new_files.begin(), new_files.end(), r)) dropped.insert(r);
    }
}

// Deletes from `dir` (holding root-relative paths below `rel`) the files that are package files (`package`: the
// previous and the new package), and folders left empty, so a kept folder holds only the player's own files.
void strip_package(const std::wstring &dir, const std::wstring &rel, const std::set<std::wstring> &package) {
    for (const std::wstring &n : entries(dir)) {
        const std::wstring p = join(dir, n), r = rel.empty() ? n : rel + L"/" + n;
        if (is_link(p)) continue;
        try {
            if (is_dir(p)) {
                strip_package(p, r, package);
                if (entries(p).empty()) remove_dir(p);
            } else if (package.count(r)) {
                retry([&] { remove_item(p); }, 3);
            }
        } catch (const Error &) {
            // a file that refuses to go stays: it is reported nowhere, the folder is only kept for the player
        }
    }
}

// True if some file below `rel` came with the previous package and is absent from the new one.
bool holds_dropped(const std::set<std::wstring> &dropped, const std::wstring &rel) {
    const std::wstring prefix = rel + L"/";
    auto it = dropped.lower_bound(prefix);
    return it != dropped.end() && it->compare(0, prefix.size(), prefix) == 0;
}

// Detects a __pycache__ folder. Python creates these beside its scripts on its own; they are neither part of any
// package nor something the player made, so they are not kept and end up in update\old instead.
bool python_cache(const std::wstring &name) { return same_name_ci(name, L"__pycache__"); }

// Brings into `fresh` everything that `old`, the set-aside copy of the same program folder, has and `fresh` lacks.
// A subfolder that `fresh` lacks moves over in one rename (same drive, so it is instant even for a whole game folder)
// unless it contains files the new package dropped. Folders found on both sides, and folders with dropped files, are
// walked entry by entry instead, and the previous package's files that the new package no longer ships are skipped.
void carry_over(const std::wstring &old, const std::wstring &fresh, const std::wstring &relative,
                const std::set<std::wstring> &dropped, std::vector<std::wstring> &kept) {
    for (const std::wstring &n : entries(old)) {
        const std::wstring item = join(old, n), target = join(fresh, n);
        const std::wstring rel = relative.empty() ? n : relative + L"/" + n;
        const bool real_dir = is_dir(item) && !is_link(item);
        if (real_dir && python_cache(n)) {
            continue;   // __pycache__ is not kept
        } else if (real_dir && is_dir(target)) {
            carry_over(item, target, rel, dropped, kept);
        } else if (exists(target) || is_link(target)) {
            continue;   // the new package supplies this one
        } else if (is_file(item) && dropped.count(rel)) {
            continue;   // shipped by the previous package, no longer by this one
        } else if (real_dir && holds_dropped(dropped, rel)) {
            // folder missing from the new package: recreate it for the player's files, without the dropped ones
            make_dirs(target);
            carry_over(item, target, rel, dropped, kept);
            if (entries(target).empty()) remove_dir(target);
        } else {
            rename(item, target);
            kept.push_back(rel);
        }
    }
}

// Cleans up a program item that the new package does not contain any more (for example a leftover python\ folder).
// Files the previous package shipped there are moved into update\old, files the player added stay, and any folder that
// ends up empty is deleted. Returns true when the item no longer exists afterwards.
bool prune(const std::wstring &path, const std::wstring &rel, const std::wstring &old, const std::set<std::wstring> &dropped,
           std::vector<std::wstring> &problems) {
    if (is_link(path)) return false;
    const std::wstring name = rel.substr(rel.find_last_of(L'/') + 1);
    if (is_dir(path) && python_cache(name)) {   // a __pycache__ folder is moved away in one piece
        try {
            const std::wstring target = join(old, native_rel(rel));
            make_dirs(parent(target));
            retry([&] { rename(path, target); }, 5);
            return true;
        } catch (const Error &e) {
            problems.push_back(rel + L" (" + e.message + L")");
            return false;
        }
    }
    if (is_dir(path)) {
        bool empty = true;
        for (const std::wstring &n : entries(path)) {
            if (!prune(join(path, n), rel + L"/" + n, old, dropped, problems)) empty = false;
        }
        if (empty && !remove_dir(path)) {
            problems.push_back(rel + L" (" + os_error(last_error(), path) + L")");
            return false;
        }
        return empty;
    }
    if (!dropped.count(rel)) return false;   // not from the previous package, so it belongs to the player
    try {
        const std::wstring target = join(old, native_rel(rel));
        make_dirs(parent(target));
        retry([&] { rename(path, target); }, 5);
        return true;
    } catch (const Error &e) {
        problems.push_back(rel + L" (" + e.message + L")");
        return false;
    }
}

// One step of a folder updated file by file: `rel` (root-relative, "\" between folders) was put in place by the update;
// `replaced` means the installed one it took the place of waits at the same path under update\old.
struct Swap {
    std::wstring rel;
    bool replaced;
};

// Puts the package folder `source` into the installed program folder `target` without moving `target` itself (used
// when Windows refuses to move the whole folder, GitHub #67). Folders on both sides are walked; every other item of the
// package is copied in, and an installed item in its way is first moved aside into update\old. Each step is recorded in
// `journal` before it can leave anything behind, so undo() can take it back.
void merge_into(const std::wstring &source, const std::wstring &target, const std::wstring &rel, const std::wstring &old,
                std::vector<Swap> &journal) {
    for (const std::wstring &n : entries(source)) {
        const std::wstring from = join(source, n), to = join(target, n), r = join(rel, n);
        if (is_dir(from) && is_dir(to) && !is_link(to)) {
            merge_into(from, to, r, old, journal);
            continue;
        }
        if (exists(to) || is_link(to)) {
            const std::wstring aside = join(old, r);
            make_dirs(parent(aside));
            retry([&] { rename(to, aside); }, 5);
            journal.push_back({r, true});
        } else {
            journal.push_back({r, false});
        }
        copy_item(from, to);
    }
}

// Takes back the steps of merge_into, newest first; what cannot be taken back is added to `stuck`.
void undo(const std::vector<Swap> &journal, const std::wstring &old, std::vector<std::wstring> &stuck) {
    for (auto it = journal.rbegin(); it != journal.rend(); ++it) {
        const std::wstring here = join(g_root, it->rel);
        try {
            retry([&] { remove_item(here); });
            if (it->replaced) retry([&] { rename(join(old, it->rel), here); });
        } catch (const Error &e) {
            stuck.push_back(it->rel + L" (could not restore: " + e.message + L")");
        }
    }
}

// The launcher wipes update\ when it starts, so this moves update\old up into the root under a dated name. Returns
// the folder's final location (unchanged if the move failed).
std::wstring preserve(const std::wstring &old) {
    const std::wstring target = join(g_root, L"update-kept-" + timestamp() + L"-old");
    try {
        retry([&] { rename(old, target); });
        return target;
    } catch (const Error &) {
        return old;
    }
}

void apply() {
    const std::wstring old = join(join(g_root, L"update"), L"old");
    retry([&] { if (exists(old)) remove_item(old); });
    make_dirs(old);
    std::vector<std::wstring> new_files;
    package_files(g_staged, L"", new_files);
    std::sort(new_files.begin(), new_files.end());
    std::set<std::wstring> dropped = read_manifest();
    for (const std::wstring &f : new_files) dropped.erase(f);
    for (const wchar_t *owned : OWNED) add_owned(owned, new_files, dropped);
    std::set<std::wstring> package = dropped;   // every package file, previous or new (not the player's)
    package.insert(new_files.begin(), new_files.end());
    std::vector<std::wstring> items;
    for (const std::wstring &n : entries(g_staged))
        if (n != L"analysis" && n != L"update") items.push_back(n);
    std::sort(items.begin(), items.end());   // plain string order, so upper and lower case differ
    std::vector<std::wstring> moved, added, merged;
    std::vector<Swap> journal;
    try {
        for (const std::wstring &name : items) {
            status(L"Copying " + name + L"...");
            const std::wstring current = join(g_root, name), source = join(g_staged, name);
            if (exists(current)) {
                try {
                    retry([&] { rename(current, join(old, name)); }, 3);
                } catch (const Error &) {
                    // A File Explorer window inside this folder (or a program with it as its current folder) keeps
                    // Windows from moving it, but not from replacing the files in it.
                    if (!is_dir(current) || is_link(current) || !is_dir(source)) throw;
                    status(L"Copying " + name + L" file by file (the folder is open in another program)...");
                    merged.push_back(name);
                    merge_into(source, current, name, old, journal);
                    continue;
                }
                moved.push_back(name);
            } else {
                added.push_back(name);
            }
            copy_item(source, current);
        }
        for (const wchar_t *relative : DEFAULTS) {
            const std::wstring rel = native_rel(relative), source = join(g_staged, rel);
            if (!is_file(source)) continue;
            const std::wstring target = join(g_root, rel);
            make_dirs(parent(target));
            const std::wstring saved = join(old, leaf(rel));
            if (exists(target)) {
                if (!copy_file(target, saved)) throw Error{os_error(last_error(), target, saved)};
            }
            if (!copy_file(source, target)) throw Error{os_error(last_error(), source, target)};
        }
    } catch (const Error &) {
        status(L"Update failed, putting the previous version back...");
        // Each undo step runs even if one before it failed (a virus scanner may be holding a file). Giving up partway
        // would strand folders in update\old and leave an install that does not start.
        std::vector<std::wstring> stuck;
        undo(journal, old, stuck);
        std::vector<std::wstring> both = moved;
        both.insert(both.end(), added.begin(), added.end());
        for (const std::wstring &name : both) {
            const bool was_added = std::find(added.begin(), added.end(), name) != added.end();
            if ((exists(join(g_root, name)) && exists(join(old, name))) || was_added) {
                try {
                    retry([&] { remove_item(join(g_root, name)); });
                } catch (const Error &e) {
                    stuck.push_back(name + L" (could not remove: " + e.message + L")");
                }
            }
        }
        for (const std::wstring &name : moved) {
            try {
                retry([&] { rename(join(old, name), join(g_root, name)); });
            } catch (const Error &e) {
                stuck.push_back(name + L" (could not restore: " + e.message + L")");
            }
        }
        for (const wchar_t *relative : DEFAULTS) {
            const std::wstring rel = native_rel(relative), saved = join(old, leaf(rel));
            if (is_file(saved) && !copy_file(saved, join(g_root, rel)))
                stuck.push_back(std::wstring(relative) + L" (" + os_error(last_error(), saved, join(g_root, rel)) + L")");
        }
        if (!stuck.empty()) {
            // The next launcher start wipes update\old, so get the leftovers out of there first.
            const std::wstring kept_at = preserve(old);
            std::wstring list;
            for (size_t k = 0; k < stuck.size(); ++k) list += (k ? L", " : L"") + stuck[k];
            throw Error{L"Not everything of the previous version could be put back; move these from " + kept_at +
                        L" back yourself: " + list};
        }
        throw;
    }
    // The new version is in place. Now the player's own files return to the new folders. Anything that refuses to move
    // goes to a separate dated folder next to them, since update\ is wiped on the launcher's next start.
    std::vector<std::wstring> kept, left_in_old;
    status(L"Carrying over your own files...");
    for (const std::wstring &name : moved) {
        if (!(is_dir(join(old, name)) && is_dir(join(g_root, name)))) continue;
        try {
            carry_over(join(old, name), join(g_root, name), name, dropped, kept);
        } catch (const Error &e) {
            const std::wstring rescue = join(g_root, L"update-kept-" + timestamp());
            status(L"Files of yours in " + name + L" could not be returned (" + e.message + L"); you can find them in " +
                   leaf(rescue) + L".");
            try {
                // the previous package's own files there are not the player's: only the player's files are kept
                strip_package(join(old, name), name, package);
                if (!create_dir(rescue) && !already_exists(last_error())) throw Error{os_error(last_error(), rescue)};
                retry([&] { rename(join(old, name), join(rescue, name)); });
            } catch (const Error &rescue_error) {
                // Only mention it: the update itself succeeded and must not be reported as failed.
                left_in_old.push_back(name + L" (" + rescue_error.message + L")");
            }
        }
    }
    if (!left_in_old.empty()) {
        std::wstring list;
        for (size_t k = 0; k < left_in_old.size(); ++k) list += (k ? L", " : L"") + left_in_old[k];
        strip_package(old, L"", package);   // the new version is in place: keep only the player's files
        status(L"A few of your old files are left in " + preserve(old) + L": " + list);
    }
    // Folders updated file by file still hold the previous package's files that the new one no longer ships; they go
    // to update\old, as they would have with the whole folder swapped. The player's files stay where they are.
    std::vector<std::wstring> problems;
    for (const std::wstring &name : merged)
        for (const std::wstring &n : entries(join(g_root, name))) {
            const std::wstring path = join(join(g_root, name), n), rel = name + L"/" + n;
            if (dropped.count(rel) || holds_dropped(dropped, rel) || (is_dir(path) && python_cache(n)))
                prune(path, rel, old, dropped, problems);
        }
    // Clean up top-level items that are not in the new package any more (e.g. python\ after the launcher stopped
    // needing Python): remove what the previous package shipped there, keep what the player put there.
    for (const std::wstring &name : entries(g_root)) {
        if (name == L"analysis" || name == L"update" || name.rfind(L"update-kept-", 0) == 0) continue;
        if (std::find(items.begin(), items.end(), name) != items.end()) continue;
        if (!dropped.count(name) && !holds_dropped(dropped, name)) continue;
        status(L"Removing " + name + L" (no longer part of the launcher)...");
        prune(join(g_root, name), name, old, dropped, problems);
    }
    // update-kept-* folders of earlier updates: their package files (a whole previous version, when the rescue moved
    // a folder) go, so they hold only the player's own files; a folder left empty goes too
    for (const std::wstring &name : entries(g_root)) {
        if (name.rfind(L"update-kept-", 0) != 0 || !is_dir(join(g_root, name)) || is_link(join(g_root, name))) continue;
        strip_package(join(g_root, name), L"", package);
        if (entries(join(g_root, name)).empty()) remove_dir(join(g_root, name));
    }
    for (const std::wstring &p : problems) print(L"Not removed: " + p);
    for (const std::wstring &rel : kept) print(L"Kept: " + rel);
    std::string manifest;
    for (const std::wstring &f : new_files) manifest += utf8(f) + NEWLINE;
    if (!write_text(join(g_root, MANIFEST), manifest))
        print_error(std::wstring(L"Failed to write ") + MANIFEST + L": " + os_error(last_error()));
}

#ifdef _WIN32
const wchar_t *const USAGE = L"usage: apply_update.exe [-h] --root ROOT [--wait-pid WAIT_PID] [--no-restart] [--quiet]";
const wchar_t *const PROGRAM = L"apply_update.exe";
#else
const wchar_t *const USAGE = L"usage: apply_update [-h] --root ROOT [--wait-pid WAIT_PID] [--no-restart] [--quiet]";
const wchar_t *const PROGRAM = L"apply_update";
#endif

// the folder of the staged package this program came with: its own folder (tools) is in it
std::wstring staged_package() {
#ifdef _WIN32
    std::vector<wchar_t> self(32768);
    GetModuleFileNameW(nullptr, self.data(), 32768);
    std::wstring s = self.data();
#else
    std::wstring s = widen(port::executable_path().string());
#endif
    s = parent(s);   // the program's own folder, tools; its parent is the staged package
    return parent(s);
}

// starts the new launcher from the root and does not wait for it
void restart_launcher() {
#ifdef _WIN32
    std::wstring starter = join(g_root, L"UCMR_Launcher.exe");
    if (GetFileAttributesW(starter.c_str()) == INVALID_FILE_ATTRIBUTES) starter = join(g_root, L"Uncharted Launcher.exe");
    std::wstring cmd = L"\"" + starter + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(starter.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, g_root.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        write_std(STD_ERROR_HANDLE, L"Failed to start " + starter + L": " + os_error(GetLastError()));
    }
#else
    const std::wstring starter = join(g_root, L"UCMR_Launcher");
    port::Spawn s;
    s.argv = {utf8(starter)};
    s.cwd = utf8(g_root);
    s.out = port::Spawn::null;
    s.own_group = true;   // it outlives this program
    port::Child child;
    std::string error;
    if (!port::spawn(s, child, error)) print_error(L"Failed to start " + starter + L": " + widen(error));
#endif
}

// the program: arguments (argv[0] first), the install, the restart; the exit code
int run(const std::vector<std::wstring> &argv) {
    const int argc = (int)argv.size();
    std::wstring root;
    unsigned long wait_pid = 0;
    bool restart = true, quiet = false, have_root = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i], value;
        const size_t eq = a.find(L'=');
        bool has_value = false;
        if (a.rfind(L"--", 0) == 0 && eq != std::wstring::npos) {
            value = a.substr(eq + 1);
            a = a.substr(0, eq);
            has_value = true;
        }
        bool missing = false;
        auto take = [&]() -> std::wstring {
            if (has_value) return value;
            if (i + 1 < argc) return argv[++i];
            missing = true;
            return L"";
        };
        if (a == L"--root") { root = take(); have_root = !missing; }
#ifdef _WIN32
        else if (a == L"--wait-pid") wait_pid = (DWORD)_wtoi(take().c_str());
#else
        else if (a == L"--wait-pid") wait_pid = (unsigned long)std::wcstoul(take().c_str(), nullptr, 10);
#endif
        else if (a == L"--no-restart") restart = false;
        else if (a == L"--quiet") quiet = true;
        else if (a == L"-h" || a == L"--help") { print(USAGE); return 0; }
        else {
            print_error(std::wstring(USAGE) + L"\n" + PROGRAM + L": error: unrecognized arguments: " + argv[i]);
            return 2;
        }
        if (missing) {
            print_error(std::wstring(USAGE) + L"\n" + PROGRAM + L": error: argument " + a + L": expected one argument");
            return 2;
        }
    }
    if (!have_root) {
        print_error(std::wstring(USAGE) + L"\n" + PROGRAM + L": error: the following arguments are required: --root");
        return 2;
    }
    g_root = full_path(root);
    g_staged = staged_package();
#ifdef _WIN32
    if (!quiet) open_window();
#else
    (void)quiet;   // no window on Linux
#endif
    bool ok = true;
    try {
        if (wait_pid && !wait_for_exit(wait_pid, 60)) throw Error{L"The launcher did not exit in time."};
        apply();
        status(L"The update is installed.");
    } catch (const Error &e) {
        ok = false;
        const std::wstring message = L"The update failed to install; the previous version stays in place.\n\n" + e.message +
                                     L"\n\nIf the game is running, or a launcher file is open in another program, close "
                                     L"it and try once more, or get the new version from the "
                                     L"releases page.";
        print_error(message);
#ifdef _WIN32
        if (g_window) MessageBoxW(g_window, message.c_str(), TITLE, MB_OK | MB_ICONERROR);
#endif
    }
#ifdef _WIN32
    if (g_window) DestroyWindow(g_window);
#endif
    if (restart) restart_launcher();
    return ok ? 0 : 1;
}

}  // namespace

#ifdef _WIN32
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    return run(std::vector<std::wstring>(argv, argv + argc));
}
#else
int main(int argc, char **argv) {
    std::vector<std::wstring> args;
    for (int i = 0; i < argc; ++i) args.push_back(widen(argv[i]));
    return run(args);
}
#endif
