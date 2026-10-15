# ragekit

[![Buy Me A Coffee](https://img.shields.io/badge/Buy%20Me%20A%20Coffee-ffdd00?style=for-the-badge&logo=buy-me-a-coffee&logoColor=black)](https://buymeacoffee.com/ratprez)

A C++20 library for reading and writing the files of Grand Theft Auto V (the RAGE engine): models, textures,
collision, map files, meta and PSO data, audio data and RPF archives. Every format is a plain struct; the only
dependency, miniz (public domain), is vendored; nothing ties it to any particular tool.

ragekit is not affiliated with or endorsed by Rockstar Games or Take-Two Interactive. It contains no game data: you work
on files from your own copy of the game.

```
include/ragekit/   the public headers (ragekit.h includes them all)
src/               the library
examples/          small complete programs
docs/              how to use it, and what is known about the formats
tools/             rk, the command line tool, and the generators of the name tables
tests/             round trips, test programs, the CodeWalker oracle
third_party/miniz  deflate (public domain)
```

## What it reads and writes

| Format | Read | Write | Notes |
|---|---|---|---|
| RSC7 resources | yes | yes | the container of every streamed asset; its own page layout (`rsc.h`) |
| .ytd textures | yes | yes | every DXT/BC/uncompressed format decoded to RGBA; DXT1/5, BC4/5 and uncompressed encoded; DDS and PNG |
| .ydr drawables, .ydd dictionaries | yes | yes | shaders, textures, skeleton, joints, LODs, lights, embedded collision |
| .ybn collision | yes | yes | every bound type, BVH read and build, meshes from triangles |
| .yft fragments | yes | no | drawable, physics LODs, groups, children, archetypes, glass |
| .ytyp / .ymap | yes | yes | typed archetypes (interiors with rooms, portals, entity sets), entities, MLO instances, car generators, occluders; a lossless generic meta layer under them |
| PSO (binary .ymt, .ypdb, .ymf) | yes | yes | byte for byte; a schema-driven walker; typed streaming manifests |
| .rel audio data | yes | yes | byte for byte; the dat151 items an interior needs; interior audio occlusion files |
| RPF7 archives | yes | yes | AES, NG, OPEN and plain; nested archives; writes plain, OPEN and AES |
| CodeWalker XML | no | yes | what CodeWalker's "Export XML" writes for .ydr/.ydd/.ybn/.ytd, which CodeWalker and Sollumz import |
| glTF 2.0 | no | yes | .glb with textures |
| .fxc / .sps shaders | yes | no | constant buffers, parameters, techniques; a database of an install's shaders |

And beyond single files: an index of every file and archetype of a GTA V install by name, with the game's DLC load
order (`gameindex.h`), and a world index of what stands where on the map the game loads (`world.h`).

## Made with ragekit

<a href="https://modlocreator.app"><img src="https://modlocreator.app/og-image.png" alt="Modlo Creator: build GTA interiors. No Blender. No CodeWalker." width="640"></a>

<p>
  <a href="https://modlocreator.app"><img src="https://modlocreator.app/brand/mark.svg" alt="" width="22" align="absmiddle"></a>&nbsp;
  <a href="https://modlocreator.app"><b>Modlo Creator</b></a>: a tailor-made 3D editor for GTA interiors (MLOs). Draw walls, rooms and
  doors, furnish and light them, and export a ready-to-stream FiveM resource. Everything it writes for the game goes through
  ragekit. <a href="https://modlocreator.app">Get it at modlocreator.app</a>.
</p>

Made something with ragekit? Open an issue or a pull request to add it here.

## Quick start

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

```cmake
include(FetchContent)
FetchContent_Declare(
    ragekit
    GIT_REPOSITORY https://github.com/RatPrez/ragekit.git
    GIT_TAG        v0.1.0        # a release tag (or a commit hash); main follows the latest
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(ragekit)

target_link_libraries(my_tool PRIVATE ragekit::ragekit)
```

or, with a copy of the repository next to your project, `add_subdirectory(ragekit)` and the same `target_link_libraries`.

```cpp
#include <ragekit/ragekit.h>
using namespace rk;

Drawable d = read_ydr(rsc::read_file("prop.ydr"));
for (const Model& m : d.models(Lod::High))
    for (const Geometry& g : m.geometries)
        std::printf("%u vertices, %zu triangles\n", g.vertex_count, g.indices.size() / 3);
rsc::write_file("prop_copy.ydr", write_ydr(d));
```

## Examples

`examples/` holds small, complete programs, built with the library when ragekit is the top-level project:

- `ydr_info` - what a drawable holds: LODs, models, geometries, shaders, embedded textures
- `ytd_export` - a texture dictionary to `.dds` and `.png` files
- `to_gltf` - a drawable to a binary glTF with its textures
- `box_collision` - a collision file made from a triangle mesh
- `make_interior` - an interior (`.ytyp`) and the map file that places it (`.ymap`)
- `rpf_list` - listing an archive, nested archives included, and extracting a file
- `game_lookup` - finding an archetype in a GTA V install and pulling its model and textures

## Documentation

- [Getting started](docs/getting-started.md): building, using it from CMake, conventions, keys
- [RSC7 resources](docs/resources.md): the container, page flags, building resources, the page layout
- [Models, textures and collision](docs/models-and-textures.md): ytd, ydr, ydd, ybn, yft, CodeWalker XML, glTF
- [Map files](docs/maps.md): the meta container, ytyp and ymap, occluders
- [PSO, manifests and audio data](docs/pso-and-audio.md): PSO files, `_manifest.ymf`, `.rel`, interior audio
- [RPF archives](docs/archives.md): reading, keys, writing
- [The game index and the world index](docs/game-index.md)
- [The rk tool](docs/rk-tool.md)
- [How it is checked](docs/testing.md)
- [ragekit and CodeWalker](docs/codewalker-comparison.md): format coverage side by side

## How it is checked

Against the game's own files and against CodeWalker. A sample of 4,860 files of the install (every 50th resource,
PSO and audio file) is read, written and read again, and every one compares equal field for field; a digest of what
is written holds every rewrite to the bytes the code wrote before. CodeWalker loads what ragekit writes: drawables,
texture dictionaries and collision with the same summary as the originals, map files with the same summary and the
same CodeWalker XML. These tests do not run the game itself. See [docs/testing.md](docs/testing.md).

## Not done yet

- Writing NG-encrypted archives.
- Writing fragments (.yft), and cloth.
- Typed models for the common PSO files (carcols, carvariations, peds ...).
- GTA V Enhanced (the gen9 formats).
- Animations (.ycd), navmeshes, path nodes and the other formats [docs/codewalker-comparison.md](docs/codewalker-comparison.md)
  lists as gaps.

## AI usage

ragekit is developed with assistance from Claude (Anthropic). The code and the tests are written with Claude's assistance
under the author's direction, and the documentation is written by Claude. The format knowledge comes from CodeWalker's
source (credited below) and from the game's own files, and every change is checked the same way: the round trip over
the game's own files, the test programs, and CodeWalker loading what ragekit writes ([docs/testing.md](docs/testing.md)).

## Credits and licence

MIT (see [LICENSE](LICENSE)). The file structures were first ported from
[CodeWalker](https://github.com/dexyfex/CodeWalker) by dexyfex and Neodymium (MIT), and ragekit's CodeWalker XML is
written to match CodeWalker's; parts are being rewritten independently, and the credit stays in LICENSE while any
derived code remains. The interior audio recipe follows ht_mlotool / gtav-audio-occlusion (MIT). miniz is by Rich
Geldreich and contributors (public domain).

---

[![Buy Me A Coffee](https://img.shields.io/badge/Buy%20Me%20A%20Coffee-ffdd00?style=for-the-badge&logo=buy-me-a-coffee&logoColor=black)](https://buymeacoffee.com/ratprez)
