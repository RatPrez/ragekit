# RSC7 resources

Every streamed asset of the game (`.ydr .ydd .ytd .yft .ybn .ytyp .ymap ...`) is an RSC7 resource. `rsc.h` reads and
writes the container; the format headers (`drawable.h`, `texture.h` ...) sit on top of it.

## The container

```
0   u32  magic "RSC7" (0x37435352)
4   u32  version (the file type's number: ydr 165, ytd 13, ybn 43, ytyp/ymap 2 ...; rsc::Version)
8   u32  system segment page flags
12  u32  graphics segment page flags
16  ...  raw deflate stream: the system segment, then the graphics segment
```

The system ("virtual") segment holds the structures; the graphics ("physical") segment holds what the GPU takes, which
in GTA V files is texture data. Pointers inside the data are `0x50000000 + offset` into the system segment and
`0x60000000 + offset` into the graphics segment. The structure at system offset 0 is the root; its second pointer (at
root + 8) points at the page map.

```cpp
rsc::Resource r = rsc::Resource::load(file);     // both segments, inflated
const uint8_t* p = r.at(ptr, 16);                 // a game pointer to bytes, bounds checked
Reader rd(r);
uint32_t n = rd.u32(ptr + 8);                     // typed reads at game pointers
std::string s = rd.str(rd.u64(ptr + 16));
```

## Page flags

A segment is a run of pages. The flags give a base size (`0x200 << shift`, bits 0-3) and how many pages of each of nine
classes there are, largest first: class *i* holds pages of `base << (8 - i)`. The count fields have room for 1, 3, 15,
63 and 127 pages of classes 0 to 4 and one page each of classes 5 to 8 (bits 4-27). Bits 28-31 carry a nibble of the
version (system: `version >> 4`, graphics: `version & 15`). Pages lie largest first; no block may cross from one page
into the next; a resource has at most 128 pages in all.

`rsc::PageFlags` decodes them (`counts()`, `sizes()`, `page_count()`, `total_size()`) and `PageFlags::make(shift, counts)`
encodes them.

## Building a resource

The writers lay out their structures as blocks and let `Builder` place them:

```cpp
Builder b;
BlockId root = b.alloc(Segment::System, 0x40);           // the first system block is the root
BlockId pages = b.alloc_pages_info();                    // the page map, sized and filled by build()
BlockId name = b.string("my_name");
BlockId pixels = b.array(Segment::Graphics, texels);

BlockWriter w(b, root);
w.u64(0);                                                // the structure's own fields ...
w.ptr(pages);                                            // root + 8: the page map
w.ptr(name);                                             // pointers are patched once the layout is known
Bytes file = b.build(rsc::kYtd);                         // pages laid out, pointers patched, deflated
```

What the game's own files do, read from 16,000 of them (every 20th resource of the Legacy install), and what
`build()` does the same:

- No page is smaller than 8 KB. The small classes 5 to 8 appear only at a shift of 1 or more, where they are 8 KB or
  larger; no segment is made of small pages alone.
- The page map is exactly 16 bytes plus 8 per page, and its counts (bytes 8 and 9) equal the flags'.
- The root opens the first, largest page.

Within those rules the layout is `build()`'s own: for every shift it packs the blocks best fit, largest first,
opening new pages in three ways (the tightest class that holds the block, the largest class any block needs, the
largest class with a page left), shrinks every page to the smallest class that still holds it, and keeps the layout
with the fewest bytes, then the fewest pages. Against the game's own files (every 200th) the pages come to 0.935 of
the bytes for drawables, 0.869 for texture dictionaries and 0.995 for collision; CodeWalker's layout comes to 0.955
and 0.918. `rk_rsc_test` checks those rules on random block sets.

## What lives where

Vertex and index buffers live in the system segment; only texture data goes to the graphics segment. The structures
were first ported from CodeWalker (dexyfex, MIT), with fixes found since: bound vertices round instead of truncating,
and a bound's quantum is kept on write, so game files come back bit-identical. Fields that are always zero are not
stored; the few unknown fields that vary are kept (`unk*` members), so a read -> write pass preserves them.
