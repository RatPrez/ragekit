#include "ragekit/gfx_build.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include "ragekit/error.h"
#include "ragekit/hash.h"

namespace rk::gfx {

namespace {

// SWF bit fields: most significant bit first, byte-aligned where a structure starts.
class BitReader {
public:
    explicit BitReader(std::span<const uint8_t> b, size_t byte = 0) : b_(b), bit_(byte * 8) {}
    uint32_t u(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; i++) {
            if ((bit_ >> 3) >= b_.size()) throw Error("gfx: a bit field runs past the tag");
            v = v << 1 | ((b_[bit_ >> 3] >> (7 - (bit_ & 7))) & 1);
            bit_++;
        }
        return v;
    }
    int32_t s(int n) {
        if (n == 0) return 0;
        const uint32_t v = u(n);
        return n < 32 && (v >> (n - 1)) & 1 ? (int32_t)(v | ~0u << n) : (int32_t)v;
    }
    void align() { bit_ = (bit_ + 7) & ~size_t(7); }
    uint8_t u8() { align(); return (uint8_t)u(8); }
    uint16_t u16() { align(); const uint16_t lo = (uint16_t)u(8); return (uint16_t)(lo | u(8) << 8); }
    uint32_t u32() { const uint32_t lo = u16(); return lo | (uint32_t)u16() << 16; }
    std::string cstr() {
        align();
        std::string s;
        for (;;) {
            const uint8_t c = (uint8_t)u(8);
            if (!c) return s;
            s.push_back((char)c);
        }
    }
    size_t byte() { align(); return bit_ >> 3; }
    bool at_end() { return ((bit_ + 7) >> 3) >= b_.size(); }
private:
    std::span<const uint8_t> b_;
    size_t bit_;
};

class BitWriter {
public:
    void u(uint32_t v, int n) {
        for (int i = n - 1; i >= 0; i--) {
            if (!(bits_ & 7)) out.push_back(0);
            if ((v >> i) & 1) out.back() |= (uint8_t)(0x80 >> (bits_ & 7));
            bits_++;
        }
    }
    void s(int32_t v, int n) { u((uint32_t)v & (n >= 32 ? ~0u : (1u << n) - 1), n); }
    void align() { bits_ = (bits_ + 7) & ~size_t(7); }
    void u8(uint8_t v) { align(); u(v, 8); }
    void u16(uint16_t v) { u8((uint8_t)v); u8((uint8_t)(v >> 8)); }
    void u32(uint32_t v) { u16((uint16_t)v); u16((uint16_t)(v >> 16)); }
    void cstr(const std::string& s) { for (char c : s) u8((uint8_t)c); u8(0); }
    void bytes(const Bytes& b) { align(); for (uint8_t c : b) u8(c); }
    Bytes out;
private:
    size_t bits_ = 0;
};

// The fewest bits a signed field holding v needs (0 for 0).
int sbits(int32_t v) {
    if (v == 0) return 0;
    int n = 1;
    while (n < 32 && !(v >= -(int64_t(1) << (n - 1)) && v < (int64_t(1) << (n - 1)))) n++;
    return n;
}
int ubits(uint32_t v) { int n = 0; while (n < 32 && (v >> n)) n++; return n; }
// The count a field read from a file used, while the value still fits in it; else the fewest.
int pick(int fewest, BitHint h) { return h.n >= fewest ? h.n : fewest; }

Rect read_rect(BitReader& r) {
    r.align();
    const int n = (int)r.u(5);
    Rect x;
    x.x_min = r.s(n); x.x_max = r.s(n); x.y_min = r.s(n); x.y_max = r.s(n);
    x.bits.n = (int8_t)n;
    return x;
}
void write_rect(BitWriter& w, const Rect& x) {
    w.align();
    const int n = pick(std::max({sbits(x.x_min), sbits(x.x_max), sbits(x.y_min), sbits(x.y_max)}), x.bits);
    w.u(n, 5);
    w.s(x.x_min, n); w.s(x.x_max, n); w.s(x.y_min, n); w.s(x.y_max, n);
}

Matrix read_matrix(BitReader& r) {
    r.align();
    Matrix m;
    if ((m.has_scale = r.u(1))) { const int n = (int)r.u(5); m.sx = r.s(n); m.sy = r.s(n); m.scale_bits.n = (int8_t)n; }
    if ((m.has_rotate = r.u(1))) { const int n = (int)r.u(5); m.r0 = r.s(n); m.r1 = r.s(n); m.rotate_bits.n = (int8_t)n; }
    const int n = (int)r.u(5);
    m.tx = r.s(n);
    m.ty = r.s(n);
    m.translate_bits.n = (int8_t)n;
    r.align();
    return m;
}
void write_matrix(BitWriter& w, const Matrix& m) {
    w.align();
    w.u(m.has_scale, 1);
    if (m.has_scale) { const int n = pick(std::max(sbits(m.sx), sbits(m.sy)), m.scale_bits); w.u(n, 5); w.s(m.sx, n); w.s(m.sy, n); }
    w.u(m.has_rotate, 1);
    if (m.has_rotate) { const int n = pick(std::max(sbits(m.r0), sbits(m.r1)), m.rotate_bits); w.u(n, 5); w.s(m.r0, n); w.s(m.r1, n); }
    const int n = pick(std::max(sbits(m.tx), sbits(m.ty)), m.translate_bits);
    w.u(n, 5);
    w.s(m.tx, n);
    w.s(m.ty, n);
    w.align();
}

ColourTransform read_cxform(BitReader& r) {
    r.align();
    ColourTransform c;
    c.has_add = r.u(1);
    c.has_mul = r.u(1);
    const int n = (int)r.u(4);
    c.bits.n = (int8_t)n;
    if (c.has_mul) for (int i = 0; i < 4; i++) c.mul[i] = (int16_t)r.s(n);
    if (c.has_add) for (int i = 0; i < 4; i++) c.add[i] = (int16_t)r.s(n);
    r.align();
    return c;
}
void write_cxform(BitWriter& w, const ColourTransform& c) {
    w.align();
    int n = 0;
    for (int i = 0; i < 4; i++) {
        if (c.has_mul) n = std::max(n, sbits(c.mul[i]));
        if (c.has_add) n = std::max(n, sbits(c.add[i]));
    }
    n = pick(n, c.bits);
    w.u(c.has_add, 1);
    w.u(c.has_mul, 1);
    w.u(n, 4);
    if (c.has_mul) for (int i = 0; i < 4; i++) w.s(c.mul[i], n);
    if (c.has_add) for (int i = 0; i < 4; i++) w.s(c.add[i], n);
    w.align();
}

Rgba read_colour(BitReader& r, bool alpha) {
    Rgba c;
    c.r = r.u8(); c.g = r.u8(); c.b = r.u8();
    c.a = alpha ? r.u8() : 255;
    return c;
}
void write_colour(BitWriter& w, const Rgba& c, bool alpha) {
    w.u8(c.r); w.u8(c.g); w.u8(c.b);
    if (alpha) w.u8(c.a);
}

FillStyle read_fill(BitReader& r, int version) {
    FillStyle f;
    f.type = r.u8();
    if (f.type == 0) {
        f.colour = read_colour(r, version >= 3);
    } else if (f.type == 0x10 || f.type == 0x12 || f.type == 0x13) {
        f.matrix = read_matrix(r);
        const uint8_t g = r.u8();
        f.gradient_flags = g & 0xF0;
        for (int i = 0; i < (g & 15); i++) {
            const uint8_t ratio = r.u8();
            f.gradient.emplace_back(ratio, read_colour(r, version >= 3));
        }
        if (f.type == 0x13) f.focal = (int16_t)r.u16();
    } else if (f.type >= 0x40 && f.type <= 0x43) {
        f.bitmap = r.u16();
        f.matrix = read_matrix(r);
    } else {
        throw Error("gfx: a fill style of type " + std::to_string(f.type));
    }
    return f;
}
void write_fill(BitWriter& w, const FillStyle& f, int version) {
    w.u8(f.type);
    if (f.type == 0) {
        write_colour(w, f.colour, version >= 3);
    } else if (f.type == 0x10 || f.type == 0x12 || f.type == 0x13) {
        if (f.gradient.size() > 15) throw Error("gfx: a gradient of more than 15 colours");
        write_matrix(w, f.matrix);
        w.u8((uint8_t)((f.gradient_flags & 0xF0) | f.gradient.size()));
        for (const auto& [ratio, c] : f.gradient) { w.u8(ratio); write_colour(w, c, version >= 3); }
        if (f.type == 0x13) w.u16((uint16_t)f.focal);
    } else if (f.type >= 0x40 && f.type <= 0x43) {
        w.u16(f.bitmap);
        write_matrix(w, f.matrix);
    } else {
        throw Error("gfx: a fill style of type " + std::to_string(f.type));
    }
}

LineStyle read_line(BitReader& r, int version) {
    LineStyle l;
    l.width = r.u16();
    if (version < 4) {
        l.colour = read_colour(r, version >= 3);
        return l;
    }
    l.flags = r.u16();
    // the 16 bits as stored: byte 0 = start cap 2, join 2, has fill 1, no h scale 1, no v scale 1, pixel hinting 1;
    // byte 1 = reserved 5, no close 1, end cap 2 (read as a little-endian u16: byte 0 is the low byte)
    const int join = (l.flags >> 4) & 3;
    if (join == 2) l.miter = r.u16();
    if ((l.flags >> 3) & 1) l.fill = read_fill(r, version);
    else l.colour = read_colour(r, true);
    return l;
}
void write_line(BitWriter& w, const LineStyle& l, int version) {
    w.u16(l.width);
    if (version < 4) { write_colour(w, l.colour, version >= 3); return; }
    uint16_t flags = (uint16_t)(l.flags & ~0x08);
    if (l.fill) flags |= 0x08;
    w.u16(flags);
    if (((flags >> 4) & 3) == 2) w.u16(l.miter);
    if (l.fill) write_fill(w, *l.fill, version);
    else write_colour(w, l.colour, true);
}

void read_styles(BitReader& r, int version, std::vector<FillStyle>& fills, std::vector<LineStyle>& lines) {
    size_t n = r.u8();
    if (n == 0xFF && version >= 2) n = r.u16();
    for (size_t i = 0; i < n; i++) fills.push_back(read_fill(r, version));
    n = r.u8();
    if (n == 0xFF) n = r.u16();
    for (size_t i = 0; i < n; i++) lines.push_back(read_line(r, version));
}
void write_styles(BitWriter& w, int version, const std::vector<FillStyle>& fills, const std::vector<LineStyle>& lines) {
    if (fills.size() >= 0xFF) {
        if (version < 2) throw Error("gfx: DefineShape holds at most 254 fill styles");
        w.u8(0xFF);
        w.u16((uint16_t)fills.size());
    } else {
        w.u8((uint8_t)fills.size());
    }
    for (const FillStyle& f : fills) write_fill(w, f, version);
    if (lines.size() >= 0xFF) { w.u8(0xFF); w.u16((uint16_t)lines.size()); }
    else w.u8((uint8_t)lines.size());
    for (const LineStyle& l : lines) write_line(w, l, version);
}

std::vector<GfxTag> read_tag_list(std::span<const uint8_t> b) {
    std::vector<GfxTag> out;
    size_t p = 0;
    while (p < b.size()) {
        if (p + 2 > b.size()) throw Error("gfx: a sprite's tag header cut short");
        const uint16_t h = (uint16_t)(b[p] | b[p + 1] << 8);
        p += 2;
        GfxTag t;
        t.code = h >> 6;
        uint32_t len = h & 0x3F;
        if (len == 0x3F) {
            if (p + 4 > b.size()) throw Error("gfx: a sprite's long tag length cut short");
            std::memcpy(&len, b.data() + p, 4);
            p += 4;
            t.long_form = true;
        }
        if (len > b.size() - p) throw Error("gfx: a sprite's tag runs past it");
        t.payload.assign(b.begin() + p, b.begin() + p + len);
        p += len;
        out.push_back(std::move(t));
    }
    return out;
}
void write_tag(Bytes& body, const GfxTag& t) {
    if (t.code > 0x3FF) throw Error("gfx: a tag code past 1023");
    const bool lf = t.long_form || t.payload.size() >= 0x3F;
    const uint16_t h = (uint16_t)(t.code << 6 | (lf ? 0x3F : t.payload.size()));
    body.push_back((uint8_t)h);
    body.push_back((uint8_t)(h >> 8));
    if (lf) {
        const uint32_t len = (uint32_t)t.payload.size();
        for (int i = 0; i < 4; i++) body.push_back((uint8_t)(len >> (8 * i)));
    }
    body.insert(body.end(), t.payload.begin(), t.payload.end());
}

GfxTag make_tag(uint16_t code, Bytes payload) {
    GfxTag t;
    t.code = code;
    t.payload = std::move(payload);
    return t;
}

}  // namespace

Matrix Matrix::make(double scale_x, double scale_y, double rotation, int32_t tx, int32_t ty) {
    Matrix m;
    const double c = std::cos(rotation), s = std::sin(rotation);
    auto fx = [](double v) { return (int32_t)std::lround(v * 65536.0); };
    m.sx = fx(scale_x * c);
    m.sy = fx(scale_y * c);
    m.r0 = fx(scale_x * s);    // y' gets x * sin
    m.r1 = fx(-scale_y * s);   // x' gets -y * sin
    m.has_scale = m.sx != 65536 || m.sy != 65536;
    m.has_rotate = m.r0 != 0 || m.r1 != 0;
    if (!m.has_scale) m.sx = m.sy = 65536;
    m.tx = tx;
    m.ty = ty;
    return m;
}

void Matrix::apply(double x, double y, double& ox, double& oy) const {
    ox = a() * x + c() * y + tx;
    oy = b() * x + d() * y + ty;
}

Shape read_shape(const GfxTag& t) {
    Shape s;
    switch (t.code) {
    case kDefineShape: s.version = 1; break;
    case kDefineShape2: s.version = 2; break;
    case kDefineShape3: s.version = 3; break;
    case kDefineShape4: s.version = 4; break;
    default: throw Error("gfx: tag " + std::to_string(t.code) + " is not a DefineShape");
    }
    BitReader r(t.payload);
    s.id = r.u16();
    s.bounds = read_rect(r);
    if (s.version == 4) {
        s.edge_bounds = read_rect(r);
        s.flags4 = r.u8();
    }
    read_styles(r, s.version, s.fills, s.lines);
    r.align();
    int nf = (int)r.u(4), nl = (int)r.u(4);
    for (;;) {
        ShapeRecord rec;
        if (r.u(1) == 0) {
            const uint32_t fl = r.u(5);
            if (fl == 0) break;
            rec.kind = ShapeRecord::Style;
            if (fl & 1) {
                const int n = (int)r.u(5);
                rec.bits.n = (int8_t)n;
                rec.has_move = true;
                rec.move_x = r.s(n);
                rec.move_y = r.s(n);
            }
            if (fl & 2) rec.fill0 = r.u(nf);
            if (fl & 4) rec.fill1 = r.u(nf);
            if (fl & 8) rec.line = r.u(nl);
            if (fl & 16) {
                rec.new_styles = true;
                read_styles(r, s.version, rec.fills, rec.lines);
                r.align();
                nf = (int)r.u(4);
                nl = (int)r.u(4);
            }
        } else if (r.u(1)) {
            rec.kind = ShapeRecord::Line;
            const int n = (int)r.u(4) + 2;
            rec.bits.n = (int8_t)n;
            if (r.u(1)) {
                rec.dx = r.s(n);
                rec.dy = r.s(n);
                if (rec.dx == 0 || rec.dy == 0) rec.general.n = 1;
            } else if (r.u(1)) {
                rec.dy = r.s(n);
            } else {
                rec.dx = r.s(n);
            }
        } else {
            rec.kind = ShapeRecord::Curve;
            const int n = (int)r.u(4) + 2;
            rec.bits.n = (int8_t)n;
            rec.cdx = r.s(n);
            rec.cdy = r.s(n);
            rec.dx = r.s(n);
            rec.dy = r.s(n);
        }
        s.records.push_back(std::move(rec));
    }
    return s;
}

GfxTag shape_tag(const Shape& s) {
    static const uint16_t codes[] = {0, kDefineShape, kDefineShape2, kDefineShape3, kDefineShape4};
    if (s.version < 1 || s.version > 4) throw Error("gfx: a shape of version " + std::to_string(s.version));
    BitWriter w;
    w.u16(s.id);
    write_rect(w, s.bounds);
    if (s.version == 4) {
        write_rect(w, s.edge_bounds);
        w.u8(s.flags4);
    }
    write_styles(w, s.version, s.fills, s.lines);
    w.align();
    int nf = ubits((uint32_t)s.fills.size()), nl = ubits((uint32_t)s.lines.size());
    w.u(nf, 4);
    w.u(nl, 4);
    for (const ShapeRecord& rec : s.records) {
        if (rec.kind == ShapeRecord::Style) {
            const uint32_t fl = (rec.has_move ? 1 : 0) | (rec.fill0 ? 2 : 0) | (rec.fill1 ? 4 : 0) | (rec.line ? 8 : 0) |
                                (rec.new_styles ? 16 : 0);
            if (!fl) throw Error("gfx: a style change record that changes nothing (it would end the shape)");
            w.u(0, 1);
            w.u(fl, 5);
            if (rec.has_move) {
                const int n = pick(std::max(sbits(rec.move_x), sbits(rec.move_y)), rec.bits);
                w.u(n, 5);
                w.s(rec.move_x, n);
                w.s(rec.move_y, n);
            }
            auto index = [&](uint32_t v, int n) {
                if (n < 32 && v >> n) throw Error("gfx: a style index past the style array");
                w.u(v, n);
            };
            if (rec.fill0) index(*rec.fill0, nf);
            if (rec.fill1) index(*rec.fill1, nf);
            if (rec.line) index(*rec.line, nl);
            if (rec.new_styles) {
                if (s.version < 2) throw Error("gfx: DefineShape cannot start new styles");
                w.align();
                write_styles(w, s.version, rec.fills, rec.lines);
                w.align();
                nf = ubits((uint32_t)rec.fills.size());
                nl = ubits((uint32_t)rec.lines.size());
                w.u(nf, 4);
                w.u(nl, 4);
            }
        } else if (rec.kind == ShapeRecord::Line) {
            w.u(1, 1);
            w.u(1, 1);
            if ((rec.dx != 0 && rec.dy != 0) || rec.general.n == 1) {
                const int n = pick(std::max({2, sbits(rec.dx), sbits(rec.dy)}), rec.bits);
                if (n > 17) throw Error("gfx: an edge longer than 17 bits");
                w.u(n - 2, 4);
                w.u(1, 1);
                w.s(rec.dx, n);
                w.s(rec.dy, n);
            } else {
                const bool vertical = rec.dx == 0;
                const int n = pick(std::max(2, sbits(vertical ? rec.dy : rec.dx)), rec.bits);
                if (n > 17) throw Error("gfx: an edge longer than 17 bits");
                w.u(n - 2, 4);
                w.u(0, 1);
                w.u(vertical, 1);
                w.s(vertical ? rec.dy : rec.dx, n);
            }
        } else {
            w.u(1, 1);
            w.u(0, 1);
            const int n = pick(std::max({2, sbits(rec.cdx), sbits(rec.cdy), sbits(rec.dx), sbits(rec.dy)}), rec.bits);
            if (n > 17) throw Error("gfx: a curve longer than 17 bits");
            w.u(n - 2, 4);
            w.s(rec.cdx, n);
            w.s(rec.cdy, n);
            w.s(rec.dx, n);
            w.s(rec.dy, n);
        }
    }
    w.u(0, 6);
    w.align();
    return make_tag(codes[s.version], std::move(w.out));
}

std::vector<ShapePath> shape_paths(const Shape& s) {
    std::vector<ShapePath> out;
    const std::vector<FillStyle>* fills = &s.fills;
    const std::vector<LineStyle>* lines = &s.lines;
    int layer = 0;
    uint32_t f0 = 0, f1 = 0, ln = 0;
    double x = 0, y = 0;
    bool open = false;
    auto start = [&]() {
        ShapePath p;
        p.layer = layer;
        p.fill0 = f0; p.fill1 = f1; p.line = ln;
        p.fill0_style = f0 && f0 <= fills->size() ? &(*fills)[f0 - 1] : nullptr;
        p.fill1_style = f1 && f1 <= fills->size() ? &(*fills)[f1 - 1] : nullptr;
        p.line_style = ln && ln <= lines->size() ? &(*lines)[ln - 1] : nullptr;
        p.x0 = x; p.y0 = y;
        out.push_back(std::move(p));
        open = true;
    };
    for (const ShapeRecord& r : s.records) {
        if (r.kind == ShapeRecord::Style) {
            open = false;
            if (r.new_styles) {
                fills = &r.fills;
                lines = &r.lines;
                layer++;
                f0 = f1 = ln = 0;
            }
            if (r.has_move) { x = r.move_x; y = r.move_y; }
            if (r.fill0) f0 = *r.fill0;
            if (r.fill1) f1 = *r.fill1;
            if (r.line) ln = *r.line;
            continue;
        }
        if (!open) start();
        ShapePath::Point p{};
        if (r.kind == ShapeRecord::Curve) {
            p.curve = true;
            p.cx = x + r.cdx;
            p.cy = y + r.cdy;
            x = p.cx + r.dx;
            y = p.cy + r.dy;
        } else {
            x += r.dx;
            y += r.dy;
        }
        p.x = x;
        p.y = y;
        out.back().points.push_back(p);
    }
    return out;
}

std::vector<std::pair<Rgba, double>> shape_fill_areas(const Shape& s) {
    std::vector<std::pair<Rgba, double>> out;
    auto add = [&](const FillStyle* f, double a) {
        if (!f || f->type != 0) return;
        for (auto& [c, v] : out) if (c == f->colour) { v += a; return; }
        out.emplace_back(f->colour, a);
    };
    // per (layer, fill index): the signed area of the edges with that fill on their right (fill1) minus left (fill0)
    std::map<std::pair<int, const FillStyle*>, double> sums;
    for (const ShapePath& p : shape_paths(s)) {
        double px = p.x0, py = p.y0;
        for (const ShapePath::Point& q : p.points) {
            double cross = 0;
            if (q.curve) {
                double ax = px, ay = py;
                for (int i = 1; i <= 8; i++) {
                    const double t = i / 8.0, u = 1 - t;
                    const double bx = u * u * px + 2 * t * u * q.cx + t * t * q.x, by = u * u * py + 2 * t * u * q.cy + t * t * q.y;
                    cross += ax * by - bx * ay;
                    ax = bx; ay = by;
                }
            } else {
                cross = px * q.y - q.x * py;
            }
            if (p.fill1_style) sums[{p.layer, p.fill1_style}] += cross;
            if (p.fill0_style) sums[{p.layer, p.fill0_style}] -= cross;
            px = q.x; py = q.y;
        }
    }
    for (const auto& [k, v] : sums) add(k.second, std::abs(v) / 2);
    return out;
}

Rect path_bounds(const Shape& s) {
    Rect b{INT32_MAX, INT32_MIN, INT32_MAX, INT32_MIN, {}};
    auto take = [&](double x, double y) {
        b.x_min = std::min(b.x_min, (int32_t)std::floor(x)); b.x_max = std::max(b.x_max, (int32_t)std::ceil(x));
        b.y_min = std::min(b.y_min, (int32_t)std::floor(y)); b.y_max = std::max(b.y_max, (int32_t)std::ceil(y));
    };
    for (const ShapePath& p : shape_paths(s)) {
        take(p.x0, p.y0);
        for (const ShapePath::Point& q : p.points) {
            take(q.x, q.y);
            if (q.curve) take(q.cx, q.cy);
        }
    }
    if (b.x_min > b.x_max) return Rect{};
    return b;
}

void ShapeBuilder::layer() {
    const Layer& l = layers_.back();
    if (l.fills.empty() && l.lines.empty() && l.paths.empty()) return;
    layers_.emplace_back();
}

uint32_t ShapeBuilder::fill(Rgba c) {
    auto& f = layers_.back().fills;
    const FillStyle s = FillStyle::solid(c);
    for (size_t i = 0; i < f.size(); i++) if (f[i] == s) return (uint32_t)i + 1;
    f.push_back(s);
    return (uint32_t)f.size();
}

uint32_t ShapeBuilder::line(uint16_t width, Rgba c) {
    auto& l = layers_.back().lines;
    LineStyle s;
    s.width = width;
    s.colour = c;
    for (size_t i = 0; i < l.size(); i++) if (l[i] == s) return (uint32_t)i + 1;
    l.push_back(s);
    return (uint32_t)l.size();
}

void ShapeBuilder::polygon(const Points& pts, uint32_t fill, uint32_t line) {
    if (pts.size() < 3) return;
    // the shoelace sum with y down: positive = clockwise on the screen = the inside on the edges' right (fill1)
    double a = 0;
    for (size_t i = 0; i < pts.size(); i++) {
        const auto& p = pts[i];
        const auto& q = pts[(i + 1) % pts.size()];
        a += (double)p.first * q.second - (double)q.first * p.second;
    }
    Path p{pts, true, a > 0 ? 0u : fill, a > 0 ? fill : 0u, line};
    layers_.back().paths.push_back(std::move(p));
}

void ShapeBuilder::polyline(const Points& pts, uint32_t line) {
    if (pts.size() < 2 || !line) return;
    layers_.back().paths.push_back(Path{pts, false, 0, 0, line});
}

Shape ShapeBuilder::finish() const {
    Shape s;
    s.id = id_;
    s.version = layers_.size() > 1 && version_ < 2 ? 2 : version_;
    int max_width = 0;
    Rect b{INT32_MAX, INT32_MIN, INT32_MAX, INT32_MIN, {}};
    for (size_t li = 0; li < layers_.size(); li++) {
        const Layer& l = layers_[li];
        for (const LineStyle& ls : l.lines) max_width = std::max(max_width, (int)ls.width);
        if (li == 0) {
            s.fills = l.fills;
            s.lines = l.lines;
        } else {
            ShapeRecord r;
            r.kind = ShapeRecord::Style;
            r.new_styles = true;
            r.fills = l.fills;
            r.lines = l.lines;
            s.records.push_back(std::move(r));
        }
        uint32_t f0 = 0, f1 = 0, ln = 0;
        for (const Path& p : l.paths) {
            ShapeRecord st;
            st.kind = ShapeRecord::Style;
            st.has_move = true;
            st.move_x = p.pts[0].first;
            st.move_y = p.pts[0].second;
            if (p.fill0 != f0) st.fill0 = f0 = p.fill0;
            if (p.fill1 != f1) st.fill1 = f1 = p.fill1;
            if (p.line != ln) st.line = ln = p.line;
            s.records.push_back(std::move(st));
            int32_t x = p.pts[0].first, y = p.pts[0].second;
            auto edge = [&](int32_t nx, int32_t ny) {
                if (nx == x && ny == y) return;
                ShapeRecord e;
                e.kind = ShapeRecord::Line;
                e.dx = nx - x;
                e.dy = ny - y;
                s.records.push_back(e);
                x = nx;
                y = ny;
            };
            for (size_t i = 1; i < p.pts.size(); i++) edge(p.pts[i].first, p.pts[i].second);
            if (p.closed) edge(p.pts[0].first, p.pts[0].second);
            for (const auto& [px, py] : p.pts) {
                b.x_min = std::min(b.x_min, px); b.x_max = std::max(b.x_max, px);
                b.y_min = std::min(b.y_min, py); b.y_max = std::max(b.y_max, py);
            }
        }
    }
    if (b.x_min > b.x_max) b = Rect{};
    if (s.version == 4) s.edge_bounds = b;
    const int h = (max_width + 1) / 2;
    s.bounds = b.x_min > b.x_max ? b : Rect{b.x_min - h, b.x_max + h, b.y_min - h, b.y_max + h, {}};
    return s;
}

Place read_place(const GfxTag& t) {
    if (t.code != kPlaceObject2 && t.code != kPlaceObject3) throw Error("gfx: tag " + std::to_string(t.code) + " is not a PlaceObject2 / 3");
    BitReader r(t.payload);
    Place p;
    p.place3 = t.code == kPlaceObject3;
    const uint8_t f = r.u8();
    const uint8_t f3 = p.place3 ? r.u8() : 0;
    p.move = f & 1;
    p.depth = r.u16();
    if (p.place3 && (f3 & 0x08)) p.class_name = r.cstr();
    if (f & 0x02) p.character = r.u16();
    if (f & 0x04) p.matrix = read_matrix(r);
    if (f & 0x08) p.colour = read_cxform(r);
    if (f & 0x10) p.ratio = r.u16();
    if (f & 0x20) p.name = r.cstr();
    if (f & 0x40) p.clip_depth = r.u16();
    if (p.place3) {
        p.flags3 = f3 & ~0x0E;
        if (!(f3 & 0x01)) {
            if (f3 & 0x02) p.blend_mode = r.u8();
            if (f3 & 0x04) p.cache_as_bitmap = r.u8();
        } else {
            p.flags3 |= f3 & 0x06;   // with filters the blend and cache bytes follow them: kept in `rest`
        }
    }
    p.clip_actions = f & 0x80;
    const size_t at = r.byte();
    p.rest.assign(t.payload.begin() + (long)std::min(at, t.payload.size()), t.payload.end());
    return p;
}

GfxTag place_tag(const Place& p) {
    BitWriter w;
    const uint8_t f = (p.move ? 1 : 0) | (p.character ? 0x02 : 0) | (p.matrix ? 0x04 : 0) | (p.colour ? 0x08 : 0) |
                      (p.ratio ? 0x10 : 0) | (!p.name.empty() ? 0x20 : 0) | (p.clip_depth ? 0x40 : 0) |
                      (p.clip_actions ? 0x80 : 0);
    w.u8(f);
    if (p.place3) {
        uint8_t f3 = p.flags3;
        if (!(f3 & 0x01)) {
            f3 &= ~0x06;
            if (p.blend_mode) f3 |= 0x02;
            if (p.cache_as_bitmap) f3 |= 0x04;
        }
        if (!p.class_name.empty()) f3 |= 0x08;
        w.u8(f3);
    }
    w.u16(p.depth);
    if (p.place3 && !p.class_name.empty()) w.cstr(p.class_name);
    if (p.character) w.u16(*p.character);
    if (p.matrix) write_matrix(w, *p.matrix);
    if (p.colour) write_cxform(w, *p.colour);
    if (p.ratio) w.u16(*p.ratio);
    if (!p.name.empty()) w.cstr(p.name);
    if (p.clip_depth) w.u16(*p.clip_depth);
    if (p.place3 && !(p.flags3 & 0x01)) {
        if (p.blend_mode) w.u8(*p.blend_mode);
        if (p.cache_as_bitmap) w.u8(*p.cache_as_bitmap);
    }
    w.bytes(p.rest);
    return make_tag(p.place3 ? kPlaceObject3 : kPlaceObject2, std::move(w.out));
}

Sprite read_sprite(const GfxTag& t) {
    if (t.code != kDefineSprite || t.payload.size() < 4) throw Error("gfx: not a DefineSprite");
    Sprite s;
    s.id = (uint16_t)(t.payload[0] | t.payload[1] << 8);
    s.frame_count = (uint16_t)(t.payload[2] | t.payload[3] << 8);
    s.tags = read_tag_list(std::span<const uint8_t>(t.payload).subspan(4));
    return s;
}

GfxTag sprite_tag(const Sprite& s) {
    Bytes b = {(uint8_t)s.id, (uint8_t)(s.id >> 8), (uint8_t)s.frame_count, (uint8_t)(s.frame_count >> 8)};
    for (const GfxTag& t : s.tags) write_tag(b, t);
    return make_tag(kDefineSprite, std::move(b));
}

GfxTag show_frame_tag() { return make_tag(kShowFrame, {}); }
GfxTag end_tag() { return make_tag(kEnd, {}); }

GfxTag frame_label_tag(const std::string& label) {
    Bytes b(label.begin(), label.end());
    b.push_back(0);
    return make_tag(kFrameLabel, std::move(b));
}

std::string read_frame_label(const GfxTag& t) {
    if (t.code != kFrameLabel) throw Error("gfx: not a FrameLabel");
    BitReader r(t.payload);
    return r.cstr();
}

GfxTag export_assets_tag(const std::vector<std::pair<uint16_t, std::string>>& symbols) {
    BitWriter w;
    w.u16((uint16_t)symbols.size());
    for (const auto& [id, name] : symbols) { w.u16(id); w.cstr(name); }
    return make_tag(kExportAssets, std::move(w.out));
}

GfxTag background_tag(Rgba c) { return make_tag(kSetBackgroundColor, {c.r, c.g, c.b}); }

GfxTag file_attributes_tag(uint32_t flags) {
    BitWriter w;
    w.u32(flags);
    return make_tag(kFileAttributes, std::move(w.out));
}

ExporterInfo read_exporter_info(const GfxTag& t) {
    if (t.code != kExporterInfo) throw Error("gfx: not an ExporterInfo tag");
    BitReader r(t.payload);
    ExporterInfo e;
    e.version = r.u16();
    e.flags = r.u32();
    e.bitmap_format = r.u16();
    auto pstr = [&]() {
        const uint8_t n = r.u8();
        std::string s;
        for (int i = 0; i < n; i++) s.push_back((char)r.u8());
        return s;
    };
    e.prefix = pstr();
    e.swf_name = pstr();
    if (r.byte() + 2 <= t.payload.size()) {
        const uint16_t n = r.u16();
        for (int i = 0; i < n; i++) e.code_offsets.push_back(r.u32());
    }
    return e;
}

GfxTag exporter_info_tag(const ExporterInfo& e) {
    if (e.prefix.size() > 255 || e.swf_name.size() > 255) throw Error("gfx: an ExporterInfo name over 255 bytes");
    BitWriter w;
    w.u16(e.version);
    w.u32(e.flags);
    w.u16(e.bitmap_format);
    w.u8((uint8_t)e.prefix.size());
    for (char c : e.prefix) w.u8((uint8_t)c);
    w.u8((uint8_t)e.swf_name.size());
    for (char c : e.swf_name) w.u8((uint8_t)c);
    w.u16((uint16_t)e.code_offsets.size());
    for (uint32_t o : e.code_offsets) w.u32(o);
    return make_tag(kExporterInfo, std::move(w.out));
}

Bytes frame_header(const Rect& frame, double rate, uint16_t frames) {
    BitWriter w;
    write_rect(w, frame);
    const int fixed = (int)std::lround(rate * 256);
    w.u8((uint8_t)(fixed & 0xFF));
    w.u8((uint8_t)(fixed >> 8));
    w.u16(frames);
    return w.out;
}

Rect frame_rect(const GfxMovie& m) {
    BitReader r(m.frame);
    return read_rect(r);
}

TagWalk all_tags(const GfxMovie& m) {
    TagWalk out;
    std::function<void(const std::vector<GfxTag>&, int, uint16_t)> walk = [&](const std::vector<GfxTag>& tags, int depth, uint16_t id) {
        for (const GfxTag& t : tags) {
            out.tags.push_back({&t, depth, id});
            if (t.code == kDefineSprite) {
                Sprite s = read_sprite(t);
                out.sprites.push_back(std::make_unique<std::vector<GfxTag>>(std::move(s.tags)));
                const std::vector<GfxTag>& inner = *out.sprites.back();
                walk(inner, depth + 1, s.id);
            }
        }
    };
    walk(m.tags, 0, 0);
    return out;
}

// ---- interior minimaps

namespace {

size_t tag_size(const GfxTag& t) { return (t.long_form || t.payload.size() >= 0x3F ? 6 : 2) + t.payload.size(); }

std::string place_name(const GfxTag& t) {
    if (t.code != kPlaceObject2 && t.code != kPlaceObject3) return {};
    return read_place(t).name;
}

}  // namespace

std::string interior_minimap_name(const std::string& archetype) { return "int" + std::to_string(joaat_lower(archetype)); }

InteriorMinimapTemplate interior_minimap_template(const GfxMovie& m) {
    InteriorMinimapTemplate t;
    t.frame = m.frame;
    bool interior = false, have_exporter = false, have_action = false;
    for (size_t i = 0; i < m.tags.size(); i++) {
        const GfxTag& g = m.tags[i];
        switch (g.code) {
        case kExporterInfo: t.exporter = read_exporter_info(g); have_exporter = true; break;
        case kFileAttributes: t.file_attributes = g; break;
        case kMetadata: t.metadata = g; break;
        case kSetBackgroundColor: t.background = g; break;
        case 12: if (!have_action) { t.do_action = g; have_action = true; } break;   // DoAction
        case kExportAssets:
            if (g.payload.size() > 4 && i + 1 < m.tags.size() && m.tags[i + 1].code == 59) {   // a class: exported as __Packages.<name>, its DoInitAction next
                const std::string name((const char*)g.payload.data() + 4);
                if (name.rfind("__Packages.", 0) == 0) t.classes.push_back({name, m.tags[i + 1]});
            }
            break;
        default:
            if (place_name(g) == "interior") interior = true;
        }
    }
    if (!have_exporter || !have_action || t.classes.empty() || !interior || t.file_attributes.payload.empty())
        throw Error("gfx: not laid out as an interior minimap (ExporterInfo, DoAction, the classes and the `interior` placement)");
    for (const auto& c : t.classes)
        if (c.init_action.payload.size() < 2) throw Error("gfx: a DoInitAction without its sprite");
    return t;
}

GfxMovie make_interior_minimap(const InteriorMinimapTemplate& t, const std::string& archetype, std::vector<InteriorMinimapLevel> levels, int32_t scale) {
    if (levels.empty()) throw Error("gfx: an interior minimap without a level");
    GfxMovie mv;
    mv.version = 8;
    mv.frame = t.frame;
    const uint16_t frames = (uint16_t)(t.frame.size() >= 2 ? t.frame[t.frame.size() - 2] | t.frame[t.frame.size() - 1] << 8 : 20);
    std::vector<GfxTag> tags;   // everything after the ExporterInfo
    tags.push_back(t.file_attributes);
    if (!t.metadata.payload.empty()) tags.push_back(t.metadata);
    if (!t.background.payload.empty()) tags.push_back(t.background);
    const uint16_t n = (uint16_t)levels.size(), holder = (uint16_t)(2 * n + 1);
    for (uint16_t i = 0; i < n; i++) {
        levels[i].shape.id = (uint16_t)(2 * i + 1);
        tags.push_back(shape_tag(levels[i].shape));
        Sprite s;
        s.id = (uint16_t)(2 * i + 2);
        Place p;
        p.depth = levels[i].shape_depth;
        p.character = levels[i].shape.id;
        p.matrix = Matrix{};
        s.tags = {place_tag(p), show_frame_tag(), end_tag()};
        tags.push_back(sprite_tag(s));
    }
    Sprite h;
    h.id = holder;
    for (uint16_t i = 0; i < n; i++) {
        Place p;
        p.depth = (uint16_t)(i + 1);
        p.character = (uint16_t)(2 * i + 2);
        p.matrix = Matrix{};
        ColourTransform c;
        c.has_mul = true;
        c.mul[3] = 0;   // hidden: the game shows the player's level
        p.colour = c;
        p.name = levels[i].name;
        h.tags.push_back(place_tag(p));
    }
    h.tags.push_back(show_frame_tag());
    h.tags.push_back(end_tag());
    tags.push_back(sprite_tag(h));
    tags.push_back(export_assets_tag({{holder, archetype}}));
    std::vector<size_t> code_tags = {tags.size()};
    tags.push_back(t.do_action);
    for (size_t k = 0; k < t.classes.size(); k++) {
        const uint16_t id = (uint16_t)(holder + 1 + k);
        Sprite e;
        e.id = id;
        e.frame_count = 0;
        e.tags = {end_tag()};
        tags.push_back(sprite_tag(e));
        tags.push_back(export_assets_tag({{id, t.classes[k].name}}));
        GfxTag init = t.classes[k].init_action;
        init.payload[0] = (uint8_t)id;
        init.payload[1] = (uint8_t)(id >> 8);
        code_tags.push_back(tags.size());
        tags.push_back(std::move(init));
    }
    tags.push_back(show_frame_tag());
    tags.push_back(frame_label_tag("INT" + std::to_string(joaat_lower(archetype))));
    Place ip;
    ip.depth = 1;
    ip.character = holder;
    Matrix m;
    m.has_scale = true;
    m.sx = m.sy = scale;
    ip.matrix = m;
    ip.ratio = 1;
    ip.name = "interior";
    tags.push_back(place_tag(ip));
    for (int f = 1; f < frames; f++) tags.push_back(show_frame_tag());
    tags.push_back(end_tag());
    // Flash writes the length of these in the long form even when short: sprites, exports, frame labels and named placements (in the sprites too)
    std::function<void(GfxTag&)> long_forms = [&](GfxTag& g) {
        if (g.code == kDefineSprite) {
            Sprite s = read_sprite(g);
            for (GfxTag& in : s.tags) long_forms(in);
            g = sprite_tag(s);
            g.long_form = true;
        } else if (g.code == kExportAssets || g.code == kFrameLabel || (g.code == kPlaceObject2 && !place_name(g).empty())) {
            g.long_form = true;
        }
    };
    for (GfxTag& g : tags) long_forms(g);
    // the code offsets: each ActionScript payload's offset from the end of the header, as if the ExporterInfo were not there
    ExporterInfo ei = t.exporter;
    ei.prefix.clear();
    ei.swf_name = interior_minimap_name(archetype);
    ei.code_offsets.clear();
    size_t at = t.frame.size(), next = 0;
    for (size_t i = 0; i < tags.size(); i++) {
        if (next < code_tags.size() && code_tags[next] == i) { ei.code_offsets.push_back((uint32_t)(at + tag_size(tags[i]) - tags[i].payload.size())); next++; }
        at += tag_size(tags[i]);
    }
    mv.tags.push_back(exporter_info_tag(ei));
    mv.tags.insert(mv.tags.end(), tags.begin(), tags.end());
    return mv;
}

std::string interior_minimap_skeleton(const GfxMovie& m) {
    std::vector<std::string> tok;
    for (size_t i = 0; i < m.tags.size(); i++) {
        const GfxTag& g = m.tags[i];
        const bool shape = g.code == kDefineShape || g.code == kDefineShape2 || g.code == kDefineShape3 || g.code == kDefineShape4;
        if (shape && i + 1 < m.tags.size() && m.tags[i + 1].code == kDefineSprite) {   // a level: its shape and the sprite holding it
            if (tok.empty() || tok.back() != "L") tok.push_back("L");
            i++;
            continue;
        }
        std::string s = std::to_string(g.code);
        if (g.code == kDefineSprite) {
            const Sprite sp = read_sprite(g);
            s += "[";
            std::string last;
            for (const GfxTag& in : sp.tags) {
                const std::string c = std::to_string(in.code);
                if (c == last && c == "26") continue;   // the levels' placements: one per level
                s += (s.back() == '[' ? "" : " ") + c;
                last = c;
            }
            s += "]";
        }
        if (g.code == kShowFrame && !tok.empty() && (tok.back() == "1" || tok.back() == "1+")) { tok.back() = "1+"; continue; }
        tok.push_back(s);
    }
    std::string out;
    for (const std::string& t : tok) out += (out.empty() ? "" : " ") + t;
    return out;
}

std::vector<std::string> interior_minimap_levels(const GfxMovie& m) {
    std::vector<std::string> out;
    std::map<uint16_t, Sprite> sprites;
    uint16_t holder = 0;
    for (const GfxTag& g : m.tags) {
        if (g.code == kDefineSprite) { Sprite s = read_sprite(g); sprites[s.id] = s; }
        else if (g.code == kPlaceObject2 || g.code == kPlaceObject3) { const Place p = read_place(g); if (p.name == "interior" && p.character) holder = *p.character; }
    }
    if (auto s = sprites.find(holder); s != sprites.end())
        for (const GfxTag& g : s->second.tags) if (const std::string n = place_name(g); !n.empty()) out.push_back(n);
    return out;
}

}  // namespace rk::gfx
