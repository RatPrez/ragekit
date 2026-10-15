// oracle world <gameFolder> <out.tsv>
//
// The map files CodeWalker's GameFileCache loads with every DLC pack (its ActiveMapRpfFiles: base map archives,
// DLC overlays and map change sets): one line per .ymap / .ybn it keeps, "ymap|ybn <tab> lower-case name <tab> path".
// Compared with ragekit's WorldIndex by `rk_world_test --oracle <out.tsv>`. CodeWalker's own rules differ from the
// game's in known ways (it applies every change set with map data whatever its group, mounts every RPF data file
// whether or not it is disabled, and skips patchday27ng), so the comparison lists differences rather than failing.
using System.Diagnostics;
using CodeWalker.GameFiles;

static class WorldOracle
{
    public static int Run(string[] args)
    {
        string game = args[1], outPath = args[2];
        var sw = Stopwatch.StartNew();
        GTA5Keys.LoadFromPath(game, false, null);
        var gfc = new GameFileCache(2L << 30, 10, game, false, "__every_pack__", false, "");
        gfc.LoadArchetypes = false;
        gfc.BuildExtendedJenkIndex = false;
        int errs = 0;
        gfc.Init(s => { }, e => { if (errs++ < 5) Console.Error.WriteLine("codewalker: " + e); });
        Console.WriteLine($"CodeWalker GameFileCache.Init {sw.Elapsed.TotalSeconds:F1} s, {errs} error messages");
        using var w = new StreamWriter(outPath);
        int ny = 0, nb = 0;
        foreach (var kv in gfc.YmapDict) { w.WriteLine($"ymap\t{kv.Value.NameLower}\t{kv.Value.Path.Replace('\\', '/')}"); ny++; }
        foreach (var kv in gfc.YbnDict) { w.WriteLine($"ybn\t{kv.Value.NameLower}\t{kv.Value.Path.Replace('\\', '/')}"); nb++; }
        Console.WriteLine($"{ny} ymaps, {nb} ybns -> {outPath}");
        return 0;
    }
}
