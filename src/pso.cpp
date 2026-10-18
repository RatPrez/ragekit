#include "ragekit/pso.h"
#include "ragekit/hash.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace rk {

namespace {

#include "pso_names.inc"

[[noreturn]] void fail(const char* fmt, uint64_t a = 0, uint64_t b = 0) {
    char buf[220];
    std::snprintf(buf, sizeof buf, fmt, (unsigned long long)a, (unsigned long long)b);
    throw Error(buf);
}

// --- big endian helpers -------------------------------------------------------------------------------------------

inline uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
inline uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline uint64_t be64(const uint8_t* p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }

struct Out {
    Bytes b;
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint16_t v) { b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v)); }
    void u32(uint32_t v) { for (int s = 24; s >= 0; s -= 8) b.push_back(uint8_t(v >> s)); }
    void bytes(const Bytes& v) { b.insert(b.end(), v.begin(), v.end()); }
};

struct In {
    const uint8_t* p; size_t n; size_t pos = 0;
    In(const uint8_t* p_, size_t n_) : p(p_), n(n_) {}
    void need(size_t k) const { if (pos + k > n) fail("pso: section runs past its end (%llu of %llu)", pos + k, n); }
    uint8_t u8() { need(1); return p[pos++]; }
    uint16_t u16() { need(2); auto v = be16(p + pos); pos += 2; return v; }
    uint32_t u32() { need(4); auto v = be32(p + pos); pos += 4; return v; }
};

// --- PMAP -----------------------------------------------------------------------------------------------------------

Bytes encode_map(const PsoDataMap& m) {
    Out o;
    o.u32((uint32_t)m.root_id);
    if (!m.extended) {
        o.u16((uint16_t)m.blocks.size());
        o.u16(m.unknown);
    } else {
        o.u16(0);
        o.u16(m.unknown);
        o.u16(m.ext[0]); o.u16(m.ext[1]); o.u16(m.ext[2]); o.u16(m.ext[3]);
    }
    for (auto& b : m.blocks) { o.u32(b.name); o.u32((uint32_t)b.offset); o.u32((uint32_t)b.unknown); o.u32((uint32_t)b.length); }
    return o.b;
}
PsoDataMap decode_map(const Bytes& payload) {
    In in(payload.data(), payload.size());
    PsoDataMap m;
    m.root_id = (int32_t)in.u32();
    int16_t count = (int16_t)in.u16();
    m.unknown = in.u16();
    size_t n = (size_t)(count < 0 ? 0 : count);
    if (count <= 0) {
        m.extended = true;
        for (auto& e : m.ext) e = in.u16();
        n = m.ext[0];
    }
    for (size_t i = 0; i < n; i++) {
        PsoBlock b;
        b.name = in.u32(); b.offset = (int32_t)in.u32(); b.unknown = (int32_t)in.u32(); b.length = (int32_t)in.u32();
        m.blocks.push_back(b);
    }
    return m;
}

// --- PSCH -----------------------------------------------------------------------------------------------------------

Bytes encode_schema(const PsoSchema& s) {
    // index (name, offset) for every element, then the elements back to back
    Out body;
    std::vector<uint32_t> offsets;
    size_t base = 4 + 8 * s.elements.size(); // payload offsets: the count, then the index
    for (auto& e : s.elements) {
        offsets.push_back(uint32_t(8 + base + body.b.size())); // offsets count from the section start (ident + length = 8)
        if (!e.is_enum) {
            body.u32(uint32_t(e.st.kind) << 24 | uint32_t(e.st.unknown) << 16 | uint16_t(e.st.entries.size()));
            body.u32((uint32_t)e.st.size);
            body.u32(e.st.unknown_c);
            for (auto& x : e.st.entries) {
                body.u32(x.name); body.u8(x.type); body.u8(x.subtype); body.u16(x.offset); body.u32(x.ref_key);
            }
        } else {
            body.u32(uint32_t(e.en.kind) << 24 | uint32_t(e.en.entries.size() & 0xFFFFFF));
            for (auto& x : e.en.entries) { body.u32(x.name); body.u32((uint32_t)x.value); }
        }
    }
    Out o;
    o.u32((uint32_t)s.elements.size());
    for (size_t i = 0; i < s.elements.size(); i++) { o.u32(s.elements[i].name); o.u32(offsets[i]); }
    o.bytes(body.b);
    return o.b;
}
PsoSchema decode_schema(const Bytes& payload) {
    PsoSchema s;
    In idx(payload.data(), payload.size());
    uint32_t count = idx.u32();
    if (count > 100000) fail("pso: schema with %llu elements", count);
    std::vector<std::pair<uint32_t, uint32_t>> index;
    for (uint32_t i = 0; i < count; i++) { uint32_t n = idx.u32(); uint32_t o = idx.u32(); index.push_back({n, o}); }
    // the elements are one after the other in the payload: an index whose entries all name one element would decode it
    // count times (a 65535-entry structure, 100000 times), so the entries' bytes are held to the payload's
    uint64_t entry_budget = payload.size();
    auto spend = [&](uint64_t n) {
        if (n > entry_budget) fail("pso: the schema's elements name more bytes than it holds (%llu)", (unsigned long long)payload.size());
        entry_budget -= n;
    };
    for (auto& ix : index) {
        if (ix.second < 8) fail("pso: schema element offset %llu inside the header", ix.second);
        In in(payload.data(), payload.size());
        in.pos = ix.second - 8;
        PsoElement e;
        e.name = ix.first;
        uint32_t x = in.u32();
        uint8_t kind = uint8_t(x >> 24);
        if (kind == 0) {
            e.st.kind = kind;
            e.st.unknown = uint8_t(x >> 16);
            uint16_t cnt = uint16_t(x);
            spend(12 + 12ull * cnt);
            e.st.size = (int32_t)in.u32();
            e.st.unknown_c = in.u32();
            for (uint16_t k = 0; k < cnt; k++) {
                PsoStructEntry en;
                en.name = in.u32(); en.type = in.u8(); en.subtype = in.u8(); en.offset = in.u16(); en.ref_key = in.u32();
                e.st.entries.push_back(en);
            }
        } else if (kind == 1) {
            e.is_enum = true;
            e.en.kind = kind;
            uint32_t cnt = x & 0xFFFFFF;
            spend(4 + 8ull * cnt);
            for (uint32_t k = 0; k < cnt; k++) { PsoEnumEntry en; en.name = in.u32(); en.value = (int32_t)in.u32(); e.en.entries.push_back(en); }
        } else fail("pso: unknown schema element kind %llu", kind);
        s.elements.push_back(std::move(e));
    }
    return s;
}

Bytes encode_checks(const PsoChecks& c) {
    Out o;
    o.u32(c.file_size); o.u32(c.checksum); o.u32(c.tag);
    return o.b;
}

} // namespace

// --- names / small helpers -------------------------------------------------------------------------------------------

std::string pso_name(uint32_t h) {
    auto* b = std::begin(kGenNames); auto* e = std::end(kGenNames);
    auto it = std::lower_bound(b, e, h, [](const GenName& g, uint32_t v) { return g.hash < v; });
    if (it != e && it->hash == h) return it->name;
    return hash_name(h);
}

const char* psotype::name(uint8_t t) {
    switch (t) {
    case Bool: return "bool"; case SByte: return "s8"; case UByte: return "u8"; case SShort: return "s16"; case UShort: return "u16";
    case SInt: return "s32"; case UInt: return "u32"; case Float: return "float"; case Float2: return "vec2"; case Float3: return "vec3";
    case Float4: return "vec4"; case String: return "string"; case Structure: return "struct"; case Array: return "array";
    case Enum: return "enum"; case Flags: return "flags"; case Map: return "map"; case Float3a: return "vec3a"; case Float4a: return "vec4a";
    case HFloat: return "half"; case Long: return "s64";
    default: return "?";
    }
}

const PsoStructInfo* PsoSchema::find_struct(uint32_t name) const {
    for (auto& e : elements) if (!e.is_enum && e.name == name) return &e.st;
    return nullptr;
}
const PsoEnumInfo* PsoSchema::find_enum(uint32_t name) const {
    for (auto& e : elements) if (e.is_enum && e.name == name) return &e.en;
    return nullptr;
}

PsoSection* Pso::find_section(uint32_t ident) {
    for (auto& s : sections) if (s.ident == ident) return &s;
    return nullptr;
}
const PsoSection* Pso::find_section(uint32_t ident) const {
    for (auto& s : sections) if (s.ident == ident) return &s;
    return nullptr;
}
Bytes& Pso::data() {
    if (auto* s = find_section(kPsoPSIN)) return s->payload;
    throw Error("pso: no PSIN section");
}
const Bytes& Pso::data() const {
    if (auto* s = find_section(kPsoPSIN)) return s->payload;
    throw Error("pso: no PSIN section");
}
const PsoBlock* Pso::block(uint32_t id) const {
    if (!has_map || id < 1 || id > map.blocks.size()) return nullptr;
    return &map.blocks[id - 1];
}
std::vector<std::string> Pso::strings(uint32_t ident) const {
    std::vector<std::string> r;
    auto* s = find_section(ident);
    if (!s) return r;
    std::string cur;
    for (uint8_t c : s->payload) { if (c) cur.push_back((char)c); else { r.push_back(cur); cur.clear(); } }
    if (!cur.empty()) r.push_back(cur);
    return r;
}

bool is_pso(std::span<const uint8_t> f) { return f.size() >= 4 && f[0] == 'P' && f[1] == 'S' && f[2] == 'I' && f[3] == 'N'; }

// --- read / write ---------------------------------------------------------------------------------------------------

Pso read_pso(std::span<const uint8_t> file) {
    if (!is_pso(file)) throw Error("pso: not a PSO file (no PSIN magic)");
    Pso p;
    size_t pos = 0;
    while (pos + 8 <= file.size()) {
        uint32_t ident = be32(file.data() + pos), len = be32(file.data() + pos + 4);
        // a section that does not fit ends the list: keep the rest as trailing bytes
        if (len < 8 || pos + len > file.size()) break;
        // all idents are four upper case letters; anything else is padding or garbage
        bool ascii = true;
        for (int s = 0; s < 32; s += 8) { uint8_t c = uint8_t(ident >> s); if (c < 'A' || c > 'Z') ascii = false; }
        if (!ascii) break;
        PsoSection sec;
        sec.ident = ident;
        sec.payload.assign(file.begin() + pos + 8, file.begin() + pos + len);
        p.sections.push_back(std::move(sec));
        pos += len;
    }
    p.trailing.assign(file.begin() + pos, file.end());
    if (!p.find_section(kPsoPSIN)) fail("pso: no PSIN section");

    // parsed views; a section keeps `parsed` only when re-encoding gives back its payload
    for (auto& s : p.sections) {
        try {
            if (s.ident == kPsoPMAP && !p.has_map) {
                PsoDataMap m = decode_map(s.payload);
                if (encode_map(m) == s.payload) { p.map = std::move(m); p.has_map = true; s.parsed = true; }
                else { p.map = decode_map(s.payload); p.has_map = true; } // usable for walking, written verbatim
            } else if (s.ident == kPsoPSCH && !p.has_schema) {
                PsoSchema sc = decode_schema(s.payload);
                bool same = encode_schema(sc) == s.payload;
                p.schema = std::move(sc); p.has_schema = true; s.parsed = same;
            } else if (s.ident == kPsoCHKS && !p.has_checks && s.payload.size() == 12) {
                p.checks.file_size = be32(s.payload.data()); p.checks.checksum = be32(s.payload.data() + 4); p.checks.tag = be32(s.payload.data() + 8);
                p.has_checks = true; s.parsed = true;
            }
        } catch (const Error&) {
            // leave it as raw payload; has_map / has_schema stay false
        }
    }
    return p;
}

Bytes write_pso(const Pso& p) {
    Out o;
    for (auto& s : p.sections) {
        Bytes payload;
        if (s.parsed && s.ident == kPsoPMAP) payload = encode_map(p.map);
        else if (s.parsed && s.ident == kPsoPSCH) payload = encode_schema(p.schema);
        else if (s.parsed && s.ident == kPsoCHKS) payload = encode_checks(p.checks);
        else payload = s.payload;
        o.u32(s.ident);
        o.u32(uint32_t(payload.size() + 8));
        o.bytes(payload);
    }
    o.bytes(p.trailing);
    return o.b;
}

uint32_t pso_checksum(std::span<const uint8_t> file) {
    // find CHKS and zero its size + checksum while hashing
    size_t chk = 0;
    size_t pos = 0;
    while (pos + 8 <= file.size()) {
        uint32_t ident = be32(file.data() + pos), len = be32(file.data() + pos + 4);
        if (len < 8 || pos + len > file.size()) break;
        if (ident == kPsoCHKS) { chk = pos; break; }
        pos += len;
    }
    uint32_t h = 0x3FAC7125;
    for (size_t i = 0; i < file.size(); i++) {
        uint8_t c = file[i];
        if (chk && i >= chk + 8 && i < chk + 16) c = 0;
        h += (uint32_t)(int32_t)(int8_t)c;
        h += h << 10;
        h ^= h >> 6;
    }
    h += h << 3; h ^= h >> 11; h += h << 15;
    return h;
}

void Pso::update_checksum() {
    PsoSection* s = find_section(kPsoCHKS);
    if (!s) return;
    has_checks = true; s->parsed = true;
    checks.file_size = 0; checks.checksum = 0;
    Bytes tmp = write_pso(*this);
    checks.file_size = (uint32_t)tmp.size();
    checks.checksum = pso_checksum(tmp);
}

std::string pso_describe(const Pso& p, bool verbose) {
    std::string r;
    char buf[300];
    for (auto& s : p.sections) {
        std::snprintf(buf, sizeof buf, "  section %c%c%c%c  %zu bytes%s\n", char(s.ident >> 24), char(s.ident >> 16), char(s.ident >> 8), char(s.ident),
                      s.payload.size() + 8, s.parsed ? "" : "  (raw)");
        r += buf;
    }
    if (!p.trailing.empty()) { std::snprintf(buf, sizeof buf, "  trailing %zu bytes\n", p.trailing.size()); r += buf; }
    if (p.has_map) {
        std::snprintf(buf, sizeof buf, "  map: root block %d, %zu blocks%s\n", p.map.root_id, p.map.blocks.size(), p.map.extended ? " (extended header)" : "");
        r += buf;
        if (verbose) {
            int i = 1;
            for (auto& b : p.map.blocks) {
                std::snprintf(buf, sizeof buf, "    block %d: %s offset %d length %d unk %d\n", i++, pso_name(b.name).c_str(), b.offset, b.length, b.unknown);
                r += buf;
            }
        }
    }
    if (p.has_checks) {
        std::snprintf(buf, sizeof buf, "  chks: size %u checksum 0x%08X tag 0x%08X\n", p.checks.file_size, p.checks.checksum, p.checks.tag);
        r += buf;
    }
    if (p.has_schema) {
        size_t ns = 0, ne = 0;
        for (auto& e : p.schema.elements) (e.is_enum ? ne : ns)++;
        std::snprintf(buf, sizeof buf, "  schema: %zu structure infos, %zu enum infos\n", ns, ne);
        r += buf;
        if (verbose) {
            for (auto& e : p.schema.elements) {
                if (!e.is_enum) {
                    std::snprintf(buf, sizeof buf, "    struct %s size %d unk %u, %zu entries\n", pso_name(e.name).c_str(), e.st.size, e.st.unknown, e.st.entries.size());
                    r += buf;
                    for (auto& x : e.st.entries) {
                        std::snprintf(buf, sizeof buf, "      +%-4u %-28s %-6s sub %-3u ref 0x%08X (%s)\n", x.offset, pso_name(x.name).c_str(), psotype::name(x.type), x.subtype, x.ref_key,
                                      x.ref_key < 0x10000 ? "" : pso_name(x.ref_key).c_str());
                        r += buf;
                    }
                } else {
                    std::snprintf(buf, sizeof buf, "    enum %s, %zu entries\n", pso_name(e.name).c_str(), e.en.entries.size());
                    r += buf;
                    for (auto& x : e.en.entries) {
                        std::snprintf(buf, sizeof buf, "      %-28s = %d\n", pso_name(x.name).c_str(), x.value);
                        r += buf;
                    }
                }
            }
        }
    }
    for (auto id : {kPsoSTRF, kPsoSTRS}) {
        auto v = p.strings(id);
        if (v.empty()) continue;
        std::snprintf(buf, sizeof buf, "  %s strings: %zu\n", id == kPsoSTRF ? "STRF" : "STRS", v.size());
        r += buf;
        if (verbose) for (auto& s : v) r += "    " + s + "\n";
    }
    return r;
}

} // namespace rk

