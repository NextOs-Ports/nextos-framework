/* NextOS Mali-450: ASTC decoder shim via dlsym(libastcUtil.so)
 * Decodes ASTC compressed bytes -> RGBA8 in software using port-bundled libastcUtil.
 */
#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* NextOS: cache de decode ASTC->RGBA8 em disco (zlib). 1a execucao decoda+grava;
 * proximas leem do cache (sem astcenc, rapido). Ativa via env NEXTOS_TEXCACHE=<dir>. */
typedef int (*compress2_t)(unsigned char*, unsigned long*, const unsigned char*, unsigned long, int);
typedef int (*uncompress_t)(unsigned char*, unsigned long*, const unsigned char*, unsigned long);
static compress2_t p_compress2 = 0;
static uncompress_t p_uncompress = 0;
static unsigned long long fnv1a(const unsigned char *d, int n) {
    unsigned long long h = 1469598103934665603ULL;
    int i;
    for (i = 0; i < n; i++) { h ^= d[i]; h *= 1099511628211ULL; }
    return h;
}
static const char* tc_dir(void) {
    static int done = 0; static const char *d = 0;
    if (!done) {
        done = 1; d = getenv("NEXTOS_TEXCACHE");
        if (d && *d) {
            void *z = dlopen("libz.so.1", RTLD_NOW|RTLD_GLOBAL);
            if (!z) z = dlopen("libz.so", RTLD_NOW|RTLD_GLOBAL);
            if (z) { p_compress2 = (compress2_t)dlsym(z, "compress2");
                     p_uncompress = (uncompress_t)dlsym(z, "uncompress"); }
            if (!p_compress2 || !p_uncompress) {
                fprintf(stderr, "[NextOS-TEXCACHE] zlib indisponivel -> cache off\n");
                d = 0;
            } else {
                fprintf(stderr, "[NextOS-TEXCACHE] ativo em %s\n", d);
            }
        } else d = 0;
    }
    return d;
}
static void tc_path(char *buf, int n, const void *src, int src_size, int w, int h) {
    snprintf(buf, n, "%s/%016llx_%dx%d.z", tc_dir(),
             (unsigned long long)fnv1a((const unsigned char*)src, src_size), w, h);
}
static int tc_load(void *dst, int rgba, const void *src, int src_size, int w, int h) {
    const char *cd = tc_dir(); char path[600]; FILE *f; long csz; unsigned char *cb;
    unsigned long dlen; int rc;
    if (!cd) return -1;
    tc_path(path, sizeof(path), src, src_size, w, h);
    f = fopen(path, "rb"); if (!f) return -1;
    fseek(f, 0, SEEK_END); csz = ftell(f); fseek(f, 0, SEEK_SET);
    if (csz <= 0) { fclose(f); return -1; }
    cb = (unsigned char*)malloc(csz); if (!cb) { fclose(f); return -1; }
    if (fread(cb, 1, csz, f) != (size_t)csz) { free(cb); fclose(f); return -1; }
    fclose(f);
    dlen = (unsigned long)rgba;
    rc = p_uncompress((unsigned char*)dst, &dlen, cb, (unsigned long)csz);
    free(cb);
    return (rc == 0 && dlen == (unsigned long)rgba) ? 0 : -1;
}
static void tc_store(const void *dst, int rgba, const void *src, int src_size, int w, int h) {
    const char *cd = tc_dir(); char path[600], tmp[640]; unsigned long bound, clen;
    unsigned char *cb; FILE *f;
    if (!cd) return;
    tc_path(path, sizeof(path), src, src_size, w, h);
    bound = (unsigned long)rgba + rgba/1000 + 64;
    cb = (unsigned char*)malloc(bound); if (!cb) return;
    clen = bound;
    if (p_compress2(cb, &clen, (const unsigned char*)dst, (unsigned long)rgba, 1) == 0) {
        snprintf(tmp, sizeof(tmp), "%s.t%d", path, (int)getpid());
        f = fopen(tmp, "wb");
        if (f) { fwrite(cb, 1, clen, f); fclose(f); rename(tmp, path); }
    }
    free(cb);
}

/* Cache v2: guarda o payload FINAL (ETC1 ou pixels pos-downscale/pos-4444)
 * pronto para upload GL. Contra o v1 (RGBA8 full-size): inflate de bem menos
 * bytes, sem halve/convert por run e pico de RSS bem menor no load. Mesmo dir;
 * arquivo `<fnv1a>_<worig>x<horig>_<flags>.z2` com header {dw,dh,gltype,len}.
 * flags codifica a config (texscale/tex16) para invalidar quando knobs mudam. */
void *nextos_tc2_load(const void *src, int src_size, int w, int h,
                      unsigned int flags, int *dw, int *dh,
                      unsigned int *gltype) {
    const char *cd = tc_dir(); char path[600]; FILE *f; long csz;
    unsigned char *cb = 0, *pix = 0; unsigned int hdr[4]; unsigned long dlen;
    if (!cd) return 0;
    snprintf(path, sizeof(path), "%s/%016llx_%dx%d_%08x.z2", cd,
             (unsigned long long)fnv1a((const unsigned char*)src, src_size),
             w, h, flags);
    f = fopen(path, "rb"); if (!f) return 0;
    if (fread(hdr, sizeof(hdr), 1, f) != 1) { fclose(f); return 0; }
    fseek(f, 0, SEEK_END); csz = ftell(f) - (long)sizeof(hdr);
    fseek(f, sizeof(hdr), SEEK_SET);
    if (csz <= 0 || hdr[3] == 0 || hdr[3] > 64u * 1024 * 1024) { fclose(f); return 0; }
    cb = (unsigned char*)malloc(csz);
    pix = (unsigned char*)malloc(hdr[3]);
    if (!cb || !pix || fread(cb, 1, csz, f) != (size_t)csz) {
        free(cb); free(pix); fclose(f); return 0;
    }
    fclose(f);
    /* Nunca passe metadados de cache corrompidos ao Mali-450. O driver antigo
     * pode segfaultar em glTexImage2D em vez de apenas devolver GL_INVALID_*.
     * Todos os payloads produzidos por este cache usam exatamente um destes
     * tres formatos e dimensoes nunca maiores que o asset original. Perfis
     * seletivos (fontes 3/4) tambem sao validos e entram na chave `flags`. */
    {
        unsigned int out_w = hdr[0], out_h = hdr[1], type = hdr[2];
        size_t expected = 0;
        int dimensions_ok = out_w > 0 && out_h > 0 &&
            out_w <= (unsigned int)w && out_h <= (unsigned int)h;
        if (type == 0x8D64u) { /* GL_ETC1_RGB8_OES */
            expected = (size_t)((out_w + 3u) / 4u) *
                       (size_t)((out_h + 3u) / 4u) * 8u;
        } else if (type == 0x8033u) { /* GL_UNSIGNED_SHORT_4_4_4_4 */
            expected = (size_t)out_w * (size_t)out_h * 2u;
        } else if (type == 0x1401u) { /* GL_UNSIGNED_BYTE */
            expected = (size_t)out_w * (size_t)out_h * 4u;
        }
        if (!dimensions_ok || expected == 0 || expected != hdr[3]) {
            static unsigned int rejected;
            if (rejected++ < 16)
                fprintf(stderr,
                        "[astc] tc2 rejeitado %ux%u type=0x%x len=%u (orig %dx%d)\n",
                        out_w, out_h, type, hdr[3], w, h);
            free(cb);
            return 0;
        }
    }
    dlen = hdr[3];
    if (p_uncompress(pix, &dlen, cb, (unsigned long)csz) != 0 || dlen != hdr[3]) {
        free(cb); free(pix); return 0;
    }
    free(cb);
    *dw = (int)hdr[0]; *dh = (int)hdr[1]; *gltype = hdr[2];
    return pix;
}
void nextos_tc2_store(const void *src, int src_size, int w, int h,
                      unsigned int flags, int dw, int dh, unsigned int gltype,
                      const void *pix, int pix_len) {
    const char *cd = tc_dir(); char path[600], tmp[640];
    unsigned long bound, clen; unsigned char *cb; FILE *f;
    unsigned int hdr[4];
    if (!cd || pix_len <= 0) return;
    snprintf(path, sizeof(path), "%s/%016llx_%dx%d_%08x.z2", cd,
             (unsigned long long)fnv1a((const unsigned char*)src, src_size),
             w, h, flags);
    bound = (unsigned long)pix_len + pix_len / 1000 + 64;
    cb = (unsigned char*)malloc(bound); if (!cb) return;
    clen = bound;
    if (p_compress2(cb, &clen, (const unsigned char*)pix,
                    (unsigned long)pix_len, 1) == 0) {
        hdr[0] = (unsigned)dw; hdr[1] = (unsigned)dh;
        hdr[2] = gltype; hdr[3] = (unsigned)pix_len;
        snprintf(tmp, sizeof(tmp), "%s.t%d", path, (int)getpid());
        f = fopen(tmp, "wb");
        if (f) {
            fwrite(hdr, sizeof(hdr), 1, f);
            fwrite(cb, 1, clen, f);
            fclose(f);
            rename(tmp, path);
        }
    }
    free(cb);
}

/* astcenc enum values */
#define ASTCENC_PRF_LDR 1
#define ASTCENC_TYPE_U8 0
#define ASTCENC_SWZ_R 0
#define ASTCENC_SWZ_G 1
#define ASTCENC_SWZ_B 2
#define ASTCENC_SWZ_A 3
#define ASTCENC_PRE_FASTEST 0.0f

/* Opaque pointer */
typedef struct astcenc_context astcenc_context;

/* astcenc_image: 5 fields = 24 bytes on aarch64 (dim_x, dim_y, dim_z, data_type, data*) */
struct astcenc_image {
    unsigned int dim_x;
    unsigned int dim_y;
    unsigned int dim_z;
    int data_type;
    void **data;
};

struct astcenc_swizzle {
    int r, g, b, a;
};

/* astcenc_config: over-allocate to be safe (real struct ~300 bytes worst case) */
typedef unsigned char astcenc_config_blob[2048];

/* Function pointers (loaded via dlsym with C++ mangled names) */
typedef int (*fn_config_init_t)(int profile, unsigned int block_x, unsigned int block_y,
                                unsigned int block_z, float quality, unsigned int flags,
                                void *config_out);
typedef int (*fn_context_alloc_t)(const void *config, unsigned int thread_count,
                                  astcenc_context **ctx_out);
typedef int (*fn_decompress_image_t)(astcenc_context *ctx, const unsigned char *data,
                                     size_t data_len, struct astcenc_image *image_out,
                                     const struct astcenc_swizzle *swizzle,
                                     unsigned int thread_index);
typedef int (*fn_decompress_reset_t)(astcenc_context *ctx);

static fn_config_init_t       p_config_init    = NULL;
static fn_context_alloc_t     p_context_alloc  = NULL;
static fn_decompress_image_t  p_decompress     = NULL;
static fn_decompress_reset_t  p_decompress_reset = NULL;

/* Cached contexts per (block_x, block_y) pair to avoid re-alloc per texture */
static astcenc_context *ctx_cache[16][16] = {0};
static int dlopen_attempted = 0;
static int dlopen_failed = 0;

static int load_libastcUtil(void) {
    if (dlopen_attempted) return dlopen_failed ? -1 : 0;
    dlopen_attempted = 1;

    /* Try multiple paths in order of preference (SB_LIBDIR primeiro) */
    char libdir_path[1024] = "";
    const char *libdir = getenv("SB_LIBDIR");
    if (libdir && *libdir)
        snprintf(libdir_path, sizeof libdir_path, "%s/libastcUtil.so", libdir);
    const char *paths[] = {
        libdir_path[0] ? libdir_path : "libastcUtil.so",
        "./libs/libastcUtil.so",
        "libastcUtil.so",
        NULL
    };
    void *h = NULL;
    for (int i = 0; paths[i] && !h; i++) {
        h = dlopen(paths[i], RTLD_NOW | RTLD_GLOBAL);
        if (!h) fprintf(stderr, "[NextOS-ASTC] dlopen %s failed: %s\n", paths[i], dlerror());
    }
    if (!h) {
        fprintf(stderr, "[NextOS-ASTC] libastcUtil.so not loadable\n");
        dlopen_failed = 1;
        return -1;
    }

    p_config_init = (fn_config_init_t) dlsym(h,
        "_Z19astcenc_config_init15astcenc_profilejjjfjP14astcenc_config");
    p_context_alloc = (fn_context_alloc_t) dlsym(h,
        "_Z21astcenc_context_allocPK14astcenc_configjPP15astcenc_context");
    p_decompress = (fn_decompress_image_t) dlsym(h,
        "_Z24astcenc_decompress_imageP15astcenc_contextPKhmP13astcenc_imagePK15astcenc_swizzlej");
    p_decompress_reset = (fn_decompress_reset_t) dlsym(h,
        "_Z24astcenc_decompress_resetP15astcenc_context");

    if (!p_config_init || !p_context_alloc || !p_decompress) {
        fprintf(stderr, "[NextOS-ASTC] symbol resolve failed cfg=%p ctx=%p dec=%p rst=%p\n",
                p_config_init, p_context_alloc, p_decompress, p_decompress_reset);
        dlopen_failed = 1;
        return -1;
    }
    fprintf(stderr, "[NextOS-ASTC] libastcUtil loaded OK\n");
    return 0;
}

static astcenc_context* get_context(int blk_x, int blk_y) {
    if (blk_x < 0 || blk_x >= 16 || blk_y < 0 || blk_y >= 16) return NULL;
    if (ctx_cache[blk_x][blk_y]) return ctx_cache[blk_x][blk_y];

    astcenc_config_blob config;
    memset(config, 0, sizeof(config));
    int rc = p_config_init(ASTCENC_PRF_LDR, blk_x, blk_y, 1, ASTCENC_PRE_FASTEST, 0, config);
    if (rc != 0) {
        fprintf(stderr, "[NextOS-ASTC] config_init %dx%d failed: %d\n", blk_x, blk_y, rc);
        return NULL;
    }
    astcenc_context *ctx = NULL;
    rc = p_context_alloc(config, 1, &ctx);
    if (rc != 0 || !ctx) {
        fprintf(stderr, "[NextOS-ASTC] context_alloc %dx%d failed: %d\n", blk_x, blk_y, rc);
        return NULL;
    }
    ctx_cache[blk_x][blk_y] = ctx;
    fprintf(stderr, "[NextOS-ASTC] context cached for %dx%d\n", blk_x, blk_y);
    return ctx;
}

/* Public: decode ASTC bytes to RGBA8.
 * dst: pointer to dst_size bytes; will be filled with w*h*4 RGBA8
 * src: ASTC compressed bytes
 * blk_x/blk_y: ASTC block dims
 * Returns 0 on success, non-zero on failure.
 */
int astc_decode_shim(void *dst, int dst_size, const void *src, int src_size,
                     int w, int h, int blk_x, int blk_y) {
    if (dst_size < w * h * 4) return -2;
    /* cache hit? evita carregar libastcUtil/decodar (rapido nas execucoes seguintes) */
    if (tc_load(dst, w * h * 4, src, src_size, w, h) == 0) return 0;
    if (load_libastcUtil() != 0) return -1;

    /* Verify src size: ASTC = (ceil(w/blk_x) * ceil(h/blk_y)) * 16 bytes */
    int blocks_x = (w + blk_x - 1) / blk_x;
    int blocks_y = (h + blk_y - 1) / blk_y;
    int expected_src = blocks_x * blocks_y * 16;
    if (src_size < expected_src) {
        /* Not enough source data; bail and let caller use fallback */
        return -3;
    }

    astcenc_context *ctx = get_context(blk_x, blk_y);
    if (!ctx) return -4;

    /* NextOS: decode to PADDED buffer to allow astcenc to write whole blocks
     * (block_w * block_h * 4 bytes) without overflowing into adjacent texture data.
     * Padded W/H rounds up to block alignment. Then copy back row-by-row to dst.
     */
    int pad_w = blocks_x * blk_x;
    int pad_h = blocks_y * blk_y;
    int pad_bytes = pad_w * pad_h * 4;
    unsigned char *pad_buf = (unsigned char*)malloc(pad_bytes);
    if (!pad_buf) {
        /* fallback to direct write to dst, same as before */
        struct astcenc_image img;
        img.dim_x = (unsigned)w;
        img.dim_y = (unsigned)h;
        img.dim_z = 1;
        img.data_type = ASTCENC_TYPE_U8;
        void *slice = dst;
        img.data = &slice;
        struct astcenc_swizzle swz = { ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A };
        int rc = p_decompress(ctx, (const unsigned char*)src, (size_t)src_size, &img, &swz, 0);
        if (p_decompress_reset) p_decompress_reset(ctx);
        if (rc != 0) return -5;
        tc_store(dst, w * h * 4, src, src_size, w, h);
        return 0;
    }

    struct astcenc_image img;
    img.dim_x = (unsigned)pad_w;
    img.dim_y = (unsigned)pad_h;
    img.dim_z = 1;
    img.data_type = ASTCENC_TYPE_U8;
    void *slice = pad_buf;
    img.data = &slice;

    struct astcenc_swizzle swz = { ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A };

    int rc = p_decompress(ctx, (const unsigned char*)src, (size_t)src_size, &img, &swz, 0);
    /* Reset context state for next decompress (required by astcenc API) */
    if (p_decompress_reset) p_decompress_reset(ctx);
    if (rc != 0) {
        fprintf(stderr, "[NextOS-ASTC] decompress %dx%d (padded %dx%d) blk %dx%d src=%d failed: %d\n",
                w, h, pad_w, pad_h, blk_x, blk_y, src_size, rc);
        free(pad_buf);
        return -5;
    }

    /* Copy w*h pixels from padded buffer to dst (row by row, dropping pad cols/rows) */
    unsigned char *dstb = (unsigned char*)dst;
    for (int y = 0; y < h; y++) {
        memcpy(dstb + y * w * 4, pad_buf + y * pad_w * 4, w * 4);
    }
    free(pad_buf);
    tc_store(dst, w * h * 4, src, src_size, w, h);
    return 0;
}
