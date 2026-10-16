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
