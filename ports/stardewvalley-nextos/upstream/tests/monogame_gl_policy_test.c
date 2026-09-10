#include <assert.h>
#include <stdint.h>

#include "sdv_monogame_gl_policy.h"

int main(void)
{
    void *real_handle = (void *)(uintptr_t)1;

    assert(sdv_monogame_hide_global_egl_bind_api(
        NULL, "eglBindAPI", 1));
    assert(!sdv_monogame_hide_global_egl_bind_api(
        real_handle, "eglBindAPI", 1));
    assert(!sdv_monogame_hide_global_egl_bind_api(
        NULL, "eglBindAPI", 0));
    assert(!sdv_monogame_hide_global_egl_bind_api(
        NULL, "glBindFramebuffer", 1));
    assert(!sdv_monogame_hide_global_egl_bind_api(NULL, NULL, 1));
    return 0;
}
