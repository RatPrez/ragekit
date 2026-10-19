# Text tables, records, path nodes, animations and audio containers

The smaller formats of the game, each a header of its own. Every one is read and written back over all of the game's
files of both editions by `rk_roundtrip --game=legacy|enhanced --only=<ext>` ([testing.md](testing.md)); `rk info <file>`
lists one and `rk roundtrip <in> <out>` rewrites it.

## Text tables (`gxt2.h`)

`.gxt2` files hold the game's strings: labels, subtitles, help texts, one table per language and text block
(`american_rel.rpf/<block>.gxt2`). A `TextTable` is a list of entries, the joaat of the label and its UTF-8 text.

```
"2TXG"  count:u32  (hash:u32 offset:u32) x count  "2TXG"  end:u32   strings, NUL terminated, in the order of the entries
```

`end` is the file's size. The game's tables keep their entries sorted by hash and lay the strings out in the entries'
order, each once, so `write_gxt2` writes exactly that and every one of the 19,477 Legacy and 19,618 Enhanced files comes
back byte for byte. `TextTable::set` adds a label in its sorted place.

```cpp
TextTable t = read_gxt2(file);
t.set(joaat_lower("MY_LABEL"), "Hello");
rsc::write_file("my.gxt2", write_gxt2(t));
```

## Vehicle and waypoint recordings (`records.h`)

`.yvr` files (`carrec.rpf`, 7,533 in Legacy) are the recorded drives scripted vehicles play back; `.ywr` files
(`waypointrec.rpf`, 1,612) the routes scripted peds and vehicles follow. Each is a small resource: a root with the page
map and one list, and an array of fixed-size entries, which ragekit reads into a vector of plain structs.

| structure | size | layout |
|---|---|---|
| vehicle recording root | 0x20 | 0x10 entries, 0x18 count:u16, 0x1A capacity:u16 |
| `VehicleRecordEntry` | 0x20 | time (ms), velocity x3 (i16, m/s x 273.0583), right and forward vectors (i8 x 127), steering (x 20), gas, brake, handbrake, position |
| waypoint recording root | 0x30 | 0x18 entries, 0x20 count:u32, the rest 0 |
| `WaypointRecordEntry` | 0x14 | position, flags:u16, heading:u8, speed:u8, unk2:u16, unk3:u16 |

The waypoint fields were named from `rk_unknowns ywr` over the 106,096 points of the Legacy files:

- `heading` is the direction to the next point in 256ths of a turn, 0 along +y, counter-clockwise: against the direction
  computed from the positions it is within 2 steps on 61 % of the points (most often 1 step under it), while the
  other convention (clockwise) spreads evenly.
- `speed` is 64, 128 or 192 on routes whose points lie about 2 m apart (15,585 / 10,604 / 4,101 points: the pedestrian
  routes, GUESS walk / run / sprint as a move blend ratio x 64); the other values sit on routes with points 5 to 30 m
  apart (vehicles) and grow with the step.
- `flags`: 0x80 on 55 %, 0 on 45 %, 0x01 0x02 0x20 0x40 on a few dozen; meaning not known.
- `unk2` is 0 on 84 % and otherwise mostly two equal bytes (0x5F5F, 0x4F4F, 0x7F7F, 0xFFFF): GUESS the free space to
  the left and right of the route. `unk3` is 0 on all but 12 points.

Both come back byte for byte but for the root's vtable, which the game's files carry in several values by exporter
build (Legacy `x64d.rpf` 0x40571218, its `update.rpf` copies 0x40573228, Enhanced 0x4068E108; ragekit writes
0x40573228) and the page map's page counts (one 16 KB page or two of 8 KB hold the same): all 9,145 Legacy and 9,142
Enhanced files.

## Path nodes (`paths.h`)

`.ynd` files (`paths.rpf/nodes<n>.ynd`, n = y x 32 + x of a 512 m cell; 1,027 in Legacy with the DLC copies) are the
road and pavement graph. `PathNodes` holds the cell's nodes (vehicle nodes first, then ped nodes, each 40 bytes:
position in quarter metres and z in 1/32 m, street name hash, the first of its links and their count, five flag bytes),
the links (8 bytes: the node they lead to by area and node id, three flag bytes, the length in metres), the junctions
(12 bytes: a box and the offset and size of a height map) with the height map bytes, and the junction references that
tie a junction to its node. The arrays are kept as the plain structs they are on disk.

The root (0x70 bytes) holds the five arrays and their counts; its other words are 0, but 0x48 is 1 and the junction
reference count is stored twice. `rk_unknowns ynd` over all 1,027 files: the 16 bytes at the start of every node, the
u16 at node + 0x14 and at junction reference + 6 are 0 on every one of the 309,416 nodes and 12,273 references (named
`unused`). Every file reads back with every array and root value the same, and the root's bytes but its pointers the
original's; the arrays are placed by ragekit's page layout, which may put them in another order than the game's files
(the game writes nodes, links, height maps, junctions, references in that order).

## Clip dictionaries (`clip.h`, `graph.h`)

`.ycd` files (24,844 in Legacy, 25,961 in Enhanced; version 46 in both) hold the game's animations and the clips that
play them: a crClipDictionary with a hash map of animations (crAnimation: frames, duration, the tracks it drives and
their compressed sequences) and a hash map of clips (crClip: the `pack:/name.clip` path, the animation or list of
animations it plays with start and end times and a rate, its properties and tags).

ragekit does not model the channel compression; it keeps every structure as it was read, as a graph of `RscBlock`s
(`graph.h`): each structure's bytes with its pointers taken out and links in their place. `write_ycd` lays the same
graph out again, so a dictionary comes back with every structure the same; `AnimationRef` and `ClipRef` read the
fields tools need (names, times, frames, tracks, the raw sequence blocks) and `set_duration` / `RscBlock::set` change
values in place. The schema follows CodeWalker's Clip.cs; `read_ycd_checked` counts 8-byte values that look like
pointers where the schema takes none (0 on every game file once the name hashes that happen to look like pointers are
marked as data).

```cpp
ClipDictionary d = read_ycd(file);
for (const ClipRef& c : d.clips())
    std::printf("%s %.2f s\n", c.name().c_str(), c.end_time() - c.start_time());
```

From `rk_unknowns ycd` (every 5th file, 48,719 animations):

- A track's second byte (CodeWalker's `Unk0`, `AnimationBoneId::format`) is its value type: every track number has one
  value on all 2.3 million tracks: 0 on vector tracks (0 translation, 2 scale, 5, 7, 25, 28 ...), 1 on quaternion tracks
  (1 rotation, 6, 8, 26), 2 on float tracks (22, 24, 27, 30 ... 53, 134 ... 140).
- The animation's byte at 0x10 has 0x10 exactly when one of its sequences carries root motion (37,674 animations);
  0x01 (332) and 0x08 (44) occur besides. 0x11 and 0x04 are 1 on every animation.
- The animation's u32 at 0x3C (CodeWalker's `UsageCount`) is 1 + the number of the dictionary's clips that play it, on
  every animation.
- The u32 at 0x1C varies (1,270 values, 0x30080E02 on a fifth); not understood.

Every Legacy and Enhanced dictionary reads back with the same graph but four: `x64f.rpf/levels/gta5/destruction/
{exterior,interior}/des_{bridge,setpiece,fib_ceiling2,hosp_ceil}.rpf` are AES-encrypted archives of older resources
(ycd 43, ydr 164, ybd 40) whose streams inflate neither after AES nor NG decryption, in ragekit or in CodeWalker (the
same bytes come out of both); Enhanced's copies read.

## Audio containers (`awc.h`)

`.awc` files (7,742 in Legacy, 7,760 in Enhanced) hold the game's sounds: sound effects, speech, music and radio. A file
is a table and the chunks it points at, per stream: a format chunk (samples, sample rate, codec), the data, and seek
tables, markers, peaks, granular grains and loops. A multi-channel file (flag 4) has one stream whose stream format
chunk lists the channels and whose data is interleaved in blocks; the other streams name the channels.

```
"ADAT" version:u16 flags:u16 streams:u32 data_offset:u32  [first chunk index:u16 per stream, flag 1]
id:29 | chunks:3 (u32 per stream)   type:8 | size:28 | offset:28 (u64 per chunk)   the chunks
```

`read_awc` keeps every chunk as stored and its offset, so `write_awc` gives the file again byte for byte (the gaps
between chunks are zero in every game file); `AwcFile::pack` lays the chunks out anew after an edit, in the order
and alignment the game's files use. Encrypted files use Rockstar's XXTEA (the reference algorithm with each key word
xored with 0x7B3A207F) with the 16-byte key that follows the hash table in CodeWalker's `magic.dat`; `rk rpf keys`
saves it as `gtav_awc_key.dat` with the other keys (`RpfKeys::awc`). A whole file can be encrypted (its magic does not
read as stored), the data chunk of a single-channel file (flag 2) or each block of a multi-channel file (flag 8).

`awc_decode` decrypts and decodes every sound to 16-bit PCM: PCM (codec 0) and the game's IMA ADPCM (codec 4: blocks of
2,048 bytes, each a step index, a predictor and 2,044 bytes of nibbles); `awc_wav` makes a .wav of one. A sound of
another codec is listed without samples: Legacy has one (codec 5, `vw_counterfeit_chips.awc`), Enhanced stores 2,538
multi-channel files and 1,664 others in codec 7, whose blocks are laid out otherwise (not decoded; GUESS a compressed
codec). Enhanced's `update2.rpf/common/shaders/fxdb/sga_win32_60_final*.awc` are shader caches with the extension, not
audio (`is_awc` is false). `rk info file.awc` lists the streams, chunks and sounds; `rk awc wav file.awc dir` writes
the sounds.

## Expression and frame filter dictionaries (`expression.h`)

`.yed` files (242 in Legacy, `anim/expressions.rpf`, version 25) hold the expressions that drive peds' faces and
secondary motion: each a name, instruction streams (compiled, kept raw), the bone tracks it drives, springs and
variables. `.yfd` files (4, `anim/filters.rpf`, version 4) hold frame filters: which tracks a blend takes, with what
weight (all of type 4, multi-weight). Both are pgDictionaries (name hashes and a list of items) and are kept as read in
a graph of blocks like the clip dictionaries, with views for the names, tracks, streams and weights; every game file of
both editions comes back with the same graph and no pointer-like value the schema does not know.

## Bound dictionaries (`bounds.h`)

`.ybd` files are pgDictionaries of collision bounds (`BoundDictionary`: name hashes and bounds, read and written with
the .ybn code). The game has four: `x64e.rpf/levels/generic/ped_props.rpf/ped_props.ybd` (version 43, three boxes;
it reads back field for field in both editions) and three in the destruction archives of `x64f.rpf`, one whose stream
does not inflate (above) and two of version 42, an older bound layout ragekit does not read: `read_ybd` and `read_ybn`
throw for a version other than 43 (until this check a version 42 file read as geometries without vertices; every .ybn
of both installs is version 43).

## Scripts (`script.h`)

`.ysc` files (1,158 in Legacy, 1,864 in Enhanced; `levels/gta5/script/script_rel.rpf` and the packs') are the compiled
scripts: a 0x80-byte header (name and its hash, code size, parameter, static, global and native counts, the globals
signature), the bytecode in pages of 16 KB, the statics' initial values, the globals of the scripts that own a globals
block (pages of 0x20000 bytes, 16,384 globals each; the count is the low 18 bits of the word at 0x28, the block the
rest), the native hashes and the strings (pages of 16 KB). In the archives they are the one resource kind stored
NG-encrypted (by the entry's name and size), which `Rpf::extract` undoes.

`read_ysc` keeps every structure as read (graph.h), `write_ysc` writes it back the same; `Script` gives the header's
values, the code and strings joined, the statics, and the natives, which are stored rotated: rotated back left by
(code size + index) & 63 they are the hashes the game's natives are known by (`WAIT` 0x4EDE34FBADD967A6 among the
first file's). There is no decompiler. Version 12 is Legacy's; Enhanced's base scripts and four of Legacy's `mppilot`
ones are version 10, the same layout. Every file of both editions reads back with the same graph.
