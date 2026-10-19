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
