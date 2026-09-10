/* glfix.h — GL provider repair by observed capability. */
#ifndef SWORDIGO_GLFIX_H
#define SWORDIGO_GLFIX_H

void glfix_set_argv(char **argv);
int glfix_renderer_is_broken(const char *renderer);
/* teardown runs just before the re-exec (destroy SDL window/context). */
void glfix_maybe_reexec(const char *renderer, const char *video_backend,
                        int window_opened, int context_current,
                        int drawable_positive, void (*teardown)(void));
/* Pre-context repair: the resolved EGL provider refused the kernel driver and
 * no window/context ever existed.  teardown runs before the probe (the display
 * and the kernel driver fd must be free).  Returns only when no repair
 * applies; the caller keeps its original fatal path. */
void glfix_maybe_reexec_noctx(const char *reason, const char *video_backend,
                              void (*teardown)(void));

#endif
