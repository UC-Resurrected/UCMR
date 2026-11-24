# part-emitters: the 3 overlay-r3 dc2 particle modules (recipe step convert-dc2-part-modules)

Rebuilds `overlay-r3/u3data/build/main/dc2/part-module/part-{cave,gompa,shambhala}-multi.bin` byte for byte from the
player's PS3 Uncharted 3 (1.19 update `dlc-patch18.psarc` + disc `build/main/bin.psarc`) and the retail PS4 Collection
`u3data/build/main/dc2/part-module/` files. Tools: the batch45/batch48 scripts of the 2026-10-27 cloud session
(recovered u3-particle-emitter-converter), made to work on bytes, with the pickles replaced by tsv tables.

    python build_part_modules.py [--apply] [--dest DIR]      (rendered by the recipe driver; dry run by default)

| step | what | check |
|---|---|---|
| batch45 | `graft44.py`: the 31 Sanctuary water effects (`batch45-graft.tsv`) copied with their object closures from the retail PS4 village / oasis / graveyard / fort / old-quarter / warzone modules into retail `part-gompa-multi.bin` | md5 c77a9e0c |
| samples | `epairs.s11_samples`: PS3 s11 / PS4 s8 typed-struct word lists of every emitter pair of the 33 MP modules (`mp-modules.tsv`); the emitter converter maps PS3 words onto PS4 positions and takes the unmapped PS4 words from the matching sample | (equal to the session's s11map.pkl) |
| batch48 | `build48.py` + `emitconv.py` (`rootconv`, `ptconv`, `bcconv`, `donormap`): 22 PS3 1.19 emitters converted to the PS4 emitter format and appended to retail cave / shambhala and to the batch45 gompa; 7 groups re-pointed to their 1.19 lists | md5 9c9d7dad / 42594391 / 33184d35 |

Nothing is written unless all 3 md5s match. part-ruins-multi (also made by batch48) is not built: the reference
overlay never installed it.

## Tables (no game data)

| file | content |
|---|---|
| `root4rules.tsv` | 94 PS4 emitter-root word rules learned from the 1824 PS3/PS4 emitter pairs: 53 `const` (one fixed 4-byte value for that offset, mostly 0, 1.0, -1.0), 34 `sw` / 4 `id` / 3 `h16` (which PS3 root offset feeds it and how it is byte-swapped) |
| `mattab.tsv` | PS3 material string id -> PS4 material string id, with the number of emitter pairs that showed it (371 PS3 ids, 377 rows) |
| `emitter-names.tsv` | the 22 emitter names build48 writes (sid = string id of the name, checked); replaces the 121 k-entry siddict.pkl |
| `batch45-graft.tsv` | the 31 effect sids and the retail PS4 module each one is grafted from (batch45 `gompa_src.txt`) |
| `mp-modules.tsv` | the 33 MP part module pairs and the PS3 layer of each (patch18 or disc) |
| `inputs.tsv` | sha256 of the 66 source modules the reference was built from (a difference is reported, not fatal) |

The session's `s11map.pkl` held 1822 PS3/PS4 struct copies (game bytes); it is not shipped: `epairs.s11_samples`
collects the same samples from the player's files (verified equal, all 3 struct types, same order).

## Root rules beyond root4rules.tsv (2026-11-15)

Checked by converting every PS3 emitter of the 33 MP module pairs that has a retail PS4 twin (1732 emitters) and
comparing with retail: 1023 differed before, 53 after (the rest: the material choice of mattab.tsv, and rare parameter
tables where PS3 holds a constant-kind entry).
- root +0x04 = PS3 root byte 0xb0 (the draw class: 0 glow / fog / sound, 2 smoke / flames, 3 additive fire, 4 heat
  distortion; equal in all 1824 pairs). It was written as 0: the converted Sanctuary / Shambhala fire, smoke and heat
  haze emitters drew as plain sprites (white torch flames, white blobs over the lamps).
- root +0x40 = the PS3 flag word +0x28 with each bit moved to its PS4 place (table in `root_words`, learned from the
  pairs; the old rule only moved the bits from 15 up by two).

## The general 1.19 rule (2026-11-15, append119.tsv)

After batch48, every PS3 part module (newest layer, test modules aside) is compared with its PS4 module (the batch48
result, else retail): every PS3 group whose emitter list the PS4 module lacks or lists otherwise gets the PS3 list,
and every emitter of it the PS4 module lacks is converted (the same emitter converter) and appended; a group PS4 lacks
gets a copy of the module's first group root with its own name, id (root +0x08: the game's module unload finds the group by it), count and list. Emitters without a name in
emitter-names.tsv are named `u3mp-ps3/<sid>` (the game finds them by the symbol table's string id). An emitter the
converter cannot place (the PS3 +0x58 pointer variant, a PS3 material missing from mattab.tsv) is left out of its groups.
Result: 8 modules (adv-fort, adv-fort-cistern, cave, gompa, multiplayer, obj-shade, ruins, shambhala); 125 of the 130
missing PS3 emitters and 35 of the 38 missing groups added. Users: ss-coop-adv-fort (groups 7e341f2e, c04b8e3b), net.bin
(part-multiplayer 386790d4), props.bin (part-obj-shade 7c0ea70e), and the PS3 level paks' spawners. The md5 of every
written module is in `append119.tsv`; nothing is written unless all match.
