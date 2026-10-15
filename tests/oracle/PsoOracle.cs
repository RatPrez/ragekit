// oracle pso <list.txt> [outDir]: each line is "original|written" (PSO files, e.g. binary .ymt).
// Both are loaded with CodeWalker's PsoFile and converted to its XML (PsoXml.GetXml); the two must be
// identical. With outDir the original's XML is written there as <name>.pso.xml, which tools/psocheck.py
// compares with ragekit's own walker (rk psodump --flat).
using CodeWalker.GameFiles;

static class PsoOracle
{
    public static string Xml(byte[] data)
    {
        var pso = new PsoFile();
        pso.Load(data);
        return PsoXml.GetXml(pso);
    }

    public static int Run(string listPath, string outDir)
    {
        int bad = 0, total = 0;
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
                var xa = Xml(da);
                var xb = Xml(db);
                if (outDir != null) File.WriteAllText(Path.Combine(outDir, Path.GetFileName(parts[0]) + ".pso.xml"), xa);
                bool sameBytes = da.AsSpan().SequenceEqual(db);
                if (xa != xb) { bad++; Console.WriteLine($"DIFF {Path.GetFileName(parts[0])}: CodeWalker XML differs (bytes {(sameBytes ? "same" : "differ")})"); }
                else Console.WriteLine($"same {Path.GetFileName(parts[0])}  xml {xa.Length} chars, bytes {(sameBytes ? "identical" : "differ")}");
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

    // oracle ymf <file>...: loads a streaming manifest with CodeWalker's YmfFile and prints what it sees, then its PsoXml
    public static int Ymf(string[] files, bool xml)
    {
        foreach (var path in files)
        {
            var data = File.ReadAllBytes(path);
            var ymf = new YmfFile();
            ymf.Load(data, null);
            if (ymf.Pso == null) { Console.WriteLine($"{Path.GetFileName(path)}: not a PSO manifest"); continue; }
            Console.WriteLine($"{Path.GetFileName(path)}: groups {ymf.MapDataGroups?.Length ?? 0} hdtxd {ymf.HDTxdAssetBindings?.Length ?? 0} imapdeps {ymf.imapDependencies?.Length ?? 0} imapdeps2 {ymf.imapDependencies2?.Length ?? 0} itypdeps2 {ymf.itypDependencies2?.Length ?? 0} interiors {ymf.Interiors?.Length ?? 0}");
            foreach (var d in ymf.imapDependencies2 ?? Array.Empty<YmfImapDependency2>())
                Console.WriteLine($"  imapDep {d.Dep.imapName.Hash:X8} flags {d.Dep.manifestFlags} -> [{string.Join(",", (d.itypDepArray ?? Array.Empty<MetaHash>()).Select(h => h.Hash.ToString("X8")))}]");
            foreach (var d in ymf.itypDependencies2 ?? Array.Empty<YmfItypDependency2>())
                Console.WriteLine($"  itypDep {d.Dep.itypName.Hash:X8} flags {d.Dep.manifestFlags} -> [{string.Join(",", (d.itypDepArray ?? Array.Empty<MetaHash>()).Select(h => h.Hash.ToString("X8")))}]");
            foreach (var d in ymf.Interiors ?? Array.Empty<YmfInterior>())
                Console.WriteLine($"  interior {d.Interior.Name.Hash:X8} -> [{string.Join(",", (d.Bounds ?? Array.Empty<MetaHash>()).Select(h => h.Hash.ToString("X8")))}]");
            if (xml) Console.Write(PsoXml.GetXml(ymf.Pso));
        }
        return 0;
    }
}
