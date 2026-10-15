// oracle rel <list.txt> [outDir]: each line is "original|written" (.rel files). Both are loaded with CodeWalker's
// RelFile and converted to its XML (RelXml.GetXml); the two must be identical. With outDir the original's XML is
// written there as <name>.xml. Also reports whether CodeWalker's own Save() of the original gives the same bytes.
// oracle relxml <in.xml> <out.rel>: compile a Dat151 / Dat54 ... XML with CodeWalker (XmlRel + Save)
// oracle psoxml <in.pso.xml> <out.ymt>: compile PSO XML with CodeWalker (XmlPso + Save)
using System.Xml;
using CodeWalker.GameFiles;

static class RelOracle
{
    public static string Xml(byte[] data, out RelFile rel)
    {
        rel = new RelFile();
        rel.Load(data, null);
        return RelXml.GetXml(rel);
    }

    public static int Run(string listPath, string outDir)
    {
        int bad = 0, total = 0, cwSame = 0, cwTried = 0;
        var perType = new SortedDictionary<uint, int[]>();
        if (outDir != null) Directory.CreateDirectory(outDir);
        foreach (var line in File.ReadAllLines(listPath))
        {
            if (string.IsNullOrWhiteSpace(line)) continue;
            var parts = line.Split('|');
            total++;
            try
            {
                var da = File.ReadAllBytes(parts[0]);
                var db = File.ReadAllBytes(parts[1]);
                var xa = Xml(da, out var ra);
                var xb = Xml(db, out var rb);
                if (!perType.TryGetValue((uint)ra.RelType, out var c)) perType[(uint)ra.RelType] = c = new int[3];
                c[0]++;
                if (outDir != null) File.WriteAllText(Path.Combine(outDir, Path.GetFileName(parts[0]) + ".xml"), xa);
                bool sameBytes = da.AsSpan().SequenceEqual(db);
                if (xa != xb) { bad++; c[2]++; Console.WriteLine($"DIFF {Path.GetFileName(parts[0])}: CodeWalker XML differs (bytes {(sameBytes ? "same" : "differ")})"); }
                else
                {
                    c[1]++;
                    Console.WriteLine($"same {Path.GetFileName(parts[0])}  xml {xa.Length} chars, bytes {(sameBytes ? "identical" : "differ")}, {ra.RelDatas?.Length ?? 0} items");
                }
                try
                {
                    cwTried++;
                    var saved = ra.Save();
                    if (saved != null && saved.AsSpan().SequenceEqual(da)) cwSame++;
                }
                catch { }
            }
            catch (Exception ex)
            {
                bad++;
                Console.WriteLine($"FAIL {Path.GetFileName(parts[0])}: {ex.GetType().Name}: {ex.Message}");
            }
        }
        foreach (var kv in perType) Console.WriteLine($"dat{kv.Key}: {kv.Value[0]} pairs, {kv.Value[1]} same XML, {kv.Value[2]} different");
        Console.WriteLine($"{total} pairs, {bad} different; CodeWalker's own Save() of the original equals it for {cwSame} of {cwTried}");
        return bad == 0 ? 0 : 1;
    }

    public static int FromXml(string xmlPath, string outPath)
    {
        var doc = new XmlDocument();
        doc.Load(xmlPath);
        var rel = XmlRel.GetRel(doc);
        var bytes = rel.Save();
        File.WriteAllBytes(outPath, bytes);
        Console.WriteLine($"ok   {Path.GetFileName(outPath)} written ({bytes.Length} bytes), {rel.RelDatas?.Length ?? 0} items");
        return 0;
    }

    public static int PsoFromXml(string xmlPath, string outPath)
    {
        var doc = new XmlDocument();
        doc.Load(xmlPath);
        var pso = XmlPso.GetPso(doc);
        var bytes = pso.Save();
        File.WriteAllBytes(outPath, bytes);
        Console.WriteLine($"ok   {Path.GetFileName(outPath)} written ({bytes.Length} bytes)");
        return 0;
    }
}
