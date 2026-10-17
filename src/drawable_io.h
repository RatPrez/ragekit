// ragekit internal - drawable structures inside a resource, shared with fragments.
#pragma once
#include "ragekit/drawable.h"
#include "ragekit/rsc.h"
#include <map>
#include <vector>
#include "texture_io.h"

namespace rk {

/// Reads the 168 byte rmcDrawableBase fields (offsets 0x10..0xA8) and everything they point at.
void read_drawable_base(const Reader& r, uint64_t ptr, Drawable& d);
/// Reads a full 208 byte gtaDrawable (base + name, lights, bound).
Drawable read_drawable(const Reader& r, uint64_t ptr);
/// Reads one 168 byte light record.
Light read_light_at(const Reader& r, uint64_t ptr);
/// Writes one 168 byte light record at the writer's position.
void write_light_at(BlockWriter& w, const Light& l);

/// Blocks a drawable's base fields point at; produced by prepare_drawable.
struct DrawableBlocks {
    BlockId shader_group, skeleton, joints, models;
    uint64_t models_offsets[5] = {0, 0, 0, 0, 0}; // offset of each LOD list inside `models`, UINT64_MAX when absent
    uint64_t models_size = 0;
};

/// The skeletons one file has written, keyed by their bytes. A fragment's drawables share one skeleton block (the main
/// drawable's and a child's in the game's glass panes): the game constructs it once, so a second, identical copy keeps the
/// file's vtable word and its first virtual call crashes.
struct SkeletonCache { std::vector<std::pair<Bytes, BlockId>> written; };

/// Writes every sub-structure of the drawable (shader group, skeleton, models...). With `skeletons`, a skeleton identical to
/// one written before points at that block.
DrawableBlocks prepare_drawable(Builder& b, const Drawable& d, SkeletonCache* skeletons = nullptr);
/// Writes offsets 0x10..0xA8 of a drawable struct; the writer must be positioned at 0x10.
void write_drawable_base(BlockWriter& w, const Drawable& d, const DrawableBlocks& parts, Builder& b);
/// Writes a complete 208 byte gtaDrawable block; `root` makes it the resource root.
BlockId write_drawable(Builder& b, const Drawable& d, bool root);

/// Enhanced vertex declarations (320 bytes): the Legacy layout of one (and the vertex size it gives), and the declaration
/// of a layout whose types all have an Enhanced format (see layout_gen9).
VertexLayout layout_from_gen9(const uint8_t* decl, uint16_t& stride);
void layout_to_gen9(const VertexLayout& l, uint8_t* decl);

/// While alive, numbers the sampler states of the shaders an Enhanced write makes from the shader table, the way the
/// game's files number them: one number per sampler name across the file, after those of the shaders read from Enhanced.
struct Gen9SamplerScope {
    explicit Gen9SamplerScope(const std::vector<const Drawable*>& drawables);
    ~Gen9SamplerScope();
    Gen9SamplerScope(const Gen9SamplerScope&) = delete;
    Gen9SamplerScope& operator=(const Gen9SamplerScope&) = delete;
    std::map<uint32_t, uint8_t> numbers;
    uint8_t next = 0;
private:
    Gen9SamplerScope* previous_;
};

/// Virtual table values the game's files carry (any value works; kept for small diffs).
namespace vft {
constexpr uint32_t kDrawable      = 1079456120u;
constexpr uint32_t kShaderGroup   = 1080113136u;
constexpr uint32_t kSkeleton      = 1080114336u;
constexpr uint32_t kJoints        = 1080130656u;
constexpr uint32_t kModel         = 1080101528u;
constexpr uint32_t kGeometry      = 1080133528u;
constexpr uint32_t kVertexBuffer  = 1080153080u;
constexpr uint32_t kIndexBuffer   = 1080152408u;
constexpr uint32_t kDrawableDict  = 1079456120u;
}

} // namespace rk
