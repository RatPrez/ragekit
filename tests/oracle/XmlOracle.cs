// oracle cwxml <file.ydr|ydd|ybn|ytd> <out folder>: CodeWalker's own "Export XML" (the XML plus the DDS folder).
// oracle xmlin <file.xml> <out binary> [dds folder]: CodeWalker's XML importer (XmlYdr/XmlYdd/XmlYbn/XmlYtd) + Save().
// oracle cwxmllist <list> <out> / xmlinlist <list> <out>: the same for many files (see ExportList / ImportList).
// oracle xmlnames <out.inc>: dumps the ShaderParamNames table (hash -> name as Enum.ToString prints it).
// These check ragekit's xml.h: its XML must equal CodeWalker's text, and CodeWalker must import it.
using System.Text;
using CodeWalker.GameFiles;

static class XmlOracle
{
    static bool seeded;
    // CodeWalker prints shader names / .sps names through JenkIndex, which a game scan normally fills.
    static void Seed()
    {
        if (seeded) return;
        seeded = true;
        var p = Path.Combine(AppContext.BaseDirectory, "ShadersGen9Conversion.xml");
        if (!File.Exists(p)) return;
        var doc = new System.Xml.XmlDocument();
        doc.Load(p);
        foreach (System.Xml.XmlNode n in doc.SelectNodes("/ShadersGen9Conversion/Item"))
        {
            var a = n.SelectSingleNode("Name")?.InnerText;
            var b = n.SelectSingleNode("FileName")?.InnerText;
            if (!string.IsNullOrEmpty(a)) JenkIndex.Ensure(a);
            if (!string.IsNullOrEmpty(b)) JenkIndex.Ensure(b);
        }
    }

    public static int Export(string[] args)
    {
        Seed();
        var path = args[1];
        var outDir = args[2];
        Directory.CreateDirectory(outDir);
        var fn = Path.GetFileName(path);
        var stem = Path.GetFileNameWithoutExtension(path);
        var folder = Path.Combine(outDir, stem);
        var data = File.ReadAllBytes(path);
        var lower = fn.ToLowerInvariant();
        string xml;
        if (lower.EndsWith(".ydr"))
        {
            var f = RpfFile.GetResourceFile<YdrFile>(data); f.Name = fn;
            xml = YdrXml.GetXml(f, folder);
        }
        else if (lower.EndsWith(".ydd"))
        {
            var f = new YddFile();
            f.Load(data); f.Name = fn;
            xml = YddXml.GetXml(f, folder);
        }
        else if (lower.EndsWith(".ytd"))
        {
            var f = RpfFile.GetResourceFile<YtdFile>(data); f.Name = fn;
            xml = YtdXml.GetXml(f, folder);
        }
        else if (lower.EndsWith(".ybn"))
        {
            var f = RpfFile.GetResourceFile<YbnFile>(data);
            xml = YbnXml.GetXml(f);
        }
        else { Console.WriteLine("unsupported " + fn); return 1; }
        File.WriteAllText(Path.Combine(outDir, fn + ".xml"), xml, new UTF8Encoding(false));
        return 0;
    }

    // oracle cwxmllist <list.txt> <out folder>: cwxml for every file named in the list (one path per line)
    public static int ExportList(string[] args)
    {
        int bad = 0, n = 0;
        foreach (var line in File.ReadAllLines(args[1]))
        {
            if (string.IsNullOrWhiteSpace(line)) continue;
            n++;
            try { if (Export(new[] { "cwxml", line.Trim(), args[2] }) != 0) bad++; }
            catch (Exception ex) { bad++; Console.WriteLine($"FAIL {Path.GetFileName(line)}: {ex.GetType().Name}: {ex.Message}"); }
        }
        Console.WriteLine($"{n} files, {bad} failed");
        return bad == 0 ? 0 : 1;
    }

    // oracle xmlinlist <list.txt> <out folder>: xmlin for each "<file.xml>|<dds folder>" line; the binary is <out>/<xml name minus .xml>
    public static int ImportList(string[] args)
    {
        int bad = 0, n = 0;
        Directory.CreateDirectory(args[2]);
        foreach (var line in File.ReadAllLines(args[1]))
        {
            if (string.IsNullOrWhiteSpace(line)) continue;
            n++;
            var parts = line.Split('|');
            var xml = parts[0].Trim();
            var outName = Path.GetFileName(xml); outName = outName.Substring(0, outName.Length - 4);
            try { if (Import(new[] { "xmlin", xml, Path.Combine(args[2], outName), parts.Length > 1 ? parts[1].Trim() : "" }) != 0) bad++; }
            catch (Exception ex) { bad++; Console.WriteLine($"FAIL {Path.GetFileName(xml)}: {ex.GetType().Name}: {ex.Message}"); }
        }
        Console.WriteLine($"{n} files, {bad} failed");
        return bad == 0 ? 0 : 1;
    }

    public static int Import(string[] args)
    {
        Seed();
        var xmlPath = args[1];
        var outPath = args[2];
        var folder = args.Length > 3 ? args[3] : "";
        var xml = File.ReadAllText(xmlPath);
        var lower = xmlPath.ToLowerInvariant();
        byte[] bytes;
        if (lower.EndsWith(".ydr.xml")) bytes = XmlYdr.GetYdr(xml, folder).Save();
        else if (lower.EndsWith(".ydd.xml")) bytes = XmlYdd.GetYdd(xml, folder).Save();
        else if (lower.EndsWith(".ytd.xml")) bytes = XmlYtd.GetYtd(xml, folder).Save();
        else if (lower.EndsWith(".ybn.xml")) bytes = XmlYbn.GetYbn(xml).Save();
        else { Console.WriteLine("unsupported " + xmlPath); return 1; }
        File.WriteAllBytes(outPath, bytes);
        return 0;
    }

    public static int Names(string[] args)
    {
        var sb = new StringBuilder();
        var seen = new HashSet<uint>();
        var vals = Enum.GetValues<ShaderParamNames>().Select(v => (uint)v).Distinct().OrderBy(v => v);
        foreach (var v in vals)
            sb.Append("{0x" + v.ToString("X8") + ", \"" + ((ShaderParamNames)v).ToString() + "\"},\n");
        File.WriteAllText(args[1], sb.ToString());
        return 0;
    }
}
