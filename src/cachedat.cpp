#include "ragekit/cachedat.h"
#include <cstdlib>
#include <cstring>
#include "ragekit/error.h"

namespace rk {

namespace {

bool is_module(const std::string& line) { return line == "fwMapDataStore" || line == "CInteriorProxy" || line == "BoundsStore"; }

const CacheDat::Part* module(const CacheDat& c, const char* name) {
    for (const CacheDat::Part& p : c.parts) if (p.module && p.line == name) return &p;
    return nullptr;
}

struct Cur {
    const Bytes& b;
    size_t p = 0;
    template <class T> T get() { T v{}; if (p + sizeof(T) <= b.size()) std::memcpy(&v, b.data() + p, sizeof(T)); p += sizeof(T); return v; }
    Vec3 vec3() { Vec3 v; v.x = get<float>(); v.y = get<float>(); v.z = get<float>(); return v; }
};

}  // namespace

bool is_cache_dat(std::span<const uint8_t> file) { return file.size() >= 100 && std::memcmp(file.data(), "[VERSION]", 9) == 0; }

CacheDat read_cache_dat(std::span<const uint8_t> file) {
    if (!is_cache_dat(file)) throw Error("cache dat: no [VERSION] header");
    CacheDat c;
    c.version_area.assign(file.begin(), file.begin() + 100);
    size_t i = 100;
    std::string line;
    while (i < file.size()) {
        const uint8_t b = file[i];
        if (b == 0) break;
        if (b != '\n') { line.push_back((char)b); i++; continue; }
        i++;
        CacheDat::Part part;
        part.line = line;
        line.clear();
        if (is_module(part.line)) {
            if (i + 4 > file.size()) throw Error("cache dat: module " + part.line + " without its length");
            uint32_t len;
            std::memcpy(&len, file.data() + i, 4);
            if (len > file.size() - i - 4) throw Error("cache dat: module " + part.line + " of " + std::to_string(len) + " bytes runs past the file");
            part.module = true;
            part.payload.assign(file.begin() + (ptrdiff_t)(i + 4), file.begin() + (ptrdiff_t)(i + 4 + len));
            i += 4 + len;
        }
        c.parts.push_back(std::move(part));
    }
    if (!line.empty()) throw Error("cache dat: the last line has no end");
    c.tail.assign(file.begin() + (ptrdiff_t)i, file.end());
    return c;
}

Bytes write_cache_dat(const CacheDat& c) {
    if (c.version_area.size() != 100) throw Error("cache dat: the version area must be 100 bytes");
    Bytes out = c.version_area;
    for (const CacheDat::Part& p : c.parts) {
        out.insert(out.end(), p.line.begin(), p.line.end());
        out.push_back('\n');
        if (!p.module) continue;
        const uint32_t len = (uint32_t)p.payload.size();
        out.insert(out.end(), (const uint8_t*)&len, (const uint8_t*)&len + 4);
        out.insert(out.end(), p.payload.begin(), p.payload.end());
    }
    out.insert(out.end(), c.tail.begin(), c.tail.end());
    return out;
}

std::string CacheDat::version() const {
    std::string s;
    for (uint8_t b : version_area) { if (!b) break; if (b != '\r' && b != '\n') s.push_back((char)b); }
    if (s.rfind("[VERSION]", 0) == 0) s.erase(0, 9);
    return s;
}

std::vector<CacheFileDate> CacheDat::file_dates() const {
    std::vector<CacheFileDate> v;
    bool in = false;
    for (const Part& p : parts) {
        if (p.module) continue;
        if (p.line == "<fileDates>") { in = true; continue; }
        if (p.line == "</fileDates>") { in = false; continue; }
        if (!in) continue;
        CacheFileDate d;
        char* end = nullptr;
        d.name = (uint32_t)std::strtoul(p.line.c_str(), &end, 10);
        d.timestamp = std::strtoll(end, &end, 10);
        d.id = (uint32_t)std::strtoul(end, &end, 10);
        v.push_back(d);
    }
    return v;
}

std::vector<CacheMapNode> CacheDat::map_nodes() const {
    std::vector<CacheMapNode> v;
    if (const Part* m = module(*this, "fwMapDataStore")) {
        Cur c{m->payload};
        while (c.p + 64 <= m->payload.size()) {
            CacheMapNode n;
            n.name = c.get<uint32_t>(); n.parent = c.get<uint32_t>(); n.content_flags = c.get<uint32_t>();
            n.streaming_min = c.vec3(); n.streaming_max = c.vec3(); n.entities_min = c.vec3(); n.entities_max = c.vec3();
            for (uint8_t& f : n.flags) f = c.get<uint8_t>();
            v.push_back(n);
        }
    }
    return v;
}

std::vector<CacheInteriorProxy> CacheDat::interior_proxies() const {
    std::vector<CacheInteriorProxy> v;
    if (const Part* m = module(*this, "CInteriorProxy")) {
        Cur c{m->payload};
        while (c.p + 104 <= m->payload.size()) {
            CacheInteriorProxy x;
            x.unk00 = c.get<uint32_t>(); x.unk04 = c.get<uint32_t>(); x.unk08 = c.get<uint32_t>();
            x.name = c.get<uint32_t>(); x.parent = c.get<uint32_t>();
            x.position = c.vec3();
            x.orientation.x = c.get<float>(); x.orientation.y = c.get<float>(); x.orientation.z = c.get<float>(); x.orientation.w = c.get<float>();
            x.bb_min = c.vec3(); x.bb_max = c.vec3();
            x.unk50 = c.get<uint64_t>(); x.unk58 = c.get<uint64_t>(); x.unk60 = c.get<uint64_t>(); x.unk68 = c.get<uint64_t>();
            v.push_back(x);
        }
    }
    return v;
}

std::vector<CacheBoundsItem> CacheDat::bounds() const {
    std::vector<CacheBoundsItem> v;
    if (const Part* m = module(*this, "BoundsStore")) {
        Cur c{m->payload};
        while (c.p + 32 <= m->payload.size()) {
            CacheBoundsItem b;
            b.name = c.get<uint32_t>(); b.min = c.vec3(); b.max = c.vec3(); b.layer = c.get<uint32_t>();
            v.push_back(b);
        }
    }
    return v;
}

}  // namespace rk
