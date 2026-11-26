#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
mkdir -p out
# One CMake build on both systems (gpu/CMakeLists.txt): the GPU library (static) and gow3-probe with every C++
# subsystem, plus gow3-gpu-capabilities. No PGO (GCC-only flags), LTO off by default (GOW3_LTO=ON turns it on).
# Windows: MSYS2 CLANG64 (packages in packaging/windows/README.md). Linux: a C++23 compiler (clang 18 or GCC 14;
# CC/CXX pick it) and the packages listed in .github/workflows/build-linux.yml, or shell.nix on NixOS.
# --test: also builds and runs the unit tests that need no game (tests/run_unit_tests.sh).
case $(uname -s) in
MINGW*|MSYS*)
    # shellcheck source=msys2-env.sh
    source ./msys2-env.sh
    for tool in clang cmake ninja pkg-config; do
        command -v "$tool" >/dev/null || { echo "Need $tool from MSYS2 CLANG64 (packaging/windows/README.md)." >&2; exit 1; }
    done
    exe=.exe
    compilers=(-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++)
    ;;
*)
    # Dependencies come from pkg-config and CMake packages. On NixOS the environment is provided by shell.nix;
    # re-enter it automatically if needed.
    if ! { command -v pkg-config >/dev/null && pkg-config --exists vulkan sdl3 && command -v cmake >/dev/null && command -v ninja >/dev/null; }; then
        if [[ -z ${GOW3_IN_NIX_SHELL:-} ]] && command -v nix-shell >/dev/null; then
            exec env GOW3_IN_NIX_SHELL=1 nix-shell shell.nix --run "bash build.sh $*"
        fi
        echo 'Need pkg-config with vulkan and sdl3, cmake and ninja (see shell.nix).' >&2; exit 1
    fi
    exe=
    compilers=()
    if [[ -n ${CC:-} ]]; then compilers+=(-DCMAKE_C_COMPILER="$CC"); fi
    if [[ -n ${CXX:-} ]]; then compilers+=(-DCMAKE_CXX_COMPILER="$CXX"); fi
    ;;
esac
if [[ ! -f gpu/third_party/fsr-vulkan/CMakeLists.txt || ! -f gpu/third_party/imgui/imgui.h ||
      ! -f gpu/third_party/xbyak/xbyak/xbyak.h || ! -f third_party/LibAtrac9/C/src/decoder.c ]]; then
    git submodule update --init --recursive
fi
# this port's changes to FSR-Vulkan (gpu/patches/fsr-vulkan), applied to its working tree once
for patch in gpu/patches/fsr-vulkan/*.patch; do
    if ! git -C gpu/third_party/fsr-vulkan apply --reverse --check "$PWD/$patch" 2>/dev/null; then
        git -C gpu/third_party/fsr-vulkan apply "$PWD/$patch"
    fi
done
# ccache is optional (the CI installs it and restores .ccache); the empty value clears it from an older cache.
cmake_cache=(-DCMAKE_C_COMPILER_LAUNCHER= -DCMAKE_CXX_COMPILER_LAUNCHER=)
command -v ccache >/dev/null && cmake_cache=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
cmake -S gpu -B out/gpu -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DGOW3_PGO=off \
    -DGOW3_LTO="${GOW3_LTO:-OFF}" "${compilers[@]}" "${cmake_cache[@]}" >/dev/null
echo "GPU library and loader: LTO ${GOW3_LTO:-OFF}"
if ! ninja -C out/gpu gow3-probe gow3-gpu-capabilities > out/gpu-build.log 2>&1; then
    grep -v '^\[' out/gpu-build.log | tail -40 >&2
    echo 'Build failed (full log: out/gpu-build.log)' >&2; exit 1
fi
cp "out/gpu/gow3-probe$exe" "out/gpu/gow3-gpu-capabilities$exe" out/
echo "Built $PWD/out/gow3-probe$exe"
if [[ ${1:-} == --test ]]; then
    bash tests/run_unit_tests.sh out/gpu
fi
