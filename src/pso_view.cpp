// PsoView: typed, schema driven access to the data of a PSO, and the generic text dump.
// Layout rules come from CodeWalker (PsoXml / PsoTypes) and were checked against real files.
#include "ragekit/pso.h"
#include "ragekit/hash.h"
#include <cstdio>
#include <cstring>

namespace rk {

namespace {

using namespace psotype;

constexpr uint32_t kMaxDepth = 40;

inline uint64_t be(const Bytes& d, size_t pos, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v = v << 8 | d[pos + i];
    return v;
}
inline bool fits(const Bytes& d, size_t pos, size_t n) { return pos <= d.size() && n <= d.size() - pos; }
inline float as_float(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }

struct Ptr {
    uint32_t block = 0, offset = 0;
    bool null() const { return block == 0; }
};
// An 8 byte pointer field is two big endian words: the first is block id | offset << 12, the second is unused (zero).
// (CodeWalker's whole-ulong byte swap puts the first word in the high half, hence p >> 32 here.)
inline Ptr decode_ptr(uint64_t p) { uint32_t w = uint32_t(p >> 32); return {w & 0xFFF, (w >> 12) & 0xFFFFF}; }

/// Position in data() of (block, offset), applying CodeWalker's "offset >> 8" fallback for offsets past the block.
bool locate(const Pso& p, Ptr ptr, size_t& pos, const PsoBlock** blk = nullptr) {
    const PsoBlock* b = p.block(ptr.block);
    if (!b) return false;
    uint32_t off = ptr.offset;
    if ((int64_t)off >= b->length) off >>= 8;
    int64_t at = int64_t(b->offset) + off - 8;
    if (at < 0 || (size_t)at > p.data().size()) return false;
    pos = (size_t)at;
    if (blk) *blk = b;
    return true;
}

/// Bytes one element of an array occupies.
size_t element_stride(const Pso& p, const PsoStructEntry& el) {
    switch (el.type) {
    case Bool: case SByte: case UByte: return 1;
    case SShort: case UShort: case HFloat: return 2;
    case SInt: case UInt: case Float: return 4;
    case Enum: return el.subtype == 2 ? 1 : (el.subtype == 1 ? 2 : 4);
    case Flags: return el.subtype == 2 ? 1 : (el.subtype == 1 ? 2 : 4);
    case Float2: case Long: return 8;
    case Float3: case Float3a: case Float4a: case Float4: return 16;
    case String:
        switch (el.subtype) { case 7: case 8: return 4; case 3: return 16; case 0: return (el.ref_key >> 16) ? (el.ref_key >> 16) : 0; default: return 8; }
    case Structure:
        if (el.subtype == 3 || el.subtype == 4) return 8;
        if (auto* s = p.schema.find_struct(el.ref_key)) return (size_t)s->size;
        return 0;
    case Array: return 16;
    default: return 0;
    }
}

std::string fmt_float(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.9g", v);
    return buf;
}
std::string quote(const std::string& s) {
    std::string r = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { r.push_back('\\'); r.push_back(c); }
        else if ((unsigned char)c < 32) { char b[8]; std::snprintf(b, sizeof b, "\\x%02X", (unsigned)(unsigned char)c); r += b; }
        else r.push_back(c);
    }
    return r + "\"";
}

} // namespace

PsoView::PsoView(const Pso& p) : pso_(p) {
    for (uint32_t id : {kPsoSTRF, kPsoSTRS})
        for (auto& s : p.strings(id)) { names_.emplace(joaat_lower(s), s); names_.emplace(joaat(s), s); }
}
std::string PsoView::name(uint32_t h) const {
    if (h == 0) return {};
    auto it = names_.find(h);
    if (it != names_.end()) return it->second;
    return pso_name(h);
}

// --- PsoView --------------------------------------------------------------------------------------------------------

PsoObject PsoView::at(uint32_t block_id, uint32_t offset, uint32_t type) const {
    PsoObject o;
    if (!pso_.has_map || !pso_.has_schema) return o;
    size_t pos;
    const PsoBlock* b = nullptr;
    if (!locate(pso_, {block_id, offset}, pos, &b)) return o;
    o.view = this;
    o.type = type ? type : b->name;
    o.pos = pos;
    o.info = pso_.schema.find_struct(o.type);
    return o;
}
PsoObject PsoView::follow(uint64_t pointer, uint32_t type) const {
    Ptr p = decode_ptr(pointer);
    if (p.null()) return {};
    // the block's own name wins when it has a structure info (the pointer may point at a derived type)
    PsoObject o = at(p.block, p.offset, 0);
    if (o && o.info) return o;
    if (type) return at(p.block, p.offset, type);
    return o;
}
PsoObject PsoView::root() const {
    if (!pso_.has_map || !pso_.has_schema) return {};
    return at((uint32_t)pso_.map.root_id, 0, 0);
}

std::vector<PsoField> PsoView::fields(const PsoObject& o) const {
    std::vector<PsoField> r;
    if (!o.info) return r;
    for (auto& e : o.info->entries) {
        if (e.name == kArrayInfo) continue;
        PsoField f;
        f.owner = o; f.entry = &e; f.pos = o.pos + e.offset;
        r.push_back(f);
    }
    return r;
}
const PsoField* PsoView::find(const PsoObject& o, uint32_t name, PsoField& storage) const {
    if (!o.info) return nullptr;
    for (auto& e : o.info->entries) {
        if (e.name != name || e.name == kArrayInfo) continue;
        storage.owner = o; storage.entry = &e; storage.pos = o.pos + e.offset;
        return &storage;
    }
    return nullptr;
}

bool PsoView::get_int(const PsoField& f, int64_t& v) const {
    if (!f.entry) return false;
    const Bytes& d = pso_.data();
    size_t pos = f.pos;
    auto rdn = [&](int n, bool sign) -> bool {
        if (!fits(d, pos, n)) return false;
        uint64_t u = be(d, pos, n);
        if (sign) { int sh = 64 - 8 * n; v = int64_t(u << sh) >> sh; } else v = (int64_t)u;
        return true;
    };
    switch (f.entry->type) {
    case Bool: case UByte: return rdn(1, false);
    case SByte: return rdn(1, true);
    case SShort: case HFloat: return rdn(2, true);
    case UShort: return rdn(2, false);
    case SInt: return rdn(4, true);
    case UInt: return rdn(4, f.entry->subtype != 0 ? false : true);
    case Long: return rdn(8, true);
    case Enum: return f.entry->subtype == 2 ? rdn(1, false) : (f.entry->subtype == 1 ? rdn(2, false) : rdn(4, true));
    case Flags: return f.entry->subtype == 2 ? rdn(1, false) : (f.entry->subtype == 1 ? rdn(2, false) : rdn(4, false));
    case String: if (f.entry->subtype == 7 || f.entry->subtype == 8) return rdn(4, false); return false;
    default: return false;
    }
}
bool PsoView::get_float(const PsoField& f, double& v) const {
    if (!f.entry) return false;
    const Bytes& d = pso_.data();
    if (f.entry->type == Float && fits(d, f.pos, 4)) { v = as_float((uint32_t)be(d, f.pos, 4)); return true; }
    if (f.entry->type == HFloat && fits(d, f.pos, 2)) { v = half_to_float((uint16_t)be(d, f.pos, 2)); return true; }
    return false;
}
bool PsoView::get_vec(const PsoField& f, float out[4], int& n) const {
    if (!f.entry) return false;
    switch (f.entry->type) {
    case Float2: n = 2; break;
    case Float3: case Float3a: case Float4a: n = 3; break;
    case Float4: n = 4; break;
    default: return false;
    }
    const Bytes& d = pso_.data();
    if (!fits(d, f.pos, 4 * n)) return false;
    for (int i = 0; i < n; i++) out[i] = as_float((uint32_t)be(d, f.pos + 4 * i, 4));
    return true;
}

bool PsoView::string_at(const PsoStructEntry& e, size_t pos, std::string& s) const {
    const Bytes& d = pso_.data();
    s.clear();
    auto cstr_at = [&](Ptr p) -> bool {
        size_t at;
        if (p.null() || !locate(pso_, p, at)) return false;
        while (at < d.size() && d[at]) s.push_back((char)d[at++]);
        return true;
    };
    switch (e.subtype) {
    case 0: {
        size_t len = e.ref_key >> 16;
        if (!fits(d, pos, len)) return false;
        for (size_t i = 0; i < len; i++) if (d[pos + i]) s.push_back((char)d[pos + i]);
        return true;
    }
    case 1: case 2:
        if (!fits(d, pos, 8)) return false;
        cstr_at(decode_ptr(be(d, pos, 8)));
        return true;
    case 3: {
        if (!fits(d, pos, 16)) return false;
        Ptr p = decode_ptr(be(d, pos, 8));
        uint32_t c1 = (uint32_t)be(d, pos + 8, 2), c2 = (uint32_t)be(d, pos + 10, 2);
        uint32_t len = c1;
        if (c2 != 0 && c2 < len) len = c2;
        size_t at;
        const PsoBlock* b = nullptr;
        if (c1 == 0 || p.null() || !locate(pso_, p, at, &b)) return true; // empty
        if (!fits(d, at, len)) return false;
        s.assign((const char*)d.data() + at, len);
        return true;
    }
    case 7: case 8:
        if (!fits(d, pos, 4)) return false;
        s = name((uint32_t)be(d, pos, 4));
        return true;
    default: return false;
    }
}
bool PsoView::get_string(const PsoField& f, std::string& s) const {
    if (!f.entry || f.entry->type != String) return false;
    return string_at(*f.entry, f.pos, s);
}

PsoObject PsoView::get_struct(const PsoField& f) const {
    if (!f.entry || f.entry->type != Structure) return {};
    if (f.entry->subtype == 0) {
        PsoObject o;
        o.view = this; o.type = f.entry->ref_key; o.pos = f.pos;
        o.info = pso_.schema.find_struct(o.type);
        return o;
    }
    const Bytes& d = pso_.data();
    if (!fits(d, f.pos, 8)) return {};
    return follow(be(d, f.pos, 8), f.entry->ref_key);
}

PsoArrayRef PsoView::array_at(const PsoObject& owner, const PsoStructEntry& e, size_t pos) const {
    PsoArrayRef a;
    if (e.type != Array || !owner.info) return a;
    const auto& entries = owner.info->entries;
    size_t idx = e.ref_key & 0xFFFF;
    if (idx >= entries.size()) idx = e.ref_key & 0xFFF;
    if (idx >= entries.size()) return a;
    a.element = &entries[idx];
    const PsoStructEntry& el = *a.element;
    uint32_t embedded = e.ref_key >> 16;
    a.pointers = el.type == Structure && el.subtype == 3;
    bool header;
    switch (e.subtype) {
    case 0: header = true; break;
    case 4: header = a.pointers; break;
    case 1: case 2: case 129: header = false; break;
    default: return a;
    }
    const Bytes& d = pso_.data();
    a.stride = a.pointers ? 8 : element_stride(pso_, el);
    if (!header) {
        a.pos = pos; a.count = embedded; a.valid = true;
        return a;
    }
    if (!fits(d, pos, 16)) return a;
    Ptr p = decode_ptr(be(d, pos, 8));
    a.count = (uint32_t)be(d, pos + 8, 2);
    a.valid = true;
    if (a.count == 0 || p.null()) { a.count = p.null() ? 0 : a.count; return a; }
    size_t at;
    if (!locate(pso_, p, at)) { a.valid = false; return a; }
    a.pos = at;
    return a;
}
PsoArrayRef PsoView::get_array(const PsoField& f) const {
    if (!f.entry) return {};
    return array_at(f.owner, *f.entry, f.pos);
}
PsoObject PsoView::array_struct(const PsoArrayRef& a, uint32_t i) const {
    if (!a.valid || !a.element || i >= a.count || a.element->type != Structure) return {};
    const Bytes& d = pso_.data();
    if (a.pointers) {
        size_t at = a.pos + 8 * (size_t)i;
        if (!fits(d, at, 8)) return {};
        return follow(be(d, at, 8), a.element->ref_key);
    }
    PsoObject o;
    o.view = this; o.type = a.element->ref_key; o.info = pso_.schema.find_struct(o.type);
    if (!o.info) return {};
    o.pos = a.pos + (size_t)o.info->size * i;
    if (o.pos > d.size()) return {};
    return o;
}
bool PsoView::array_int(const PsoArrayRef& a, uint32_t i, int64_t& v) const {
    if (!a.valid || !a.element || i >= a.count || !a.stride) return false;
    PsoField f;
    PsoStructEntry unsigned_el = *a.element;
    if (unsigned_el.type == UInt) unsigned_el.subtype = 1; // arrays of UInt are unsigned whatever the subtype says
    f.entry = &unsigned_el; f.pos = a.pos + a.stride * (size_t)i;
    switch (a.element->type) {
    case Bool: case SByte: case UByte: case SShort: case UShort: case SInt: case UInt: case Long: case Enum: case Flags: case HFloat:
        return get_int(f, v);
    case String: return (a.element->subtype == 7 || a.element->subtype == 8) && get_int(f, v);
    default: return false;
    }
}
bool PsoView::array_float(const PsoArrayRef& a, uint32_t i, double& v) const {
    if (!a.valid || !a.element || i >= a.count || !a.stride) return false;
    PsoField f;
    f.entry = a.element; f.pos = a.pos + a.stride * (size_t)i;
    return get_float(f, v);
}
bool PsoView::array_vec(const PsoArrayRef& a, uint32_t i, float out[4], int& n) const {
    if (!a.valid || !a.element || i >= a.count || !a.stride) return false;
    PsoField f;
    f.entry = a.element; f.pos = a.pos + a.stride * (size_t)i;
    return get_vec(f, out, n);
}
bool PsoView::array_string(const PsoArrayRef& a, uint32_t i, std::string& s) const {
    if (!a.valid || !a.element || i >= a.count || !a.stride || a.element->type != String) return false;
    return string_at(*a.element, a.pos + a.stride * (size_t)i, s);
}

std::string PsoView::enum_name(uint32_t enum_hash, int64_t value) const {
    auto* e = pso_.schema.find_enum(enum_hash);
    if (!e) return {};
    for (auto& x : e->entries) if (x.value == (int32_t)value) return pso_name(x.name);
    return {};
}
std::string PsoView::flag_names(uint32_t enum_hash, uint64_t value) const {
    auto* e = pso_.schema.find_enum(enum_hash);
    std::string r;
    if (!e) return r;
    for (auto& x : e->entries) {
        if (x.value < 0 || x.value > 31) continue;
        if (value & (1u << x.value)) { if (!r.empty()) r += ' '; r += pso_name(x.name); }
    }
    return r;
}

std::vector<PsoView::MapItem> PsoView::get_map(const PsoField& f) const {
    std::vector<MapItem> r;
    if (!f.entry || f.entry->type != Map || f.entry->subtype != 1) return r;
    const Bytes& d = pso_.data();
    if (!fits(d, f.pos, 24)) return r;
    uint32_t x1 = (uint32_t)be(d, f.pos, 4);
    uint64_t ptr = be(d, f.pos + 8, 8);
    uint32_t count = (uint32_t)be(d, f.pos + 16, 2);
    if (x1 != 0x1000000) { // the other layout: counts and a pointer at +16
        count = (uint32_t)be(d, f.pos + 2, 2);
        ptr = be(d, f.pos + 16, 8);
    }
    Ptr p = decode_ptr(ptr);
    if (count == 0 || p.null()) return r;
    size_t at;
    const PsoBlock* b = nullptr;
    if (!locate(pso_, p, at, &b)) return r;
    const PsoStructInfo* ws = pso_.schema.find_struct(b->name);
    if (!ws) return r;
    const PsoStructEntry *ke = nullptr, *ie = nullptr;
    uint32_t key_hash = joaat("Key"), item_hash = joaat("Item");
    for (auto& e : ws->entries) {
        if (e.name == key_hash || pso_name(e.name) == "Key") ke = &e;
        if (e.name == item_hash || pso_name(e.name) == "Item") ie = &e;
    }
    if (!ke || !ie || ke->type != String) return r;
    for (uint32_t n = 0; n < count; n++) {
        size_t base = at + (size_t)ws->size * n;
        if (base + ws->size > d.size()) break;
        MapItem m;
        string_at(*ke, base + ke->offset, m.key);
        if (ie->type == String) {
            m.is_string = true;
            string_at(*ie, base + ie->offset, m.text);
        } else if (ie->ref_key != 0 && ie->type == Structure && ie->subtype == 0) {
            m.object.view = this; m.object.type = ie->ref_key; m.object.pos = base + ie->offset;
            m.object.info = pso_.schema.find_struct(ie->ref_key);
        } else if (fits(d, base + ie->offset, 8)) {
            m.object = follow(be(d, base + ie->offset, 8), 0);
        }
        r.push_back(std::move(m));
    }
    return r;
}

// --- dump -----------------------------------------------------------------------------------------------------------

namespace {

struct Dumper {
    const PsoView& v;
    size_t max_items;
    std::string out;

    void line(int ind, const std::string& s) { out.append(size_t(ind) * 2, ' '); out += s; out += '\n'; }

    std::string vec_text(const float* x, int n) {
        std::string s = "(";
        for (int i = 0; i < n; i++) { if (i) s += ", "; s += fmt_float(x[i]); }
        return s + ")";
    }

    std::string scalar_text(const PsoStructEntry& e, const PsoObject& owner, const PsoField& f) {
        int64_t iv;
        double dv;
        float vec[4];
        int n;
        std::string sv;
        switch (e.type) {
        case Bool: return v.get_int(f, iv) ? (iv ? "true" : "false") : "?";
        case Float: case HFloat: return v.get_float(f, dv) ? fmt_float(dv) : "?";
        case Float2: case Float3: case Float3a: case Float4a: case Float4: return v.get_vec(f, vec, n) ? vec_text(vec, n) : "?";
        case String: return v.get_string(f, sv) ? quote(sv) : "?";
        case Enum: {
            if (!v.get_int(f, iv)) return "?";
            std::string nm = v.enum_name(e.ref_key, iv);
            return nm.empty() ? std::to_string(iv) : nm;
        }
        case Flags: {
            if (!v.get_int(f, iv)) return "?";
            uint32_t fe = e.ref_key & 0xFFF;
            std::string nm;
            if (fe != 0xFFF && owner.info && fe < owner.info->entries.size() && owner.info->entries[fe].name == kArrayInfo)
                nm = v.flag_names(owner.info->entries[fe].ref_key, (uint64_t)iv);
            if (!nm.empty()) return nm;
            char b[32]; std::snprintf(b, sizeof b, "0x%llX", (unsigned long long)iv);
            return b;
        }
        case UInt:
            if (!v.get_int(f, iv)) return "?";
            if (e.subtype == 1) { char b[32]; std::snprintf(b, sizeof b, "0x%08llX", (unsigned long long)iv); return b; }
            return std::to_string(iv);
        case Long:
            return v.get_int(f, iv) ? std::to_string((unsigned long long)iv) : "?";
        default:
            return v.get_int(f, iv) ? std::to_string(iv) : "?";
        }
    }

    void object(int ind, const std::string& label, const PsoObject& o, uint32_t depth) {
        if (!o) { line(ind, label + " = <no structure info>"); return; }
        if (depth > kMaxDepth) { line(ind, label + " = ..."); return; }
        line(ind, label + pso_name(o.type) + " {");
        for (auto& f : v.fields(o)) field(ind + 1, f, depth);
        line(ind, "}");
    }

    void field(int ind, const PsoField& f, uint32_t depth) {
        const PsoStructEntry& e = *f.entry;
        std::string name = pso_name(e.name);
        switch (e.type) {
        case Structure: {
            PsoObject s = v.get_struct(f);
            if (!s) {
                if (e.subtype == 0 || true) line(ind, name + " = null");
                return;
            }
            object(ind, name + " = ", s, depth + 1);
            return;
        }
        case Array:
            array(ind, name, f.owner, e, f.pos, depth);
            return;
        case Map: {
            auto items = v.get_map(f);
            if (items.empty()) { line(ind, name + " = {}"); return; }
            line(ind, name + " = map[" + std::to_string(items.size()) + "] {");
            for (auto& m : items) {
                if (m.is_string) line(ind + 1, quote(m.key) + " = " + quote(m.text));
                else object(ind + 1, quote(m.key) + " = ", m.object, depth + 1);
            }
            line(ind, "}");
            return;
        }
        default:
            line(ind, name + " = " + scalar_text(e, f.owner, f));
        }
    }

    void array(int ind, const std::string& name, const PsoObject& owner, const PsoStructEntry& e, size_t pos, uint32_t depth) {
        PsoArrayRef a = v.array_at(owner, e, pos);
        if (!a.valid || !a.element) { line(ind, name + " = <unreadable array>"); return; }
        const PsoStructEntry& el = *a.element;
        std::string head = name + "[" + std::to_string(a.count) + "]";
        if (a.count == 0) { line(ind, head + " = []"); return; }
        size_t shown = (max_items && a.count > max_items) ? max_items : a.count;
        std::string more = shown < a.count ? " ... +" + std::to_string(a.count - shown) + " more" : "";
        switch (el.type) {
        case Structure: {
            line(ind, head + " (" + pso_name(el.ref_key) + (a.pointers ? " pointers" : "") + ") {");
            for (size_t i = 0; i < shown; i++) {
                PsoObject o = v.array_struct(a, (uint32_t)i);
                if (!o && a.pointers) { line(ind + 1, "[" + std::to_string(i) + "] = null"); continue; }
                object(ind + 1, "[" + std::to_string(i) + "] ", o, depth + 1);
            }
            if (!more.empty()) line(ind + 1, more);
            line(ind, "}");
            return;
        }
        case Array: {
            line(ind, head + " {");
            for (size_t i = 0; i < shown; i++) array(ind + 1, "[" + std::to_string(i) + "]", owner, el, a.pos + 16 * i, depth + 1);
            line(ind, "}");
            return;
        }
        case String: {
            std::string s = "[";
            for (size_t i = 0; i < shown; i++) {
                std::string t;
                bool ok = v.array_string(a, (uint32_t)i, t);
                if (i) s += ", ";
                s += ok ? quote(t) : "?";
            }
            line(ind, head + " = " + s + more + "]");
            return;
        }
        case Float2: case Float3: case Float3a: case Float4a: case Float4: {
            std::string s = "[";
            for (size_t i = 0; i < shown; i++) {
                float x[4]; int n = 0;
                if (i) s += ", ";
                s += v.array_vec(a, (uint32_t)i, x, n) ? vec_text(x, n) : "?";
            }
            line(ind, head + " = " + s + more + "]");
            return;
        }
        case Float: case HFloat: {
            std::string s = "[";
            for (size_t i = 0; i < shown; i++) {
                double x;
                if (i) s += ", ";
                s += v.array_float(a, (uint32_t)i, x) ? fmt_float(x) : "?";
            }
            line(ind, head + " = " + s + more + "]");
            return;
        }
        default: {
            std::string s = "[";
            for (size_t i = 0; i < shown; i++) {
                int64_t x;
                if (i) s += ", ";
                if (!v.array_int(a, (uint32_t)i, x)) { s += "?"; continue; }
                if (el.type == Enum) { std::string nm = v.enum_name(el.ref_key, x); s += nm.empty() ? std::to_string(x) : nm; }
                else s += std::to_string(x);
            }
            line(ind, head + " = " + s + more + "]");
        }
        }
    }
};

// Flat listing: one "path<TAB>value" line per scalar / primitive array, depth first, for machine comparison with other readers
// (tools/psocheck.py checks it against CodeWalker's XML). Array items are [i]; long arrays are never cut.
struct Flattener {
    const PsoView& v;
    Dumper& d;
    std::string out;

    void emit(const std::string& path, const std::string& val) { out += path + "\t" + val + "\n"; }

    void object(const std::string& path, const PsoObject& o, uint32_t depth) {
        if (!o || depth > kMaxDepth) return;
        for (auto& f : v.fields(o)) field(path + "/" + pso_name(f.entry->name), f, depth);
    }
    void field(const std::string& path, const PsoField& f, uint32_t depth) {
        const PsoStructEntry& e = *f.entry;
        switch (e.type) {
        case Structure: object(path, v.get_struct(f), depth + 1); return;
        case Array: array(path, f.owner, e, f.pos, depth); return;
        case Map:
            {
                size_t mi = 0;
                for (auto& m : v.get_map(f)) {
                    std::string ip = path + "/[" + std::to_string(mi++) + "]";
                    if (m.is_string) emit(ip, m.text);
                    else object(ip, m.object, depth + 1);
                }
            }
            return;
        default: emit(path, d.scalar_text(e, f.owner, f));
        }
    }
    void array(const std::string& path, const PsoObject& owner, const PsoStructEntry& e, size_t pos, uint32_t depth) {
        PsoArrayRef a = v.array_at(owner, e, pos);
        if (!a.valid || !a.element) { emit(path, "<unreadable>"); return; }
        const PsoStructEntry& el = *a.element;
        if (el.type == Structure) {
            for (uint32_t i = 0; i < a.count; i++) object(path + "/[" + std::to_string(i) + "]", v.array_struct(a, i), depth + 1);
            return;
        }
        if (el.type == Array) {
            for (uint32_t i = 0; i < a.count; i++) array(path + "/[" + std::to_string(i) + "]", owner, el, a.pos + 16 * i, depth + 1);
            return;
        }
        std::string s;
        for (uint32_t i = 0; i < a.count; i++) {
            std::string t;
            float x[4]; int n = 0; int64_t iv; double dv;
            if (el.type == String) { if (!v.array_string(a, i, t)) t = "?"; }
            else if (el.type >= Float2 && el.type <= Float4) { t = v.array_vec(a, i, x, n) ? d.vec_text(x, n) : "?"; }
            else if (el.type == Float3a || el.type == Float4a) { t = v.array_vec(a, i, x, n) ? d.vec_text(x, n) : "?"; }
            else if (el.type == Float || el.type == HFloat) { t = v.array_float(a, i, dv) ? fmt_float(dv) : "?"; }
            else if (el.type == Enum) { if (v.array_int(a, i, iv)) { t = v.enum_name(el.ref_key, iv); if (t.empty()) t = std::to_string(iv); } else t = "?"; }
            else { t = v.array_int(a, i, iv) ? std::to_string(iv) : "?"; }
            if (i) s += " ";
            s += t;
        }
        emit(path, s);
    }
};

} // namespace

std::string pso_dump(const Pso& p, size_t max_items, bool flat) {
    if (!p.has_map || !p.has_schema) return "(PSO without a readable PMAP / PSCH)\n";
    PsoView v(p);
    Dumper d{v, max_items, {}};
    PsoObject r = v.root();
    if (!r) return "(PSO without a root structure)\n";
    if (flat) {
        Flattener fl{v, d, {}};
        fl.object("/" + pso_name(r.type), r, 0);
        return fl.out;
    }
    d.object(0, "", r, 0);
    return d.out;
}

} // namespace rk
