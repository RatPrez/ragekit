// oracle pair <list.txt>: each line is "original|written". Both files are loaded with CodeWalker,
// summarised (archetypes / rooms / portals / entities with names) and converted to CodeWalker's
// own XML; the two must agree. This is the check that a file written by ragekit means to
// CodeWalker what the game's original file does.
using System.Text;
using CodeWalker.GameFiles;

static class Pair
{
    public static int Run(string listPath, string xmlDir)
    {
        int bad = 0, total = 0;
        if (xmlDir != null) Directory.CreateDirectory(xmlDir);
        foreach (var line in File.ReadAllLines(listPath))
        {
            if (string.IsNullOrWhiteSpace(line)) continue;
            var parts = line.Split('|');
            total++;
            try
            {
                var a = Describe(parts[0], out var xa);
                var b = Describe(parts[1], out var xb);
                bool sameSummary = a == b;
                bool sameXml = xa == xb;
                if (!sameSummary || !sameXml)
                {
                    bad++;
                    Console.WriteLine($"DIFF {Path.GetFileName(parts[0])}: summary {(sameSummary ? "same" : "DIFFERENT")}, xml {(sameXml ? "same" : "DIFFERENT")}");
                    if (xmlDir != null)
                    {
                        var stem = Path.GetFileName(parts[0]);
                        File.WriteAllText(Path.Combine(xmlDir, stem + ".orig.txt"), a + "\n----\n" + xa);
                        File.WriteAllText(Path.Combine(xmlDir, stem + ".written.txt"), b + "\n----\n" + xb);
                    }
                    else FirstDiff(a == b ? xa : a, a == b ? xb : b);
                }
                else Console.WriteLine($"same {Path.GetFileName(parts[0])}  ({a.Split('\n')[0]})");
            }
            catch (Exception ex)
            {
                bad++;
                Console.WriteLine($"FAIL {Path.GetFileName(parts[0])}: {ex.GetType().Name}: {ex.Message}");
            }
        }
        Console.WriteLine($"{total} pairs, {bad} different");
        return bad == 0 ? 0 : 1;
    }

    static void FirstDiff(string a, string b)
    {
        var la = a.Split('\n'); var lb = b.Split('\n');
        for (int i = 0; i < Math.Max(la.Length, lb.Length); i++)
        {
            var x = i < la.Length ? la[i] : "<end>"; var y = i < lb.Length ? lb[i] : "<end>";
            if (x != y) { Console.WriteLine($"     line {i}: '{x.Trim()}' vs '{y.Trim()}'"); return; }
        }
    }

    public static string Describe(string path, out string xml)
    {
        var data = File.ReadAllBytes(path);
        var sb = new StringBuilder();
        var lower = path.ToLowerInvariant();
        if (lower.EndsWith(".ytyp"))
        {
            var ytyp = RpfFile.GetResourceFile<YtypFile>(data);
            if (ytyp.Meta == null) throw new Exception("not a meta ytyp");
            sb.AppendLine($"ytyp archetypes {ytyp.AllArchetypes?.Length ?? 0} ext {ytyp.Extensions?.Length ?? 0} composite {ytyp.CompositeEntityTypes?.Length ?? 0} name {ytyp.NameHash:X8}");
            foreach (var arch in ytyp.AllArchetypes ?? Array.Empty<Archetype>())
            {
                var d = arch._BaseArchetypeDef;
                sb.AppendLine($" {arch.Type} {d.name.Hash:X8} asset {d.assetName.Hash:X8} txd {d.textureDictionary.Hash:X8} bb {d.bbMin}..{d.bbMax} lod {d.lodDist} flags {d.flags} ext {arch.Extensions?.Length ?? 0}");
                if (arch is MloArchetype mlo)
                {
                    sb.AppendLine($"  mlo flags {mlo._MloArchetypeDefData.mloFlags} rooms {mlo.rooms?.Length ?? 0} portals {mlo.portals?.Length ?? 0} entities {mlo.entities?.Length ?? 0} sets {mlo.entitySets?.Length ?? 0} tcm {mlo.timeCycleModifiers?.Length ?? 0}");
                    foreach (var r in mlo.rooms ?? new MCMloRoomDef[0]) sb.AppendLine($"  room '{r.RoomName}' portals {r._Data.portalCount} bb {r._Data.bbMin}..{r._Data.bbMax} attached {r.AttachedObjects?.Length ?? 0} floor {r._Data.floorId}");
                    foreach (var p in mlo.portals ?? new MCMloPortalDef[0]) sb.AppendLine($"  portal {p._Data.roomFrom}->{p._Data.roomTo} flags {p._Data.flags} corners {p.Corners?.Length ?? 0} attached {p.AttachedObjects?.Length ?? 0}");
                    foreach (var e in mlo.entities ?? new MCEntityDef[0]) sb.AppendLine($"  entity {e._Data.archetypeName.Hash:X8} {e._Data.position} {e._Data.rotation} guid {e._Data.guid} ext {e.Extensions?.Length ?? 0}");
                    foreach (var s in mlo.entitySets ?? new MCMloEntitySet[0]) sb.AppendLine($"  set {s._Data.name.Hash:X8} entities {s.Entities?.Length ?? 0} locations {s.Locations?.Length ?? 0}");
                }
            }
            xml = MetaXml.GetXml(ytyp.Meta);
        }
        else if (lower.EndsWith(".ymap"))
        {
            var ymap = RpfFile.GetResourceFile<YmapFile>(data);
            if (ymap.Meta == null) throw new Exception("not a meta ymap");
            var m = ymap._CMapData;
            sb.AppendLine($"ymap {m.name.Hash:X8} parent {m.parent.Hash:X8} entities {ymap.AllEntities?.Length ?? 0} mlo {ymap.MloEntities?.Length ?? 0} cargens {ymap.CarGenerators?.Length ?? 0} box {ymap.BoxOccluders?.Length ?? 0} occmodels {ymap.OccludeModels?.Length ?? 0} tcm {ymap.TimeCycleModifiers?.Length ?? 0} physdicts {ymap.physicsDictionaries?.Length ?? 0} lodlights {ymap.LODLights?.direction?.Length ?? 0} distant {ymap.DistantLODLights?.positions?.Length ?? 0} grass {ymap.GrassInstanceBatches?.Length ?? 0}");
            foreach (var e in ymap.AllEntities ?? Array.Empty<YmapEntityDef>())
                sb.AppendLine($" entity {e._CEntityDef.archetypeName.Hash:X8} {e.Position} lod {e._CEntityDef.lodLevel} parent {e._CEntityDef.parentIndex} guid {e._CEntityDef.guid} ext {e.Extensions?.Length ?? 0}");
            foreach (var c in ymap.CarGenerators ?? Array.Empty<YmapCarGen>()) sb.AppendLine($" cargen {c.CCarGen.carModel.Hash:X8} {c.CCarGen.position}");
            xml = MetaXml.GetXml(ymap.Meta);
        }
        else throw new Exception("unsupported file type " + path);
        return sb.ToString();
    }
}
