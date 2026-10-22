# Rendering: ragekit::render

`ragekit::render` is everything a renderer needs to draw the game's drawables with the game's own shading, with no
graphics API in it: materials read from the shaders, meshes and textures laid out for upload, the scene light from the
game's timecycle, the drawables' lights, and particle effects played. A backend uploads what it gives and draws it.

It is a second target, `ragekit::render`, built when `RAGEKIT_BUILD_RENDER` is on (the default when ragekit is the
top-level project):

```cmake
set(RAGEKIT_BUILD_RENDER ON)
add_subdirectory(ragekit)
target_link_libraries(my_viewer PRIVATE ragekit::render)
```

```cpp
#include <ragekit/render/render.h>
using namespace rk;
```

Game space throughout: Z up, metres, row-vector matrices as the game stores them.

## Models

`render::build_model` turns a drawable (or several, a fragment, or a ped's components) at one LOD into a `Model`:

- `meshes`: one per geometry, an indexed triangle list with its vertex attributes described in terms any API has
  (`Attrib`, `AttribFormat`, offsets and stride), its material, its placement inside the model (fragment parts) and its
  bounds. A proxy model or a shadow-only material is kept with `shadow_only` set; `render_mask` says whether it casts a
  shadow.
- `textures`: every texture a material samples that was found, as stored (`PixelFormat`: BC1-BC7, RGBA8, BGRA8, A8,
  L8 ...), every mip level. Texels are what the game samples: no sRGB decode, the shaders do the gamma.
- `missing`: the texture names no dictionary had.
- `lights`: the drawable's light attributes placed on their bones.

The vertex and texel spans point into `Model::sources`, which keeps the drawables and dictionaries alive.

Textures are looked up in the drawable's own dictionary first, then in the `TextureDictionaries` passed, nearest first.
With a game install, `render::load_archetype(gi, "prop_chair_01a")` finds the drawable and its dictionaries the way the
game does (the archetype's dictionary, then its gtxd parents; vehshare for vehicles; variation 000 of each ped component)
and says which files it used.

```cpp
GameIndex gi = GameIndex::open(game_dir);
auto am = render::load_archetype(gi, "v_res_tre_sofa_s");
for (const render::Mesh& m : am->model.meshes) {
    const render::Material& mat = am->model.materials[m.material];
    // upload m.vertices / m.indices, bind mat's textures by role, draw with mat.state and mat.constants
}
```

## Materials

The game's drawable shaders share one lighting model; what differs is where each input comes from. `make_material`
reads a shader's name and parameters into a `Material` an uber-shader consumes:

- the family (megashader, vehicle, weapon, ped, hair, terrain) and the render bucket (opaque, alpha, decal, cutout ...);
- feature switches and the sources of specular intensity, gloss and fresnel, the vehicles' and weapons' sheen, emissive
  masks and alpha;
- the fixed-function state (`PipelineState`: blending, depth writes, culling, the decals' depth offset, alpha test);
- the constants as a std140 block (`MaterialConstants`, 224 bytes);
- the texture each role samples (`TextureRole`: diffuse, normal, specular, ...), by name and hash.

`DrawParams` carries what the engine sets per entity and the files do not hold: tint and palette rows, vehicle paint,
dirt, window tint, and the ped hair colours (`set_hair`). `shader_name(hash)` names a shader hash.

The formulas come from the game's Legacy shaders; Enhanced drawables use the same materials (`Shader::params` is the
Legacy view of either edition).

## The scene light

An `Environment` is the light a scene is drawn in, as the game's lighting shaders take it: the sun (or the moon),
natural (sky) and artificial ambient, directional ambient, reflections, the sky, fog, exposure and the filmic tone map.
`Environment::preset("day")` (also overcast, evening, night, interior, studio) needs no game files.

`render::Timecycle::load(gi)` reads the game's timecycle: the weather files, the keyframe hours and visualsettings.dat.

```cpp
render::Timecycle tc = render::Timecycle::load(gi);
render::Environment env = tc.environment("EXTRASUNNY", 13.5f, "GLOBAL", "mpaparthigh", 1.0f);
```

`environment` blends the keyframes around the hour and applies a timecycle modifier (an interior room's timecycle) at a
strength, or several in turn. `value` reads one variable, `modifiers` and `modifier_values` list what the mods files hold,
and `setting` reads visualsettings.dat. `render_exterior` follows the modifier, so a room that hides the outside says so.

Some constants sit between the files and the shaders in the game's own code and are reconstructed. They are gathered in
`Environment::Tuning` at the values a backend uses by default; a backend draws with `tuned()` and applies the lights'
part per light (`tuned_light`). `ambient_light`, `exposure_ev` and `adapt_ev` are the shaders' arithmetic on the CPU, for
checking a backend's picture.

## Lights

`render::Light` is a drawable's light attribute where it shines: point, spot or capsule, colour, intensity, range,
falloff, cone, cull plane, its time flags and flashiness. `make_light` places an attribute on its bone, `transformed`
moves it with an entity, `on_at(light, hour)` says whether it is on, and `flash` gives its brightness over time.
`light_constants` lays one out as the game's light shaders take it, with the timecycle's falloff and intensity
multipliers; `attenuation` is the shaders' falloff on the CPU.

## Particle effects

`render::EffectLibrary` decodes the rules of one or more `.ypt` dictionaries (effects, emitters, particle rules, their
domains, behaviours and keyframes) added to it; `EffectLibrary::load(gi, "core")` finds the dictionary the game uses at
the high particle quality. `EffectInstance` plays one effect: `update(dt)` spawns and moves its particles, `batches()` gives what
to draw this frame (`Particle`: position, size, rotation, colour, frame of the texture's grid), and `lights()` the lights
its particles give. `set_transform`, `set_scale` and `set_ground` place it, scale it and give its particles a floor to
bounce on.

```cpp
render::EffectLibrary lib;
lib.add(render::EffectLibrary::load(gi, "core"));
render::EffectInstance fire(lib.effect("ent_amb_fire_ring"), lib);
fire.set_transform(placement);                     // row vectors, as the game's
fire.update(dt);                                   // each frame
for (const render::ParticleBatch* b : fire.all_batches()) { /* draw b->particles with b->rule */ }
```

The files hold the rules; how the game plays them is code in the game's executable, so the simulation is a reconstruction
from the field names, the game's data and its sprite shaders.

## Backends

`ragekit::render` stops at the data a GPU takes. An OpenGL 3.3 backend built on it draws the Rendered view of
[Modlo Creator](https://modlocreator.app); it is not part of this repository.
