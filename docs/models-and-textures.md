# Models, textures and collision

| file | header | read | write |
|---|---|---|---|
| `.ytd` texture dictionary | `texture.h`, `image.h` | yes | yes |
| `.ydr` drawable | `drawable.h` | yes | yes |
| `.ydd` drawable dictionary | `drawable.h` | yes | yes |
| `.ybn` collision | `bounds.h` | yes | yes |
| `.yld` cloth dictionary | `cloth.h` | yes | yes |
| `.yft` fragment | `fragment.h` | yes (with its cloth) | no (its cloth: yes) |

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

## Cloth

A `.yld` is a cloth dictionary (RSC7 version 8) of character cloth, the parts of a ped's clothing the game simulates,
keyed by the name of the drawable they belong to in the `.ydd` of the same name beside it. The install has 61, each
with one cloth: story and cutscene peds (`cutspeds.rpf`, `streamedpeds_players.rpf`, `componentpeds_*.rpf` and DLC
copies). Fragments carry environment cloth (flags, banners, canopies, parasols, tarps): 1,828 of the 61,430 `.yft`,
one cloth each, which `read_yft` reads into `Fragment::cloths`.

```cpp
ClothDictionary d = read_yld(rsc::read_file("uppr_025_u.yld"));
const CharacterCloth& c = d.cloths[0];                     // d.hashes[0]: joaat("uppr_025_u")
const VerletCloth& sim = *c.controller->verlet[0];         // the simulated mesh: vertices, edges
rsc::write_file("uppr_025_u.yld", write_yld(d));

Fragment f = read_yft(rsc::read_file("prop_flag_us.yft"));
for (const EnvironmentCloth& e : f.cloths) { ... }         // drawn through f.drawable_cloth if set, else f.drawable
```

Both kinds hang off a cloth controller: the bridge between simulated and drawn vertices, the verlet cloth of up to
three LODs (vertices, edge constraints and, for environment cloth, a collision bound of capsules and planes) and, for
environment cloth, a morph controller between the LODs. A character cloth adds the skinning (vertices, triangles, bone
weights, bone ids) and a bound of capsules with one bone id per capsule; an environment cloth may add an instance
tuning. Unknown fields that vary are kept as `unk*` members named after their offset; the rest are zero or constant in
every game file and are written as they are found. A fragment writer embeds environment cloth with
`write_environment_cloth` (`src/cloth_io.h`), passing the block of the fragment drawable the cloth draws through.

What the game's files show:

| structure | bytes | layout |
|---|---|---|
| cloth dictionary | 0x40 | 0x20 name hashes (sorted), 0x30 cloth pointers |
| characterCloth | 0xD0 | 0x10 Vec4 list, 0x20 controller, 0x28 bound, 0x30 a bone id per bound child, 0x50 matrix (m33 is 0), 0x90 u32 list |
| clothController | 0x80 | 0x10 bridge, 0x18 morph controller, 0x20 0x28 0x30 verlet cloth per LOD (0x38 always null), 0x50 type (2 character, 3 environment), 0x58 name char[32], 0x78 float (1 character, 0 environment) |
| characterClothController | 0xF0 | a controller, then 0x80 u16 triangles, 0x90 vertices, 0xB0 u32 list, 0xC0 bone weights (Vec4 and 4 u32), 0xE0 bone ids |
| clothBridgeSimGfx | 0x140 | 0x10 vertex count per LOD (that LOD's verlet cloth's); per LOD a list at 0x20, 0x60, 0xA0 (floats; CodeWalker reads LODs 1 and 2 of 0x60 and 0xA0 as integers) and 0xE0 (u16, a vertex map); 0x128 a bit set: count ceil(bits / 32) words, capacity the bit count (the vertex count, one more in 2 files) |
| phVerletCloth | 0x180 | 0x18 bound, 0x30 0x40 box (with 0x7f7fffff / 0xff7fffff in fragments), 0x50 an embedded object with its own vtable (not a float), 0x70 vertices2, 0x80 vertices, 0xEC edge count, 0xF0 and 0x14A vertex count, 0x100 custom edges, 0x110 edges (u16, u16, 3 floats), 0x130 a 0x40 byte behaviour and 0x140 16 bytes, both all zero |
| phMorphController | 0x40 | 0x18 0x20 0x28 a morph map per LOD (0x190 bytes: Vec4 lists at 0x50 0xA0, u16 lists at 0x60..0x90, 0xB0..0xE0, 0x150, 0x160, u32 at 0x180) |
| environmentCloth | 0x80 | 0x10 instance tuning (0x40 bytes, 211 files), 0x18 drawable (the fragment's at 0xF8 when it has one, else the one at 0x30), 0x28 controller, 0x60 u32 list (count at 0x68 and 0x6A, 3 files), 0x78 u32 (0, or 16 / 48 with a tuning) |

In cloth bounds a capsule carries a float at 0x70 (now kept as `Bound::unk70`), and type 15 (`BoundType::Cloth`) is a
112 byte plane: a point on it in `box_center`, its normal (horizontal, about unit length) in `sphere_center`.

`rk_roundtrip --game --only=.yld,.yft` reads, writes and reads again every cloth of the install and compares the two
models field by field: all 61 `.yld` and all 1,828 fragment cloths compare equal. Walking the structures by offset in
the original and the written resource, every byte of every structure and array (25.8 MB in 1,889 cloths) is the same
except the vtable words, which ragekit writes from one table (the game's files carry several, by exporter build).

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
