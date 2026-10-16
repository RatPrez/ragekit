# Navmeshes

`navmesh.h` reads and writes `.ynv` files, the surfaces peds walk on. Legacy and Enhanced share the format (version 2).

## Where they live

- The map is a grid of 100 x 100 cells of 150 m, covering x and y from -6000 to 9000. Each cell is one file,
  `navmesh[X*3][Y*3].ynv`, and carries the area id X + Y*100 (Mission Row police station: cell 42, 33, file
  `navmesh[126][99].ynv`, area 3342).
- The base game's cells are in `x64f.rpf/levels/gta5/navmeshes0-9.rpf`. DLCs ship whole rebuilt cells in their own
  `navmeshes.rpf`, each later pack holding the earlier packs' changes; the newest copy is the one the game uses.
- Vehicles with a deck people walk on (trailers, boats, trains) have navmeshes of their own, named after the vehicle.

## What a cell holds

| part | in NavMesh |
|---|---|
| polygons | `polys`: 3 to 13 corners, counter-clockwise, flags; a polygon's corners are `indices[first_index ..]` |
| edges | `edges`: one per index, the side from that corner to the next, saying what lies across it |
| vertices | `vertices`: 16-bit fractions of the cell's box (`position()`, `quantize()`) |
| cover points | in the leaves of the sector quadtree (`sectors`), with a type and a direction |
| special links | `portals` (climbs, drops, ladders) and `portal_links` |
| adjacent areas | `adjacent_areas`: the area ids edges refer to by index |

Neighbouring polygons are linked by index, not by position: an edge names the area (an index into `adjacent_areas`) and
the polygon on the other side, inside the cell and across its borders. Renumbering polygons breaks every link to them,
including the neighbouring cells'. A single edited cell can be shipped on its own as long as the polygons along its
borders keep their numbers; Rockstar's DLCs do exactly that.

`NavEdgePart` decodes an edge: the area index, the polygon (0x3FFF: none, a wall or the edge of the mesh), the link kind
(0 a link, 1 an open edge, 2 a drop or climb to a polygon that shares no edge) and, on a walkable link, the free space at
that edge in 0.25 m steps (bit 0: a height step).

## Reading and writing

```cpp
NavMesh n = read_ynv(rsc::read_file("navmesh[126][99].ynv"));
for (size_t i = 0; i < n.polys.size(); i++) {
    std::vector<Vec3> corners = n.poly_vertices(i);
    const NavEdge& first = n.edges[n.polys[i].first_index];
}
rsc::write_file("navmesh[126][99].ynv", write_ynv(n));
```

`NavMesh` keeps what the game computed and cannot be recomputed exactly: a polygon's box (worked out from its float
corners before they were rounded to 16 bits), the portal link offsets, which polygons each sector leaf lists, and the
order of the adjacent areas. The header's byte total and point count are recomputed, and come out as the game's.

All 10,567 navmeshes of each edition read back field for field after a write, with every byte of the header the
original's, and CodeWalker reads ragekit's writes with the same vertices, indices, edges, polygons and portals.

## Editing

`NavMesh::shapes()` gives every polygon as its corners in world space, its edges and its stored fields;
`set_polys()` takes such a list back and rebuilds the vertices, indices, edges and the sector leaves' polygon lists. A
polygon's number is its place in the list. A shape that still names its `source` polygon keeps that polygon's stored
corners, box, centre and leaves; a new or reshaped one (`source = -1`) gets them worked out the way the game does
(the box from the float corners in quarter metres, towards zero; every leaf its box reaches). Taking every cell of the
game apart this way and putting it back gives it back unchanged.

```cpp
auto shapes = n.shapes();
shapes[17].corners[2].z += 0.5f;   // move a corner
shapes[17].source = -1;            // reshaped: box, centre and leaves recomputed
n.set_polys(shapes);
for (auto& problem : n.check_links()) std::puts(problem.c_str());
```

`check_links()` lists links that do not hold (a link must name a polygon sharing that side, whose side links back);
`cross_cell_changes(before, after)` lists links into the next cells an edit changed. The game's own cells have a few
links the check reports, among the zero-width stitch polygons along their borders.

## Rebuilding an area from collision

`patch_navmesh` (navbuild.h) rebuilds the polygons of one area of a cell from the walkable ground under them, for a map
that moved steps, planters or railings the game's mesh still has. The new polygons take the replaced polygons' numbers,
so nothing else in the cell is renumbered and the cell can be shipped on its own.

1. Every collision surface under a 0.25 m grid; a floor is no steeper than 45 degrees, faces up, is not inside a solid
   and has 1.8 m clear above it.
2. The ground flooded from a seed (steps of at most 0.3 m), kept 0.5 m from walls and drops; 0.4 m around anything
   thin the grid misses that rises more than 0.5 m above the ground beside it (rails, posts), and 0.25 m around props
   given as posts (bollards: the game leaves them to peds' steering, which fails in tight gaps).
3. Heights smoothed over about 1 m (a flight of box steps becomes one ramp, as the game models stairs), the ground split
   into regions within 0.1 m of a plane.
4. The regions outlined together (shared borders stay identical), clipped to the area, triangulated (constrained
   Delaunay) and merged into convex polygons of at most 8 corners.
5. Sides split where a corner of the other side lands on them, linked where both ends meet at the same height; flags
   copied from the replaced polygon underneath, free space computed at every link, cover points left in the open
   dropped.

It works on one level at a time: the area is a rectangle inside one cell and a height range, and the polygons in it
must all lie on the ground flooded from the seed (a roof, a bridge or a tunnel in the rectangle stops it with a message
naming the heights). Drop-down and climb links and new cover points are not generated.

```
rk navmesh patch 414.5,-1011,434,-965 --z 27,35 --ybn mypack/dt1_19_0.ybn --ymap mypack/mrpd.ymap \
   --props prop_bollard_03a -o "navmesh[126][99].ynv"
```

`rk navmesh patch` reads the game's copy of the cell (or `--in`), the game's static collision reaching the area (the
multiplayer map; `--sp` for the single-player one) with the given `.ybn` files in place of the game's of the same name,
writes the cell and checks it: every link of the written polygons, and the links into the next cells unchanged. On the
game's own collision at the Mission Row police station's front, the rebuilt area covers the same ground as the game's
mesh at 91% (intersection over union), heights within 0.19 m, every link holding; CodeWalker reads the result.
`rk navmesh check <file>` checks a cell's links, `rk navmesh cell <x> <y>` names the cell of a point.
