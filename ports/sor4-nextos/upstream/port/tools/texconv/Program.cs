// texconv — PRÉ-PROCESSADOR de texturas do SOR4 (offline/progressor).
// Converte cada .xnb ASTC -> ETC1 (opaca, Mali lê nativo, 8x leve) / RGBA8 (com alpha),
// aplica o TEXSCALE. Espelha EXATAMENTE a lógica do Texture2DReader.
// DOIS modos:
//   1) DIRETÓRIO (legado): texconv <gameassets_dir> [scale] [--dry]
//      -> converte os .xnb que JA estao no disco, no lugar.
//   2) APK set (fusao extrai+converte): texconv --apk <base> [--apk <split> ...]
//      -> monta assets/ de todos os APKs, rejeita colisoes e converte em uma passada.
//         UM unico 0->100% cobrindo tudo. Imprime "N/TOTAL  PCT%  caminho" por arquivo.
// Reusa Lz4DecoderStream do MonoGame + libsor4astc.so (sor4_astc_decode/sor4_etc1_encode).
using System;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using MonoGame.Framework.Utilities;
using Microsoft.Xna.Framework.Content.Pipeline.Utilities.LZ4;

static class TexConv {
    [DllImport("sor4astc")] static extern int sor4_astc_decode(byte[] data, ulong len, int w, int h, int bx, int by, byte[] outRGBA);
    [DllImport("sor4astc")] static extern int sor4_etc1_encode(byte[] rgba, int w, int h, byte[] outEtc1);
    [DllImport("sor4astc")] static extern int sor4_etc2_eac_rgba_encode(byte[] rgba, int w, int h, byte[] outEtc2);

    static readonly int[,] AstcBlk = {{4,4},{5,4},{5,5},{6,5},{6,6},{8,5},{8,6},{8,8},{10,5},{10,6},{10,8},{10,10},{12,10},{12,12}};
    const int SF_Color = 0;
    const int SF_Bgra4444 = 3;    // BGRA4444 16bpp (fonte/UI) -> reader hoje explode p/ RGBA8 32bpp
    const int SF_RgbEtc1 = 60;
    const int SF_Rgb8Etc2 = 90;   // GLES3: ETC2-RGB (4bpp, mesmo bitstream do ETC1)
    const int SF_Rgba8Etc2 = 94;  // GLES3: ETC2-EAC RGBA (8bpp, com alpha)
    const int ASTC_MIN = 96;
    const long MaxApkEntryBytes = 128L * 1024 * 1024;
    const int MaxDecodedXnbBytes = 128 * 1024 * 1024;
    const long MaxDecodedTextureBytes = 256L * 1024 * 1024;

    static double Scale = 2;
    static bool Dry = false;
    // MASKFIX DESLIGADO por padrao: o diagnostico provou que NENHUMA fonte depende dele
    // (os atlas gui/fonts sao COLORIDOS, renderizam normal) e que ele so EMBRANQUECIA por
    // engano sprites (animatedsprites: sombras/silhuetas) e cenario (decors: cercas/escadas).
    // Reative com --maskfix se algum texto sumir (nao deve: fontes validadas legiveis sem ele).
    static bool NoMaskFix = true;
    static bool Diag = false;
    // downscale tambem das texturas Color/RGBA8 FULL-SIZE (menus/fundos) que antes passavam batido.
    // ON por padrao (reduzir CMA). ColorMin = so toca Color com lado >= N (poupa fonte/icone pequeno).
    static bool DownColor = true;
    static int ColorMin = 512;
    // ETC2 (GLES3): opaca->ETC2-RGB(90), alpha->ETC2-EAC RGBA(94, 8bpp). Liga com --etc2 ou SOR4_ETC2=1.
    // O bake decide pela VERSAO GLES do device (GLES3=on, GLES2=off->ETC1 como hoje).
    static bool Etc2 = System.Environment.GetEnvironmentVariable("SOR4_ETC2")=="1";
    // RESUME crash-safe: arquivo de progresso (1 nome de entrada por linha). Re-rodar pula o que ja
    // foi feito (idempotente) -> sobrevive a reboot/power-loss (R36S reboota por bateria) sem zerar.
    static string ResumeFile = null;
    [ThreadStatic] static int LastReason;
    [ThreadStatic] static bool LastWhitened;

    static int Main(string[] args){
        var apks = new List<string>();
        var pos = new List<string>();
        for (int i=0;i<args.Length;i++){
            if (args[i]=="--apk" && i+1<args.Length) apks.Add(args[++i]);
            else if (args[i]=="--dry") Dry=true;
            else if (args[i]=="--nomaskfix") NoMaskFix=true;
            else if (args[i]=="--maskfix") NoMaskFix=false;   // reativa o MASKFIX (default = OFF)
            else if (args[i]=="--diag") Diag=true;
            else if (args[i]=="--nocolor") DownColor=false;            // NAO downscalar as Color full-size
            else if (args[i]=="--colormin" && i+1<args.Length) int.TryParse(args[++i], out ColorMin);
            else if (args[i]=="--etc2") Etc2=true;                     // GLES3: alpha em ETC2-EAC (8bpp)
            else if (args[i]=="--noetc2") Etc2=false;
            else if (args[i]=="--resume" && i+1<args.Length) ResumeFile=args[++i];  // resume crash-safe
            else if (double.TryParse(args[i], System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out double sc)) Scale = sc;
            else pos.Add(args[i]);
        }
        if (Scale<1) Scale=1; if (Scale>8) Scale=8;   // permite escala FRACIONARIA (ex 4.5) p/ devices de pouca CMA

        int nthreads = Math.Max(1, Environment.ProcessorCount);
        { var e=Environment.GetEnvironmentVariable("SOR4_CONV_THREADS"); int t;
          if(!string.IsNullOrEmpty(e) && int.TryParse(e,out t) && t>=1) nthreads=t; }

        if (!SelfTestLz4()){ Console.Error.WriteLine("[texconv] AUTO-TESTE LZ4 FALHOU — abortando."); return 3; }

        if (apks.Count > 0){
            if (pos.Count < 1){ Console.Error.WriteLine("uso: texconv --apk <apk> [--apk <split> ...] <gameassets_dir> [scale]"); return 2; }
            return RunFromApks(apks, pos[0], nthreads);
        }
        if (pos.Count < 1){ Console.Error.WriteLine("uso: texconv <dir> [scale] [--dry]  |  texconv --apk <apk> <dir> [scale]"); return 2; }
        return RunFromDir(pos[0], nthreads);
    }

    // ---------- modo APK: extrai+converte numa passada (1 progresso) ----------
    static string ValidateAssetRelative(string fullName, bool isDirectory){
        if (!fullName.StartsWith("assets/", StringComparison.Ordinal)) return null;
        if (fullName.IndexOf('\0') >= 0 || fullName.IndexOf('\\') >= 0 || fullName.Any(char.IsControl))
            throw new InvalidDataException($"unsafe ZIP entry name: {fullName}");
        string rel = fullName.Substring("assets/".Length);
        if (isDirectory){
            if (rel.Length == 0) return null;
            rel = rel.Substring(0, rel.Length - 1);
        }
        if (rel.Length == 0 || rel.StartsWith("/", StringComparison.Ordinal))
            throw new InvalidDataException($"unsafe asset path: {fullName}");
        foreach (string part in rel.Split('/'))
            if (part.Length == 0 || part == "." || part == ".." || part.Contains(':'))
                throw new InvalidDataException($"unsafe asset path: {fullName}");
        return rel;
    }

    static string SafeOutputPath(string root, string relative){
        string canonicalRoot = Path.GetFullPath(root);
        string prefix = canonicalRoot.EndsWith(Path.DirectorySeparatorChar)
            ? canonicalRoot : canonicalRoot + Path.DirectorySeparatorChar;
        string destination = Path.GetFullPath(Path.Combine(canonicalRoot, relative));
        if (!destination.StartsWith(prefix, StringComparison.Ordinal))
            throw new InvalidDataException($"asset destination escapes output root: {relative}");
        return destination;
    }

    static int RunFromApks(IReadOnlyList<string> apks, string gaDir, int nthreads){
        if (apks.Count > 64){ Console.Error.WriteLine("[texconv] APK set excede o limite de 64 fontes"); return 2; }
        foreach (string apk in apks)
            if (!File.Exists(apk)){ Console.Error.WriteLine("[texconv] APK nao existe: "+apk); return 2; }
        List<(int Source, string Name, string Relative, long Length, uint Crc)> entries = new();
        try {
            var seenDestinations = new Dictionary<string,(string Relative,long Length,uint Crc)>(StringComparer.OrdinalIgnoreCase);
            for (int sourceIndex=0; sourceIndex<apks.Count; sourceIndex++){
                using var probe = ZipFile.OpenRead(apks[sourceIndex]);
                var seenNames = new HashSet<string>(StringComparer.Ordinal);
                foreach (var entry in probe.Entries){
                    if (!seenNames.Add(entry.FullName))
                        throw new InvalidDataException("duplicate ZIP entry: "+entry.FullName);
                    bool isDirectory = entry.FullName.EndsWith("/", StringComparison.Ordinal);
                    string rel = ValidateAssetRelative(entry.FullName, isDirectory);
                    if (rel == null || isDirectory) continue;
                    if (entry.Length < 0 || entry.Length > MaxApkEntryBytes)
                        throw new InvalidDataException($"asset entry exceeds safe size limit: {rel}");
                    if (seenDestinations.TryGetValue(rel, out var previous)){
                        if (previous.Relative != rel || previous.Length != entry.Length || previous.Crc != entry.Crc32)
                            throw new InvalidDataException("mixed APK set: conflicting asset destination "+rel);
                        continue;
                    }
                    seenDestinations.Add(rel, (rel, entry.Length, entry.Crc32));
                    // Resolve and verify every output before creating the output tree.
                    SafeOutputPath(gaDir, rel);
                    entries.Add((sourceIndex, entry.FullName, rel, entry.Length, entry.Crc32));
                }
            }
        } catch (Exception e) {
            Console.Error.WriteLine("[texconv] APK-set path validation failed: "+e.Message);
            return 2;
        }
        Directory.CreateDirectory(gaDir);
        int total = entries.Count;
        Console.WriteLine($"Extraindo + convertendo {total} arquivos de {apks.Count} APK(s) (escala 1/{Scale})...");
        Console.WriteLine($"[texconv] modo: {nthreads} thread(s)");
        Console.Out.Flush();

        int done=0, conv=0, skip=0, copy=0, err=0, resumed=0; long saved=0;
        // RESUME: carrega os ja-feitos + abre writer de progresso (AutoFlush = crash-safe por linha).
        var doneSet = new HashSet<string>(StringComparer.Ordinal);
        StreamWriter resumeW = null; object resumeLock = new object();
        if (ResumeFile != null){
            try { if (File.Exists(ResumeFile)) foreach (var ln in File.ReadAllLines(ResumeFile)) if (ln.Length>0) doneSet.Add(ln); } catch {}
            resumeW = new StreamWriter(new FileStream(ResumeFile, FileMode.Append, FileAccess.Write, FileShare.ReadWrite)){ AutoFlush = true };
            Console.WriteLine($"[texconv] resume: {doneSet.Count} entradas ja feitas, pulando");
        }
        var po = new ParallelOptions{ MaxDegreeOfParallelism = nthreads };
        Parallel.ForEach(entries, po,
            () => apks.Select(ZipFile.OpenRead).ToArray(), // um set de handles por worker
            (item, st, archives) => {
                string rel = item.Relative;
                var entry = archives[item.Source].GetEntry(item.Name) ?? throw new InvalidDataException("missing ZIP entry: "+item.Name);
                string dst = SafeOutputPath(gaDir, rel);
                bool isXnb = rel.EndsWith(".xnb", StringComparison.OrdinalIgnoreCase);
                bool outputComplete = false;
                try {
                    var info = new FileInfo(dst);
                    outputComplete = info.Exists &&
                        (isXnb ? info.Length >= 10 : info.Length == entry.Length);
                } catch {}
                if (resumeW != null && doneSet.Contains(rel) && outputComplete){
                    int dr = Interlocked.Increment(ref done); Interlocked.Increment(ref resumed);
                    Console.WriteLine($"{dr}/{total}  {(int)((long)dr*100/total)}%  {rel}  (resume)"); Console.Out.Flush();
                    return archives;
                }
                try {
                    byte[] raw;
                    using (var s = entry.Open()){ using var ms = new MemoryStream(); s.CopyTo(ms); raw = ms.ToArray(); }
                    var dd = Path.GetDirectoryName(dst); if (!string.IsNullOrEmpty(dd)) Directory.CreateDirectory(dd);
                    if (isXnb){
                        int r = ConvertBytes(raw, out byte[] outb);
                        if (Diag){   // so categoriza, NAO escreve (rapido)
                            if (r==0) Console.Error.WriteLine($"[SKIP r{LastReason}] {rel}");
                            else if (r==1 && LastWhitened) Console.Error.WriteLine($"[WHITEN] {rel}");
                            if (r==1) Interlocked.Increment(ref conv); else if (r==0) Interlocked.Increment(ref skip); else Interlocked.Increment(ref err);
                        }
                        else if (r==1 && !Dry){ WriteAllBytesAtomic(dst, outb); Interlocked.Increment(ref conv); Interlocked.Add(ref saved, raw.Length - outb.Length); }
                        else if (r==1){ Interlocked.Increment(ref conv); }     // dry
                        else if (r==0){
                            WriteAllBytesAtomic(dst, raw);
                            Interlocked.Increment(ref skip);
                        } else {
                            // A decode/encode failure is not a valid intentional skip.
                            // Never publish the original ASTC payload into an ETC bake and
                            // never let the resume journal turn that failure into success.
                            throw new InvalidDataException($"texture conversion failed (reason {LastReason})");
                        }
                    } else {
                        if (!Diag){ WriteAllBytesAtomic(dst, raw); Interlocked.Increment(ref copy); }
                    }
                    if (resumeW != null && !Diag && !Dry)
                        lock(resumeLock) resumeW.WriteLine(rel);  // payload atomico OK -> marca feito
                } catch (Exception e){ Interlocked.Increment(ref err); Console.Error.WriteLine($"[texconv] ERR {rel}: {e.Message}"); }
                int d = Interlocked.Increment(ref done);
                int pct = (int)((long)d*100/total);
                Console.WriteLine($"{d}/{total}  {pct}%  {rel}"); Console.Out.Flush();
                return archives;
            },
            (archives) => { foreach (var archive in archives) archive.Dispose(); });
        resumeW?.Dispose();
        Console.WriteLine($"Pronto! convertidas={conv} copiadas={copy} puladas={skip} erros={err} resumidas={resumed}  (economia ~{saved/1024/1024} MB)");
        Console.Out.Flush();
        return err > 0 ? 1 : 0;
    }

    // ---------- modo DIRETORIO (legado) ----------
    static int RunFromDir(string root, int nthreads){
        if (!Directory.Exists(root)){ Console.Error.WriteLine("[texconv] dir nao existe: "+root); return 2; }
        var files = new List<string>(Directory.EnumerateFiles(root, "*.xnb", SearchOption.AllDirectories));
        int total = files.Count;
        Console.WriteLine($"Pré-processando {total} texturas (ETC1, escala 1/{Scale})...");
        Console.WriteLine($"[texconv] modo: {nthreads} thread(s)");
        Console.Out.Flush();
        int done=0, conv=0, skip=0, err=0; long saved=0;
        var po = new ParallelOptions{ MaxDegreeOfParallelism = nthreads };
        Parallel.ForEach(files, po, f => {
            long before=0, after=0; int r;
            try { before = new FileInfo(f).Length; r = ConvertFile(f, out after); }
            catch (Exception e){ r = -1; Console.Error.WriteLine($"[texconv] ERR {f}: {e.Message}"); }
            if (r==1){ Interlocked.Increment(ref conv); Interlocked.Add(ref saved, before-after); }
            else if (r==0) Interlocked.Increment(ref skip); else Interlocked.Increment(ref err);
            int d = Interlocked.Increment(ref done);
            int pct = (int)((long)d*100/total);
            string rel; try { rel = Path.GetRelativePath(root, f); } catch { rel = f; }
            Console.WriteLine($"{d}/{total}  {pct}%  {rel}"); Console.Out.Flush();
        });
        Console.WriteLine($"Pronto! convertidas={conv} puladas={skip} erros={err}  (economia ~{saved/1024/1024} MB)");
        Console.Out.Flush();
        return err > 0 ? 1 : 0;
    }

    static int Read7Bit(BinaryReader br){
        int result=0, bits=0; byte b;
        do { b=br.ReadByte(); result |= (b & 0x7F) << bits; bits+=7; } while ((b & 0x80)!=0);
        return result;
    }
    static void ReadXnbString(BinaryReader br){ int len=Read7Bit(br); br.ReadBytes(len); }
    static void ReadFull(Stream s, byte[] buf){
        int off=0; while (off<buf.Length){ int n=s.Read(buf,off,buf.Length-off); if (n<=0) throw new EndOfStreamException(); off+=n; }
    }
    static byte[] Lz4Compress(byte[] body){
        int maxLen = LZ4Codec.MaximumOutputLength(body.Length);
        var outArr = new byte[maxLen];
        int clen = LZ4Codec.Encode32HC(body, 0, body.Length, outArr, 0, maxLen);
        if (clen <= 0) return null;
        var r = new byte[clen]; Array.Copy(outArr, r, clen); return r;
    }
    static byte[] Lz4Decompress(byte[] comp, int decompSize){
        using var cin = new MemoryStream(comp);
        using var dec = new Lz4DecoderStream(cin);
        var outb = new byte[decompSize]; ReadFull(dec, outb); return outb;
    }
    static bool SelfTestLz4(){
        try {
            var rnd = new byte[200000];
            for (int i=0;i<rnd.Length;i++) rnd[i]=(byte)((i*131+7) ^ (i>>3));
            var c = Lz4Compress(rnd); if (c==null) return false;
            var d = Lz4Decompress(c, rnd.Length);
            if (d.Length!=rnd.Length) return false;
            for (int i=0;i<rnd.Length;i++) if (d[i]!=rnd[i]) return false;
            Console.WriteLine($"[texconv] auto-teste LZ4 OK (200KB -> {c.Length}B -> 200KB iguais)");
            return true;
        } catch (Exception e){ Console.Error.WriteLine("[texconv] selftest exc: "+e.Message); return false; }
    }

    // Same-filesystem crash-safe output. Resume is recorded only after the
    // payload is durable and its temporary name has been atomically replaced.
    static void WriteAllBytesAtomic(string path, byte[] data){
        string temporary = path + ".sor4-part";
        try {
            using (var output = new FileStream(temporary, FileMode.Create, FileAccess.Write, FileShare.None)) {
                output.Write(data, 0, data.Length);
                output.Flush(true);
            }
            File.Move(temporary, path, true);
        } finally {
            try { if (File.Exists(temporary)) File.Delete(temporary); } catch {}
        }
    }

    // disco -> disco (modo diretorio)
    static int ConvertFile(string path, out long newLen){
        newLen = 0;
        byte[] raw = File.ReadAllBytes(path);
        int r = ConvertBytes(raw, out byte[] outb);
        if (r != 1) return r;
        if (Dry){ newLen = raw.Length; return 1; }
        WriteAllBytesAtomic(path, outb); newLen = outb.Length; return 1;
    }

    // NUCLEO: bytes XNB ASTC -> bytes XNB ETC1/Color. retorna 1=convertido(outXnb), 0=pular, -1=erro.
    static int ConvertBytes(byte[] raw, out byte[] outXnb){
        outXnb = null; LastReason=0; LastWhitened=false;
        if (raw.Length < 10 || raw[0]!='X' || raw[1]!='N' || raw[2]!='B'){ LastReason=1; return 0; }
        byte platform = raw[3], version = raw[4], flags = raw[5];
        bool lz4 = (flags & 0x40)!=0, lzx = (flags & 0x80)!=0;
        if (lzx){ LastReason=2; return 0; }

        byte[] content;
        if (lz4){
            int decompSize = BitConverter.ToInt32(raw, 10);
            if (decompSize <= 0 || decompSize > MaxDecodedXnbBytes){ LastReason=5; return -1; }
            using var cin = new MemoryStream(raw, 14, raw.Length-14);
            using var dec = new Lz4DecoderStream(cin);
            content = new byte[decompSize];
            ReadFull(dec, content);
        } else {
            content = new byte[raw.Length-10];
            Array.Copy(raw, 10, content, 0, content.Length);
        }

        using var body = new MemoryStream(content);
        using var br = new BinaryReader(body);
        int trCount = Read7Bit(br);
        for (int i=0;i<trCount;i++){ ReadXnbString(br); br.ReadInt32(); }
        int sharedCount = Read7Bit(br);
        int typeIdx = Read7Bit(br);
        long headEnd = body.Position;
        if (typeIdx < 1 || typeIdx > trCount) return 0;

        int fmt = br.ReadInt32();
        int w = br.ReadInt32(), h = br.ReadInt32(), levelCount = br.ReadInt32();
        if (w<=0 || h<=0 || levelCount<=0){ LastReason=5; return -1; }
        long rgbaBytes = (long)w * h * 4;
        if (rgbaBytes <= 0 || rgbaBytes > MaxDecodedTextureBytes || rgbaBytes > int.MaxValue){ LastReason=5; return -1; }
        int lvl0 = br.ReadInt32();
        if (lvl0<=0 || lvl0 > content.Length){ LastReason=5; return -1; }

        byte[] full;
        double effScale = Scale;   // escala POR-textura: pequena nao-ASTC em ETC2 = sem downscale (fonte crisp)
        if (fmt >= ASTC_MIN){
            // ASTC -> decode p/ RGBA
            byte[] astc = br.ReadBytes(lvl0);
            int nb = lvl0/16, bx=0, by=0;
            for (int i=0;i<AstcBlk.GetLength(0);i++){ int cbx=AstcBlk[i,0], cby=AstcBlk[i,1];
                if (((w+cbx-1)/cbx)*((h+cby-1)/cby) == nb){ bx=cbx; by=cby; break; } }
            if (bx==0){ LastReason=6; return -1; }  // ASTC desconhecido jamais pode vazar para um bake ETC.
            full = new byte[(int)rgbaBytes];
            if (sor4_astc_decode(astc, (ulong)lvl0, w, h, bx, by, full) != 0){ LastReason=7; return -1; }
        } else if (fmt==SF_Color && lvl0==w*h*4 && (Etc2 || (DownColor && (w>=ColorMin||h>=ColorMin)))){
            // Color/RGBA8 (menus/fontes/fundos). Em ETC2 converte QUALQUER tamanho (8bpp) E
            // aplica o downscale em TUDO (device apertado, nao da p/ poupar nada).
            full = br.ReadBytes(lvl0);
        } else if (Etc2 && fmt==SF_Bgra4444 && lvl0==w*h*2){
            // BGRA4444 (16bpp) -> RGBA8 -> ETC2-EAC (8bpp). Antes o reader explodia p/ 32bpp na VRAM.
            // Decode (igual Texture2DReader): nibbles B/G/R/A, cada um *17 (0..15 -> 0..255).
            byte[] src = br.ReadBytes(lvl0);
            full = new byte[(int)rgbaBytes];
            for (int i=0;i<w*h;i++){
                int p = src[i*2] | (src[i*2+1]<<8);
                full[i*4]   = (byte)(((p>>8)&0xF)*17);  // R
                full[i*4+1] = (byte)(((p>>4)&0xF)*17);  // G
                full[i*4+2] = (byte)(((p)   &0xF)*17);  // B
                full[i*4+3] = (byte)(((p>>12)&0xF)*17); // A
            }
        } else {
            LastReason=4; return 0;   // DXT/outros: deixa como esta
        }

        if (!NoMaskFix){
            // MESMA regra do Texture2DReader.SOR4.cs: so e mascara de FONTE se entre os pixels
            // OPACOS (alpha>128) quase nenhum tem cor (<1%). Cerca/grade/escada de fundo =
            // pixel opaco COLORIDO (fio/degrau) -> NAO e mascara -> POUPA (mantem a cor).
            // O criterio antigo media cor GLOBAL e pintava de branco cercas finas (<1% global).
            long sa=0; int nOp=0, nOpCol=0, np=full.Length/4;
            for (int p=0;p<full.Length;p+=4){ byte a=full[p+3]; sa+=a; int mx=full[p]; if(full[p+1]>mx)mx=full[p+1]; if(full[p+2]>mx)mx=full[p+2]; if(a>128){ nOp++; if(mx>16) nOpCol++; } }
            if (np>0 && sa/np > 12 && nOp>0 && (long)nOpCol*100 < (long)nOp){ LastWhitened=true;
                for (int p=0;p<full.Length;p+=4){ byte a=full[p+3]; full[p]=a; full[p+1]=a; full[p+2]=a; } }
        }

        int tw=Math.Max((int)(w/effScale),1), th=Math.Max((int)(h/effScale),1);
        byte[] rgba;
        if (effScale<=1.0){ rgba=full; }
        else {
            // box-filter de AREA (suporta escala fracionaria, ex 4.5): cada texel de saida media
            // a regiao de origem [x*S,(x+1)*S) x [y*S,(y+1)*S). Reduz a equivalente p/ S inteiro.
            rgba = new byte[tw*th*4];
            for (int y=0;y<th;y++) for (int x=0;x<tw;x++){
                int sx0=(int)(x*effScale), sy0=(int)(y*effScale);
                int sx1=(int)((x+1)*effScale); if(sx1<=sx0)sx1=sx0+1; if(sx1>w)sx1=w;
                int sy1=(int)((y+1)*effScale); if(sy1<=sy0)sy1=sy0+1; if(sy1>h)sy1=h;
                int rr=0,gg=0,bb=0,aa=0,cnt=0;
                for (int py=sy0;py<sy1;py++) for (int px=sx0;px<sx1;px++){
                    int o=(py*w+px)*4; rr+=full[o]; gg+=full[o+1]; bb+=full[o+2]; aa+=full[o+3]; cnt++; }
                int d=(y*tw+x)*4; if(cnt==0)cnt=1;
                rgba[d]=(byte)(rr/cnt); rgba[d+1]=(byte)(gg/cnt); rgba[d+2]=(byte)(bb/cnt); rgba[d+3]=(byte)(aa/cnt);
            }
        }

        bool opaque=true;
        for (int p=3;p<rgba.Length;p+=4){ if (rgba[p] < 250){ opaque=false; break; } }
        int outFmt; byte[] outData;
        int bwB=(tw+3)&~3, bhB=(th+3)&~3;
        if (opaque){
            var etc1=new byte[(bwB/4)*(bhB/4)*8];
            // ETC1 bitstream = ETC2-RGB base valido -> mesmos bytes, so muda o enum (90 no GLES3).
            if (sor4_etc1_encode(rgba, tw, th, etc1)==0){ outFmt = Etc2 ? SF_Rgb8Etc2 : SF_RgbEtc1; outData=etc1; }
            else { outFmt=SF_Color; outData=rgba; }
        } else if (Etc2){
            // ALPHA em ETC2-EAC RGBA = 8bpp (vs 32bpp do SF_Color) -> 4x menos GPU, mantem resolucao.
            var eac=new byte[(bwB/4)*(bhB/4)*16];
            if (sor4_etc2_eac_rgba_encode(rgba, tw, th, eac)==0){ outFmt=SF_Rgba8Etc2; outData=eac; }
            else { outFmt=SF_Color; outData=rgba; }
        } else { outFmt=SF_Color; outData=rgba; }

        if (Dry){ outXnb = raw; return 1; }

        byte[] newBody;
        using (var nb2 = new MemoryStream()){
            using (var bw = new BinaryWriter(nb2)){
                bw.Write(content, 0, (int)headEnd);
                bw.Write(outFmt); bw.Write(tw); bw.Write(th); bw.Write(1);
                bw.Write(outData.Length); bw.Write(outData);
            }
            newBody = nb2.ToArray();
        }
        byte[] comp = Lz4Compress(newBody);
        bool useComp = comp != null && (14 + comp.Length) < (10 + newBody.Length);
        using (var outMs = new MemoryStream()){
            using (var bw = new BinaryWriter(outMs)){
                bw.Write((byte)'X'); bw.Write((byte)'N'); bw.Write((byte)'B');
                bw.Write(platform); bw.Write(version);
                if (useComp){
                    bw.Write((byte)0x40);
                    bw.Write((uint)(14 + comp.Length));
                    bw.Write(newBody.Length);
                    bw.Write(comp);
                } else {
                    bw.Write((byte)0);
                    bw.Write(10 + newBody.Length);
                    bw.Write(newBody);
                }
            }
            outXnb = outMs.ToArray();
        }
        return 1;
    }
}
