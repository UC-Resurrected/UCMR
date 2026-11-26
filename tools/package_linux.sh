#!/usr/bin/env bash
# package_linux.sh - assembles the Linux package: dist/UCMR_linux/ and its tarball, dist/UCMR_linux.tar.gz, with the
# folder layout of the Windows zip (tools/package_windows.sh; docs/WINDOWS-DEPENDENCIES.md §7.5): the launcher, the
# imports and the game runtime (§13 phase 4; no game was started from it on Linux yet).
#
# What the package holds:
#   UCMR_Launcher            what players start (tools/linux_starter/UCMR_Launcher.sh)
#   UCMR_Launcher.desktop    a menu entry to install (README.txt says how)
#   launcher-qt/             the launcher's window (Qt 6) with its Qt libraries, plugins and QML
#   launcher/native/         the launcher core (libuncharted_native.so) and the game starter
#   gow3_pc/out/             the game runtime (gow3-probe) with the libraries it brings (stage_runtime below)
#   u2mp/, u3mp/             the multiplayer importers (u2import, u3import, u3fix, tables, the helper build, ffmpeg)
#   tools/                   the base game's file list
#   licenses/                the licenses of everything bundled
# Never any game file. GNU objdump (the Uncharted 3 import's Havok tables) is the system's (binutils).
#
# Run on Linux from the repository root after tools/build_ffmpeg.sh. Settings:
#   CXX        the C++ compiler (default clang++); the importers need full C++20 (clang 16 or newer)
#   QT_DIR     the Qt 6 (6.5 or newer) to build against and ship, e.g. ~/Qt/6.8.3/gcc_64; default: qtpaths6 on PATH
# The runtime needs a C++23 compiler and its libraries (gow3_pc/build.sh); "package_linux.sh runtime" stages only it.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."

# ---------------------------------------------------------------------------------------------------------- setup

CXX=${CXX:-clang++}
export CXX
ffmpeg=downloads/ffmpeg-8.1.1-min-linux   # tools/build_ffmpeg.sh: the decoders the port uses, nothing else
need() {   # need <file> <how to make it>
    [[ -e $1 ]] || { echo "Missing $1: $2" >&2; exit 1; }
}
need "$ffmpeg/bin/ffmpeg" 'build the small FFmpeg first (tools/build_ffmpeg.sh)'

# The game runtime (gow3_pc/build.sh, linked with the small FFmpeg's static libraries) staged in dist/runtime:
# gow3_pc/out/gow3-probe without debug information, the libraries it loads that systems often lack (SDL3, fmt, Zydis,
# xxhash, ...: every one but the C library, the graphics stack, X11/Wayland, sound and zlib) beside it (the runtime
# finds them through its $ORIGIN run path) and their licenses. "package_linux.sh runtime" makes only this part (the
# Linux CI builds it on a newer system than the package); the package takes a staged runtime when there is one.
runtime_stage=dist/runtime
stage_runtime() {
    PKG_CONFIG_PATH="$PWD/$ffmpeg/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" bash gow3_pc/build.sh
    local dest=$runtime_stage/gow3_pc/out rlic=$runtime_stage/licenses listed name path files=()
    rm -rf -- "$runtime_stage"
    mkdir -p "$dest" "$rlic/runtime"
    strip --strip-debug -o "$dest/gow3-probe" gow3_pc/out/gow3-probe
    listed=$(ldd gow3_pc/out/gow3-probe)
    if grep -q 'not found' <<< "$listed"; then echo "The runtime needs libraries that are not installed:" >&2; grep 'not found' <<< "$listed" >&2; exit 1; fi
    while read -r name path; do
        [[ $name =~ ^(ld-linux|linux-vdso|libc|libm|libdl|libpthread|librt|libresolv|libgcc_s|libstdc\+\+|libvulkan|libX|libxcb|libz|libbsd|libmd|libGL|libEGL|libdrm|libgbm|libwayland|libasound|libpulse|libpipewire|libdbus|libudev|libsystemd)[.-] ]] && continue
        cp -L "$path" "$dest/$name"
        files+=("$path")
    done < <(awk '$2 == "=>" && $3 ~ /^\// {print $1, $3}' <<< "$listed" | sort -u)
    echo "runtime: $(du -sh "$dest" | cut -f1) with $(cd "$dest" && ls | grep -c '\.so') bundled libraries"
    # licenses: LibAtrac9 (MIT, in the source), FFmpeg (in licenses/ffmpeg), SDL3 (built from source, zlib license),
    # and the copyright files of the distribution packages a bundled library or the static Opus came from
    mkdir -p "$rlic/runtime/LibAtrac9"
    cp gow3_pc/third_party/LibAtrac9/LICENSE* "$rlic/runtime/LibAtrac9/" 2>/dev/null || true
    local sdl
    for path in "${files[@]}"; do
        if [[ $(basename -- "$path") == libSDL3.so* ]]; then
            sdl=$(dirname -- "$(readlink -f -- "$path")")
            mkdir -p "$rlic/runtime/SDL3"
            find "$sdl/.." "$sdl/../share" -maxdepth 4 -path '*licenses/SDL3/*' -exec cp {} "$rlic/runtime/SDL3/" \; 2>/dev/null || true
        fi
    done
    local opus
    opus=$(find /usr/lib /usr/lib64 -name libopus.a -print -quit 2>/dev/null || true)
    [[ -n $opus ]] && files+=("$opus")
    if command -v dpkg > /dev/null; then
        local packages p f
        mapfile -t packages < <(for f in "${files[@]}"; do
            f=$(readlink -f -- "$f")
            dpkg -S "$f" 2>/dev/null || dpkg -S "/usr$f" 2>/dev/null || dpkg -S "${f#/usr}" 2>/dev/null || true
        done | cut -d: -f1 | sort -u)
        for p in "${packages[@]}"; do
            [[ -f /usr/share/doc/$p/copyright ]] && { mkdir -p "$rlic/runtime/$p"; cp "/usr/share/doc/$p/copyright" "$rlic/runtime/$p/"; }
        done
        if (( ${#packages[@]} )); then
            {
                echo "Libraries from the build system's distribution packages in gow3_pc/out (or linked into gow3-probe)."
                dpkg-query -W -f '${Package} ${Version} (source: ${source:Package})\n' "${packages[@]}"
            } > "$rlic/runtime/THIRD-PARTY.txt"
        fi
    fi
}
if [[ ${1:-} == runtime ]]; then
    stage_runtime
    exit 0
fi
qtpaths=${QT_DIR:+$QT_DIR/bin/qtpaths}
[[ -n $qtpaths && -x $qtpaths ]] || qtpaths=$(command -v qtpaths6 || command -v qtpaths || true)
[[ -x $qtpaths ]] || { echo "No Qt 6 found: set QT_DIR (the folder with bin/qtpaths)" >&2; exit 1; }
qt_prefix=$("$qtpaths" --query QT_INSTALL_PREFIX)
qt_libs=$("$qtpaths" --query QT_INSTALL_LIBS)
qt_plugins=$("$qtpaths" --query QT_INSTALL_PLUGINS)
qt_qml=$("$qtpaths" --query QT_INSTALL_QML)
echo "Qt $("$qtpaths" --query QT_VERSION) from $qt_prefix"

# Only the latest helper build may be committed (AGENTS.md): as tools/package_windows.sh checks it
committed_builds=$(git ls-files u3mp/patcher/build | cut -d/ -f4 | sort -u)
if (( $(grep -c . <<< "$committed_builds") > 1 )); then
    echo "More than one helper build committed under u3mp/patcher/build:" $committed_builds >&2
    exit 1
fi

out=dist/stage/UCMR_linux
lic=$out/licenses
obj=dist/stage/obj
rm -rf -- dist/stage
mkdir -p "$out/tools" "$out/launcher/assets" "$out/launcher/native" "$out/analysis/uncharted-user" "$lic" "$obj"

# distribution packages whose files ship (their copyright files go into licenses/, see distro_licenses)
from_distro=()

drop_caches() {
    find "$1" -name __pycache__ -prune -exec rm -rf {} +
}

# ---------------------------------------------------------------------------------------------------------- parts

importers() {   # u2import, u3import and u3fix (tools/build_importers_linux.sh: the sources the Windows package builds)
    OUT=$obj/importers bash tools/build_importers_linux.sh
    mkdir -p "$out/u2mp/docs" "$out/u2mp/bin" "$out/u3mp/bin"
    cp u2mp/README.md "$out/u2mp/"
    cp u2mp/docs/import-list.tsv "$out/u2mp/docs/"
    cp "$obj/importers/u2mp/bin/u2import" "$out/u2mp/bin/"
    cp "$obj/importers/u3mp/bin/u3import" "$obj/importers/u3mp/bin/u3fix" "$out/u3mp/bin/"
    cp "$ffmpeg/bin/ffmpeg" "$out/u3mp/bin/"   # the EULA pages and the sound banks (as on Windows: FFmpeg 8.1.1)
}

runtime() {   # the game runtime and what it loads (stage_runtime: staged before, by the CI on another system, or now)
    [[ -f $runtime_stage/gow3_pc/out/gow3-probe ]] || stage_runtime
    mkdir -p "$out/gow3_pc"
    cp -r "$runtime_stage/gow3_pc/out" "$out/gow3_pc/"
    chmod 755 "$out/gow3_pc/out/gow3-probe"   # (a CI artifact keeps no file modes)
    cp -r "$runtime_stage/licenses/runtime" "$lic/"
}

u3_patcher() {   # the Uncharted 3 recipe and tools, and the newest multiplayer helper build (as on Windows)
    cp -r u3mp/patcher "$out/u3mp/patcher"
    rm -rf "$out/u3mp/patcher/maintainer"
    drop_caches "$out/u3mp/patcher"
    local builds=$out/u3mp/patcher/build newest=$committed_builds d want have
    for d in "$builds"/v*/; do [[ $(basename -- "$d") == "$newest" ]] || rm -rf -- "$d"; done
    want=$(grep -o '"link.json": *"[0-9a-f]*"' "$builds/$newest/build.json" | grep -o '[0-9a-f]\{64\}')
    have=$(sha256sum "$builds/$newest/link.json" 2>/dev/null | cut -c1-64 || true)
    if [[ -z $have && ${PACKAGE_WITHOUT_HELPER:-} == 1 ]]; then
        echo "multiplayer helper build $newest: its helper is not in this test package (no private files)"
    elif [[ -z $want || $want != "$have" ]]; then
        echo "The helper of build $newest is missing or is not the one its build.json names (private files out of step?)" >&2
        exit 1
    else
        echo "multiplayer helper build in the package: $newest"
    fi
    cp gow3_pc/LICENSE "$out/u3mp/patcher/LICENSE.txt"
}

launcher_core() {   # the launcher's native code: its core and the game starter
    bash launcher/native/build.sh
    cp launcher/native/libuncharted_native.so launcher/native/game_starter "$out/launcher/native/"
    cp tools/game_files_102.json.gz "$out/tools/"
    cp launcher/assets/UCMR.ico launcher/assets/icon.png launcher/assets/window_icon.bmp "$out/launcher/assets/"
}

# ship_libs <folder> <elf files...>: the libraries they load that are Qt's own (Qt and its ICU) or that desktop
# systems often lack (the xcb helpers Qt's X11 plugin needs), into <folder>; every other one is the system's
# (C library, graphics drivers, X11/Wayland, fontconfig, ...). A library that is not found stops the package.
ship_libs() {
    local dest=$1 listed missing name path
    shift
    # (the copied plugins' run paths point at launcher-qt/lib, which is still empty: Qt's own folder stands in for it)
    listed=$(LD_LIBRARY_PATH=$qt_libs ldd "$@" 2>/dev/null) || true
    missing=$(grep 'not found' <<< "$listed" | sort -u || true)
    if [[ -n $missing ]]; then
        echo "The launcher window needs libraries that are not installed here:" >&2
        echo "$missing" >&2
        return 1
    fi
    while read -r name path; do
        [[ -f $dest/$name ]] && continue
        if [[ $name == libQt6* || $name == libicu* ]] ||
           [[ $name =~ ^lib(xcb-(cursor|icccm|image|keysyms|render-util|util|xinerama|xkb)|xkbcommon-x11)\.so ]]; then
            cp -L "$path" "$dest/$name"
            [[ $path == "$qt_libs"/* ]] || from_distro+=("$path")
        fi
    done < <(awk '$2 == "=>" && $3 ~ /^\// {print $1, $3}' <<< "$listed" | sort -u)
}

# the Qt window with what it loads and nothing else
qt_window() {
    local build=dist/stage/qt-build win=$out/launcher-qt
    cmake -S launcher-qt -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="$CXX" \
        -DCMAKE_PREFIX_PATH="$qt_prefix" > "$build.log" 2>&1 || { cat "$build.log" >&2; exit 1; }
    ninja -C "$build" UCMRLauncher >> "$build.log" 2>&1 || { tail -40 "$build.log" >&2; exit 1; }
    mkdir -p "$win/lib" "$win/plugins" "$win/qml"
    strip -o "$win/UCMRLauncher" "$build/UCMRLauncher"
    # the platform plugins (X11, Wayland, and offscreen for checks without a screen), their GL and Wayland parts,
    # compose-key input, the .ico reader (the window icon) and the desktop portal (file dialogs in sandboxes)
    local dir f
    mkdir -p "$win/plugins/platforms" "$win/plugins/imageformats"
    for f in "$qt_plugins"/platforms/lib{qxcb,qwayland*,qoffscreen}.so; do [[ -f $f ]] && cp "$f" "$win/plugins/platforms/"; done
    cp "$qt_plugins/imageformats/libqico.so" "$win/plugins/imageformats/"
    for dir in xcbglintegrations wayland-shell-integration wayland-decoration-client wayland-graphics-integration-client \
            platforminputcontexts; do
        [[ -d $qt_plugins/$dir ]] && cp -r "$qt_plugins/$dir" "$win/plugins/"
    done
    if [[ -f $qt_plugins/platformthemes/libqxdgdesktopportal.so ]]; then
        mkdir -p "$win/plugins/platformthemes"
        cp "$qt_plugins/platformthemes/libqxdgdesktopportal.so" "$win/plugins/platformthemes/"
    fi
    # the QML the window imports (QtCore, QtQuick, Controls in the Basic style, Effects, Layouts); the rest goes, as on
    # Windows
    for dir in QtCore QtQml QtQuick; do cp -r "$qt_qml/$dir" "$win/qml/"; done
    local style module
    for style in FluentWinUI3 Fusion Imagine Material Universal Windows macOS iOS; do
        rm -rf "$win/qml/QtQuick/Controls/$style"
    done
    for module in QtQml/XmlListModel QtQuick/Dialogs QtQuick/LocalStorage QtQuick/NativeStyle QtQuick/Particles \
            QtQuick/Shapes QtQuick/VectorImage QtQuick/tooling QtQuick/Scene2D QtQuick/Scene3D QtQuick/Timeline \
            QtQuick/VirtualKeyboard QtQuick/Pdf; do
        rm -rf "$win/qml/$module"
    done
    find "$win/qml" -name '*.debug' -delete
    printf '[Paths]\nPrefix = .\nLibraries = lib\nPlugins = plugins\nQmlImports = qml\n' > "$win/qt.conf"   # never a system Qt
    # the libraries the program, its plugins and its QML plugins load (ldd lists what those load in turn)
    local elves
    mapfile -t elves < <(echo "$win/UCMRLauncher"; find "$win/plugins" "$win/qml" -name '*.so')
    ship_libs "$win/lib" "${elves[@]}"
    echo "launcher window: $(du -sh "$win" | cut -f1)"
}

texts() {
    cp tools/linux_starter/UCMR_Launcher.sh "$out/UCMR_Launcher"
    chmod 755 "$out/UCMR_Launcher"
    cp launcher-qt/linux/UCMR_Launcher.desktop "$out/"
    cp tools/VERSION "$out/VERSION"   # the update check compares this with the releases
    cp tools/README-Linux.txt "$out/README.txt"
    cp recomplauncher.json "$out/"
    cp gow3_pc/LICENSE "$out/LICENSE.txt"
    cp tools/defaults/disabled-optimizations.txt "$out/analysis/uncharted-user/"
}

licenses() {
    mkdir -p "$lic/bearssl" "$lic/ffmpeg" "$lic/qt"
    cp tools/licenses/bearssl/LICENSE.txt tools/licenses/bearssl/SOURCE.txt "$lic/bearssl/"   # TLS of the online parts
    cp "$ffmpeg/COPYING.LGPLv2.1" "$lic/ffmpeg/"
    printf '%s\n' "FFmpeg 8.1.1 (https://ffmpeg.org/releases/ffmpeg-8.1.1.tar.xz), LGPL 2.1 or later, unchanged; built by" \
        "tools/build_ffmpeg.sh in the port's source with only the decoders, formats and filters the port uses." \
        > "$lic/ffmpeg/SOURCE.txt"
    # Qt: LGPL 3 (its license texts as the system has them; base-files ships them on Debian and Ubuntu)
    local f
    for f in /usr/share/common-licenses/{LGPL-3,GPL-3}; do [[ -f $f ]] && cp "$f" "$lic/qt/"; done
    for f in "$qt_prefix"/../../Licenses/* "$qt_prefix"/LICENSES/*; do [[ -f $f ]] && cp "$f" "$lic/qt/"; done
    printf '%s\n' "Qt $("$qtpaths" --query QT_VERSION) (https://download.qt.io/official_releases/qt/), the libraries, plugins" \
        "and QML modules in launcher-qt/, unchanged, under the GNU LGPL 3 (LGPL-3 here; GPL-3 is the text it builds on)." \
        "Their source: https://download.qt.io/official_releases/qt/ (qtbase, qtdeclarative, qtwayland and their ICU)." \
        > "$lic/qt/SOURCE.txt"
    distro_licenses
}

# every distribution package with a file in the package: its copyright file and, in THIRD-PARTY.txt, its version
distro_licenses() {
    (( ${#from_distro[@]} )) || return 0
    command -v dpkg > /dev/null || { echo "Note: no dpkg: licenses/ lacks the system libraries' copyright files" >&2; return 0; }
    local packages p f
    # dpkg knows each file under one name: /usr/lib/... or /lib/... (merged /usr), whichever ldd did not print
    mapfile -t packages < <(for f in "${from_distro[@]}"; do
        f=$(readlink -f -- "$f")
        dpkg -S "$f" 2>/dev/null || dpkg -S "/usr$f" 2>/dev/null || dpkg -S "${f#/usr}" 2>/dev/null ||
            echo "Note: no distribution package owns $f" >&2
    done | cut -d: -f1 | sort -u)
    (( ${#packages[@]} )) || { echo "No distribution package found for the bundled system libraries" >&2; exit 1; }
    mkdir -p "$lic/distro"
    for p in "${packages[@]}"; do
        [[ -f /usr/share/doc/$p/copyright ]] && { mkdir -p "$lic/distro/$p"; cp "/usr/share/doc/$p/copyright" "$lic/distro/$p/"; }
    done
    {
        echo "Libraries from the build system's distribution packages that ship in launcher-qt/lib."
        dpkg-query -W -f '${Package} ${Version} (source: ${source:Package})\n' "${packages[@]}"
    } > "$lic/distro/THIRD-PARTY.txt"
}

# nothing from a game or a player may be in the package (the same check as tools/package_windows.sh)
nothing_from_games() {
    drop_caches "$out"
    local found
    found=$(find "$out" \( -iname eboot.bin -o -iname '*.elf' -o -iname sce_sys -o -iname savedata -o -iname cache \
        -o -iname '*.log' -o -iname 'probe*.ini' -o -iname '*.pkg' -o -iname '*.psarc' -o -iname '*.pak' \
        -o -iname 'boot*.bin' -o -iname '*.dds' -o -iname '*.at9' \) -print)
    found+=$(find "$out" -type d -iname 'overlay-*' -not -path '*/u3mp/patcher/tools/*' -not -path '*/u3mp/patcher/recipe/*' -print)
    found+=$(find "$out/u3mp/patcher" -type d -iname 'overlay-*' -exec find {} -type f \
        \( -iname '*.pak' -o -iname '*.bin' -o -iname '*.psarc' -o -iname '*.dds' \) \; 2>/dev/null)
    if [[ -n $found ]]; then
        printf 'These must not be in the package:\n%s\n' "$found" >&2
        exit 1
    fi
}

# the systems the package runs on: the newest C library (and C++ library) version any shipped program asks for
baseline() {
    local versions
    versions=$(find "$out" -type f \( -perm -u+x -o -name '*.so*' \) -exec sh -c 'head -c4 "$1" | grep -q ELF' _ {} \; \
        -exec objdump -T {} \; 2>/dev/null | grep -o 'GLIBC_[0-9.]*\|GLIBCXX_[0-9.]*' | sort -u -V)
    echo "needs at most: $(grep GLIBC_ <<< "$versions" | tail -1), $(grep GLIBCXX_ <<< "$versions" | tail -1 || echo 'no libstdc++')"
}

# ---------------------------------------------------------------------------------------------------------- build

importers
runtime
u3_patcher
launcher_core
qt_window
texts
licenses
nothing_from_games
baseline

rm -rf dist/UCMR_linux dist/UCMR_linux.tar.gz
mv "$out" dist/UCMR_linux
tar -C dist --owner=0 --group=0 --numeric-owner --sort=name -czf dist/UCMR_linux.tar.gz UCMR_linux
du -sh dist/UCMR_linux dist/UCMR_linux.tar.gz
