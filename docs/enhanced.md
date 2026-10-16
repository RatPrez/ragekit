# GTA V Enhanced (gen9)

GTA V Enhanced (the 2025 PC release, DirectX 12, `GTA5_Enhanced.exe`) keeps Legacy's archives, keys and RSC7 container,
and almost all of its file formats: collision, cloth, maps, meta, PSO, RBF, XML data and audio are the same. Four resource
types are laid out differently, and carry versions of their own:

| type | Legacy | Enhanced | what changed |
|---|---|---|---|
| `.ytd` | 13 | 5 | the texture structure (DXGI-ordered format codes, a shader resource view) |
| `.ydr` `.ydd` | 165 | 159 | shaders (a parameter table and constant buffers), vertex and index buffers, vertex declarations, textures |
| `.yft` | 162 | 171 | its drawables, and the vertex declaration of each glass window |

ragekit reads both editions into the same structs and writes either. (`.ypt`, 68 / 71, is not read yet.)

## Choosing the edition

A program chooses its edition once, before it loads anything, the way CodeWalker asks at start up (`game.h`):

```cpp
rk::set_default_game(rk::Game::Enhanced);       // writers and GameIndex::default_game_dir() follow it

Drawable d = read_ydr(file);                     // either edition: the version says which
Bytes out = write_ydr(d);                        // Enhanced, as chosen
Bytes old = write_ydr(d, Game::Legacy);          // or name the edition
GameIndex gi = GameIndex::open(GameIndex::default_game_dir());   // the Enhanced install
```

`rsc::game_of(type, version)` tells a resource's edition; `rsc::version_for(type, game)` gives the version to write.
`GameIndex` opens either install (`info().game` says which; `game_of_dir`), and `default_game_dir(Game)` looks for
each in the Rockstar Games, Steam and Epic folders (or `$RAGEKIT_GAME` / `$RAGEKIT_GAME_ENHANCED`). On the command line:
`rk --enhanced ...` / `rk --legacy ...` (or `RAGEKIT_EDITION=enhanced`), and `rk convert <in> <out> --enhanced`.

## What the structs hold

The structs are Legacy's, and what Enhanced stores beyond them is kept next to them, so an Enhanced file written back as
Enhanced keeps every byte that is not a pointer or a vtable:

- `Texture::gen9` (`TextureGen9`): the format code when it is not implied by `format` (the sRGB variants), dimension,
  tile mode, flags and the byte at 0x23. The pixel data is the same in both editions.
- `Shader::gen9` (`ShaderGen9`): the effect's parameter table (Enhanced names, kind, slot, offset, length), the
  constant buffer sizes and contents, the texture bound to each texture slot, and the sampler states.
  `Shader::params` is the Legacy view of the same shader in both editions: Legacy names, order, register slots.
- `Geometry::layout` is a Legacy vertex declaration; Enhanced's 320-byte declaration (52 semantic slots with an
  offset, stride and DXGI format each) is converted to and from it, and the vertices are reordered (Enhanced keeps the
  components in slot order). `vb_flags` 1024 is Enhanced's bind flags 0x00586409.
- `Fragment::glass_windows` keep Legacy's 112 bytes; `glass_declarations_gen9` keeps the declarations as read.

## Converting

Writing a file in the other edition converts it. Most of it is mechanical; the shaders need a table. Enhanced names its
shader parameters after the second name each variable of the Legacy effect carries (`specularFalloffMult` is
`Specular`, `DiffuseSampler` is `DiffuseTex`), lays them out in constant buffers the Legacy files do not describe, and
adds parameters Legacy does not have. `src/shader_gen9.inc` holds, for each of the 248 shaders of the game's Enhanced
drawables: its Enhanced parameter table, buffer sizes and the values the game's files hold most in every word of the
buffers; its Legacy parameter list as the game's Legacy drawables carry it most (names, order, slots, values); the presets
per draw bucket; and the vertex layouts Legacy uses where they are not the standard one (cloth). `rk_gen_shader_table`
makes it from a Legacy and an Enhanced install (`tools/gen_shader_table.cpp`).

- **Legacy to Enhanced**: each shader gets the table's layout with its Legacy values written in by name; Enhanced-only
  values are the table's. Sampler states number the samplers of the file, one number per sampler name, as in the game's
  files. Textures get Enhanced's format codes and usage flags. Half-float texture coordinates and tangents and signed
  normals (cloth) become floats: Enhanced's files use only float, float2/3/4 and 8-bit colour formats.
- **Enhanced to Legacy**: each shader gets the Legacy parameter list, its values read from the Enhanced buffers by name;
  the preset (`.sps`) and `unk12` come from the bucket (Enhanced stores no preset: a shader drawn in one bucket through
  more than one preset comes back with the one the game's Legacy drawables use most). Cloth geometries get Legacy's
  half-float layout back.

A shader the table does not know can be read (its parameters keep their Enhanced names) and written back in its own
edition, but not converted to Enhanced.

## How it is checked

- `rk_roundtrip --game=<Enhanced install>` reads, writes as Enhanced and reads back the game's own files, and compares
  every Enhanced-only structure byte for byte with the original (shaders, their parameter blocks and tables, texture
  stubs, textures, vertex and index buffers, declarations, buffer views, glass windows): pointers only as null / not
  null, vtables not at all. `--cross` also writes each file in the other edition and compares what both hold.
- `rk_gen9_test <Legacy install> <Enhanced install>` takes every file at the same path in both installs (Rockstar made
  the Enhanced files from the Legacy ones), converts each edition's file to the other and compares it with the game's own.
- The CodeWalker oracle reads ragekit's Enhanced files with CodeWalker's gen9 reader.

What these gave on the game's own files (Legacy 1.0.3889.0 and Enhanced, October 2026):

- Every 50th file of the Enhanced install (6,128 files, 3,000 of them textures, drawables and fragments): every one reads
  back field for field after an Enhanced write, with every Enhanced structure byte for byte the original's, and after a
  write in Legacy too.
- 2,511 files found at the same path in both installs (every 100th), converted each way: Legacy to Enhanced, every one of
  135,984 shader constants that comes from a Legacy parameter holds what Rockstar's Enhanced file holds, and all but one
  of the Enhanced-only ones; usage flags match in 2,831 of 3,029 textures. Enhanced to Legacy, the parameter lists, order
  and register slots of all 82,413 parameters are the game's Legacy files'. What differs beyond that is content Rockstar
  changed between the editions (re-encoded textures, reworked meshes, reflection textures dropped).
- CodeWalker reads ragekit's Enhanced writes of 648 files with the summaries of the originals (every shader parameter,
  vertex, index and texture data hash), and loads all of the 1,277 conversions both ways.
- The Legacy writers are unchanged: every 50th Legacy file (6,149) is written with the bytes it was before.
