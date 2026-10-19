// ragekit - the tags of a Scaleform movie (gfx.h) read into their fields and made from scratch: shapes (DefineShape 1..4
// with their fill and line styles and edge records), sprites, PlaceObject2 / 3, frame labels, exports, the background,
// the file attributes and Scaleform's ExporterInfo, and the movie header (frame rectangle, rate, count).
//
// Every coordinate is in twips (1/20 of a pixel), as the file stores it. A shape is kept as SWF's own record list
// (style changes with an optional move, straight and curved edges as deltas), so a shape read from a file is written
// back as the same bits (BitHint: the bit counts the file used), and every shape and placement of the game's movies
// comes back byte for byte; a shape made here takes the fewest bits. `shape_paths` turns the records into absolute paths for tools; the
// builders (`ShapeBuilder`) make records from polygons.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <memory>
#include <vector>
#include "gfx.h"

namespace rk {

namespace gfx {

/// Tag codes this header reads and writes.
enum Code : uint16_t {
    kEnd = 0, kShowFrame = 1, kDefineShape = 2, kSetBackgroundColor = 9, kDefineShape2 = 22, kPlaceObject2 = 26,
    kRemoveObject2 = 28, kDefineShape3 = 32, kDefineSprite = 39, kFrameLabel = 43, kExportAssets = 56,
    kFileAttributes = 69, kPlaceObject3 = 70, kMetadata = 77, kDefineShape4 = 83, kExporterInfo = 1000,
};

/// The bit count a file used for a bit-packed field (-1: none, the encoder takes the fewest bits). Exporters differ
/// (Flash gives -1 two bits, Scaleform's tools one; a zero translation takes 0 or 1 bit), so a field read from a file
/// keeps its count and is written with it again unless the value no longer fits. Never part of a comparison.
struct BitHint {
    int8_t n = -1;
    bool operator==(const BitHint&) const { return true; }
};

struct Rgba {
    uint8_t r = 0, g = 0, b = 0, a = 255;
    bool operator==(const Rgba&) const = default;
};

/// A rectangle in twips (SWF's RECT: x min, x max, y min, y max).
struct Rect {
    int32_t x_min = 0, x_max = 0, y_min = 0, y_max = 0;
    BitHint bits;
    bool operator==(const Rect&) const = default;
};

/// SWF's MATRIX: x' = sx * x + r1 * y + tx, y' = r0 * x + sy * y + ty; scales and rotations as 16.16 fixed point.
struct Matrix {
    bool has_scale = false, has_rotate = false;
    int32_t sx = 65536, sy = 65536, r0 = 0, r1 = 0;   // 16.16
    int32_t tx = 0, ty = 0;                           // twips
    BitHint scale_bits, rotate_bits, translate_bits;
    bool operator==(const Matrix&) const = default;
    /// Scale, then a rotation by `rotation` radians (x' = x cos - y sin, y' = x sin + y cos: with the movie's y down
    /// that turns clockwise on the screen), then a translation in twips.
    static Matrix make(double scale_x, double scale_y, double rotation, int32_t tx, int32_t ty);
    /// sx, r0, r1, sy as doubles.
    double a() const { return sx / 65536.0; }
    double b() const { return r0 / 65536.0; }
    double c() const { return r1 / 65536.0; }
    double d() const { return sy / 65536.0; }
    void apply(double x, double y, double& ox, double& oy) const;
};

/// CXFORMWITHALPHA: (colour * mul / 256) + add per channel r, g, b, a.
struct ColourTransform {
    bool has_mul = false, has_add = false;
    int16_t mul[4] = {256, 256, 256, 256}, add[4] = {0, 0, 0, 0};
    BitHint bits;
    bool operator==(const ColourTransform&) const = default;
};

struct FillStyle {
    uint8_t type = 0;            // 0 solid, 0x10 linear, 0x12 radial, 0x13 focal gradient, 0x40..0x43 bitmap
    Rgba colour;                 // solid
    Matrix matrix;               // gradient and bitmap
    uint8_t gradient_flags = 0;  // spread mode << 6 | interpolation << 4 (the count is the records')
    std::vector<std::pair<uint8_t, Rgba>> gradient;   // ratio, colour
    int16_t focal = 0;           // 8.8, focal gradients
    uint16_t bitmap = 0;         // bitmap fills: the character id
    bool operator==(const FillStyle&) const = default;
    static FillStyle solid(Rgba c) { FillStyle f; f.colour = c; return f; }
};

struct LineStyle {
    uint16_t width = 20;         // twips
    Rgba colour;
    uint16_t flags = 0;          // DefineShape4 (LINESTYLE2): the 16 bits of caps, join, has fill, scale and hinting flags
    uint16_t miter = 0;          // DefineShape4 with a miter join (8.8)
    std::optional<FillStyle> fill;   // DefineShape4 with the has-fill flag
    bool operator==(const LineStyle&) const = default;
};

/// One SWF shape record. A style change selects styles (indices from 1 into the current style arrays, 0 = none),
/// moves the pen (absolute, from the shape's origin) and may start new style arrays; edges are deltas from the pen.
struct ShapeRecord {
    enum Kind : uint8_t { Style, Line, Curve } kind = Style;
    bool has_move = false;
    int32_t move_x = 0, move_y = 0;
    std::optional<uint32_t> fill0, fill1, line;
    bool new_styles = false;
    std::vector<FillStyle> fills;
    std::vector<LineStyle> lines;
    int32_t cdx = 0, cdy = 0;    // a curve's control point, from the pen
    int32_t dx = 0, dy = 0;      // the edge's end, from the control point (curve) or the pen (line)
    BitHint bits;                // the move's or the edge's bit count
    BitHint general;             // n = 1: a line along an axis written in the general (two-delta) form
    bool operator==(const ShapeRecord&) const = default;
};

struct Shape {
    uint16_t id = 0;
    int version = 2;             // 1 DefineShape, 2 DefineShape2, 3 DefineShape3 (RGBA), 4 DefineShape4
    Rect bounds;
    Rect edge_bounds;            // version 4
    uint8_t flags4 = 0;          // version 4: winding rule 4, non-scaling strokes 2, scaling strokes 1
    std::vector<FillStyle> fills;
    std::vector<LineStyle> lines;
    std::vector<ShapeRecord> records;
    bool operator==(const Shape&) const = default;
};

/// A shape's edges as absolute paths: a run of edges between style changes, with the styles in force and the style
/// arrays they index (`layer` counts the new-styles records before it).
struct ShapePath {
    int layer = 0;
    uint32_t fill0 = 0, fill1 = 0, line = 0;
    const FillStyle* fill0_style = nullptr;   // into the shape: valid while it lives
    const FillStyle* fill1_style = nullptr;
    const LineStyle* line_style = nullptr;
    struct Point { double x, y; bool curve; double cx, cy; };   // the end of an edge (and a curve's control point)
    double x0 = 0, y0 = 0;
    std::vector<Point> points;
};
std::vector<ShapePath> shape_paths(const Shape& s);
/// The area (square twips) each solid fill colour covers, from the signed areas of the edges on its two sides (curves
/// flattened into 8 pieces); layers and overlaps are summed.
std::vector<std::pair<Rgba, double>> shape_fill_areas(const Shape& s);
/// The bounds of the edges (curves by their control points).
Rect path_bounds(const Shape& s);

/// Builds a shape from closed polygons and polylines, layer by layer as the game's minimap art is drawn (an outline,
/// floors over it, furniture over those): a later layer draws over an earlier one. A polygon's fill goes on the side
/// of its inside whichever way its points run.
class ShapeBuilder {
public:
    using Points = std::vector<std::pair<int32_t, int32_t>>;
    explicit ShapeBuilder(uint16_t id, int version = 2) : id_(id), version_(version) { layers_.emplace_back(); }
    /// Starts a new layer (a new-styles record); an empty current layer is reused.
    void layer();
    /// A style of the current layer (indices from 1; equal styles are shared).
    uint32_t fill(Rgba c);
    uint32_t line(uint16_t width, Rgba c);
    /// A closed polygon (twips) filled with `fill` of the current layer, optionally outlined with `line`.
    void polygon(const Points& pts, uint32_t fill, uint32_t line = 0);
    /// An open polyline drawn with `line`.
    void polyline(const Points& pts, uint32_t line);
    /// The shape with its bounds computed (strokes widen them by half their width).
    Shape finish() const;
private:
    struct Path { Points pts; bool closed; uint32_t fill0, fill1, line; };
    struct Layer { std::vector<FillStyle> fills; std::vector<LineStyle> lines; std::vector<Path> paths; };
    uint16_t id_;
    int version_;
    std::vector<Layer> layers_;
};

Shape read_shape(const GfxTag& t);
GfxTag shape_tag(const Shape& s);

struct Place {
    bool move = false;
    uint16_t depth = 1;
    std::optional<uint16_t> character;
    std::optional<Matrix> matrix;
    std::optional<ColourTransform> colour;
    std::optional<uint16_t> ratio;
    std::string name;            // empty: no name
    std::optional<uint16_t> clip_depth;
    bool clip_actions = false;   // the clip actions are the end of `rest`
    // PlaceObject3 only
    bool place3 = false;
    uint8_t flags3 = 0;          // the second flag byte's bits this struct does not model (filters 0x01, image 0x10,
                                 // visible 0x20, background 0x40): their data stays in `rest`
    std::string class_name;
    std::optional<uint8_t> blend_mode;        // without filters (with them both stay in `rest`)
    std::optional<uint8_t> cache_as_bitmap;
    Bytes rest;                  // filters, clip actions ..., kept as bytes
    bool operator==(const Place&) const = default;
};
Place read_place(const GfxTag& t);   // PlaceObject2 or 3
GfxTag place_tag(const Place& p);

struct Sprite {
    uint16_t id = 0;
    uint16_t frame_count = 1;
    std::vector<GfxTag> tags;    // ends with End
};
Sprite read_sprite(const GfxTag& t);
GfxTag sprite_tag(const Sprite& s);

GfxTag show_frame_tag();
GfxTag end_tag();
GfxTag frame_label_tag(const std::string& label);
std::string read_frame_label(const GfxTag& t);
GfxTag export_assets_tag(const std::vector<std::pair<uint16_t, std::string>>& symbols);
GfxTag background_tag(Rgba c);
GfxTag file_attributes_tag(uint32_t flags = 0);

/// Scaleform's ExporterInfo (tag 1000) as the game's movies carry it: version 0x36F, flags 2, bitmap format 14, no
/// prefix, the movie's name, and the code offsets (none for a movie without ActionScript).
struct ExporterInfo {
    uint16_t version = 0x36F;
    uint32_t flags = 2;
    uint16_t bitmap_format = 14;
    std::string prefix;
    std::string swf_name;
    std::vector<uint32_t> code_offsets;
};
ExporterInfo read_exporter_info(const GfxTag& t);
GfxTag exporter_info_tag(const ExporterInfo& e);

/// The movie header's frame bytes: the frame rectangle, the rate (8.8) and the frame count.
Bytes frame_header(const Rect& frame, double rate, uint16_t frames);
Rect frame_rect(const GfxMovie& m);

/// The tags inside a sprite or a movie, with the depth of sprites (0 = the movie's own list).
struct TagRef { const GfxTag* tag; int sprite_depth; uint16_t sprite_id; };
struct TagWalk {
    std::vector<TagRef> tags;
    std::vector<std::unique_ptr<std::vector<GfxTag>>> sprites;   // the sprites' own tag lists the refs point into
};
TagWalk all_tags(const GfxMovie& m);

}  // namespace gfx

}  // namespace rk
