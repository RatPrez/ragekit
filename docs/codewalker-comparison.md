# ragekit and CodeWalker: file format coverage

What CodeWalker.Core can do per file type, next to what ragekit does today. "XML" means CodeWalker's XML round trip for
that type (its import and export). Updated 2026-10-16.

Legend: **yes** = read and write; **read** = read only; **-** = nothing yet.

## Containers

| Format | CodeWalker | ragekit | Gap |
|---|---|---|---|
| RSC7 resource container | read, write (ResourceBuilder, page layout) | **yes** (own page layout, following the game's own files; deflate via miniz) | none |
| RPF7 archive | read, write (create, add/replace/delete/rename entries, defragment, encrypt AES/NG) | **yes** (read AES/NG/OPEN/none, nested archives, resources as RSC7 files; write none/OPEN/AES/NG, nested archives, lazy copies out of any archive; Legacy and Enhanced) | none |
| RPF2 (FiveM cache `resource.rpf`) | - | - | neither reads these |
| Encryption keys | from the exe and the embedded `magic.dat` | from the exe (GTA5.exe or GTA5_Enhanced.exe: the same keys) and `magic.dat` (`rk rpf keys`), kept in `~/.ragekit/keys`; NG encryption derived from the decrypt tables | none |
| GTA V Enhanced (gen9) .ytd .ydr .ydd .yft .ypt | read; write by converting Legacy files (`Gen9Converter`, a shader table from one sample per shader) | **yes**: read, write, Enhanced files written back keep every non-pointer byte; conversion both ways, from a table of every shader of the game's drawables (Legacy parameter lists, Enhanced layouts and values, presets, cloth layouts) | none |

## Resources (RSC7)

| Format | CodeWalker | ragekit | Gap |
|---|---|---|---|
| .ytd texture dictionary | yes + XML/DDS | **yes** + DDS/PNG; decodes every DXT/BC/uncompressed format; encodes DXT1/5, BC4/5 and uncompressed; mips; **XML export** | BC7/BC6H encode; XML import |
| .ydr drawable | yes + XML | **yes** (shaders and parameters, textures, skeleton, joints, LODs, lights, embedded bounds); **XML export**; the shader database names every parameter | XML import |
| .ydd drawable dictionary | yes + XML | **yes**; XML export | XML import |
| .yft fragment | yes + XML | **yes** (drawables, physics LODs with groups, children, archetypes, bounds and joints, glass, vehicle glass, lights, environment cloth; every tenth game file round-trips field for field) | XML |
| .ybn collision | yes + XML | **yes** (every bound type, BVH read and build, geometry builder, materials with room id); XML export | XML import; ragekit builds its own BVH trees |
| .ycd clip dictionary (animations) | yes + XML | - | everything: clips, animations, channel packing, sequences |
| .ynv navmesh | yes + XML + builder | **yes** (every stored value kept, so the game's cells come back exactly) | a builder; XML |
| .ynd path nodes (traffic) | yes + XML + builder | - | everything |
| .yed expression dictionary | yes + XML | - | everything |
| .yld cloth dictionary | yes + XML | **yes** (character cloth: controller, bridge, verlet cloth, skinning, capsule bounds); all 61 game files round-trip, every structure byte the same but vtables | XML |
| .yfd frame filter dictionary | yes + XML | - | everything |
| .ypdb pose matcher | yes + XML | PSO container only | typed model |
| .ypt particle effects | read (+ partial XML) | **yes** (textures, drawables; every rule, behaviour, domain, keyframe, evolution and shader variable kept as read and written back; both editions; all 2,784 game files round-trip) | typed fields beyond names; XML |
| .yvr vehicle record | yes + XML | - | everything |
| .ywr waypoint record | yes + XML | - | everything |
| .ysc script | - (no decompiler) | - | |

## Meta data (`.ytyp` / `.ymap` / `.ymt` / `.ymf` and the data files)

| Format | CodeWalker | ragekit | Gap |
|---|---|---|---|
| Meta container (RSC7 `PRD0`) | yes + XML (MetaXml/XmlMeta, drops some lists) | **yes**, a lossless generic layer (`meta.h`) | XML |
| .ytyp map types | yes + XML (loses container LODs etc.) | **yes** typed (base/time/MLO archetypes, rooms, portals, entity sets, extensions) + generic | XML |
| .ymap map data | yes + XML | **yes** typed (entities, MLO instances, car generators, box occluders, timecycle modifiers; occluder meshes, LOD lights, instanced grass/props, container LODs kept as they are) + decoded occluders (`occluder.h`) | typed LOD lights and grass; XML |
| PSO (`.ymt`, `.ypdb`, `.ymf`, many `.meta` binaries) | yes + XML (PsoBuilder) | **yes** as a container (byte for byte), a schema-driven walker and dump (`pso.h`), typed `_manifest.ymf` (`manifest.h`) | XML -> PSO; typed models for carcols, carvariations, peds ... |
| RBF (creature metadata `.ymt`, cutscene / Bink metadata) | yes + XML | **yes**, byte for byte on all 200 game files (`rbf.h`) | XML |
| XML `.meta` text files (vehicles.meta, carcols.meta, handling, content.xml, setup2.xml) | parsed into typed classes | **yes**: a DOM reader and writer (`xml_dom.h`); all 2,857 game files read back the same | typed models |
| .gxt2 text tables | yes | - | everything |
| Cache dat (`gta5_cache_y.dat`) | read | read: the bounds store and interior proxies (`world.h`) | the rest |
| Heightmap / watermap / distant lights | read, write | - | everything |
| Audio: .awc wave containers | yes + XML | - | everything |
| Audio: .rel (dat54 sounds, dat151 game, dat4 speech/audioconfig, dat10 synths, dat15 mix, dat16 curves, dat22 categories, dat149/150) | yes + XML (a typed class per item type) | **container: yes** (`rel.h`: byte for byte on 378 game and server files; index, name, hash and pack tables; a builder with the game's alignment and index order); **typed: dat151 InteriorSettings, InteriorRoom, StaticEmitterList, DoorAudioSettingsLink** | typed structs for the other ~150 item types; XML |
| Audio occlusion `.ymt` (naOcclusionInteriorMetadata) | read as a generic PSO; no builder | **yes** (`audio_occlusion.h`: typed read/write of portal infos and path nodes, paths from rooms and portals, proxy hash) | the game's order of the path node list is not reproduced |
| .fxc compiled shaders | read + XML | **read**: constant buffers, variables with types, registers, defaults and annotations, techniques (`fxc.h`) | the bytecode |
| .mrf move networks | read + XML | - | everything |
| .cut cutscenes | read + XML | - | everything |

## Beyond file formats

| Capability | CodeWalker | ragekit |
|---|---|---|
| Game data model (archetype lookup, DLC load order, content.xml change sets) | GameFileCache | `gameindex.h` (files and archetypes by name, load order, texture parents, cached) and `world.h` (the mounted map, entities and collision at a position) |
| Jenkins hash and name registry | yes, with large name lists | `joaat`, `hash_or_name`, names from the install's own file names |
| Shader parameter names per shader | ShaderParams.cs tables | `shader_db.h`, read from the install's `.fxc` / `.sps` files |
| Model export | OBJ, XML; the viewer renders | glTF 2.0 `.glb` with textures (write only) |
| Image codecs | DDSIO (DDS only) | DDS and PNG, decode and encode, mip generation |
| Editing UI, 3D world viewer, project system | yes (the CodeWalker app) | out of scope: a library for tools to build on |
| Platform | .NET on Windows | any C++20 compiler; tested with MSYS2 GCC on Windows |

## Next for ragekit

1. Typed models for the common PSO files (carcols, carvariations, peds ...).
2. Animations (`.ycd`), and the smaller formats people ask for (`.gxt2`, `.awc`).
