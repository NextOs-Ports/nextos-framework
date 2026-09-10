# Ferramentas GL / windowing

## sdl2-compat (libSDL2 sobre SDL3-mali do device)
Cross-build aarch64:
  git clone https://github.com/libsdl-org/sdl2-compat (ou tarball main)
  cmake -G Ninja -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_BUILD_TYPE=Release
  ninja   # ignora falha dos testes; gera libSDL2-2.0.so.0.3200.xx (só linka libc, dlopen SDL3)

## glprobe.c — sonda do GL entregue pelo stack
  aarch64-linux-gnu-gcc glprobe.c -o glprobe <path>/libSDL2-2.0.so.0.3200.xx
No device (frontend PARADO p/ liberar fb0):
  systemctl stop emustation; pkill -9 emulationstatio
  LD_LIBRARY_PATH=<libs>:/usr/lib SDL_VIDEODRIVER=mali ./glprobe
Resultado esperado: contexto GLES2 do Mali (sem ARB/EXT_framebuffer_object → DesktopGL rejeita;
por isso vamos de MonoGame GLES).

## Decode ASTC real (libsor4astc.so)
Mali-450 nao suporta ASTC. Decodificamos em runtime p/ RGBA8 via astcenc:
- Fonte astcenc 5.0.0 em ~/deadcells-deploy/astc-encoder; build arm64 decompressor-only+NEON, SEM LTO:
  cmake -DASTCENC_ISA_NEON=ON -DASTCENC_DECOMPRESSOR=ON -DASTCENC_SHAREDLIB=ON -DASTCENC_CLI=OFF
        -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF -DASTCENC_INVARIANCE=OFF (cross aarch64)
- wrapper port/tools/sor4astc.c -> sor4_astc_decode(data,len,w,h,bx,by,outRGBA); linka libastcdec-neon-static.a
- Texture2DReader (port/monogame-gles-patches/Texture2DReader.SOR4.cs): fmt>=96 -> detecta bloco
  pelo tamanho dos dados (numBlocks=len/16, casa ceil(w/bx)*ceil(h/by)) -> decode -> Color RGBA8.
- Deploy libsor4astc.so em libs/. P/Invoke DllImport("sor4astc").
