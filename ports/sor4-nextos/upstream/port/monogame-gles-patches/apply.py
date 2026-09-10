#!/usr/bin/env python3
# Aplica os patches GLES no source do MonoGame 3.8.4 p/ rodar no Mali-450 (GLES2).
# Uso: python3 apply.py <caminho_MonoGame/MonoGame.Framework>
import sys,os,shutil
root=sys.argv[1] if len(sys.argv)>1 else "."
here=os.path.dirname(os.path.abspath(__file__))
def patch(rel, old, new):
    p=os.path.join(root,rel); s=open(p).read()
    assert old in s, f"padrao nao encontrado em {rel}"
    open(p,"w").write(s.replace(old,new,1)); print("patched",rel)

# 1) contexto OpenGL ES em vez de GL desktop. O probe/launcher seleciona ES3
# quando ele foi realmente criado e cai para ES2 nos devices estritos.
patch("Platform/GraphicsDeviceManager.SDL.cs",
"""            Sdl.GL.SetAttribute(Sdl.GL.Attribute.DoubleBuffer, 1);
            Sdl.GL.SetAttribute(Sdl.GL.Attribute.ContextMajorVersion, 2);
            Sdl.GL.SetAttribute(Sdl.GL.Attribute.ContextMinorVersion, 1);""",
"""            Sdl.GL.SetAttribute(Sdl.GL.Attribute.DoubleBuffer, 1);
#if GLES
            Sdl.GL.SetAttribute(Sdl.GL.Attribute.ContextProfileMAsl, 4); // SDL_GL_CONTEXT_PROFILE_ES
            int sor4GlMajor = 2;
            var sor4GlText = System.Environment.GetEnvironmentVariable("SOR4_GLES");
            if (!string.IsNullOrEmpty(sor4GlText) && int.TryParse(sor4GlText, out int sor4GlValue) &&
                sor4GlValue >= 2 && sor4GlValue <= 3) sor4GlMajor = sor4GlValue;
            Sdl.GL.SetAttribute(Sdl.GL.Attribute.ContextMajorVersion, sor4GlMajor);
            Sdl.GL.SetAttribute(Sdl.GL.Attribute.ContextMinorVersion, 0);
#else
            Sdl.GL.SetAttribute(Sdl.GL.Attribute.ContextMajorVersion, 2);
            Sdl.GL.SetAttribute(Sdl.GL.Attribute.ContextMinorVersion, 1);
#endif""")

# 2) BoundApi = ES no caminho SDL quando GLES
patch("Platform/Graphics/OpenGL.SDL.cs",
"""        static partial void LoadPlatformEntryPoints()
        {
            BoundApi = RenderApi.GL;
        }""",
"""        static partial void LoadPlatformEntryPoints()
        {
#if GLES
            BoundApi = RenderApi.ES;
#else
            BoundApi = RenderApi.GL;
#endif
        }""")

# 3) PolygonMode (desktop-only, nil no GLES) nao deve ser chamado no build hibrido
patch("Platform/Graphics/States/RasterizerState.OpenGL.cs",
"#if WINDOWS || DESKTOPGL",
"#if (WINDOWS || DESKTOPGL) && !GLES")

# 4) Complete Android ASTC enum (the SOR4 fork uses values 96..109).
patch("Graphics/SurfaceFormat.cs",
"""        Astc4X4Rgba = 96,""",
"""        Astc4X4Rgba = 96,
        Astc5X4Rgba = 97,
        Astc5X5Rgba = 98,
        Astc6X5Rgba = 99,
        Astc6X6Rgba = 100,
        Astc8X5Rgba = 101,
        Astc8X6Rgba = 102,
        Astc8X8Rgba = 103,
        Astc10X5Rgba = 104,
        Astc10X6Rgba = 105,
        Astc10X8Rgba = 106,
        Astc10X10Rgba = 107,
        Astc12X10Rgba = 108,
        Astc12X12Rgba = 109,""")

patch("Graphics/GraphicsCapabilities.cs",
"""        internal bool SupportsEtc2 { get; private set; }

        /// <summary>
        /// Gets the support for ATITC""",
"""        internal bool SupportsEtc2 { get; private set; }

        /// <summary>Gets support for LDR ASTC texture compression.</summary>
        internal bool SupportsAstc { get; private set; }

        /// <summary>
        /// Gets the support for ATITC""")

patch("Platform/Graphics/GraphicsCapabilities.OpenGL.cs",
"""            SupportsEtc1 = GL.Extensions.Contains("GL_OES_compressed_ETC1_RGB8_texture");
            SupportsAtitc""",
"""            SupportsEtc1 = GL.Extensions.Contains("GL_OES_compressed_ETC1_RGB8_texture");
            SupportsAstc = GL.Extensions.Contains("GL_KHR_texture_compression_astc_ldr") ||
                           GL.Extensions.Contains("GL_OES_texture_compression_astc");
            SupportsAtitc""")

patch("Graphics/GraphicsExtensions.cs",
"""            var supportsEtc2 = graphicsDevice.GraphicsCapabilities.SupportsEtc2;
            var supportsAtitc""",
"""            var supportsEtc2 = graphicsDevice.GraphicsCapabilities.SupportsEtc2;
            var supportsAstc = graphicsDevice.GraphicsCapabilities.SupportsAstc;
            var supportsAtitc""")

patch("Graphics/GraphicsExtensions.cs",
"""                break;
			case SurfaceFormat.RgbPvrtc2Bpp:""",
"""                break;
            case SurfaceFormat.Astc4X4Rgba:
            case SurfaceFormat.Astc5X4Rgba:
            case SurfaceFormat.Astc5X5Rgba:
            case SurfaceFormat.Astc6X5Rgba:
            case SurfaceFormat.Astc6X6Rgba:
            case SurfaceFormat.Astc8X5Rgba:
            case SurfaceFormat.Astc8X6Rgba:
            case SurfaceFormat.Astc8X8Rgba:
            case SurfaceFormat.Astc10X5Rgba:
            case SurfaceFormat.Astc10X6Rgba:
            case SurfaceFormat.Astc10X8Rgba:
            case SurfaceFormat.Astc10X10Rgba:
            case SurfaceFormat.Astc12X10Rgba:
            case SurfaceFormat.Astc12X12Rgba:
                if (!supportsAstc)
                    goto case InvalidFormat;
                // SOR4 Android 1.4.5 serializes all 23,745 ASTC textures as
                // SurfaceFormat 98, while their validated payload geometry is 6x6.
                // Its MonoGame fork used a compact square-only enum, unlike the
                // complete Khronos ordering used by current MonoGame.
                glInternalFormat = (PixelInternalFormat)(((int)format == 98)
                    ? 0x93B4 : 0x93B0 + ((int)format - 96));
                glFormat = PixelFormat.CompressedTextureFormats;
                break;
			case SurfaceFormat.RgbPvrtc2Bpp:""")

# Add all ASTC formats to the compressed-format predicate.
patch("Graphics/GraphicsExtensions.cs",
"""                case SurfaceFormat.SRgb8A8Etc2:
                case SurfaceFormat.RgbPvrtc2Bpp:
                case SurfaceFormat.RgbPvrtc4Bpp:
                    return true;""",
"""                case SurfaceFormat.SRgb8A8Etc2:
                case SurfaceFormat.RgbPvrtc2Bpp:
                case SurfaceFormat.RgbPvrtc4Bpp:
                case SurfaceFormat.Astc4X4Rgba:
                case SurfaceFormat.Astc5X4Rgba:
                case SurfaceFormat.Astc5X5Rgba:
                case SurfaceFormat.Astc6X5Rgba:
                case SurfaceFormat.Astc6X6Rgba:
                case SurfaceFormat.Astc8X5Rgba:
                case SurfaceFormat.Astc8X6Rgba:
                case SurfaceFormat.Astc8X8Rgba:
                case SurfaceFormat.Astc10X5Rgba:
                case SurfaceFormat.Astc10X6Rgba:
                case SurfaceFormat.Astc10X8Rgba:
                case SurfaceFormat.Astc10X10Rgba:
                case SurfaceFormat.Astc12X10Rgba:
                case SurfaceFormat.Astc12X12Rgba:
                    return true;""")

# ASTC always stores 16 bytes per compression block.
patch("Graphics/GraphicsExtensions.cs",
"""                case SurfaceFormat.Rgba8Etc2:
                case SurfaceFormat.SRgb8A8Etc2:
                    // One texel in DXT3""",
"""                case SurfaceFormat.Rgba8Etc2:
                case SurfaceFormat.SRgb8A8Etc2:
                case SurfaceFormat.Astc4X4Rgba:
                case SurfaceFormat.Astc5X4Rgba:
                case SurfaceFormat.Astc5X5Rgba:
                case SurfaceFormat.Astc6X5Rgba:
                case SurfaceFormat.Astc6X6Rgba:
                case SurfaceFormat.Astc8X5Rgba:
                case SurfaceFormat.Astc8X6Rgba:
                case SurfaceFormat.Astc8X8Rgba:
                case SurfaceFormat.Astc10X5Rgba:
                case SurfaceFormat.Astc10X6Rgba:
                case SurfaceFormat.Astc10X8Rgba:
                case SurfaceFormat.Astc10X10Rgba:
                case SurfaceFormat.Astc12X10Rgba:
                case SurfaceFormat.Astc12X12Rgba:
                    // One texel in DXT3""")

# Exact ASTC block dimensions for SetData validation/allocation.
patch("Graphics/GraphicsExtensions.cs",
"""                default:
                    width = 1;
                    height = 1;
                    break;""",
"""                case SurfaceFormat.Astc4X4Rgba: width = 4; height = 4; break;
                case SurfaceFormat.Astc5X4Rgba: width = 5; height = 4; break;
                // SOR4 v1.4.5's serialized value 98 is ASTC 6x6 (see mapping above).
                case SurfaceFormat.Astc5X5Rgba: width = 6; height = 6; break;
                case SurfaceFormat.Astc6X5Rgba: width = 6; height = 5; break;
                case SurfaceFormat.Astc6X6Rgba: width = 6; height = 6; break;
                case SurfaceFormat.Astc8X5Rgba: width = 8; height = 5; break;
                case SurfaceFormat.Astc8X6Rgba: width = 8; height = 6; break;
                case SurfaceFormat.Astc8X8Rgba: width = 8; height = 8; break;
                case SurfaceFormat.Astc10X5Rgba: width = 10; height = 5; break;
                case SurfaceFormat.Astc10X6Rgba: width = 10; height = 6; break;
                case SurfaceFormat.Astc10X8Rgba: width = 10; height = 8; break;
                case SurfaceFormat.Astc10X10Rgba: width = 10; height = 10; break;
                case SurfaceFormat.Astc12X10Rgba: width = 12; height = 10; break;
                case SurfaceFormat.Astc12X12Rgba: width = 12; height = 12; break;
                default:
                    width = 1;
                    height = 1;
                    break;""")

patch("Graphics/Texture.cs",
"""                case SurfaceFormat.RgbEtc1:
                case SurfaceFormat.Rgb8Etc2:""",
"""                case SurfaceFormat.RgbEtc1:
                case SurfaceFormat.Astc4X4Rgba:
                case SurfaceFormat.Astc5X4Rgba:
                case SurfaceFormat.Astc5X5Rgba:
                case SurfaceFormat.Astc6X5Rgba:
                case SurfaceFormat.Astc6X6Rgba:
                case SurfaceFormat.Astc8X5Rgba:
                case SurfaceFormat.Astc8X6Rgba:
                case SurfaceFormat.Astc8X8Rgba:
                case SurfaceFormat.Astc10X5Rgba:
                case SurfaceFormat.Astc10X6Rgba:
                case SurfaceFormat.Astc10X8Rgba:
                case SurfaceFormat.Astc10X10Rgba:
                case SurfaceFormat.Astc12X10Rgba:
                case SurfaceFormat.Astc12X12Rgba:
                case SurfaceFormat.Rgb8Etc2:""")

patch("Graphics/Texture.cs",
"""                    pitch = ((width + 3) / 4) * _format.GetSize();""",
"""                    int blockWidth, blockHeight;
                    _format.GetBlockSize(out blockWidth, out blockHeight);
                    pitch = ((width + blockWidth - 1) / blockWidth) * _format.GetSize();""")

# MonoGame's compressed-texture validation historically rounded with a bit mask,
# which only works for power-of-two block dimensions. ASTC 6x6 needs arithmetic
# rounding or valid SOR4 payloads are rejected before reaching OpenGL.
patch("Graphics/Texture2D.cs",
"""                var roundedWidth = (checkedRect.Width + blockWidthMinusOne) & ~blockWidthMinusOne;
                var roundedHeight = (checkedRect.Height + blockHeightMinusOne) & ~blockHeightMinusOne;
                checkedRect = new Rectangle(checkedRect.X & ~blockWidthMinusOne, checkedRect.Y & ~blockHeightMinusOne,""",
"""                var roundedWidth = ((checkedRect.Width + blockWidthMinusOne) / blockWidth) * blockWidth;
                var roundedHeight = ((checkedRect.Height + blockHeightMinusOne) / blockHeight) * blockHeight;
                checkedRect = new Rectangle(checkedRect.X - checkedRect.X % blockWidth,
                    checkedRect.Y - checkedRect.Y % blockHeight,""")

# 5) SOR4 reader/Android bridge + bounded texture pager. These are maintained as
# complete port files so the generated MonoGame tree stays reproducible.
shutil.copy2(os.path.join(here,"Texture2DReader.SOR4.cs"),
             os.path.join(root,"Content/ContentReaders/Texture2DReader.cs"))
shutil.copy2(os.path.join(here,"SOR4Compat.cs"),
             os.path.join(root,"Platform/SOR4Compat.cs"))
shutil.copy2(os.path.join(here,"SOR4TexturePager.cs"),
             os.path.join(root,"Graphics/SOR4TexturePager.cs"))
shutil.copy2(os.path.join(here,"SpriteOESBatch.cs"),
             os.path.join(root,"Graphics/SpriteOESBatch.cs"))
shutil.copy2(os.path.join(here,"SuperVideoPlayer.cs"),
             os.path.join(root,"Media/SuperVideoPlayer.cs"))

# Android fork compatibility types above satisfy the managed game's signatures.
# The following focused runtime fixes are the proven GLES2/PortMaster deltas.
patch("Platform/SDL/SDLGamePlatform.cs",
"""                SdlRunLoop();
                Game.Tick();""",
"""                SdlRunLoop();
                SOR4CheckQuitCombo();
                Game.Tick();""")

patch("Platform/SDL/SDLGamePlatform.cs",
"""        private bool ShouldExit()
        {""",
"""        private void SOR4CheckQuitCombo()
        {
            if (_keys.Contains(Keys.Escape) && _keys.Contains(Keys.Enter))
                Environment.Exit(0);
            for (var index = 0; index < 4; index++)
            {
                var state = GamePad.GetState((PlayerIndex)index);
                if (state.IsConnected && state.Buttons.Back == ButtonState.Pressed &&
                    state.Buttons.Start == ButtonState.Pressed)
                    Environment.Exit(0);
            }
        }

        private bool ShouldExit()
        {""")

patch("Platform/Graphics/GraphicsDevice.OpenGL.FramebufferHelper.cs",
"""                this.SupportsBlitFramebuffer = GL.BlitFramebuffer != null;""",
"""                // Keep SOR4 render targets single-sampled on low-memory handhelds.
                // The Android fork assumes indexed resolve paths that are not stable
                // across the GLES2/3 fbdev and KMSDRM drivers targeted by this port.
                this.SupportsBlitFramebuffer = false;""")

patch("Platform/Graphics/OpenGL.SDL.cs",
"""        private static T LoadFunction<T>(string function, bool throwIfNotFound = false)
        {""",
"""        private static IntPtr _sor4Noop = IntPtr.Zero;

        private static T LoadFunction<T>(string function, bool throwIfNotFound = false)
        {""")

patch("Platform/Graphics/OpenGL.SDL.cs",
"""                if (throwIfNotFound)
                    throw new EntryPointNotFoundException(function);

                return default(T);""",
"""                if (throwIfNotFound)
                    throw new EntryPointNotFoundException(function);
                if (_sor4Noop == IntPtr.Zero)
                {
                    try
                    {
                        var library = NativeLibrary.Load("libWwise.so");
                        _sor4Noop = NativeLibrary.GetExport(library, "sor4_gl_noop");
                    }
                    catch { }
                }
                if (_sor4Noop != IntPtr.Zero)
                    return Marshal.GetDelegateForFunctionPointer<T>(_sor4Noop);
                return default(T);""")

patch("Platform/Graphics/Shader/Shader.OpenGL.cs",
"""            //
            _shaderHandle = GL.CreateShader(Stage == ShaderStage.Vertex ? ShaderType.VertexShader : ShaderType.FragmentShader);""",
"""            // Utgard fragment mediump loses the range used by SOR4 UI effects.
            if (Stage != ShaderStage.Vertex && _glslCode != null &&
                Environment.GetEnvironmentVariable("SOR4_HIGHP") != "0")
            {
                _glslCode = _glslCode
                    .Replace("precision mediump float", "precision highp float")
                    .Replace("precision mediump int", "precision highp int");
            }
            _shaderHandle = GL.CreateShader(Stage == ShaderStage.Vertex ? ShaderType.VertexShader : ShaderType.FragmentShader);""")

patch("Platform/Graphics/States/BlendState.OpenGL.cs",
"""            if (_independentBlendEnable)
            {""",
"""            // Indexed blending is unavailable on the GLES2 compatibility path.
            if (false && _independentBlendEnable)
            {""")

patch("Platform/Graphics/Texture2D.OpenGL.cs",
"""                    else
                    {
                        GL.TexImage2D(TextureTarget.Texture2D, level, glInternalFormat, w, h, 0, glFormat, glType, IntPtr.Zero);
                        GraphicsExtensions.CheckGLError();
                    }""",
"""                    else
                    {
                        // With reflections disabled SOR4 still binds and samples its
                        // 32x32 Color fallback, but never renders or clears it. Texture
                        // storage created from a null pointer is undefined and some Mali
                        // drivers expose it as white/oil-like puddles. Initialize only
                        // that tiny render-target class to transparent pixels.
                        var initialData = IntPtr.Zero;
                        var clearHandle = default(GCHandle);
                        try
                        {
                            if (type == SurfaceType.RenderTarget && format == SurfaceFormat.Color &&
                                level == 0 && w == 32 && h == 32)
                            {
                                var clearPixels = new byte[w * h * 4];
                                clearHandle = GCHandle.Alloc(clearPixels, GCHandleType.Pinned);
                                initialData = clearHandle.AddrOfPinnedObject();
                            }
                            GL.TexImage2D(TextureTarget.Texture2D, level, glInternalFormat, w, h, 0, glFormat, glType, initialData);
                            GraphicsExtensions.CheckGLError();
                        }
                        finally
                        {
                            if (clearHandle.IsAllocated)
                                clearHandle.Free();
                        }
                    }""")

patch("Platform/Graphics/Texture2D.OpenGL.cs",
"""                GL.PixelStore(PixelStoreParameter.UnpackAlignment, Math.Min(_format.GetSize(), 8));

                if (glFormat == GLPixelFormat.CompressedTextureFormats)""",
"""                GL.PixelStore(PixelStoreParameter.UnpackAlignment, Math.Min(_format.GetSize(), 8));

                // SOR4's mobile gauge texture is stored as an inverted magenta mask.
                // The Android shader restores it; the simplified GLES2 shader does not.
                if (glFormat != GLPixelFormat.CompressedTextureFormats &&
                    w > 0 && h > 0 && w <= 512 && h <= 512 &&
                    Environment.GetEnvironmentVariable("SOR4_UNMAG") != "0")
                {
                    try
                    {
                        var needed = w * h * 4;
                        if (elementCount * elementSizeInByte >= needed)
                        {
                            var pixels = new byte[needed];
                            Marshal.Copy(dataPtr, pixels, 0, needed);
                            var magenta = 0;
                            for (var pixel = 0; pixel < w * h; pixel++)
                            {
                                var offset = pixel * 4;
                                if (pixels[offset + 3] > 40 && pixels[offset] > 150 &&
                                    pixels[offset + 1] < 110 && pixels[offset + 2] > 150)
                                    magenta++;
                            }
                            if (magenta * 100 >= w * h * 35)
                            {
                                for (var pixel = 0; pixel < w * h; pixel++)
                                {
                                    var offset = pixel * 4;
                                    var intensity = Math.Max(pixels[offset], pixels[offset + 2]);
                                    pixels[offset] = 0;
                                    pixels[offset + 1] = intensity;
                                    pixels[offset + 2] = 0;
                                }
                                Marshal.Copy(pixels, 0, dataPtr, needed);
                            }
                        }
                    }
                    catch { }
                }

                if (glFormat == GLPixelFormat.CompressedTextureFormats)""")

# Every texture bind touches/requests its page even when the GL binding itself
# did not change. Upload/eviction maintenance runs once at Present, not per draw.
patch("Platform/Graphics/TextureCollection.OpenGL.cs",
"""        void PlatformSetTextures(GraphicsDevice device)
        {
            // Skip out if nothing has changed.
            if (_dirty == 0)
                return;""",
"""        void PlatformSetTextures(GraphicsDevice device)
        {
            for (var pageIndex = 0; pageIndex < _textures.Length; pageIndex++)
                SOR4TexturePager.Touch(_textures[pageIndex]);

            // Skip out if nothing has changed.
            if (_dirty == 0)
                return;""")

patch("Graphics/GraphicsDevice.cs",
"""            _graphicsMetrics = new GraphicsMetrics();
            PlatformPresent();""",
"""            _graphicsMetrics = new GraphicsMetrics();
            SOR4TexturePager.FrameEnd();
            PlatformPresent();""")

# Do not retain disposed Texture2D objects in the pager metadata.
patch("Platform/Graphics/Texture.OpenGL.cs",
"""        protected override void Dispose(bool disposing)
        {
            if (!IsDisposed)
            {
                DeleteGLTexture();""",
"""        protected override void Dispose(bool disposing)
        {
            if (!IsDisposed)
            {
                SOR4TexturePager.Forget(this);
                DeleteGLTexture();""")
print("OK - todos os patches aplicados")
