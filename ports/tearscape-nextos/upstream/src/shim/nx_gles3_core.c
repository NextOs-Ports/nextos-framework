/* nx_gles3_core.c — hand-written heart of the GLES3->GLES2 shim.
 * v0: version lies, VAO replay emulation, CPU buffer shadows, fence=finish,
 * shader source capture + Mali compile error logging. Translation lands here
 * iteratively, driven by device logs. */
#define _GNU_SOURCE
#include "nx_shim.h"
#include "nx_gles3_frame_proof_state.h"
#include <GLES2/gl2ext.h>
#include <ctype.h>
#include <stdint.h>

/* ---- tiny helpers ------------------------------------------------------ */
#define REAL(ret, name, args) \
    static ret (*p_##name) args; \
    if (!p_##name) p_##name = nx_mali_sym(#name);

/* ---- glGetString / version lies ---------------------------------------- */
GL_APICALL const GLubyte *GL_APIENTRY glGetString(GLenum name)
{
    REAL(const GLubyte *, glGetString, (GLenum));
    switch (name) {
    case GL_VERSION:
        return (const GLubyte *)"OpenGL ES 3.0 (NextOS nxgles3 facade)";
    case GL_SHADING_LANGUAGE_VERSION:
        return (const GLubyte *)"OpenGL ES GLSL ES 3.00";
    default:
        return p_glGetString(name);
    }
}

static int ext_count;
static char **ext_list;

/* State owned by the logical GLES3 facade.  Keep this above glGetIntegerv:
 * frame proof resolves that public facade, not the physical GLES2 symbols. */
static GLuint bound_array, bound_element, bound_uniform, bound_copy_r, bound_copy_w;
static GLuint bound_pixel_pack, bound_pixel_unpack;
static GLuint nx_bound_fbo;

static void build_ext_list(void)
{
    REAL(const GLubyte *, glGetString, (GLenum));
    const char *s = (const char *)p_glGetString(GL_EXTENSIONS);
    if (!s) s = "";
    char *dup = strdup(s);
    int cap = 16;
    ext_list = malloc(cap * sizeof(char *));
    for (char *tok = strtok(dup, " "); tok; tok = strtok(NULL, " ")) {
        if (ext_count == cap) {
            cap *= 2;
            ext_list = realloc(ext_list, cap * sizeof(char *));
        }
        ext_list[ext_count++] = tok;
    }
    nx_log("extensions: %d (%s)", ext_count, s);
    {
        /* Real driver limits, logged once. Guessing these from the chip name is
         * how ports end up silently over the attribute or varying ceiling. */
        REAL(void, glGetIntegerv, (GLenum, GLint *));
        static const struct { GLenum e; const char *n; } lim[] = {
            {0x8869, "MAX_VERTEX_ATTRIBS"},
            {0x8DFB, "MAX_VERTEX_UNIFORM_VECTORS"},
            {0x8DFC, "MAX_VARYING_VECTORS"},
            {0x8DFD, "MAX_FRAGMENT_UNIFORM_VECTORS"},
            {0x8872, "MAX_TEXTURE_IMAGE_UNITS"},
            {0x8B4C, "MAX_VERTEX_TEXTURE_IMAGE_UNITS"},
            {0x0D33, "MAX_TEXTURE_SIZE"},
            {0x851C, "MAX_CUBE_MAP_TEXTURE_SIZE"},
        };
        for (unsigned i = 0; i < sizeof(lim) / sizeof(lim[0]); i++) {
            GLint v = -1;
            p_glGetIntegerv(lim[i].e, &v);
            nx_log("limit %s = %d", lim[i].n, v);
        }
        {
            GLint dims[2] = {-1, -1};
            p_glGetIntegerv(0x0D3A /*GL_MAX_VIEWPORT_DIMS*/, dims);
            nx_log("limit MAX_VIEWPORT_DIMS = %dx%d", dims[0], dims[1]);
        }
    }
}

GL_APICALL const GLubyte *GL_APIENTRY glGetStringi(GLenum name, GLuint index)
{
    if (name != GL_EXTENSIONS) return glGetString(name);
    if (!ext_list) build_ext_list();
    if ((int)index >= ext_count) return NULL;
    return (const GLubyte *)ext_list[index];
}

/* ---- glGetIntegerv lies ------------------------------------------------ */
GL_APICALL void GL_APIENTRY glGetIntegerv(GLenum pname, GLint *data)
{
    REAL(void, glGetIntegerv, (GLenum, GLint *));
    if (nx_gles3_frame_proof_state_query(
            pname, data, nx_bound_fbo, bound_pixel_pack))
        return;
    switch (pname) {
    case GL_MAJOR_VERSION: *data = 3; return;
    case GL_MINOR_VERSION: *data = 0; return;
    case GL_NUM_EXTENSIONS:
        if (!ext_list) build_ext_list();
        *data = ext_count;
        return;
    case GL_MAX_UNIFORM_BUFFER_BINDINGS: *data = 24; return;
    case GL_MAX_UNIFORM_BLOCK_SIZE: *data = 65536; return;
    case GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT: *data = 16; return;
    case GL_MAX_VERTEX_UNIFORM_BLOCKS: *data = 12; return;
    case GL_MAX_FRAGMENT_UNIFORM_BLOCKS: *data = 12; return;
    case GL_MAX_COMBINED_UNIFORM_BLOCKS: *data = 24; return;
    case GL_MAX_COLOR_ATTACHMENTS: *data = 1; return;
    case GL_MAX_DRAW_BUFFERS: *data = 1; return;
    case GL_MAX_ARRAY_TEXTURE_LAYERS: *data = 256; return;
    case GL_MAX_3D_TEXTURE_SIZE: *data = 256; return;
    case GL_MAX_ELEMENTS_INDICES: *data = 65536; return;
    case GL_MAX_ELEMENTS_VERTICES: *data = 65536; return;
    case GL_MAX_SAMPLES: *data = 1; return;
    case GL_MAX_VERTEX_OUTPUT_COMPONENTS: *data = 32; return;
    case GL_MAX_FRAGMENT_INPUT_COMPONENTS: *data = 32; return;
    case GL_MIN_PROGRAM_TEXEL_OFFSET: *data = 0; return;
    case GL_MAX_PROGRAM_TEXEL_OFFSET: *data = 0; return;
    case GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS: *data = 4; return;
    case GL_MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS: *data = 64; return;
    default:
        p_glGetIntegerv(pname, data);
    }
}

/* Number of values glGetIntegerv writes for a pname. Assuming one is a real
 * bug: Godot reads GL_MAX_VIEWPORT_DIMS as a pair, and leaving the second
 * element untouched made the engine clamp the window height to zero, so the
 * whole scene rendered into a viewport of height 0. */
static int nx_pname_value_count(GLenum pname)
{
    switch (pname) {
    case 0x0D3A: /* GL_MAX_VIEWPORT_DIMS */
    case 0x846D: /* GL_ALIASED_POINT_SIZE_RANGE */
    case 0x846E: /* GL_ALIASED_LINE_WIDTH_RANGE */
    case 0x0B12: /* GL_LINE_WIDTH_RANGE */
    case 0x0B70: /* GL_DEPTH_RANGE */
        return 2;
    case 0x0BA2: /* GL_VIEWPORT */
    case 0x0C10: /* GL_SCISSOR_BOX */
    case 0x0C22: /* GL_COLOR_CLEAR_VALUE */
    case 0x0C23: /* GL_COLOR_WRITEMASK */
    case 0x0D05: /* GL_PACK_ALIGNMENT is 1, but blend color is 4 */
        return pname == 0x0D05 ? 1 : 4;
    default:
        return 1;
    }
}

GL_APICALL void GL_APIENTRY glGetInteger64v(GLenum pname, GLint64 *data)
{
    GLint v[4] = {0, 0, 0, 0};
    int n = nx_pname_value_count(pname);
    glGetIntegerv(pname, v);
    for (int i = 0; i < n; i++) data[i] = v[i];
}

/* ---- buffer shadows (UBO emulation + instancing groundwork) ------------ */
typedef struct {
    GLuint id;
    GLenum last_target;
    void *data;
    size_t size;
} NxBuf;

#define NX_MAX_BUFS 4096
static NxBuf bufs[NX_MAX_BUFS];

static NxBuf *buf_get(GLuint id, int create)
{
    if (!id) return NULL;
    unsigned h = id % NX_MAX_BUFS;
    for (unsigned i = 0; i < NX_MAX_BUFS; i++) {
        NxBuf *b = &bufs[(h + i) % NX_MAX_BUFS];
        if (b->id == id) return b;
        if (!b->id && create) { b->id = id; return b; }
        if (!b->id) return NULL;
    }
    return NULL;
}


static unsigned buf_generation; /* bumped on every buffer upload */
extern unsigned long nx_draw_count;
static NxDrawStats nx_draw_stats;
void nx_check_gl_error(const char *where);
void nx_trace_draw(const char *what, GLsizei n);
void nx_probe_after_draw(const char *what);

static GLuint *bound_slot(GLenum target)
{
    switch (target) {
    case GL_ARRAY_BUFFER: return &bound_array;
    case GL_ELEMENT_ARRAY_BUFFER: return &bound_element;
    case GL_UNIFORM_BUFFER: return &bound_uniform;
    case GL_COPY_READ_BUFFER: return &bound_copy_r;
    case GL_COPY_WRITE_BUFFER: return &bound_copy_w;
    case GL_PIXEL_PACK_BUFFER: return &bound_pixel_pack;
    case GL_PIXEL_UNPACK_BUFFER: return &bound_pixel_unpack;
    default: return NULL;
    }
}

/* UBO binding points (glBindBufferBase) */
#define NX_MAX_UBO_BIND 32
static GLuint ubo_binding[NX_MAX_UBO_BIND];
static size_t ubo_bind_off[NX_MAX_UBO_BIND];

struct NxVao;
static void vao_track_element(GLuint id);

GL_APICALL void GL_APIENTRY glBindBuffer(GLenum target, GLuint buffer)
{
    REAL(void, glBindBuffer, (GLenum, GLuint));
    GLuint *slot = bound_slot(target);
    if (slot) *slot = buffer;
    if (target == GL_ELEMENT_ARRAY_BUFFER)
        vao_track_element(buffer);
    /* GLES2 blob only understands ARRAY/ELEMENT; keep the rest CPU-side. */
    if (target == GL_ARRAY_BUFFER || target == GL_ELEMENT_ARRAY_BUFFER)
        p_glBindBuffer(target, buffer);
}

GL_APICALL void GL_APIENTRY glBufferData(GLenum target, GLsizeiptr size,
                                         const void *data, GLenum usage)
{
    REAL(void, glBufferData, (GLenum, GLsizeiptr, const void *, GLenum));
    GLuint *slot = bound_slot(target);
    GLuint id = slot ? *slot : 0;
    NxBuf *b = buf_get(id, 1);
    if (b) {
        b->last_target = target;
        if (b->size != (size_t)size) {
            free(b->data);
            b->data = malloc(size > 0 ? size : 1);
            b->size = size;
        }
        if (data) memcpy(b->data, data, size);
        else memset(b->data, 0, size);
        buf_generation++;
    }
    if (target == GL_ARRAY_BUFFER || target == GL_ELEMENT_ARRAY_BUFFER)
        p_glBufferData(target, size, data, usage);
}

GL_APICALL void GL_APIENTRY glBufferSubData(GLenum target, GLintptr off,
                                            GLsizeiptr size, const void *data)
{
    REAL(void, glBufferSubData, (GLenum, GLintptr, GLsizeiptr, const void *));
    GLuint *slot = bound_slot(target);
    NxBuf *b = buf_get(slot ? *slot : 0, 0);
    if (b && b->data && (size_t)(off + size) <= b->size && data) {
        memcpy((char *)b->data + off, data, size);
        buf_generation++;
    }
    if (target == GL_ARRAY_BUFFER || target == GL_ELEMENT_ARRAY_BUFFER)
        p_glBufferSubData(target, off, size, data);
}

GL_APICALL void GL_APIENTRY glDeleteBuffers(GLsizei n, const GLuint *ids)
{
    REAL(void, glDeleteBuffers, (GLsizei, const GLuint *));
    for (GLsizei i = 0; i < n; i++) {
        GLuint id = ids[i];
        NxBuf *b = buf_get(ids[i], 0);
        if (b) { free(b->data); memset(b, 0, sizeof(*b)); }
        if (bound_array == id) bound_array = 0;
        if (bound_element == id) {
            bound_element = 0;
            vao_track_element(0);
        }
        if (bound_uniform == id) bound_uniform = 0;
        if (bound_copy_r == id) bound_copy_r = 0;
        if (bound_copy_w == id) bound_copy_w = 0;
        if (bound_pixel_pack == id) bound_pixel_pack = 0;
        if (bound_pixel_unpack == id) bound_pixel_unpack = 0;
        for (unsigned binding = 0; binding < NX_MAX_UBO_BIND; binding++) {
            if (ubo_binding[binding] == id) {
                ubo_binding[binding] = 0;
                ubo_bind_off[binding] = 0;
            }
        }
    }
    p_glDeleteBuffers(n, ids);
}

GL_APICALL void GL_APIENTRY glCopyBufferSubData(GLenum rt, GLenum wt,
                                                GLintptr ro, GLintptr wo,
                                                GLsizeiptr size)
{
    GLuint *rs = bound_slot(rt), *ws = bound_slot(wt);
    NxBuf *rb = buf_get(rs ? *rs : 0, 0);
    NxBuf *wb = buf_get(ws ? *ws : 0, 0);
    if (rb && wb && rb->data && wb->data)
        memcpy((char *)wb->data + wo, (char *)rb->data + ro, size);
    /* refresh GPU copy if destination is a real GLES2 target */
    if (wb && (wt == GL_ARRAY_BUFFER || wt == GL_ELEMENT_ARRAY_BUFFER)) {
        REAL(void, glBufferSubData, (GLenum, GLintptr, GLsizeiptr, const void *));
        p_glBufferSubData(wt, wo, size, (char *)wb->data + wo);
    }
}

/* glMapBufferRange via shadow + upload on unmap */
static NxBuf *mapped_buf;
static GLintptr mapped_off;
static GLsizeiptr mapped_len;
static GLenum mapped_target;

GL_APICALL void *GL_APIENTRY glMapBufferRange(GLenum target, GLintptr off,
                                              GLsizeiptr len, GLbitfield access)
{
    GLuint *slot = bound_slot(target);
    NxBuf *b = buf_get(slot ? *slot : 0, 0);
    if (!b || !b->data || (size_t)(off + len) > b->size) {
        NX_STUB_LOG("glMapBufferRange-miss");
        return NULL;
    }
    mapped_buf = b; mapped_off = off; mapped_len = len; mapped_target = target;
    return (char *)b->data + off;
}

GL_APICALL GLboolean GL_APIENTRY glUnmapBuffer(GLenum target)
{
    if (mapped_buf && (target == GL_ARRAY_BUFFER || target == GL_ELEMENT_ARRAY_BUFFER)) {
        REAL(void, glBufferSubData, (GLenum, GLintptr, GLsizeiptr, const void *));
        p_glBufferSubData(target, mapped_off, mapped_len,
                          (char *)mapped_buf->data + mapped_off);
    }
    mapped_buf = NULL;
    return GL_TRUE;
}

GL_APICALL void GL_APIENTRY glFlushMappedBufferRange(GLenum t, GLintptr o, GLsizeiptr l)
{
    (void)t; (void)o; (void)l;
}

GL_APICALL void GL_APIENTRY glBindBufferBase(GLenum target, GLuint index, GLuint buffer)
{
    if (target == GL_UNIFORM_BUFFER && index < NX_MAX_UBO_BIND) {
        ubo_binding[index] = buffer;
        ubo_bind_off[index] = 0;
        bound_uniform = buffer;
    }
}

GL_APICALL void GL_APIENTRY glBindBufferRange(GLenum target, GLuint index,
                                              GLuint buffer, GLintptr off, GLsizeiptr size)
{
    (void)size;
    if (target == GL_UNIFORM_BUFFER && index < NX_MAX_UBO_BIND) {
        ubo_binding[index] = buffer;
        ubo_bind_off[index] = off;
        bound_uniform = buffer;
    }
}

/* ---- VAO emulation (replay) -------------------------------------------- */
#define NX_MAX_ATTRS 16
typedef struct NxVao {
    GLuint id;
    GLuint element;
    struct {
        GLuint enabled;
        GLuint buffer;
        GLint size;
        GLenum type;
        GLboolean norm;
        GLsizei stride;
        const void *ptr;
        GLuint divisor;
        GLuint integer;
    } attr[NX_MAX_ATTRS];
} NxVao;

#define NX_MAX_VAOS 512
static NxVao vaos[NX_MAX_VAOS];
static NxVao vao0;          /* default VAO */
static NxVao *cur_vao = &vao0;
static GLuint next_vao = 1;

static NxVao *vao_get(GLuint id)
{
    if (!id) return &vao0;
    for (int i = 0; i < NX_MAX_VAOS; i++)
        if (vaos[i].id == id) return &vaos[i];
    return NULL;
}

static void vao_track_element(GLuint id)
{
    cur_vao->element = id;
}

GL_APICALL void GL_APIENTRY glGenVertexArrays(GLsizei n, GLuint *arrays)
{
    for (GLsizei i = 0; i < n; i++) {
        GLuint id = next_vao++;
        for (int j = 0; j < NX_MAX_VAOS; j++) {
            if (!vaos[j].id) { vaos[j].id = id; break; }
        }
        arrays[i] = id;
    }
}

GL_APICALL void GL_APIENTRY glDeleteVertexArrays(GLsizei n, const GLuint *arrays)
{
    for (GLsizei i = 0; i < n; i++) {
        NxVao *v = vao_get(arrays[i]);
        if (v && v != &vao0) memset(v, 0, sizeof(*v));
    }
}

GL_APICALL GLboolean GL_APIENTRY glIsVertexArray(GLuint id)
{
    return vao_get(id) ? GL_TRUE : GL_FALSE;
}

GL_APICALL void GL_APIENTRY glBindVertexArray(GLuint id)
{
    REAL(void, glBindBuffer, (GLenum, GLuint));
    REAL(void, glEnableVertexAttribArray, (GLuint));
    REAL(void, glDisableVertexAttribArray, (GLuint));
    REAL(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *));
    NxVao *v = vao_get(id);
    if (!v) return;
    cur_vao = v;
    /* replay */
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, v->element);
    bound_element = v->element;
    for (int i = 0; i < NX_MAX_ATTRS; i++) {
        if (v->attr[i].enabled) {
            p_glEnableVertexAttribArray(i);
            if (v->attr[i].divisor == 0) {
                p_glBindBuffer(GL_ARRAY_BUFFER, v->attr[i].buffer);
                p_glVertexAttribPointer(i, v->attr[i].size, v->attr[i].type,
                                        v->attr[i].norm, v->attr[i].stride,
                                        v->attr[i].ptr);
            }
        } else {
            p_glDisableVertexAttribArray(i);
        }
    }
    p_glBindBuffer(GL_ARRAY_BUFFER, bound_array);
}

GL_APICALL void GL_APIENTRY glEnableVertexAttribArray(GLuint index)
{
    REAL(void, glEnableVertexAttribArray, (GLuint));
    if (index < NX_MAX_ATTRS) cur_vao->attr[index].enabled = 1;
    p_glEnableVertexAttribArray(index);
}

GL_APICALL void GL_APIENTRY glDisableVertexAttribArray(GLuint index)
{
    REAL(void, glDisableVertexAttribArray, (GLuint));
    if (index < NX_MAX_ATTRS) cur_vao->attr[index].enabled = 0;
    p_glDisableVertexAttribArray(index);
}

GL_APICALL void GL_APIENTRY glVertexAttribPointer(GLuint idx, GLint size,
                                                  GLenum type, GLboolean norm,
                                                  GLsizei stride, const void *ptr)
{
    REAL(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *));
    if (idx < NX_MAX_ATTRS) {
        cur_vao->attr[idx].buffer = bound_array;
        cur_vao->attr[idx].size = size;
        cur_vao->attr[idx].type = type;
        cur_vao->attr[idx].norm = norm;
        cur_vao->attr[idx].stride = stride;
        cur_vao->attr[idx].ptr = ptr;
        cur_vao->attr[idx].integer = 0;
    }
    p_glVertexAttribPointer(idx, size, type, norm, stride, ptr);
}

GL_APICALL void GL_APIENTRY glVertexAttribDivisor(GLuint idx, GLuint div)
{
    if (idx < NX_MAX_ATTRS) cur_vao->attr[idx].divisor = div;
}

GL_APICALL void GL_APIENTRY glVertexAttribIPointer(GLuint idx, GLint size, GLenum type,
                                                   GLsizei stride, const void *ptr)
{
    /* ES2 has no integer attribs: recorded only; delivered as float
     * constants by the instanced-draw emulation. */
    if (idx < NX_MAX_ATTRS) {
        cur_vao->attr[idx].buffer = bound_array;
        cur_vao->attr[idx].size = size;
        cur_vao->attr[idx].type = type;
        cur_vao->attr[idx].norm = GL_FALSE;
        cur_vao->attr[idx].stride = stride;
        cur_vao->attr[idx].ptr = ptr;
        cur_vao->attr[idx].integer = 1;
    }
}

/* Read a pixel straight back from the bound framebuffer after a draw. When a
 * frame comes out empty this separates "the draw was rejected" from "the draw
 * ran and wrote nothing visible". */
void nx_probe_after_draw(const char *what)
{
    static int budget = -1;
    if (budget < 0) {
        const char *value = getenv("NX_DRAW_PROBE");
        budget = value && !strcmp(value, "1") ? 40 : 0;
    }
    if (budget <= 0) return;
    budget--;
    REAL(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *));
    REAL(void, glGetIntegerv, (GLenum, GLint *));
    GLint fbo = 0, vp[4] = {0};
    p_glGetIntegerv(0x8CA6, &fbo);
    p_glGetIntegerv(0x0BA2, vp);
    unsigned char px[4] = {9, 9, 9, 9};
    p_glReadPixels(vp[0] + vp[2] / 2, vp[1] + vp[3] / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    nx_log("probe after %s: fbo=%d center=%u,%u,%u,%u", what, fbo, px[0], px[1], px[2], px[3]);
}

void nx_trace_draw(const char *what, GLsizei n)
{
    /* Startup-only diagnostic.  Periodic state queries made the Mali driver
     * synchronize several times during normal gameplay and produced visible
     * frame-time spikes even though the average FPS looked acceptable. */
    static int traced;
    if (traced >= 40) return;
    traced++;
    REAL(void, glGetIntegerv, (GLenum, GLint *));
    GLint fbo = 0, prog = 0, vp[4] = {0};
    p_glGetIntegerv(0x8CA6 /*GL_FRAMEBUFFER_BINDING*/, &fbo);
    p_glGetIntegerv(0x8B8D /*GL_CURRENT_PROGRAM*/, &prog);
    p_glGetIntegerv(0x0BA2 /*GL_VIEWPORT*/, vp);
    REAL(GLboolean, glIsEnabled, (GLenum));
    REAL(void, glGetBooleanv, (GLenum, GLboolean *));
    GLint sc[4] = {0};
    GLboolean cmask[4] = {0};
    p_glGetIntegerv(0x0C10 /*GL_SCISSOR_BOX*/, sc);
    p_glGetBooleanv(0x0C23 /*GL_COLOR_WRITEMASK*/, cmask);
    REAL(GLenum, glCheckFramebufferStatus, (GLenum));
    GLenum fbst = p_glCheckFramebufferStatus(GL_FRAMEBUFFER);
    nx_log("draw %s n=%d fbo=%d(st=0x%x) prog=%d vp=%d,%d %dx%d scissor=%d[%d,%d %dx%d] blend=%d depth=%d cull=%d mask=%d%d%d%d",
           what, n, fbo, (int)fbst, prog, vp[0], vp[1], vp[2], vp[3],
           (int)p_glIsEnabled(GL_SCISSOR_TEST), sc[0], sc[1], sc[2], sc[3],
           (int)p_glIsEnabled(GL_BLEND), (int)p_glIsEnabled(GL_DEPTH_TEST),
           (int)p_glIsEnabled(GL_CULL_FACE),
           (int)cmask[0], (int)cmask[1], (int)cmask[2], (int)cmask[3]);
}

void nx_check_gl_error(const char *where)
{
    REAL(GLenum, glGetError, (void));
    /* Deduplicating by error code alone hides which call site is failing and
     * how often, which is exactly what a blank frame needs to be diagnosed. */
    static int logged;
    GLenum e = p_glGetError();
    if (!e) return;
    if (logged < 30) {
        logged++;
        nx_log("GL ERROR 0x%x at %s (draw #%lu)", e, where, nx_draw_count);
    }
}

static int nx_deep_gl_diagnostics_enabled(void)
{
    static int initialized;
    static int enabled;
    if (!initialized) {
        const char *value = getenv("NX_DEEP_GL_DIAGNOSTICS");
        enabled = value && !strcmp(value, "1");
        initialized = 1;
    }
    return enabled;
}

/* ---- fences: glFinish semantics ---------------------------------------- */
GL_APICALL GLsync GL_APIENTRY glFenceSync(GLenum cond, GLbitfield flags)
{
    (void)cond; (void)flags;
    return (GLsync)(uintptr_t)0xF0F0;
}
GL_APICALL void GL_APIENTRY glDeleteSync(GLsync s) { (void)s; }
GL_APICALL GLboolean GL_APIENTRY glIsSync(GLsync s) { return s != 0; }
GL_APICALL GLenum GL_APIENTRY glClientWaitSync(GLsync s, GLbitfield f, GLuint64 t)
{
    (void)s; (void)f; (void)t;
    REAL(void, glFinish, (void));
    p_glFinish();
    return GL_ALREADY_SIGNALED;
}
GL_APICALL void GL_APIENTRY glWaitSync(GLsync s, GLbitfield f, GLuint64 t)
{
    (void)s; (void)f; (void)t;
}
GL_APICALL void GL_APIENTRY glGetSynciv(GLsync s, GLenum pname, GLsizei sz,
                                        GLsizei *len, GLint *values)
{
    (void)s; (void)sz;
    if (pname == GL_SYNC_STATUS && values) *values = GL_SIGNALED;
    if (len) *len = 1;
}

/* ---- samplers: record + apply at bind (v0: no-op with log) -------------- */
GL_APICALL void GL_APIENTRY glGenSamplers(GLsizei n, GLuint *s)
{
    static GLuint next = 1;
    for (GLsizei i = 0; i < n; i++) s[i] = next++;
}
GL_APICALL void GL_APIENTRY glDeleteSamplers(GLsizei n, const GLuint *s) { (void)n; (void)s; }
GL_APICALL GLboolean GL_APIENTRY glIsSampler(GLuint s) { return s != 0; }
GL_APICALL void GL_APIENTRY glBindSampler(GLuint unit, GLuint s)
{
    (void)unit; (void)s;
    NX_STUB_LOG("glBindSampler");
}
GL_APICALL void GL_APIENTRY glSamplerParameteri(GLuint s, GLenum p, GLint v) { (void)s;(void)p;(void)v; }
GL_APICALL void GL_APIENTRY glSamplerParameterf(GLuint s, GLenum p, GLfloat v) { (void)s;(void)p;(void)v; }
GL_APICALL void GL_APIENTRY glSamplerParameteriv(GLuint s, GLenum p, const GLint *v) { (void)s;(void)p;(void)v; }
GL_APICALL void GL_APIENTRY glSamplerParameterfv(GLuint s, GLenum p, const GLfloat *v) { (void)s;(void)p;(void)v; }
GL_APICALL void GL_APIENTRY glGetSamplerParameteriv(GLuint s, GLenum p, GLint *v) { (void)s;(void)p; if (v) *v = 0; }
GL_APICALL void GL_APIENTRY glGetSamplerParameterfv(GLuint s, GLenum p, GLfloat *v) { (void)s;(void)p; if (v) *v = 0; }

/* ---- misc ES3 entries --------------------------------------------------- */
GL_APICALL void GL_APIENTRY glReadBuffer(GLenum m) { (void)m; }
GL_APICALL void GL_APIENTRY glDrawBuffers(GLsizei n, const GLenum *bufs_)
{
    (void)bufs_;
    if (n > 1) NX_STUB_LOG("glDrawBuffers>1");
}
GL_APICALL void GL_APIENTRY glInvalidateFramebuffer(GLenum t, GLsizei n, const GLenum *a)
{
    (void)t; (void)n; (void)a;
}
GL_APICALL void GL_APIENTRY glInvalidateSubFramebuffer(GLenum t, GLsizei n, const GLenum *a,
                                                       GLint x, GLint y, GLsizei w, GLsizei h)
{
    (void)t; (void)n; (void)a; (void)x; (void)y; (void)w; (void)h;
}
GL_APICALL void GL_APIENTRY glBlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1,
                                              GLint dx0, GLint dy0, GLint dx1, GLint dy1,
                                              GLbitfield mask, GLenum filter)
{
    (void)sx0;(void)sy0;(void)sx1;(void)sy1;(void)dx0;(void)dy0;(void)dx1;(void)dy1;(void)mask;(void)filter;
    NX_STUB_LOG("glBlitFramebuffer");
}

/* ---- instanced draws: per-instance attribs as float constants ----------- */

/* corner attrib: substitutes gl_VertexID for the canvas quad/primitive path */
static GLuint corner_vbo;

static void ensure_corner(void)
{
    REAL(void, glGenBuffers, (GLsizei, GLuint *));
    REAL(void, glBindBuffer, (GLenum, GLuint));
    REAL(void, glBufferData, (GLenum, GLsizeiptr, const void *, GLenum));
    if (corner_vbo) return;
    static const GLfloat corners[1024 % 64 ? 64 : 64] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
        16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
        32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
        48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63
    };
    p_glGenBuffers(1, &corner_vbo);
    p_glBindBuffer(GL_ARRAY_BUFFER, corner_vbo);
    p_glBufferData(GL_ARRAY_BUFFER, sizeof(corners), corners, GL_STATIC_DRAW);
    p_glBindBuffer(GL_ARRAY_BUFFER, bound_array);
}

/* scratch USHORT element buffers for UINT index draws (Mali has no
 * OES_element_index_uint); keyed by source element buffer id. */
typedef struct { GLuint src; GLuint scratch; unsigned gen; } NxIdx;
#define NX_MAX_IDX 64
static NxIdx idxcache[NX_MAX_IDX];


static GLuint ushort_scratch_for(GLuint src)
{
    REAL(void, glGenBuffers, (GLsizei, GLuint *));
    REAL(void, glBindBuffer, (GLenum, GLuint));
    REAL(void, glBufferData, (GLenum, GLsizeiptr, const void *, GLenum));
    NxBuf *b = buf_get(src, 0);
    if (!b || !b->data) return 0;
    NxIdx *slot = NULL;
    for (int i = 0; i < NX_MAX_IDX; i++) {
        if (idxcache[i].src == src) { slot = &idxcache[i]; break; }
        if (!idxcache[i].src && !slot) slot = &idxcache[i];
    }
    if (!slot) slot = &idxcache[0];
    if (slot->src != src || slot->gen != buf_generation) {
        if (!slot->scratch) p_glGenBuffers(1, &slot->scratch);
        size_t n = b->size / 4;
        GLushort *tmp = malloc(n * 2);
        const GLuint *u = (const GLuint *)b->data;
        for (size_t i = 0; i < n; i++) tmp[i] = (GLushort)u[i];
        p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, slot->scratch);
        p_glBufferData(GL_ELEMENT_ARRAY_BUFFER, n * 2, tmp, GL_DYNAMIC_DRAW);
        free(tmp);
        slot->src = src;
        slot->gen = buf_generation;
    } else {
        p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, slot->scratch);
    }
    return slot->scratch;
}

static void set_instance_constants(GLsizei inst)
{
    REAL(void, glVertexAttrib4f, (GLuint, GLfloat, GLfloat, GLfloat, GLfloat));
    for (int i = 0; i < NX_MAX_ATTRS; i++) {
        if (!cur_vao->attr[i].enabled || cur_vao->attr[i].divisor == 0) continue;
        NxBuf *b = buf_get(cur_vao->attr[i].buffer, 0);
        if (!b || !b->data) continue;
        size_t base = (size_t)(uintptr_t)cur_vao->attr[i].ptr +
                      (size_t)inst * (cur_vao->attr[i].stride ? cur_vao->attr[i].stride
                                                             : cur_vao->attr[i].size * 4);
        if (base + cur_vao->attr[i].size * 4 > b->size) continue;
        GLfloat v[4] = {0, 0, 0, 1};
        const char *src = (const char *)b->data + base;
        for (int c = 0; c < cur_vao->attr[i].size && c < 4; c++) {
            if (cur_vao->attr[i].type == GL_FLOAT)
                v[c] = ((const GLfloat *)src)[c];
            else if (cur_vao->attr[i].type == GL_UNSIGNED_INT)
                v[c] = (GLfloat)((const GLuint *)src)[c];
            else if (cur_vao->attr[i].type == GL_INT)
                v[c] = (GLfloat)((const GLint *)src)[c];
        }
        p_glVertexAttrib4f(i, v[0], v[1], v[2], v[3]);
    }
}

static void nx_log_large_instanced_layout(const char *kind, GLenum mode,
                                           GLsizei count, GLenum type,
                                           const void *indices, GLsizei prim)
{
    static int logged;
    const char *batch_debug = getenv("NX_BATCH_INSTANCING");
    if (logged || (prim < 64 && (!batch_debug || strcmp(batch_debug, "1"))))
        return;
    logged = 1;
    NxBuf *element = buf_get(bound_element, 0);
    nx_log("large %s mode=0x%x count=%d type=0x%x indices=%p prim=%d element=%u/%zu",
           kind, mode, count, type, indices, prim, bound_element,
           element ? element->size : 0);
    for (int i = 0; i < NX_MAX_ATTRS; i++) {
        if (!cur_vao->attr[i].enabled)
            continue;
        NxBuf *buffer = buf_get(cur_vao->attr[i].buffer, 0);
        nx_log("large attr=%d buffer=%u/%zu size=%d type=0x%x norm=%u stride=%d ptr=%p divisor=%u integer=%u",
               i, cur_vao->attr[i].buffer, buffer ? buffer->size : 0,
               cur_vao->attr[i].size, cur_vao->attr[i].type,
               (unsigned)cur_vao->attr[i].norm, cur_vao->attr[i].stride,
               cur_vao->attr[i].ptr, cur_vao->attr[i].divisor,
               cur_vao->attr[i].integer);
    }
}

static void nx_note_instanced(int arrays, GLsizei count, GLsizei prim)
{
    unsigned bin;
    nx_draw_stats.api_draws++;
    nx_draw_stats.instanced_calls++;
    if (arrays)
        nx_draw_stats.instanced_arrays++;
    else
        nx_draw_stats.instanced_elements++;
    if (prim > 0) {
        unsigned long instances = (unsigned long)prim;
        nx_draw_stats.instanced_instances += instances;
        nx_draw_stats.instanced_vertices += instances * (unsigned long)(count > 0 ? count : 0);
        if (instances > nx_draw_stats.instanced_max)
            nx_draw_stats.instanced_max = instances;
        if (instances <= 1) bin = 0;
        else if (instances <= 3) bin = 1;
        else if (instances <= 7) bin = 2;
        else if (instances <= 15) bin = 3;
        else if (instances <= 31) bin = 4;
        else if (instances <= 63) bin = 5;
        else if (instances <= 127) bin = 6;
        else bin = 7;
        nx_draw_stats.instanced_hist[bin]++;
    }
}

/* Godot's GLES3 canvas submits every sprite as an element-instanced quad.
 * The Mali-450 GLES2 blob has no instancing extension, so the conservative
 * fallback above repeats one real draw per sprite.  For the exact, verified
 * canvas layout used by this build, expand those quads into client-side
 * vertex arrays and submit the entire Godot batch in one GLES2 draw.
 *
 * This remains opt-in while it is validated on hardware.  Every layout or
 * bounds mismatch falls back to the already working per-instance path. */
#define NX_CANVAS_BATCH_ATTR_FIRST 6
#define NX_CANVAS_BATCH_ATTR_COUNT 9
#define NX_CANVAS_BATCH_STRIDE 144
#define NX_CANVAS_BATCH_FLOATS (1 + NX_CANVAS_BATCH_ATTR_COUNT * 4)
static GLfloat *nx_canvas_batch_cpu;
static size_t nx_canvas_batch_capacity;

static int nx_canvas_batch_enabled(void)
{
    static int initialized;
    static int enabled;
    if (!initialized) {
        const char *value = getenv("NX_BATCH_INSTANCING");
        if (value) {
            enabled = !strcmp(value, "1");
        } else {
            const char *defaults = getenv("NX_TEARSCAPE_DEFAULTS");
            enabled = defaults && !strcmp(defaults, "1");
        }
        initialized = 1;
        nx_log("canvas CPU batching: %s", enabled ? "enabled" : "disabled");
    }
    return enabled;
}

static int nx_try_batch_canvas_elements(GLenum mode, GLsizei count,
                                         GLenum type, const void *idx,
                                         GLsizei prim)
{
    REAL(void, glBindBuffer, (GLenum, GLuint));
    REAL(void, glEnableVertexAttribArray, (GLuint));
    REAL(void, glDisableVertexAttribArray, (GLuint));
    REAL(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *));
    REAL(void, glDrawArrays, (GLenum, GLint, GLsizei));

    if (!nx_canvas_batch_enabled())
        return 0;
    if (mode != GL_TRIANGLES || count != 6 || prim < 2 ||
        type != GL_UNSIGNED_INT || cur_vao->attr[0].enabled)
        goto fallback;

    GLuint source_buffer = 0;
    for (int i = 0; i < NX_MAX_ATTRS; i++) {
        int canvas_attr = i >= NX_CANVAS_BATCH_ATTR_FIRST &&
                          i < NX_CANVAS_BATCH_ATTR_FIRST + NX_CANVAS_BATCH_ATTR_COUNT;
        if (!!cur_vao->attr[i].enabled != canvas_attr)
            goto fallback;
        if (!canvas_attr)
            continue;
        if (cur_vao->attr[i].size != 4 ||
            cur_vao->attr[i].stride != NX_CANVAS_BATCH_STRIDE ||
            cur_vao->attr[i].divisor != 1 || !cur_vao->attr[i].buffer)
            goto fallback;
        if (cur_vao->attr[i].type != GL_FLOAT || cur_vao->attr[i].integer)
            goto fallback;
        if (!source_buffer)
            source_buffer = cur_vao->attr[i].buffer;
        else if (source_buffer != cur_vao->attr[i].buffer)
            goto fallback;
    }

    NxBuf *elements = buf_get(bound_element, 0);
    NxBuf *instances = buf_get(source_buffer, 0);
    size_t index_offset = (size_t)(uintptr_t)idx;
    if (!elements || !elements->data || !instances || !instances->data ||
        index_offset > elements->size ||
        (size_t)count * sizeof(GLuint) > elements->size - index_offset)
        goto fallback;

    GLuint corners[6];
    memcpy(corners, (const char *)elements->data + index_offset, sizeof(corners));
    for (int vertex = 0; vertex < count; vertex++)
        if (corners[vertex] > 63)
            goto fallback;

    for (int a = 0; a < NX_CANVAS_BATCH_ATTR_COUNT; a++) {
        int i = NX_CANVAS_BATCH_ATTR_FIRST + a;
        size_t attr_offset = (size_t)(uintptr_t)cur_vao->attr[i].ptr;
        size_t last_offset = attr_offset +
                             (size_t)(prim - 1) * NX_CANVAS_BATCH_STRIDE;
        if (attr_offset > instances->size || last_offset > instances->size ||
            4 * sizeof(GLuint) > instances->size - last_offset)
            goto fallback;
    }

    size_t vertices = (size_t)count * (size_t)prim;
    if (vertices > (size_t)INT32_MAX ||
        vertices > SIZE_MAX / (NX_CANVAS_BATCH_FLOATS * sizeof(GLfloat)))
        goto fallback;
    size_t bytes = vertices * NX_CANVAS_BATCH_FLOATS * sizeof(GLfloat);
    if (bytes > nx_canvas_batch_capacity) {
        size_t capacity = nx_canvas_batch_capacity ? nx_canvas_batch_capacity : 65536;
        while (capacity < bytes && capacity <= SIZE_MAX / 2)
            capacity *= 2;
        if (capacity < bytes)
            goto fallback;
        GLfloat *grown = realloc(nx_canvas_batch_cpu, capacity);
        if (!grown)
            goto fallback;
        nx_canvas_batch_cpu = grown;
        nx_canvas_batch_capacity = capacity;
    }

    for (GLsizei instance = 0; instance < prim; instance++) {
        GLfloat values[NX_CANVAS_BATCH_ATTR_COUNT][4];
        for (int a = 0; a < NX_CANVAS_BATCH_ATTR_COUNT; a++) {
            int i = NX_CANVAS_BATCH_ATTR_FIRST + a;
            size_t source_offset = (size_t)(uintptr_t)cur_vao->attr[i].ptr +
                                   (size_t)instance * NX_CANVAS_BATCH_STRIDE;
            const char *source = (const char *)instances->data + source_offset;
            memcpy(values[a], source, 4 * sizeof(GLfloat));
        }
        for (int vertex = 0; vertex < count; vertex++) {
            size_t output_vertex = (size_t)instance * (size_t)count + (size_t)vertex;
            GLfloat *output = nx_canvas_batch_cpu + output_vertex * NX_CANVAS_BATCH_FLOATS;
            output[0] = (GLfloat)corners[vertex];
            memcpy(output + 1, values, sizeof(values));
        }
    }

    /* GLES2 permits client-side vertex arrays when ARRAY_BUFFER is zero.
     * The draw consumes the memory before returning, so the persistent CPU
     * scratch can be reused immediately for the next Godot batch. */
    p_glBindBuffer(GL_ARRAY_BUFFER, 0);
    p_glEnableVertexAttribArray(0);
    p_glVertexAttribPointer(0, 1, GL_FLOAT, GL_FALSE,
                            NX_CANVAS_BATCH_FLOATS * sizeof(GLfloat),
                            nx_canvas_batch_cpu);
    for (int a = 0; a < NX_CANVAS_BATCH_ATTR_COUNT; a++) {
        GLuint i = NX_CANVAS_BATCH_ATTR_FIRST + a;
        p_glEnableVertexAttribArray(i);
        p_glVertexAttribPointer(i, 4, GL_FLOAT, GL_FALSE,
                                NX_CANVAS_BATCH_FLOATS * sizeof(GLfloat),
                                nx_canvas_batch_cpu + 1 + a * 4);
    }
    p_glDrawArrays(mode, 0, (GLsizei)vertices);
    p_glDisableVertexAttribArray(0);
    for (int a = 0; a < NX_CANVAS_BATCH_ATTR_COUNT; a++)
        p_glDisableVertexAttribArray(NX_CANVAS_BATCH_ATTR_FIRST + a);
    p_glBindBuffer(GL_ARRAY_BUFFER, bound_array);

    nx_draw_stats.batched_calls++;
    nx_draw_stats.batched_vertices += vertices;
    return 1;

fallback:
    nx_draw_stats.batch_fallbacks++;
    return 0;
}

/* enable/disable real arrays to match instanced expectations; also inject
 * the corner attrib at slot 0 when the draw has no per-vertex attribs. */
static int prep_instanced_arrays(void)
{
    REAL(void, glEnableVertexAttribArray, (GLuint));
    REAL(void, glDisableVertexAttribArray, (GLuint));
    REAL(void, glBindBuffer, (GLenum, GLuint));
    REAL(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *));
    int used_corner = 0;
    int has_vertex_array = 0;
    for (int i = 0; i < NX_MAX_ATTRS; i++) {
        if (!cur_vao->attr[i].enabled) continue;
        if (cur_vao->attr[i].divisor > 0)
            p_glDisableVertexAttribArray(i);
        else
            has_vertex_array = 1;
    }
    if (!has_vertex_array && !cur_vao->attr[0].enabled) {
        ensure_corner();
        p_glBindBuffer(GL_ARRAY_BUFFER, corner_vbo);
        p_glEnableVertexAttribArray(0);
        p_glVertexAttribPointer(0, 1, GL_FLOAT, GL_FALSE, 0, 0);
        p_glBindBuffer(GL_ARRAY_BUFFER, bound_array);
        used_corner = 1;
    }
    return used_corner;
}

static void unprep_instanced_arrays(int used_corner)
{
    REAL(void, glEnableVertexAttribArray, (GLuint));
    REAL(void, glDisableVertexAttribArray, (GLuint));
    if (used_corner)
        p_glDisableVertexAttribArray(0);
    for (int i = 0; i < NX_MAX_ATTRS; i++)
        if (cur_vao->attr[i].enabled && cur_vao->attr[i].divisor > 0)
            ; /* left disabled: replay/bind or next pointer call restores */
}

GL_APICALL void GL_APIENTRY glDrawArraysInstanced(GLenum mode, GLint first,
                                                  GLsizei count, GLsizei prim)
{
    REAL(void, glDrawArrays, (GLenum, GLint, GLsizei));
    extern void nx_flush_ubos(void);
    nx_flush_ubos();
    nx_note_instanced(1, count, prim);
    nx_log_large_instanced_layout("arrays", mode, count, 0, (const void *)(uintptr_t)first, prim);
    nx_trace_draw("arrInst", prim);
    int c = prep_instanced_arrays();
    for (GLsizei i = 0; i < prim; i++) {
        set_instance_constants(i);
        p_glDrawArrays(mode, first, count);
    }
    unprep_instanced_arrays(c);
    nx_draw_count += prim;
    if (nx_deep_gl_diagnostics_enabled())
        nx_check_gl_error("DrawArraysInstanced");
}

GL_APICALL void GL_APIENTRY glDrawElementsInstanced(GLenum mode, GLsizei count,
                                                    GLenum type, const void *idx,
                                                    GLsizei prim)
{
    REAL(void, glDrawElements, (GLenum, GLsizei, GLenum, const void *));
    REAL(void, glBindBuffer, (GLenum, GLuint));
    extern void nx_flush_ubos(void);
    nx_flush_ubos();
    nx_note_instanced(0, count, prim);
    if (nx_try_batch_canvas_elements(mode, count, type, idx, prim)) {
        nx_trace_draw("elemBatch", prim);
        nx_draw_count++;
        if (nx_deep_gl_diagnostics_enabled())
            nx_check_gl_error("DrawElementsInstancedBatch");
        return;
    }
    const void *use_idx = idx;
    GLenum use_type = type;
    int rebound = 0;
    if (type == GL_UNSIGNED_INT) {
        GLuint sc = ushort_scratch_for(bound_element);
        if (sc) {
            use_type = GL_UNSIGNED_SHORT;
            use_idx = (const void *)((uintptr_t)idx / 2);
            rebound = 1;
        } else {
            /* Mali-450 has no OES_element_index_uint: leaving the type alone
             * makes the driver reject the draw and the frame comes out empty. */
            static int warned;
            if (warned < 8) {
                warned++;
                nx_log("no ushort scratch for element buffer %u: draw stays UNSIGNED_INT", bound_element);
            }
        }
    }
    nx_log_large_instanced_layout("elements", mode, count, type, idx, prim);
    nx_trace_draw("elemInst", prim);
    int deep_diagnostics = nx_deep_gl_diagnostics_enabled();
    if (deep_diagnostics)
        nx_check_gl_error("before DrawElementsInstanced");
    int c = prep_instanced_arrays();
    if (deep_diagnostics)
        nx_check_gl_error("prep_instanced_arrays");
    for (GLsizei i = 0; i < prim; i++) {
        set_instance_constants(i);
        if (deep_diagnostics)
            nx_check_gl_error("set_instance_constants");
        p_glDrawElements(mode, count, use_type, use_idx);
        if (deep_diagnostics)
            nx_check_gl_error("glDrawElements");
        nx_probe_after_draw("elemInst");
    }
    unprep_instanced_arrays(c);
    if (rebound)
        p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bound_element);
    nx_draw_count += prim;
    if (deep_diagnostics)
        nx_check_gl_error("DrawElementsInstanced");
}

/* ---- texture: sized internal formats -> ES2 ----------------------------- */
static void unsize_format(GLint *internal, GLenum *format, GLenum *type)
{
    switch (*internal) {
    case GL_RGBA8: case GL_SRGB8_ALPHA8: *internal = GL_RGBA; *format = GL_RGBA; *type = GL_UNSIGNED_BYTE; break;
    case GL_RGB8: case GL_SRGB8: *internal = GL_RGB; *format = GL_RGB; *type = GL_UNSIGNED_BYTE; break;
    case GL_RGB10_A2: *internal = GL_RGBA; *format = GL_RGBA; *type = GL_UNSIGNED_BYTE; break;
    case GL_RGB565: *internal = GL_RGB; break;
    case GL_RGBA4: *internal = GL_RGBA; break;
    case GL_RGB5_A1: *internal = GL_RGBA; break;
    case GL_R8: *internal = GL_LUMINANCE; *format = GL_LUMINANCE; *type = GL_UNSIGNED_BYTE; break;
    case GL_RG8: *internal = GL_LUMINANCE_ALPHA; *format = GL_LUMINANCE_ALPHA; *type = GL_UNSIGNED_BYTE; break;
    case GL_RGBA16F: case GL_RGBA32F:
        *internal = GL_RGBA; *format = GL_RGBA; *type = GL_UNSIGNED_BYTE;
        NX_STUB_LOG("float-texture->rgba8");
        break;
    case GL_DEPTH_COMPONENT24: case GL_DEPTH_COMPONENT32F:
        *internal = GL_DEPTH_COMPONENT; *format = GL_DEPTH_COMPONENT; *type = GL_UNSIGNED_INT; break;
    case GL_DEPTH24_STENCIL8: *internal = GL_DEPTH_STENCIL_OES; *format = GL_DEPTH_STENCIL_OES; *type = GL_UNSIGNED_INT_24_8_OES; break;
    default: break;
    }
}

GL_APICALL void GL_APIENTRY glTexImage2D(GLenum target, GLint level, GLint internal,
                                         GLsizei w, GLsizei h, GLint border,
                                         GLenum format, GLenum type, const void *pixels)
{
    REAL(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *));
    unsize_format(&internal, &format, &type);
    p_glTexImage2D(target, level, internal, w, h, border, format, type, pixels);
}

GL_APICALL void GL_APIENTRY glTexStorage2D(GLenum target, GLsizei levels,
                                           GLenum internal, GLsizei w, GLsizei h)
{
    GLint ifmt = internal;
    GLenum fmt = GL_RGBA, type = GL_UNSIGNED_BYTE;
    unsize_format(&ifmt, &fmt, &type);
    for (GLsizei l = 0; l < levels; l++) {
        glTexImage2D(target, l, ifmt, w > 1 ? w : 1, h > 1 ? h : 1, 0, fmt, type, NULL);
        w >>= 1; h >>= 1;
    }
}

GL_APICALL void GL_APIENTRY glRenderbufferStorage(GLenum target, GLenum internal,
                                                  GLsizei w, GLsizei h)
{
    REAL(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei));
    switch (internal) {
    case GL_RGBA8: internal = 0x8058; break;            /* OES_rgb8_rgba8 keeps RGBA8 */
    case GL_RGB8: internal = 0x8051; break;
    case GL_DEPTH_COMPONENT24: internal = 0x81A6; break; /* OES_depth24 */
    case GL_DEPTH_COMPONENT32F: internal = 0x81A6; break;
    case GL_DEPTH24_STENCIL8: internal = 0x88F0; break;  /* OES_packed_depth_stencil */
    default: break;
    }
    p_glRenderbufferStorage(target, internal, w, h);
}

/* ---- program/shader: capture + log Mali errors -------------------------- */
GL_APICALL void GL_APIENTRY glShaderSource(GLuint shader, GLsizei count,
                                           const GLchar *const *string,
                                           const GLint *length)
{
    REAL(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *));
    /* concat for logging/translation */
    size_t total = 0;
    for (GLsizei i = 0; i < count; i++)
        total += length && length[i] >= 0 ? (size_t)length[i] : strlen(string[i]);
    char *src = malloc(total + 1);
    size_t off = 0;
    for (GLsizei i = 0; i < count; i++) {
        size_t l = length && length[i] >= 0 ? (size_t)length[i] : strlen(string[i]);
        memcpy(src + off, string[i], l);
        off += l;
    }
    src[off] = 0;
    extern char *nx_translate_glsl(GLuint shader, const char *src);
    char *out = nx_translate_glsl(shader, src);
    const GLchar *one = out ? out : src;
    p_glShaderSource(shader, 1, &one, NULL);
    free(src);
    free(out);
}

GL_APICALL void GL_APIENTRY glCompileShader(GLuint shader)
{
    REAL(void, glCompileShader, (GLuint));
    REAL(void, glGetShaderiv, (GLuint, GLenum, GLint *));
    REAL(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *));
    REAL(void, glGetShaderSource, (GLuint, GLsizei, GLsizei *, GLchar *));
    p_glCompileShader(shader);
    GLint ok = 0;
    p_glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        GLsizei n = 0;
        p_glGetShaderInfoLog(shader, sizeof(log), &n, log);
        nx_log("COMPILE FAIL shader %u: %.*s", shader, n, log);
        if (getenv("NX_SHIM_DUMP")) {
            static unsigned fail_seq;
            GLint source_len = 0;
            p_glGetShaderiv(shader, GL_SHADER_SOURCE_LENGTH, &source_len);
            if (source_len > 1 && source_len < 1024 * 1024) {
                char *source = malloc((size_t)source_len);
                if (source) {
                    GLsizei written = 0;
                    p_glGetShaderSource(shader, source_len, &written, source);
                    char path[160];
                    snprintf(path, sizeof(path), "/tmp/nx_fail_%03u_sh%u.glsl",
                             ++fail_seq, shader);
                    FILE *dump = fopen(path, "w");
                    if (dump) {
                        fwrite(source, 1, (size_t)written, dump);
                        fclose(dump);
                    }
                    free(source);
                }
            }
        }
    }
}

GL_APICALL void GL_APIENTRY glLinkProgram(GLuint prog)
{
    REAL(void, glLinkProgram, (GLuint));
    REAL(void, glGetProgramiv, (GLuint, GLenum, GLint *));
    REAL(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *));
    extern void nx_prelink(GLuint prog);
    nx_prelink(prog);
    p_glLinkProgram(prog);
    GLint ok = 0;
    p_glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        GLsizei n = 0;
        p_glGetProgramInfoLog(prog, sizeof(log), &n, log);
        nx_log("LINK FAIL prog %u: %.*s", prog, n, log);
    }
}

GL_APICALL void GL_APIENTRY glUseProgram(GLuint prog)
{
    REAL(void, glUseProgram, (GLuint));
    extern void nx_on_use_program(GLuint prog);
    p_glUseProgram(prog);
    nx_on_use_program(prog);
}

unsigned long nx_draw_count;

unsigned long nx_get_and_reset_draws(void)
{
    unsigned long v = nx_draw_count;
    nx_draw_count = 0;
    return v;
}

void nx_get_and_reset_draw_stats(NxDrawStats *out)
{
    if (!out)
        return;
    nx_draw_stats.physical_draws = nx_draw_count;
    *out = nx_draw_stats;
    memset(&nx_draw_stats, 0, sizeof(nx_draw_stats));
    nx_draw_count = 0;
}

GL_APICALL void GL_APIENTRY glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    REAL(void, glDrawArrays, (GLenum, GLint, GLsizei));
    extern void nx_flush_ubos(void);
    nx_flush_ubos();
    nx_draw_stats.api_draws++;
    nx_draw_count++;
    nx_trace_draw("arr", count);
    p_glDrawArrays(mode, first, count);
}

GL_APICALL void GL_APIENTRY glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *idx)
{
    REAL(void, glDrawElements, (GLenum, GLsizei, GLenum, const void *));
    extern void nx_flush_ubos(void);
    nx_flush_ubos();
    nx_draw_stats.api_draws++;
    nx_draw_count++;
    p_glDrawElements(mode, count, type, idx);
}

/* ---- GLES3-only capabilities filtered before the GLES2 driver -------- */

static int nx_gles2_capability(GLenum cap)
{
    switch (cap) {
    case GL_BLEND:
    case GL_CULL_FACE:
    case GL_DEPTH_TEST:
    case GL_DITHER:
    case GL_POLYGON_OFFSET_FILL:
    case GL_SAMPLE_ALPHA_TO_COVERAGE:
    case GL_SAMPLE_COVERAGE:
    case GL_SCISSOR_TEST:
    case GL_STENCIL_TEST:
        return 1;
    default:
        return 0;
    }
}

static void nx_log_ignored_cap(GLenum cap)
{
    static GLenum seen[16];
    for (unsigned i = 0; i < sizeof(seen) / sizeof(seen[0]); i++) {
        if (seen[i] == cap)
            return;
        if (!seen[i]) {
            seen[i] = cap;
            nx_log("ignored GLES3 capability 0x%x on GLES2", cap);
            return;
        }
    }
}

GL_APICALL void GL_APIENTRY glEnable(GLenum cap)
{
    REAL(void, glEnable, (GLenum));
    if (nx_gles2_capability(cap))
        p_glEnable(cap);
    else
        nx_log_ignored_cap(cap);
}

GL_APICALL void GL_APIENTRY glDisable(GLenum cap)
{
    REAL(void, glDisable, (GLenum));
    if (nx_gles2_capability(cap))
        p_glDisable(cap);
    else
        nx_log_ignored_cap(cap);
}

/* ---- 2D-array textures aliased to plain 2D --------------------------- */
#define NX_T2DA 0x8C1A
static GLenum map_tex_target(GLenum t) { return t == NX_T2DA ? GL_TEXTURE_2D : t; }

static int nx_gles2_tex_parameter(GLenum pname)
{
    return pname == GL_TEXTURE_MAG_FILTER || pname == GL_TEXTURE_MIN_FILTER ||
           pname == GL_TEXTURE_WRAP_S || pname == GL_TEXTURE_WRAP_T;
}

static void nx_log_ignored_tex_parameter(GLenum pname)
{
    static GLenum seen[16];
    for (unsigned i = 0; i < sizeof(seen) / sizeof(seen[0]); i++) {
        if (seen[i] == pname)
            return;
        if (!seen[i]) {
            seen[i] = pname;
            nx_log("ignored GLES3 texture parameter 0x%x on GLES2", pname);
            return;
        }
    }
}

GL_APICALL void GL_APIENTRY glBindTexture(GLenum target, GLuint tex)
{
    REAL(void, glBindTexture, (GLenum, GLuint));
    p_glBindTexture(map_tex_target(target), tex);
}

GL_APICALL void GL_APIENTRY glTexParameteri(GLenum target, GLenum pname, GLint v)
{
    REAL(void, glTexParameteri, (GLenum, GLenum, GLint));
    if (nx_gles2_tex_parameter(pname))
        p_glTexParameteri(map_tex_target(target), pname, v);
    else
        nx_log_ignored_tex_parameter(pname);
}

GL_APICALL void GL_APIENTRY glTexParameterf(GLenum target, GLenum pname, GLfloat v)
{
    REAL(void, glTexParameterf, (GLenum, GLenum, GLfloat));
    if (nx_gles2_tex_parameter(pname))
        p_glTexParameterf(map_tex_target(target), pname, v);
    else
        nx_log_ignored_tex_parameter(pname);
}

GL_APICALL void GL_APIENTRY glGenerateMipmap(GLenum target)
{
    REAL(void, glGenerateMipmap, (GLenum));
    p_glGenerateMipmap(map_tex_target(target));
}

GL_APICALL void GL_APIENTRY glTexImage3D(GLenum target, GLint level, GLint internal,
                                         GLsizei w, GLsizei h, GLsizei depth, GLint border,
                                         GLenum format, GLenum type, const void *pixels)
{
    (void)depth;
    if (target == NX_T2DA)
        glTexImage2D(GL_TEXTURE_2D, level, internal, w, h, border, format, type, pixels);
    else
        NX_STUB_LOG("glTexImage3D-3D");
}

GL_APICALL void GL_APIENTRY glTexSubImage3D(GLenum target, GLint level, GLint x, GLint y,
                                            GLint z, GLsizei w, GLsizei h, GLsizei depth,
                                            GLenum format, GLenum type, const void *pixels)
{
    (void)depth;
    REAL(void, glTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *));
    if (target == NX_T2DA && z == 0)
        p_glTexSubImage2D(GL_TEXTURE_2D, level, x, y, w, h, format, type, pixels);
}

GL_APICALL void GL_APIENTRY glTexStorage3D(GLenum target, GLsizei levels, GLenum internal,
                                           GLsizei w, GLsizei h, GLsizei depth)
{
    (void)depth;
    if (target == NX_T2DA)
        glTexStorage2D(GL_TEXTURE_2D, levels, internal, w, h);
}

/* passthrough leftovers that live in overrides for future use */
GL_APICALL void GL_APIENTRY glGetShaderiv(GLuint s, GLenum p, GLint *v)
{
    REAL(void, glGetShaderiv, (GLuint, GLenum, GLint *));
    p_glGetShaderiv(s, p, v);
}
GL_APICALL void GL_APIENTRY glGetProgramiv(GLuint s, GLenum p, GLint *v)
{
    REAL(void, glGetProgramiv, (GLuint, GLenum, GLint *));
    p_glGetProgramiv(s, p, v);
}
GL_APICALL void GL_APIENTRY glGetShaderInfoLog(GLuint s, GLsizei sz, GLsizei *n, GLchar *l)
{
    REAL(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *));
    p_glGetShaderInfoLog(s, sz, n, l);
}
GL_APICALL void GL_APIENTRY glGetProgramInfoLog(GLuint s, GLsizei sz, GLsizei *n, GLchar *l)
{
    REAL(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *));
    p_glGetProgramInfoLog(s, sz, n, l);
}
GL_APICALL GLint GL_APIENTRY glGetUniformLocation(GLuint p, const GLchar *n)
{
    REAL(GLint, glGetUniformLocation, (GLuint, const GLchar *));
    return p_glGetUniformLocation(p, n);
}
GL_APICALL GLenum GL_APIENTRY glGetError(void)
{
    REAL(GLenum, glGetError, (void));
    return p_glGetError();
}
GL_APICALL void GL_APIENTRY glGetFramebufferAttachmentParameteriv(GLenum t, GLenum a, GLenum p, GLint *v)
{
    REAL(void, glGetFramebufferAttachmentParameteriv, (GLenum, GLenum, GLenum, GLint *));
    p_glGetFramebufferAttachmentParameteriv(t, a, p, v);
}
GL_APICALL void GL_APIENTRY glFramebufferTexture2D(GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l)
{
    REAL(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint));
    REAL(GLenum, glCheckFramebufferStatus, (GLenum));
    GLenum mapped_target = map_tex_target(tt);
    p_glFramebufferTexture2D(t, a, mapped_target, tex, l);
    {
        static int attach_logged;
        if (attach_logged < 24) {
            attach_logged++;
            nx_log("attach fbo=%u target=0x%x attachment=0x%x tex=%u", nx_bound_fbo, t, a, tex);
        }
    }
    GLenum st = p_glCheckFramebufferStatus(t);
    static int logged;
    if (st != GL_FRAMEBUFFER_COMPLETE && logged < 12) {
        logged++;
        nx_log("FBO incomplete 0x%x after attach tex=%u att=0x%x target=0x%x->0x%x level=%d",
               st, tex, a, tt, mapped_target, l);
    }
}
GL_APICALL void GL_APIENTRY glDeleteFramebuffers(GLsizei n, const GLuint *ids)
{
    REAL(void, glDeleteFramebuffers, (GLsizei, const GLuint *));
    for (GLsizei i = 0; i < n; i++) {
        if (nx_bound_fbo == ids[i]) nx_bound_fbo = 0;
    }
    p_glDeleteFramebuffers(n, ids);
}
GL_APICALL void GL_APIENTRY glBindFramebuffer(GLenum t, GLuint f)
{
    REAL(void, glBindFramebuffer, (GLenum, GLuint));
    /* GLES2 has a single framebuffer binding point; the read/draw split simply
     * does not exist, so only the real target updates the tracked state. */
    if (t == GL_FRAMEBUFFER) nx_bound_fbo = f;
    p_glBindFramebuffer(t, f);
}

/* shadow data for a UBO binding point (consumed by nx_glsl.c) */
const void *nx_ubo_data(GLuint binding, size_t *size)
{
    if (binding >= NX_MAX_UBO_BIND) return NULL;
    NxBuf *b = buf_get(ubo_binding[binding], 0);
    if (!b || !b->data) return NULL;
    *size = b->size - ubo_bind_off[binding];
    return (const char *)b->data + ubo_bind_off[binding];
}

GL_APICALL void GL_APIENTRY glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y,
                                            GLsizei w, GLsizei h, GLenum format, GLenum type,
                                            const void *pixels)
{
    REAL(void, glTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *));
    p_glTexSubImage2D(map_tex_target(target), level, x, y, w, h, format, type, pixels);
}

GL_APICALL void GL_APIENTRY glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
    REAL(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat));
    /* The game alternates its world and backbuffer clear colors every frame.
     * Logging every transition line-flushes to slow removable storage dozens
     * of times per second, so keep this diagnostic explicit and bounded. */
    static int budget = -1;
    static float lr = -1, lg = -1, lb = -1, la = -1;
    if (budget < 0) {
        const char *value = getenv("NX_CLEAR_LOG");
        budget = value && !strcmp(value, "1") ? 60 : 0;
    }
    if (budget > 0 && (r != lr || g != lg || b != lb || a != la)) {
        budget--;
        lr = r; lg = g; lb = b; la = a;
        nx_log("glClearColor(%.2f, %.2f, %.2f, %.2f)", r, g, b, a);
    }
    p_glClearColor(r, g, b, a);
}

GL_APICALL void GL_APIENTRY glClear(GLbitfield mask)
{
    REAL(void, glClear, (GLbitfield));
    REAL(void, glGetIntegerv, (GLenum, GLint *));
    REAL(void, glGetFloatv, (GLenum, GLfloat *));
    extern unsigned long nx_draw_count;
    static int budget = -1;
    if (budget < 0) {
        const char *value = getenv("NX_CLEAR_LOG");
        budget = value && !strcmp(value, "1") ? 60 : 0;
    }
    if (budget > 0) {
        budget--;
        GLint fbo = 0;
        GLfloat col[4] = {0, 0, 0, 0};
        p_glGetIntegerv(0x8CA6, &fbo);
        p_glGetFloatv(0x0C22 /*GL_COLOR_CLEAR_VALUE*/, col);
        nx_log("clear fbo=%d mask=0x%x color=%.2f,%.2f,%.2f,%.2f draw#%lu",
               fbo, mask, col[0], col[1], col[2], col[3], nx_draw_count);
    }
    p_glClear(mask);
}

GL_APICALL void GL_APIENTRY glViewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
    REAL(void, glViewport, (GLint, GLint, GLsizei, GLsizei));
    static int diagnostics = -1;
    static unsigned calls, zero_calls;
    if (diagnostics < 0) {
        const char *value = getenv("NX_VIEWPORT_LOG");
        diagnostics = value && !strcmp(value, "1");
    }
    calls++;
    int zero_size = width <= 0 || height <= 0;
    if ((diagnostics && (calls <= 80 || calls % 300 == 0)) ||
        (zero_size && ++zero_calls <= 20))
        nx_log("glViewport #%u %d,%d %dx%d", calls, x, y, width, height);
    p_glViewport(x, y, width, height);
}
