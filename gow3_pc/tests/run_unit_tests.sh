#!/usr/bin/env bash
# Builds and runs the unit tests that need no game (gpu/CMakeLists.txt, EXCLUDE_FROM_ALL targets) in a configured
# build folder: bash tests/run_unit_tests.sh out/gpu [test ...]. Used by build.sh --test and the Linux CI. Each test
# runs from gpu/ (its data paths are relative to it); a failing build or run is named, and in GitHub Actions also
# annotated. isa-test needs a CPU with MOVBE, BMI1, LZCNT and F16C (it compares the emulation with it). On Windows run the tests that write
# temporary files from PowerShell, not MSYS2 bash (its TMP is a POSIX path).
set -uo pipefail
cd -- "$(dirname -- "$0")/.."
build=${1:?build folder, e.g. out/gpu}
shift
build=$(cd -- "$build" && pwd)
exe=
case $(uname -s) in MINGW*|MSYS*) exe=.exe ;; esac
tests=("$@")
if (( ${#tests[@]} == 0 )); then
    tests=(
        # GPU library parts without a Vulkan device
        motion-history-test ui-composition-test async-compiler-test warmup-inbox-test async-pipeline-cache-test
        depth-stencil-state-test blend-rewrite-test aliasing-test async-fetch-snapshot-test png-decode-test
        upscaler-support-test orb-multiplier-test stat-multipliers-test perf-overlay-test graphics-settings-test
        cheat-hooks-test signatures-test readback-queue-test readback-sources-test wait-diagnostics-test
        page-state-test actor-hook-test cheat-hook-test fiber-test videodec2-test
        # the renderer, linked with gow3gpu and its runtime stubs
        pipeline-binary-cache-test pipeline-binary-initialization-test motion-shader-test scene-resolution-test
        taa-shader-test camera-motion-test
        # the runtime and its C++ subsystems
        mp3-test videodec-test services-test clocks-test savedata-test range-names-test content-test
        shared-handles-test pad-buffer-test pad-map-test split-pad-test audio-downmix-test cpu-check-test
        tsc-clock-test pokes-test name-dictionary-test debug-menus-test u3-tonemap-test u3-job-trace-test
        u2mp-net-test u2mp-voice-test u2mp-progression-test u2mp-events-test u2mp-roster-test u2mp-gamenet-test
        u2mp-cinema-test u2mp-peer-rules-test u2mp-overlay-test u2mp-clantag-test u2mp-start-rules-test
        u2mp-presence-test u2mp-coop-rules-test host-api-test isa-test
    )
fi
# test arguments (data files, relative to gpu/)
args_of() {
    case $1 in
    mp3-test) echo ../tests/data/sine.mp3 ;;
    videodec-test|videodec2-test) echo ../tests/data/tiny.h264 ;;
    esac
}
annotate() { [[ -n ${GITHUB_ACTIONS:-} ]] && echo "::error title=$1::$(tr '\n' ' ' < "$2" | cut -c1-900)"; }
failed=()
for t in "${tests[@]}"; do
    [[ -n ${GITHUB_ACTIONS:-} ]] && echo "::group::$t"
    log="$build/$t.log"
    if ! cmake --build "$build" --target "$t" > "$log" 2>&1; then
        cat "$log"; [[ -n ${GITHUB_ACTIONS:-} ]] && echo "::endgroup::"
        grep -m4 'error' "$log" > "$log.short"; annotate "$t (build)" "$log.short"
        failed+=("$t (build)"); continue
    fi
    # shellcheck disable=SC2046
    if ! (cd gpu && timeout 300 "$build/$t$exe" $(args_of "$t")) > "$build/$t.run" 2>&1; then
        cat "$build/$t.run"; [[ -n ${GITHUB_ACTIONS:-} ]] && echo "::endgroup::"
        tail -n 4 "$build/$t.run" > "$log.short"; annotate "$t (run)" "$log.short"
        failed+=("$t (run)"); continue
    fi
    cat "$build/$t.run"; [[ -n ${GITHUB_ACTIONS:-} ]] && echo "::endgroup::"
done
echo "${#tests[@]} tests, ${#failed[@]} failed${failed:+: ${failed[*]}}"
(( ${#failed[@]} == 0 ))
