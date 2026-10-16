# Getting started

## Building

Any C++20 compiler and CMake 3.20 or newer. The two dependencies are vendored: miniz (deflate, public domain) in
`third_party/miniz`, Clipper2 (polygon clipping for the navmesh builder, Boost licence) in `third_party/clipper2`.

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

On Windows the author builds with MSYS2's UCRT64 GCC (`build.cmd` sets the path and runs the two commands above).

| CMake option | default | builds |
|---|---|---|
| `RAGEKIT_BUILD_CLI` | ON when ragekit is the top-level project | `rk`, the command line tool ([rk-tool.md](rk-tool.md)) |
| `RAGEKIT_BUILD_TESTS` | ON when ragekit is the top-level project | the `rk_*` test programs ([testing.md](testing.md)) |
| `RAGEKIT_BUILD_EXAMPLES` | ON when ragekit is the top-level project | the programs in `examples/` |

## Using it from your project

### Fetched by CMake

CMake downloads ragekit from GitHub at configure time and builds it with your project; you need CMake 3.20 or newer and git:

```cmake
include(FetchContent)
FetchContent_Declare(
    ragekit
    GIT_REPOSITORY https://github.com/RatPrez/ragekit.git
    GIT_TAG        v0.2.0        # a release tag (or a commit hash); main follows the latest
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(ragekit)

target_link_libraries(my_tool PRIVATE ragekit::ragekit)
```

- `GIT_TAG` picks the version. A release tag (`v0.2.0`) or a commit hash keeps your build reproducible; `main` gets
  whatever was pushed last. `GIT_SHALLOW` only works with a tag or a branch, so drop it when you pin a commit hash.
- Inside your project only the library builds; the `rk` tool, the tests and the examples stay off unless you set
  `RAGEKIT_BUILD_CLI`, `RAGEKIT_BUILD_TESTS` or `RAGEKIT_BUILD_EXAMPLES` to ON before `FetchContent_MakeAvailable`.
- Linking `ragekit::ragekit` adds its include directory and C++20 to your target.
- To build against a local checkout instead (offline, or while changing ragekit), configure with
  `-DFETCHCONTENT_SOURCE_DIR_RAGEKIT=path/to/ragekit`: CMake then uses that folder and downloads nothing.

### A copy in your tree

```cmake
add_subdirectory(third_party/ragekit)          # a git submodule, or a copy of the repository
target_link_libraries(my_tool PRIVATE ragekit::ragekit)
```

### Then



```cpp
#include <ragekit/ragekit.h>     // every header; or include just the ones you use
using namespace rk;

int main() {
    Drawable d = read_ydr(rsc::read_file("prop.ydr"));
    d.name = "prop_copy";
    rsc::write_file("prop_copy.ydr", write_ydr(d));
}
```

## Conventions

- **Bytes in, bytes out.** Readers take a `std::span<const uint8_t>` of a whole file and writers return `rk::Bytes`
  (`std::vector<uint8_t>`); `rsc::read_file` / `rsc::write_file` are small helpers. Only those helpers, archives
  (`rpf.h`), the game and world indexes and the shader database (with their caches) touch the file system.
- **Errors throw `rk::Error`** (a `std::runtime_error`) with a message that says what was wrong and where.
- **Plain structs.** Every format is a struct you can build, read and change. Fields the game stores but the library
  does not model are kept (`raw`, `unk*` members), so read -> write keeps them.
- **Hashes.** Most game names are hashed with Jenkins one-at-a-time over the lower-cased name: `joaat_lower("prop_x")`.
  A few (PSO field and structure names) are case-sensitive: `joaat`. `hash_or_name` parses `hash_XXXXXXXX`, `0x...` or
  a name.
- **Coordinates** are the game's: X east, Y north, Z up, in metres. Matrices are row-vector (translation in the last
  row), as the game stores them.

## Retail archives need keys

Archives with no encryption or `OPEN` encryption (most mod `dlc.rpf` files) open as they are. The game's own archives
are AES or NG encrypted, and the keys come from your copy of the game. They are never part of this repository. See
[archives.md](archives.md#keys) for how `rk rpf keys` creates them in `%USERPROFILE%\.ragekit\keys`.

## Examples

`examples/` has small, complete programs; each one starts with a comment that says what it does and how to run it.

| program | shows |
|---|---|
| `ydr_info` | reading a drawable: LODs, models, geometries, shaders, embedded textures |
| `ytd_export` | a texture dictionary to `.dds` and `.png` files |
| `to_gltf` | a drawable to a binary glTF with its textures |
| `box_collision` | a collision file (`.ybn`) built from a triangle mesh |
| `make_interior` | an interior (`.ytyp`: MLO archetype, rooms, portal, entity) and the `.ymap` that places it |
| `rpf_list` | listing an archive, nested archives included, and extracting a file |
| `game_lookup` | finding an archetype in a GTA V install and pulling its model and textures |
