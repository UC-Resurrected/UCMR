# pak23-levels: the converted level paks of overlay-port / -r2 / -r3

`u3fix.exe levels <port|r2|r3> --apply ...` (driver `levels.cpp`; recipe steps `convert-levels-port`, `-r2`, `-r3`)
builds every row of `plan.tsv` from the player's own PS3 / PS4 files with **the latest converter**, all in C++
(`tools/fixes/dctools`): f525859 for every row, the sky converter's version for the sky rows. No Python runs.
Since 2026-11-13 no older converter version and no recorded per-level choice is kept (owner's decision: all assets with
the latest converter).

| file | what |
|---|---|
| `plan.tsv` | one row per level pak: method, options (`mb` = map-build fix passes), PS3 source archive / pak, sha256 of the latest converter's build |
| `levels.cpp` | the driver: skip a file already at its sha256, build the rest, install only a matching build, back up a replaced file to `work/backup-pak23-levels/` |
| `data/conv_anim_actors.json` | actor names the converter treats as animated |
| `data/water_templates.json` | the retail water.fx templates the sky planner scores PS3 water materials against |
| `data/overrides/overlay-r3/coop-syria-area1.json`, `-area2.json` | `gobo`: the projector-light gobo textures taken from the retail PS4 syria paks |

Methods (C++ files in `dctools/`):

| method | rows | what |
|---|---|---|
| stage | 321 | `stageplan.cpp` (the survey of the retail PS4 paks, the material plan, the retail material / effect / texture extraction) and `ps4stage.cpp` (the PS3 background pak as one PS4 GEOMETRY_1 with its materials, effects, textures, light table, water, collision `collision.cpp`, VIS `visinfo.cpp`); `mb`: + paramfix + cubefix |
| ingame | 355 | `ps4ingame.cpp` |
| empty | 124 | the 248-byte empty level (`ps4writer.cpp`) |
| vischeat | 23 | `vischeat.cpp` |
| sky | 8 | `ps4stage.cpp` `sky_convert` (the sky converter's version: sky materials on retail sky-clouds.fx templates, its own BC3 encoder and collision fix, the PS3 fragment-program readers in `ps3fp.cpp`) + paramfix |
| hud | 1 | the retail hud.pak with 2 full-size PS3 HUD textures (`texcodec.cpp`) |
| windoff | 1 | a retail pak with some wind strengths zeroed (`windoff.cpp`) |

Work folder: `<project>/scratch/pak23-levels-work` (PS3 extracts, ps3idx.tsv, the survey `res/`, plan files).

## Level fixes (recipe 1.17.0)

The in-place batches of 2026-10-29 / 30 (cloud-batches b102 / b103 / h017c / h017co / h028, lost-batches 97 / 98 / 99 /
100 / 101 / 101m / n012 / n014 / n015 / n015b) patched the old converter's output at recorded md5s, so none of them
could apply to the latest converter. Their rules are part of the stage conversion now (`stage::Options::fixes`, set by
levels.cpp for every stage row; the sky rows always had them):

| batch | rule in `dctools/ps4stage.cpp` / `collision.cpp` |
|---|---|
| n015 / n015b | LEVEL_INFO_1 = the PS3 level flags |
| 99, 100 C | a placeholder lightmap-uv stream (all integers) does not flag the instance as lightmapped |
| h017c / h017co | degenerate collision triangles (no area) dropped, as Havok's tools do, so no primitive reads as a Havok CUSTOM one (the Borneo co-op freeze; the older rotation left (a,b,b) as the CUSTOM (b,b,a): 19 installed paks, 154 primitives) |
| 101 / 101m | a detail-multiply colour layer gets the PS3 '-detail' map, else neutral white (not the black flat mask) |
| b102 | '-height' / '-blend2' / '-inc' maps in BlendMap / ColorTintMask3 / IncandescenceMask (BC1 UNORM) |
| b103 (+ b104) | blend-map polarity from the PS3 fragment program (inverted copy), blend factor from another texture's channel |
| h028 | spec terms of templates without a spec-map slot scaled by the PS3 mask mean |
| 98 | water materials on the best-scoring retail water.fx template (`data/water_templates.json`); the water collision layer was already right |
| n014 | ldn-train-multi-tunnel: light probes 3 and 4 take probe 5's data (a bad PS3 bake that lit the player white) |

batch100 A / B (shadow blockers) and n012 (Museum blockers and fades) are covered by the converter's shadow-only blocker
rule and its own fade records (recipe 1.30.0; before, the steps map-lighting and london-fade). Checked on the player's files: 0 of the 116 paks those batches
touched still has a LEVEL_INFO or lightmap-flag error (before: 60 and 52), no detail layer is black (before: 10), flat
blend / tint / glow bindings 182 -> 80 (the rest are the cases the batches left open too).

## Closer to PS3 (2026-11-22)

Each rule checked against the PS3 levels (their textures and material texture lists) and the retail PS4 paks:

| rule | where | effect (154 level paks measured) |
|---|---|---|
| a retail level pak holds only a resident 64x64 copy of each texture the game streams; the converted levels do not stream, so such a texture comes at full size from the PS4 texture dictionaries (`texturedict1/ps4`, same path and format, top mips above the PS3 texture's size left out), else from the PS3 texture | `stageplan.cpp` `dict_texture`, `ps4stage.cpp` `texture_for_full` | 10396 textures in 124 paks at full size instead of 64x64 (Desert: 139 of 143; the co-op London / Syria / Airport levels nearly all); VRAM of the built paks 1862 -> 3925 MB in all, at most ~1.3x the PS3 level's (PS4 normal maps are BC5) |
| layers 4..6 are the specular colour layers (retail binds its '-spec-mask' maps to Mask4 6275 times, Mask5 1133, Color4 1009, Mask6 549): not foreign to a PS3 material with a spec map, and a PS3 spec map counts as the mask it becomes (ps4stage binds it to a mask slot) | `stageplan.cpp` `v2_v23`, the role score | of the 30339 textures the PS3 materials list (lightmaps aside), 28463 -> 29241 are bound; spec-mask layers 2698 -> 3265 |
| a PS3 material with two normal maps needs a template with a second normal slot | `v2_v23` | second normal maps 3325 -> 4025 |
| a PS3 '-blend' map needs a template with a blend-map slot | the role score | blend-map materials 5531 -> 5580 |

A moving PS3 material (scroll / flow) keeps its scroll first: the three planner terms do not apply to it. Render classes
(blend / cut-off / two-sided) are unchanged. Open: san-upper-multi-stage is a retail pak (method windoff) and keeps its
resident copies.

## Soft blends, water reflections, cut-out maps (2026-11-22)

Checked against the PS3 levels (fragment programs, material textures and constants, render states, WATER_DATA kinds):

| rule | where | effect (113 level paks with a PS3 source) |
|---|---|---|
| a PS3 layer blend with a softness (the program divides the blend-map term by 2s + 0.01) needs a template with BLEND_FACTOR_SOFTNESS; without it the layer edge is a hard step and the blend map shows as black-and-white blotches (Facility's frozen ship and walls). Hard-edge PS3 programs are not asked for one | `stageplan.cpp` `soft_blend_score` | PS3 soft blends on a softness template 3208 -> 3367 of 3396 (Facility 37 -> 60 of 60, co-op Monastery cloister 23 -> 40, Highrise 80 -> 93, Graveyard 76 -> 88, Museum 43 -> 54) |
| a PS3 water program reflects its own '-env-reflection' cube: the water template needs a g_tNdFetchEnvCubeMap slot (ps4stage binds the PS3 cube), else the surface draws as its dark refraction colour (Museum's fountain: flat black). A template of another surface kind of the same class (4 / 5 mesh, 7 / 8 grid, 9 displacement) fits; a PS3 diffuse colour layer (algae) is kept first | `water_env_ok`, `water_class_ok`, `water_diffuse_ok` (the water template key) | PS3 env-cube waters with the cube bound 18 -> 21 of 27 (Museum's three basins); the 6 left are algae pools (Oasis, Dig co-op) whose colour + mask layer no env-cube template carries |
| an alpha-tested or blending PS3 material whose alpha is its own '-trans' map needs a template with a transparency-map slot; a template cutting by the colour map's alpha (opaque BC1 here) draws the cut-out parts solid (gate lattices) | `trans_map_score` | cut-out materials reading the PS3 '-trans' map 560 -> 564 of 576 (Facility's gate and girders, the ornate gates of Ruins and the co-op Monastery cloister) |
| a shadow-only blocker submesh's pass-exclusion word (+0x24: 0x1 = left out of the rain / snow occluder depth map, 0x100 = left out of the surface-reflection pass) follows retail's blockers by material: general/shadowblocker boxes 0 (a roof box keeps snow and rain off the ground under it: Facility's hangar box Prototype114.Submesh17 was 0x101), canopy shadows 0x100, library / layout / ik-only blockers 0x101 | `ps4stage.cpp` `blocker_pass_flags` | 66 stage rows (Facility, Fort, Airstrip, Chateau, Dig, Museum, Village, Warzone, Shambhala, co-op levels) |

Trade-off: the softness templates near some PS3 materials have no specular layers, so PS3 material textures bound 29050 -> 28990 of 30152 (107 lost, 66 of them on the co-op Monastery floors and walls: 83 spec masks, 12 second normal maps; 47 gained). Render classes unchanged. 154 stage rows rehashed, built together with the fix-up steps folded into the converter; 66 again for the blocker pass flags.
