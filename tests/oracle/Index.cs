// oracle index <gameFolder> <rk-dump.tsv> [--seed N] [--ytyps N] [--names N]
//
// Compares ragekit's game index (`rk index dump <game> <rk-dump.tsv>`) with CodeWalker's GameFileCache, initialised
// headless the way the game's DLC list is: every archive, every ytyp, the global ydr/ydd/ytd/yft dictionaries.
//   * file counts (total and per extension)
//   * archetype count of every .ytyp (and the N random ones listed)
//   * for every archetype name and for N random ones: the same ytyp / lod / txd / dictionary fields, and the drawable
//     and texture dictionary file CodeWalker picks (GetYddEntry / GetYdrEntry / GetYftEntry / GetYtdEntry) against
//     the file ragekit resolves (same archive and entry path).
// CodeWalker's dictionaries keep the LAST copy in scan order; ragekit keeps the copy loaded last by the game
// (dlclist.xml / setup2 order), so a different pick for a name that lives in several archives is expected and is
// counted separately ("order differences") when ragekit also knows the file CodeWalker picked.
using System.Diagnostics;
using CodeWalker.GameFiles;

static class IndexOracle
{
    const string TAB = "\t";
    static string N(string p) => p.Replace('\\', '/').ToLowerInvariant();

    class Row { public string Kind, Ytyp, Draw, Txd; public string Lod; public uint Txd0, DDict, Asset; public int AType; }

    public static int Run(string[] args)
    {
        string game = args[1], tsv = args[2];
        int seed = 12345, nY = 50, nA = 200;
        for (int i = 3; i + 1 < args.Length; i += 2)
        {
            if (args[i] == "--seed") seed = int.Parse(args[i + 1]);
            else if (args[i] == "--ytyps") nY = int.Parse(args[i + 1]);
            else if (args[i] == "--names") nA = int.Parse(args[i + 1]);
        }

        // ---- ragekit's side
        var rkY = new Dictionary<string, int>();
        var rkA = new Dictionary<uint, Row>();
        var rkExt = new Dictionary<string, int>();
        int rkFiles = 0;
        foreach (var line in File.ReadLines(tsv))
        {
            var c = line.Split('\t');
            if (c[0] == "Y") rkY[c[1]] = int.Parse(c[2]);
            else if (c[0] == "I") rkFiles = int.Parse(c[2]);
            else if (c[0] == "E") rkExt[c[1]] = int.Parse(c[2]);
            else if (c[0] == "A")
                rkA[Convert.ToUInt32(c[1], 16)] = new Row
                {
                    Kind = c[2], Lod = c[3], Ytyp = c[4], Txd0 = Convert.ToUInt32(c[5], 16), DDict = Convert.ToUInt32(c[6], 16),
                    Asset = Convert.ToUInt32(c[7], 16), AType = int.Parse(c[8]), Draw = c[9], Txd = c[10],
                };
        }
        Console.WriteLine($"ragekit: {rkFiles} files, {rkY.Count} ytyps, {rkA.Count} archetype names");

        // ---- CodeWalker's side
        var sw = Stopwatch.StartNew();
        GTA5Keys.LoadFromPath(game, false, null);
        var gfc = new GameFileCache(2L << 30, 10, game, false, "patchday28ng", false, "");
        gfc.BuildExtendedJenkIndex = false;
        int errs = 0;
        gfc.Init(s => { }, e => { if (errs++ < 5) Console.Error.WriteLine("codewalker: " + e); });
        Console.WriteLine($"CodeWalker GameFileCache.Init {sw.Elapsed.TotalSeconds:F1} s, {errs} error messages, {gfc.AllRpfs.Count} archives");

        int bad = 0;
        // files
        var cwExt = new Dictionary<string, int>();
        int cwFiles = 0;
        foreach (var rpf in gfc.AllRpfs)
        {
            if (rpf.AllEntries == null) continue;
            foreach (var e in rpf.AllEntries)
            {
                if (!(e is RpfFileEntry)) continue;
                cwFiles++;
                int dot = e.NameLower.LastIndexOf('.');
                var ext = dot > 0 ? e.NameLower.Substring(dot) : "";
                cwExt[ext] = cwExt.GetValueOrDefault(ext) + 1;
            }
        }
        Console.WriteLine($"files: CodeWalker {cwFiles}, ragekit {rkFiles}" + (cwFiles == rkFiles ? "  (equal)" : "  DIFFERENT"));
        if (cwFiles != rkFiles) bad++;
        foreach (var kv in cwExt.OrderBy(k => k.Key))
            if (rkExt.GetValueOrDefault(kv.Key) != kv.Value) { Console.WriteLine($"  ext {kv.Key}: CodeWalker {kv.Value}, ragekit {rkExt.GetValueOrDefault(kv.Key)}"); bad++; }

        // ytyps: load each with CodeWalker, keep every archetype in scan order (last copy of a name wins, as in its dictionary)
        var cwY = new Dictionary<string, int>();
        var cwWinner = new Dictionary<uint, Archetype>();
        var cwAll = new List<Archetype>();
        int ytypErrors = 0;
        foreach (var rpf in gfc.AllRpfs)
        {
            if (rpf.AllEntries == null) continue;
            foreach (var e in rpf.AllEntries)
            {
                if (!e.NameLower.EndsWith(".ytyp")) continue;
                try
                {
                    var y = gfc.RpfMan.GetFile<YtypFile>(e);
                    if (y?.Meta == null) { ytypErrors++; continue; }
                    cwY[N(e.Path)] = y.AllArchetypes?.Length ?? 0;
                    foreach (var a in y.AllArchetypes ?? Array.Empty<Archetype>())
                    {
                        cwAll.Add(a);
                        if (a._BaseArchetypeDef.name.Hash != 0) cwWinner[a._BaseArchetypeDef.name.Hash] = a;
                    }
                }
                catch { ytypErrors++; }
            }
        }
        Console.WriteLine($"ytyp files: CodeWalker {cwY.Count} ({ytypErrors} unreadable), ragekit {rkY.Count}; archetypes {cwAll.Count} vs {rkY.Values.Sum()}; distinct names {cwWinner.Count} vs {rkA.Count}");
        if (cwAll.Count != rkY.Values.Sum() || cwWinner.Count != rkA.Count) bad++;
        int ydiff = 0, yonly = 0;
        foreach (var kv in cwY)
            if (!rkY.TryGetValue(kv.Key, out var n) || n != kv.Value) { if (ydiff++ < 10) Console.WriteLine($"  ytyp {kv.Key}: CodeWalker {kv.Value}, ragekit {(rkY.TryGetValue(kv.Key, out var m) ? m.ToString() : "absent")}"); }
        foreach (var k in rkY.Keys) if (!cwY.ContainsKey(k)) { yonly++; Console.WriteLine($"  ytyp {k}: only in ragekit ({rkY[k]} archetypes; a PSO file CodeWalker cannot read)"); }
        Console.WriteLine($"per-ytyp archetype counts: {cwY.Count - ydiff} of {cwY.Count} equal ({ydiff} different); {yonly} ytyp files only ragekit lists");
        if (ydiff > 0) bad++;

        var rnd = new Random(seed);
        Console.WriteLine($"-- {nY} random ytyps (seed {seed}):");
        int ysame = 0;
        var keys = rkY.Keys.OrderBy(k => k, StringComparer.Ordinal).ToList();
        for (int i = 0; i < nY && keys.Count > 0; i++)
        {
            var k = keys[rnd.Next(keys.Count)];
            bool same = cwY.TryGetValue(k, out var cwn) && cwn == rkY[k];
            if (same) ysame++;
            Console.WriteLine($"   {(same ? "same" : "DIFF")}  {rkY[k],6} {cwY.GetValueOrDefault(k),6}  {k}");
        }
        Console.WriteLine($"   {ysame} of {nY} equal");
        if (ysame != nY) bad++;

        // ---- archetypes
        string Pick(RpfFileEntry e, string ext) => e == null ? null : ext + ":" + N(e.Path);
        string CwDraw(Archetype a)
        {
            if (a.DrawableDict != 0)
            {
                var y = gfc.GetYddEntry(a.DrawableDict);
                if (y != null) return Pick(y, "ydd");
            }
            var d = gfc.GetYdrEntry(a.Hash);
            if (d != null) return Pick(d, "ydr");
            var f = gfc.GetYftEntry(a.Hash);
            return f != null ? Pick(f, "yft") : "-";
        }
        string CwTxd(Archetype a)
        {
            if (a.TextureDict == 0) return "-";
            var t = gfc.GetYtdEntry(a.TextureDict);
            return t == null ? "-" : N(t.Path);
        }
        string Kind(Archetype a) => a is MloArchetype ? "mlo" : a is TimeArchetype ? "time" : "base";

        int total = 0, fieldBad = 0, ytypOther = 0, drawSame = 0, drawDiff = 0, txdSame = 0, txdDiff = 0, missing = 0, oldFormat = 0, oldCwHas = 0;
        var diffs = new List<string>();
        var explain = new List<string>();
        var wins = new Dictionary<uint, string>(); // name -> "same" | "ytyp" | "old" | "diff", for the sample
        foreach (var kv in cwWinner)
        {
            total++;
            if (!rkA.TryGetValue(kv.Key, out var r)) { missing++; wins[kv.Key] = "missing"; if (missing <= 10) Console.WriteLine($"  archetype {kv.Key:X8} missing in ragekit"); continue; }
            var a = kv.Value;
            var d = a._BaseArchetypeDef;
            var cwYtyp = N(a.Ytyp.RpfFileEntry.Path);
            if (r.Ytyp != cwYtyp)
            {
                // the same name defined in two ytyps: CodeWalker keeps the one scanned last, ragekit the one loaded last by the game.
                ytypOther++; wins[kv.Key] = "ytyp";
                explain.Add($"ytyp{TAB}{cwYtyp}{TAB}{r.Ytyp}");
                continue;
            }
            bool old = r.AType == 0; // 128 byte CBaseArchetypeDef without asset fields: CodeWalker reads garbage past the structure
            if (r.Kind != Kind(a) || r.Txd0 != d.textureDictionary.Hash || r.DDict != d.drawableDictionary.Hash || (!old && (r.Asset != d.assetName.Hash || r.AType != (int)d.assetType))
                || r.Lod != d.lodDist.ToString("R", System.Globalization.CultureInfo.InvariantCulture))
            { fieldBad++; wins[kv.Key] = "diff"; if (fieldBad <= 10) Console.WriteLine($"  fields differ for {kv.Key:X8}: {r.Kind}/{Kind(a)} txd {r.Txd0:X8}/{d.textureDictionary.Hash:X8} lod {r.Lod}/{d.lodDist}"); }
            var cd = CwDraw(a);
            var ct = CwTxd(a);
            if (old)
            {
                oldFormat++; wins[kv.Key] = "old";
                if (cd != "-") oldCwHas++;
                if (ct != r.Txd) { txdDiff++; diffs.Add($"txd  {kv.Key:X8}  CodeWalker {ct}  ragekit {r.Txd}"); explain.Add($"txd{TAB}{ct}{TAB}{r.Txd}"); } else txdSame++;
                continue;
            }
            bool ok = true;
            if (cd == r.Draw) drawSame++; else { ok = false; drawDiff++; diffs.Add($"draw {kv.Key:X8}  CodeWalker {cd}  ragekit {r.Draw}"); explain.Add($"draw{TAB}{cd}{TAB}{r.Draw}"); }
            if (ct == r.Txd) txdSame++; else { ok = false; txdDiff++; diffs.Add($"txd  {kv.Key:X8}  CodeWalker {ct}  ragekit {r.Txd}"); explain.Add($"txd{TAB}{ct}{TAB}{r.Txd}"); }
            if (!wins.ContainsKey(kv.Key)) wins[kv.Key] = ok ? "same" : "load order";
        }
        Console.WriteLine($"all {total} archetype names: {missing} missing in ragekit; field differences {fieldBad} among the {total - ytypOther} names both tools read from the same ytyp;");
        Console.WriteLine($"   {ytypOther} names are defined in several ytyps and the two tools pick different ones (load order, below); {oldFormat} names have the older 128 byte structure (CodeWalker finds a drawable for {oldCwHas} of them)");
        Console.WriteLine($"   drawable file: {drawSame} same, {drawDiff} different;  texture dictionary file: {txdSame} same, {txdDiff} different (same-ytyp names with the new structure)");
        if (missing > 0 || fieldBad > 0) bad++;
        foreach (var l in diffs.Take(5)) Console.WriteLine("   " + l);
        File.WriteAllLines(tsv + ".diffs", explain.Distinct());
        Console.WriteLine($"   the {explain.Distinct().Count()} distinct differing picks are in {tsv}.diffs: `rk index explain` says why");

        // the random sample, drawn from the names whose definition both tools read from the same ytyp (the others are the load order differences above)
        Console.WriteLine($"-- {nA} random archetype names (seed {seed}):");
        var names = cwWinner.Keys.Where(k => wins[k] != "ytyp").OrderBy(x => x).ToList();
        int sDraw = 0, sTxd = 0, sFields = 0, sCmp = 0, sSkip = 0;
        for (int i = 0; i < nA; i++)
        {
            var h = names[rnd.Next(names.Count)];
            var a = cwWinner[h];
            var r = rkA[h];
            var d = a._BaseArchetypeDef;
            if (wins[h] == "ytyp") { sSkip++; Console.WriteLine($"   {h:X8} {JenkIndex.TryGetString(h)}: defined in several ytyps, tools pick different ones (see below)"); continue; }
            sCmp++;
            bool old = r.AType == 0;
            bool draw = old || CwDraw(a) == r.Draw, txd = CwTxd(a) == r.Txd;
            bool fields = r.Kind == Kind(a) && r.Txd0 == d.textureDictionary.Hash && r.DDict == d.drawableDictionary.Hash && (old || r.Asset == d.assetName.Hash);
            if (draw) sDraw++; if (txd) sTxd++; if (fields) sFields++;
            if (!draw || !txd || !fields)
                Console.WriteLine($"   {h:X8} {JenkIndex.TryGetString(h)}: draw {(draw ? "same" : "CW " + CwDraw(a) + " | RK " + r.Draw)}  txd {(txd ? "same" : "CW " + CwTxd(a) + " | RK " + r.Txd)}  fields {(fields ? "same" : "DIFF")}");
        }
        Console.WriteLine($"   compared {sCmp} ({sSkip} skipped, different ytyp): drawable file same {sDraw}/{sCmp}, texture dictionary file same {sTxd}/{sCmp}, archetype fields same {sFields}/{sCmp}");
        if (sDraw != sCmp || sTxd != sCmp || sFields != sCmp) bad++;
        return bad == 0 ? 0 : 1;
    }
}
