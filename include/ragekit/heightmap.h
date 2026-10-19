// ragekit - heightmap.dat: the coarse height of the map (`common.rpf/data/levels/gta5/heightmap.dat`, its copy in
// update.rpf, and `heightmapheistisland.dat`), two grids of bytes: the highest and the lowest point of each cell.
//
// Layout (big endian on PC; a file whose magic reads little endian is taken as such): "HMAP", version 1.1, a u16 pad,
// u32 compressed (1 on every game file), u16 width and height (cells), the bounds min and max (Vec3), the data length,
// then per row a (u16 first cell, u16 count, i32 offset) header and the bytes: the rows' highest heights one after the
// other, then their lowest heights in the same places, the second block starting half-way. A cell outside a row's
// stored run is 0 in both grids. Cells are (max - min) / (width, height) in size (50 m in the game's files), row 0 at
// min.y; a byte b is the height min.z + b / 255 * (max.z - min.z). Checked on the map: the cell of Mount Chiliad's
// summit holds 806 / 768 m, the Maze Bank's 326 m over a lowest 26 m, the sea 0.
#pragma once
#include <cstdint>
#include <span>
#include <vector>
#include "types.h"

namespace rk {

struct Heightmap {
    uint8_t version_major = 1, version_minor = 1;
    uint16_t pad = 0;
    bool little_endian = false;          // the game's files are big endian
    uint16_t width = 0, height = 0;      // cells along x and y
    Vec3 min, max;                       // the grid's extent and the height range of a byte
    std::vector<uint8_t> max_heights;    // width * height, row by row from min.y; 0 = no data
    std::vector<uint8_t> min_heights;

    /// The cell holding a world position, false outside the grid.
    bool cell_at(float x, float y, int& cx, int& cy) const;
    /// The height a stored byte stands for.
    float height_of(uint8_t b) const { return min.z + b / 255.0f * (max.z - min.z); }
    /// The lowest and highest heights of the cell holding (x, y); false outside the grid or in a cell with no data.
    bool heights_at(float x, float y, float& lowest, float& highest) const;
};

bool is_heightmap(std::span<const uint8_t> file);
/// Throws rk::Error for a file that is not a heightmap, an uncompressed one (none in the game) or rows outside the data.
Heightmap read_heightmap(std::span<const uint8_t> file);
/// Each row stored from its first to its last cell with a highest height other than 0, the game's own layout: every
/// game file comes back byte for byte. A cell with a lowest height but no highest one outside that run is lost.
Bytes write_heightmap(const Heightmap& h);

}  // namespace rk
