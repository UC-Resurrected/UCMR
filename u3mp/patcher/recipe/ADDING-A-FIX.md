# How to add a fix to the patcher

The recipe is a list of **steps**. Each step is one json file in `recipe/steps/`, and the only
registration point is `recipe/steps/ORDER.txt` (one step id per line, run top to bottom). A step runs
`u3fix.exe` commands; their C++ source lives in `u3mp/patcher/tools/fixes/<job>/` and is compiled into
`u3fix.exe` (the list of sources is in `tools/package_windows.sh`, the command names in
`tools/fixes/dctools/u3fix.cpp`). The driver is `u3import.exe` (`u3mp/patcher/importer`). No step runs Python.

## 0. A map, character or texture fix is a converter rule, not a step

A fix to a converted level, actor or texture pak goes into the converter that writes that pak, so the import writes
each pak once, already fixed. No new step that patches converted paks afterwards, and no second pass over the bytes
the converter just wrote. Where the rules live:

* levels (`convert-levels-*`, `tools/fixes/pak23-levels/plan.tsv`): `dctools/ps4stage.cpp` (geometry, instance
  flags, materials and bindings, light table), `dctools/stageplan.cpp` (template and texture choices); a level's PS3
  source is the row's `archive` / `src`;
* actors and net.pak (`convert-actors-r3`, `tools/fixes/actor33/jobs.tsv`): `dctools/ps4actor.cpp` (method
  `ps4actor`; `am` = a retail material template, modes `retail-mats`, `tint`, `tint-all`), `dctools/ps4mats.cpp`
  (`convert_net`, `wpnmods`).

Prefer a general rule (it fixes every pak with the same cause); a fix that is really one asset's is a rule keyed by
that level / material name, with a comment giving the reason. Then rehash the rows whose output changed (plan.tsv /
jobs.tsv `sha256`) and bump the recipe version.

## 1. Put the fix in a job folder

`tools/fixes/<job>/` holds the source of one fix job and its small data (plans, name lists). Rules:

* **No game data.** No paks, no PS3/PS4 extracts, no `.pak/.bin/.dds` built from the game. Only source, small
  json/tsv plans and name lists.
* **No machine paths.** The step passes the folders as arguments; the driver fills these placeholders in the
  step's `commands` (and in the job folder's text files when it renders the job into `<out>/scratch/<job>/`
  before running it):

  | placeholder | value |
  |---|---|
  | `{{PROJECT}}` | the output root; `{{PROJECT}}/work/overlay-r3/...` is the overlay being built (the project's `work/` convention) |
  | `{{WORK}}` | `{{PROJECT}}/work` |
  | `{{SCRATCH}}` / `{{RECOVERED}}` | `{{PROJECT}}/scratch` (every job is rendered there) |
  | `{{JOB}}` | the rendered job folder (`<out>/scratch/<job>`) |
  | `{{PS4}}` | the PS4 game folder (`eboot.bin`, `u3data/`, `u2data/`) |
  | `{{PS3_DISC}}` | PS3 U3 disc data `USRDIR` (`build/main/pak23.psarc`, `goty-*.psarc`, ...) |
  | `{{PS3_UPDATE}}` | PS3 U3 1.19 update `USRDIR` (`dlc-patch13.psarc`, `dlc-patch18.psarc`) |
  | `{{PS3_DATA2}}` | PS3 U3 `...DATA2/USRDIR` (`net18.bin`, `build/main/*-7.psarc`, `http-cache`) |
  | `{{PS3_DLCMAPS}}` | PS3 U3 DLC map packs `USRDIR` (`dlc-map1..4.psarc`, or the GOTY disc) |
  | `{{PS3_U2}}` | PS3 U2 `USRDIR` |
  | `{{SPEECH_LANGS}}` | comma list of the speech languages chosen with `--speech-langs` |

  The same values are in the step's environment as `RECIPE_<NAME>`, with `RECIPE_SHADOWED`, `RECIPE_MANIFEST`,
  `RECIPE_JOBS`, `RECIPE_PS4_ELF`, `RECIPE_PS3_ELF` and `RECIPE_FFMPEG` (see `importer/recipe.cpp`).
* **Dry run by default, `--apply` to write, idempotent.** The command must detect that it has
  already been applied (compare a hash, a magic value, a size, or keep a `.bak`) and then do
  nothing and exit 0. The driver also records the sha256 of every file the step wrote in
  `<out>/_recipe/state.json`; on the next run a step whose recorded outputs are unchanged is
  skipped as "already applied", so an updated patcher only reruns new or changed steps. Back up
  what you overwrite to `{{PROJECT}}/work/backup-<job>/` (never overwrite an existing backup).
* Read the PS3/PS4 data only through the placeholders above; do not look for other folders.

## 2. Write the step file

`recipe/steps/<NNN>-<id>.json` (NNN = three digits, leave gaps; `id` = short kebab-case name):

```json
{
 "id": "my-fix",
 "title": "One line: what it changes and where (u3fix.exe my-fix, my-job/apply_my_fix.cpp)",
 "kind": "script",
 "job": "my-job",
 "with_jobs": ["dctools"],
 "family": "overlay-r3/pak23",
 "needs": ["ps4", "ps3_disc"],
 "commands": [["u3fix.exe", "my-fix", "--apply", "--project", "{{PROJECT}}", "--ps4", "{{PS4}}"]],
 "writes": ["work/overlay-r3/u3data/build/main/pak23/ps4/air-multi*.pak"],
 "status": "reproducible",
 "applied": "2026-11-13",
 "notes": "what it depends on, what the user will see",
 "recipe_version_added": "1.18.0"
}
```

Fields:

* `kind`: `script` (runs `commands` in the rendered job folder), `copy-list` (the driver copies
  the files of `recipe/lists/<list>.tsv` from the inputs, sha256-checked; no program), or
  `unavailable` (documents a step that cannot run; give `reason`).
* `needs`: inputs the step must have: `ps4`, `ps3_disc`, `ps3_update`, `ps3_data2`,
  `ps3_dlcmaps`, `ps3_u2`, `tool:<path under tools/>`.
  Missing input = the step is skipped with a message, not an error.
  External programs are not a `needs` token: a command that uses ffmpeg looks it up itself (`RECIPE_FFMPEG`, PATH,
  `C:/msys64/clang64/bin/ffmpeg.exe`) and degrades or skips with a message (see `eula-batch/make_eula.cpp`).
* `commands`: argv lists; `u3fix.exe` is the one next to `u3import.exe`. `{{JOB}}` = the rendered job folder,
  `{{OUT}}` = the output root, every placeholder of the table works; an argument `@<file>` is replaced by that
  file's content (for long `--only` lists). `cwd` (optional) overrides the working directory (default `{{JOB}}`).
  `with_jobs` (optional): other job folders rendered for the step; their data files (not their code) join its
  fingerprint. The code a step runs counts through u3fix.exe's code ids (`tools/code_ids.cpp`): a change in the
  code a u3fix step uses, wherever it lives, reruns it at the next update; nothing else does (RECIPE.md).
  `ok_codes` (default `[0]`; a code other than 0 in the list = an input is missing, the step runs again next time),
  `expect` (a string that must appear in the output), `log_lines`.
* `writes`: globs (relative to the output root) of the files the step may change; the driver hashes
  them before and after to record what changed. `must_write: false` lets a step succeed when it
  changes nothing (already applied).
  A step whose `writes` files all equal `manifest.tsv` is skipped as "already at the reference state". A new
  step that changes files the manifest predates must set `reference_check: false` (or update the manifest),
  or it never runs on a build that matches the reference (recipe 1.11.0's map-lighting was skipped that way).
* `status`: `reproducible`, `needs-converted-base` (works only once the converted paks exist),
  `needs-build` (ships the build script but not its output), `blocked-lost-tool`,
  `not-reproducible-locally`. Informational, printed by `--list`.

## 3. Register it and bump the version

1. Add the command to `tools/fixes/dctools/u3fix.cpp` / `u3fix.hpp` and the source to the u3fix line of
   `tools/package_windows.sh`.
2. Append the id to `recipe/steps/ORDER.txt` at the position it must run (new fixes normally go
   last; a fix to `net.bin` must come after everything else that rewrites `net.bin`, and
   `classic-netgames` must be rerun after any `net.bin` rebuild).
3. Bump `recipe/VERSION` (semver: new step = minor, changed step = patch).
4. Test: `u3import.exe --recipe ... --out <existing output> --only my-fix`, then run it a
   second time: it must print `already applied`. Then `u3import.exe --verify-output <out>` if the
   reference `manifest.tsv` was updated with the new file hashes.

## 4. Updating a user's build

The launcher reruns the import (`u3import.exe`) on the same data folder. Steps already recorded in
`<out>/_recipe/state.json` whose fingerprint and files still match are skipped; new steps and steps whose
definition, data, code or inputs changed run again (the log says why); `--force` or `--only <id>` reruns by hand.
`--dry-run` shows what would run and why. `--list` shows the current step table.
