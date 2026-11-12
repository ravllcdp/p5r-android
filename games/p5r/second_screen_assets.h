// Companion asset extraction: reads portrait and icon sprites directly from the game's RomFS
// (via nx::game_image()), decodes their DDS textures (BC1/BC3), and writes them as raw RGBA
// files that SecondScreenService.java loads at startup.
//
// Output layout under out_dir/companion/:
//   portraits/p{0..10}.rgba   — party member portraits (index = kUnitMemberId), 84×84 RGBA8
//   icons/hp.rgba             — HP icon, 39×26 RGBA8 tinted #66FDDC
//   icons/sp.rgba             — SP icon, 39×26 RGBA8 tinted #FF64E8
//
// File format for every .rgba:
//   uint32_t width  (little-endian)
//   uint32_t height (little-endian)
//   uint8_t  rgba[width * height * 4]   (R, G, B, A per pixel, row-major)
#pragma once
#include <string>

namespace p5r::companion {

// Start the background extraction thread. Idempotent.
void start_asset_extraction(const std::string& out_dir);

// True once the extraction thread has finished (success or failure).
bool assets_done();

}  // namespace p5r::companion
