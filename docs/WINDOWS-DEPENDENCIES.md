# Windows dependencies: the map for one build that runs on Windows and Linux

**Goal:** one source tree that builds and runs on **both Windows and Linux**. Windows stays fully supported with no
behaviour change; Linux is added beside it. The CI builds a Windows package (`UCMR_windows.zip`) **and** a Linux
package (a tarball first, an AppImage later) from the same commit, runs the same tests on both, and one release
carries both.

For every dependency below the target is **a shared code path** (standard C++ that works on both) or **one small
platform function with the Windows and the Linux body side by side** behind `#ifdef _WIN32` (AGENTS.md, "Portable
code"). Never "replace Windows with Linux": the Windows body stays as it is today.

Snapshot: release 0.10.169 (commit `81badc48`). Counts come from a token scan of every tracked file (Win32/CRT names
followed by `(`, Windows headers, types, wide-string constructs, `.exe`/MSYS2 names), so they include comments and
`CloseHandle`-style bookkeeping; they size the work, they are not exact call counts. Nothing was compiled on Linux.

## Decisions (owner, 2026-11-25)

| Question | Decision |
|---|---|
| Goal | One source tree, **two packages from every commit**: the Windows zip as today and a Linux package. Windows keeps working exactly as before. |
| First Linux package | **Tarball** (`UCMR_linux.tar.gz`) with the same folder layout as the Windows zip, so the launcher can keep writing into its own folder. An AppImage comes later (it is read-only; its data folder has to move outside it first). |
| Online sign-in token on Linux | A **private file with mode 0600** next to the other settings. Windows keeps DPAPI. No keyring dependency (works on any distro and on Steam Deck game mode). |
| U3 helper PC side | **One shared backend**: the runtime exports a small table of host functions (sockets, time, files, ...) and the helper's single PC backend calls them on both Windows and Linux, instead of a third, Linux-only backend. |
| The earlier draft's code (`launcher/native/port.h`, `sha256.h` and edits in 9 files) | **Taken over** into this work and checked against this map, so nothing is done twice. |

**Progress:** phases 0, 1 and 2 of §13 are done: the Linux CI workflow (§7.4), the launcher core building on Linux
with the platform layer (§4.1, §12), and processes: the launcher starts the imports, the game starter and the runtime,
checks for its own updates over libcurl, and `u2import`, `u3import` and `u3fix` build, link and run on Linux (§4.7,
§4.8). Phase 3 is done too: the Qt window runs on Linux (§4.2) and every push builds `UCMR_linux.tar.gz` (§7.4, §7.5).
Phase 4 is done in code: one `gow3-probe` CMake target for both systems, the runtime's Linux platform functions in C++
(`host_linux.cpp`, `host_linux_shared.cpp`), the unit tests out of `if (WIN32)`, and the runtime in the Linux package
(§4.3–§4.6). The runtime starts on Linux and its tests pass there; no game has been played on Linux yet. Phase 5 is
done in code: the runtime's host function table, the helper's one PC backend over it (helper v7.267, not shipped yet:
it needs a LAN and online test first) and both online programs on Linux. Phase 6 is done except DLSS and the U2 crash
log (§13), phase 7 except the AppImage (the tarball comes first, as decided).

## How to help

This is a large job and contributions are welcome. To keep two people from changing the same files:

1. **Pick one row** of §4 (one file or one small group of files) or one phase item of §13, and say which one you take
   on the project's Discord or in a GitHub issue before you start.
2. **Read** `AGENTS.md` ("Portable code") and `CONTRIBUTING.md`: C++20 only (no new C, no Python), the standard
   library first, and every OS call that has no portable form goes into **one small platform function** with the
   Windows and the Linux body side by side behind `#ifdef _WIN32`. Use the platform layer in §12 instead of writing a
   new one.
3. **Windows must not change.** Keep the Windows body exactly as it is today (move it into the platform function as
   is); the Windows CI build and its tests must stay green.
4. **One focused pull request per row**, from your fork, rebased on the latest `main`. In the PR say which row(s) of
   this document it covers, how you built it on Windows, and whether it compiled on Linux (any distro; Ubuntu 22.04 is
   what CI will use).
5. **Update this file** in the same PR: mark the row done (or "Linux body is a stub"), so the next person sees the
   current state.
6. The U3 helper's PC backend (§4.10) and the online programs depend on code outside this repository; the maintainers
   change those. The host function table (`gow3_pc/src/uncharted_host.h`, `runtime_host_api.cpp`) is open like any
   other runtime file, but its layout is fixed (version 1): new functions go at the end with a new version.

Contents

1. [Summary by area](#1-summary-by-area)
2. [Biggest blockers for a Linux build](#2-biggest-blockers-for-a-linux-build)
3. [Already portable: do not redo](#3-already-portable-do-not-redo)
4. [Source: per-area and per-file tables](#4-source-per-area-and-per-file-tables)
5. [Calling convention, TLS, stack, exceptions (the runtime's ABI work)](#5-calling-convention-tls-stack-exceptions)
6. [Libraries and system DLLs](#6-libraries-and-system-dlls)
7. [Build system, toolchain, CI, packaging, releases](#7-build-system-toolchain-ci-packaging-releases)
8. [Paths and text](#8-paths-and-text)
9. [Processes and runtime behaviour](#9-processes-and-runtime-behaviour)
10. [Data and scripts](#10-data-and-scripts)
11. [Game-data assumptions](#11-game-data-assumptions)
12. [The platform layer](#12-the-platform-layer)
13. [Transition order (both builds green at every step)](#13-transition-order)
14. [Not checked / open questions](#14-not-checked--open-questions)
15. [Relation to the earlier draft](#15-relation-to-the-earlier-draft)

Effort: **S** = hours, a mechanical change; **M** = a day or two, needs a platform function and a test; **L** = a
design decision or a feature that needs its own Linux implementation.

---

## 1. Summary by area

"Files w/ API" = files that call a Windows API; "API sites" = Win32/CRT call tokens; "wide" = `wchar_t`,
`std::wstring`, `L"..."` hits; "#if" = existing `_WIN32` / `__linux__` conditionals (a high number means both bodies
often exist already).

| Area | Files | Lines | Files w/ API | API sites | Wide | `#if` | State | Effort |
|---|---:|---:|---:|---:|---:|---:|---|---|
| `launcher/native` (core DLL, game starter, updater) | 33 | 18 940 | 27 | 855 | 1 921 | 0 | Windows-only, no Linux branch anywhere | **L** |
| `launcher-qt` (Qt window) | 5 C++ + 24 QML | 1 258 | 0 | 0 | 4 | 0 | Qt is portable; DLL name, `LOCALAPPDATA`, `wchar_t` API, `WIN32`/winres in CMake | S — **done** (phase 3) |
| `gow3_pc/src/*.c` (HLE runtime, loader `probe.c`) | 28 | 13 756 | 13 | 262 | 1 | 79 | Originally POSIX; Windows added via `compat_win.{c,h}` and `#ifdef` bodies; both exist | M |
| `gow3_pc/src` C++ top level (`pokes`, `debug_menus`, `name_dictionary`, `sid_names`, ...) | 19 | 2 073 | 2 | 16 | 13 | 3 | `pokes.cpp` unguarded Win32 | S |
| `gow3_pc/src/u2mp` | 77 | 33 387 | 5 | 59 | 15 | 19 | Mostly portable (`host_memory.h`, `net.cpp`, `online_link.cpp` have both bodies); 2 unguarded spots | S |
| `gow3_pc/src/u3fix` | 26 | 3 709 | 4 | 7 | 0 | 11 | Both bodies present | S |
| `gow3_pc/src/u3voice` | 2 | 385 | 1 | 1 | 0 | 0 | `__declspec(dllexport)` | S |
| `gow3_pc/gpu` own code (`shim/`, `dlss_bridge/`, `tools/`) | 67 | 12 812 | 10 | 52 | 22 | 23 | Mostly both bodies; DLSS bridge Windows/MSVC-only | M |
| `gow3_pc/gpu/shadps4` (vendored, with `gow3:` changes) | 376 | 124 815 | 25 | 99 | 37 | 137 | Upstream is Linux-first; our `gow3:` Windows additions need Linux bodies in ~8 files | M |
| `gow3_pc/tests` | 104 | 34 017 | 24 | 127 | 6 | 32 | ~36 of the CMake test targets sit inside `if (WIN32)` | M |
| `gow3_pc/tools`, `gow3_pc/packaging` | 5 | 795 | 4 | 18 | 13 | 4 | Windows tools by design (`play.c`, `bridge_helper`) | S |
| `u3mp/patcher/importer` (`u3import.exe`) | 11 | 5 475 | 4 | 73 | 85 | 0 | Process runner, junctions, hard links, `wmain` | M |
| `u3mp/patcher/tools/fixes` (`u3fix.exe`) | 78 | 31 425 | 5 | 96 | 67 | 0 | 73 files already pure std C++; `proc.cpp` is the hot spot | M |
| `u3mp/patcher/recipe` | 1 | 602 | 1 | 6 | 0 | 4 | `CommandLineToArgvW` | S |
| `u2mp/patcher/importer` (`u2import.exe`) | 21 | 9 542 | 1 | 15 | 12 | 0 | Only `u2import.cpp` | S |
| `u3mp/helper-source`, `u3mp/helper` (guest code) | 145 | 138 091 | 5 | 40 | 5 | 16 | Runs inside the game, but its **PC backend `plat_pc.inc` calls Windows DLLs** | **L** |
| `tools/` (`windows_starter`, `mkzip`, `code_ids`, scripts) | 3 C++ + 5 sh | 406 C++ | 3 | 23 | 29 | 0 | Build/packaging tools; Windows-shaped | M |
| Build scripts, CMake, CI, packaging | see §7 | | | | | | MSYS2 CLANG64 only; Linux branch of `gow3_pc/build.sh` is stale | **L** |
| **Own code total** (without vendored shadPS4/third_party) | | | **109** | **~1 650** | **~2 200** | | | |

Typed macros for the guest ABI (not counted above, see §5): `ABI` 415 uses in 28 files, `U2MP_GUEST` 658 uses in 54
files, `U3VOICE_ABI` 19 uses, plain `__attribute__((sysv_abi))` 41 uses in `u3fix`.

## 2. Biggest blockers for a Linux build

1. **No Linux target builds the full runtime.** `gow3_pc/gpu/CMakeLists.txt` defines `gow3-probe` (runtime + `u2mp` +
   `u3fix` + `u3voice` + `pokes` + `debug_menus` + Opus) and ~36 tests only inside `if (WIN32)`. The Linux branch of
   `gow3_pc/build.sh` is the old pre-Windows path: it compiles `src/runtime*.c` + `probe.c` with `cc -std=c11` and
   none of the C++ subsystems. First step: one CMake target for both systems.
2. **`launcher/native` has no Linux code at all** (855 API sites, 1 921 wide-string hits, exported API in
   `wchar_t`). It needs the platform layer (§12) plus a decision on its exported string type (§4.1).
3. **The U3 multiplayer helper's PC backend is Windows code running inside the guest** (`u3mp/helper-source/plat_pc.inc`):
   it walks the PEB, finds `ws2_32.dll`/`kernel32.dll`/`user32.dll`/`advapi32.dll` exports and calls them through
   `ms_abi` thunks (Winsock, `VirtualAlloc`, `CreateThread`, a Win32 text box, `RtlGenRandom`, `_putenv_s`). Linux
   needs another backend, or (simpler, one backend for both PCs) runtime-provided host functions in the SysV ABI.
4. **Case-sensitive file systems.** The runtime resolves `/app0` paths against the game folder with the exact case
   the game asks for (only the update overlays are indexed case-insensitively, `runtime_file.c`); the launcher and
   importers fold case with `CharLowerBuffW` and open `PS3_GAME/USRDIR/...`, `eboot.bin`, `sce_sys` by fixed names.
   See §8.4.
5. **Process control everywhere is `CreateProcessW` + anonymous pipes + handle lists** (`core.cpp`, `play.cpp`,
   `start.cpp`, `updater.cpp`, `apply.cpp`, importer `common.cpp`, `dctools/proc.cpp`, runtime `online_link.cpp`).
   One `port::spawn` (§12) unlocks the launcher, the importers and the recipe.
6. **Update and install model.** The launcher keeps all data in its own install folder (`analysis/`, `u3mp/`,
   `u2mp/`, `game/`, `launcher.json`) and updates itself with `apply_update.exe` (a Win32 window). An AppImage is a
   read-only image: either ship the Linux build as a folder/tarball with the same layout (same updater flow), or make
   the AppImage keep its data root outside the image. See §7.6.
7. **Online programs** (`uncharted_online.exe`, `u2_online.exe`, built outside this repository) use Winsock, BCrypt
   and DPAPI (`CryptProtectData`) for the stored sign-in token. **Done** (phase 5): the same sources build
   `uncharted_online` and `u2_online` on Linux (BSD sockets with Winsock error numbers, own hashes, the token in the
   `0600` settings file as decided); the Linux package job builds them on Ubuntu 22.04.
8. **Windows-only runtime features with Linux stubs:** old-CPU instruction emulation, the freeze report and the guest
   profiler have Linux bodies now (phase 6); minidumps (dbghelp) have no Linux counterpart without a debugger, and DLSS
   (MSVC bridge) stays Windows-only.

## 3. Already portable: do not redo

- **Runtime C core** (`gow3_pc/src/runtime_*.c`, `probe.c`): written against POSIX (pthreads, `mmap`, `sigaction`,
  `clock_gettime`, BSD sockets, `opendir`); Windows is a layer on top (`compat_win.h`/`compat_win.c`, winpthreads,
  `#ifdef _WIN32` bodies: 79 conditionals). POSIX bodies exist for memory, threads/TLS (`arch_prctl(ARCH_SET_GS)`),
  fault handling (`probe.c` `fault()`), files, time, sockets, savedata. Check them for staleness (§13 phase 4) but do
  not rewrite them.
- **`gow3_pc/src/u2mp`**: `host_memory.h` (VirtualAlloc/mmap, both), `net.cpp` (Winsock/BSD), `online_link.cpp`
  (job object / `__linux__` parent-death branch), `cinema_store.cpp`, `crashlog.cpp` (`#else` stub). Only
  `session.cpp:3054` (`_putenv_s`) and `u2_online_abi.h` (`__cdecl`, 30 uses) are unguarded.
- **`gow3_pc/src/u3fix`**: `hook.cpp` (VirtualAlloc near the image / `mmap(MAP_32BIT)`), `counter_wait_log.cpp`,
  `net_pool.cpp`, `script_errors_log.cpp`, `tonemap_curve.cpp` all have both bodies.
- **`gow3_pc/gpu`**: shadPS4 is Linux-first upstream (13 vendored files carry Linux/Unix branches; X11/Wayland window
  code in `shim/window.cpp`; `gow3gpu` already builds as a shared library on non-Windows with
  `--allow-shlib-undefined`). Our shims `gow3_threads.h`, `gow3_write_log.cpp`, `gow3_terminate.cpp`,
  `gow3_perf_stats.cpp`, `gow3_profiler.cpp`, `gow3_overlay.cpp`, `gow3gpu.cpp` have `#else` branches.
- **Importers**: 73 of 78 `u3mp/patcher/tools/fixes` files, 6 of 11 importer files and 20 of 21 `u2mp/patcher`
  files use only `std::filesystem`, `<fstream>`, `<thread>`; path text is UTF-8 through `P()`/`S()` helpers.
- **`launcher-qt` QML** (24 files) and Qt C++ apart from 5 spots; SDL3 (pads, window, audio), Vulkan, zlib, Opus,
  FFmpeg are cross-platform.
- **Bash scripts** already run under a POSIX shell; `.gitattributes` keeps `*.sh` and patches LF.

## 4. Source: per-area and per-file tables

### 4.1 `launcher/native` (the launcher core `uncharted_native.dll`, `game_starter.exe`, `apply_update.exe`)

Exported API (`native.h`): `UN_API` = `__declspec(dllexport)`; 14 of 35 exported functions take `const wchar_t *`
paths, and `launcher-qt` passes `QString::toStdWString()`. Proposal: `using un_path_char =
std::filesystem::path::value_type;` (`wchar_t` on Windows, `char` UTF-8 on Linux) so the Windows ABI is unchanged
and Linux gets UTF-8; `UN_API` becomes `__attribute__((visibility("default")))` off Windows.

**State (phase 1 done):** `native.h` has `typedef wchar_t un_path_char` on Windows and `char` (UTF-8) on Linux, and
every exported path argument uses it (the Windows ABI is unchanged; `launcher-qt` changes in phase 3). The whole
core compiles and links on Linux (`libuncharted_native.so`, `-Wl,--no-undefined`) and `launcher/native/core_smoke.cpp`
calls it in CI. Inside, the files keep their wide (`std::wstring`) text for now: `port::wtext` / `port::arg` convert
at the exported API, `port::path` at the file system (on Linux a backslash in our path text becomes `/`).

**State (phase 2 done):** the process layer is in `port.h` (§12): on Linux `port::spawn` (fork + execve with a
close-on-exec error pipe, so a missing program fails to start like `CreateProcessW`), `wait`, `running`, `kill`,
`read_some`/`write_all`, `kill_tree`, `process_alive`, `list_processes`, `program_name`. Every Windows caller keeps its
own `CreateProcessW` body unchanged beside the Linux one. `launcher/native/port_smoke.cpp` checks the layer in CI.

| File | Lines | Sites | Windows APIs / constructs | Shared path or platform function | Effort |
|---|---:|---:|---|---|---|
| `core.cpp` | 2 565 | 96 | `CreateFileW`, `FindFirstFileW`, `MoveFileExW`, `SHCreateDirectoryExW`, `GetDiskFreeSpaceExW`; `CreatePipe`/`CreateProcessW`/`PeekNamedPipe`/`TerminateProcess` with `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`, `CREATE_NO_WINDOW`, `CREATE_UNICODE_ENVIRONMENT`; `GetEnvironmentStringsW`; `GetTickCount64`; `SecureZeroMemory`; `%APPDATA%`/`USERPROFILE` seed file | `std::filesystem`; `port::spawn`/`port::Pipe`; `port::env_block`; `std::chrono::steady_clock`; `port::secure_zero`; `port::user_dirs` | L — **done**: file helpers, folders, free space, the settings file and the environment list are portable (phase 1); `run`, the imports (log file, `/dev/null` input, nice 10 for `BELOW_NORMAL_PRIORITY_CLASS`), STOP (`port::kill_tree`) and the online program's pipes use `port::spawn` (phase 2); program names through `port::program_name`. The online program itself has no Linux build yet (phase 5), so the online section stays hidden there |
| `apply.cpp` (`apply_update.exe`) | 722 | 90 | Standalone updater: Win32 progress window (`RegisterClassW`, `CreateWindowExW`, `PeekMessageW`, DPI), `MessageBoxW`, `wWinMain`, `CommandLineToArgvW`, `OpenProcess` (wait for the launcher), `CopyFileW`/`MoveFileExW`/`RemoveDirectoryW`, `CharLowerBuffW` sort | File logic shared (`std::filesystem`); `port::wait_process_exit`; UI: Windows window kept, Linux runs without a window (log + Qt message from the relaunched launcher). Not used inside an AppImage (§7.6) | L — **done** (phase 7): one program for both systems; every file-system and process call is a small function with the Windows body unchanged and a Linux body beside it; on Linux no window (status lines to `analysis/update-apply.log`), `tools/apply_update` in the package. The Windows exe leaves byte-identical folders to the one before (checked on a synthetic update, also with a folder held open) |
| `updater.cpp` | 733 | 82 | WinHTTP (GitHub API + download), BCrypt SHA-256, `CreateProcessW` (starts apply), asset name `UCMR_windows.zip` | `port::http_get` (WinHTTP / libcurl), the shared SHA-256 (§6), `port::spawn`; asset name per OS (§7.7) | M — **phase 2 done**: one `http_get`-style `Http` in the file (WinHTTP body unchanged, libcurl on Linux: same User-Agent, time-outs, redirects never down to http); the installed version, the release lookup, the download with resume and the SHA-256 check are shared; Linux picks the asset `UCMR_linux.tar.gz` by name (no release has it yet, so it finds nothing newer). **Phase 7 done**: the tarball is unpacked as a stream through zlib (GNU long names, pax paths, permission bits, links only inside the package) into `update/staged` with the zip path rules (single top folder dropped, nothing outside the folder, required files, VERSION), then `tools/apply_update` starts in its own session |
| `start.cpp` | 1 067 | 79 | Runtime start: `CopyFileW`, `SetFileTime`, `GetFinalPathNameByHandleW`, `DuplicateHandle`, `CreateProcessW` + `ResumeThread`, `VirtualQueryEx`, `QueryPerformanceCounter`, `GetSystemTimePreciseAsFileTime`, `\\?\` paths | `std::filesystem` (`copy_file`, `last_write_time`, `weakly_canonical`), `port::spawn`; drop the suspended start on Linux | L — **done**: the start sequence is shared; the runtime starts through `port::spawn` (argv vector, whole environment, log, working folder); no suspended start, `VirtualQueryEx` reserve check or ASLR policy on Linux; `VK_ADD_LAYER_PATH` uses `:`. The runtime itself is phase 4 |
| `hang.cpp` | 514 | 75 | Freeze watchdog: `dbghelp` (`MiniDumpWriteDump`, `StackWalk64`, `Sym*`), `SuspendThread`/`GetThreadContext`, `ReadProcessMemory`, toolhelp thread list, `psapi` | Windows body kept; Linux: `/proc/<pid>/task/*/stat` + `ptrace(PTRACE_SEIZE/INTERRUPT)` for registers, or a stub "no freeze report on Linux" first | L — **done** (phase 6): the same log watch and `hang-*.txt`; threads stopped one at a time with `PTRACE_SEIZE`/`PTRACE_INTERRUPT` (the starter is the game parent), memory through `process_vm_readv`, host frames from the rbp chain; no minidump on Linux (the report names `gcore`) |
| `starter_files.cpp` | 527 | 61 | Game-folder tree copy (`FindFirstFileW`, `CopyFileW`, `SetFileTime`), WIC (`IWICImagingFactory`) icon decode via COM, `CharLowerBuffW` | `std::filesystem`; icon: Windows only (Linux uses the `.desktop` icon) | M — **done**: user folder, game executable copy (`gow3-probe` → `Uncharted`), boot view and old-view cleanup on `std::filesystem`; the window icon is decoded by `png.h` on Linux (the BMP is byte-identical to the Windows one) |
| `system.cpp` | 631 | 53 | Wine detection (`wine_get_version`), Wine path mapping, `GetVolumeInformationW`/`GetDriveTypeW`/`DeviceIoControl` (slow-drive warning), `SHGetKnownFolderPath(FOLDERID_Desktop)`, `OpenProcess` liveness, `\\?\` | Wine code stays Windows-only (native Linux never runs under Wine); `port::drive_kind` (`/sys/block/*/queue/rotational`), `port::user_dirs` (XDG desktop), `port::process_alive` | M — **done**: `un_system`, `un_pid_alive` (`kill(pid, 0)`), folder listing with the same `*`/`?` patterns; Wine helpers answer "no"; the slow-drive warning stays Windows-only on purpose (its cause, no write caching on USB/SD/FAT drives, and its advice are Windows ones) |
| `steam.cpp` | 993 | 52 | Steam path from registry (`RegGetValueW` HKCU `SteamPath` / HKLM `InstallPath`), `ProgramFiles`, process scan for `steam.exe` (toolhelp), `shortcuts.vdf` writer, WIC PNG encode (grid art), `_wcsicmp` | VDF writer is portable; `port::steam_root` (`~/.steam/steam`, `~/.local/share/Steam`, Flatpak `~/.var/app/com.valvesoftware.Steam/...`), `port::list_processes`; PNG: own encoder on zlib or Qt | M–L — **done** (phase 6): the VDF code, matching, backups and artwork are shared; Linux bodies for files, Steam folders (native, Flatpak, Snap), the `steam` process and PNG through `png.h` (zlib); artwork pixel-identical to Windows |
| `play.cpp` | 801 | 41 | Game process + stdout pipe, `GetLocalTime`, `\r\n` logs | `port::spawn`, `<chrono>` | M — **done**: the environment, crash reason, questions and end are shared; the starter runs through `port::spawn` with one output pipe, the same run log (`\r\n`), STOP = `port::kill_tree` |
| `memstat.cpp` | 133 | 32 | toolhelp, `K32GetProcessMemoryInfo`, `GlobalMemoryStatusEx`, `CreateThread` | `port::process_memory` (`/proc/<pid>/status`), `/proc/meminfo`, `std::thread` | S–M — **done** (phase 6): `/proc/<pid>/status`, `/proc/<pid>/fd`, `/proc/meminfo` into the same kind of CSV (Linux column names) |
| `runtime_env.cpp` | 734 | 31 | Tree copy, `CreateThread`, `"C:/msys64"` dev fallback | `std::filesystem`, `std::thread`; MSYS2 fallback Windows-only | M — **done** (the `C:/msys64` PATH fallback stays Windows-only) |
| `gameupdate.cpp` | 438 | 27 | Tree walk, `GetFinalPathNameByHandleW`, `CharLowerBuffW`, `\\?\` | `std::filesystem`; case rule §8.4 | M — **done**: name matches still fold case; `resolved()` keeps the case on Linux (two names that differ only in case are two folders there) |
| `config.cpp` | 524 | 26 | JSON config, atomic `MoveFileExW`, `lstrcmpiW`, `wine_get_unix_file_name` | `port::replace_file`; Wine part Windows-only | S–M — **done** |
| `starter.cpp` | 262 | 24 | BCrypt SHA-256, env, file read | shared SHA-256, `std::getenv`, `<fstream>` | S — **done** |
| `session_value.cpp` | 278 | 24 | Whole-file/part reads, atomic write, `SecureZeroMemory` | `<fstream>`, `port::write_atomic`, `port::secure_zero` | S — **done** |
| `self_reader.h` | 244 | 19 | BCrypt SHA-256, file I/O | shared SHA-256, `<fstream>` | S — **done** (`sha1.h`/`sha256.h` on Linux, bcrypt kept on Windows) |
| `shortcuts.cpp` | 171 | 17 | COM `IShellLinkW` + `IPersistFile` `.lnk` on the Desktop | Windows body kept; Linux writes a freedesktop `.desktop` file (desktop and `~/.local/share/applications`) | M — **done** (phase 6): freedesktop `.desktop` files (Exec quoted per the specification, executable), desktop folder from XDG user dirs |
| `gamepads.cpp` | 200 | 15 | `LoadLibraryExW("SDL3.dll")`, `__cdecl` pointers, `"C:/msys64"` fallback | `port::load_library` (`SDL3.dll` / `libSDL3.so.0`) | S — **done** |
| `actions.cpp` | 476 | 14 | Recursive delete with attributes, `ShellExecuteW("open")` | `std::filesystem::remove_all`, `port::open_path` (`xdg-open`) | S — **done** (`port::open_path`: `xdg-open`) |
| `display.cpp` | 401 | 13 | DXGI adapters/outputs | Windows body kept; Linux: SDL3 `SDL_GetDisplays` + Vulkan device name | S–M — **done** (adapter names through the Vulkan loader on Linux) |
| `prepare.cpp` | 428 | 12 | `FindFirstFileW`, `GetFullPathNameW`, `bcrypt.h` | `std::filesystem` | S — **done** |
| `gamefiles.cpp` | 127 | 12 | `GetFileInformationByHandleEx` (file identity) | `port::file_id` (`st_dev`/`st_ino`) | S — **done** |
| `patcher.cpp` | 498 | 9 | File read/write, `GetEnvironmentVariableA` | `<fstream>`, `std::getenv` | S — **done** |
| `game_starter.cpp` | 60 | 9 | `wmain`, `LoadLibraryExW("uncharted_native.dll")` | `main` + `port::load_library` (`libuncharted_native.so`) | S — **done** (`main` + `dlopen`) |
| `updates.cpp` | 367 | 6 | `GetTickCount64`, `_wgetenv` | `<chrono>`, `std::getenv` | S — **done** |
| `patch_tables.cpp` | 923 | 4 | `GetEnvironmentVariableA`, `SetLastError` | `std::getenv` | S — **done** |
| `native.h` | 123 | 1 | `__declspec(dllexport)`, `wchar_t` API | see above | S — **done**: `un_path_char` |
| `link.cpp`, `controls.cpp`, `pages.cpp`, `json.h`, `inflate.h`, `log_history.h` | 2 400+ | 0–1 | `std::wstring` names only (`log_history.h`, `link.cpp`) | `fs::path` / UTF-8 strings | S — **done** |
| `build.sh` | 35 | | MSYS2 `CLANG64` default `/c/msys64/clang64`, `-municode`, `-shared -static`, 11 system libs, winres | Branch on `uname`: Linux builds `libuncharted_native.so` (`-fPIC -shared`), no winres | S — **done**: `libuncharted_native.so` + `game_starter` (`CXX`, `CXXFLAGS` choose the compiler); links `-lcurl` on Linux (Ubuntu: `libcurl4-openssl-dev`) |

### 4.2 `launcher-qt`

| File | Windows constructs | Shared path / platform function | Effort |
|---|---|---|---|
| `src/backend.cpp` | `launcher/native/uncharted_native.dll` file name (3x), `%LOCALAPPDATA%` cache path via `qEnvironmentVariable`, `toStdWString()` for the core API, `SDL3.dll` note | file name per OS (`QLibrary` with base name + suffix), `QStandardPaths::CacheLocation`, `un_path_char` (§4.1) | S — **done**: `CoreFile` is `libuncharted_native.so` on Linux; `corePath()` gives the core `un_path_char` (UTF-16 on Windows as before, `QFile::encodeName` UTF-8 on Linux); the old `%LOCALAPPDATA%` cache clean-up is Windows-only (no `QStandardPaths` needed: the QML cache lives in the launcher's folder on both systems, and no Linux version ever wrote one elsewhere) |
| `src/backend.h` | `wchar_t` function pointer types | `un_path_char` | S — **done** (includes `native.h`) |
| `src/main.cpp` | — | `--self-check` for CI (QML and core load, core ready, normal close, exit 0); `setDesktopFileName` on Linux | S — **done** |
| `CMakeLists.txt` | `qt_add_executable(UCMRLauncher WIN32 ...)`, `ucmr_winres(...)` | `WIN32` and winres only `if (WIN32)`; Linux: `.desktop` + PNG icon | S — **done**: `WIN32_EXECUTABLE` and winres inside `if (WIN32)`; Linux run path `$ORIGIN/lib`; `linux/UCMR_Launcher.desktop`, icon = the existing `launcher/assets/icon.png` (256x256). Fonts: the QML asks for Segoe UI, which Linux lacks (fontconfig picks its sans font); the layout is unchanged |

### 4.3 `gow3_pc/src` runtime (C)

| File | Lines | Sites | Windows constructs | Linux body | Effort |
|---|---:|---:|---|---|---|
| `probe.c` (loader, fault handler) | 1 643 | 125 | VEH (`AddVectoredExceptionHandler`, `EXCEPTION_*`, `CONTEXT`), `SetUnhandledExceptionFilter`, `RtlLookupFunctionEntry`/`RtlVirtualUnwind`, TEB (`NtCurrentTeb`, stack bounds), `VirtualAlloc`/`VirtualQuery`/`FlushInstructionCache`, toolhelp thread suspend for the exec watch, `GetModuleFileNameA`, `CreateProcessA` restart (`GOW3_RESTART_COMMAND`), TEB TLS slot patch (`patch_tcb_loads`) | **done**: the Linux signal handlers, the watchdog (off unless `--timeout`, as Windows has none), the restart and the near-image stub memory are C++ in `host_linux.cpp`; they step over the `int 0x41` assertion trap and a lone `int3` as the Windows handler does, and a crash on a pipeline cache warm-up thread leaves the cache mark. The exec watch (`GOW3_EXEC_WATCH`, `GOW3_EXEC_WATCH_FILE`) is in `host_linux.cpp` too (phase 6): one hardware breakpoint per watch and thread through `perf_event_open` with a synchronous `SIGTRAP` (`TRAP_PERF`; Linux 5.13 or newer, `perf_event_paranoid` 2 or less), logged in the Windows format. Still Windows-only: guest affinity/scheduling | M — **done** |
| `compat_win.c` / `compat_win.h` | 253 / 88 | 44 | Windows-only by design: `pread`/`pwrite`/`fsync`/`realpath`/`mkdir` shims, `timeBeginPeriod`, high-resolution waitable timers, `BCryptGenRandom`, `GetProcessTimes`, `gow3_setjmp`/`gow3_longjmp` (asm), `compat_remove_tree` | Not compiled on Linux (the POSIX calls are native); `compat_remove_tree` has its Linux body in `host_linux_shared.cpp` (`std::filesystem::remove_all`), which the Linux tests link as the Windows tests link `compat_win.c` | none |
| `runtime_memory.c` | 1 192 | 32 | `VirtualAlloc2` placeholders for the PS4 range, `VirtualProtect`/`VirtualQuery`/`VirtualFree` | `mmap`/`mprotect` bodies exist (`#else` at 405–430); build and tests checked on Linux | S — **done** (no game run yet) |
| `runtime_thread.c` | 538 | 29 | TEB TLS slot for the guest TCB (`TlsAlloc`, `gs:[0x1480+8*slot]`), `__chkstk`/`StackLimit` widening, `SetThreadDescription`, affinity, priorities | `arch_prctl(ARCH_SET_GS)` exists; `pthread_setname_np`, `pthread_setaffinity_np`; build and tests checked on Linux | S — **done** (no game run yet) |
| `runtime_redzone.cpp` (was `.c`) | 727 | 29 | Rewrites guest leaf functions because Windows exception frames overwrite the red zone; the same pass prepares the old-CPU instructions | **done** (phase 6, C++): signal frames skip the 128-byte red zone, so on Linux only the old-CPU part runs (the functions from `.eh_frame_hdr`, LZCNT/TZCNT to their faulting form) when `runtime_isa` is on | S — **done** |
| `runtime_isa.cpp` (was `.c`) | 1 201 | 20 | Emulates MOVBE/BMI1/F16C/LZCNT/TZCNT on CPUs without them, on a `CONTEXT` | **done** (phase 6, C++): one emulation over a small register view, a `CONTEXT` or a `ucontext_t` (the YMM upper halves from the signal frame's XSAVE area); the stubs' helpers use `ms_abi` on Linux too, so the stub bytes are the same. `isa-test` runs on both (on Windows all three modes pass; on Linux the register view was checked on a real kernel, the whole test runs in CI) | M — **done** |
| `runtime_file.c` | 718 | 10 | `_stat64` + reparse-point fix (UCRT symlink stat), `GetDiskFreeSpaceExA`, `MoveFileExA`, `_chsize_s`, drive-root handling | `statvfs`, `rename`, `ftruncate` bodies exist; **done**: a name the game asks for in another case is looked up in the game folder (`host_linux_fold_case`, only when the path does not exist as written, §8.4) | M — **done** |
| `runtime_kernel.c` | 426 | 5 | `QueryPerformanceCounter`, `Sleep`, `__rdtsc` native TSC | `clock_gettime`/`nanosleep` exist; **done**: `GOW3_NATIVE_TSC` (which the launcher turns on for every game) measures the TSC against the monotonic clock on Linux (`host_linux_tsc_hz`), so the guest's inline `rdtsc` and the reported frequency agree on both systems | S — **done** |
| `runtime_services.c` | 925 | 4 | Winsock headers | BSD sockets exist | S — **done** |
| `runtime_savedata.c`, `runtime_mutex.c`, `runtime.c`, `runtime_audio.c`, `runtime_pad.c`, `runtime_sema.c`, `runtime_rtc.c`, `runtime_time.c` | | 1–2 each | `MoveFileExA`, `rand_s`, small `#ifdef` bodies | both bodies present; `runtime_savedata.c` removes folders with `compat_remove_tree` on both systems | S — **done** |

These C files are also on the "port to C++" list (AGENTS.md); doing the C++ port and the Linux check in the same pass
avoids touching each file twice.

### 4.4 `gow3_pc/src` C++ (`u2mp`, `u3fix`, `u3voice`, top level)

| File | Windows constructs | Shared path / platform function | Effort |
|---|---|---|---|
| `pokes.cpp` | unguarded `<windows.h>`, `CreateThread`, `ReadProcessMemory`/`WriteProcessMemory` on its own process (so unmapped addresses are skipped, not faulted) | **done**: `std::thread` on both; Linux reads and writes through `/proc/self/mem`, so an unmapped address is skipped there too (Windows keeps `Read/WriteProcessMemory`) | S — **done** |
| `name_dictionary.cpp` | `FindFirstFileW`, `_wfopen` (guarded) | both bodies present (builds and its test passes on Linux) | S — **done** |
| `u2mp/session.cpp:3054` | `_putenv_s` (unguarded) | **done** without touching the file: `src/linux_compat.h` (force-included into the runtime's C++ on Linux only) maps `_putenv_s` to `setenv`/`unsetenv` (an empty value removes the variable, as on Windows) | S — **done** |
| `u2mp/u2_online_abi.h` | 30 `__cdecl` function pointer types | **done** without touching the file: `linux_compat.h` defines `__cdecl` empty (x86-64 has one C convention). The online program has a Linux build since phase 5 (`u2_online`, built by the Linux package job) | S — **done** |
| `u2mp/crashlog.cpp` | VEH, `RtlVirtualUnwind`, `psapi` | `#else` stub exists; Linux crash log via `sigaction` + frame walk later (left to the `u2mp` maintainer; the runtime's own fault report covers Linux crashes meanwhile) | M (u2mp maintainer) |
| `u2mp/online_link.cpp` | `CreateProcessW` + job object `KILL_ON_JOB_CLOSE`, pipes, `CREATE_SUSPENDED` | `__linux__` branch exists; later reuse `port::spawn` | S (with phase 5) |
| `u2mp/net.cpp`, `u2mp/host_memory.h` | Winsock / `VirtualAlloc` | both bodies present (`u2mp-net-test` passes on Linux) | none |
| `u3fix/*` (5 files) | `VirtualQuery` readability checks, `VirtualAlloc` near image | both bodies present (the u3fix tests pass on Linux) | none |
| `u3voice/u3voice.h` | `__declspec(dllexport)` (the helper finds `uncharted_u3_voice` in the runtime's exports) | **done**: default visibility and `-rdynamic` on Linux (`nm -D gow3-probe` lists `uncharted_u3_voice`); the helper's lookup changes with its backend (§4.10, phase 5) | S — **done** |

`u2mp` has its own maintainer: coordinate its changes with them.

### 4.5 `gow3_pc/gpu`: our code vs vendored shadPS4

Our code:

| File | Windows constructs | Linux | Effort |
|---|---|---|---|
| `shim/gow3_profiler.cpp` | `SuspendThread`/`GetThreadContext` sampling, `RtlVirtualUnwind`, `psapi` | **done** (phase 6): sampling and reports shared; Linux samples with `SIGPROF` (the handler reads the signal frame and follows the saved frame pointers with checked reads) and names addresses with `dladdr`; the same files as on Windows | M — **done** |
| `shim/gow3_perf_stats.cpp` | `GetProcessTimes`, `K32GetProcessMemoryInfo`, `GlobalMemoryStatusEx`, NVML via `LoadLibraryExW("nvml.dll")` | `getrusage`, `/proc`, `dlopen("libnvidia-ml.so.1")` | S |
| `shim/gow3_terminate.cpp` | `RtlCaptureStackBackTrace`, `GetModuleHandleExA` | `backtrace()` / `dladdr` (branch exists) | S |
| `shim/gow3_threads.h`, `gow3_write_log.cpp`, `gow3_overlay.cpp`, `gow3gpu.cpp`, `window.cpp` | priorities, `EXCEPTION_POINTERS`, `_putenv_s` (guarded) | branches exist; `window.cpp` handles X11 and Wayland | none |
| `shim/gow3_win_compat.h` | force-included on Windows only (`gettid`) | not used on Linux | none |
| `dlss_bridge/` (`gow3_dlss.dll`) | MSVC + NVIDIA DLSS SDK, Windows only | optional; Linux NGX libraries exist in the DLSS SDK: later, behind the same `vk_dlss.cpp` interface | L (optional) |
| `tools/fsr4_bench.cpp` | `ReadFile` token only | portable | none |

Vendored shadPS4 (`gpu/shadps4`, commit in `VENDOR.txt`, local changes marked `gow3:`): 30 files have `_WIN32`
conditionals, 13 of them also Linux/Unix branches (upstream is Linux-first). Files where **our `gow3:` changes added
Windows-only code** that needs a Linux body:

| File | `gow3:` Windows additions | Linux |
|---|---|---|
| `core/libraries/videoout/driver.cpp` (12 `#if`) | waitable timers, `SetThreadPriority`, `CreateEventW`/`WaitForMultipleObjects`, `K32GetProcessMemoryInfo` | **checked**: Linux keeps upstream's `std::condition_variable` / `sleep_until` waits, which glibc times to the nanosecond (the Windows timers work around libc++ rounding to whole milliseconds there); the vblank thread priority and the memory numbers in the 5-second log are Windows-only (log shows 0 on Linux) |
| `video_core/amdgpu/liverpool.cpp` (6) | thread priority/affinity | upstream `pthread_*` branches |
| `video_core/renderer_vulkan/vk_pipeline_serialization.cpp` (3) | `MoveFileExW` atomic replace, the warm-up crash mark (`CreateFileW`) | **done**: `std::filesystem::rename`; the mark is written with `open`/`write` from the Linux fault report (`host_linux.cpp`), so a driver crash during the warm-up clears the cache at the next start on Linux too |
| `video_core/renderer_vulkan/vk_scheduler.cpp`, `buffer_cache/buffer_cache.cpp` | `RtlCaptureStackBackTrace` diagnostics (`GOW3_SUBMIT_STATS`) | Windows-only diagnostics; `backtrace()` later (phase 6) |
| `video_core/renderer_vulkan/vk_dlss.cpp` | `LoadLibraryW(L"gow3_dlss.dll")`, `nvngx_dlss.dll` | stub on Linux: the settings menu says DLSS is "not on Linux yet" (`vk_temporal_upscaler.cpp`); a Linux bridge later (optional) |
| `video_core/page_manager.cpp` | `EXCEPTION_POINTERS` fault path | upstream Linux path (signals) |
| `common/*` (`thread`, `io_file`, `ntapi`, `signal_context`, `rdtsc`, `path_util`, `string_util`, `srw_shared_mutex.h`) | upstream Windows branches | upstream Linux branches; **`path_util.cpp`**: on Linux too the GPU user folder is the port's portable folder (`GOW3_GPU_USER_DIR`), created at start, never shadPS4's own `~/.local/share/shadPS4` (before, the runtime aborted before `main` when that folder's parent was missing) |

CMake builds `gow3gpu` `STATIC` on both systems and links it into `gow3-probe` (Windows: `ws2_32 bcrypt ntdll
synchronization winmm pdh`; Linux: X11, `z`, `m`, `dl`, pthreads).

### 4.6 `gow3_pc/tests`, `gow3_pc/tools`, `gow3_pc/packaging`

| Item | Windows constructs | Shared path | Effort |
|---|---|---|---|
| `tests/u2mp_rewrite/*` (old-vs-new harness, 10 files) | `_spawnl`, `_putenv_s`, `_popen`, `GetModuleFileNameA`, `_aligned_malloc`, `__declspec(dllimport)`, `_get_osfhandle`; `run.sh` builds `test_*.exe`; `flags_progression.sh` uses `cygpath` if present | `posix_spawn`/`setenv`/`readlink("/proc/self/exe")` behind a tiny test helper; `.exe` suffix by OS | M |
| `tests/test_isa.c`, `test_tsc_clock.c`, `test_cheat_hook(s).cpp`, `test_actor_hook.cpp`, `test_orb_multiplier.cpp`, `test_stat_multipliers.cpp`, `test_debug_menus.cpp`, `test_u3_job_trace.cpp` | `VirtualAlloc`/`VirtualProtect`/`FlushInstructionCache`, VEH, affinity | **done**: the hook tests share `tests/test_memory.h` (`mmap`/`mprotect` on Linux); `test_tsc_clock.c`'s core-hopping part stays Windows-only; `test_isa` stays Windows-only until §4.3 `runtime_isa` has a Linux body | S–M — **done** |
| `tests/tool_checks.cpp` | `CreateProcessA`, `CreateFileW` | `posix_spawn` branch | S |
| `tests/test_environment.h`, `test_pad.c`, others | `_putenv_s` | both bodies present (`setenv`/`unsetenv`) | none |
| CMake test targets | ~36 tests inside `if (WIN32)`; 31 GPU/unit tests outside (already built on any OS) | **done**: every unit test except `isa-test` is defined for both systems; `tests/run_unit_tests.sh` builds and runs the 63 that need no game (the Linux CI job, with lavapipe; on Windows run them from PowerShell, MSYS2 bash gives the temp-writing tests a POSIX TMP). Linux tests link `host_linux_shared.cpp` where Windows tests link `compat_win.c` | M — **done** |
| `tools/got_import.cpp`, `tools/fsr4_rewrite.cpp` | `GetModuleFileNameW`, `_setmode` (guarded) | both bodies | S |
| `tools/bridge_helper/bridge_helper.c` | Proton/Wine test helper (`wine_server_fd_to_handle`) | Windows-only by design (runs under Wine) | none |
| `tools/{ab,press,restart,soak}.sh` | POSIX (`pkill`, `pgrep`), from the Linux era; `restart.sh` calls a `run.sh` that no longer exists | refresh when the Linux runtime runs | S |
| `packaging/windows/play.c` (`gow3-play.exe`), `*.rc`, `gow3-probe.manifest`, `UCMR.ico` | `wWinMain`, `MessageBoxW`, manifest selects the **UTF-8 code page** for the runtime's narrow paths | Windows-only by design; Linux needs no manifest (UTF-8 native) | none |

### 4.7 `u3mp/patcher` (importer `u3import.exe`, steps `u3fix.exe`, recipe)

| File | Windows constructs | Shared path / platform function | Effort |
|---|---|---|---|
| `tools/fixes/dctools/proc.cpp` | Process runner for steps/ffmpeg/objdump: `CreatePipe` (8), `CreateProcessW` (5), `SetHandleInformation`, `WaitForSingleObject`, env block, `quote_arg` (Python `list2cmdline` emulation) | `port::spawn(argv vector)`; quoting only in the Windows body | M — **done**: `run`, `run_pipe`, `run_to_file`, `run_to_lines` and the worker pass the argv vector to `port::spawn` (stderr apart where the step reads it: `Spawn::err_pipe`) |
| `importer/common.cpp` | Same runner + `GetModuleFileNameW`, BCrypt, env merge "case-insensitive like Windows" (line 419) | `port::spawn`, `port::executable_path`, shared SHA-256; env names case-sensitive on Linux | M — **done** |
| `importer/u3import.cpp` | `wmain`, `SetConsoleOutputCP`, directory **junctions** via `DeviceIoControl(FSCTL_SET_REPARSE_POINT)`, `CreateHardLinkW`, `MoveFileExW`, `_wfopen`, ffmpeg fallback `C:\msys64\...\ffmpeg.exe` | `main` (UTF-8 argv on Linux), `port::dir_link` (junction / `symlink`), `port::hard_link` (`CreateHardLinkW` / `link`), `port::replace_file` | M — **done** (`main` on Linux, `wmain` kept; PATH joined with `:`; the MSYS2 ffmpeg fallback is Windows-only) |
| `importer/ps3keep.cpp` | `CreateHardLinkW`, `GetFileAttributesExW`, `lower()` path compare with `"\\"`, `\r\n` | `port::hard_link`; case rule §8.4 | S–M — **done** |
| `importer/recipe.cpp` | `_wstat64`, `CopyFileW`, fnmatch emulated "as on Windows" (case-insensitive, `/` → `\`), runs `u3fix.exe`/`u3import.exe` by name (line 1280) | `std::filesystem`; keep the matcher identical on both OS; `.exe` mapping (§10) | S — **done**: on Linux a step's program goes through `port::program_name` at run time; the JSON and the fingerprints are unchanged (`--recipe-fingerprint` gives the same value on both systems) |
| `importer/mpimage.cpp` | includes `<windows.h>` only | drop the include | S — **done** |
| `tools/fixes/dctools/fixlib.cpp` | `game_running()` via toolhelp; `win_path()` (`/` → `\`) | `port::list_processes`; see §8.2 for `win_path` | S — **done** (`win_path` leaves Linux paths as they are) |
| `tools/fixes/dctools/u3fix.cpp` | `wmain`, `tlhelp32.h` | `main` | S — **done** |
| `tools/fixes/pak23-levels/levels.cpp`, `actor33/build_actors.cpp` | `_wstat64` | `std::filesystem::file_size`/`status` | S — **done** (`stat`) |
| `airport-igc3`, `registry/run_mkregistry`, `pak23-registry/apply_registry` | build some paths with `"\\"` | file access works through the `/` folding of `upath()`/`P()`; text handed to a child or written to a file may still hold `\` | S — not checked (needs game files) |
| `tools/fixes/sound-banks/convert_banks.cpp` | `<windows.h>` include only | drop the include | S — **done** |
| `recipe/recipe_tools.cpp` | `CommandLineToArgvW`, `GetFileInformationByHandle` | `main(argc, argv)` + `port::file_id` | S |

### 4.8 `u2mp/patcher/importer` (`u2import.exe`)

| File | Windows constructs | Shared path | Effort |
|---|---|---|---|
| `u2import.cpp` | `wmain`, `SetConsoleOutputCP`, BCrypt SHA-256, `MoveFileExW`, `_wfopen`, `GetLocalTime` | `main`, shared SHA-256, `port::replace_file`, `<fstream>`, `<chrono>` | S — **done**: builds, links and runs on Linux (`tools/build_importers_linux.sh`, checked by `tools/importer_smoke.sh`) |
| the other 20 files | none (std C++) | | none |

### 4.9 `tools/`

| File | Windows constructs | Shared path | Effort |
|---|---|---|---|
| `windows_starter/starter.cpp` (`UCMR_Launcher.exe`) | `wWinMain`, `CreateProcessW`, `MessageBoxW`, `-mwindows` | Windows-only by design; Linux: the AppImage `AppRun` / a 3-line shell `UCMR_Launcher` starts `launcher-qt/UCMRLauncher` | S |
| `mkzip.cpp` | `wmain`, `GetFileAttributesExW`, `FileTimeToLocalFileTime` | `main`, `std::filesystem::last_write_time`; Linux packages use `tar` (or keep mkzip for a zip) | S — **done** |
| `code_ids.cpp` | `wmain`, BCrypt SHA-1 | `main`, a small portable SHA-1 (or SHA-256 if the code-id format allows; it must stay identical) | S — **done** (`sha256.h` on Linux) |
| `winres/` (`winres.sh`, `ucmr.cmake`, `ucmr.rc.in`, `ucmr.manifest`) | windres + `cygpath` | Windows-only by design | none |
| `package_windows.sh`, `build_ffmpeg.sh`, `fetch_aa_files.sh`, `README-Windows.txt` | see §7 | add `package_linux.sh`, a Linux branch in `build_ffmpeg.sh`, `README-Linux.txt` | M |

### 4.10 U3 helper (guest code)

The helper is freestanding C/C++ built for the PS4 (x86-64 ELF, `--target=x86_64-unknown-elf`) and loaded into the
game; it is not a Windows program. Two parts depend on the **host** OS:

| File | What | Linux | Effort |
|---|---|---|---|
| `helper-source/plat_pc.inc` (277 lines) | PC backend of `plat.h`: PEB walk + PE export lookup of `ws2_32.dll`, `kernel32.dll`, `kernelbase.dll`, `user32.dll`, `gdi32.dll`, `advapi32.dll`, `ucrtbase.dll`; `ms_abi` thunks (`WSF`); Winsock sockets, `VirtualAlloc`, `CreateThread`, `Sleep`, `GetSystemTime`, `RtlGenRandom`, a Win32 one-line text box, `Get/SetEnvironmentVariableA` hand-off (`U3MP_HANDOFF`, `U3MP_RELAY`) | Either a third backend (`plat_linux.inc`) that finds libc through the runtime, or **one PC backend for both systems**: the runtime exports a small table of host functions in the SysV ABI (sockets, alloc, thread, sleep, time, random, env, text box via SDL3), found the way `uncharted_u3_voice` is found today. The second keeps one backend and removes the PEB walk; it is a normal helper version. **Done** that way (v7.267, below) | L — **done** |
| `helper-source/plat.h` | `PLAT_THREAD_ATTR __attribute__((ms_abi,force_align_arg_pointer))` for threads on PC | **done** (v7.267): System V threads started by the host table; `PLAT_BIND` reads the table at kind 44 | S — **done** |
| `helper-source/u3_bearssl.inc` | BearSSL's own `_WIN32` autodetection (third-party) | unchanged | none |
| `helper/u3_pool_init.c` | `movq %gs:0` (guest TCB through the runtime's GS setup) | same on Linux (GS base = guest TCB there) | none |

The runtime makes `sceNetSocket` answer `ENETUNREACH` on PC so the helper uses host sockets (`plat_net_pick`); on
Linux the same switch applies with the new backend.

**Done in code (phase 5, helper v7.267, not shipped yet):** the second way. The runtime hands the helper one table of
host functions (`gow3_pc/src/uncharted_host.h`: System V calls, Winsock error numbers and option values on both
systems; version 1, 208 bytes) through its own `sceKernelOpen` of `/dev/uncharted_host`: the helper reads 8 bytes, the
table's address, at kind 44 (`PLAT_BIND`, `u3x_host_bind` in `u3x_fixes.cpp`). `plat_pc.inc` calls only the table: no
PEB walk, no `ms_abi` thunks, the Win32 text box moved into the runtime (zenity or kdialog on Linux). An older runtime
has no such file: the helper then finds no host functions (no LAN or online play, as on Linux before). The image is
built by the maintainer's helper build; Windows behaviour must be checked with a LAN and an online test before it
ships.

## 5. Calling convention, TLS, stack, exceptions

The runtime runs PS4 guest code (x86-64 **System V** ABI) inside the host process. On Windows the host is **Microsoft
x64**, so every crossing is marked; on Linux the host already is System V, so most of this work **disappears**, while
the Windows code paths stay as they are.

| Topic | Windows today (files) | Linux | Effort |
|---|---|---|---|
| Functions the guest calls (HLE exports) | `ABI` = `__attribute__((sysv_abi))` (`runtime.h`, 415 uses, 28 files), `U2MP_GUEST` (`u2mp_internal.h`, 658 uses, 54 files), `U3VOICE_ABI`, `sysv_abi` in `u3fix` (41), `gow3_fiber.h`, `gow3_orbs.cpp` | the attribute is the default: keep it (no-op), one source | none |
| Pointers to guest functions | `GuestCallback`, `CounterUpdate`, `SplitPadUpdate` typed `sysv_abi` | same | none |
| ms_abi in guest code | helper `plat_pc.inc` `WSF`, `PLAT_THREAD_ATTR` | **done**: gone with the host table (§4.10, v7.267) | L — **done** |
| `__cdecl`, `WINAPI`, `CALLBACK` | `u2_online_abi.h` (30), `gamepads.cpp` (10), `system.cpp`, `config.cpp`, Windows-only files | empty macro or removed in shared headers | S |
| Guest TLS (TCB) | Guest code reads its TCB with `mov rax, fs:[0]`. `launcher/native/link.cpp` rewrites these to `gs:` (shared step). On Windows GS is the TEB and cannot move: `probe.c patch_tcb_loads` changes the displacement to a TEB TLS slot `gs:[0x1480+8*slot]`, the slot must be below 64 and taken before drivers load (`runtime_thread.c`, `probe.c:1252`) | GS is free under glibc: `arch_prctl(ARCH_SET_GS, tcb)` per guest thread (already in `runtime_thread.c:127`), no displacement patch | none (verify) |
| Guest main stack | Runs on the process stack: `--stack 16777216,16777216` linker flag, committed; `__chkstk` probes vs TEB `StackLimit` (`runtime_thread_widen_stack_limit`); TEB stack bounds patched so `RtlGuardRestoreContext` accepts resumed contexts (`probe.c:361`) | `pthread_attr_setstacksize` / `enter_on_stack` (exists, `probe.c:126`); no TEB | none (verify) |
| Red zone | Windows builds exception frames below `rsp` without the 128-byte red zone: `runtime_redzone.cpp` rewrites guest leaf functions | kernel signal frames skip the red zone: only the old-CPU part of the pass runs | none |
| Faults | VEH on `CONTEXT` (`probe.c`), `SetUnhandledExceptionFilter`, `crashlog.cpp`, `page_manager.cpp` | `sigaction(SA_SIGINFO|SA_ONSTACK)` + `sigaltstack` on `ucontext_t` (exists in `probe.c fault()`) | none (verify) |
| Fault recovery | `gow3_setjmp`/`gow3_longjmp` (asm in `compat_win.c`: no SEH unwinding through guest frames, saves xmm6–15) | `sigsetjmp`/`siglongjmp` (`runtime.h` already maps) | none |
| Old-CPU instruction emulation | `runtime_isa.cpp` on `CONTEXT` | **done**: the same code on a `ucontext_t` (register view, XSAVE area) | M — **done** |
| Unwinding for diagnostics | `RtlLookupFunctionEntry`/`RtlVirtualUnwind` (`probe.c`, `crashlog.cpp`, `gow3_profiler.cpp`), `RtlCaptureStackBackTrace` | frame-pointer walk / `_Unwind_Backtrace` / `backtrace()`; guest frames have no unwind data on either OS | M (later) |
| PS4 address range 64 GiB–1 TiB | `-Wl,--disable-high-entropy-va`, `VirtualAlloc2` placeholders reserved early (`probe.c:1251`) | `mmap(MAP_FIXED_NOREPLACE)` reservation early; `-no-pie` already in `build.sh`; glibc maps near `0x7f...`, so the range is normally free | S (verify) |
| Hooks near the image | `VirtualAlloc` search within ±2 GiB (`u3fix/hook.cpp`, `probe.c:842`) | `mmap` with hint / `MAP_32BIT` (exists) | none |
| Inline assembly | `compat_win.c` (setjmp), `probe.c`, `runtime_thread.c` | POSIX `enter_on_stack` exists | none |

## 6. Libraries and system DLLs

| Windows library / DLL | Used by | Linux equivalent | Notes |
|---|---|---|---|
| `ws2_32` | runtime (`u2mp/net.cpp`, `runtime_services.c`), `gow3gpu`, `u2mp-net-test`, online programs | libc BSD sockets | both bodies exist in the runtime |
| `winmm` (`timeBeginPeriod`) | `compat_win.c`, `gow3gpu`, several tests | none (`clock_nanosleep` is precise) | |
| `bcrypt` (SHA-256/SHA-1/random) | launcher (`updater`, `starter`, `self_reader.h`, `prepare`), `u3import`, `u2import`, `code_ids`, runtime `compat_win.c`, CI's private-files `apply.cpp` | **one shared SHA-256** + `getrandom()` | 4 portable SHA-256 copies already exist (`session_value.cpp`, `updater.cpp`, `importer/importer.hpp`, `recipe/recipe_tools.cpp`): keep one |
| `dbghelp`, `psapi` | `hang.cpp`, `memstat.cpp`, `crashlog.cpp`, `gow3_perf_stats.cpp`, `gow3_profiler.cpp` | `/proc`, `ptrace` | |
| `shell32` | `ShellExecuteW`, `SHGetKnownFolderPath`, `CommandLineToArgvW` | `xdg-open`, XDG dirs, `argv` | |
| `ole32`, `uuid`, `windowscodecs` | `.lnk` shortcuts, WIC icon decode/PNG encode | `.desktop` files; PNG via zlib (already linked) or Qt | |
| `advapi32` | registry (`steam.cpp`), helper `RtlGenRandom` | files / `getrandom` | |
| `user32`, `gdi32` | `apply.cpp` progress window, `MessageBoxW` (starter, `play.c`), helper text box | Qt in the launcher; SDL3 in the game | |
| `dxgi` | `display.cpp` | SDL3 + Vulkan | |
| `winhttp` | `updater.cpp` | libcurl (or Qt Network in the window) | libcurl is in every distro; bundle in the AppImage |
| `onecore` (`VirtualAlloc2`) | `gow3-probe`, tests | `mmap` | |
| `ntdll`, `synchronization`, `pdh` | `gow3gpu` (upstream common code) | pthreads/futex | upstream handles it |
| `crypt32` (DPAPI) | online program token store | Secret Service (libsecret) or a `0600` file | owner decision (§14) |
| `vulkan-1.dll` (system, never shipped) | runtime, `gow3-gpu-capabilities` | `libvulkan.so.1` (system, never bundled) | |
| `SDL3.dll` | runtime, launcher pads (loaded at run time) | `libSDL3.so.0` bundled | |
| FFmpeg 8.1.1 small build (`build_ffmpeg.sh`: `--target-os=mingw32`, `--enable-w32threads`, shared DLLs, `ffmpeg.exe`) | runtime (avcodec/avformat/swscale), import (`ffmpeg.exe` for EULA pages / MP3) | same configure with `--target-os=linux`, pthreads; `.so` + `ffmpeg` binary in the package | `build_ffmpeg.sh` needs a Linux branch |
| GNU `objdump.exe` (UCRT64 binutils) | import, Havok tables | distro `objdump` or bundled binutils | |
| Qt 6 (`windeployqt6`, QML plugin DLL fix-up, `qwindows`, `qico`) | `launcher-qt` | `linuxdeploy` + `linuxdeploy-plugin-qt`; platform plugins `xcb` and `wayland` | |
| Opus (`libopus.a` static) | runtime voice | `libopus.a` or `.so` | |
| zlib (`-lz` static) | `mkzip`, FFmpeg | system zlib | |
| libc++ (CLANG64, static) | every C++ program | libstdc++ or libc++; static libstdc++ or bundled in the AppImage | |
| NVIDIA DLSS (`gow3_dlss.dll`, MSVC) + `nvngx_dlss.dll` | optional DLAA | DLSS SDK Linux libraries, later | |
| NVML (`nvml.dll`, run time) | perf stats | `libnvidia-ml.so.1` | |

Static vs dynamic: Windows builds link statically (`-static`) and copy only the DLLs `ldd` finds outside
`C:/Windows` (`copy_dlls`, with fixed name rules after 0.10.117). On Linux, glibc cannot be linked statically in a
sensible way: build on an old distro (glibc baseline, e.g. Ubuntu 22.04 = glibc 2.35, which RecompLauncher's
`system.linux.glibc` can state), bundle every non-system `.so` (SDL3, FFmpeg, Qt, libcurl, libstdc++ if needed),
never bundle `libvulkan`, `libGL`, `libc`, X11/Wayland client libraries.

## 7. Build system, toolchain, CI, packaging, releases

### 7.1 MSYS2 CLANG64 assumptions

| Assumption | Where | Both-OS form |
|---|---|---|
| `/c/msys64` default, `MSYS2_ROOT`, `$msys/clang64/bin` on `PATH` | `gow3_pc/msys2-env.sh`, `tools/package_windows.sh`, `tools/build_ffmpeg.sh`, `launcher/native/build.sh` (`CLANG64`), CI | `case $(uname -s)` in each script (as `gow3_pc/build.sh` does) |
| `C:/msys64` in C++ dev fallbacks | `gamepads.cpp:72`, `runtime_env.cpp:422`, `u3import.cpp:576` (ffmpeg) | inside `#ifdef _WIN32` |
| `cygpath` | `package_windows.sh`, `build_ffmpeg.sh`, `winres.sh`, `fetch_aa_files.sh`, CI, `tests/u2mp_rewrite/flags_progression.sh` (optional already) | Windows branch only |
| `-municode` + `wmain` | `u3import`, `u2import`, `u3fix`, `code_ids`, `mkzip`, `game_starter`, the launcher DLL flags | `main(int, char**)` on Linux (UTF-8 argv), `wmain` kept on Windows (one `#ifdef` around the entry point, or a tiny `port::args()` helper) |
| `-mwindows` / `wWinMain` | `apply_update.exe`, `UCMR_Launcher.exe`, `gow3-play.exe` | Windows-only programs |
| `.exe` suffixes in build outputs and `need` checks | `package_windows.sh`, `gow3_pc/build.sh:34`, `tests/u2mp_rewrite/run.sh` | `exe=$([[ $OS == Windows_NT ]] && echo .exe)` |
| windres resources, manifests, `.ico` | `tools/winres/*`, `gow3_pc/packaging/windows/*.rc`, `gow3-probe.manifest` (UTF-8 code page, long paths), `launcher-qt` | `.desktop` + PNG/SVG icon; AppStream metadata optional |
| `-Wl,--no-insert-timestamp`, `--disable-high-entropy-va`, `--stack` | link lines | Linux: `-Wl,--build-id`, `-no-pie` for the runtime, stack per thread |
| MSVC (DLSS bridge, `vswhere`) | `tools/fetch_aa_files.sh` | Windows only |

### 7.2 `gow3_pc/build.sh` and `gpu/CMakeLists.txt`

- Windows: `build.sh` runs CMake, which builds `gow3gpu` (static) and the full `gow3-probe.exe` inside `if (WIN32)`.
- Linux: `build.sh` runs CMake only for `gow3gpu.so`, then `cc -std=c11 -Werror ... src/probe.c src/runtime*.c` and
  the C tests: **no `u2mp`/`u3fix`/`u3voice`/C++ files, no Opus**, so the current Linux probe lacks every multiplayer
  feature.
- Target: one `gow3-probe` CMake target for both systems (Windows extras: `compat_win.c`, winres, `onecore`,
  linker flags; Linux extras: `-ldl -rdynamic -no-pie`), `build.sh` uses CMake on both, tests out of `if (WIN32)`.
  Decide whether `gow3gpu` stays shared on Linux (current) or becomes static like Windows (one link model, simpler
  packaging): static is simpler and matches Windows.

### 7.3 CI today (`.github/workflows/build-windows.yml`)

`windows-latest`, `msys2/setup-msys2@v2` (CLANG64, pacboy packages incl. `qt6-*`, `sdl3`, `opus`, `vulkan-*`,
`zydis`, `python`, UCRT64 binutils), default shell `msys2 {0}`, two `pwsh` steps (git config; the release step with
`gh release create`), ccache, the small FFmpeg cache, the private package files step (builds `apply.cpp` with
`-municode -static -lbcrypt`), `tools/package_windows.sh`, upload of `UCMR_windows.zip`. The release step runs inside
the Windows job.

### 7.4 A Linux job next to it

**State (phase 0):** `.github/workflows/build-linux.yml`, its own workflow (push to `main`, every pull request), so it
never holds up the Windows build or its release. Jobs: **Launcher core** (ubuntu-22.04, clang 15:
`launcher/native/build.sh` builds `libuncharted_native.so` and `game_starter`, `core_smoke` calls the library, both
files are kept as an artifact), **Importer core and build tools** (ubuntu-24.04, clang 18: the Uncharted 2 importer's
files and the converters it shares compile, `mkzip` and `code_ids` build and `mkzip` makes a zip `unzip` reads back),
**GPU unit tests** (ubuntu-24.04 for C++23, SDL3 built from source and cached, lavapipe as a software Vulkan device:
21 `gow3_pc/gpu` test targets outside `if (WIN32)` that need no `gow3gpu`, built and run). Each failing file, package
or test is named in the run's annotations. Still to come: `gow3gpu.so`, the runtime, the window, the package and the
release job.

**State (phase 2):** the launcher core job also installs `libcurl4-openssl-dev` and runs `port_smoke` (the process
layer: start, pipes, folder, environment, time-outs, process list, `kill_tree`). The importers job builds `u2import`,
`u3import` and `u3fix` with `tools/build_importers_linux.sh` (the source lists of `tools/package_windows.sh`, with
u3fix's code ids) and runs `tools/importer_smoke.sh`: the checks that need no game files (usage and exit codes, the
code ids, `--shipped-id`, `--recipe-fingerprint`, `--recipe --list`). The Windows workflow runs the same script on the
package (`OUT=dist/UCMR_windows`). The repository has no importer fixture tests with small inputs yet; a test that
runs one recipe step on a tiny made-up input would exercise the step runner on both systems.

Toolchain note: clang 15 with libstdc++ 12 (the newest in Ubuntu 22.04) rejects some of the importers' C++20
(lambdas that capture structured bindings), so the importers' job uses Ubuntu 24.04. A Linux package built for the
glibc 2.35 baseline (phase 3) needs a newer clang on 22.04 (apt.llvm.org) or an older-glibc container. `json.h`'s
`Value` defines its destructor after the class so clang with libstdc++ accepts it in C++20.

**State (phase 3):** the **Linux package** job (ubuntu-22.04, the glibc 2.35 baseline) installs clang 18 from
apt.llvm.org and Qt 6.8.3 from download.qt.io through aqtinstall (CI glue only; 22.04's own Qt 6.2 is older than the
6.5 the window needs; Qt and the small FFmpeg are cached), runs `tools/build_ffmpeg.sh` (its Linux branch) and
`tools/package_linux.sh`, and uploads `UCMR_linux.tar.gz` as an artifact. Its check unpacks the tarball, moves the Qt
it was built with out of the way, checks that every library of `launcher-qt/` resolves, starts
`UCMR_Launcher --self-check` with `QT_QPA_PLATFORM=offscreen` (the QML and the core load, the core gets ready, the
launcher closes the normal way) and runs `tools/importer_smoke.sh` on the unpacked folder. The package prints the
newest `GLIBC_`/`GLIBCXX_` version its programs ask for. It is a test package (no private files, so no helper) and
not part of a release (phase 7). `build_importers_linux.sh` now links libstdc++ statically, like the launcher core.

The plan:

- `runs-on: ubuntu-22.04` (glibc 2.35 baseline; or a container of the oldest supported distro).
- Packages: clang/LLVM, cmake, ninja, ccache, pkg-config, Vulkan headers + loader, glslang, SPIRV-Tools/Headers/Cross,
  SDL3 (build from source if the distro lacks it), fmt, boost, robin-map, xxhash, zydis, opus, Qt 6 (base +
  declarative), libcurl, zlib, X11/Wayland dev headers, binutils.
- Steps: runtime (`gow3_pc/build.sh`), small FFmpeg (Linux branch), launcher core (`launcher/native/build.sh` →
  `libuncharted_native.so`), importers (`u3import`, `u3fix`, `u2import`), private package files (the same `apply.cpp`,
  compiled with `main` on Linux), Qt window (CMake), `tools/package_linux.sh`, tests (`ctest` / the same test
  targets as Windows), upload `UCMR_linux.tar.gz` and `UCMR-x86_64.AppImage`.
- Both jobs run on every push/PR; the release moves to a third job (`needs: [windows, linux]`) that downloads both
  artifacts and runs `gh release create` once with both assets (bash, no `pwsh`).

### 7.5 Packaging

| Windows (`package_windows.sh`) | Linux (`package_linux.sh`, new) |
|---|---|
| `UCMR_Launcher.exe` (starter) | `UCMR_Launcher` (shell or tiny C++ starter) / AppImage `AppRun` |
| `launcher-qt\` + `windeployqt6` + QML DLL fix-up | `launcher-qt/` + `linuxdeploy-plugin-qt` (xcb + wayland) |
| `launcher\native\uncharted_native.dll`, `game_starter.exe`, `tools\apply_update.exe` | `libuncharted_native.so`, `game_starter`, `tools/apply_update` (tarball only) |
| `gow3_pc\out\gow3-probe.exe` + `copy_dlls` | `gow3_pc/out/gow3-probe` + bundled `.so` (`$ORIGIN` rpath) |
| `u3mp\bin\` `u3import.exe`, `u3fix.exe`, `ffmpeg.exe` + DLLs, `binutils\objdump.exe` | `u3mp/bin/` `u3import`, `u3fix`, `ffmpeg`, `objdump` |
| `u2mp\bin\u2import.exe` | `u2mp/bin/u2import` |
| `licenses\` incl. MSYS2 package licenses (`pacman -Qqo`) | `licenses/` incl. distro package licenses (`dpkg -S` / copyright files) |
| `README.txt` (`README-Windows.txt`), `recomplauncher.json` | `README.txt` (`README-Linux.txt`), `recomplauncher.json` |
| `mkzip.exe` → `UCMR_windows.zip` | `tar` → `UCMR_linux.tar.gz`; `appimagetool` → AppImage |
| no-game-files check | same check |

**State (phase 3):** `tools/package_linux.sh` makes `dist/UCMR_linux.tar.gz` (top folder `UCMR_linux/`, as the zip's
`UCMR_windows/`):

- `UCMR_Launcher`: a short start script (`tools/linux_starter/UCMR_Launcher.sh`) that changes to its folder and
  `exec`s `launcher-qt/UCMRLauncher` with the same arguments (`--play` included). It sets **no** environment
  variable: the window finds its Qt libraries through its run path (`$ORIGIN/lib`, Qt's own libraries and plugins
  carry `$ORIGIN`-relative run paths) and its plugins and QML through `launcher-qt/qt.conf`, so a file manager or
  browser the launcher opens never inherits the package's Qt.
- `launcher-qt/`: the program, `lib/` (Qt's libraries and its ICU, found with `ldd`, plus the xcb helpers desktops
  often lack: `xcb-cursor`, `xcb-icccm`, `xcb-image`, `xcb-keysyms`, `xcb-render-util`, `xcb-util`, `xcb-xinerama`,
  `xcb-xkb`, `xkbcommon-x11`), `plugins/` (xcb, Wayland with its shell/decoration/graphics parts, offscreen, the xcb
  GL integrations, compose-key input, the `.ico` reader, the xdg desktop portal theme for file dialogs), `qml/`
  (QtCore, QtQml, QtQuick pruned to the Basic style as on Windows). A copy of a library that is not installed stops
  the package. No custom tool (no linuxdeploy), so nothing is downloaded but Qt itself.
- `UCMR_Launcher.desktop` (`launcher-qt/linux/`), `launcher/assets/` (icon.png), `launcher/native/`
  (`libuncharted_native.so`, `game_starter`), `u2mp/bin/u2import`, `u3mp/bin/` (`u3import`, `u3fix`, `ffmpeg`),
  `u3mp/patcher/` (the same newest-helper check as Windows), `tools/game_files_102.json.gz`, `README.txt`
  (`tools/README-Linux.txt`: **the games do not run on Linux yet**), `LICENSE.txt`, `VERSION`, `recomplauncher.json`,
  `analysis/uncharted-user/disabled-optimizations.txt`.
- `licenses/`: Qt (LGPL 3 texts from the system and a source note), FFmpeg, BearSSL, and `distro/` with the
  `copyright` file of each distribution package a bundled system library came from (`dpkg -S`).
- FFmpeg: `tools/build_ffmpeg.sh` has a Linux branch that builds the same FFmpeg 8.1.1 parts as one `ffmpeg` with the
  libraries linked in (static; needs only the C library and zlib), into `downloads/ffmpeg-8.1.1-min-linux`.
- Missing on purpose: the game runtime (`gow3_pc`, phase 4), `tools/apply_update` (phase 7), GNU objdump (the system's
  `binutils`; the README says so: without it the U3 import skips the Havok tables, as on Windows without it), SDL3
  (the window's controller navigation uses the system's `libSDL3.so.0` if installed; the runtime brings its own in
  phase 4).

### 7.6 Install and update model on Linux

The launcher writes into its own install folder (`analysis/` with `uncharted-user/savedata`, `u3mp/`, `u2mp/`,
`game/`, `launcher.json`, import work folders). An AppImage mounts read-only, so:

- **Tarball (folder) package**: same layout and same updater flow as Windows (download, verify SHA-256, replace
  files, restart). On Linux a running binary can be replaced by `rename`, so `apply_update` can be a mode of the
  launcher itself instead of a separate program.
- **AppImage**: needs a writable data root outside the image (e.g. a folder next to the `.AppImage` taken from
  `$APPIMAGE`, or `$XDG_DATA_HOME/UCMR`), and updates the whole image (download the new AppImage, verify, swap).
  RecompLauncher unpacks AppImages into a plain folder (its schema says so), where the folder layout is writable.
- Simplest first step: ship the tarball; add the AppImage when the data root can be moved.

### 7.7 One release with both packages

- **Yes, one release can carry both.** Windows launchers look for the exact asset name `UCMR_windows.zip`
  (`updater.cpp:40`, plus `Uncharted-windows.zip` for old releases), so an extra Linux asset does not disturb them.
  GitHub publishes a SHA-256 `digest` per asset, which the updater already requires.
- The updater picks its asset by OS: `UCMR_windows.zip` on Windows (unchanged), `UCMR_linux.tar.gz` (or the
  AppImage) on Linux, chosen at compile time (`#ifdef _WIN32`). A release without the Linux asset is simply "no
  update" for Linux launchers, like the existing `continue` for releases without a digest.
- Tag rule stays: the tag equals `tools/VERSION`; `tools/release.sh` unchanged.
- **RecompLauncher** (`recomplauncher.json`, schema at recomplauncher.com): `start.program` is a list "tried in
  order", so `["UCMR_Launcher.exe", "UCMR_Launcher"]` (or an `.AppImage` entry, which the schema resolves under any
  installed name) serves both systems from one file. `system` accepts `windows` (`version`, `graphics`) and `linux`
  (`glibc`, `graphics`): add `"linux": {"glibc": "2.35", "graphics": {"vulkan": "1.3"}}`. The schema does not show
  how RecompLauncher picks a release asset per OS: check its developer documentation before naming the Linux asset.
  `docs/RECOMPLAUNCHER.md` and the `{install}` paths need no change.
- RecompLauncher's developer guide (recomplauncher.com/sdk-files/guide.md, read 2026-11-27): the launcher picks the release file for the player's PC by the system and processor words in its name (its examples: `...-Windows-x64.zip`, `...-Linux-x64.AppImage`), not by a field; on Linux it takes a tarball (`.tar.gz`, `.tar.xz`, `.tar.zst`) or unpacks an AppImage. `UCMR_linux.tar.gz` names the system as `UCMR_windows.zip` does; whether it also wants the processor word is not stated. `system.linux.glibc` is the oldest glibc the build runs with (a warning only, never a stop).

## 8. Paths and text

### 8.1 Wide strings

`launcher/native` has 1 921 wide-string hits in 30 files (`std::wstring` paths, `L"..."`, `wide()`/`utf8()`
conversions with `MultiByteToWideChar`/`WideCharToMultiByte`); `u3mp/patcher` 152 in 11 files; `tools` 29; runtime
29. Rule (AGENTS.md): UTF-8 inside, wide only at a Windows API boundary. Practical route: `std::filesystem::path` for
paths (native on both), `std::string` UTF-8 for text, `wide()` only inside `#ifdef _WIN32` bodies. The `wchar_t`
exported API is handled with `un_path_char` (§4.1).

### 8.2 Separators, drive letters, prefixes

- Backslash string literals: `launcher/native` 151 in 23 files, `u3mp/patcher` 63 in 12 files, runtime 110 in 6
  files (part are escapes/regex). Joins with `L"\\"` (e.g. `shortcuts.cpp:126`) become `fs::path /`.
- `win_path()` (`dctools/fixlib.cpp`, 10 call sites in `u3fix` steps) turns `/` into `\` "as Python's `os.path` gives
  it on Windows": where such text only goes to messages, use `fs::path` native form; where it lands in an output file,
  keep it identical on both systems (outputs must not depend on the OS).
- `recipe.cpp:87` emulates Windows `fnmatch` (case-insensitive, `/` = `\`): keep the same matcher on both systems so
  a recipe selects the same files everywhere.
- Drive roots: `runtime_file.c` (`"E:"` handling), `\\?\` long-path prefixes (`gameupdate.cpp`, `start.cpp`,
  `system.cpp`): Windows bodies only.
- `stageplan.cpp:2016` `"d:/dev/CP9/..."`: game data strings, not host paths: unchanged.

### 8.3 Environment and user folders

| Windows | Where | Linux |
|---|---|---|
| `%APPDATA%\Uncharted Launcher\launcher.json` (seed from before GitHub #47), `USERPROFILE` | `core.cpp appdata_file()` | `$XDG_CONFIG_HOME` (`~/.config`); the seed only matters for old Windows installs |
| `%LOCALAPPDATA%` (Qt cache), `%LOCALAPPDATA%` note | `launcher-qt/src/backend.cpp:84`, `system.cpp` | `QStandardPaths::CacheLocation` (`~/.cache`) |
| Desktop (`FOLDERID_Desktop`) | `system.cpp:394`, `shortcuts.cpp` | `xdg-user-dir DESKTOP` / `~/Desktop` |
| `ProgramFiles`, `ProgramFiles(x86)` | `steam.cpp` | Steam paths (§4.1) |
| env names case-insensitive | `importer/common.cpp:419` | case-sensitive |

### 8.4 Case sensitivity (important on Linux)

Windows (NTFS, default) ignores case; Linux file systems (ext4, btrfs, and `ntfs3` mounts) do not. Places that rely
on it:

| Place | Today | Risk on Linux | Both-OS rule |
|---|---|---|---|
| Runtime `/app0` / `/hostapp` base mount (`runtime_file.c resolve()`) | guest path appended to the game folder as given | the game asks for a name in another case than on disk → `ENOENT` | index the base mount like the overlays (the overlay index already stores lower-case keys, `overlay_key()`), or a case-folding retry on `ENOENT`; identical result on Windows |
| `/app0` update overlays | case-insensitive index | none | keep |
| `merge_listing` (`strcasecmp`) | directory listing merge | none | keep |
| Launcher game-folder checks: `eboot.bin`, `sce_sys`, `param.sfo` (`actions.cpp`, `core.cpp`, `gamefiles.cpp`, `gameupdate.cpp`, `prepare.cpp`, `start.cpp`, `starter_files.cpp`, `steam.cpp`, `system.cpp`) | fixed lower-case names | a dump with `EBOOT.BIN` (FAT/exFAT copies) fails | `port::find_ci(dir, name)` at the few entry points that read the player's folders |
| PS3 sources: `PS3_GAME/USRDIR`, `PARAM.SFO` (`ps3keep.cpp`, `ps3values.cpp`, `recipe.cpp`, `u3import.cpp`, `u2mp/.../sources.cpp`) | discovery already compares `lower(name) == "usrdir"`, but later joins use fixed names | mixed-case copies | same `find_ci` at discovery, then use the real names |
| `CharLowerBuffW` / `lstrcmpiW` / `_wcsicmp` folding (`apply.cpp`, `gameupdate.cpp`, `starter_files.cpp`, `steam.cpp`, `system.cpp`, `config.cpp`) | compare and sort names "like Windows/Python" | none if kept | keep the same folding on both systems (behaviour identical); use a portable Unicode-lower or ASCII-lower exactly as the data needs |
| Import outputs (`u3fix` overlays, registries) | written with the recipe's case | the game reads them through the case-insensitive overlay index | none |
| Steam `shortcuts.vdf` matching | `folded()` (Unicode lower) | none | keep |

### 8.5 Line endings, limits, locking, links

- `\r\n` written in 25 files (`play.cpp`, `start.cpp`, logs, `lab_reg.cpp`, `run_mkregistry.cpp` registry text,
  `ps3keep.cpp`, ...). Game-data formats keep `\r\n` on both systems (they are data, not host text); logs and config
  may keep it too so files are identical. `.gitattributes` keeps `*.sh` LF and `*.cmd` CRLF.
- `MAX_PATH`: 35 uses (mostly `system.cpp` 12, `steam.cpp` 10), runtime fixed buffers (1 024 / `PATH_MAX`). Use
  `fs::path`/`std::string`; Linux `PATH_MAX` is 4 096.
- Share modes: `FILE_SHARE_*`/`MOVEFILE_*`/reparse flags in 27 files. Windows blocks deleting/renaming open files;
  Linux never does. Code must not use "rename failed" as the signal that the game is running (use process checks).
  `MoveFileExW(MOVEFILE_REPLACE_EXISTING)` = `rename()` (atomic on Linux).
- Links: junctions (`u3import.cpp`, `FSCTL_SET_REPARSE_POINT`), hard links (`u3import.cpp`, `ps3keep.cpp`, same drive
  else copy), symlink update views (need Developer Mode/administrator on Windows; `runtime_file.c` works around the
  UCRT `stat` of symlinks). Linux: `symlink` always allowed, `link` on the same file system.

## 9. Processes and runtime behaviour

| Behaviour | Where | Windows | Linux (second body) |
|---|---|---|---|
| Launcher → `u3import.exe`, `u2import.exe`, online program, `game_starter.exe`, runtime (`Uncharted.exe`/`gow3-probe.exe`) | `core.cpp`, `play.cpp`, `start.cpp` | `CreateProcessW`, anonymous pipes, `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`, `CREATE_NO_WINDOW`, `CREATE_UNICODE_ENVIRONMENT`, suspended start + `ResumeThread` | `posix_spawn` (or `fork`+`exec` when a working directory is needed), `pipe2(O_CLOEXEC)`, argv vector, `poll` |
| Importer → `u3fix.exe` steps, `ffmpeg.exe`, `objdump.exe` (`u3mp/bin` put on `PATH`) | `dctools/proc.cpp`, `importer/common.cpp`, `recipe.cpp` | same, with `list2cmdline` quoting | same `port::spawn` |
| Launcher → `apply_update.exe` then exit | `updater.cpp`, `apply.cpp` | separate program because a running `.exe` cannot be replaced | tarball: replace by `rename`, restart; AppImage: swap the image |
| Runtime → online program (`u2_online.exe`) | `u2mp/online_link.cpp` | job object `KILL_ON_JOB_CLOSE` | `prctl(PR_SET_PDEATHSIG)` (branch exists) |
| Runtime restart with new patches | `probe.c` `GOW3_RESTART_COMMAND` | `CreateProcessA` with the same output handles | `execl("/bin/sh", "-c", command)` exists; its fallback still runs the old `run.sh`: update |
| Kill / wait / liveness | many | `TerminateProcess`, `WaitForSingleObject`, `OpenProcess(SYNCHRONIZE)` | `kill`, `waitpid`, `kill(pid, 0)` / `pidfd` |
| Process lists (Steam running, game running, memory, freeze) | `steam.cpp`, `memstat.cpp`, `hang.cpp`, `dctools/fixlib.cpp`, `probe.c` | toolhelp | `/proc` scan (`port::list_processes`) |
| Console windows | `-mwindows`, `CREATE_NO_WINDOW`, `GetStdHandle`+`WriteFile` logging in the runtime, `SetConsoleOutputCP(UTF-8)` in importers | Windows concept | not needed; `write(1, ...)` |
| Single instance | none in the public launcher ("already running" is in-process state) | | |
| Steam detection / Add to Steam | `steam.cpp` | registry, `steam.exe`, `ProgramFiles` | `~/.steam`, Flatpak path, `/proc`; same VDF writer |
| GPU / display queries | `display.cpp` (DXGI), `gow3_perf_stats.cpp` (NVML), `gow3-gpu-capabilities` (Vulkan) | DXGI, `nvml.dll` | SDL3/Vulkan, `libnvidia-ml.so.1` |
| Timers | `compat_win.c` (`timeBeginPeriod`, high-resolution waitable timer, power throttling, `GOW3_TIMER_RES`), QPC, `GetTickCount64` | as is | `clock_nanosleep`/`CLOCK_MONOTONIC` (exists) |
| Threads | runtime: winpthreads (pthread API on Windows too); launcher: `CreateThread` (`hang`, `memstat`, `runtime_env`), `pokes.cpp` | | `std::thread` in shared code; `pthread_setname_np`, `pthread_setaffinity_np`, priorities via `setpriority`/`sched` |
| Wine/Proton support of the Windows build | `system.cpp` (`wine_get_version`), `config.cpp`/`actions.cpp` (`wine_get_unix_file_name`, `game_dir_unix`), Steam Deck hints | keep: the Windows build under Proton stays supported | not compiled into the native Linux build |

## 10. Data and scripts

- **Recipe step commands** name programs with `.exe`: 33 step files call `"u3fix.exe"`, one `"u3import.exe"`
  (`u3mp/patcher/recipe/steps/*.json`); `recipe.cpp:1280` resolves those two names next to `u3import.exe`, else on
  `PATH`. The recipe fingerprint includes the step data, so **do not rename them in the JSON** (every step would
  rerun on update). Map `<name>.exe` → `<name>` once, at run time, in the Linux body of the resolver. **Done**
  (`port::program_name` in `recipe.cpp`'s Linux body; the fingerprint is the same on both systems).
- Recipe notes mention `.exe` and Windows tools in prose only (no effect).
- `.ps1`/`.bat`/`.cmd` in the repository: only `.claude/hooks/no-python.ps1` (a development hook, not part of the
  build). `pwsh` steps: two in `build-windows.yml` (git config, release) → bash in the release job.
- Tools called by name with `.exe` in C++: `uncharted_native.dll` (42 mentions), `u3fix.exe` (24), `u2import.exe` (19),
  `u3import.exe` (16), `u2_online.exe` (13), `Uncharted.exe` (11), `UCMR_Launcher.exe` (11), `uncharted_online.exe`
  (8), `ffmpeg.exe` (8), `apply_update.exe` (8), `objdump.exe` (5), `game_starter.exe` (5), `SDL3.dll` (5),
  `python.exe`/`pythonw.exe` (7, the update bridge for old launchers, Windows only). Proposal: one
  `port::exe_name("u3fix")` that appends `.exe` on Windows, and library names via `port::library_name("SDL3")`.

## 11. Game-data assumptions

- The player's PS4 copy (`CUSA02344` with `eboot.bin` and `sce_sys`), PS3 folders and the update are read in place;
  nothing requires NTFS. Case: §8.4.
- Import output and the update view live under the install folder: hard links / junctions on the same drive with copy
  fallback (`u3import.cpp`, `ps3keep.cpp`); on Linux hard links need the same file system (copy fallback stays).
- File systems on Linux: external NTFS drives mounted with `ntfs3` are case-sensitive; exFAT/FAT is case-insensitive
  but has no hard links or symlinks (copy fallback needed for the junction case too).
- The slow-drive warning uses `GetDriveTypeW`/`DeviceIoControl` (seek penalty): Linux reads
  `/sys/block/<dev>/queue/rotational`.
- Free-space checks: `GetDiskFreeSpaceExW` → `std::filesystem::space` (runtime already uses `statvfs`).
- Under Wine/Proton today the launcher stores the game folder's Unix path (`game_dir_unix`) so another prefix still
  finds it; a native Linux build stores plain paths, and a config written by the Windows build under Proton can be
  read by the Linux build through that field.

## 12. The platform layer

One header for the launcher, importers and tools (`launcher/native/port.h`; the importers and `u3fix` already build
with `-Ilauncher/native`). It holds only calls that have no standard form; everything else uses `std::filesystem`,
`<fstream>`, `<chrono>`, `<thread>`. Each function has its Windows body (today's code, unchanged) and its Linux body
side by side:

| Function | Windows body | Linux body |
|---|---|---|
| `port::utf8(path)`, `port::path(utf8)` | `WideCharToMultiByte`/`MultiByteToWideChar` | identity |
| `port::replace_file(from, to)` / `write_atomic` | `MoveFileExW(REPLACE_EXISTING)` | `rename` |
| `port::hard_link`, `port::dir_link` | `CreateHardLinkW`, junction (`FSCTL_SET_REPARSE_POINT`) | `link`, `symlink` |
| `port::file_id(path)` | `GetFileInformationByHandle(Ex)` | `stat` `st_dev`/`st_ino` |
| `port::find_ci(dir, name)` | exact open (NTFS) | directory scan with ASCII fold |
| `port::executable_path()` | `GetModuleFileNameW` | `readlink("/proc/self/exe")` |
| `port::user_dirs()` | `SHGetKnownFolderPath`, `%APPDATA%`, `%LOCALAPPDATA%` | XDG variables + `~` |
| `port::env(name)`, `port::set_env`, `port::env_block` | `GetEnvironmentVariableW`, `_putenv_s`, `GetEnvironmentStringsW` | `getenv`, `setenv`, `environ` |
| `port::spawn(Spawn, Child&, error)` → `Child` (`read_some`, `write_all`, `running`, `wait`, `kill`, `close`) | each caller's own `CreateProcessW` + pipes + handle list + `CREATE_NO_WINDOW`, unchanged | **done**: `fork` + `pipe2(O_CLOEXEC)` + `execve` (the child sets folder, priority, death signal and stdio before the program starts; a failed `execve` comes back through a close-on-exec pipe) + `waitpid`; PATH lookup in the parent |
| `Spawn::die_with_parent` (kill_with_parent) | job object (`u2mp/online_link.cpp`) | **done**: `PR_SET_PDEATHSIG` in the child (it fires when the starting *thread* ends: start such a child from a thread that lives as long as it). No launcher caller needs it today |
| `port::kill_tree(pid)` | `taskkill /PID <pid> /T /F` (kept in the callers) | **done**: the `/proc` descendants, stopped, then `SIGKILL` |
| `port::process_alive(pid)`, `port::list_processes()`, `port::process_memory(pid)` | `OpenProcess`, toolhelp, `K32GetProcessMemoryInfo` | **done** except `process_memory` (phase 6): `kill(pid,0)`, `/proc/<pid>/stat` + the `exe` link's name |
| `port::exe_name(base)`, `port::program_name(name)` | `+ ".exe"`, unchanged | as is, without `.exe` — **done** |
| `port::load_library(name)`, `port::symbol` | `LoadLibraryExW`, `GetProcAddress` | `dlopen`, `dlsym` |
| `port::open_path(path_or_url)` | `ShellExecuteW("open")` | `xdg-open` |
| `port::safe_read`, `port::safe_write` (own process, unmapped = skipped) | `ReadProcessMemory`/`WriteProcessMemory` | `process_vm_readv`/`process_vm_writev` |
| `port::random_bytes`, `port::secure_zero` | `BCryptGenRandom`, `SecureZeroMemory` | `getrandom`, volatile loop |
| `port::http_get(url, sink)` | WinHTTP | libcurl — **done** inside `updater.cpp` (its only user), so curl stays out of the importers' builds |
| `port::drive_kind(path)` | `GetDriveTypeW` + seek penalty | `/sys/block/*/queue/rotational` |
| `port::library_name(base)` | `+ ".dll"` | `lib + ".so"` |
| `sha256` (no OS code) | one shared implementation replacing BCrypt and the 4 copies | same |
| `sha1.h` (no OS code) | bcrypt kept | the Linux builds' SHA-1 (PS4 NIDs, the U2 import's source check) |
| `port::wtext(arg)`, `port::arg(text)`, `port::arg_string` | exported path argument ↔ the core's wide text (`wchar_t` as before) | UTF-8 `char` |
| `port::not_yet(what)`, `port::not_yet_answer` | not used | the stubs of later phases: one stderr line, "not on Linux yet" |

The runtime keeps its own split (C today: `compat_win.{c,h}` + POSIX bodies; `u2mp/host_memory.h` style for C++).
When its C files move to C++, they can use the same `port.h` where it fits.

## 13. Transition order

Every phase keeps the Windows build and its tests green; the Linux job grows phase by phase. "Both" = built and tested
in both CI jobs.

| Phase | Work | Both builds show |
|---|---|---|
| 0. CI skeleton | `build-linux.yml` (or a second job): build what is already portable (u2 importer core, importer/fix libraries, `gow3gpu.so`, the GPU unit tests outside `if (WIN32)`); release moves to its own job | Linux job green on a subset; Windows unchanged. **Done** (§7.4) except `gow3gpu.so` and the release job (it stays in `build-windows.yml` until phase 7) |
| 1. Platform layer + leaf files | `port.h`, one shared SHA-256, `un_path_char` in `native.h`; port the S-files of §4.1 (`session_value`, `updates`, `gamepads`, `pages`, `patch_tables`, `starter`, `self_reader.h`, `prepare`, `gamefiles`, `patcher`, `config`, `actions`), `u2import.cpp`, `mkzip`, `code_ids` | launcher core compiles on Linux with stubs that say "not on Linux yet" for Steam, shortcuts, hang, display. **Done**: every §4.1 file builds on Linux; the stubs are marked in §4.1 |
| 2. Processes | `port::spawn`; `core.cpp`, `play.cpp`, `start.cpp`, `updater.cpp`, `dctools/proc.cpp`, `importer/common.cpp`, `u3import.cpp` (links), recipe `.exe` mapping, `wmain`→`main` | `u3import`/`u3fix`/`u2import` run on both; an import test on a fixture runs in both jobs. **Done**: both CI jobs run `tools/importer_smoke.sh` (no game files; no fixture test exists yet, §7.4) and Linux runs `port_smoke`. Still stubs on Linux: unpacking the update tarball and the apply step (phase 7), the window icon's PNG decode, the online program (phase 5), the runtime itself (phase 4) |
| 3. Window + package | `launcher-qt` CMake/file names/`QStandardPaths`; `package_linux.sh` (tarball); Linux branch of `build_ffmpeg.sh`; README-Linux | Linux tarball artifact from every push. **Done** (§4.2, §7.4, §7.5): the window runs on Linux (checked headless in CI with `--self-check`), `UCMR_linux.tar.gz` from every push and PR; no game runtime in it yet, no release yet |
| 4. Runtime | one `gow3-probe` CMake target for both; tests out of `if (WIN32)`; fix unguarded `pokes.cpp`, `session.cpp`, `u2_online_abi.h`, `u3voice.h`; check every POSIX body in `probe.c`/`runtime_*.c` against current Windows features (exec watch, restart command, scheduling, native TSC); `gow3:` Windows additions in shadPS4 get Linux bodies; case-insensitive base mount | the game boots on Linux in CI smoke tests that need no game files (runtime tests, loader tests). **Done** (§4.3–§4.6): the CI job builds the runtime with every subsystem, starts it without a game (usage, a missing boot file, `--vulkan-only` on lavapipe, the `uncharted_u3_voice` export) and runs the 63 unit tests; the package carries the runtime. Found and fixed on the way: native TSC, the GPU user folder, the warm-up crash mark, save data removal. Not yet: a game played on Linux; exec watch, old-CPU emulation, DLSS and the freeze report stay Windows-only (phase 6); multiplayer needs phase 5 |
| 5. Online and helper | the runtime host-function table and one helper PC backend over it, used on both systems; online programs with BSD sockets and the `0600` token file on Linux (DPAPI stays on Windows) | U3 MP/LAN on Linux; Windows helper behaviour unchanged. **Done in code**: the runtime's host function table (`uncharted_host.h`, `runtime_host_api.cpp`, found through `/dev/uncharted_host`; `host-api-test` on both systems), `plat_pc.inc` over it on both systems (helper v7.267, built, not shipped: it changes the Windows helper and needs a LAN and online test with two PCs first), and both online programs built and tested on Linux (`u2_online` through the runtime's `online_link.cpp`, `uncharted_online` with the relay sign-out test). Not yet: a multiplayer match on Linux |
| 6. Linux bodies for Windows-only features | `runtime_isa` on `ucontext_t`, hang watchdog (`ptrace`), Add to Steam on Linux, `.desktop` shortcuts, display info, profiler, DLSS (optional) | feature parity list shrinks. **Done in the launcher**: Add to Steam, desktop shortcuts, the freeze watchdog and thread report, the memory monitor, the window icon (§4.1); display info was done in phase 1. **Done in the runtime**: old-CPU emulation (`runtime_isa.cpp` on a `ucontext_t`, and the pass over the game's functions in `runtime_redzone.cpp`), the exec watch (perf-event breakpoints), the guest profiler (`SIGPROF` sampling). **Still Windows-only**: DLSS, the U2 crash log (`u2mp/crashlog.cpp`, the `u2mp` maintainer's), minidumps (no Linux equivalent without gdb) |
| 7. Releases | release job attaches `UCMR_windows.zip` + `UCMR_linux.tar.gz` (+ AppImage with a movable data root); updater picks its asset by OS; `recomplauncher.json` gains the Linux program name and `system.linux` | one tag, both packages. **Done**: the launcher update on Linux (download, unpack `UCMR_linux.tar.gz`, `tools/apply_update`, restart); the Linux package job fetches the private package files as the Windows job does (the online programs are built there from their source, then the private `apply` puts the Linux files in), checks the online programs, and on a pushed tag adds `UCMR_linux.tar.gz` to the release the Windows job makes; `recomplauncher.json` names `UCMR_Launcher` after `UCMR_Launcher.exe` and has `system.linux` (glibc 2.38, the game runtime's; Vulkan 1.3). Not run yet: a tag (no release since). **Open**: the AppImage (§7.6) |

Rules kept throughout: C++20 only (no new C, no Python), one platform function per OS call with both bodies side by
side, no behaviour change on Windows, the simplest form that does the full job.

## 14. Not checked / open questions

- The launcher core, the importers, the runtime and its unit tests build and run on Linux (§7.4); no game has been
  played on Linux yet (no game files in CI), so the runtime's Linux paths past the loader (guest memory, threads,
  files, the GPU on a real driver) are built and unit-tested but not seen in a game.
- The shipped U3 multiplayer helper (v7.266) still looks for the Windows process block, which Linux does not have (its
  `gs:0x60` read lands in the guest's zeroed thread block): every PC call is skipped there, so the game runs without
  LAN or online play and without voice on Linux until helper v7.267 (the host table, §4.10) ships. The host table, the
  online programs and the Linux exec watch, profiler and old-CPU code were tested with unit tests and small test
  programs on a Linux kernel (WSL2), not in a game.
- The Linux bodies of the imports, PLAY and the game start were built and run with made-up folders and `sh`, never
  with a real game or a whole import on Linux.
- The Linux window was checked headless (`QT_QPA_PLATFORM=offscreen`) in CI only, not yet on a real X11/Wayland
  desktop or a Steam Deck; the QML is unchanged, but Linux has no Segoe UI, so text uses the system's sans font.
- Counts are token-based (comments included); `helper-source` counts include BearSSL.
- Submodules under `gow3_pc/gpu/third_party` (FSR-Vulkan, xbyak, ...) were not scanned (not tracked files).
- RecompLauncher picks a release file by the system's word in its name (its SDK page): `UCMR_windows.zip` and
  `UCMR_linux.tar.gz` qualify; not tried with the launcher itself.
- Decided (see "Decisions" at the top): tarball first, `0600` token file, one shared helper backend over a runtime
  host-function table. Still open: where an AppImage keeps its data, and whether `gow3gpu` becomes static on Linux.
- Legal/licensing of bundling SDL3/Qt/FFmpeg in an AppImage follows the same license files as the Windows package;
  not reviewed in detail.

## 15. Relation to the earlier draft

A parallel session wrote an uncommitted draft (`linux-port.md` with a plan and a `launcher/native/port.h` outline,
`linux-port-inventory.md` with per-file Win32 call counts for `launcher/native`, the importer and `gow3_pc`). This
document is a superset: it adds `launcher-qt`, the gpu shims and our `gow3:` changes in shadPS4, the tests, the U3
helper's PC backend, the online programs, the ABI/TLS/exception work, libraries, build, CI, packaging, releases,
RecompLauncher, case sensitivity, and the "Windows and Linux from one tree" framing. Counts here are from release
0.10.169 and differ from the draft in places (for example `system.cpp` 53 sites here, `u2mp` now mostly portable). The
draft's code changes (`port.h`, `session_value.cpp`, `updates.cpp`, `gamepads.cpp`, the `.so` file name in
`launcher-qt`) are not on `main` yet. The owner decided to take that draft over into this work: this file stays the
one inventory and reference, the draft's `port.h` API is folded into §12 and its per-file tiers into §13 phases 1–2.
