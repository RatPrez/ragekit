#include "ragekit/gfx.h"
#include <cstring>
#include "ragekit/error.h"
#include "ragekit/rsc.h"

namespace rk {

namespace {

uint32_t adler32(std::span<const uint8_t> d) {
    uint32_t a = 1, b = 0;
    for (uint8_t c : d) { a = (a + c) % 65521; b = (b + a) % 65521; }
    return b << 16 | a;
}

}  // namespace

float GfxMovie::frame_rate() const { return frame.size() >= 4 ? frame[frame.size() - 4] / 256.f + frame[frame.size() - 3] : 0.f; }
uint16_t GfxMovie::frame_count() const { return frame.size() >= 4 ? (uint16_t)(frame[frame.size() - 2] | frame[frame.size() - 1] << 8) : 0; }

std::vector<std::string> GfxMovie::exports() const {
    std::vector<std::string> out;
    for (const GfxTag& t : tags) {
        if (t.code != 56 || t.payload.size() < 2) continue;
        const size_t n = (size_t)(t.payload[0] | t.payload[1] << 8);
        size_t p = 2;
        for (size_t i = 0; i < n && p + 2 <= t.payload.size(); i++) {
            p += 2;   // the symbol's id
            const size_t start = p;
            while (p < t.payload.size() && t.payload[p]) p++;
            out.emplace_back((const char*)t.payload.data() + start, p - start);
            p++;
        }
    }
    return out;
}

bool is_gfx(std::span<const uint8_t> file) {
    return file.size() >= 8 && (file[0] == 'G' || file[0] == 'C') && file[1] == 'F' && file[2] == 'X';
}

GfxMovie read_gfx(std::span<const uint8_t> file) {
    if (!is_gfx(file)) throw Error("gfx: no GFX / CFX signature");
    GfxMovie m;
    m.compressed = file[0] == 'C';
    m.version = file[3];
    uint32_t length;
    std::memcpy(&length, file.data() + 4, 4);
    Bytes body;
    std::span<const uint8_t> b;
    if (m.compressed) {
        if (file.size() < 10) throw Error("gfx: a compressed movie without its stream");
        body = rsc::inflate(file.subspan(10), length > 8 ? length - 8 : 0);   // after the 2-byte zlib header
        b = body;
    } else {
        if (length != file.size()) throw Error("gfx: the header says " + std::to_string(length) + " bytes, the file has " + std::to_string(file.size()));
        b = file.subspan(8);
    }
    if (b.empty()) throw Error("gfx: no frame rectangle");
    const size_t nbits = b[0] >> 3, rect = (5 + 4 * nbits + 7) / 8;
    if (rect + 4 > b.size()) throw Error("gfx: the frame rectangle runs past the file");
    m.frame.assign(b.begin(), b.begin() + rect + 4);
    size_t p = rect + 4;
    while (p < b.size()) {
        if (p + 2 > b.size()) throw Error("gfx: a tag header cut short at byte " + std::to_string(p + 8));
        const uint16_t h = (uint16_t)(b[p] | b[p + 1] << 8);
        p += 2;
        GfxTag t;
        t.code = h >> 6;
        uint32_t len = h & 0x3F;
        if (len == 0x3F) {
            if (p + 4 > b.size()) throw Error("gfx: a long tag length cut short");
            std::memcpy(&len, b.data() + p, 4);
            p += 4;
            t.long_form = true;
        }
        if (len > b.size() - p) throw Error("gfx: tag " + std::to_string(t.code) + " of " + std::to_string(len) + " bytes runs past the file");
        t.payload.assign(b.begin() + p, b.begin() + p + len);
        p += len;
        m.tags.push_back(std::move(t));
    }
    return m;
}

Bytes write_gfx(const GfxMovie& m) {
    Bytes body = m.frame;
    for (const GfxTag& t : m.tags) {
        if (t.code > 0x3FF) throw Error("gfx: a tag code past 1023");
        const bool lf = t.long_form || t.payload.size() >= 0x3F;
        const uint16_t h = (uint16_t)(t.code << 6 | (lf ? 0x3F : t.payload.size()));
        body.push_back((uint8_t)h);
        body.push_back((uint8_t)(h >> 8));
        if (lf) {
            const uint32_t len = (uint32_t)t.payload.size();
            body.insert(body.end(), (const uint8_t*)&len, (const uint8_t*)&len + 4);
        }
        body.insert(body.end(), t.payload.begin(), t.payload.end());
    }
    Bytes out = {(uint8_t)(m.compressed ? 'C' : 'G'), 'F', 'X', m.version, 0, 0, 0, 0};
    const uint32_t length = (uint32_t)(body.size() + 8);
    std::memcpy(out.data() + 4, &length, 4);
    if (!m.compressed) {
        out.insert(out.end(), body.begin(), body.end());
        return out;
    }
    out.push_back(0x78);
    out.push_back(0xDA);
    const Bytes z = rsc::deflate(body);
    out.insert(out.end(), z.begin(), z.end());
    const uint32_t ad = adler32(body);
    for (int s = 24; s >= 0; s -= 8) out.push_back((uint8_t)(ad >> s));
    return out;
}

}  // namespace rk
