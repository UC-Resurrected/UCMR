// wall-emblem/wall_emblem.hpp: the wall emblems of the Uncharted 3 multiplayer levels, made by the level conversion
// itself (wall-emblem/make_emblem.cpp; used by pak23-levels/levels.cpp).
//
// PS3 draws the leading team's emblem on background patches: the triangles of the materials with 'emblem' in their
// name whose emblem UVs cover the square 0..1 (bg::emblem_triangles). PS4's background has no per-instance overlay,
// so for every '<map>-multi' level:
//   * the stage converter leaves those triangles out of the level (stage::Options::emblem_walls; degenerate triangles);
//   * the levels driver builds one actor pak per PS3 instance of each emblem prototype group, holding exactly those
//     triangles, with the wall's look baked in (actors(); written to overlay-r3's actor33 with the level);
//   * the ingame12 converter places one NdSimpleObject spawner per actor at its centre (spawners());
//   * the registry recipe (pak23-registry/recipes/overlay-r3.json) lists the actors in their level's block.
// The helper's wall walk fills the actors' per-instance overlay with the player emblem (PS3 f_31444c).
#pragma once
#include "ndpak.hpp"
#include "ps4ingame.hpp"

#include <string>
#include <utility>
#include <vector>

namespace u3::wallemblem {

// a level that gets wall emblems: '<map>-multi' (the main multiplayer levels)
bool emblem_level(const std::string &level);
// the spawners of a level (actor name '<art>', spawner '<art>-spawner', at the patch centre); empty without patches
std::vector<ExtraSpawner> spawners(const Pak &p3stage, const std::string &level);
// the actor paks of a level: (art, pak), built from the PS3 level, the converted level pak (its wall materials and
// colour maps) and the retail actors (loading-menu-cloth effects, cargo-plane-int-moving-multi skeleton)
std::vector<std::pair<std::string, Bytes>> actors(const Pak &p3stage, const std::string &level, const Pak &converted, const fs::path &retail_actor33);

}  // namespace u3::wallemblem
