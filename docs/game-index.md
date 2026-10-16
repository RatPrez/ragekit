# The game index and the world index

## Game asset index (`gameindex.h`)

`GameIndex` indexes a GTA V install, Legacy or Enhanced (`info().game`; enhanced.md), so a tool can find and pull game
assets by name. `GameIndex::open(game_dir)`
scans every archive (`common.rpf`, `x64*.rpf`, `x64/audio/**.rpf`, `update/update.rpf`, `update2.rpf`, every
`dlcpacks/*/dlc*.rpf` and every archive nested in them: 179 on disk and 5,199 nested on the 1.0.3889 install) and keeps:

- every file entry (name hash, type, archive, entry, directory, size);
- every archetype of every `.ytyp`: name, kind (base, time, MLO), asset type and name, texture / drawable / physics /
  clip dictionary, lodDist, bounding box, flags and the ytyp it comes from;
- the texture parents of `gtxd.ymt` / `gtxd.meta`, and a hash -> name table made of the file names.

The first scan takes about half a second with a warm disk cache (8 threads, header reads only); the cache loads in
0.07 s.

```cpp
auto gi = GameIndex::open(GameIndex::default_game_dir());     // the cache, or a scan
auto hits = gi.find_archetypes("door", 20);
if (auto a = gi.archetype("prop_cs_cardbox_01")) {
    ResolvedDrawable d = gi.resolve_drawable(*a);           // the .ydr / .yft / .ydd that holds it
    auto txd = gi.resolve_txd(a->texture_dictionary);
    Bytes ydr = gi.extract(*d.file);                         // a standalone RSC7 file, nested archives included
    for (uint32_t p : gi.txd_parents(a->texture_dictionary)) { /* the gtxd chain, nearest first */ }
}
std::string n = gi.name(hash);                               // "hash_XXXXXXXX" when no file name has it
```

**Load order**: base archives < `update.rpf` < `update2.rpf` < the packs named in `update.rpf/common/data/dlclist.xml`,
sorted by the `<order>` in their `setup2.xml` (ties keep the dlclist position); `update.rpf/dlc_patch/<pack>` ranks
just above the pack it patches. A pack the list does not name never wins a lookup. Lookups (`resolve_file`,
`archetype`) return the highest rank, and at equal rank the file scanned last; `versions()` and `archetype_versions()`
give every copy.

**Cache**: `%USERPROFILE%\.ragekit\index\<key>.bin`, format version `GameIndex::kFormat` (the layout is documented at
the top of `src/gameindex.cpp`). The key hashes the format version, the folder, the executable's version, size and
time, and the name, size and time of every archive, so a game update or another folder gets a new file (old ones are
never deleted). About 38 MB for the Legacy install.

**Limits**: `vehicles.meta` texture relationships are not read (only the gtxd files); archetypes come from every ytyp
of the install, not only those a DLC change set requests; the six PSO-format `.ytyp` files are listed as warnings and
not indexed (CodeWalker cannot read them either).

```
rk index build [<game dir>] [--game DIR]      scan and write the cache (always rescans)
rk index info  [<game dir>] [-v]              counts, load order, warnings
rk index versions <name|0xhash> [.ext]        every copy of a file name, lowest load order first
rk find <substring> [--type archetype|ydr|ytd|yft|ybn|ytyp|ydd] [--limit N]
rk pull <archetype or file name> <out dir>
```

The game folder is the argument, `--game`, `$RAGEKIT_GAME`, or the usual Rockstar Games folder. `rk pull` of an
archetype writes its model (the `.ydr`, `.yft` or `.ydd` that holds it), its texture dictionary and the dictionaries it
falls back on, the physics `.ybn` when one has the dictionary's name, the archetype as CodeWalker-style XML and as a
one-archetype `.ytyp`, and reports textures its shaders name that neither the model nor those dictionaries hold (the
game takes those from its global dictionaries).

## World index (`world.h`)

`WorldIndex` answers "what stands here" on the map the game loads. `WorldIndex::open(gi)` works out which archives are
mounted (the base map archives, minus those the mounted packs' content change sets invalidate, plus the ones they
enable), takes the newest copy of every name among them, and reads every ymap's header (extents, flags, entity count,
MLO instances), the IPL groups of every `_manifest.ymf` (ymaps a script must request) and the game's own bounds store
(`*_cache_y.dat`: the box of every static collision file). 5,429 ymaps, 763 interior instances and 10,689 collision
files on the Legacy install, built in about 2 s and cached.

```cpp
WorldIndex w = WorldIndex::open(gi);
for (const WorldEntity& e : w.entities_at({-806, 175, 76}))   // Michael's house: bh1_48_michaels, v_michael ...
    std::printf("%s %s\n", gi.name(e.def.archetype_name).c_str(), e.def.is_mlo_instance ? "(interior)" : "");
for (uint32_t b : w.bounds_in(mn, mx)) { const WorldBounds& wb = w.bounds()[b]; Bytes ybn = gi.extract(wb.file); }
```

Queries load the ymaps whose extents reach the area (a small cache keeps the last 96). `WorldEntity::orientation` is
the world orientation.

```
rk world info                                       counts, change sets, warnings
rk world at <x> <y> <z> [--all-lods] [--groups]     entities whose box holds the point, smallest first
rk world near <x> <y> <z> <r>                       entities and collision files within r
rk world mlos [name]                                interior instances with their position and yaw
rk world occluders <x> <y> <z> <r>                  the ymaps whose occluders reach the area
```
