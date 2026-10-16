// Rebuilding part of a navmesh cell from collision (navbuild.h). Polygon booleans and offsets come from the vendored
// Clipper2.
#include "ragekit/navbuild.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <unordered_map>
#include "clipper2/clipper.h"
#include "ragekit/error.h"

namespace rk {

namespace C2 = Clipper2Lib;

// ---------------------------------------------------------------- collision input

namespace {

constexpr uint32_t kMapDynamic = 1u << 2, kMapStairs = 1u << 30;   // composite child flags kept besides 0

struct Xf {
    Vec3 col[4]{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, 0}};
    Vec3 operator()(const Vec3& p) const { return col[0] * p.x + col[1] * p.y + col[2] * p.z + col[3]; }
    Xf then(const BoundTransform& t) const {
        Xf o;
        for (int k = 0; k < 3; k++) o.col[k] = col[0] * t.col[k].x + col[1] * t.col[k].y + col[2] * t.col[k].z;
        o.col[3] = col[0] * t.col[3].x + col[1] * t.col[3].y + col[2] * t.col[3].z + col[3];
        return o;
    }
};

Vec3 cross(const Vec3& a, const Vec3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

/// A closed primitive's triangles wound outwards: flipped when their signed volume is negative.
void add_closed(std::vector<NavCollision::Triangle>& out, const std::vector<BoundTriangle>& tris, const Xf& xf, const AABB* clip) {
    if (tris.empty()) return;
    double vol = 0;
    std::vector<NavCollision::Triangle> t;
    t.reserve(tris.size());
    for (const BoundTriangle& b : tris) {
        NavCollision::Triangle n{{xf(b.v[0]), xf(b.v[1]), xf(b.v[2])}, true};
        vol += dot(n.v[0], cross(n.v[1], n.v[2]));
        t.push_back(n);
    }
    for (auto& n : t) {
        if (vol < 0) std::swap(n.v[1], n.v[2]);
        if (clip) {
            const Vec3 mn = vmin(vmin(n.v[0], n.v[1]), n.v[2]), mx = vmax(vmax(n.v[0], n.v[1]), n.v[2]);
            if (mx.x < clip->min.x || mn.x > clip->max.x || mx.y < clip->min.y || mn.y > clip->max.y || mx.z < clip->min.z || mn.z > clip->max.z) continue;
        }
        out.push_back(n);
    }
}

void add_bound_at(NavCollision& c, const Bound& b, const Xf& xf, const AABB* clip) {
    switch (b.type) {
        case BoundType::Composite:
            for (const BoundChild& ch : b.children) {
                if (!ch.bound) continue;
                if (b.has_child_flags && ch.flags1 != 0 && (ch.flags1 & (kMapDynamic | kMapStairs)) == 0) continue;   // foliage, cover-only ...
                add_bound_at(c, *ch.bound, xf.then(ch.transform), clip);
            }
            return;
        case BoundType::Geometry:
        case BoundType::GeometryBVH: {
            if (!b.geometry) return;
            const Bound::Geometry& g = *b.geometry;
            for (size_t i = 0; i < g.polygons.size(); i++) {
                const auto tris = polygon_triangles(g, i, 8);
                if (g.polygons[i].kind != BoundPolygon::Triangle) { add_closed(c.triangles, tris, xf, clip); continue; }
                for (const BoundTriangle& t : tris) {
                    NavCollision::Triangle n{{xf(t.v[0]), xf(t.v[1]), xf(t.v[2])}, false};
                    if (clip) {
                        const Vec3 mn = vmin(vmin(n.v[0], n.v[1]), n.v[2]), mx = vmax(vmax(n.v[0], n.v[1]), n.v[2]);
                        if (mx.x < clip->min.x || mn.x > clip->max.x || mx.y < clip->min.y || mn.y > clip->max.y || mx.z < clip->min.z || mn.z > clip->max.z) continue;
                    }
                    c.triangles.push_back(n);
                }
            }
            return;
        }
        default:   // a whole primitive
            add_closed(c.triangles, bound_triangles(b, 8), xf, clip);
            return;
    }
}

}   // namespace

void NavCollision::add_bound(const Bound& b, const AABB* clip) { add_bound_at(*this, b, Xf{}, clip); }

// ---------------------------------------------------------------- 2D geometry

namespace {

constexpr int kPrec = 3;          // millimetres
constexpr uint32_t kNoPoly = 0x3FFF;
constexpr double kSnap = 0.02;    // corners meet on a 2 cm grid (the game's stored ones sit ~1 mm off round values)

using Area = C2::PathsD;          // rings, outer counter-clockwise, holes clockwise (non-zero fill)
using Ring = C2::PathD;

struct P3 { double x = 0, y = 0, z = 0; };
using Key = std::pair<int64_t, int64_t>;
// shifted by 5 mm: corners on the 0.25 m grid (12.5 steps) then sit a quarter step from a rounding boundary, not on one
Key key(double x, double y) { return {(int64_t)std::llround((x + 0.005) / kSnap), (int64_t)std::llround((y + 0.005) / kSnap)}; }
Key key(const P3& p) { return key(p.x, p.y); }

Area uni(const Area& a) { return C2::Union(a, C2::FillRule::NonZero, kPrec); }
Area inter(const Area& a, const Area& b) { return C2::Intersect(a, b, C2::FillRule::NonZero, kPrec); }
Area diff(const Area& a, const Area& b) { return C2::Difference(a, b, C2::FillRule::NonZero, kPrec); }
Area grow(const Area& a, double d, C2::JoinType j = C2::JoinType::Miter, int prec = kPrec) {
    return C2::InflatePaths(a, d, j, C2::EndType::Polygon, 2.0, prec);
}
void append(Area& to, const Area& a) { to.insert(to.end(), a.begin(), a.end()); }

double ring_area(const Ring& r) {
    double s = 0;
    for (size_t i = 0, n = r.size(); i < n; i++) s += r[i].x * r[(i + 1) % n].y - r[(i + 1) % n].x * r[i].y;
    return s / 2;
}
double area_of(const Area& a) { double s = 0; for (auto& r : a) s += ring_area(r); return std::fabs(s); }
double area2(const std::vector<P3>& p) {
    double s = 0;
    for (size_t i = 0, n = p.size(); i < n; i++) s += p[i].x * p[(i + 1) % n].y - p[(i + 1) % n].x * p[i].y;
    return s / 2;
}
Ring ring_of(const std::vector<P3>& p) { Ring r; for (auto& q : p) r.push_back({q.x, q.y}); return r; }
Area area_of_poly(const std::vector<P3>& p) {
    Ring r = ring_of(p);
    if (ring_area(r) < 0) std::reverse(r.begin(), r.end());
    return uni(Area{r});
}

/// The separate polygons of an area, each as its outer ring and holes.
std::vector<Area> parts(const Area& a) {
    std::vector<Area> out;
    if (a.empty()) return out;
    C2::ClipperD c(kPrec);
    c.AddSubject(a);
    C2::PolyTreeD tree;
    c.Execute(C2::ClipType::Union, C2::FillRule::NonZero, tree);
    std::vector<const C2::PolyPathD*> todo{&tree};
    while (!todo.empty()) {
        const C2::PolyPathD* n = todo.back();
        todo.pop_back();
        for (size_t i = 0; i < n->Count(); i++) {
            const C2::PolyPathD* outer = n->Child(i);
            Area p{outer->Polygon()};
            for (size_t h = 0; h < outer->Count(); h++) {
                p.push_back(outer->Child(h)->Polygon());
                todo.push_back(outer->Child(h));   // islands inside the hole
            }
            out.push_back(std::move(p));
        }
    }
    return out;
}

bool inside(const Area& a, double x, double y) {
    int n = 0;
    for (auto& r : a)
        if (C2::PointInPolygon(C2::PointD{x, y}, r) == C2::PointInPolygonResult::IsInside) n++;
    return n & 1;
}

double seg_dist(double px, double py, double ax, double ay, double bx, double by) {
    const double dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
    double t = l2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0;
    t = std::clamp(t, 0.0, 1.0);
    const double qx = ax + t * dx - px, qy = ay + t * dy - py;
    return std::sqrt(qx * qx + qy * qy);
}
double point_dist(const Area& a, double x, double y) {
    if (inside(a, x, y)) return 0;
    double best = 1e30;
    for (auto& r : a)
        for (size_t i = 0, n = r.size(); i < n; i++) best = std::min(best, seg_dist(x, y, r[i].x, r[i].y, r[(i + 1) % n].x, r[(i + 1) % n].y));
    return best;
}
double area_dist(const Area& a, const Area& b) {
    double best = 1e30;
    for (auto& r : a) for (auto& p : r) { best = std::min(best, point_dist(b, p.x, p.y)); if (best == 0) return 0; }
    for (auto& r : b) for (auto& p : r) { best = std::min(best, point_dist(a, p.x, p.y)); if (best == 0) return 0; }
    return best;
}

/// The length along which two sets of rings run over each other (collinear overlap within 1 mm).
double shared_length(const Area& a, const Area& b) {
    double len = 0;
    for (auto& r : a)
        for (size_t i = 0, n = r.size(); i < n; i++) {
            const C2::PointD p = r[i], q = r[(i + 1) % n];
            const double dx = q.x - p.x, dy = q.y - p.y, L = std::sqrt(dx * dx + dy * dy);
            if (L < 1e-6) continue;
            const double ux = dx / L, uy = dy / L;
            for (auto& s : b)
                for (size_t j = 0, m = s.size(); j < m; j++) {
                    const C2::PointD c = s[j], d = s[(j + 1) % m];
                    if (std::fabs((c.x - p.x) * uy - (c.y - p.y) * ux) > 1e-3 || std::fabs((d.x - p.x) * uy - (d.y - p.y) * ux) > 1e-3) continue;
                    const double tc = (c.x - p.x) * ux + (c.y - p.y) * uy, td = (d.x - p.x) * ux + (d.y - p.y) * uy;
                    len += std::max(0.0, std::min(L, std::max(tc, td)) - std::max(0.0, std::min(tc, td)));
                }
        }
    return len;
}

struct Box2 {
    double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
    void add(double x, double y) { x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y); }
    bool meets(const Box2& o, double g = 0) const { return x0 <= o.x1 + g && x1 >= o.x0 - g && y0 <= o.y1 + g && y1 >= o.y0 - g; }
};
Box2 box_of(const Area& a) { Box2 b; for (auto& r : a) for (auto& p : r) b.add(p.x, p.y); return b; }
Box2 box_of(const std::vector<P3>& p) { Box2 b; for (auto& q : p) b.add(q.x, q.y); return b; }

/// Least-squares plane z = a x + b y + c; `residual` = largest |plane - z|.
std::array<double, 3> fit(const std::vector<P3>& pts, double* residual) {
    double mx = 0, my = 0;
    for (auto& p : pts) { mx += p.x; my += p.y; }
    mx /= (double)pts.size(); my /= (double)pts.size();
    double sxx = 0, sxy = 0, syy = 0, sxz = 0, syz = 0, sz = 0;
    for (auto& p : pts) {
        const double x = p.x - mx, y = p.y - my;
        sxx += x * x; sxy += x * y; syy += y * y; sxz += x * p.z; syz += y * p.z; sz += p.z;
    }
    const double det = sxx * syy - sxy * sxy, zm = sz / (double)pts.size();
    double a = 0, b = 0;
    if (std::fabs(det) > 1e-12) { a = (sxz * syy - syz * sxy) / det; b = (syz * sxx - sxz * sxy) / det; }
    const double c = zm - a * mx - b * my;
    if (residual) {
        *residual = 0;
        for (auto& p : pts) *residual = std::max(*residual, std::fabs(a * p.x + b * p.y + c - p.z));
    }
    return {a, b, c};
}

bool convex(const std::vector<P3>& p) {
    for (size_t i = 0, n = p.size(); i < n; i++) {
        const P3 &a = p[i], &b = p[(i + 1) % n], &c = p[(i + 2) % n];
        if ((b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x) < -1e-9) return false;
    }
    return true;
}

using EdgeKey = std::pair<Key, Key>;
EdgeKey unordered(const Key& a, const Key& b) { return a <= b ? EdgeKey{a, b} : EdgeKey{b, a}; }

/// Two counter-clockwise polygons sharing side `e` as one ring, collinear corners dropped; empty when they don't share it.
std::vector<P3> merge(const std::vector<P3>& a, const std::vector<P3>& b, const EdgeKey& e) {
    int ia = -1, jb = -1;
    for (size_t i = 0; i < a.size() && ia < 0; i++) if (unordered(key(a[i]), key(a[(i + 1) % a.size()])) == e) ia = (int)i;
    for (size_t j = 0; j < b.size() && jb < 0; j++) if (unordered(key(b[j]), key(b[(j + 1) % b.size()])) == e) jb = (int)j;
    if (ia < 0 || jb < 0) return {};
    std::vector<P3> m;
    for (size_t k = 0; k < a.size(); k++) m.push_back(a[(ia + 1 + k) % a.size()]);               // after the side .. a[ia]
    for (size_t k = 2; k < b.size(); k++) m.push_back(b[(jb + k) % b.size()]);                  // b's corners off the side
    std::vector<P3> o;
    for (size_t i = 0, n = m.size(); i < n; i++) {
        const P3 &p = m[(i + n - 1) % n], &c = m[i], &q = m[(i + 1) % n];
        if (std::fabs((c.x - p.x) * (q.y - c.y) - (c.y - p.y) * (q.x - c.x)) > 1e-7) o.push_back(c);
    }
    return o.size() >= 3 ? o : std::vector<P3>{};
}

// ---------------------------------------------------------------- triangulation

double orient(const P3& a, const P3& b, const P3& c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); }
bool same_xy(const P3& a, const P3& b) { return std::fabs(a.x - b.x) < 1e-9 && std::fabs(a.y - b.y) < 1e-9; }
bool segments_cross(const P3& a, const P3& b, const P3& c, const P3& d) {   // proper crossing, shared ends excluded
    if (same_xy(a, c) || same_xy(a, d) || same_xy(b, c) || same_xy(b, d)) return false;
    const double d1 = orient(a, b, c), d2 = orient(a, b, d), d3 = orient(c, d, a), d4 = orient(c, d, b);
    return ((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0));
}

/// One ring split where it touches itself (Clipper's output may pinch at a corner) into simple loops.
void split_pinches(std::vector<P3> ring, std::vector<std::vector<P3>>& out) {
    for (size_t i = 0; i < ring.size(); i++)
        for (size_t j = i + 1; j < ring.size(); j++)
            if (same_xy(ring[i], ring[j])) {
                std::vector<P3> loop(ring.begin() + (ptrdiff_t)i, ring.begin() + (ptrdiff_t)j), rest(ring.begin(), ring.begin() + (ptrdiff_t)i);
                rest.insert(rest.end(), ring.begin() + (ptrdiff_t)j, ring.end());
                split_pinches(std::move(loop), out);
                split_pinches(std::move(rest), out);
                return;
            }
    if (ring.size() >= 3 && std::fabs(area2(ring)) > 1e-9) out.push_back(std::move(ring));
}

/// A simple counter-clockwise ring with clockwise holes inside it as counter-clockwise triangles: the holes bridged
/// into the ring, ears clipped, then edges flipped until the triangulation is Delaunay inside the polygon's own sides
/// (a constrained Delaunay triangulation). Empty when it fails.
std::vector<std::vector<P3>> triangulate_simple(std::vector<P3> outer, std::vector<std::vector<P3>> holes) {
    if (outer.size() < 3) return {};
    std::vector<std::pair<P3, P3>> own_sides;
    auto add_sides = [&](const std::vector<P3>& r) { for (size_t i = 0; i < r.size(); i++) own_sides.push_back({r[i], r[(i + 1) % r.size()]}); };
    add_sides(outer);
    for (auto& h : holes) add_sides(h);
    // holes in order of their rightmost corner, each joined to a corner of the ring it can see
    std::sort(holes.begin(), holes.end(), [](const auto& a, const auto& b) {
        double ma = -1e300, mb = -1e300;
        for (auto& p : a) ma = std::max(ma, p.x);
        for (auto& p : b) mb = std::max(mb, p.x);
        return ma > mb;
    });
    for (size_t h = 0; h < holes.size(); h++) {
        const auto& hole = holes[h];
        size_t hm = 0;
        for (size_t i = 1; i < hole.size(); i++) if (hole[i].x > hole[hm].x) hm = i;
        const P3 m = hole[hm];
        auto visible = [&](const P3& q) {
            for (size_t i = 0; i < outer.size(); i++) if (segments_cross(m, q, outer[i], outer[(i + 1) % outer.size()])) return false;
            for (size_t k = h; k < holes.size(); k++)
                for (size_t i = 0; i < holes[k].size(); i++) if (segments_cross(m, q, holes[k][i], holes[k][(i + 1) % holes[k].size()])) return false;
            return true;
        };
        std::vector<size_t> order(outer.size());
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return std::hypot(outer[a].x - m.x, outer[a].y - m.y) < std::hypot(outer[b].x - m.x, outer[b].y - m.y);
        });
        size_t best = SIZE_MAX;
        for (size_t c : order) {
            // the corner's own wedge must hold the bridge, so the joined ring does not cross itself
            const P3 &prev = outer[(c + outer.size() - 1) % outer.size()], &q = outer[c], &next = outer[(c + 1) % outer.size()];
            const bool convex_corner = orient(prev, q, next) > 0;
            const double a1 = orient(prev, q, m), a2 = orient(q, next, m);
            if (convex_corner ? !(a1 > 0 && a2 > 0) : !(a1 > 0 || a2 > 0)) continue;
            if (visible(q)) { best = c; break; }
        }
        if (best == SIZE_MAX) return {};
        std::vector<P3> joined(outer.begin(), outer.begin() + (ptrdiff_t)best + 1);
        for (size_t k = 0; k <= hole.size(); k++) joined.push_back(hole[(hm + k) % hole.size()]);
        joined.push_back(outer[best]);
        joined.insert(joined.end(), outer.begin() + (ptrdiff_t)best + 1, outer.end());
        outer = std::move(joined);
    }
    // ear clipping
    std::vector<std::array<P3, 3>> tris;
    std::vector<P3> v = outer;
    auto inside_tri = [](const P3& p, const P3& a, const P3& b, const P3& c) {
        if (same_xy(p, a) || same_xy(p, b) || same_xy(p, c)) return false;
        return orient(a, b, p) >= 0 && orient(b, c, p) >= 0 && orient(c, a, p) >= 0;
    };
    size_t guard = v.size() * v.size() + 16;
    while (v.size() > 3 && guard-- > 0) {
        const size_t n = v.size();
        size_t ear = SIZE_MAX;
        double best_q = -1;
        for (size_t i = 0; i < n; i++) {
            const P3 &a = v[(i + n - 1) % n], &b = v[i], &c = v[(i + 1) % n];
            if (orient(a, b, c) <= 1e-12) continue;
            bool empty = true;
            for (size_t k = 0; k < n && empty; k++)
                if (k != i && k != (i + n - 1) % n && k != (i + 1) % n && inside_tri(v[k], a, b, c)) empty = false;
            if (!empty) continue;
            // the best shaped ear first (the smallest angle largest), so few slivers are left for the flips
            const double la = std::hypot(b.x - a.x, b.y - a.y), lb = std::hypot(c.x - b.x, c.y - b.y), lc = std::hypot(a.x - c.x, a.y - c.y);
            const double q = orient(a, b, c) / std::max(1e-12, la * la + lb * lb + lc * lc);
            if (q > best_q) { best_q = q; ear = i; }
        }
        if (ear == SIZE_MAX) {   // only degenerate corners left: drop a collinear one
            size_t flat = SIZE_MAX;
            for (size_t i = 0; i < n && flat == SIZE_MAX; i++)
                if (std::fabs(orient(v[(i + n - 1) % n], v[i], v[(i + 1) % n])) <= 1e-12) flat = i;
            if (flat == SIZE_MAX) return {};
            v.erase(v.begin() + (ptrdiff_t)flat);
            continue;
        }
        tris.push_back({v[(ear + n - 1) % n], v[ear], v[(ear + 1) % n]});
        v.erase(v.begin() + (ptrdiff_t)ear);
    }
    if (v.size() == 3 && orient(v[0], v[1], v[2]) > 1e-12) tris.push_back({v[0], v[1], v[2]});
    if (tris.empty()) return {};
    // Delaunay flips across sides that are not the polygon's own
    std::set<EdgeKey> fixed;
    for (auto& [a, b] : own_sides) fixed.insert(unordered(key(a), key(b)));
    auto in_circle = [](const P3& a, const P3& b, const P3& c, const P3& d) {
        const double ax = a.x - d.x, ay = a.y - d.y, bx = b.x - d.x, by = b.y - d.y, cx = c.x - d.x, cy = c.y - d.y;
        return (ax * ax + ay * ay) * (bx * cy - cx * by) - (bx * bx + by * by) * (ax * cy - cx * ay) + (cx * cx + cy * cy) * (ax * by - bx * ay) > 1e-9;
    };
    for (int pass = 0; pass < 50; pass++) {
        bool flipped = false;
        std::map<EdgeKey, std::vector<std::pair<size_t, int>>> sides;
        for (size_t t = 0; t < tris.size(); t++)
            for (int e = 0; e < 3; e++) sides[unordered(key(tris[t][e]), key(tris[t][(e + 1) % 3]))].push_back({t, e});
        std::vector<uint8_t> touched(tris.size());
        for (auto& [ek, ts] : sides) {
            if (ts.size() != 2 || fixed.count(ek)) continue;
            auto [t1, e1] = ts[0];
            auto [t2, e2] = ts[1];
            if (touched[t1] || touched[t2]) continue;
            const P3 a = tris[t1][e1], b = tris[t1][(e1 + 1) % 3], c = tris[t1][(e1 + 2) % 3], d = tris[t2][(e2 + 2) % 3];
            if (!in_circle(a, b, c, d)) continue;
            if (orient(c, a, d) <= 1e-12 || orient(d, b, c) <= 1e-12) continue;   // the quad must be convex
            tris[t1] = {c, a, d};
            tris[t2] = {d, b, c};
            touched[t1] = touched[t2] = 1;
            flipped = true;
        }
        if (!flipped) break;
    }
    std::vector<std::vector<P3>> out;
    for (auto& t : tris) if (std::fabs(orient(t[0], t[1], t[2])) > 2e-6) out.push_back({t[0], t[1], t[2]});
    return out;
}

/// A polygon with holes (outer ring first, as parts() gives them) as counter-clockwise triangles (empty when it fails).
std::vector<std::vector<P3>> triangulate(const Area& poly) {
    std::vector<std::vector<P3>> outers, holes;
    for (size_t k = 0; k < poly.size(); k++) {
        std::vector<P3> v;
        for (auto& p : poly[k]) if (v.empty() || !same_xy(v.back(), {p.x, p.y, 0})) v.push_back({p.x, p.y, 0});
        while (v.size() > 1 && same_xy(v.front(), v.back())) v.pop_back();
        std::vector<std::vector<P3>> loops;
        split_pinches(std::move(v), loops);
        for (auto& l : loops) (area2(l) > 0 ? outers : holes).push_back(std::move(l));
    }
    std::vector<std::vector<std::vector<P3>>> held(outers.size());
    for (auto& h : holes) {
        // the loop holding a corner of the hole that is not on its own ring
        int owner = -1;
        for (size_t o = 0; o < outers.size() && owner < 0; o++)
            for (const P3& q : h) {
                bool on = false;
                for (const P3& c : outers[o]) if (same_xy(c, q)) { on = true; break; }
                if (on) continue;
                if (inside(Area{ring_of(outers[o])}, q.x, q.y)) owner = (int)o;
                break;
            }
        if (owner < 0) return {};
        held[owner].push_back(std::move(h));
    }
    std::vector<std::vector<P3>> out;
    for (size_t o = 0; o < outers.size(); o++) {
        auto t = triangulate_simple(outers[o], held[o]);
        if (t.empty()) return {};
        out.insert(out.end(), t.begin(), t.end());
    }
    return out;
}

/// triangulate(), and where that fails the two halves of the polygon (split across its longer side), `depth` times.
std::vector<std::vector<P3>> triangulate_or_halve(const Area& poly, int depth) {
    auto t = triangulate(poly);
    if (!t.empty() || depth == 0) return t;
    const Box2 b = box_of(poly);
    Area h1, h2;
    if (b.x1 - b.x0 >= b.y1 - b.y0) {
        const double m = std::round((b.x0 + b.x1) / 2 * 4) / 4;
        h1 = {Ring{{b.x0 - 1, b.y0 - 1}, {m, b.y0 - 1}, {m, b.y1 + 1}, {b.x0 - 1, b.y1 + 1}}};
        h2 = {Ring{{m, b.y0 - 1}, {b.x1 + 1, b.y0 - 1}, {b.x1 + 1, b.y1 + 1}, {m, b.y1 + 1}}};
    } else {
        const double m = std::round((b.y0 + b.y1) / 2 * 4) / 4;
        h1 = {Ring{{b.x0 - 1, b.y0 - 1}, {b.x1 + 1, b.y0 - 1}, {b.x1 + 1, m}, {b.x0 - 1, m}}};
        h2 = {Ring{{b.x0 - 1, m}, {b.x1 + 1, m}, {b.x1 + 1, b.y1 + 1}, {b.x0 - 1, b.y1 + 1}}};
    }
    std::vector<std::vector<P3>> out;
    for (const Area& h : {h1, h2})
        for (const Area& part : parts(inter(poly, h))) {
            if (area_of(part) < 1e-4) continue;
            auto pt = triangulate_or_halve(part, depth - 1);
            if (pt.empty()) return {};
            out.insert(out.end(), pt.begin(), pt.end());
        }
    return out;
}

// ---------------------------------------------------------------- the height field

struct Hit { float z, nz; bool solid; };

struct Field {
    double x0 = 0, y0 = 0, st = 0.25;   // the first sample, spacing
    int nx = 0, ny = 0;
    std::vector<std::vector<float>> floors;   // per sample, top first
    double X(int i) const { return x0 + i * st; }
    double Y(int j) const { return y0 + j * st; }
    size_t at(int i, int j) const { return (size_t)j * nx + i; }
};

/// Every floor under each sample: a surface no steeper than 45 degrees, facing up, not inside a solid, with `head`
/// clear to the next surface above.
Field sample(const NavCollision& col, const AABB& ext, double st, double head) {
    Field f;
    f.st = st;
    f.x0 = std::floor(ext.min.x / st) * st + st / 2;
    f.y0 = std::floor(ext.min.y / st) * st + st / 2;
    f.nx = (int)std::ceil((ext.max.x - f.x0) / st) + 1;
    f.ny = (int)std::ceil((ext.max.y - f.y0) / st) + 1;
    f.floors.assign((size_t)f.nx * f.ny, {});
    std::vector<std::vector<uint32_t>> bins((size_t)f.nx * f.ny);
    for (uint32_t t = 0; t < col.triangles.size(); t++) {
        const auto& v = col.triangles[t].v;
        const double a2 = (v[1].x - v[0].x) * (v[2].y - v[0].y) - (v[1].y - v[0].y) * (v[2].x - v[0].x);
        if (std::fabs(a2) < 1e-9) continue;   // edge-on: a vertical ray never meets it
        const double mnx = std::min({v[0].x, v[1].x, v[2].x}), mxx = std::max({v[0].x, v[1].x, v[2].x});
        const double mny = std::min({v[0].y, v[1].y, v[2].y}), mxy = std::max({v[0].y, v[1].y, v[2].y});
        const int i0 = std::max(0, (int)std::ceil((mnx - f.x0) / st)), i1 = std::min(f.nx - 1, (int)std::floor((mxx - f.x0) / st));
        const int j0 = std::max(0, (int)std::ceil((mny - f.y0) / st)), j1 = std::min(f.ny - 1, (int)std::floor((mxy - f.y0) / st));
        for (int j = j0; j <= j1; j++) for (int i = i0; i <= i1; i++) bins[f.at(i, j)].push_back(t);
    }
    std::vector<Hit> hits;
    for (int j = 0; j < f.ny; j++)
        for (int i = 0; i < f.nx; i++) {
            const double x = f.X(i), y = f.Y(j);
            hits.clear();
            for (uint32_t t : bins[f.at(i, j)]) {
                const NavCollision::Triangle& tr = col.triangles[t];
                const Vec3 &a = tr.v[0], &b = tr.v[1], &c = tr.v[2];
                const double d = (double)(b.y - c.y) * (a.x - c.x) + (double)(c.x - b.x) * (a.y - c.y);
                const double l0 = ((b.y - c.y) * (x - c.x) + (c.x - b.x) * (y - c.y)) / d;
                const double l1 = ((c.y - a.y) * (x - c.x) + (a.x - c.x) * (y - c.y)) / d;
                const double l2 = 1 - l0 - l1;
                if (l0 < -1e-9 || l1 < -1e-9 || l2 < -1e-9) continue;
                const Vec3 n = cross(b - a, c - a);
                const float len = n.length();
                if (len <= 0) continue;
                hits.push_back({(float)(l0 * a.z + l1 * b.z + l2 * c.z), n.z / len, tr.solid});
            }
            std::sort(hits.begin(), hits.end(), [](const Hit& p, const Hit& q) { return p.z > q.z; });
            // a ray along a shared edge meets both triangles: one hit each way
            size_t w = 0;
            for (size_t k = 0; k < hits.size(); k++) {
                if (w > 0 && std::fabs(hits[w - 1].z - hits[k].z) < 1e-3f && (hits[w - 1].nz > 0) == (hits[k].nz > 0) && hits[w - 1].solid == hits[k].solid) continue;
                hits[w++] = hits[k];
            }
            hits.resize(w);
            auto& out = f.floors[f.at(i, j)];
            int depth = 0;   // closed solids the ray is inside
            float above = std::numeric_limits<float>::infinity();
            for (const Hit& h : hits) {
                if (h.nz >= 0.7f && depth == 0 && above - h.z >= head) out.push_back(h.z);
                if (h.solid) depth = std::max(0, depth + (h.nz > 0 ? 1 : -1));
                above = h.z;
            }
        }
    return f;
}

constexpr int kNb4[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
const double kNaN = std::numeric_limits<double>::quiet_NaN();

/// The triangle clipped to the vertical square prism around (cx, cy) of half size h.
std::vector<Vec3> clip_square(const Vec3* tri, double cx, double cy, double h) {
    std::vector<Vec3> poly(tri, tri + 3), next;
    auto pass = [&](int axis, double lim, bool keep_below) {
        next.clear();
        auto val = [&](const Vec3& p) { return (axis == 0 ? p.x : p.y) - lim; };
        auto in = [&](const Vec3& p) { return keep_below ? val(p) <= 0 : val(p) >= 0; };
        for (size_t i = 0, n = poly.size(); i < n; i++) {
            const Vec3 &a = poly[i], &b = poly[(i + 1) % n];
            const bool ia = in(a), ib = in(b);
            if (ia) next.push_back(a);
            if (ia != ib) {
                const double t = val(a) / (val(a) - val(b));
                next.push_back(a + (b - a) * (float)t);
            }
        }
        poly.swap(next);
    };
    pass(0, cx + h, true); if (poly.empty()) return poly;
    pass(0, cx - h, false); if (poly.empty()) return poly;
    pass(1, cy + h, true); if (poly.empty()) return poly;
    pass(1, cy - h, false);
    return poly;
}

}   // namespace

// ---------------------------------------------------------------- the patch

namespace {

std::vector<P3> corners_of(const NavMesh& nav, size_t i) {
    std::vector<P3> o;
    for (const Vec3& v : nav.poly_vertices(i)) o.push_back({v.x, v.y, v.z});
    return o;
}
P3 mean(const std::vector<P3>& p) {
    P3 m;
    for (auto& q : p) { m.x += q.x; m.y += q.y; m.z += q.z; }
    const double n = (double)std::max<size_t>(1, p.size());
    return {m.x / n, m.y / n, m.z / n};
}

/// Replaced polygons: ground-level polygons whose centre is in the area.
std::vector<uint32_t> replaced(const NavMesh& nav, const NavPatchOptions& o) {
    std::vector<uint32_t> s;
    for (size_t i = 0; i < nav.polys.size(); i++) {
        if (nav.polys[i].vertex_count() < 3) continue;
        const auto c = corners_of(nav, i);
        const P3 m = mean(c);
        double zmin = 1e30, zmax = -1e30;
        for (auto& v : c) { zmin = std::min(zmin, v.z); zmax = std::max(zmax, v.z); }
        if (m.x >= o.x0 && m.x <= o.x1 && m.y >= o.y0 && m.y <= o.y1 && zmin >= o.z_lo && zmax <= o.z_hi) s.push_back((uint32_t)i);
    }
    return s;
}

constexpr double kMargin = 3;   // the field reaches this far past the area, so walls just outside still push the edge back

}   // namespace

AABB nav_patch_extent(const NavMesh& nav, const NavPatchOptions& o) {
    Vec3 mn{std::min(o.x0, o.x1), std::min(o.y0, o.y1), o.z_lo}, mx{std::max(o.x0, o.x1), std::max(o.y0, o.y1), o.z_hi};
    for (uint32_t i : replaced(nav, o))
        for (const Vec3& v : nav.poly_vertices(i)) { mn = vmin(mn, v); mx = vmax(mx, v); }
    const Vec3 m{(float)kMargin + 1, (float)kMargin + 1, 6};
    mn = mn - m;
    mx = mx + m;
    mn.z = std::max(mn.z, nav.box_min().z - 6);
    mx.z = std::min(mx.z, nav.box_max().z + 6);
    return AABB{Vec4(mn, 0), Vec4(mx, 0)};
}

NavPatchResult patch_navmesh(NavMesh& nav, const NavPatchOptions& o, const NavCollision& col) {
    NavPatchResult r;
    const size_t np = nav.polys.size();
    const uint32_t own = nav.area_id;
    auto area_at = [&](const NavEdgePart& e) { return e.area_index() < nav.adjacent_areas.size() ? nav.adjacent_areas[e.area_index()] : 0xFFFFFFFFu; };
    auto edge = [&](size_t p, size_t k) -> const NavEdge& { return nav.edges[nav.polys[p].first_index + k]; };
    std::vector<std::vector<P3>> P(np);
    for (size_t i = 0; i < np; i++) P[i] = corners_of(nav, i);
    const Area rect{Ring{{o.x0, o.y0}, {o.x1, o.y0}, {o.x1, o.y1}, {o.x0, o.y1}}};

    // ---- 1. the polygons to replace
    const std::vector<uint32_t> Slist = replaced(nav, o);
    if (Slist.empty()) throw Error("navmesh patch: no polygons in the area at that height");
    std::vector<uint8_t> inS(np);
    for (uint32_t i : Slist) inS[i] = 1;
    for (uint32_t i : Slist)
        for (size_t k = 0; k < P[i].size(); k++) {
            const NavEdgePart& e = edge(i, k).a;
            if (!e.open() && area_at(e) != own)
                throw Error("navmesh patch: the area reaches the cell's border (polygon " + std::to_string(i) + " links into area " +
                            std::to_string(area_at(e)) + "): keep it inside the cell");
        }
    for (const NavPortal& p : nav.portals)
        for (uint16_t q : {p.poly_from1, p.poly_to1})
            if (q < np && inS[q]) { r.notes.push_back("a special link (climb, drop or ladder) names replaced polygon " + std::to_string(q)); break; }

    // ---- 2. walkable ground
    const AABB ext = nav_patch_extent(nav, o);
    const Field F = sample(col, ext, o.cell, o.head);
    const int nx = F.nx, ny = F.ny;
    const double st = F.st;
    std::vector<std::pair<size_t, double>> seeds;
    auto add_seed = [&](double x, double y, double z, double tol) {
        const int i = (int)std::lround((x - F.x0) / st), j = (int)std::lround((y - F.y0) / st);
        if (i < 0 || j < 0 || i >= nx || j >= ny) return;
        double best = kNaN;
        for (float w : F.floors[F.at(i, j)])
            if (std::fabs(w - z) <= tol && (std::isnan(best) || std::fabs(w - z) < std::fabs(best - z))) best = w;
        if (!std::isnan(best)) seeds.push_back({F.at(i, j), best});
    };
    if (o.seed) add_seed(o.seed->x, o.seed->y, o.seed->z, 1.2);
    else for (uint32_t i : Slist) { const P3 m = mean(P[i]); add_seed(m.x, m.y, m.z, 0.3); }
    if (seeds.empty()) throw Error("navmesh patch: no ground under the area's polygons to start from: give a seed point");
    std::vector<double> Zw((size_t)nx * ny, kNaN);   // the flooded ground
    {
        std::deque<size_t> q;
        for (auto& [c, z] : seeds) if (std::isnan(Zw[c])) { Zw[c] = z; q.push_back(c); }
        while (!q.empty()) {
            const size_t c = q.front();
            q.pop_front();
            const int i = (int)(c % nx), j = (int)(c / nx);
            const double z = Zw[c];
            for (auto& d : kNb4) {
                const int a = i + d[0], b = j + d[1];
                if (a < 0 || b < 0 || a >= nx || b >= ny || !std::isnan(Zw[F.at(a, b)])) continue;
                double best = kNaN;
                for (float w : F.floors[F.at(a, b)])
                    if (std::fabs(w - z) <= o.step && (std::isnan(best) || std::fabs(w - z) < std::fabs(best - z))) best = w;
                if (!std::isnan(best)) { Zw[F.at(a, b)] = best; q.push_back(F.at(a, b)); }
            }
        }
    }
    // clearance from walls and drops (samples past the grid count as ground)
    std::vector<double> Z = Zw;
    const int cl = o.clear_cells;
    for (int j = 0; j < ny; j++)
        for (int i = 0; i < nx; i++) {
            if (std::isnan(Zw[F.at(i, j)])) continue;
            bool near = false;
            for (int dj = -cl; dj <= cl && !near; dj++)
                for (int di = -cl; di <= cl && !near; di++) {
                    if (di * di + dj * dj > cl * cl) continue;
                    const int a = i + di, b = j + dj;
                    if (a >= 0 && b >= 0 && a < nx && b < ny && std::isnan(Zw[F.at(a, b)])) near = true;
                }
            if (near) Z[F.at(i, j)] = kNaN;
        }
    // thin things the samples miss: steep surfaces rising more than `knee` above the ground beside them, below head
    // height, within `rail_clear`; posts within `post_clear`
    for (const NavCollision::Triangle& t : col.triangles) {
        const Vec3 n = cross(t.v[1] - t.v[0], t.v[2] - t.v[0]);
        const float len = n.length();
        if (len <= 0 || std::fabs(n.z / len) >= 0.7f) continue;
        const double c = o.rail_clear;
        const double mnx = std::min({t.v[0].x, t.v[1].x, t.v[2].x}) - c, mxx = std::max({t.v[0].x, t.v[1].x, t.v[2].x}) + c;
        const double mny = std::min({t.v[0].y, t.v[1].y, t.v[2].y}) - c, mxy = std::max({t.v[0].y, t.v[1].y, t.v[2].y}) + c;
        const int i0 = std::max(0, (int)std::ceil((mnx - F.x0) / st)), i1 = std::min(nx - 1, (int)std::floor((mxx - F.x0) / st));
        const int j0 = std::max(0, (int)std::ceil((mny - F.y0) / st)), j1 = std::min(ny - 1, (int)std::floor((mxy - F.y0) / st));
        for (int j = j0; j <= j1; j++)
            for (int i = i0; i <= i1; i++) {
                const size_t k = F.at(i, j);
                const double f = Zw[k];
                if (std::isnan(f) || std::isnan(Z[k])) continue;
                const auto part = clip_square(t.v, F.X(i), F.Y(j), c);
                if (part.empty()) continue;
                double zmin = 1e30, zmax = -1e30, d = 1e30;
                for (size_t m = 0; m < part.size(); m++) {
                    zmin = std::min(zmin, (double)part[m].z); zmax = std::max(zmax, (double)part[m].z);
                    const Vec3 &a = part[m], &b = part[(m + 1) % part.size()];
                    d = std::min(d, seg_dist(F.X(i), F.Y(j), a.x, a.y, b.x, b.y));
                }
                if (zmax > f + o.knee && zmin < f + o.head && d < c) { Z[k] = kNaN; r.blocked_samples++; }
            }
    }
    for (const NavCollision::Post& p : col.posts) {
        const double c = o.post_clear + p.radius;
        const int i0 = std::max(0, (int)std::ceil((p.x - c - F.x0) / st)), i1 = std::min(nx - 1, (int)std::floor((p.x + c - F.x0) / st));
        const int j0 = std::max(0, (int)std::ceil((p.y - c - F.y0) / st)), j1 = std::min(ny - 1, (int)std::floor((p.y + c - F.y0) / st));
        for (int j = j0; j <= j1; j++)
            for (int i = i0; i <= i1; i++) {
                const size_t k = F.at(i, j);
                const double f = Zw[k];
                if (std::isnan(f) || std::isnan(Z[k])) continue;
                if (!(p.z + p.height > f + o.knee && p.z < f + o.head)) continue;
                if (std::hypot(F.X(i) - p.x, F.Y(j) - p.y) < c) { Z[k] = kNaN; r.blocked_samples++; }
            }
    }
    auto ground_near = [&](double x, double y) -> std::optional<double> {
        const int i = (int)std::lround((x - F.x0) / st), j = (int)std::lround((y - F.y0) / st);
        int bd = -1;
        double bz = 0;
        for (int jj = j - 4; jj <= j + 4; jj++)
            for (int ii = i - 4; ii <= i + 4; ii++)
                if (ii >= 0 && jj >= 0 && ii < nx && jj < ny && !std::isnan(Zw[F.at(ii, jj)])) {
                    const int d = (ii - i) * (ii - i) + (jj - j) * (jj - j);
                    if (bd < 0 || d < bd) { bd = d; bz = Zw[F.at(ii, jj)]; }
                }
        return bd < 0 ? std::nullopt : std::optional<double>(bz);
    };

    // one level at a time: a replaced polygon far from the ground found under it is on another level (a roof, a
    // bridge, a tunnel), which this rebuild would lose
    {
        std::vector<double> off_levels;
        double gmin = 1e30, gmax = -1e30;
        for (uint32_t i : Slist) {
            const P3 m = mean(P[i]);
            // the flooded ground in the polygon's own column (not the nearest: that may be a step or a kiosk beside it)
            const int ci = (int)std::lround((m.x - F.x0) / st), cj = (int)std::lround((m.y - F.y0) / st);
            if (ci < 0 || cj < 0 || ci >= nx || cj >= ny || std::isnan(Zw[F.at(ci, cj)])) continue;
            const std::optional<double> g = Zw[F.at(ci, cj)];
            gmin = std::min(gmin, *g);
            gmax = std::max(gmax, *g);
            if (std::fabs(*g - m.z) > 2.5) off_levels.push_back(m.z);
        }
        if (!off_levels.empty()) {
            std::sort(off_levels.begin(), off_levels.end());
            char buf[400];
            std::snprintf(buf, sizeof buf, "%zu of the polygons to replace (at heights %.1f to %.1f) are not on the ground found from the seed (heights "
                          "%.1f to %.1f): the area holds more than one level; narrow it to one with the height range (--z)",
                          off_levels.size(), off_levels.front(), off_levels.back(), gmin, gmax);
            throw Error(std::string("navmesh patch: ") + buf);
        }
    }

    // ---- 3. the area: replaced polygons plus the holes inside the rect, minus kept ground-level polygons (a kept
    // polygon on another level, like a ramp under a plaza, does not block the ground above it)
    auto on_ground = [&](size_t p) {
        int hits = 0, n = 0;
        auto pts = P[p];
        pts.push_back(mean(P[p]));
        for (auto& q : pts) {
            const auto g = ground_near(q.x, q.y);
            if (!g) continue;
            n++;
            if (std::fabs(*g - q.z) < 0.6) hits++;
        }
        return n > 0 && hits * 2 >= n;
    };
    const Box2 rbox = box_of(rect);
    Area K;
    for (size_t p = 0; p < np; p++)
        if (!inS[p] && P[p].size() >= 3 && box_of(P[p]).meets(rbox) && on_ground(p)) append(K, area_of_poly(P[p]));
    K = uni(K);
    Area A;
    for (uint32_t i : Slist) append(A, area_of_poly(P[i]));
    append(A, diff(rect, K));
    A = diff(uni(A), K);
    A = uni(grow(grow(A, 1e-3, C2::JoinType::Miter, 4), -1e-3, C2::JoinType::Miter, 4));
    if (area_of(A) < 1e-3) throw Error("navmesh patch: nothing left to rebuild in the area");
    r.removed = (uint32_t)Slist.size();

    // ---- 4. height regions over the walkable samples in the area (heights smoothed over ~1 m first)
    std::vector<uint8_t> inA((size_t)nx * ny);
    {
        // past the area by more than the outline simplification can take back, so the clipped outlines reach its edge
        const Area Ag = grow(A, 2 * st, C2::JoinType::Round);
        const Box2 ab = box_of(Ag);
        for (int j = 0; j < ny; j++)
            for (int i = 0; i < nx; i++) {
                const size_t k = F.at(i, j);
                if (std::isnan(Z[k]) || F.X(i) < ab.x0 || F.X(i) > ab.x1 || F.Y(j) < ab.y0 || F.Y(j) > ab.y1) continue;
                inA[k] = inside(Ag, F.X(i), F.Y(j));
            }
    }
    const std::vector<double>& Zraw = Z;
    std::vector<double> Zs = Z;
    for (int j = 0; j < ny; j++)
        for (int i = 0; i < nx; i++) {
            if (!inA[F.at(i, j)]) continue;
            double sum = 0;
            int n = 0;
            for (int jj = std::max(0, j - 2); jj <= std::min(ny - 1, j + 2); jj++)
                for (int ii = std::max(0, i - 2); ii <= std::min(nx - 1, i + 2); ii++) {
                    const double z = Zraw[F.at(ii, jj)];
                    if (!std::isnan(z) && std::fabs(z - Zraw[F.at(i, j)]) < 0.5) { sum += z; n++; }
                }
            Zs[F.at(i, j)] = sum / n;
        }
    std::vector<int> lab((size_t)nx * ny, -1);
    std::vector<std::array<double, 3>> planes;
    auto cell_pts = [&](const std::vector<size_t>& cells, const std::vector<double>& zz) {
        std::vector<P3> v;
        v.reserve(cells.size());
        for (size_t c : cells) v.push_back({F.X((int)(c % nx)), F.Y((int)(c / nx)), zz[c]});
        return v;
    };
    {
        const std::set<size_t> refits{3, 6, 12, 25, 50, 100, 200, 400, 800, 1600, 3200};
        for (size_t c0 = 0; c0 < lab.size(); c0++) {
            if (!inA[c0] || lab[c0] >= 0) continue;
            const int rg = (int)planes.size();
            std::vector<size_t> cells{c0};
            lab[c0] = rg;
            std::deque<size_t> q{c0};
            std::array<double, 3> k{0, 0, Zs[c0]};
            while (!q.empty()) {
                const size_t c = q.front();
                q.pop_front();
                const int i = (int)(c % nx), j = (int)(c / nx);
                for (auto& d : kNb4) {
                    const int a = i + d[0], b = j + d[1];
                    if (a < 0 || b < 0 || a >= nx || b >= ny) continue;
                    const size_t e = F.at(a, b);
                    if (!inA[e] || lab[e] >= 0) continue;
                    if (std::fabs(k[0] * F.X(a) + k[1] * F.Y(b) + k[2] - Zs[e]) > o.plane_tolerance) continue;
                    if (std::fabs(Zs[e] - Zs[c]) > o.step) continue;
                    lab[e] = rg;
                    cells.push_back(e);
                    q.push_back(e);
                    if (refits.count(cells.size())) {
                        double res = 0;
                        const auto k2 = fit(cell_pts(cells, Zs), &res);
                        if (res <= o.plane_tolerance) k = k2;
                    }
                }
            }
            if (cells.size() >= 3) planes.push_back(fit(cell_pts(cells, Zs), nullptr));
            else { double s = 0; for (size_t c : cells) s += Zs[c]; planes.push_back({0, 0, s / (double)cells.size()}); }
        }
        for (int pass = 0; pass < 3; pass++) {
            std::vector<std::vector<size_t>> members(planes.size());
            for (size_t c = 0; c < lab.size(); c++) if (lab[c] >= 0) members[lab[c]].push_back(c);
            std::vector<int> order(planes.size());
            std::iota(order.begin(), order.end(), 0);
            std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return members[a].size() < members[b].size(); });
            for (int rg : order) {
                auto& cells = members[rg];
                if (cells.empty() || cells.size() >= 64) continue;
                std::set<int> nbrs;
                for (size_t c : cells) {
                    const int i = (int)(c % nx), j = (int)(c / nx);
                    for (auto& d : kNb4) {
                        const int a = i + d[0], b = j + d[1];
                        if (a >= 0 && b >= 0 && a < nx && b < ny && lab[F.at(a, b)] >= 0 && lab[F.at(a, b)] != rg) nbrs.insert(lab[F.at(a, b)]);
                    }
                }
                double best_err = 0;
                int best = -1;
                for (int qn : nbrs) {
                    const auto& k = planes[qn];
                    double err = 0;
                    for (size_t c : cells) err = std::max(err, std::fabs(k[0] * F.X((int)(c % nx)) + k[1] * F.Y((int)(c / nx)) + k[2] - Zraw[c]));
                    if (err <= 0.25 && (best < 0 || err < best_err)) { best_err = err; best = qn; }
                }
                if (best >= 0) {
                    for (size_t c : cells) lab[c] = best;
                    members[best].insert(members[best].end(), cells.begin(), cells.end());
                    cells.clear();
                }
            }
        }
    }
    {
        std::set<int> used(lab.begin(), lab.end());
        used.erase(-1);
        r.regions = (uint32_t)used.size();
    }

    // ---- 5. region outlines, simplified together so shared borders stay identical
    // grid corner (i, j) sits at (x0 - st/2 + i st, y0 - st/2 + j st); a boundary run between two labels is a chain
    // between nodes (corners where more than two labels meet, or where a chain must end)
    const int gx = nx + 1, gy = ny + 1;
    auto L = [&](int i, int j) { return i < 0 || j < 0 || i >= nx || j >= ny ? -1 : lab[F.at(i, j)]; };
    auto cx = [&](int i) { return F.x0 - st / 2 + i * st; };
    auto cy = [&](int j) { return F.y0 - st / 2 + j * st; };
    struct GEdge { int a, b, left, right; };   // corners a -> b with `left` on the left
    std::vector<GEdge> gedges;
    std::vector<std::vector<int>> at_corner((size_t)gx * gy);
    auto corner = [&](int i, int j) { return j * gx + i; };
    for (int j = 0; j <= ny; j++)
        for (int i = 0; i < nx; i++) {   // horizontal: between (i, j-1) below and (i, j) above
            const int above = L(i, j), below = L(i, j - 1);
            if (above == below) continue;
            gedges.push_back({corner(i, j), corner(i + 1, j), above, below});
        }
    for (int j = 0; j < ny; j++)
        for (int i = 0; i <= nx; i++) {  // vertical: between (i-1, j) left and (i, j) right, going +y
            const int left = L(i - 1, j), right = L(i, j);
            if (left == right) continue;
            gedges.push_back({corner(i, j), corner(i, j + 1), left, right});
        }
    for (int e = 0; e < (int)gedges.size(); e++) { at_corner[gedges[e].a].push_back(e); at_corner[gedges[e].b].push_back(e); }
    auto pair_of = [&](const GEdge& e) { return std::minmax(e.left, e.right); };
    std::vector<uint8_t> node((size_t)gx * gy);
    for (size_t c = 0; c < at_corner.size(); c++) {
        const auto& es = at_corner[c];
        if (es.empty()) continue;
        if (es.size() != 2 || pair_of(gedges[es[0]]) != pair_of(gedges[es[1]])) node[c] = 1;
    }
    struct Chain { std::vector<int> corners; int left, right; };
    std::vector<Chain> chains;
    std::vector<uint8_t> done(gedges.size());
    auto walk = [&](int start_edge, int from) {
        Chain ch;
        const GEdge& e0 = gedges[start_edge];
        // orient the chain so that the first edge runs from `from`
        const bool fwd = e0.a == from;
        ch.left = fwd ? e0.left : e0.right;
        ch.right = fwd ? e0.right : e0.left;
        ch.corners.push_back(from);
        int e = start_edge, c = from;
        while (true) {
            done[e] = 1;
            const int next = gedges[e].a == c ? gedges[e].b : gedges[e].a;
            ch.corners.push_back(next);
            if (node[next]) break;
            int f = -1;
            for (int k : at_corner[next]) if (k != e && !done[k]) f = k;
            if (f < 0) break;
            e = f;
            c = next;
        }
        chains.push_back(std::move(ch));
    };
    for (size_t c = 0; c < at_corner.size(); c++)
        if (node[c]) for (int e : at_corner[c]) if (!done[e]) walk(e, (int)c);
    for (int e = 0; e < (int)gedges.size(); e++)
        if (!done[e]) {   // a loop with no node: its first corner becomes one
            node[gedges[e].a] = 1;
            walk(e, gedges[e].a);
        }
    auto chain_pts = [&](const Chain& ch, bool simplify) {
        std::vector<C2::PointD> pts;
        for (int c : ch.corners) pts.push_back({cx(c % gx), cy(c / gx)});
        if (!simplify || pts.size() < 3) return pts;
        // Douglas-Peucker keeping both ends; a closed loop is split at its farthest corner first
        std::vector<uint8_t> keep(pts.size());
        keep.front() = keep.back() = 1;
        std::vector<std::pair<size_t, size_t>> todo;
        if (ch.corners.front() == ch.corners.back()) {
            size_t far = 1;
            double fd = -1;
            for (size_t k = 1; k + 1 < pts.size(); k++) {
                const double d = std::hypot(pts[k].x - pts[0].x, pts[k].y - pts[0].y);
                if (d > fd) { fd = d; far = k; }
            }
            keep[far] = 1;
            todo.push_back({0, far});
            todo.push_back({far, pts.size() - 1});
        } else todo.push_back({0, pts.size() - 1});
        while (!todo.empty()) {
            auto [a, b] = todo.back();
            todo.pop_back();
            if (b <= a + 1) continue;
            double fd = -1;
            size_t far = a;
            for (size_t k = a + 1; k < b; k++) {
                const double d = seg_dist(pts[k].x, pts[k].y, pts[a].x, pts[a].y, pts[b].x, pts[b].y);
                if (d > fd) { fd = d; far = k; }
            }
            if (fd > o.simplify) { keep[far] = 1; todo.push_back({a, far}); todo.push_back({far, b}); }
        }
        std::vector<C2::PointD> out;
        for (size_t k = 0; k < pts.size(); k++) if (keep[k]) out.push_back(pts[k]);
        return out;
    };
    // a region's rings: its chains, each run with the region on its left, joined end to start
    auto region_area = [&](int rg, bool simplify) {
        std::multimap<int, std::vector<C2::PointD>> from;   // start corner -> points (region on the left)
        for (const Chain& ch : chains) {
            if (ch.left != rg && ch.right != rg) continue;
            auto pts = chain_pts(ch, simplify);
            std::vector<int> ends{ch.corners.front(), ch.corners.back()};
            if (ch.right == rg) { std::reverse(pts.begin(), pts.end()); std::swap(ends[0], ends[1]); }
            from.insert({ends[0], std::move(pts)});
        }
        Area rings;
        while (!from.empty()) {
            auto it = from.begin();
            const int start = it->first;
            Ring ring;
            auto pts = std::move(it->second);
            from.erase(it);
            while (true) {
                ring.insert(ring.end(), pts.begin(), pts.end() - 1);
                const C2::PointD end = pts.back();
                const int ec = corner((int)std::lround((end.x - cx(0)) / st), (int)std::lround((end.y - cy(0)) / st));
                if (ec == start) break;
                auto nx_it = from.find(ec);
                if (nx_it == from.end()) break;
                pts = std::move(nx_it->second);
                from.erase(nx_it);
            }
            if (ring.size() >= 3) rings.push_back(std::move(ring));
        }
        return uni(rings);
    };
    std::vector<int> regs;
    for (int rg = 0; rg < (int)planes.size(); rg++) {
        bool any = false;
        for (const Chain& ch : chains) if (ch.left == rg || ch.right == rg) { any = true; break; }
        if (any) regs.push_back(rg);
    }
    struct Piece { int region; Area g; };
    std::vector<Piece> pieces;
    Area full;
    for (int rg : regs) {
        append(full, region_area(rg, false));
        for (Area& part : parts(inter(region_area(rg, true), A)))
            if (area_of(part) > 1e-4) pieces.push_back({rg, std::move(part)});
    }
    {   // simplified neighbours may overlap a little: later ones give way
        Area all;
        double sum = 0;
        for (auto& p : pieces) { append(all, p.g); sum += area_of(p.g); }
        if (sum - area_of(uni(all)) > 1e-4) {
            Area so_far;
            std::vector<Piece> fixed;
            for (auto& p : pieces) {
                for (Area& part : parts(diff(p.g, so_far))) if (area_of(part) > 1e-4) fixed.push_back({p.region, std::move(part)});
                append(so_far, p.g);
                so_far = uni(so_far);
            }
            pieces = std::move(fixed);
        }
    }
    {   // what simplification took off inside the area comes back
        Area all;
        for (auto& p : pieces) append(all, p.g);
        for (Area& part : parts(diff(inter(uni(full), A), uni(all))))
            if (area_of(part) > 1e-4) pieces.push_back({-1, std::move(part)});
    }
    // slivers join the neighbour they share most border with
    while (true) {
        std::vector<size_t> small;
        for (size_t k = 0; k < pieces.size(); k++) if (area_of(pieces[k].g) < 0.5) small.push_back(k);
        std::stable_sort(small.begin(), small.end(), [&](size_t a, size_t b) { return area_of(pieces[a].g) < area_of(pieces[b].g); });
        bool merged = false;
        for (size_t k : small) {
            double best_len = 0;
            int best = -1;
            const Box2 kb = box_of(pieces[k].g);
            for (size_t q = 0; q < pieces.size(); q++) {
                if (q == k || !box_of(pieces[q].g).meets(kb, 0.01)) continue;
                const double len = shared_length(pieces[k].g, pieces[q].g);
                if (len > best_len) { best_len = len; best = (int)q; }
            }
            if (best_len > 0.01) {
                Area u = pieces[best].g;
                append(u, pieces[k].g);
                auto ps = parts(u);
                if (ps.size() == 1) {
                    pieces[best].g = std::move(ps[0]);
                    pieces.erase(pieces.begin() + (ptrdiff_t)k);
                    merged = true;
                    break;
                }
            }
        }
        if (!merged) break;
    }
    for (auto& p : pieces)
        if (p.region < 0) {
            double bd = 1e30;
            int near = -1;
            for (auto& q : pieces)
                if (q.region >= 0) {
                    const double d = area_dist(q.g, p.g);
                    if (d < bd) { bd = d; near = q.region; }
                }
            p.region = near;
        }
    std::erase_if(pieces, [](const Piece& p) { return p.region < 0 || area_of(p.g) <= 0.01; });

    // ---- 6. triangulate (constrained Delaunay), merge triangles into convex polygons of at most max_corners
    std::vector<std::pair<int, std::vector<P3>>> newpolys;
    for (const Piece& pc : pieces) {
        std::vector<std::vector<P3>> polys = triangulate_or_halve(pc.g, 6);
        if (polys.empty()) {
            r.notes.push_back("a piece of " + std::to_string(area_of(pc.g)) + " m2 could not be triangulated and is left out");
            continue;
        }
        bool changed = true;
        while (changed) {
            changed = false;
            std::map<EdgeKey, std::vector<int>> sides;
            for (int pi = 0; pi < (int)polys.size(); pi++)
                for (size_t i = 0; i < polys[pi].size(); i++)
                    sides[unordered(key(polys[pi][i]), key(polys[pi][(i + 1) % polys[pi].size()]))].push_back(pi);
            struct Cand { double area; int a, b; std::vector<P3> m; };
            std::vector<Cand> cands;
            for (auto& [ek, ps] : sides) {
                if (ps.size() != 2) continue;
                auto m = merge(polys[ps[0]], polys[ps[1]], ek);
                if (!m.empty() && (int)m.size() <= o.max_corners && convex(m)) cands.push_back({area2(m), ps[0], ps[1], std::move(m)});
            }
            std::stable_sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) { return x.area > y.area; });
            std::vector<uint8_t> used(polys.size()), drop(polys.size());
            for (auto& c : cands) {
                if (used[c.a] || used[c.b]) continue;
                polys[c.a] = std::move(c.m);
                drop[c.b] = used[c.a] = used[c.b] = 1;
                changed = true;
            }
            std::vector<std::vector<P3>> kept;
            for (size_t i = 0; i < polys.size(); i++) if (!drop[i]) kept.push_back(std::move(polys[i]));
            polys = std::move(kept);
        }
        for (auto& p : polys) newpolys.push_back({pc.region, std::move(p)});
    }
    // every replaced slot gets a new polygon (nothing else is renumbered), a few spare for slivers dropped later:
    // the largest polygons are cut in two (a triangle at the middle of its longest side)
    std::erase_if(newpolys, [](const auto& p) { return std::fabs(area2(p.second)) < 0.03; });
    while (!newpolys.empty() && newpolys.size() < Slist.size() + std::max<size_t>(4, Slist.size() / 10)) {
        size_t k = 0;
        for (size_t i = 1; i < newpolys.size(); i++) if (area2(newpolys[i].second) > area2(newpolys[k].second)) k = i;
        if (area2(newpolys[k].second) < 0.2) break;
        auto [rg, pts] = newpolys[k];
        if (pts.size() == 3) {
            size_t e = 0;
            double best = -1;
            for (size_t i = 0; i < 3; i++) {
                const double l = std::hypot(pts[(i + 1) % 3].x - pts[i].x, pts[(i + 1) % 3].y - pts[i].y);
                if (l > best) { best = l; e = i; }
            }
            const P3 a = pts[e], b = pts[(e + 1) % 3], c = pts[(e + 2) % 3], m{(a.x + b.x) / 2, (a.y + b.y) / 2, 0};
            newpolys[k] = {rg, {a, m, c}};
            newpolys.push_back({rg, {m, b, c}});
            continue;
        }
        const size_t m = pts.size() / 2;
        newpolys[k] = {rg, std::vector<P3>(pts.begin(), pts.begin() + (ptrdiff_t)m + 1)};
        std::vector<P3> rest(pts.begin() + (ptrdiff_t)m, pts.end());
        rest.push_back(pts[0]);
        newpolys.push_back({rg, std::move(rest)});
    }

    // ---- 7. heights: the smoothed ground of the nearest samples of the corner's own region; a corner shared by
    // regions takes the mean; a kept polygon's corner keeps its own height where it agrees
    auto zat = [&](double x, double y, int rg) {
        const double fi = (x - F.x0) / st, fj = (y - F.y0) / st;
        std::vector<std::pair<double, double>> best;
        for (int jj = (int)std::floor(fj) - 2; jj <= (int)std::floor(fj) + 3; jj++)
            for (int ii = (int)std::floor(fi) - 2; ii <= (int)std::floor(fi) + 3; ii++)
                if (ii >= 0 && jj >= 0 && ii < nx && jj < ny && lab[F.at(ii, jj)] == rg)
                    best.push_back({(ii - fi) * (ii - fi) + (jj - fj) * (jj - fj), Zs[F.at(ii, jj)]});
        if (best.empty()) { const auto& k = planes[rg]; return k[0] * x + k[1] * y + k[2]; }
        std::stable_sort(best.begin(), best.end(), [](auto& a, auto& b) { return a.first < b.first; });
        double ws = 0, zs = 0;
        for (size_t t = 0; t < std::min<size_t>(4, best.size()); t++) { const double w = 1 / (0.05 + best[t].first); ws += w; zs += w * best[t].second; }
        return zs / ws;
    };
    std::map<Key, std::vector<double>> zsum;
    for (auto& [rg, pts] : newpolys) for (auto& p : pts) zsum[key(p)].push_back(zat(p.x, p.y, rg));
    std::set<uint32_t> kept_touch;
    for (size_t p = 0; p < np; p++) {
        if (inS[p]) continue;
        for (size_t k = 0; k < P[p].size(); k++) {
            const NavEdgePart& e = edge(p, k).a;
            if (!e.open() && area_at(e) == own && e.poly() < np && inS[e.poly()]) { kept_touch.insert((uint32_t)p); break; }
        }
    }
    {
        const Box2 ab = box_of(A);
        for (size_t p = 0; p < np; p++) {
            if (inS[p] || kept_touch.count((uint32_t)p) || P[p].size() < 3) continue;
            const double zm = mean(P[p]).z;
            if (zm < o.z_lo - 1 || zm > o.z_hi + 1 || !box_of(P[p]).meets(ab, 0.01)) continue;
            if (shared_length(Area{ring_of(P[p])}, A) > 0.05) kept_touch.insert((uint32_t)p);
        }
    }
    std::map<Key, double> kept_z;
    for (uint32_t i : kept_touch) for (auto& v : P[i]) kept_z[key(v)] = v.z;
    auto vz = [&](double x, double y) {
        const Key k = key(x, y);
        auto it = zsum.find(k);
        std::optional<double> z;
        if (it != zsum.end()) z = std::accumulate(it->second.begin(), it->second.end(), 0.0) / (double)it->second.size();
        auto kt = kept_z.find(k);
        if (kt != kept_z.end() && (!z || std::fabs(kt->second - *z) < 0.4)) return kt->second;
        return z.value_or(0.0);
    };

    // ---- 8. one corner set for everything: T-junctions split, zero-length sides and slivers gone, links by shared
    // corner pairs at the same height
    struct Item { int kept = -1; int region = -1; std::vector<P3> pts; uint32_t slot = 0; };
    std::vector<Item> items;
    for (uint32_t i : kept_touch) items.push_back({(int)i, -1, P[i], 0});
    for (auto& [rg, pts] : newpolys) {
        Item it{-1, rg, {}, 0};
        for (auto& p : pts) it.pts.push_back({p.x, p.y, vz(p.x, p.y)});
        items.push_back(std::move(it));
    }
    {
        std::map<std::pair<Key, int64_t>, P3> allv;
        for (auto& it : items) for (auto& v : it.pts) allv.try_emplace({key(v), (int64_t)std::llround(v.z / 0.4)}, v);
        std::unordered_map<int64_t, std::vector<P3>> grid;   // 1 m buckets
        auto bucket = [](double x, double y) { return (int64_t)std::floor(x) * 1000003 + (int64_t)std::floor(y); };
        for (auto& [k, v] : allv) grid[bucket(v.x, v.y)].push_back(v);
        for (auto& it : items) {
            const auto& pts = it.pts;
            std::vector<P3> outp;
            for (size_t i = 0; i < pts.size(); i++) {
                const P3 a = pts[i], b = pts[(i + 1) % pts.size()];
                outp.push_back(a);
                const double dx = b.x - a.x, dy = b.y - a.y, len = std::sqrt(dx * dx + dy * dy);
                if (len < 1e-6) continue;
                const double ux = dx / len, uy = dy / len;
                std::vector<std::pair<double, P3>> mids;
                for (int64_t bx = (int64_t)std::floor(std::min(a.x, b.x) - 0.01); bx <= (int64_t)std::floor(std::max(a.x, b.x) + 0.01); bx++)
                    for (int64_t by = (int64_t)std::floor(std::min(a.y, b.y) - 0.01); by <= (int64_t)std::floor(std::max(a.y, b.y) + 0.01); by++) {
                        auto g = grid.find(bx * 1000003 + by);
                        if (g == grid.end()) continue;
                        for (const P3& v : g->second) {
                            const double wx = v.x - a.x, wy = v.y - a.y, t = wx * ux + wy * uy;
                            if (t > 0.005 && t < len - 0.005 && std::fabs(wx * uy - wy * ux) < 0.005 && std::fabs(a.z + (b.z - a.z) * t / len - v.z) < 0.4)
                                mids.push_back({t, v});
                        }
                    }
                std::stable_sort(mids.begin(), mids.end(), [](auto& p, auto& q) { return p.first < q.first; });
                for (auto& [t, v] : mids) outp.push_back({v.x, v.y, a.z + (b.z - a.z) * t / len});
            }
            std::vector<P3> ded;
            for (auto& v : outp) if (ded.empty() || key(ded.back()) != key(v)) ded.push_back(v);
            while (ded.size() > 1 && key(ded.front()) == key(ded.back())) ded.pop_back();
            it.pts = std::move(ded);
        }
    }
    for (auto& it : items)
        if (it.kept >= 0 && it.pts.size() < 3) throw Error("navmesh patch: kept polygon " + std::to_string(it.kept) + " collapsed while stitching");
    std::erase_if(items, [](const Item& it) { return it.kept < 0 && (it.pts.size() < 3 || std::fabs(area2(it.pts)) < 0.03); });
    std::map<std::pair<Key, Key>, std::vector<std::pair<int, int>>> owner;
    for (int n = 0; n < (int)items.size(); n++)
        for (int i = 0; i < (int)items[n].pts.size(); i++) owner[{key(items[n].pts[i]), key(items[n].pts[(i + 1) % items[n].pts.size()])}].push_back({n, i});
    auto neighbour = [&](int n, int i) -> int {
        const auto& pts = items[n].pts;
        const P3 a = pts[i], b = pts[(i + 1) % pts.size()];
        auto it = owner.find({key(b), key(a)});
        if (it == owner.end()) return -1;
        for (auto [q, k] : it->second) {
            if (q == n) continue;
            const auto& qp = items[q].pts;
            if (std::fabs(qp[k].z - b.z) < 0.4 && std::fabs(qp[(k + 1) % qp.size()].z - a.z) < 0.4) return q;
        }
        return -1;
    };

    // ---- 9. slots: new polygons take the replaced numbers, extras go on the end
    std::vector<int> news;
    for (int n = 0; n < (int)items.size(); n++) if (items[n].kept < 0) news.push_back(n);
    if (news.size() < Slist.size())
        throw Error("navmesh patch: only " + std::to_string(news.size()) + " new polygons for " + std::to_string(Slist.size()) + " replaced numbers");
    for (size_t k = 0; k < news.size(); k++) items[news[k]].slot = k < Slist.size() ? Slist[k] : (uint32_t)(np + (k - Slist.size()));
    for (auto& it : items) if (it.kept >= 0) it.slot = (uint32_t)it.kept;
    if (np + news.size() - Slist.size() > 0x3FFF) throw Error("navmesh patch: the cell would hold more than 16383 polygons");

    // flags: from the replaced polygon the new one overlaps most at a similar height, else the nearest replaced one
    std::map<uint32_t, Area> old_geo;
    for (uint32_t i : Slist) old_geo[i] = area_of_poly(P[i]);
    auto flags_from = [&](const Item& it) -> uint32_t {
        const Area g = area_of_poly(it.pts);
        const P3 c = mean(it.pts);
        double ba = -1, bd = -1e30;
        int best = -1;
        const Box2 gb = box_of(it.pts);
        for (auto& [i, og] : old_geo) {
            if (std::fabs(mean(P[i]).z - c.z) > 0.6) continue;
            const double a = box_of(og).meets(gb) ? area_of(inter(og, g)) : 0.0;
            const double d = -point_dist(og, c.x, c.y);
            if (best < 0 || a > ba || (a == ba && d > bd)) { ba = a; bd = d; best = (int)i; }
        }
        if (best >= 0) return (uint32_t)best;
        double bv = 1e30;
        for (auto& [i, og] : old_geo) {
            const double v = point_dist(og, c.x, c.y) + std::fabs(mean(P[i]).z - c.z);
            if (v < bv) { bv = v; best = (int)i; }
        }
        return (uint32_t)best;
    };

    // free space at a side: distance from its middle to the nearest open side (wall) of the final mesh at a similar
    // height, in quarter metres, capped at 31, stored shifted left one (bit 0: a height step)
    struct Wall { double ax, ay, bx, by, z; };
    std::vector<Wall> walls;
    for (size_t p = 0; p < np; p++) {
        if (inS[p] || kept_touch.count((uint32_t)p) || P[p].size() < 2) continue;
        for (size_t k = 0; k < P[p].size(); k++)
            if (edge(p, k).a.open()) {
                const P3 a = P[p][k], b = P[p][(k + 1) % P[p].size()];
                walls.push_back({a.x, a.y, b.x, b.y, (a.z + b.z) / 2});
            }
    }
    for (int n = 0; n < (int)items.size(); n++)
        for (int i = 0; i < (int)items[n].pts.size(); i++)
            if (neighbour(n, i) < 0) {
                const P3 a = items[n].pts[i], b = items[n].pts[(i + 1) % items[n].pts.size()];
                walls.push_back({a.x, a.y, b.x, b.y, (a.z + b.z) / 2});
            }
    std::unordered_map<int64_t, std::vector<uint32_t>> wgrid;   // 4 m buckets
    auto wb = [](int64_t x, int64_t y) { return x * 1000003 + y; };
    for (uint32_t k = 0; k < walls.size(); k++) {
        const Wall& w = walls[k];
        for (int64_t x = (int64_t)std::floor(std::min(w.ax, w.bx) / 4); x <= (int64_t)std::floor(std::max(w.ax, w.bx) / 4); x++)
            for (int64_t y = (int64_t)std::floor(std::min(w.ay, w.by) / 4); y <= (int64_t)std::floor(std::max(w.ay, w.by) / 4); y++) wgrid[wb(x, y)].push_back(k);
    }
    auto nearest_wall = [&](double x, double y, double z, double reach) {
        double best = 99;
        std::set<uint32_t> seen;
        for (int64_t gx2 = (int64_t)std::floor((x - reach) / 4); gx2 <= (int64_t)std::floor((x + reach) / 4); gx2++)
            for (int64_t gy2 = (int64_t)std::floor((y - reach) / 4); gy2 <= (int64_t)std::floor((y + reach) / 4); gy2++) {
                auto g = wgrid.find(wb(gx2, gy2));
                if (g == wgrid.end()) continue;
                for (uint32_t k : g->second) {
                    if (!seen.insert(k).second) continue;
                    const Wall& w = walls[k];
                    if (std::fabs(w.z - z) < 2) best = std::min(best, seg_dist(x, y, w.ax, w.ay, w.bx, w.by));
                }
            }
        return best;
    };
    auto free_space = [&](const P3& a, const P3& b) {
        const double d = nearest_wall((a.x + b.x) / 2, (a.y + b.y) / 2, (a.z + b.z) / 2, 8);
        return (uint32_t)std::min(31, (int)std::lround(d / 0.25));
    };

    // the check against the ground: every new polygon's plane against the ground samples inside it
    for (const Item& it : items) {
        if (it.kept >= 0) continue;
        const auto k = fit(it.pts, nullptr);
        const Box2 b = box_of(it.pts);
        const Area g{ring_of(it.pts)};
        const int i0 = std::max(0, (int)((b.x0 - F.x0) / st)), i1 = std::min(nx - 1, (int)((b.x1 - F.x0) / st) + 1);
        const int j0 = std::max(0, (int)((b.y0 - F.y0) / st)), j1 = std::min(ny - 1, (int)((b.y1 - F.y0) / st) + 1);
        for (int j = j0; j <= j1; j++)
            for (int i = i0; i <= i1; i++) {
                const double z = Zraw[F.at(i, j)];
                if (!std::isnan(z) && inside(g, F.X(i), F.Y(j)))
                    r.worst_height = std::max(r.worst_height, (float)std::fabs(k[0] * F.X(i) + k[1] * F.Y(j) + k[2] - z));
            }
    }

    // ---- 10. write it into the cell
    auto area_index = [&](uint32_t area) {
        for (size_t i = 0; i < nav.adjacent_areas.size(); i++) if (nav.adjacent_areas[i] == area) return (uint32_t)i;
        if (nav.adjacent_areas.size() >= 32) throw Error("navmesh patch: the cell already names 32 areas");
        nav.adjacent_areas.push_back(area);
        return (uint32_t)nav.adjacent_areas.size() - 1;
    };
    const uint32_t own_index = area_index(own), none_index = area_index(kNoPoly);
    auto link = [&](uint32_t poly, uint32_t free) {
        return NavEdge{NavEdgePart::make(own_index, poly, 0, free << 1), NavEdgePart::make(own_index, poly, 0, 0)};
    };
    const NavEdge open{NavEdgePart::make(none_index, kNoPoly, 1, 0), NavEdgePart::make(none_index, kNoPoly, 1, 1)};
    auto on_seg = [](const P3& p, const P3& a, const P3& b) {
        const double dx = b.x - a.x, dy = b.y - a.y, len = std::sqrt(dx * dx + dy * dy), wx = p.x - a.x, wy = p.y - a.y;
        if (len < 1e-6) return std::sqrt(wx * wx + wy * wy) < 0.005;
        const double ux = dx / len, uy = dy / len, t = wx * ux + wy * uy;
        return t >= -0.005 && t <= len + 0.005 && std::fabs(wx * uy - wy * ux) < 0.005;
    };
    std::vector<NavMesh::PolyShape> shapes = nav.shapes();
    const uint32_t total = (uint32_t)(np + (news.size() > Slist.size() ? news.size() - Slist.size() : 0));
    shapes.resize(total);
    std::vector<uint8_t> filled(total);
    for (size_t i = 0; i < np; i++) filled[i] = !inS[i];
    for (int n = 0; n < (int)items.size(); n++) {
        const Item& it = items[n];
        NavMesh::PolyShape& s = shapes[it.slot];
        if (it.kept < 0) {
            const NavPoly& src = nav.polys[flags_from(it)];
            s.poly = NavPoly{};
            s.poly.flags0 = src.flags0;
            s.poly.flags1 = src.flags1;
            s.poly.flags2 = src.flags2 & 0xFFFF0000u;
            s.poly.area_id = (uint16_t)own;
            r.created++;
        }
        bool same = it.kept >= 0 && it.pts.size() == P[it.kept].size();
        for (size_t i = 0; same && i < it.pts.size(); i++)
            same = it.pts[i].x == P[it.kept][i].x && it.pts[i].y == P[it.kept][i].y && it.pts[i].z == P[it.kept][i].z;
        s.source = same ? it.kept : -1;
        s.corners.clear();
        for (auto& q : it.pts) s.corners.push_back({(float)q.x, (float)q.y, (float)q.z});
        s.edges.assign(it.pts.size(), open);
        for (int i = 0; i < (int)it.pts.size(); i++) {
            const P3 a = it.pts[i], b = it.pts[(i + 1) % it.pts.size()];
            const int nb = neighbour(n, i);
            if (nb >= 0) { s.edges[i] = link(items[nb].slot, free_space(a, b)); r.links++; continue; }
            if (it.kept >= 0) {
                // a kept side: its link stays unless it named a replaced polygon
                const auto& ov = P[it.kept];
                int old = -1;
                for (size_t k = 0; k < ov.size() && old < 0; k++)
                    if (on_seg(a, ov[k], ov[(k + 1) % ov.size()]) && on_seg(b, ov[k], ov[(k + 1) % ov.size()])) old = (int)k;
                if (old >= 0) {
                    const NavEdge& oe = edge(it.kept, old);
                    if (!(!oe.a.open() && area_at(oe.a) == own && oe.a.poly() < np && inS[oe.a.poly()])) { s.edges[i] = oe; continue; }
                }
            }
            r.open_sides++;
        }
        filled[it.slot] = 1;
        r.written.push_back(it.slot);
        if (it.kept >= 0) r.kept_changed++;
    }
    for (uint32_t i = 0; i < total; i++)
        if (!filled[i]) throw Error("navmesh patch: polygon number " + std::to_string(i) + " left unfilled");
    nav.set_polys(shapes);
    std::sort(r.written.begin(), r.written.end());

    // cover points in the area no longer by a wall go (left standing in the open)
    if (nav.sectors) {
        std::vector<NavSector*> leaves;
        std::vector<NavSector*> todo{&*nav.sectors};
        while (!todo.empty()) {
            NavSector* s = todo.back();
            todo.pop_back();
            if (s->has_data) leaves.push_back(s);
            for (auto& c : s->children) if (c) todo.push_back(c.get());
        }
        for (NavSector* l : leaves)
            std::erase_if(l->points, [&](const NavPoint& pt) {
                const Vec3 q = nav.position(NavVertex{pt.x, pt.y, pt.z});
                if (!(inside(A, q.x, q.y) && q.z >= o.z_lo - 0.5 && q.z <= o.z_hi + 0.5)) return false;
                if (nearest_wall(q.x, q.y, q.z, 1) <= 0.8) return false;
                r.points_dropped++;
                return true;
            });
        nav.renumber_points();
    }
    return r;
}

} // namespace rk
