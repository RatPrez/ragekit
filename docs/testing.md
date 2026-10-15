# How ragekit is checked

The library is checked against the game's own files and against CodeWalker, the established tool for these formats.

## Round trips (`rk_roundtrip`)

`tests/roundtrip.cpp` reads every resource under the paths it is given, writes it back, reads the result and compares
the two models field by field, one line per file.

```
rk_roundtrip [--only=.ytyp,.ymap,.rel] [--record=f | --check=f] <file or directory>...
rk_roundtrip --game[=install dir] [--every=N] [--max=N] [--only=...] [--record=f | --check=f]
```

`--game` takes a sample of a GTA V install through the game index: every Nth `rel ybn ydd ydr yld yft ymap ymf ymt ytd ytyp meta xml`.
PSO, RBF and `.rel` files must come back byte for byte, XML data files as the same document. A `.yft` is written
with `write_yft`, read again and compared whole: drawables, physics, glass, lights and cloth.
`--record` keeps a digest of everything written per file and `--check` compares a later run with it, so a rewrite of
any module can be held to the bytes the previous code wrote. With `--every=50` that is 4,958 files of the Legacy install
in about four and a half minutes, all of which read back field for field.

## Test programs

| program | checks |
|---|---|
| `rk_rsc_test` | page flags, and the page layout `Builder` makes on random block sets: the root at offset 0, every block inside one page, no overlaps, no page under 8 KB, at most 128 pages, the page map, contents and pointers |
| `rk_texenc_test` | block compression quality (RMS limits on synthetic images) |
| `rk_rpf_test` | cipher test vectors and the archive writer; with archive arguments, every ytd/ydr/ydd/ybn/yft/ytyp/ymap inside them (nested archives too) extracted and parsed |
| `rk_occluder_test` | box occluders, occlude models, entities removed from a ymap; every occlusion ymap of the game round-trips |
| `rk_audio_test` | `.rel` containers and interior audio (proxy hashes, occlusion paths); no game files needed |
| `rk_shaderdb_test` | the shader database: rules, cache, the game's layouts |
| `rk_world_test` | the world index against the game install: counts, MLO instance rotation, queries |
| `rk_xml_test` | the CodeWalker XML writers (`selftest`, and the steps of `tests/cmp_xml.sh`) |
| `rk_build_mlo` | builds a small interior with the typed API, for the oracle |
| `rk_ymf_full` | writes a manifest that uses every array, for CodeWalker to read back (`oracle ymf --xml`) |

`rk_world_test` and `rk_occluder_test` look for the game where `GameIndex::default_game_dir()` finds it and skip their
checks on the game's files without one.

## The CodeWalker oracle

`tests/oracle` is a .NET console program around CodeWalker.Core. It is a test tool only, never part of the library:
it loads files ragekit wrote with CodeWalker's own parsers, so a file is known to mean to CodeWalker what the game's
original means. Build it with `dotnet build -c Release tests/oracle -p:CodeWalkerCore=path\to\CodeWalker.Core.csproj`.

```
oracle <file>...                        load .ydr/.ybn/.ytd/.yft/.ytyp/.ymap/.ymf files and print a summary
oracle pair <list.txt> [xml dir]        "original|written" pairs of ytyp/ymap: the summaries and CodeWalker XML must agree
oracle pso <list.txt> / rel <list.txt>  the same for PSO and .rel files
oracle rpf <game dir> <file.rpf> ...    list an archive as CodeWalker does (tests/cmp_rpf.sh diffs it with rk rpf ls)
oracle index <game dir> ...             CodeWalker's GameFileCache next to the game index (tests/cmp_index.sh)
oracle world <game dir> <tsv>           CodeWalker's world next to the world index
oracle cwxml / cwxmllist / xmlin / xmlinlist   CodeWalker's XML export and import (tests/cmp_xml.sh)
```

## CodeWalker XML

`tests/cmp_xml.sh <list.txt> <work dir>` (list: absolute paths of .ydr/.ydd/.ybn/.ytd files with unique names) checks
the XML writers in four steps: the oracle writes CodeWalker's XML and DDS files, `rk_xml_test gen` writes ragekit's and
compares the text (byte identical, equal within 1e-5, or different), the oracle imports ragekit's XML with CodeWalker
and saves the binaries, and `rk_xml_test summary` reads the originals and the re-imports with ragekit and compares
them (shaders and parameters, textures, models, vertex and index counts and hashes, skeleton, lights, bounds, polygons,
materials). `rk_xml_test synth <dir>` makes files ragekit builds itself (a composite with every primitive kind, lights,
joints, a skeleton, array parameters, every texture format) for the same test.

## Archives

`tests/cmp_rpf.sh` lists an archive with `rk rpf ls` and with the oracle and diffs the listing, flags, sizes and the
SHA256 of every extracted entry. `tests/rpf_blockdiff.py a.rpf b.rpf` compares two unencrypted archives block by block.
