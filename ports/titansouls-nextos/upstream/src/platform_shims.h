#ifndef ASM2_PLATFORM_SHIMS_H
#define ASM2_PLATFORM_SHIMS_H

#include <GLES2/gl2.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "bionic_compat.h"

struct asm2_platform_stats {
  uint64_t guest_glfinish_calls;
  uint64_t finish_fence_calls;
  uint64_t fallback_glfinish_calls;
  uint64_t test_fence_calls;
  uint64_t fallback_test_true_calls;
  int native_finish_fence;
  int native_test_fence;
};

void asm2_platform_get_stats(struct asm2_platform_stats *stats);
void ASM2_GUEST_PCS asm2_glFinish(void);

void ASM2_GUEST_PCS asm2_glCompressedTexImage3DOES(
    GLenum target, GLint level, GLenum internal_format, GLsizei width,
    GLsizei height, GLsizei depth, GLint border, GLsizei image_size,
    const void *data);
void ASM2_GUEST_PCS asm2_glCompressedTexSubImage3DOES(
    GLenum target, GLint level, GLint x_offset, GLint y_offset, GLint z_offset,
    GLsizei width, GLsizei height, GLsizei depth, GLenum format,
    GLsizei image_size, const void *data);
void ASM2_GUEST_PCS asm2_glTexImage3DOES(
    GLenum target, GLint level, GLenum internal_format, GLsizei width,
    GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type,
    const void *pixels);
void ASM2_GUEST_PCS asm2_glTexSubImage3DOES(
    GLenum target, GLint level, GLint x_offset, GLint y_offset, GLint z_offset,
    GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type,
    const void *pixels);
void ASM2_GUEST_PCS asm2_glDeleteFencesNV(GLsizei count,
                                           const GLuint *fences);
void ASM2_GUEST_PCS asm2_glGenFencesNV(GLsizei count, GLuint *fences);
void ASM2_GUEST_PCS asm2_glSetFenceNV(GLuint fence, GLenum condition);
void ASM2_GUEST_PCS asm2_glFinishFenceNV(GLuint fence);
GLboolean ASM2_GUEST_PCS asm2_glTestFenceNV(GLuint fence);

void *ASM2_GUEST_PCS asm2_ALooper_forThread(void);
void *ASM2_GUEST_PCS asm2_ALooper_prepare(int options);
void *ASM2_GUEST_PCS asm2_ASensorManager_getInstance(void);
void *ASM2_GUEST_PCS asm2_ASensorManager_getDefaultSensor(void *manager,
                                                           int sensor_type);
void *ASM2_GUEST_PCS asm2_ASensorManager_createEventQueue(
    void *manager, void *looper, int identifier, void *callback, void *data);
int ASM2_GUEST_PCS asm2_ASensorEventQueue_disableSensor(void *queue,
                                                        void *sensor);
int ASM2_GUEST_PCS asm2_ASensorEventQueue_enableSensor(void *queue,
                                                       void *sensor);
ssize_t ASM2_GUEST_PCS asm2_ASensorEventQueue_getEvents(void *queue,
                                                         void *events,
                                                         size_t count);
int ASM2_GUEST_PCS asm2_ASensorEventQueue_setEventRate(void *queue,
                                                       void *sensor,
                                                       int32_t usec);

uintptr_t ASM2_GUEST_PCS asm2_unwind_find_exidx(uintptr_t pc, int *count);
void ts_exidx_register(uintptr_t lo, uintptr_t hi, uintptr_t table, int count);

#endif
