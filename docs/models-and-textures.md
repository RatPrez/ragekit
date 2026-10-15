# Models, textures and collision

| file | header | read | write |
|---|---|---|---|
| `.ytd` texture dictionary | `texture.h`, `image.h` | yes | yes |
| `.ydr` drawable | `drawable.h` | yes | yes |
| `.ydd` drawable dictionary | `drawable.h` | yes | yes |
| `.ybn` collision | `bounds.h` | yes | yes |
| `.yft` fragment | `fragment.h` | yes (no cloth) | no |

## Textures

`read_ytd` gives a `TextureDictionary`: textures sorted by name hash, as the game keeps them, each with its format,
size, mip count and the full mip chain as stored. Formats: DXT1/3/5, BC4/5/7, A8R8G8B8, A8, L8 and the other
uncompressed ones.

```cpp
TextureDictionary txd = read_ytd(rsc::read_file("props.ytd"));
const Texture* t = txd.find("prop_door_01");
Image rgba = decode_texture(*t);                                   // any format to 8-bit RGBA
rsc::write_file("door.png", write_png(rgba));
rsc::write_file("door.dds", write_dds(*t));                       // as stored

Texture mine = encode_texture(read_png(rsc::read_file("logo.png")), TextureFormat::DXT5, "logo");   // mips made
txd.add(mine);
rsc::write_file("props.ytd", write_ytd(txd));
```

`encode_texture` writes DXT1/5, BC4/5 (principal-axis endpoints with a least-squares refit; `rk_texenc_test` holds the
RMS error to limits) and the uncompressed formats, with the mip chain down to 1x1 unless told otherwise; `read_dds`
takes a DDS file as a texture.

## Drawables

```cpp
Drawable d = read_ydr(rsc::read_file("prop.ydr"));
for (const Model& m : d.models(Lod::High))
    for (const Geometry& g : m.geometries)
        for (uint32_t i = 0; i < g.vertex_count; i++) {
            Vec3 p = g.position(i);
            Vec2 uv = g.uv(i);
        }
rsc::write_file("prop_copy.ydr", write_ydr(d));
```

A drawable holds its shader group (shaders with their parameters, and the textures it embeds), up to four LODs of
models, each model a list of geometries (a vertex buffer with its declaration, a triangle list, a shader index), a
skeleton and joints when it has them, lights and an embedded collision bound. `Geometry::get` / `set` read and write
any vertex component through its declared type; `compute_bounds` recomputes boxes and spheres after editing, and
`compute_tangents` fills tangents the way the game's own bump-mapped shaders expect them.

`shader_db.h` reads every shader and preset of an install (`.fxc`, `.sps`) and knows the parameters a drawable stores
for each; `instantiate()` makes a shader exactly as the game's drawables hold it.

## Collision

```cpp
BoundPtr mesh = Bound::make_mesh(vertices, triangle_indices, material);   // a geometry with its BVH
BoundChild c{mesh};
c.flags1 = 0x3E; c.flags2 = 0x7F3BEC0;                                     // static map collision
rsc::write_file("thing.ybn", write_ybn(*Bound::make_composite({c})));
```

Every bound type reads and writes: composites, geometries with and without a BVH, boxes, spheres, capsules,
cylinders, discs. `Bound::finalize` recomputes what is derived (boxes, sphere, quantum, edges, materials table, shrunk
vertices, octants, BVH) after an edit. Materials carry the room id an interior's collision needs.

## CodeWalker XML

`xml.h` writes a drawable, a drawable dictionary, a collision bound or a texture dictionary as the XML CodeWalker's
"Export XML" produces, text for text, so CodeWalker's importers and Sollumz (Blender) read it. Textures are not in the
XML: each one is `<name>.dds` in a folder, which `dds_files()` makes.

```cpp
Drawable d = read_ydr(rsc::read_file("prop.ydr"));
std::string xml = write_drawable_xml(d);
for (auto& [name, dds] : dds_files(d)) rsc::write_file("prop/" + name, dds);
std::string bounds = write_bounds_xml(*read_ybn(rsc::read_file("x.ybn")));
std::string textures = write_texture_dictionary_xml(read_ytd(rsc::read_file("x.ytd")));
```

The same element order, tag names, one-space indentation, self-closing rules, number formatting (.NET's shortest round
trip: `1E-05`, `0.07499981`, `NaN`) and CR LF line ends as CodeWalker. `XmlOptions`: `hash_name` names hashes the
library does not know (it knows every stock shader and preset name and CodeWalker's shader parameter table); `crlf`
(default true); `unknown_param_as_number` (default false: a parameter outside CodeWalker's table is written
`hash_XXXXXXXX`, which its importer reads back). `rk xml <file> <out folder>` does what CodeWalker's command does.
[testing.md](testing.md#codewalker-xml) explains how this is checked against CodeWalker.

## glTF

`write_glb` exports a drawable, a fragment or a set of placed drawables as a binary glTF 2.0 file with PNG textures,
turned from the game's Z-up to glTF's Y-up. `GltfOptions` picks the LOD, whether to embed textures, and a
`find_texture` callback for textures the drawable takes from other dictionaries (see `examples/to_gltf.cpp`).
