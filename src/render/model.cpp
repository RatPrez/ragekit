#include "ragekit/render/model.h"
#include <algorithm>
#include <cfloat>
#include <functional>
#include "ragekit/gameindex.h"
#include "ragekit/hash.h"

namespace rk::render {

namespace {

bool attrib_of(VertexSemantic s, Attrib& out) {
    switch (s) {
        case VertexSemantic::Position: out = Attrib::Position; return true;
        case VertexSemantic::Normal: out = Attrib::Normal; return true;
        case VertexSemantic::Tangent: out = Attrib::Tangent; return true;
        case VertexSemantic::Colour0: out = Attrib::Colour0; return true;
        case VertexSemantic::Colour1: out = Attrib::Colour1; return true;
        case VertexSemantic::TexCoord0: out = Attrib::TexCoord0; return true;
        case VertexSemantic::TexCoord1: out = Attrib::TexCoord1; return true;
        case VertexSemantic::BlendWeights: out = Attrib::BlendWeights; return true;
        case VertexSemantic::BlendIndices: out = Attrib::BlendIndices; return true;
        default: return false;
    }
}

bool format_of(VertexComponentType t, VertexSemantic s, AttribFormat& out) {
    switch (t) {
        case VertexComponentType::Float: out = AttribFormat::Float1; return true;
        case VertexComponentType::Float2: out = AttribFormat::Float2; return true;
        case VertexComponentType::Float3: out = AttribFormat::Float3; return true;
        case VertexComponentType::Float4: out = AttribFormat::Float4; return true;
        case VertexComponentType::Half2: out = AttribFormat::Half2; return true;
        case VertexComponentType::Half4: out = AttribFormat::Half4; return true;
        // R8G8B8A8_UNORM in the game's declarations (bytes in RGBA order); blend indices are integers
        case VertexComponentType::Colour: out = s == VertexSemantic::BlendIndices ? AttribFormat::UInt8x4 : AttribFormat::UNorm8x4; return true;
        case VertexComponentType::UByte4: out = AttribFormat::UInt8x4; return true;
        case VertexComponentType::RGBA8SNorm: out = AttribFormat::SNorm8x4; return true;
        default: return false;
    }
}

Vec3 transform_point(const Mat4& m, Vec3 p) {   // row vectors: p * M
    return {p.x * m.at(0, 0) + p.y * m.at(1, 0) + p.z * m.at(2, 0) + m.at(3, 0),
            p.x * m.at(0, 1) + p.y * m.at(1, 1) + p.z * m.at(2, 1) + m.at(3, 1),
            p.x * m.at(0, 2) + p.y * m.at(1, 2) + p.z * m.at(2, 2) + m.at(3, 2)};
}

const std::vector<rk::Model>* models_at(const Drawable& d, Lod lod) {
    // the LOD asked for, else the next lower one with models, else the next higher
    for (int l = (int)lod; l < 4; l++) if (!d.lods[l].empty()) return &d.lods[l];
    for (int l = (int)lod - 1; l >= 0; l--) if (!d.lods[l].empty()) return &d.lods[l];
    return nullptr;
}

// A model hangs on a bone (skeleton_binding); a skinned model is in the skeleton's bind pose already.
Mat4 bone_placement(const Drawable& d, const rk::Model& m) {
    if (m.has_skin() || !d.skeleton) return Mat4::identity();
    return d.skeleton->bind_matrix(m.bone_index());
}

// The fragment's default pose (Fragment::bone_transforms, 3 rows of 4 in column-vector form) for a bone, as a row-vector
// matrix; CodeWalker's Renderable does the same.
std::optional<Mat4> frag_pose(const Fragment& f, uint32_t bone) {
    if (bone >= f.bone_transforms.size()) return std::nullopt;
    auto& p = f.bone_transforms[bone];
    Mat4 m;
    for (int c = 0; c < 3; c++)
        for (int r = 0; r < 4; r++) m.at(r, c) = p[(size_t)c * 4 + r];
    m.at(0, 3) = m.at(1, 3) = m.at(2, 3) = 0;
    m.at(3, 3) = 1;
    return m;
}

struct Builder {
    Model out;
    std::vector<const TextureDictionary*> own;   // the drawables' embedded dictionaries
    const TextureDictionaries* txds = nullptr;

    /// `place` gives each model's transform inside the whole (the bone it hangs on).
    void add(const Drawable& d, const Drawable* shaders_from, const std::function<Mat4(const rk::Model&)>& place, Lod lod) {
        const Drawable& sd = d.shader_group ? d : (shaders_from ? *shaders_from : d);
        if (!sd.shader_group) return;
        if (sd.shader_group->textures) own.push_back(&*sd.shader_group->textures);
        const uint32_t base = (uint32_t)out.materials.size();
        for (auto& s : sd.shader_group->shaders) out.materials.push_back(make_material(s));
        auto* models = models_at(d, lod);
        if (!models) return;
        for (auto& model : *models) {
            const Mat4 transform = place(model);
            const bool proxy = (model.render_mask & 1) == 0;
            for (auto& g : model.geometries) {
                if (g.vertex_count == 0 || g.indices.empty() || g.shader_index >= sd.shader_group->shaders.size()) continue;
                Mesh m;
                m.stride = g.layout.stride();
                m.vertex_count = g.vertex_count;
                m.vertices = std::span<const uint8_t>(g.vertices.data(), (size_t)g.vertex_count * m.stride);
                m.indices = g.indices;
                m.material = base + g.shader_index;
                m.transform = transform;
                m.shadow_only = proxy || out.materials[m.material].has(FeatShadowOnly);
                m.bounds_min = {g.bounds.min.x, g.bounds.min.y, g.bounds.min.z};
                m.bounds_max = {g.bounds.max.x, g.bounds.max.y, g.bounds.max.z};
                for (int i = 0; i < 16; i++) {
                    auto s = (VertexSemantic)i;
                    VertexAttrib a;
                    if (!g.layout.has(s) || !attrib_of(s, a.attrib) || !format_of(g.layout.type_of(s), s, a.format)) continue;
                    a.offset = g.layout.offset_of(s);
                    m.attribs.push_back(a);
                }
                out.meshes.push_back(std::move(m));
            }
        }
    }

    void finish() {
        Vec3 lo{FLT_MAX, FLT_MAX, FLT_MAX}, hi{-FLT_MAX, -FLT_MAX, -FLT_MAX};
        for (auto& m : out.meshes) {
            for (int c = 0; c < 8; c++) {
                Vec3 p{c & 1 ? m.bounds_max.x : m.bounds_min.x, c & 2 ? m.bounds_max.y : m.bounds_min.y,
                       c & 4 ? m.bounds_max.z : m.bounds_min.z};
                p = transform_point(m.transform, p);
                lo = vmin(lo, p); hi = vmax(hi, p);
            }
        }
        if (out.meshes.empty()) lo = hi = {};
        out.bounds_min = lo; out.bounds_max = hi;

        // every texture a drawn material samples: the drawable's own dictionaries first, then the given ones in order
        std::vector<bool> drawn(out.materials.size());
        for (auto& m : out.meshes) if (!m.shadow_only) drawn[m.material] = true;
        for (size_t i = 0; i < out.materials.size(); i++) {
            if (!drawn[i]) continue;
            auto& mat = out.materials[i];
            for (auto& slot : mat.textures) {
                if (!slot.used() || out.texture(slot.hash)) continue;
                const Texture* t = nullptr;
                for (auto* d : own) if ((t = d->find(slot.hash))) break;
                if (!t && txds) for (auto& d : *txds) if (d && (t = d->find(slot.hash))) break;
                if (!t) {
                    if (std::find(out.missing.begin(), out.missing.end(), slot.name) == out.missing.end())
                        out.missing.push_back(slot.name);
                    continue;
                }
                auto td = texture_data(*t);
                td.hash = slot.hash;
                out.textures.push_back(std::move(td));
            }
        }
        if (txds) for (auto& d : *txds) if (d) out.sources.push_back(d);
    }
};

} // namespace

bool pixel_format_is_block(PixelFormat f) { return f >= PixelFormat::BC1 && f <= PixelFormat::BC7; }

TextureData texture_data(const Texture& t) {
    TextureData td;
    td.name = t.name;
    td.hash = t.name_hash();
    td.width = t.width;
    td.height = t.height;
    switch (t.format) {
        case TextureFormat::DXT1: td.format = PixelFormat::BC1; break;
        case TextureFormat::DXT3: td.format = PixelFormat::BC2; break;
        case TextureFormat::DXT5: td.format = PixelFormat::BC3; break;
        case TextureFormat::ATI1: td.format = PixelFormat::BC4; break;
        case TextureFormat::ATI2: td.format = PixelFormat::BC5; break;
        case TextureFormat::BC7: td.format = PixelFormat::BC7; break;
        case TextureFormat::A8R8G8B8: td.format = PixelFormat::BGRA8; break;
        case TextureFormat::X8R8G8B8: td.format = PixelFormat::BGRX8; break;
        case TextureFormat::A8B8G8R8: td.format = PixelFormat::RGBA8; break;
        case TextureFormat::A1R5G5B5: td.format = PixelFormat::BGR5A1; break;
        case TextureFormat::A8: td.format = PixelFormat::A8; break;
        case TextureFormat::L8: td.format = PixelFormat::L8; break;
        default: td.format = PixelFormat::Unknown; break;
    }
    if (td.format == PixelFormat::Unknown || t.depth > 1) return td;
    for (uint32_t l = 0; l < t.levels; l++) {
        TextureLevel lv;
        lv.data = t.level(l, lv.width, lv.height);
        if (lv.data.empty()) break;
        td.levels.push_back(lv);
    }
    return td;
}

const TextureData* Model::texture(uint32_t hash) const {
    for (auto& t : textures) if (t.hash == hash) return &t;
    return nullptr;
}

// A bone's matrix by its tag (lights name their bone by tag): the skeleton's bind pose, else the fragment's default pose.
Mat4 bone_by_tag(const Drawable& d, uint16_t tag, const Fragment* f = nullptr) {
    if (!d.skeleton) return Mat4::identity();
    const Bone* b = d.skeleton->find_tag(tag);
    if (!b) return Mat4::identity();
    if (f)
        if (auto pose = frag_pose(*f, (uint32_t)b->index)) return *pose;
    return d.skeleton->bind_matrix((size_t)b->index);
}

void add_lights(Model& out, const std::vector<rk::Light>& attrs, const Drawable& d, const Fragment* f = nullptr) {
    auto ls = lights_of(attrs, [&](uint16_t tag) { return bone_by_tag(d, tag, f); });
    for (auto& l : ls) l.seed += (uint32_t)out.lights.size() * 7919u;
    out.lights.insert(out.lights.end(), ls.begin(), ls.end());
}

Model build_model(std::shared_ptr<const Drawable> d, const TextureDictionaries& txds, Lod lod) {
    Builder b;
    b.txds = &txds;
    if (d) {
        b.out.name = d->name;
        const Drawable& dr = *d;
        b.add(dr, nullptr, [&](const rk::Model& m) { return bone_placement(dr, m); }, lod);
        add_lights(b.out, dr.lights, dr);
        b.out.sources.push_back(d);
    }
    b.finish();
    return std::move(b.out);
}

Model build_model(const std::vector<std::shared_ptr<const Drawable>>& parts, const TextureDictionaries& txds, Lod lod) {
    Builder b;
    b.txds = &txds;
    for (auto& d : parts) {
        if (!d) continue;
        if (b.out.name.empty()) b.out.name = d->name;
        const Drawable& dr = *d;
        b.add(dr, nullptr, [&](const rk::Model& m) { return bone_placement(dr, m); }, lod);
        add_lights(b.out, dr.lights, dr);
        b.out.sources.push_back(d);
    }
    b.finish();
    return std::move(b.out);
}

Model build_model(std::shared_ptr<const Fragment> f, const TextureDictionaries& txds, Lod lod) {
    Builder b;
    b.txds = &txds;
    if (f) {
        b.out.name = f->name;
        // the main drawable: each model on its bone in the fragment's default pose
        const Drawable* main = f->drawable ? &*f->drawable : (f->drawable_cloth ? &*f->drawable_cloth : nullptr);
        if (main) {
            b.add(*main, nullptr, [&](const rk::Model& m) {
                if (m.has_skin()) return Mat4::identity();
                auto pose = frag_pose(*f, m.bone_index());
                return pose ? *pose : bone_placement(*main, m);
            }, lod);
            add_lights(b.out, f->lights, *main, f.get());
            add_lights(b.out, main->lights, *main, f.get());
        }
        // the physics children (wheels, doors, breakable parts) at their LOD 1 transforms; a child without a shader
        // group draws with the main drawable's shaders
        if (auto& pl = f->physics[0]) {
            const Vec3 offset = pl->position_offset();
            // vehicles carry one wheel drawable per axle; the other wheels draw a copy of it, the right side wheels
            // turned half round (CodeWalker's Renderer.RenderFragment does the same)
            auto axle = [](uint16_t tag) {
                switch (tag) {
                    case 27922: case 26418: return 1;                                // wheel_lf, wheel_rf
                    case 29921: case 29922: case 29923: case 27902:                  // wheel_lm1..3, wheel_lr
                    case 5857: case 5858: case 5859: case 26398: return 2;           // wheel_rm1..3, wheel_rr
                    default: return 0;
                }
            };
            auto right = [](uint16_t tag) { return tag == 26418 || tag == 5857 || tag == 5858 || tag == 5859 || tag == 26398; };
            auto drawn = [](const std::optional<FragDrawable>& d) {
                if (!d) return false;
                for (auto& l : d->lods) if (!l.empty()) return true;
                return false;
            };
            const Drawable *front = nullptr, *rear = nullptr;
            for (auto& ch : pl->children) {
                if (!drawn(ch.drawable)) continue;
                if (axle(ch.bone_tag) == 1 && !front) front = &*ch.drawable;
                if (axle(ch.bone_tag) == 2 && !rear) rear = &*ch.drawable;
            }
            for (size_t i = 0; i < pl->children.size(); i++) {
                auto& ch = pl->children[i];
                const Drawable* d = drawn(ch.drawable) ? &*ch.drawable : nullptr;
                Mat4 t = i < pl->transforms.size() ? pl->transforms[i] : Mat4::identity();
                t.at(3, 0) += offset.x; t.at(3, 1) += offset.y; t.at(3, 2) += offset.z;
                t.at(0, 3) = t.at(1, 3) = t.at(2, 3) = 0;   // stored 4x3: the last column is not 0,0,0,1 in the files
                t.at(3, 3) = 1;
                if (int a = axle(ch.bone_tag)) {
                    if (!d) d = a == 1 ? (front ? front : rear) : (rear ? rear : front);
                    if (right(ch.bone_tag))
                        for (int r = 0; r < 3; r++)
                            for (int c = 0; c < 3; c++) t.at(r, c) = r != c ? 0.f : r == 1 ? 1.f : -1.f;
                }
                if (!d) continue;
                b.add(*d, main, [&](const rk::Model&) { return t; }, lod);
            }
        }
        b.out.sources.push_back(f);
    }
    b.finish();
    return std::move(b.out);
}

std::optional<ArchetypeModel> load_archetype(const GameIndex& gi, const std::string& name, Lod lod) {
    const uint32_t h = hash_or_name(name);
    auto arch = gi.archetype(h);
    ResolvedDrawable rd = arch ? gi.resolve_drawable(*arch) : gi.resolve_drawable(h);
    // a ped (peds.meta, not a ytyp): its components in a .ydd of its name (the .yft of the name is its ragdoll)
    std::optional<IndexedFile> ped_ydd = arch ? std::nullopt : gi.resolve_file(h, ".ydd");
    if (ped_ydd) rd.type = ResolvedDrawable::None;
    if (!ped_ydd && (rd.type == ResolvedDrawable::None || !rd.file)) return std::nullopt;

    // the archetype's texture dictionary (a bare drawable: the one of its own name), then its gtxd parents
    ArchetypeModel am;
    TextureDictionaries txds;
    std::vector<uint32_t> roots{arch && arch->texture_dictionary ? arch->texture_dictionary : h};
    if (!arch && rd.type == ResolvedDrawable::Yft) {
        // a vehicle (vehicles.meta, not a ytyp): "<name>_hi" draws with "<name>+hi" over "<name>"; vehshare last, in
        // case vehicles.meta names no parent
        std::string lower = name;
        for (auto& ch : lower) ch = (char)std::tolower((unsigned char)ch);
        if (lower.size() > 3 && lower.ends_with("_hi")) {
            const std::string base = lower.substr(0, lower.size() - 3);
            roots = {joaat_lower(base + "+hi"), joaat_lower(base)};
        }
    }
    std::vector<uint32_t> chain;
    auto add = [&](uint32_t t) { if (std::find(chain.begin(), chain.end(), t) == chain.end()) chain.push_back(t); };
    for (uint32_t r : roots) {
        add(r);
        for (uint32_t p : gi.txd_parents(r)) add(p);   // gtxd and vehicles.meta txdRelationships
    }
    // the dictionaries the game keeps resident, last: mapdetail (the ENV_* detail maps) and vehshare (CodeWalker's
    // GameFileCache loads the same two for textures no dictionary chain names)
    add(joaat_lower("mapdetail"));
    add(joaat_lower("vehshare"));
    for (uint32_t t : chain) {
        auto f = gi.resolve_txd(t);
        if (!f) { am.txd_names.push_back(gi.name(t) + " (not found)"); continue; }
        try {
            auto bytes = gi.extract(*f);
            txds.push_back(std::make_shared<const TextureDictionary>(read_ytd(bytes)));
            am.txd_names.push_back(gi.file_name(*f));
        } catch (const std::exception&) {
            // an unreadable dictionary leaves its textures missing
        }
    }

    if (ped_ydd) {
        // variation 000 of each component: "<component>_000_<suffix>" (u universal, r race, m ...)
        auto dict = std::make_shared<const DrawableDictionary>(read_ydd(gi.extract(*ped_ydd)));
        am.drawable_path = gi.path(*ped_ydd);
        std::vector<std::shared_ptr<const Drawable>> parts;
        for (const char* comp : {"head", "berd", "hair", "uppr", "lowr", "hand", "feet", "teef", "accs", "task", "decl", "jbib"})
            for (const char* race : {"u", "r", "m"})
                if (const Drawable* d = dict->find(joaat_lower(std::string(comp) + "_000_" + race))) {
                    parts.push_back(std::shared_ptr<const Drawable>(dict, d));
                    break;
                }
        if (parts.empty() && !dict->drawables.empty()) parts.push_back(std::shared_ptr<const Drawable>(dict, &dict->drawables[0]));
        am.model = build_model(parts, txds, lod);
        am.model.name = gi.name(h);
        return am;
    }

    auto bytes = gi.extract(*rd.file);
    am.drawable_path = gi.path(*rd.file);
    switch (rd.type) {
        case ResolvedDrawable::Ydr:
            am.model = build_model(std::make_shared<const Drawable>(read_ydr(bytes)), txds, lod);
            break;
        case ResolvedDrawable::Yft:
            am.model = build_model(std::make_shared<const Fragment>(read_yft(bytes)), txds, lod);
            break;
        case ResolvedDrawable::Ydd: {
            auto dict = std::make_shared<const DrawableDictionary>(read_ydd(bytes));
            const Drawable* d = dict->find(rd.drawable_name);
            if (!d) return std::nullopt;
            am.model = build_model(std::shared_ptr<const Drawable>(dict, d), txds, lod);
            break;
        }
        default: return std::nullopt;
    }
    if (am.model.name.empty()) am.model.name = gi.name(h);
    return am;
}

} // namespace rk::render
