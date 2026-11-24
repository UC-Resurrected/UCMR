# Rules for coding agents (Claude, Codex, Copilot, Cursor and others)

These rules apply to every change in this repository. They are the owner's decision and are not up for discussion.

## Language: EVERYTHING in C++. No C, no Python.

**The goal is a codebase that is 100% C++.** Another language is used only where C++ is really impossible or clearly
not practical, and that case is rare. "It is quicker", "the file is already C" or "it is just a small script" are not
reasons.

- **Write all new code in C++** (C++20, built with clang through the existing `build.sh` / CMake setups). This includes
  tools, tests, build and import steps, one-off helpers and fixes.
- **All existing C is ported to C++.** That covers the Uncharted 3 helper's `u3_lan_profiles.c` and its `.inc` files,
  the PC runtime `gow3_pc/src/*.c` and every other `.c` / `.h`. Porting them is a goal in itself, not only "when you
  change a function". Do not add C files or C functions.
- **"Done" means the code is C++.** Reimplementing game code from descriptions and moving our own C to C++ are both
  part of the job; a component still written in C is not finished.
- **Report progress as the share of C++ in the repository** (GitHub's language bar), not only as functions or groups.
- **No Python, ever.** Never add a Python file (`.py`, `.pyw`, `.pyi`, notebooks): not for a feature, a fix, a "quick"
  tool, a test, a build step, a one-off script or a CI step. Never extend existing Python: port the tool (or the part
  you need) to C++, switch the callers, and remove the Python file in its own commit. Never copy Python files into new
  folders (for example a new helper build folder under `u3mp/patcher/build/`).
- **The only accepted exceptions,** because C++ cannot do the job or it would not be practical:
  - third-party libraries kept as they are (for example BearSSL, `u3_bearssl.inc`);
  - a few lines of assembly inside C++ where the game's calling convention or an instruction needs it (calls that
    pass floats in x87 / xmm registers, `int 0x41`);
  - QML for the launcher's Qt window layout (`launcher-qt/`), with all logic in C++;
  - short shell (`bash`) for CI and packaging, PowerShell on Windows, CMake for builds: glue only, anything with real
    logic goes into C++.
- **If you think something really needs another language, stop and ask the owner** before writing it. Do not write it
  "for now".

## Map fixes are part of the PS3 -> PS4 conversion itself

- **Every map and asset fix is built into the asset converters own conversion logic** (the level, actor, DC, sound and
  particle converters in `u3mp/patcher/tools/fixes/`), so the converter writes the correct result directly and the
  import converts each asset in one pass, already fixed.
- **No separate fix-ups:** no new recipe steps that patch converted maps afterwards, and no "convert, then patch"
  pass inside a step either (no second pass that re-reads and edits what the converter just wrote). Fix the rule that
  produced the wrong bytes.
- **The goal is a faster import with fewer steps.** Existing fix-up steps (map-colors, map-lighting, london-fade,
  water-jitter, grass-leaf, facility-collapse, wall-emblem and the like) are turned into converter rules and removed.
- No per-map patches and no md5- or offset-locked edits.

## Keep it simple, but legal and exact

- **Prefer the simplest implementation that does the job**, with fewer moving parts, fewer special cases and no
  layers, options or abstractions nobody needs yet. Before adding code, check whether existing code already does it.
- **Always legal:** code that comes from the game (PS3 or PS4) is reimplemented from a description of what it does,
  never rewritten 1:1 from the original code, so that no copyrighted code is reproduced. Do not copy constant tables
  or values from the game into the source.
- **Simple only when it is exact:** a simpler version is used only if it replicates 100% of what the original does,
  in every case. If the simpler way would change or break any behaviour, keep the full behaviour and do not simplify.
  Check this with a test that runs the old and the new code on the same inputs.

## Assets: PS4 first, PS3 only when PS4 has none

- **Port an asset from the PS3 version only when the PS4 version has no such asset.** If PS4 already has it
  anywhere (multiplayer or campaign), the import uses the PS4 asset: it usually has better textures, and not
  bringing over a second copy keeps the import smaller.
- Example: baseball-shirt Drake (red shirt Drake) is in PS4 as a campaign skin, so it is not ported from PS3; the
  import uses the PS4 asset.
- Before adding a PS3 asset to an import step, check PS4's data (campaign and multiplayer) for the same asset.

## Versions: keep ONLY the latest

- **The repository holds only the latest version of anything versioned.** Never keep an older version next to a new
  one: no `v7.109/` beside `v7.110/`, no `tool_v2.cpp` beside `tool.cpp`, no `old/`, `backup/` or `-copy` folders.
  Older versions live in git history only.
- **A new helper build replaces the previous one in the same commit:** `git mv u3mp/patcher/build/v<old>
  u3mp/patcher/build/v<new>`, then update its files. `u3mp/patcher/build/` always contains exactly one folder.
  `tools/package_windows.sh` stops when more than one is committed.
- **Never copy files to make a new version.** Change the existing files. Shared code lives in one place and is used
  from there, not duplicated per version.

## Where C++ goes

- Launcher: `launcher/native/` (the core, pages, updates, game setup). The window is `launcher-qt/` (Qt/QML).
- Game runtime and tests: `gow3_pc/` (build with ninja; see its README).
- New import or recipe steps: a C++ tool built with the launcher, run by the recipe like any other step.

## Helper source: several people at once

The Uncharted 3 helper's source is `u3mp/helper-source/` (see CONTRIBUTING.md, section 7).

In short: the contributor syncs their fork, edits only `u3mp/helper-source/`, and opens a pull request; the
maintainer merges it, builds and ships the next helper version. Nobody else changes a version number or builds. Each
person writes only in their own marked spots and ports only the rows assigned to them, so changes never collide.
When you work for a contributor:

- **Never make a helper version.** No new or renamed `u3mp/patcher/build/v*` folder, no `build.json` change, no
  version number. Only the maintainer ships helper versions, once per batch, after merging the helper PRs.
- **Never edit or commit the generated block** at the end of `u3_lan_profiles.c` (from the line
  `u3x (C++): generated from u3x_fixes.cpp` to the end; it is not published, the file ends with a note there) or any
  built file.
- **Write only between the contributor markers:** C++ in `u3x_fixes.cpp` above `===== end of contributor section
  =====`; `u3x_` prototypes and `U3H` exports in their contributor blocks in `u3_lan_profiles.c`. Outside them, only
  turn the C function you ported into a one-line wrapper that calls your `u3x_` function. Never change the three
  lines after an end marker. The maintainer's agent appends only at the very end of `u3x_fixes.cpp`.
- **Port only the rows assigned to your contributor.** The old-versus-new tests (`tests/`) are retired; the group
  tests are maintainer-only.

## Agent tools: context-mode and caveman lite, always on

- **context-mode is always on.** Agents that have the context-mode connector (MCP server / plugin) use it for every
  step that counts, sums, filters or searches more than about 20 lines of data: logs (`boot.log`), build and test
  output, disassembly, big JSON / TSV, long source files, web pages. Run the step with `ctx_execute` and print only the
  answer, not the raw data; use `ctx_search` to find more in output it already indexed. Small output and file edits
  use the normal tools. Do not turn it off or work around it.
- **Caveman lite is the reply style, always on.** Answer first, then the reason, then the next step. Short, complete,
  natural sentences; no greetings, filler, hedging or recaps. Keep every technical fact exact: commands, paths,
  numbers, addresses, error lines verbatim. Never drop a "not", "never", "no" or "only". Use plain full sentences for
  security warnings, irreversible actions, step-by-step instructions and questions to the owner. It applies to chat
  replies only: code, comments, commit messages, pull requests, docs and release notes stay normal, clear prose. The
  style stays on until the owner says "normal mode".

## Other rules

- No game files, no server addresses, host names or credentials, anywhere in the repository or in commit messages.
- Keep the launcher's UI exactly as it is unless the owner asks for a change.
