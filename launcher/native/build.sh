#!/usr/bin/env bash
# build.sh - builds uncharted_native.dll (the launcher's native code) with MSYS2 CLANG64, statically linked: it needs
# only Windows system DLLs. Output: launcher/native/uncharted_native.dll (tools/package_windows.sh runs this).
# On Linux it builds libuncharted_native.so and game_starter instead (docs/WINDOWS-DEPENDENCIES.md §4.1, §7.4).
set -euo pipefail
cd -- "$(dirname -- "$0")"
SOURCES=(session_value steam hang memstat config display controls system gamepads patcher starter runtime_env starter_files
         patch_tables prepare link start play updater gamefiles gameupdate pages shortcuts actions updates core)
if [[ $(uname -s) == Linux ]]; then
    # CXX picks the compiler (clang++ by default, as on Windows); CXXFLAGS adds flags (an include folder, for example)
    CXX=(${CXX:-clang++})
    command -v ccache >/dev/null && CXX=(ccache "${CXX[@]}")
    FLAGS=(-O2 -DNDEBUG -fPIC -fvisibility=hidden -ffunction-sections -fdata-sections -std=c++17 -Wall -Wextra
           -Wno-unused-parameter ${CXXFLAGS:-})
    mkdir -p build
    OBJECTS=()
    for s in "${SOURCES[@]}"; do
        "${CXX[@]}" "${FLAGS[@]}" -c "$s.cpp" -o "build/$s.o"
        OBJECTS+=("build/$s.o")
    done
    # every symbol resolved at link time; the C++ runtime linked in, so the library needs only the system's C library
    "${CXX[@]}" -shared -s -Wl,--gc-sections -Wl,--no-undefined -static-libstdc++ -static-libgcc \
        -o libuncharted_native.so "${OBJECTS[@]}" -ldl -lpthread -lcurl -lz   # libcurl: the launcher update (updater.cpp); zlib: Steam artwork (steam.cpp)
    sha256sum libuncharted_native.so
    "${CXX[@]}" "${FLAGS[@]}" -s -static-libstdc++ -static-libgcc -o game_starter game_starter.cpp -ldl
    # the launcher update's installer (apply.cpp: tools/apply_update in the package; it stands alone, as on Windows)
    "${CXX[@]}" "${FLAGS[@]}" -s -static-libstdc++ -static-libgcc -o apply_update apply.cpp
    exit 0
fi
CLANG64=${CLANG64:-/c/msys64/clang64}
export PATH="$CLANG64/bin:$PATH"
# ccache is optional (the CI installs it and restores .ccache); without it this compiles as before.
CXX=(clang++)
command -v ccache >/dev/null && CXX=(ccache clang++)
mkdir -p build
source ../../tools/winres/winres.sh   # winres: version information and manifest of each file built here
FLAGS=(-O2 -DNDEBUG -fno-ident -ffunction-sections -fdata-sections -std=c++17 -Wall -Wextra -Wno-unused-parameter -municode)
OBJECTS=()
for s in "${SOURCES[@]}"; do
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
