#include "ragekit/heightmap.h"
#include <cmath>
#include <cstring>
#include <string>
#include "ragekit/error.h"

namespace rk {

namespace {

constexpr uint32_t kMagic = 0x484D4150;   // "HMAP"
constexpr size_t kHeader = 44;

struct In {
    std::span<const uint8_t> b;
    bool little;
    uint32_t u32(size_t at) const {
        const uint8_t* p = b.data() + at;
        return little ? (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24
                      : (uint32_t)p[3] | (uint32_t)p[2] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[0] << 24;
    }
    uint16_t u16(size_t at) const {
        const uint8_t* p = b.data() + at;
        return little ? (uint16_t)(p[0] | p[1] << 8) : (uint16_t)(p[1] | p[0] << 8);
    }
    float f32(size_t at) const {
        const uint32_t v = u32(at);
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }
};

struct Out {
    Bytes& b;
    bool little;
    void u32(uint32_t v) {
        for (int i = 0; i < 4; i++) b.push_back((uint8_t)(little ? v >> (8 * i) : v >> (8 * (3 - i))));
    }
    void u16(uint16_t v) {
        for (int i = 0; i < 2; i++) b.push_back((uint8_t)(little ? v >> (8 * i) : v >> (8 * (1 - i))));
    }
    void f32(float f) {
        uint32_t v;
        std::memcpy(&v, &f, 4);
        u32(v);
    }
};

}  // namespace

bool Heightmap::cell_at(float x, float y, int& cx, int& cy) const {
    if (width == 0 || height == 0 || !(max.x > min.x) || !(max.y > min.y)) return false;
    const float fx = (x - min.x) / ((max.x - min.x) / width), fy = (y - min.y) / ((max.y - min.y) / height);
    if (!(fx >= 0 && fy >= 0 && fx < width && fy < height)) return false;
    cx = (int)fx;
    cy = (int)fy;
    return true;
}

bool Heightmap::heights_at(float x, float y, float& lowest, float& highest) const {
    int cx, cy;
    if (!cell_at(x, y, cx, cy)) return false;
    const size_t i = (size_t)cy * width + cx;
    if (i >= max_heights.size() || i >= min_heights.size() || max_heights[i] == 0) return false;
    lowest = height_of(min_heights[i]);
    highest = height_of(max_heights[i]);
    return true;
}

bool is_heightmap(std::span<const uint8_t> file) {
    if (file.size() < 4) return false;
    return In{file, false}.u32(0) == kMagic || In{file, true}.u32(0) == kMagic;
}

Heightmap read_heightmap(std::span<const uint8_t> file) {
    if (!is_heightmap(file)) throw Error("heightmap: no HMAP magic");
    if (file.size() < kHeader) throw Error("heightmap: a file of " + std::to_string(file.size()) + " bytes holds no header");
    const In in{file, In{file, true}.u32(0) == kMagic};
    Heightmap h;
    h.little_endian = in.little;
    h.version_major = file[4];
    h.version_minor = file[5];
    h.pad = in.u16(6);
    if (in.u32(8) == 0) throw Error("heightmap: an uncompressed heightmap (none in the game) is not read");
    h.width = in.u16(12);
    h.height = in.u16(14);
    h.min = {in.f32(16), in.f32(20), in.f32(24)};
    h.max = {in.f32(28), in.f32(32), in.f32(36)};
    const uint32_t length = in.u32(40);
    if (length != file.size() - kHeader) throw Error("heightmap: the data length " + std::to_string(length) + " is not the rest of the file");
    const size_t rows = (size_t)h.height * 8;
    if (rows > length) throw Error("heightmap: " + std::to_string(h.height) + " row headers do not fit in " + std::to_string(length) + " bytes");
    const size_t data_at = kHeader + rows, data_len = length - rows, half = data_len / 2;
    const size_t cells = (size_t)h.width * h.height;
    h.max_heights.assign(cells, 0);
    h.min_heights.assign(cells, 0);
    for (size_t y = 0; y < h.height; y++) {
        const size_t at = kHeader + y * 8;
        const uint16_t first = in.u16(at), count = in.u16(at + 2);
        const int32_t offset = (int32_t)in.u32(at + 4);
        if ((size_t)first + count > h.width) throw Error("heightmap: row " + std::to_string(y) + " runs past the grid's width");
        if (count == 0) continue;
        const int64_t lo = (int64_t)offset + first, hi = lo + count;
        if (lo < 0 || (size_t)hi > half) throw Error("heightmap: row " + std::to_string(y) + " lies outside the data");
        std::memcpy(&h.max_heights[y * h.width + first], file.data() + data_at + lo, count);
        std::memcpy(&h.min_heights[y * h.width + first], file.data() + data_at + half + lo, count);
    }
    return h;
}

Bytes write_heightmap(const Heightmap& h) {
    const size_t cells = (size_t)h.width * h.height;
    if (h.max_heights.size() != cells || h.min_heights.size() != cells) throw Error("heightmap: the grids do not hold width * height cells");
    struct Row { uint16_t first = 0, count = 0; int32_t offset = 0; };
    std::vector<Row> rows(h.height);
    Bytes highest, lowest;
    for (size_t y = 0; y < h.height; y++) {
        const uint8_t* m = &h.max_heights[y * h.width];
        size_t first = 0, end = 0;
        for (size_t x = 0; x < h.width; x++) if (m[x]) { first = x; break; }
        for (size_t x = h.width; x-- > 0;) if (m[x]) { end = x + 1; break; }
        Row& r = rows[y];
        if (end > first) {
            r.first = (uint16_t)first;
            r.count = (uint16_t)(end - first);
            r.offset = (int32_t)highest.size() - (int32_t)first;
            highest.insert(highest.end(), m + first, m + end);
            lowest.insert(lowest.end(), &h.min_heights[y * h.width + first], &h.min_heights[y * h.width + end]);
        }
    }
    Bytes b;
    b.reserve(kHeader + rows.size() * 8 + highest.size() + lowest.size());
    Out out{b, h.little_endian};
    out.u32(kMagic);
    out.u16((uint16_t)(h.little_endian ? h.version_major | h.version_minor << 8 : h.version_major << 8 | h.version_minor));
    out.u16(h.pad);
    out.u32(1);
    out.u16(h.width);
    out.u16(h.height);
    out.f32(h.min.x); out.f32(h.min.y); out.f32(h.min.z);
    out.f32(h.max.x); out.f32(h.max.y); out.f32(h.max.z);
    out.u32((uint32_t)(rows.size() * 8 + highest.size() + lowest.size()));
    for (const Row& r : rows) {
        out.u16(r.first);
        out.u16(r.count);
        out.u32((uint32_t)r.offset);
    }
    b.insert(b.end(), highest.begin(), highest.end());
    b.insert(b.end(), lowest.begin(), lowest.end());
    return b;
}

}  // namespace rk
