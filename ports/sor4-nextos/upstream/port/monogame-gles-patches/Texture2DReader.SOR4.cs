// MonoGame - Copyright (C) MonoGame Foundation, Inc
// This file is subject to the terms and conditions defined in
// file 'LICENSE.txt', which is part of this source code package.

using System;
using System.IO;
using Microsoft.Xna.Framework.Graphics;
using MonoGame.Framework.Utilities;

namespace Microsoft.Xna.Framework.Content
{
    [System.Diagnostics.CodeAnalysis.DynamicallyAccessedMembers(System.Diagnostics.CodeAnalysis.DynamicallyAccessedMemberTypes.All)]
    internal class Texture2DReader : ContentTypeReader<Texture2D>
    {
        static int Sor4Read7(BinaryReader reader){
            int result=0, bits=0; byte value;
            do { value=reader.ReadByte(); result |= (value & 0x7f) << bits; bits += 7; }
            while ((value & 0x80) != 0 && bits < 35);
            return result;
        }

        // Reopen one original XNB and stream-decompress only far enough to recover
        // its level-0 GPU payload. The pager uses this instead of duplicating every
        // native ASTC/ETC texture into a second on-disk pack.
        internal static byte[] Sor4ReadPage(string assetName, int expectedFormat, int expectedLength){
            try {
                using (var source = SOR4Bridge.AssetBridge.Open(assetName))
                using (var header = new BinaryReader(source, System.Text.Encoding.UTF8, true)) {
                    if (header.ReadByte()!='X' || header.ReadByte()!='N' || header.ReadByte()!='B') return null;
                    header.ReadByte(); // platform
                    header.ReadByte(); // version
                    byte flags=header.ReadByte();
                    int xnbLength=header.ReadInt32();
                    Stream content;
                    if ((flags & 0x80)!=0) return null; // LZX fallback pages stay in pagepack.
                    if ((flags & 0x40)!=0) {
                        header.ReadInt32(); // decompressed length
                        content=new Lz4DecoderStream(source, xnbLength-14);
                    } else content=source;
                    using (content)
                    using (var reader = new BinaryReader(content)) {
                        int readers=Sor4Read7(reader);
                        for(int i=0;i<readers;i++){ reader.ReadString(); reader.ReadInt32(); }
                        Sor4Read7(reader); // shared-resource count
                        int typeIndex=Sor4Read7(reader);
                        if(typeIndex<1 || typeIndex>readers) return null;
                        int format=reader.ReadInt32();
                        reader.ReadInt32(); // width
                        reader.ReadInt32(); // height
                        int levels=reader.ReadInt32();
                        int length=reader.ReadInt32();
                        if(levels<1 || (expectedFormat>=0 && format!=expectedFormat) ||
                           (expectedLength>0 && length!=expectedLength)) return null;
                        var data=reader.ReadBytes(length);
                        return data.Length==length ? data : null;
                    }
                }
            } catch(Exception error) {
                if(System.Environment.GetEnvironmentVariable("SOR4_PAGELOG")=="1")
                    System.Console.Error.WriteLine("[SOR4-PAGE] source read failed "+assetName+": "+error.Message);
                return null;
            }
        }

        [System.Runtime.InteropServices.DllImport("sor4astc")]
        static extern int sor4_astc_decode(byte[] data, ulong len, int w, int h, int bx, int by, byte[] outRGBA);
        [System.Runtime.InteropServices.DllImport("sor4astc")]
        static extern int sor4_etc1_encode(byte[] rgba, int w, int h, byte[] outEtc1);
        static readonly bool Sor4Etc1 = System.Environment.GetEnvironmentVariable("SOR4_ETC1")=="1";
        static readonly bool Sor4NativeAstc = System.Environment.GetEnvironmentVariable("SOR4_NATIVE_ASTC")=="1";
        // ASTC -> GPU: opaca -> ETC1 (Mali le nativo, 8x mais leve); com alpha -> RGBA8.
        // Cacheia com header [1 byte fmt] (tag=2). fmt out: RgbEtc1 ou Color.
        static byte[] Sor4AstcToGpu(byte[] data, int len, int w, int h, int scale, out SurfaceFormat fmt){
            string ckey = Sor4CacheDir != null ? Sor4CacheKey(data, len, w, h, scale, 2) : null;
            if (ckey != null){ var ch = Sor4CacheGet(ckey); if (ch != null && ch.Length>=1){ fmt = ch[0]==1?SurfaceFormat.RgbEtc1:SurfaceFormat.Color; var o=new byte[ch.Length-1]; System.Array.Copy(ch,1,o,0,o.Length); return o; } }
            byte[] rgba = Sor4DecodeAstc(data, len, w, h, scale);   // RGBA8 (tw x th), com MASKFIX/scale/cache proprio
            int tw = System.Math.Max(w/scale,1), th = System.Math.Max(h/scale,1);
            bool opaque = true;
            for (int p=3; p<rgba.Length; p+=4){ if (rgba[p] < 250){ opaque=false; break; } }
            byte[] outData;
            if (opaque){
                int bw=(tw+3)&~3, bh=(th+3)&~3; byte[] etc1=new byte[(bw/4)*(bh/4)*8];
                if (sor4_etc1_encode(rgba, tw, th, etc1)==0){ fmt=SurfaceFormat.RgbEtc1; outData=etc1; }
                else { fmt=SurfaceFormat.Color; outData=rgba; opaque=false; }
            } else { fmt=SurfaceFormat.Color; outData=rgba; }
            if (ckey != null){ var store=new byte[outData.Length+1]; store[0]=(byte)(opaque?1:0); System.Array.Copy(outData,0,store,1,outData.Length); Sor4CachePut(ckey, store); }
            return outData;
        }
        static readonly int[,] AstcBlk = {{4,4},{5,4},{5,5},{6,5},{6,6},{8,5},{8,6},{8,8},{10,5},{10,6},{10,8},{10,10},{12,10},{12,12}};
        static readonly string Sor4CacheDir = System.Environment.GetEnvironmentVariable("SOR4_TEXCACHE");
        static string Sor4CacheKey(byte[] data, int len, int w, int h, int scale, int tag){
            ulong hsh=1469598103934665603UL;
            for(int i=0;i<len;i+= (len>4096?257:1)){ hsh=(hsh^data[i])*1099511628211UL; }
            hsh=(hsh^(ulong)len)*1099511628211UL; hsh=(hsh^(ulong)(w*8192+h))*1099511628211UL;
            hsh=(hsh^(ulong)(scale*16+tag))*1099511628211UL;
            return System.IO.Path.Combine(Sor4CacheDir, hsh.ToString("x16")+"_"+w+"x"+h+"_"+scale+"_"+tag+".rgba");
        }
        static byte[] Sor4CacheGet(string key){ try{ if(System.IO.File.Exists(key)) return System.IO.File.ReadAllBytes(key); }catch{} return null; }
        static void Sor4CachePut(string key, byte[] rgba){ try{ System.IO.File.WriteAllBytes(key, rgba); }catch{} }
        static readonly int Sor4TexScale = SOR4_GetScale();
        static int SOR4_GetScale(){ var v=System.Environment.GetEnvironmentVariable("SOR4_TEXSCALE"); int s; if(!string.IsNullOrEmpty(v)&&int.TryParse(v,out s)&&s>=1&&s<=4) return s; return 2; }
        // decodifica ASTC (w x h) -> RGBA8 e reduz por 'scale' (box filter) -> retorna (w/scale)x(h/scale)
        static byte[] Sor4DecodeAstc(byte[] data, int len, int w, int h, int scale) {
            string ckey = Sor4CacheDir != null ? Sor4CacheKey(data, len, w, h, scale, 1) : null;
            if (ckey != null) { var ch = Sor4CacheGet(ckey); if (ch != null) return ch; }
            int nb = len / 16; int bx = 0, by = 0;
            for (int i=0;i<AstcBlk.GetLength(0);i++){ int cbx=AstcBlk[i,0],cby=AstcBlk[i,1];
                if (((w+cbx-1)/cbx)*((h+cby-1)/cby) == nb){ bx=cbx; by=cby; break; } }
            var full = new byte[w*h*4];
            if (bx==0) { for(int p=0;p<full.Length;p+=4){full[p]=128;full[p+1]=128;full[p+2]=128;full[p+3]=255;} System.Console.Error.WriteLine("[ASTC] bloco?? len="+len+" "+w+"x"+h); }
            else { try { int rc = sor4_astc_decode(data, (ulong)len, w, h, bx, by, full);
                if (rc!=0){ System.Console.Error.WriteLine("[ASTC] decode rc="+rc); for(int p=0;p<full.Length;p+=4){full[p]=128;full[p+1]=128;full[p+2]=128;full[p+3]=255;} } }
                catch (System.Exception e){ System.Console.Error.WriteLine("[ASTC] EXC "+e.Message); } }
            // SOR4: atlas de FONTE/mascara = glifos guardados SO no ALPHA, com RGB=0 onde o
            // alpha e alto -> RGB=A (branco premult) p/ o texto ficar visivel (a cor real vem
            // do vertice). CERCAS/GRADES/ESCADAS de fundo NAO sao mascara: onde sao OPACAS
            // (fio/degrau) tem COR REAL -> nao podem virar branco.
            // CRITERIO (preciso): so e mascara se entre os pixels OPACOS (alpha>128) quase
            // NENHUM tem cor (<1%). O criterio antigo media cor GLOBAL e ainda pegava cercas
            // finas (fio <1% da textura toda) -> as pintava de branco. Olhar so o OPACO separa
            // fonte (opaco sem cor) de cerca/escada (opaco colorido). SOR4_MASKFIX_PCT ajusta o
            // limiar; SOR4_NOMASKFIX=1 desliga. Mudanca invalida o texcache (limpar SOR4_TEXCACHE).
            // MASKFIX de runtime DESLIGADO por padrao (o progressor ja converte tudo; e o
            // diagnostico provou que nenhuma fonte depende do maskfix). Reative com SOR4_MASKFIX=1.
            if (System.Environment.GetEnvironmentVariable("SOR4_MASKFIX")=="1") {
                long sa=0; int colored=0, nOp=0, nOpCol=0, np=full.Length/4;
                for(int p=0;p<full.Length;p+=4){
                    byte a=full[p+3]; sa+=a;
                    int mx=full[p]; if(full[p+1]>mx)mx=full[p+1]; if(full[p+2]>mx)mx=full[p+2];
                    bool col = mx>16; if(col) colored++;
                    if(a>128){ nOp++; if(col) nOpCol++; }
                }
                int pct=1; var pv=System.Environment.GetEnvironmentVariable("SOR4_MASKFIX_PCT"); if(!string.IsNullOrEmpty(pv)) int.TryParse(pv, out pct);
                bool isMask = nOp>0 && (long)nOpCol*100 < (long)nOp*pct;   // <pct% dos OPACOS tem cor
                if (np>0 && sa/np > 12 && isMask) {
                    for(int p=0;p<full.Length;p+=4){ byte a=full[p+3]; full[p]=a; full[p+1]=a; full[p+2]=a; }
                    if(System.Environment.GetEnvironmentVariable("SOR4_TEXLOG")=="1") System.Console.Error.WriteLine($"[MASKFIX] {w}x{h} RGB=A (opacoColorido={nOpCol}/{nOp}, globalColored={colored}/{np})");
                } else if(System.Environment.GetEnvironmentVariable("SOR4_TEXLOG")=="1" && np>0 && sa/np>12 && (long)colored*100<(long)np) {
                    System.Console.Error.WriteLine($"[MASKFIX-POUPADO cerca/escada?] {w}x{h} opacoColorido={nOpCol}/{nOp} globalColored={colored}/{np}");
                }
            }
            if (System.Environment.GetEnvironmentVariable("SOR4_DUMPTEX")=="1" && w<=400 && h<=400) {
                try {
                    // stats
                    long sr=0,sg=0,sb=0,sa=0; for(int p=0;p<full.Length;p+=4){sr+=full[p];sg+=full[p+1];sb+=full[p+2];sa+=full[p+3];}
                    int np=full.Length/4;
                    System.Console.Error.WriteLine($"[TEXDUMP] {w}x{h} avgR={sr/np} avgG={sg/np} avgB={sb/np} avgA={sa/np}");
                    System.IO.File.WriteAllBytes($"/tmp/tex_{w}x{h}.raw", full);
                } catch {}
            }
            if (scale<=1) { if(ckey!=null) Sor4CachePut(ckey, full); return full; }
            int nw=System.Math.Max(w/scale,1), nh=System.Math.Max(h/scale,1);
            var small=new byte[nw*nh*4];
            for(int y=0;y<nh;y++) for(int x=0;x<nw;x++){
                int sx=x*scale, sy=y*scale, r=0,g=0,b=0,a=0,cnt=0;
                for(int dy=0;dy<scale;dy++) for(int dx=0;dx<scale;dx++){
                    int px=sx+dx, py=sy+dy; if(px>=w||py>=h) continue;
                    int o=(py*w+px)*4; r+=full[o]; g+=full[o+1]; b+=full[o+2]; a+=full[o+3]; cnt++; }
                int d=(y*nw+x)*4; if(cnt==0)cnt=1;
                small[d]=(byte)(r/cnt); small[d+1]=(byte)(g/cnt); small[d+2]=(byte)(b/cnt); small[d+3]=(byte)(a/cnt);
            }
            if(ckey!=null) Sor4CachePut(ckey, small);
            return small;
        }
		public Texture2DReader()
		{
			// Do nothing
		}

        protected internal override Texture2D Read(ContentReader reader, Texture2D existingInstance)
		{
			Texture2D texture = null;

            var surfaceFormat = (SurfaceFormat)reader.ReadInt32();
            int width = reader.ReadInt32();
            int height = reader.ReadInt32();
            int levelCount = reader.ReadInt32();
            int levelCountOutput = levelCount;

            // If the system does not fully support Power of Two textures,
            // skip any mip maps supplied with any non PoT textures.
            if (levelCount > 1 && !reader.GetGraphicsDevice().GraphicsCapabilities.SupportsNonPowerOfTwo &&
                (!MathHelper.IsPowerOfTwo(width) || !MathHelper.IsPowerOfTwo(height)))
            {
                levelCountOutput = 1;
                System.Diagnostics.Debug.WriteLine(
                    "Device does not support non Power of Two textures. Skipping mipmaps.");
            }

			SurfaceFormat convertedFormat = surfaceFormat;
			switch (surfaceFormat)
			{
				case SurfaceFormat.Dxt1:
				case SurfaceFormat.Dxt1a:
					if (!reader.GetGraphicsDevice().GraphicsCapabilities.SupportsDxt1)
						convertedFormat = SurfaceFormat.Color;
					break;
				case SurfaceFormat.Dxt1SRgb:
					if (!reader.GetGraphicsDevice().GraphicsCapabilities.SupportsDxt1)
						convertedFormat = SurfaceFormat.ColorSRgb;
					break;
				case SurfaceFormat.Dxt3:
				case SurfaceFormat.Dxt5:
					if (!reader.GetGraphicsDevice().GraphicsCapabilities.SupportsS3tc)
						convertedFormat = SurfaceFormat.Color;
					break;
				case SurfaceFormat.Dxt3SRgb:
				case SurfaceFormat.Dxt5SRgb:
					if (!reader.GetGraphicsDevice().GraphicsCapabilities.SupportsS3tc)
						convertedFormat = SurfaceFormat.ColorSRgb;
					break;
				case SurfaceFormat.NormalizedByte4:
					convertedFormat = SurfaceFormat.Color;
					break;
			}
			
            int sor4Scale = 1;
            bool sor4Astc = (int)surfaceFormat >= 96 && (int)surfaceFormat <= 109;
            bool sor4AstcNative = sor4Astc && Sor4NativeAstc &&
                reader.GetGraphicsDevice().GraphicsCapabilities.SupportsAstc;
            if (sor4Astc) {
                convertedFormat = sor4AstcNative ? surfaceFormat : SurfaceFormat.Color;
                sor4Scale = sor4AstcNative ? 1 : Sor4TexScale;
                // Keep level 0 only. This bounds residency and avoids requiring the
                // pager to preserve/reload every mip level independently.
                levelCountOutput = 1;
            }
            if (surfaceFormat == SurfaceFormat.Bgra4444) { convertedFormat = SurfaceFormat.Color; levelCountOutput = 1; } // SOR4: 4444 -> RGBA8 (fonte/Mali)
            // SOR4 ETC1 (gateado por SOR4_ETC1): ASTC -> opaca=ETC1 nativo (Mali, 8x mais leve) /
            // com alpha=RGBA8. O formato so se sabe APOS decodificar (precisa do alpha), entao
            // tratamos o ASTC inteiro aqui e retornamos cedo (consumindo todos os niveis do stream).
            if (Sor4Etc1 && (int)surfaceFormat >= 96 && reader.GetGraphicsDevice().GraphicsCapabilities.SupportsEtc1) {
                int etcScale = Sor4TexScale;
                int etcW = System.Math.Max(width/etcScale,1), etcH = System.Math.Max(height/etcScale,1);
                byte[] astc0 = null;
                for (int lv=0; lv<levelCount; lv++){
                    int sz = reader.ReadInt32();
                    byte[] ld = new byte[sz];
                    reader.Read(ld, 0, sz);
                    if (lv==0) astc0 = ld;
                }
                SurfaceFormat etcFmt;
                byte[] gpu = Sor4AstcToGpu(astc0, astc0.Length, width, height, etcScale, out etcFmt);
                texture = existingInstance ?? new Texture2D(reader.GetGraphicsDevice(), etcW, etcH, false, etcFmt);
#if OPENGL
                Threading.BlockOnUIThread(() => { texture.SetData(0, null, gpu, 0, gpu.Length); });
#else
                texture.SetData(0, null, gpu, 0, gpu.Length);
#endif
                SOR4TexturePager.Register(texture, gpu, gpu.Length, null);
                return texture;
            }
            int sor4TexW = System.Math.Max(width/sor4Scale,1), sor4TexH = System.Math.Max(height/sor4Scale,1);
            texture = existingInstance ?? new Texture2D(reader.GetGraphicsDevice(), sor4TexW, sor4TexH, levelCountOutput > 1, convertedFormat);
#if OPENGL
            Threading.BlockOnUIThread(() =>
            {
#endif
                for (int level = 0; level < levelCount; level++)
			    {
				    var levelDataSizeInBytes = reader.ReadInt32();
                    var levelData = ContentManager.ScratchBufferPool.Get(levelDataSizeInBytes);
                    reader.Read(levelData, 0, levelDataSizeInBytes);
                    int levelWidth = Math.Max(width >> level, 1);
                    int levelHeight = Math.Max(height >> level, 1);

                    if (level >= levelCountOutput)
                        continue;

                    if (sor4Astc && !sor4AstcNative) {
                        // SOR4: ASTC -> decode p/ RGBA8 via astcenc (Mali-450 nao tem ASTC nativo)
                        levelData = Sor4DecodeAstc(levelData, levelDataSizeInBytes, levelWidth, levelHeight, sor4Scale);
                        levelDataSizeInBytes = levelData.Length;
                    }
				    //Convert the image data if required
				    switch (surfaceFormat)
				    {
					    case SurfaceFormat.Dxt1:
                        case SurfaceFormat.Dxt1SRgb:
                        case SurfaceFormat.Dxt1a:
				            if (!reader.GetGraphicsDevice().GraphicsCapabilities.SupportsDxt1 && convertedFormat == SurfaceFormat.Color)
				            {
				                levelData = DxtUtil.DecompressDxt1(levelData, levelWidth, levelHeight);
				                levelDataSizeInBytes = levelData.Length;
				            }
				            break;
					    case SurfaceFormat.Dxt3:
					    case SurfaceFormat.Dxt3SRgb:
                            if (!reader.GetGraphicsDevice().GraphicsCapabilities.SupportsS3tc)
				                if (!reader.GetGraphicsDevice().GraphicsCapabilities.SupportsS3tc &&
				                    convertedFormat == SurfaceFormat.Color)
				                {
				                    levelData = DxtUtil.DecompressDxt3(levelData, levelWidth, levelHeight);
                                    levelDataSizeInBytes = levelData.Length;
                                }
				            break;
					    case SurfaceFormat.Dxt5:
					    case SurfaceFormat.Dxt5SRgb:
                            if (!reader.GetGraphicsDevice().GraphicsCapabilities.SupportsS3tc)
				                if (!reader.GetGraphicsDevice().GraphicsCapabilities.SupportsS3tc &&
				                    convertedFormat == SurfaceFormat.Color)
				                {
				                    levelData = DxtUtil.DecompressDxt5(levelData, levelWidth, levelHeight);
                                    levelDataSizeInBytes = levelData.Length;
                                }
				            break;
                        case SurfaceFormat.Bgra5551:
                            {
#if OPENGL
                                // Shift the channels to suit OpenGL
                                int offset = 0;
                                for (int y = 0; y < levelHeight; y++)
                                {
                                    for (int x = 0; x < levelWidth; x++)
                                    {
                                        ushort pixel = BitConverter.ToUInt16(levelData, offset);
                                        pixel = (ushort)(((pixel & 0x7FFF) << 1) | ((pixel & 0x8000) >> 15));
                                        levelData[offset] = (byte)(pixel);
                                        levelData[offset + 1] = (byte)(pixel >> 8);
                                        offset += 2;
                                    }
                                }
#endif
                            }
                            break;
					    case SurfaceFormat.Bgra4444:
						    {
                                // SOR4: decodifica BGRA4444 -> RGBA8 (Color) p/ Mali-450 (com cache)
                                string bkey = Sor4CacheDir != null ? Sor4CacheKey(levelData, levelDataSizeInBytes, levelWidth, levelHeight, 1, 1) : null;
                                byte[] rgba = bkey != null ? Sor4CacheGet(bkey) : null;
                                if (rgba == null) {
                                    rgba = new byte[levelWidth*levelHeight*4];
                                    for (int i=0;i<levelWidth*levelHeight;i++){
                                        ushort p = BitConverter.ToUInt16(levelData, i*2);
                                        int b=(p)&0xF, g=(p>>4)&0xF, r=(p>>8)&0xF, a=(p>>12)&0xF;
                                        rgba[i*4]=(byte)(r*17); rgba[i*4+1]=(byte)(g*17); rgba[i*4+2]=(byte)(b*17); rgba[i*4+3]=(byte)(a*17);
                                    }
                                    if (bkey != null) Sor4CachePut(bkey, rgba);
                                }
                                if(System.Environment.GetEnvironmentVariable("SOR4_DUMPTEX")=="1" && levelWidth<=600){ try{ System.IO.File.WriteAllBytes($"/tmp/font_{levelWidth}x{levelHeight}.raw", rgba);}catch{} }
                                levelData = rgba; levelDataSizeInBytes = rgba.Length;
						    }
						    break;
					    case SurfaceFormat.NormalizedByte4:
						    {
							    int bytesPerPixel = surfaceFormat.GetSize();
							    int pitch = levelWidth * bytesPerPixel;
							    for (int y = 0; y < levelHeight; y++)
							    {
								    for (int x = 0; x < levelWidth; x++)
								    {
									    int color = BitConverter.ToInt32(levelData, y * pitch + x * bytesPerPixel);
									    levelData[y * pitch + x * 4] = (byte)(((color >> 16) & 0xff)); //R:=W
									    levelData[y * pitch + x * 4 + 1] = (byte)(((color >> 8) & 0xff)); //G:=V
									    levelData[y * pitch + x * 4 + 2] = (byte)(((color) & 0xff)); //B:=U
									    levelData[y * pitch + x * 4 + 3] = (byte)(((color >> 24) & 0xff)); //A:=Q
								    }
							    }
						    }
						    break;
				    }
				
                    texture.SetData(level, null, levelData, 0, levelDataSizeInBytes);
                    if (level == 0 && levelCountOutput == 1) {
                        // Texture2DReader is also invoked for the atlas embedded in
                        // a SpriteFont XNB. In that case AssetName names the font
                        // container, not a root Texture2D, so it cannot be reopened
                        // by Sor4ReadPage. Font atlases stay resident (three files in
                        // the validated v1.4.5 data) to keep HUD/text stable.
                        string assetName = reader.AssetName ?? string.Empty;
                        bool embeddedFont = assetName.IndexOf("/fonts/", StringComparison.OrdinalIgnoreCase) >= 0 ||
                            assetName.StartsWith("gui/fonts/", StringComparison.OrdinalIgnoreCase);
                        string pageSource = convertedFormat == surfaceFormat && sor4Scale == 1 &&
                            !(reader.BaseStream is LzxDecoderStream) && !embeddedFont
                            ? reader.AssetName : null;
                        if (!embeddedFont)
                            SOR4TexturePager.Register(texture, levelData, levelDataSizeInBytes, pageSource);
                    }
                    ContentManager.ScratchBufferPool.Return(levelData);
			    }
#if OPENGL
            });
#endif
        			
			return texture;
		}
    }
}
