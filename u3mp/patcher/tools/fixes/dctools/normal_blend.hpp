// dctools/normal_blend.hpp: the stage converter's rule for PS3 layer blends that come from a normal map's blue channel
// (formerly the level pass map-colors2, which rebuilt the finished pak).
//
// A PS3 prime-shader material whose main-pass fragment program takes the blend factor from the blue (z) channel of a
// normal map (the term t + M / (2s + 0.01) traced back to a texture channel), converted onto a template whose BlendMap
// is a flat generated mask, gets a grey BC1 copy of that blue channel as its BlendMap (inverted when the material
// inverts the blend map): 'art/generated/u3mp/zblend/<normal>-zblend[inv].tga', the mips from the first within 256 px.
#pragma once
#include "ndpak.hpp"

#include <string>
#include <vector>

namespace u3::nblend {

// a converted material as the level pak will hold it
struct Material {
    std::string name, shader;                                  // 'path:variant,GUID,shader', 'prime-shader.fx'
    std::vector<std::string> feats;
    std::vector<std::pair<std::string, std::string>> binds;   // parameter -> short texture path as written
    std::string base() const { return name.substr(0, name.find(',')); }
};
// one new blend map: the material (index into the list given; the last material of that name, as the game finds it)
// binds g_tNdFetchBlendMap to it
struct Blend {
    size_t mat;
    std::string shrt;   // 'art/generated/u3mp/zblend/....tga'
    Bytes texels;       // PS4 tiled BC1
    uint32_t w, h, mips;
};
// the blends of one level (ps3 = the PS3 level pak it is converted from), in material order; none when the PS3 programs
// cannot be read
std::vector<Blend> normal_blue_blends(const Pak &ps3, const std::vector<Material> &mats);

}  // namespace u3::nblend
