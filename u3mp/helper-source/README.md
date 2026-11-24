# Uncharted 3 multiplayer helper: source

The source of the helper that the patcher loads into Uncharted 3 (the built helper is in `u3mp/patcher/build/v*`).
This folder has no version in its name on purpose: it is never renamed, so a pull request here never collides with a
new helper version.

| File | What it is |
|---|---|
| `u3_lan_profiles.c` | the C part; it ends with a note where the maintainer's build adds a generated block (see below) |
| `u3x_fixes.cpp` | the C++ part: where ported code goes |
| `agc_port.inc`, `kr_port.inc` | C parts included by `u3_lan_profiles.c` |
| `plat.h`, `plat_*.inc`, `u3_bearssl.inc` | the platform layer and TLS |
| `spec/`, `reimpl2/` | the descriptions of the function groups and their new code as it was tested |

**The generated block.** The maintainer's build compiles `u3x_fixes.cpp`, together with private parts that are not
in this repository, to assembly and puts it at the end of `u3_lan_profiles.c` as one block (from the line
`/* ===== u3x (C++): generated from u3x_fixes.cpp ... do not edit by hand ===== */` to the end of the file). That
block is not published: the `u3_lan_profiles.c` here ends with a short note where it goes. Never add or commit such a
block.

**Tests.** The old-versus-new tests that used to be in `tests/` are retired: the code they compared against has been
replaced. The group tests that check the helper now are maintainer-only and are run before every helper version.

How to contribute here without conflicts: [CONTRIBUTING.md, section 7](../../CONTRIBUTING.md#-7-helper-source-working-side-by-side).
