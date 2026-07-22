// World map exporter — renders the whole overworld to a PNG so its structure can be *looked at*.
//
// This is a debugging tool first and a preview second, and it earns its place: world generation is
// the one system whose failures are invisible from inside the game. A village walled in by a
// stronghold, a ring boundary that reads as a dartboard, an ocean cutting the map in half — none of
// those show up in a screenshot of one 40x22-tile viewport, and all of them are obvious in one look
// at the whole map.
//
// It deliberately draws FLAT COLOURS rather than tiles. A debug map is for reading structure, and
// 1024 tiles of pixel art at 1px each would be mush.
//
// It also needs no actor engine: terrain is a pure function of (seed, x, y), so this links nothing
// but `tiles.hpp` and raylib's image API. That is the same property that lets any node in a cluster
// compute terrain without asking anyone — here it just makes the tool trivial.
//
// Run:  build/mmo_worldmap [--seed N] [--scale N] [--out FILE] [--rings]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "raylib.h"

#include "world/tiles.hpp"

using namespace mmo;

namespace {

struct Rgb {
    unsigned char r, g, b;
};

// Chosen for legibility at 1px, not for beauty: neighbouring terrains must be tellable apart when
// each is a single pixel.
[[nodiscard]] Rgb colour_of(Terrain t) {
    switch (t) {
        case Terrain::kGrass: return {104, 158, 74};
        case Terrain::kDirt: return {148, 106, 66};
        case Terrain::kWater: return {58, 100, 168};
        case Terrain::kStone: return {126, 126, 134};
        case Terrain::kSand: return {214, 190, 130};
        case Terrain::kTree: return {48, 104, 56};
        case Terrain::kSnow: return {228, 234, 242};
        case Terrain::kMarsh: return {86, 96, 62};
        case Terrain::kAsh: return {74, 66, 76};
        case Terrain::kCount: break;
    }
    return {255, 0, 255};
}

constexpr const char* kRingNames[kRingCount] = {"Meadow", "Forest", "Wetland", "Snow", "Wasteland"};

void put(std::vector<unsigned char>& px, int w, int x, int y, Rgb c, unsigned char a = 255) {
    const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
    px[i] = c.r;
    px[i + 1] = c.g;
    px[i + 2] = c.b;
    px[i + 3] = a;
}

void cross(std::vector<unsigned char>& px, int w, int h, int x, int y, int arm, Rgb c) {
    for (int d = -arm; d <= arm; ++d) {
        if (x + d >= 0 && x + d < w) put(px, w, x + d, y, c);
        if (y + d >= 0 && y + d < h) put(px, w, x, y + d, c);
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t seed = 0x5EED'0BEEF'CAFEull;
    int scale = 1;  // output pixels per tile
    bool rings = false;
    std::string out = "worldmap.png";

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = std::strtoull(argv[++i], nullptr, 0);
        } else if (std::strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            scale = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out = argv[++i];
        } else if (std::strcmp(argv[i], "--rings") == 0) {
            rings = true;
        } else {
            std::printf("usage: %s [--seed N] [--scale N] [--out FILE] [--rings]\n", argv[0]);
            return 2;
        }
    }
    if (scale < 1) scale = 1;

    const int w = kMapTiles * scale;
    const int h = kMapTiles * scale;
    std::vector<unsigned char> px(static_cast<std::size_t>(w) * h * 4, 255);

    // --- terrain + a per-ring tally ------------------------------------------------------------
    long long ring_tiles[kRingCount] = {};
    long long terrain_tiles[static_cast<int>(Terrain::kCount)] = {};
    long long blocked = 0;

    for (int ty = 0; ty < kMapTiles; ++ty) {
        for (int tx = 0; tx < kMapTiles; ++tx) {
            const Terrain t = terrain_of(seed, kOverworld, tx, ty);
            const Ring r = ring_of(seed, tx, ty);
            ++ring_tiles[static_cast<int>(r)];
            ++terrain_tiles[static_cast<int>(t)];
            if (!is_walkable(t)) ++blocked;

            Rgb c = colour_of(t);
            if (rings) {
                // Tint alternate rings very slightly so the bands are visible without hiding
                // the terrain underneath.
                if (static_cast<int>(r) % 2 == 1) {
                    c.r = static_cast<unsigned char>(c.r * 0.88f);
                    c.g = static_cast<unsigned char>(c.g * 0.88f);
                    c.b = static_cast<unsigned char>(c.b * 0.94f);
                }
            }
            for (int sy = 0; sy < scale; ++sy) {
                for (int sx = 0; sx < scale; ++sx) {
                    put(px, w, tx * scale + sx, ty * scale + sy, c);
                }
            }
        }
    }

    // --- markers -------------------------------------------------------------------------------
    cross(px, w, h, kHomeTx * scale, kHomeTy * scale, 6 * scale, Rgb{255, 80, 80});

    Image img{};
    img.data = px.data();
    img.width = w;
    img.height = h;
    img.mipmaps = 1;
    img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    if (!ExportImage(img, out.c_str())) {
        std::printf("failed to write %s\n", out.c_str());
        return 1;
    }

    // --- report --------------------------------------------------------------------------------
    const double total = static_cast<double>(kMapTiles) * kMapTiles;
    std::printf("world map  seed=0x%llX  %dx%d tiles -> %s (%dx%d px)\n\n",
                static_cast<unsigned long long>(seed), kMapTiles, kMapTiles, out.c_str(), w, h);

    std::printf("rings\n");
    for (int i = 0; i < kRingCount; ++i) {
        std::printf("  %-10s %8lld  %5.1f%%\n", kRingNames[i], ring_tiles[i],
                    100.0 * static_cast<double>(ring_tiles[i]) / total);
    }
    static const char* tn[] = {"grass", "dirt",  "water", "stone", "sand",
                               "tree",  "snow",  "marsh", "ash"};
    std::printf("\nterrain\n");
    for (int i = 0; i < static_cast<int>(Terrain::kCount); ++i) {
        std::printf("  %-6s %9lld  %5.1f%%%s\n", tn[i], terrain_tiles[i],
                    100.0 * static_cast<double>(terrain_tiles[i]) / total,
                    (i == static_cast<int>(Terrain::kWater) || i == static_cast<int>(Terrain::kTree))
                        ? "   <-- impassable"
                        : "");
    }
    std::printf("\nimpassable total: %.1f%%\n", 100.0 * static_cast<double>(blocked) / total);
    return 0;
}
