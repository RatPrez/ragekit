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
            var d = ydr.Drawable;
            Console.WriteLine($"ok   {Path.GetFileName(path)}: drawable '{d.Name}' shaders {d.ShaderGroup?.Shaders?.data_items?.Length ?? 0} textures {d.ShaderGroup?.TextureDictionary?.Textures?.data_items?.Length ?? 0} high models {d.DrawableModels?.High?.Length ?? 0}");
            foreach (var m in d.AllModels)
                foreach (var g in m.Geometries)
                    Console.WriteLine($"       geom: {g.VerticesCount} verts {g.IndicesCount} indices shader {g.ShaderID} ({g.Shader?.Name}) decl {g.VertexBuffer?.Info?.Flags} stride {g.VertexStride}");
            if (d.Bound != null) Console.WriteLine($"       bound {d.Bound.Type}");
            if (!string.IsNullOrEmpty(d.ErrorMessage)) { Console.WriteLine("       ERROR " + d.ErrorMessage); failures++; }
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
            foreach (var t in ytd.TextureDict.Textures.data_items) Console.WriteLine($"       {t.Name} {t.Width}x{t.Height} {t.Format} levels {t.Levels} {t.Data?.FullData?.Length} bytes");
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
