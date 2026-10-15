// oracle rpf <gameFolder> <file.rpf> [--hash] [--nested]: lists every entry of an archive with CodeWalker, in the
// same tab separated layout as `rk rpf ls -r --tsv [--hash] --nested`, to compare the two byte for byte.
//   kind  path  size  stored  uncompressed  system flags  graphics flags  encrypted  [sha256 of ExtractFile]
using System.Security.Cryptography;
using CodeWalker.GameFiles;

static class RpfDump
{
    public static int Run(string[] args)
    {
        string game = args[1], file = args[2];
        bool hash = args.Contains("--hash"), nested = args.Contains("--nested");
        GTA5Keys.LoadFromPath(game, false, null);
        var rpf = new RpfFile(file, "");
        int errors = 0;
        rpf.ScanStructure(s => { }, e => { Console.Error.WriteLine("scan: " + e); errors++; });
        if (rpf.AllEntries == null) { Console.Error.WriteLine("could not read " + file + ": " + rpf.LastError); return 1; }
        var stdout = new StreamWriter(Console.OpenStandardOutput()) { NewLine = "\n" };
        Dump(rpf, nested, hash, stdout);
        stdout.Flush();
        return errors == 0 ? 0 : 1;
    }

    /// oracle rpfkeys <gameFolder> <outFolder>: CodeWalker's own key material, written in the cache file layout.
    public static int Keys(string[] args)
    {
        GTA5Keys.LoadFromPath(args[1], false, null);
        Directory.CreateDirectory(args[2]);
        File.WriteAllBytes(Path.Combine(args[2], "gtav_aes_key.dat"), GTA5Keys.PC_AES_KEY);
        CryptoIO.WriteNgKeys(Path.Combine(args[2], "gtav_ng_key.dat"), GTA5Keys.PC_NG_KEYS);
        CryptoIO.WriteNgTables(Path.Combine(args[2], "gtav_ng_decrypt_tables.dat"), GTA5Keys.PC_NG_DECRYPT_TABLES);
        File.WriteAllBytes(Path.Combine(args[2], "gtav_hash_lut.dat"), GTA5Keys.PC_LUT);
        return 0;
    }

    static string Norm(string p) => p.Replace('\\', '/').TrimStart('/');

    static void Dump(RpfFile rpf, bool nested, bool hash, TextWriter o)
    {
        foreach (var e in rpf.AllEntries.Skip(1)) // skip the root
        {
            string kind, size, stored = "0", unc = "0", sys = "0", gfx = "0";
            int enc = 0;
            if (e is RpfDirectoryEntry) { kind = "D"; size = "0"; }
            else if (e is RpfBinaryFileEntry b)
            {
                kind = "B"; stored = b.FileSize.ToString(); unc = b.FileUncompressedSize.ToString();
                size = (b.FileUncompressedSize != 0 ? b.FileUncompressedSize : b.FileSize).ToString();
                enc = b.IsEncrypted ? 1 : 0;
            }
            else
            {
                var r = (RpfResourceFileEntry)e;
                kind = "R"; stored = r.FileSize.ToString();
                sys = r.SystemFlags.Value.ToString(); gfx = r.GraphicsFlags.Value.ToString();
                size = ((long)r.SystemSize + r.GraphicsSize).ToString();
                enc = r.IsEncrypted ? 1 : 0;
            }
            string line = $"{kind}\t{Norm(e.Path)}\t{size}\t{stored}\t{unc}\t{sys}\t{gfx}\t{enc}";
            if (hash)
            {
                string h = "-";
                if (e is RpfFileEntry fe)
                {
                    byte[] data = fe.File.ExtractFile(fe);
                    h = data == null || data.Length == 0 ? "empty" : Convert.ToHexString(SHA256.HashData(data)).ToLowerInvariant();
                }
                line += "\t" + h;
            }
            o.WriteLine(line);
        }
        if (nested && rpf.Children != null) foreach (var c in rpf.Children) Dump(c, nested, hash, o);
    }
}
