/*
 * Minimal Vulkan loader ABI used only to let a firmware-provided FFmpeg start
 * when its optional libplacebo dependency was installed with a broken
 * libvulkan.so.1 symlink. Huntdown decodes the authored H.264 movies entirely
 * in software; no Vulkan entry point is requested on that path.
 *
 * libplacebo has one direct Vulkan dependency: vkGetInstanceProcAddr. Returning
 * NULL is the Vulkan loader contract for an unavailable entry point and makes
 * the optional backend stay disabled. The library is built with -nostdlib and
 * therefore carries no glibc requirement.
 */

__attribute__((visibility("default")))
void *vkGetInstanceProcAddr(void *instance, const char *name) {
  (void)instance;
  (void)name;
  return (void *)0;
}
