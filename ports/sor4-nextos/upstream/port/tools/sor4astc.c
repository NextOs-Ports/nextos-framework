#include "astcenc.h"
extern "C" {
#include "etc1_encode.h"
}
#include <string.h>
#include <stdlib.h>

// Encode RGBA8 (w x h) -> ETC1 (GL_ETC1_RGB8_OES). out precisa ((w+3)/4)*((h+3)/4)*8 bytes.
// Padroniza p/ multiplo de 4 (clamp na borda). alpha ignorado (ETC1 = RGB). retorna 0 ok.
extern "C" int sor4_etc1_encode(const unsigned char* rgba, int w, int h, unsigned char* out) {
    if (!rgba || !out || w <= 0 || h <= 0) return -1;
    int bw = (w + 3) & ~3, bh = (h + 3) & ~3;
    if (bw == w && bh == h) { etc1_encode_image(rgba, w, h, 4, out); return 0; }
    unsigned char* tmp = (unsigned char*)malloc((size_t)bw * bh * 4);
    if (!tmp) return -2;
    for (int y = 0; y < bh; y++) {
        int sy = y < h ? y : h - 1;
        for (int x = 0; x < bw; x++) {
            int sx = x < w ? x : w - 1;
            memcpy(tmp + ((size_t)y * bw + x) * 4, rgba + ((size_t)sy * w + sx) * 4, 4);
        }
    }
    etc1_encode_image(tmp, bw, bh, 4, out);
    free(tmp);
    return 0;
}

// decode ASTC -> RGBA8. retorna 0 ok. outRGBA precisa w*h*4 bytes.
extern "C" int sor4_astc_decode(const unsigned char* data, unsigned long len,
                                int w, int h, int bx, int by, unsigned char* outRGBA) {
    astcenc_config config;
    astcenc_error st = astcenc_config_init(ASTCENC_PRF_LDR, bx, by, 1,
                          ASTCENC_PRE_FAST, ASTCENC_FLG_DECOMPRESS_ONLY, &config);
    if (st != ASTCENC_SUCCESS) return -1;
    astcenc_context* ctx = 0;
    st = astcenc_context_alloc(&config, 1, &ctx);
    if (st != ASTCENC_SUCCESS) return -2;
    astcenc_image image;
    image.dim_x = w; image.dim_y = h; image.dim_z = 1;
    image.data_type = ASTCENC_TYPE_U8;
    void* slice = outRGBA;
    image.data = &slice;
    astcenc_swizzle swz = { ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A };
    st = astcenc_decompress_image(ctx, data, len, &image, &swz, 0);
    astcenc_context_free(ctx);
    return st == ASTCENC_SUCCESS ? 0 : -3;
}
