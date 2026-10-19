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
