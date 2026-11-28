#!/usr/bin/env bash
# Builds u2import, u3fix and u3import on Linux from the same sources tools/package_windows.sh builds them from on
# Windows (docs/WINDOWS-DEPENDENCIES.md §13 phase 2), with u3fix's code ids. Run from the repository root; CXX picks the
# compiler (default clang++), OUT the folder (default out/importers). tools/importer_smoke.sh checks the programs.
set -euo pipefail
CXX=${CXX:-clang++}
out=${OUT:-out/importers}
mkdir -p "$out"
fx=u3mp/patcher/tools/fixes im=u3mp/patcher/importer u2=u2mp/patcher/importer
fail=0
compile() {   # <object> <source> <flags...>: its first errors as an annotation when it fails
    local o=$1 s=$2
    shift 2
    "$CXX" "$@" -c "$s" -o "$o" 2> "$o.log" && return 0
    cat "$o.log"
    echo "::error title=$s::$(grep -m4 'error' "$o.log" | tr '\n' ' ')"
    return 1
}
build() {   # <name> <flags as one string> <sources...>: compiles in parallel; the object list in $out/<name>/objects.txt
    local name=$1 flags=$2 s o pids=() failed=0
    shift 2
    mkdir -p "$out/$name"
    : > "$out/$name/objects.txt"
    for s in "$@"; do
        o=$out/$name/$(echo "$s" | tr / _).o
        echo "$o $s" >> "$out/$name/objects.txt"
        # shellcheck disable=SC2086
        compile "$o" "$s" $flags -MMD -MF "$o.d" &
        pids+=($!)
        while (( $(jobs -rp | wc -l) >= $(nproc) )); do sleep 0.2; done
    done
    for p in "${pids[@]}"; do wait "$p" || failed=$((failed + 1)); done
    echo "$name: ${#pids[@]} files, $failed failed"
    return $(( failed > 0 ))
}
link() {   # <program> <objects.txt> [extra...]
    local exe=$1 list=$2
    shift 2
    # the C++ runtime linked in (as the launcher core's): the programs need only the system's C library
    # shellcheck disable=SC2046
    if ! "$CXX" -s -Wl,--gc-sections -static-libstdc++ -static-libgcc $(cut -d' ' -f1 "$list") "$@" -pthread -o "$exe" 2> "$exe.log"; then
        cat "$exe.log"; echo "::error title=link $(basename "$exe")::$(grep -m4 'undefined\|error' "$exe.log" | tr '\n' ' ')"; return 1
    fi
}

dc="$fx/dctools"
build u2import "-std=c++20 -O2 -I$u2 -I$im -I$dc -Ilauncher/native" \
    "$u2/u2import.cpp" "$u2/sources.cpp" "$u2/sound.cpp" "$u2/registry.cpp" "$u2/texts.cpp" "$u2/levels.cpp" "$u2/stage.cpp" \
    "$u2/netimg.cpp" "$u2/netpatch.cpp" "$u2/actor.cpp" "$u2/actorparts.cpp" "$u2/actors.cpp" "$u2/havok.cpp" "$u2/twin.cpp" \
    "$u2/overlay.cpp" "$u2/stagecheck.cpp" "$u2/validate.cpp" "$u2/devtools.cpp" "$im/common.cpp" "$dc/psarc.cpp" "$dc/dc.cpp" \
    "$dc/proc.cpp" "$dc/fixlib.cpp" "$fx/sound-banks/convert_banks.cpp" "$dc/ndpak.cpp" "$dc/ps4writer.cpp" "$dc/ps4ingame.cpp" \
    "$dc/ps4stage.cpp" "$dc/stageplan.cpp" "$dc/ps3bg.cpp" "$dc/visinfo.cpp" "$dc/collision.cpp" "$dc/texcodec.cpp" \
    "$dc/hkconvert.cpp" "$dc/ps3fp.cpp" "$dc/ps4mats.cpp" "$dc/ps4actor.cpp" "$dc/anim.cpp" "$dc/skel.cpp" "$dc/fgcoll.cpp" \
    "$dc/dcconv.cpp" "$dc/normal_blend.cpp" || fail=1
build u3fix "-std=c++20 -O2 -I$dc -Ilauncher/native" \
    "$dc/u3fix.cpp" "$dc/fixlib.cpp" "$dc/backup_folder.cpp" "$dc/dc.cpp" "$dc/eula_box.cpp" "$fx/no-lab/remove_lab.cpp" \
    "$fx/no-rpcn-menu/remove_rpcn_page.cpp" "$fx/netgame-order/netgame_order.cpp" "$fx/enemy-stall/apply_ss_option_defaults.cpp" \
    "$fx/coop-chars/apply_coop_desc_fix.cpp" "$fx/airport-igc3/airport_igc3.cpp" "$dc/psarc.cpp" "$fx/text1-merge/merge_text1.cpp" \
    "$fx/coop-audit/apply_option_defaults_2.cpp" "$fx/registry/run_mkregistry.cpp" "$fx/pak23-registry/apply_registry.cpp" \
    "$fx/hat-chloe/apply_hair_key.cpp" "$fx/pak23-levels/levels.cpp" "$fx/actor33/build_actors.cpp" "$dc/proc.cpp" "$dc/ndpak.cpp" \
    "$dc/ps4writer.cpp" "$dc/ps4ingame.cpp" "$dc/vischeat.cpp" "$dc/windoff.cpp" "$dc/texcodec.cpp" "$dc/ps3bg.cpp" \
    "$dc/stageplan.cpp" "$dc/ps4stage.cpp" "$dc/collision.cpp" "$dc/visinfo.cpp" "$dc/ps3fp.cpp" "$dc/anim.cpp" "$dc/skel.cpp" \
    "$dc/hkconvert.cpp" "$dc/fgcoll.cpp" "$dc/ps4actor.cpp" "$dc/ps4mats.cpp" "$dc/dcconv.cpp" "$fx/dcconv/convert_dc1.cpp" \
    "$fx/dcconv/apply_recipes.cpp" "$fx/sound-banks/convert_banks.cpp" "$fx/part-emitters/build_part_modules.cpp" \
    "$fx/audio-defs/audio_defs.cpp" "$fx/music-streams/copy_music.cpp" "$fx/actor33/actors_hk.cpp" "$fx/coop-vo/apply_vo.cpp" \
    "$fx/coop-voice2/apply_coop_vox.cpp" "$dc/net_edits.cpp" "$fx/eula-batch/make_eula.cpp" "$fx/wall-emblem/make_emblem.cpp" \
    "$dc/lab_reg.cpp" "$fx/world-levels/world_levels.cpp" "$dc/normal_blend.cpp" || fail=1
build u3import "-std=c++20 -O2 -I$im -I$dc -Ilauncher/native" \
    "$im/u3import.cpp" "$im/recipe.cpp" "$im/net18adapt.cpp" "$im/mpimage.cpp" "$im/ps3keep.cpp" "$im/ps3values.cpp" "$im/common.cpp" \
    "$dc/psarc.cpp" "$dc/dc.cpp" "$dc/fixlib.cpp" launcher/native/prepare.cpp launcher/native/link.cpp launcher/native/system.cpp || fail=1
(( fail == 0 )) || exit 1

# u3fix's code ids (tools/code_ids.cpp), as package_windows.sh makes them: the recipe reruns a step only when its code changed
"$CXX" -std=c++20 -O2 -o "$out/code_ids" tools/code_ids.cpp
nm -A -P -g $(cut -d' ' -f1 "$out/u3fix/objects.txt") > "$out/u3fix/nm.txt"
"$out/code_ids" "$dc/u3fix.cpp" "$out/u3fix/objects.txt" "$out/u3fix/nm.txt" "$("$CXX" --version | head -1)" "$out/u3fix/code_ids.cpp" \
    > "$out/u3fix/code_ids.log"
"$CXX" -std=c++20 -O2 -I"$dc" -Ilauncher/native -c "$out/u3fix/code_ids.cpp" -o "$out/u3fix/code_ids.o"

mkdir -p "$out/u2mp/bin" "$out/u3mp/bin"
link "$out/u2mp/bin/u2import" "$out/u2import/objects.txt" || fail=1
link "$out/u3mp/bin/u3fix" "$out/u3fix/objects.txt" "$out/u3fix/code_ids.o" -Wl,-u,u3fix_code_ids || fail=1
link "$out/u3mp/bin/u3import" "$out/u3import/objects.txt" -ldl || fail=1
(( fail == 0 )) || exit 1
grep -aq 'U3FIX-CODE-IDS 1' "$out/u3mp/bin/u3fix" || { echo "::error::u3fix carries no code ids"; exit 1; }
ls -l "$out/u2mp/bin/u2import" "$out/u3mp/bin/u3fix" "$out/u3mp/bin/u3import"
