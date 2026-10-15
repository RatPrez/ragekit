# The rk tool

`rk` (`tools/rk.cpp`, built with `RAGEKIT_BUILD_CLI`) puts most of the library on the command line.

```
rk info <file> [-v]                    header, segments, contents (.ytyp/.ymap: structure infos and blocks; RBF: the tree)
rk roundtrip <in> <out>                read, write, read back
rk psodump [--max N] [--flat] <file>   a text dump of a PSO file (binary .ymt, .ypdb, .ymf ...); info -v lists its sections and schema
rk ymf <in.ymf> [out.ymf]              list a streaming manifest, optionally write it back with write_ymf
rk ymf-make <out.ymf> <interior> <ytyp> <ybn> [<ytyp dependency>...]   the manifest a custom interior resource needs
rk metaout <dir> <outdir> [--generic]  write every .ytyp/.ymap under dir (typed, or through the generic container) to outdir
rk xml <ydr|ydd|ybn|ytd> <out folder>  CodeWalker's "Export XML": <name>.<ext>.xml and a <name> folder of DDS files
rk ytd export <ytd> <dir>              every texture as .dds and .png
rk ytd build <dir> <out.ytd>           every .dds / .png of a folder into a dictionary
rk gltf <ydr|ydd|yft> <out.glb> [textures.ytd ...] [--all-lods] [--no-textures]
rk fxc <file.fxc>                      a compiled shader effect: constant buffers, variables, techniques
rk shaders ...                         the shader database of an install
rk rel info|roundtrip|scan ...         .rel audio data files
rk occlusion <ytyp> <out dir> [--ymap f | --pos x,y,z] [--archetype n]   interior audio: <proxy>.ymt and the dat151 rel
rk occlusion info|roundtrip|scan ...   inspect, rewrite or bulk-check occlusion files
rk rpf ls|cat|extract|keys|pack|add|rm|rewrite ...   archives, see archives.md
rk index build|info|versions, rk find, rk pull        the game asset index, see game-index.md
rk world info|at|near|mlos|occluders                 the world index, see game-index.md
```

Run a command without arguments for its own usage.
