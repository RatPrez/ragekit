// oracle: loads files written by ragekit with CodeWalker's own parser, and converts
// CodeWalker XML (ytyp/ymap) to binary. Usage: oracle <file>...
using System.Xml;
using CodeWalker.GameFiles;

RpfManager.IsGen9 = false;
// oracle rpf <gameFolder> <file.rpf> [--hash] [--nested]: list an archive (see Rpf.cs)
if (args.Length >= 3 && args[0] == "rpf") return RpfDump.Run(args);
if (args.Length == 3 && args[0] == "rpfkeys") return RpfDump.Keys(args);
// oracle index <gameFolder> <rk-dump.tsv>: ragekit's game index against CodeWalker's GameFileCache (see Index.cs)
if (args.Length >= 3 && args[0] == "index") return IndexOracle.Run(args);
// oracle world <gameFolder> <out.tsv>: the ymaps / ybns CodeWalker loads with every DLC pack (see World.cs)
if (args.Length == 3 && args[0] == "world") return WorldOracle.Run(args);
// oracle pair <list.txt> [xmlDiffDir]: compare original|written pairs of ytyp/ymap files (see Pair.cs)
if (args.Length >= 2 && args[0] == "pair") return Pair.Run(args[1], args.Length > 2 ? args[2] : null);
// oracle pso <list.txt> [xmlDir]: PSO files (binary ymt): CodeWalker XML of original|written must agree (see PsoOracle.cs)
if (args.Length >= 2 && args[0] == "pso") return PsoOracle.Run(args[1], args.Length > 2 ? args[2] : null);
// oracle rel <list.txt> [xmlDir]: .rel files: CodeWalker XML of original|written must agree (see RelOracle.cs)
if (args.Length >= 2 && args[0] == "rel") return RelOracle.Run(args[1], args.Length > 2 ? args[2] : null);
// oracle relxml <in.xml> <out.rel> / oracle psoxml <in.pso.xml> <out.ymt>: compile XML with CodeWalker
if (args.Length == 3 && args[0] == "relxml") return RelOracle.FromXml(args[1], args[2]);
if (args.Length == 3 && args[0] == "psoxml") return RelOracle.PsoFromXml(args[1], args[2]);
// oracle ymf [--xml] <file>...: CodeWalker YmfFile view of streaming manifests
if (args.Length >= 2 && args[0] == "ymf") { bool x = args[1] == "--xml"; return PsoOracle.Ymf(args.Skip(x ? 2 : 1).ToArray(), x); }
// xml: oracle cwxml <file> <outdir> / xmlin <file.xml> <out> [ddsdir] / xmlnames <out.inc>: CodeWalker's drawable/bound/texture XML (see XmlOracle.cs)
if (args.Length >= 3 && args[0] == "cwxml") return XmlOracle.Export(args);
if (args.Length >= 3 && args[0] == "xmlin") return XmlOracle.Import(args);
if (args.Length == 3 && args[0] == "cwxmllist") return XmlOracle.ExportList(args);
if (args.Length == 3 && args[0] == "xmlinlist") return XmlOracle.ImportList(args);
if (args.Length == 2 && args[0] == "xmlnames") return XmlOracle.Names(args);
// oracle xml <outdir> <file>...: CodeWalker's XML for ytyp/ymap files
if (args.Length >= 2 && args[0] == "xml")
{
    Directory.CreateDirectory(args[1]);
    foreach (var f in args.Skip(2))
    {
        var summary = Pair.Describe(f, out var xmlText);
        File.WriteAllText(Path.Combine(args[1], Path.GetFileName(f) + ".xml"), xmlText);
        Console.Write(summary);
    }
    return 0;
}
// oracle ymf <manifest.pso.xml> <out.ymf>: compile a CPackFileMetaData manifest with CodeWalker (XmlPso) and read it back
if (args.Length == 3 && args[0] == "ymf")
{
    var doc = new XmlDocument();
    doc.Load(args[1]);
    var pso = XmlPso.GetPso(doc);
    var bytes = pso.Save();
    File.WriteAllBytes(args[2], bytes);
    var ymf = new YmfFile();
    ymf.Load(bytes, new RpfBinaryFileEntry { Name = Path.GetFileName(args[2]), NameLower = Path.GetFileName(args[2]).ToLowerInvariant() });
    Console.WriteLine($"ok   {Path.GetFileName(args[2])} written ({bytes.Length} bytes): imapDeps2 {ymf.imapDependencies2?.Length ?? 0} itypDeps2 {ymf.itypDependencies2?.Length ?? 0} interiors {ymf.Interiors?.Length ?? 0}");
    if (ymf.itypDependencies2 != null) foreach (var d in ymf.itypDependencies2) Console.WriteLine($"       ityp {d.Dep.itypName} flags {d.Dep.manifestFlags} deps {d.itypDepArray?.Length ?? 0}");
    if (ymf.imapDependencies2 != null) foreach (var d in ymf.imapDependencies2) Console.WriteLine($"       imap {d.Dep.imapName} flags {d.Dep.manifestFlags} deps {d.itypDepArray?.Length ?? 0}");
    if (ymf.Interiors != null) foreach (var d in ymf.Interiors) Console.WriteLine($"       interior {d.Interior.Name} bounds {d.Bounds?.Length ?? 0}");
    return 0;
}
int failures = 0;
foreach (var path in args)
{
    try
    {
        var data = File.ReadAllBytes(path);
        // GTA V Enhanced resources (ytd 5, ydr/ydd 159, yft 171): CodeWalker reads them with RpfManager.IsGen9 set
        var rscVersion = data.Length >= 8 && BitConverter.ToUInt32(data, 0) == 0x37435352 ? BitConverter.ToInt32(data, 4) : 0;
        var lowerName = path.ToLowerInvariant();
        RpfManager.IsGen9 = (lowerName.EndsWith(".ytd") && rscVersion == 5) || ((lowerName.EndsWith(".ydr") || lowerName.EndsWith(".ydd")) && (rscVersion == 159 || rscVersion == 154))
                            || (lowerName.EndsWith(".yft") && rscVersion == 171) || (lowerName.EndsWith(".ypt") && rscVersion == 71);
        var lower = path.ToLowerInvariant();
        if (lower.EndsWith(".ytyp") || lower.EndsWith(".ymap"))
        {
            Console.Write("ok   " + Path.GetFileName(path) + ": " + Pair.Describe(path, out _));
        }
        else if (lower.EndsWith(".yft"))
        {
            var f = RpfFile.GetResourceFile<YftFile>(data).Fragment;
            string drw(DrawableBase d) => d == null ? "-" : $"{d.AllModels?.Length ?? 0} models {d.AllModels?.Sum(m => m.Geometries?.Length ?? 0) ?? 0} geoms {d.ShaderGroup?.Shaders?.data_items?.Length ?? 0} shaders";
            Console.WriteLine($"ok   {Path.GetFileName(path)}: fragment '{f.Name}' sphere {f.BoundingSphereCenter} r {f.BoundingSphereRadius} drawable [{drw(f.Drawable)}] array {f.DrawableArray?.data_items?.Length ?? 0} cloth drawable [{drw(f.DrawableCloth)}]");
            Console.WriteLine($"       glass {f.GlassWindows?.data_items?.Length ?? 0} vehicle glass {(f.VehicleGlassWindows != null ? f.VehicleGlassWindows.TotalLength : 0)} cloths {f.Cloths?.data_items?.Length ?? 0} lights {f.LightAttributes?.data_items?.Length ?? 0} bones {f.BoneTransforms?.Items?.Length ?? 0} gravity {f.GravityFactor} buoyancy {f.BuoyancyFactor}");
            var lods = new[] { f.PhysicsLODGroup?.PhysicsLOD1, f.PhysicsLODGroup?.PhysicsLOD2, f.PhysicsLODGroup?.PhysicsLOD3 };
            for (int l = 0; l < 3; l++)
            {
                var lod = lods[l];
                if (lod == null) continue;
                Console.WriteLine($"       lod{l}: groups {lod.Groups?.data_items?.Length ?? 0} children {lod.Children?.data_items?.Length ?? 0} joints {lod.ArticulatedBodyType?.Joints?.data_items?.Length ?? 0} transforms {lod.FragTransforms?.Matrices?.Length ?? 0} bound {lod.Bound?.Type} archetype '{lod.Archetype1?.Name}' {lod.Archetype1?.Mass} damaged '{lod.Archetype2?.Name}'");
                if (lod.Groups?.data_items != null) foreach (var g in lod.Groups.data_items) Console.WriteLine($"         group {g.Name} parent {g.ParentIndex} children {g.ChildCount} mass {g.Mass} strength {g.Strength}");
                if (lod.Children?.data_items != null) foreach (var c in lod.Children.data_items) Console.WriteLine($"         child group {c.GroupIndex} bone {c.BoneTag} mass {c.PristineMass}/{c.DamagedMass} drawable [{drw(c.Drawable1)}] damaged [{drw(c.Drawable2)}] events {(c.EvtSet != null ? 1 : 0)}");
            }
        }
        else if (lower.EndsWith(".ydr"))
        {
            var ydr = RpfFile.GetResourceFile<YdrFile>(data);
            Console.WriteLine($"ok   {Path.GetFileName(path)}:");
            if (!DescribeDrawable(ydr.Drawable)) failures++;
        }
        else if (lower.EndsWith(".ynv"))
        {
            var ynv = RpfFile.GetResourceFile<YnvFile>(data);
            var n = ynv.Nav;
            byte[] bytesOf<T>(IEnumerable<T> items) where T : struct
            {
                var list = items?.ToArray() ?? Array.Empty<T>();
                int size = System.Runtime.InteropServices.Marshal.SizeOf<T>();
                var buf = new byte[list.Length * size];
                var h = System.Runtime.InteropServices.GCHandle.Alloc(list, System.Runtime.InteropServices.GCHandleType.Pinned);
                System.Runtime.InteropServices.Marshal.Copy(h.AddrOfPinnedObject(), buf, 0, buf.Length);
                h.Free();
                return buf;
            }
            Console.WriteLine($"ok   {Path.GetFileName(path)}: area {n.AreaID} flags {n.ContentFlags} vertices {n.VerticesCount} polys {n.PolysCount} indices {n.EdgesIndicesCount} points {n.PointsCount} portals {n.PortalsCount} links {n.PortalLinksCount} total {n.TotalBytes} adjacent [{string.Join(" ", n.AdjAreaIDs.Values)}]");
            Console.WriteLine($"       vertices {Fnv(bytesOf(n.Vertices?.GetFullList())):X16} indices {Fnv(bytesOf(n.Indices?.GetFullList())):X16} edges {Fnv(bytesOf(n.Edges?.GetFullList())):X16} polys {Fnv(bytesOf(n.Polys?.GetFullList())):X16} portals {Fnv(bytesOf(n.Portals)):X16}");
        }
        else if (lower.EndsWith(".ypt"))
        {
            var ypt = RpfFile.GetResourceFile<YptFile>(data);
            var l = ypt.PtfxList;
            var prules = l?.ParticleRuleDictionary?.ParticleRules?.data_items;
            var erules = l?.EffectRuleDictionary?.EffectRules?.data_items;
            var mrules = l?.EmitterRuleDictionary?.EmitterRules?.data_items;
            Console.WriteLine($"ok   {Path.GetFileName(path)}: '{l?.Name?.Value}' textures {l?.TextureDictionary?.Textures?.data_items?.Length ?? 0} drawables {l?.DrawableDictionary?.Drawables?.data_items?.Length ?? 0} particle rules {prules?.Length ?? 0} effect rules {erules?.Length ?? 0} emitter rules {mrules?.Length ?? 0}");
            if (l?.TextureDictionary?.Textures?.data_items != null)
                foreach (var t in l.TextureDictionary.Textures.data_items) Console.WriteLine($"       texture {t.Name} {t.Width}x{t.Height} {t.Format} levels {t.Levels} data {Fnv(t.Data?.FullData):X16}");
            if (prules != null) foreach (var r in prules) Console.WriteLine($"       particle rule {r.Name?.Value} behaviours {r.AllBehaviours?.data_items?.Length ?? 0} shader {r.ShaderFile?.Value}/{r.ShaderTechnique?.Value} vars {r.ShaderVars?.data_items?.Length ?? 0} drawables {r.Drawables?.data_items?.Length ?? 0}");
            if (erules != null) foreach (var r in erules) Console.WriteLine($"       effect rule {r.Name?.Value} emitters {r.EventEmitters?.data_items?.Count(e => e != null) ?? 0} duration {r.DurationMin}..{r.DurationMax}");
            if (mrules != null) foreach (var r in mrules) Console.WriteLine($"       emitter rule {r.Name?.Value} domains {(r.CreationDomainObj != null ? 1 : 0)}{(r.TargetDomainObj != null ? 1 : 0)}{(r.AttractorDomainObj != null ? 1 : 0)}");
            if (l?.DrawableDictionary?.Drawables?.data_items != null) foreach (var d in l.DrawableDictionary.Drawables.data_items) if (!DescribeDrawable(d)) failures++;
        }
        else if (lower.EndsWith(".ydd"))
        {
            var ydd = RpfFile.GetResourceFile<YddFile>(data);
            Console.WriteLine($"ok   {Path.GetFileName(path)}: {ydd.Drawables?.Length ?? 0} drawables");
            if (ydd.Drawables != null) foreach (var d in ydd.Drawables) if (!DescribeDrawable(d)) failures++;
        }
        else if (lower.EndsWith(".ybn"))
        {
            var ybn = RpfFile.GetResourceFile<YbnFile>(data);
            void dump(Bounds b, int depth)
            {
                var ind = new string(' ', depth * 2);
                Console.Write($"       {ind}{b.Type} box {b.BoxMin} .. {b.BoxMax} mat {b.MaterialIndex}");
                if (b is BoundGeometry g) Console.Write($" verts {g.Vertices?.Length} polys {g.Polygons?.Length} mats {g.Materials?.Length}");
                if (b is BoundBVH bv) Console.Write($" bvh nodes {bv.BVH?.Nodes?.EntriesCount}/{bv.BVH?.Nodes?.EntriesCapacity} trees {bv.BVH?.Trees?.EntriesCount}");
                if (b is BoundComposite c)
                {
                    Console.Write($" children {c.Children?.data_items?.Length}");
                    if (c.ChildrenFlags1 != null) for (int i = 0; i < c.ChildrenFlags1.Length; i++) Console.Write($" [child {i} flags1 {c.ChildrenFlags1[i].Flags1} ({(uint)c.ChildrenFlags1[i].Flags1:X}) flags2 {(uint)c.ChildrenFlags1[i].Flags2:X}]");
                }
                Console.WriteLine();
                if (b is BoundComposite cc && cc.Children?.data_items != null) foreach (var ch in cc.Children.data_items) if (ch != null) dump(ch, depth + 1);
            }
            Console.WriteLine($"ok   {Path.GetFileName(path)}:");
            dump(ybn.Bounds, 0);
        }
        else if (lower.EndsWith(".ymf"))
        {
            var ymf = new YmfFile();
            ymf.Load(data, new RpfBinaryFileEntry { Name = Path.GetFileName(path), NameLower = Path.GetFileName(path).ToLowerInvariant() });
            Console.WriteLine($"ok   {Path.GetFileName(path)}: {(ymf.Pso != null ? "PSO" : ymf.Rbf != null ? "RBF" : "?")} imapDeps2 {ymf.imapDependencies2?.Length ?? 0} itypDeps2 {ymf.itypDependencies2?.Length ?? 0} interiors {ymf.Interiors?.Length ?? 0} mapDataGroups {ymf.MapDataGroups?.Length ?? 0} hdTxd {ymf.HDTxdAssetBindings?.Length ?? 0}");
            if (ymf.itypDependencies2 != null) foreach (var d in ymf.itypDependencies2) Console.WriteLine($"       ityp {d.Dep.itypName} flags {d.Dep.manifestFlags} deps [{string.Join(", ", (d.itypDepArray ?? Array.Empty<MetaHash>()).Select(h => h.ToString()))}]");
            if (ymf.imapDependencies2 != null) foreach (var d in ymf.imapDependencies2) Console.WriteLine($"       imap {d.Dep.imapName} flags {d.Dep.manifestFlags} deps [{string.Join(", ", (d.itypDepArray ?? Array.Empty<MetaHash>()).Select(h => h.ToString()))}]");
            if (ymf.Interiors != null) foreach (var d in ymf.Interiors) Console.WriteLine($"       interior {d.Interior.Name} bounds [{string.Join(", ", (d.Bounds ?? Array.Empty<MetaHash>()).Select(h => h.ToString()))}]");
            if (ymf.Pso != null) Console.WriteLine(PsoXml.GetXml(ymf.Pso));
        }
        else if (lower.EndsWith(".ytd"))
        {
            var ytd = RpfFile.GetResourceFile<YtdFile>(data);
            Console.WriteLine($"ok   {Path.GetFileName(path)}: {ytd.TextureDict?.Textures?.data_items?.Length} textures");
            foreach (var t in ytd.TextureDict.Textures.data_items) Console.WriteLine($"       {t.Name} {t.Width}x{t.Height} {t.Format} levels {t.Levels} {t.Data?.FullData?.Length} bytes usage {t.Usage} data {Fnv(t.Data?.FullData):X16}");
        }
        else if (lower.EndsWith(".awc"))
        {
            // a wave pack: every stream with its format and an FNV-1a of its samples decoded to 16-bit PCM by CodeWalker (its ADPCM
            // decoder gives whole blocks: the hash covers the stream's own sample count)
            var awc = new AwcFile();
            awc.Load(data, new RpfBinaryFileEntry { Name = Path.GetFileName(path), NameLower = Path.GetFileName(path).ToLowerInvariant() });
            if (!string.IsNullOrEmpty(awc.ErrorMessage)) throw new Exception(awc.ErrorMessage);
            Console.WriteLine($"ok   {Path.GetFileName(path)}: flags 0x{awc.Flags:X4} streams {awc.Streams?.Length ?? 0}");
            foreach (var s in awc.Streams ?? Array.Empty<AwcStream>())
            {
                var f = s.FormatChunk;
                var pcm = s.GetPcmData();
                if (pcm != null && pcm.Length > s.SampleCount * 2) pcm = pcm.Take(s.SampleCount * 2).ToArray();
                Console.WriteLine($"       stream {(uint)s.Hash:X8} chunks [{string.Join(" ", s.Chunks.Select(c => c.ChunkInfo.Type.ToString()))}] codec {f?.Codec} samples {s.SampleCount} rate {s.SamplesPerSecond} loop {f?.LoopPoint} peak {f?.PeakVal} peaks {s.PeakChunk?.Data?.Length ?? 0} pcm {pcm?.Length ?? 0} bytes fnv {Fnv(pcm):X16}");
            }
        }
        else if (lower.EndsWith(".rel"))
        {
            // an audio data file: every item as CodeWalker types it, then its XML
            var xml = RelOracle.Xml(data, out var rel);
            Console.WriteLine($"ok   {Path.GetFileName(path)}: {rel.RelType} items {rel.RelDatas?.Length ?? 0} hash table {rel.HashTableCount} pack table {rel.PackTableCount}");
            foreach (var d in rel.RelDatas ?? Array.Empty<RelData>()) Console.WriteLine($"       item {(uint)d.NameHash:X8} {d.GetType().Name}");
            Console.WriteLine(xml);
        }
        else if (lower.EndsWith(".ytyp.xml") || lower.EndsWith(".ymap.xml"))
        {
            var doc = new XmlDocument();
            doc.Load(path);
            var meta = XmlMeta.GetMeta(doc);
            if (meta == null) throw new Exception("XML did not produce a meta");
            var bytes = ResourceBuilder.Build(meta, 2);
            var outPath = path.Substring(0, path.Length - 4);
            File.WriteAllBytes(outPath, bytes);
            if (lower.EndsWith(".ytyp.xml"))
            {
                var ytyp = RpfFile.GetResourceFile<YtypFile>(bytes);
                Console.WriteLine($"ok   {Path.GetFileName(outPath)} written ({bytes.Length} bytes): {ytyp.AllArchetypes?.Length} archetypes");
                foreach (var a in ytyp.AllArchetypes)
                {
                    Console.WriteLine($"       {a.Type} {a.Name} drawable {a.AssetName} bb {a.BBMin} .. {a.BBMax}");
                    if (a is MloArchetype mlo)
                    {
                        foreach (var r in mlo.rooms) Console.WriteLine($"         room {r.RoomName} portals {r._Data.portalCount} bb {r._Data.bbMin} .. {r._Data.bbMax}");
                        foreach (var p in mlo.portals) Console.WriteLine($"         portal {p._Data.roomFrom} -> {p._Data.roomTo} flags {p._Data.flags} corners {p.Corners?.Length}");
                    }
                }
                var back = MetaXml.GetXml(ytyp.Meta);
                File.WriteAllText(outPath + ".back.xml", back);
                Console.WriteLine($"       round-trip xml written to {Path.GetFileName(outPath)}.back.xml ({back.Split('\n').Count(l => l.Contains("corners"))} corner lines)");
            }
            else
            {
                var ymap = RpfFile.GetResourceFile<YmapFile>(bytes);
                Console.WriteLine($"ok   {Path.GetFileName(outPath)} written ({bytes.Length} bytes): {ymap.AllEntities?.Length} entities, {ymap.MloEntities?.Length} mlo");
                foreach (var e in ymap.AllEntities) Console.WriteLine($"       {e._CEntityDef.archetypeName} at {e.Position} lod {e._CEntityDef.lodLevel} flags {e._CEntityDef.flags}");
            }
        }
        else Console.WriteLine($"skip {path}");
    }
    catch (Exception ex)
    {
        failures++;
        Console.WriteLine($"FAIL {Path.GetFileName(path)}: {ex.GetType().Name}: {ex.Message}");
    }
}
return failures;

// FNV-1a of a byte array (0 for none)
static ulong Fnv(byte[] b)
{
    if (b == null) return 0;
    ulong h = 1469598103934665603;
    foreach (var x in b) { h ^= x; h *= 1099511628211; }
    return h;
}

// A drawable as CodeWalker sees it: shaders with their parameters (Legacy names, values, textures), every geometry with
// hashes of its vertices (CodeWalker's Legacy order) and indices, skeleton, lights, bound. Errors return false.
static bool DescribeDrawable(DrawableBase d)
{
    if (d == null) { Console.WriteLine("       (null drawable)"); return true; }
    Console.WriteLine($"       drawable '{(d as Drawable)?.Name}' shaders {d.ShaderGroup?.Shaders?.data_items?.Length ?? 0} textures {d.ShaderGroup?.TextureDictionary?.Textures?.data_items?.Length ?? 0} models {d.AllModels?.Length ?? 0} bones {d.Skeleton?.Bones?.Items?.Length ?? 0} lights {(d as Drawable)?.LightAttributes?.data_items?.Length ?? 0}");
    var shaders = d.ShaderGroup?.Shaders?.data_items;
    if (shaders != null)
        foreach (var s in shaders)
        {
            var line = $"       shader {s.Name} bucket {s.RenderBucket}:";
            var ps = s.ParametersList?.Parameters; var hs = s.ParametersList?.Hashes;
            if (ps != null && hs != null)
                for (int i = 0; i < ps.Length && i < hs.Length; i++)
                {
                    var v = ps[i].Data;
                    string val = v is TextureBase tb ? "'" + tb.Name + "'" : v is SharpDX.Vector4 q ? $"({q.X} {q.Y} {q.Z} {q.W})" : v is SharpDX.Vector4[] a ? $"[{a.Length}]({string.Join(" ", a.Select(x => $"{x.X},{x.Y},{x.Z},{x.W}"))})" : "-";
                    line += $" {(uint)hs[i]:X8}={val}";
                }
            Console.WriteLine(line);
        }
    if (d.AllModels != null)
        foreach (var m in d.AllModels)
            if (m.Geometries != null)
                foreach (var g in m.Geometries)
                    Console.WriteLine($"       geom: {g.VerticesCount} verts {g.IndicesCount} indices shader {g.ShaderID} ({g.Shader?.Name}) decl {g.VertexBuffer?.Info?.Flags} stride {g.VertexStride} vertices {Fnv(g.VertexData?.VertexBytes):X16} indices {Fnv(g.IndexBuffer?.Indices?.SelectMany(BitConverter.GetBytes).ToArray()):X16}");
    if ((d as Drawable)?.Bound != null) Console.WriteLine($"       bound {(d as Drawable).Bound.Type}");
    var err = (d as Drawable)?.ErrorMessage;
    if (!string.IsNullOrEmpty(err)) { Console.WriteLine("       ERROR " + err); return false; }
    return true;
}
