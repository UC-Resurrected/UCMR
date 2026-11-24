#!/usr/bin/env bash
# build.sh - builds uncharted_native.dll (the launcher's native code) with MSYS2 CLANG64, statically linked: it needs
# only Windows system DLLs. Output: launcher/native/uncharted_native.dll (tools/package_windows.sh runs this).
set -euo pipefail
cd -- "$(dirname -- "$0")"
CLANG64=${CLANG64:-/c/msys64/clang64}
export PATH="$CLANG64/bin:$PATH"
# ccache is optional (the CI installs it and restores .ccache); without it this compiles as before.
CXX=(clang++)
command -v ccache >/dev/null && CXX=(ccache clang++)
mkdir -p build
source ../../tools/winres/winres.sh   # winres: version information and manifest of each file built here
FLAGS=(-O2 -DNDEBUG -fno-ident -ffunction-sections -fdata-sections -std=c++17 -Wall -Wextra -Wno-unused-parameter -municode)
OBJECTS=()
for s in session_value steam hang memstat config display controls system gamepads patcher starter runtime_env starter_files patch_tables prepare link start play updater gamefiles gameupdate pages shortcuts actions updates core; do
    "${CXX[@]}" "${FLAGS[@]}" -c "$s.cpp" -o "build/$s.o"
    OBJECTS+=("build/$s.o")
done
winres build/uncharted_native.res.o uncharted_native.dll "UCMR launcher core"
"${CXX[@]}" -shared -static -s -Wl,--gc-sections -Wl,--no-insert-timestamp -o uncharted_native.dll \
    "${OBJECTS[@]}" build/uncharted_native.res.o \
    -lole32 -lwindowscodecs -luuid -ladvapi32 -luser32 -ldbghelp -ldxgi -lgdi32 -lshell32 -lbcrypt -lwinhttp
llvm-objdump -p uncharted_native.dll | grep "DLL Name" | sort -u
sha256sum uncharted_native.dll
# The game starter PLAY runs (game_starter.cpp: a console program; it loads uncharted_native.dll next to it).
winres build/game_starter.res.o game_starter.exe "UCMR game starter"
"${CXX[@]}" "${FLAGS[@]}" -static -s -Wl,--gc-sections -Wl,--no-insert-timestamp -o game_starter.exe game_starter.cpp \
    build/game_starter.res.o
llvm-objdump -p game_starter.exe | grep "DLL Name" | sort -u
# The launcher update's installer (apply.cpp: tools\apply_update.exe in the package). It stands alone (static, no DLL
# of ours).
winres build/apply_update.res.o apply_update.exe "UCMR update installer"
"${CXX[@]}" "${FLAGS[@]}" -mwindows -static -s -Wl,--gc-sections -Wl,--no-insert-timestamp -o apply_update.exe apply.cpp \
    build/apply_update.res.o -luser32 -lgdi32 -lshell32
llvm-objdump -p apply_update.exe | grep "DLL Name" | sort -u
