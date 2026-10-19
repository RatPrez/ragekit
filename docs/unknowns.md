# Fields ragekit does not name, and what the game's files say about them

The structs keep fields whose meaning is not known as `unk<offset>` members, written back as read. `rk_unknowns`
(`tests/unknowns.cpp`, [testing.md](testing.md)) measures them over an install: per field the values it takes, how often,
and what they go with. A field the statistics explain is named (the old `unk` name stays as an alias where code may
use it: `union { float rest_length2; float unk4; }`); the others are documented here with what was measured. Counts
are from the Legacy install (b3889) unless an edition is named; `--every=N` samples are said so.

## Cloth (`cloth.h`; `rk_unknowns --every=3 cloth`: 628 environment cloths in 20,477 fragments, 21 character cloths)

| field | values | conclusion |
|---|---|---|
| `ClothEdge::unk4` | the squared distance of the edge's two vertices on all 213,846 edges (0 on the 18,042 padding slots, vertices 0 - 0) | **`rest_length2`** |
| `ClothEdge::unk8` | 0 on every edge whose vertex0 is pinned (index < the verlet's `unkE8`), 1 where only vertex1 is, 0.5 where neither is | **`weight0`**: vertex0's share of a correction |
| `ClothEdge::unkC` | 0.25 on every edge, 0 on padding | unknown, constant |
| `VerletCloth::unkE8` | at most the vertex count; the edges' weights put the pinned vertices first | **`pinned_count`** |
| `VerletCloth::unk148` | ceil(vertices / 32) on all 749 | **`vertex_words`** (a bit per vertex) |
| `VerletCloth::unkA8` | the archetype's lodDist on 589 of 676 fragments (the rest 40 to 200 under it) | **`lod_distance`** |
| `VerletCloth::unk3C`, `unk4C` | 0x7F7FFFFF / 0xFF7FFFFF (FLT_MAX / -FLT_MAX) in fragments, 0 in .yld | the W lanes of the box |
| `VerletCloth::unkAC` | 0 in every fragment; 9999, 86, 48, 60, 216 in .yld | unknown |
| `VerletCloth::unkFA` | 0, or 2 on 108 | unknown |
| `VerletCloth::unk158` | 1 on 623, then 2, 1.5, 1.2, 0.35, 5 | unknown |
| `EnvironmentCloth::unk78` | 0 without a tuning, 16 with one, 48 when the tuning's flags hold 8 (6 cloths) | **`flags`** |
| `EnvironmentCloth::unk60` | empty but on one cloth (554 words) | unknown |
| `ClothController::unk78` | 1 on character cloth, 0 on environment cloth | kind-constant |
| `CharacterClothController::unkA0`, `unkDC` | 0.04 and 1 on all 21 | constant |
| `CharacterClothController::unkB0` | as many as `bone_ids`, values 1 .. 234 | unknown (bone indices?) |
| `CharacterCloth::unk10`, `unk90` | `unk10` empty but on two cloths (318, 2,340 Vec4), `unk90` always empty | unknown |
| `ClothBridge::unk20` | 0 .. 1; on character cloth 0 on 598 of 605 pinned vertices, spread over 0 .. 1 on free ones; on environment cloth 1 or 0 mostly, pinned or not | a per-vertex weight; unknown |
| `ClothBridge::unk60` | small positive values, many squares of round numbers (0.0004 = 0.02^2, 0.002025 = 0.045^2, 0.0001, 0.000225 = 0.015^2) but also 0.002, 0.004; pinned or not | unknown (a squared distance per vertex?) |
| `ClothBridge::unkA0` | 0 on every character cloth vertex; on environment cloth 0 on 88 %, 1, 0.2, 0.5, 0.8 on the rest | unknown |
| `ClothTuning` | `unk10` pi and `unk14` pi / 6 on 79 of 80 (4.7 and 0.52 on one); `unk20 .. unk2C` 0 on 77..79; `unk34` -1 on 67; `unk38` 0 on 59, small on the rest; `unk3C` 0 but once | unknown |

Character cloth vertices carry W = 0x7FC00001 (a NaN) on 728 of the 749 verlet meshes.

## Meta and PSO (`meta.h`, `pso.h`; `rk_unknowns --every=4 metaheader pso`: 6,114 meta files, 2,334 PSO files)

- `MetaStruct::unknown8` is the structure's alignment: 1024, 768, 512, 256, 0 = log2 4, 3, 2, 1, 0 shifted by 8, and on
  38,377 of 39,225 descriptions it is the alignment the widest field needs (16 for vectors, 8 for arrays and pointers, 4,
  2, 1 for plain values; the other 848 are structures this simple rule reads too strictly). `MetaStruct::alignment()`
  gives it in bytes.
- `MetaStruct::unknownC`, `unknown1C`, `MetaEnum::unknown14` and the header's `unknown17`, `unknown18`, `unknown4e` and
  `unknown50[8]` are 0 in every file, the header version 0x79.
- `MetaEntry::unknown9` is 0 on every entry but arrays (0 on 97,854, 4 on 9,690, 0x24 on 119) and data block pointers
  (4 on all 82): flags of the array kinds, unknown.
- `PsoStructInfo::unknown` is 0 but on the two pseudo-structures 0x100 and 0x101 (2); `PsoStructInfo::unknown_c` and
  `PsoBlock::unknown` are 0 everywhere; `PsoDataMap::unknown` is 0x7070 in every file and no file uses the extended map
  header.

## Bounds (`bounds.h`; `rk_unknowns --every=20 bounds`: 757 .ybn, 3,072 .yft)

- `Bound::unk3C` (phBound::m_RefCount) is 1 on every .ybn bound and 2 on every fragment bound.
- The bytes 0x58..0x5F are the bound's own material id in a BoundMaterial's layout (`Bound::material()`): `unk_flags`
  and `poly_flags` are its flag bytes, 0 on every .ybn bound and 0x02, 0x10, 0x12, 0x40, 0x54 ... on 681 of 8,291
  fragment parts. `unk5E` is 0 everywhere.
- `Geometry::unkAC` is 0.0025 on every .ybn GeometryBVH and 0.02 on every fragment Geometry, whatever their margins (0.005,
  0.025, 0.04); `unk9C` is `unkAC / 32767` on all of them. The W lanes of the centre and the quantum; constant per kind.
- `Geometry::unk80` is 0, `unk82` 1 exactly when the geometry has vertex colours; `unk140` 0xFFFF on every BVH.
- `unk70` is a capsule's half height (as before) and 0 on every other type.

## Drawables and textures (`--every=20 drawable`, `texture` in both editions)

- `Shader::unk12` is 0x8000 on 15,627 of 15,769 shaders and 0 on 142.
- `Skeleton::unk1C` is 0 on all 480 skeletons of one bone and 0x01000000 on all 174 of more: a flag of the bone tag map.
- `Light::unk45`, `unk46` are 0 on all 2,862 lights of the sampled drawables.
- `Drawable::unk3C`, `unk4C` are 0x7F800001 on every drawable.
- `Texture::unk32`: Legacy's conversion flags 0x80 on 93 %, 0x30, 0x20, 0x40, 0x38, 0x28, 0x90 on the rest, the layer
  count byte always 0; Enhanced's 0x80 on every texture.
- `TextureGen9::unk23` (Enhanced) is 0x28 on half the textures and 0 on the other half, the same within a dictionary on
  4,408 of 4,491 (0x28 mostly in the base archives, 0 in the DLC packs), and not tied to the format, the mips or the
  usage: an attribute of the build that converted the file, unknown.

Doubled texture names: 5,976 of 124,287 texture parameters (every 7th .ydr) name a texture like
`PO1_RBM_fencePO1_RBM_fence_A` or `bj_weeds01bj_weeds01_a`. The file holds that one string (no NUL between the halves:
`po1_03_brig3_csup01.ydr`'s stub at 0x86EF0 points at it) and the dictionary the drawable uses holds a texture of that
name (`po1_03_bridge.ytd`: `po1_rbm_fencepo1_rbm_fence_a`, 256 x 256 DXT5): the names are the game's own, made by its
exporter from a colour image and its alpha image, and ragekit reads them right.

## Fragments (`--every=20 fragment`: 3,072 .yft)

- fragType 0xB0 is 0 but on 3 (944), 0xB8 0 but on 9 (multiples of 16 up to 7,136), 0xBC 0, 0xCC 0; 0xC0 0xFF00 on
  3,010, 0, 0x100 or 0x300 on the rest; 0xC4 1 on 2,916, 0x43 on 126, 0x41 on 24, 3 on 6. Unknown.
- The bone transforms' u16 (`bone_transforms_unk`) is 0 on 2,885 and 1 on 187.
- `FragPhysicsLod::unknown_data1` / `unknown_data2` are empty in every sampled file.
- `FragChild::unk_float` is 0 on 6,442 children, 100 on 596, FLT_MAX on 374, then 400, 210, 500, 2000 ...: unknown (a
  strength?); `unk_vec.w` 0 on 7,877.

## The new formats

`docs/other-formats.md` gives the measured fields of the waypoint recordings (heading, speed), path nodes (the unused
words), clip dictionaries (a track's value type, the animation flags and usage count) and pose matchers.

## Entity ambient occlusion

`Entity::ambient_occlusion_multiplier` and `artificial_ambient_occlusion` (CEntityDef `ambientOcclusionMultiplier`,
`artificialAmbientOcclusion`) are named and read; what they do in the renderer is not known (most MLO entities keep
255 / 255).
