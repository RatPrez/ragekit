// ragekit - glTF 2.0 interchange. Drawables become meshes with PBR-ish materials and
// embedded PNG textures; the game's Z-up axes are converted to glTF's Y-up.
#pragma once
#include <functional>
#include <string>
#include <vector>
#include "drawable.h"
#include "fragment.h"

namespace rk {

struct GltfOptions {
    Lod lod = Lod::High;                 // which LOD to export
    bool textures = true;                // embed diffuse/normal textures as PNG
    bool all_lods_as_nodes = false;      // every LOD as its own node (named by LOD)
    /// Finds a texture by name when the drawable's own dictionary lacks it (e.g. a car's .ytd).
    std::function<const Texture*(std::string_view name)> find_texture;
};

/// One placed drawable in the export.
struct GltfInstance {
    const Drawable* drawable;
    Mat4 transform;        // game space (row vectors, translation in row 4)
    std::string name;
};

/// Builds a binary glTF (.glb) from placed drawables.
Bytes write_glb(const std::vector<GltfInstance>& instances, const GltfOptions& opt = {});
Bytes write_glb(const Drawable& d, const GltfOptions& opt = {});
Bytes write_glb(const Fragment& f, const GltfOptions& opt = {});

} // namespace rk
