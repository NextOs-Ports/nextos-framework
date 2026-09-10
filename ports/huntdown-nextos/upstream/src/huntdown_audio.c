#define _GNU_SOURCE
#include "huntdown_audio.h"
#include "huntdown_build.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <unistd.h>

#include "so_util.h"

/*
 * Huntdown 0.1.23 embeds Unity 2022.3.47f1. AudioClip::CreateFMODSound calls
 * this exact FMOD::System::createSound wrapper with
 * (system, data, mode, exinfo, sound**). The RVA was derived from Huntdown's
 * own libunity and the full entry signature prevents cross-version patching.
 */
#define HD_CREATE_SOUND_RVA 0xC2BCE0UL
#define HD_FMOD_CREATESTREAM 0x00000080U

typedef int (*HdCreateSound)(void *system, const void *data, uint32_t mode,
                             void *exinfo, void *output);

static const unsigned char g_create_sound_signature[32] = {
    0xff,0x03,0x01,0xd1, 0xf6,0x57,0x01,0xa9,
    0xf6,0x03,0x01,0xaa, 0xe1,0x23,0x00,0x91,
    0xf4,0x4f,0x02,0xa9, 0xfd,0x7b,0x03,0xa9,
    0xfd,0xc3,0x00,0x91, 0xf3,0x03,0x04,0xaa,
};

static HdCreateSound g_create_sound_original;
static int g_installed;
static int g_trace;
static int g_stream_fallback;

static int hd_env_enabled(const char *name) {
  const char *value = getenv(name);
  if (!value) return 0;
  return strcmp(value, "0") != 0 && strcasecmp(value, "false") != 0 &&
         strcasecmp(value, "no") != 0 && strcasecmp(value, "off") != 0;
}

static int hd_create_sound_hook(void *system, const void *data, uint32_t mode,
                                void *exinfo, void *output) {
  int result = g_create_sound_original(system, data, mode, exinfo, output);
  static unsigned observed;
  unsigned observation = observed++;
  if (g_trace && (observation < 48 || result != 0)) {
    fprintf(stderr,
            "[HD-AUDIO] createSound mode=0x%x stream=%u openmem=%u "
            "data=%p exinfo=%p -> %d\n",
            mode, !!(mode & HD_FMOD_CREATESTREAM),
            !!(mode & (0x800U | 0x10000000U)), data, exinfo, result);
  }

  if (result != 0 && (mode & HD_FMOD_CREATESTREAM) && g_stream_fallback) {
    uint32_t sample_mode = mode & ~HD_FMOD_CREATESTREAM;
    int retry = g_create_sound_original(system, data, sample_mode, exinfo,
                                        output);
    static unsigned retries;
    if (retries++ < 16) {
      fprintf(stderr,
              "[HD-AUDIO] stream falhou (%d); retry sample mode=0x%x -> %d\n",
              result, sample_mode, retry);
    }
    if (retry == 0) return 0;
    result = retry;
  }
  return result;
}

static void *hd_make_create_sound_trampoline(uintptr_t target,
                                              size_t page_size) {
  unsigned char *page = mmap(NULL, page_size,
                             PROT_READ | PROT_WRITE | PROT_EXEC,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (page == MAP_FAILED) return NULL;

  /* All four overwritten instructions are position-independent and covered
   * by the exact signature above: sub/stp/mov/add. */
  memcpy(page, (const void *)target, 16);
  uint32_t *tail = (uint32_t *)(page + 16);
  tail[0] = 0x58000051U; /* ldr x17, [pc, #8] */
  tail[1] = 0xD61F0220U; /* br x17 */
  *(uint64_t *)&tail[2] = (uint64_t)(target + 16);
  __builtin___clear_cache((char *)page, (char *)page + 32);
  if (mprotect(page, page_size, PROT_READ | PROT_EXEC) != 0) {
    munmap(page, page_size);
    return NULL;
  }
  return page;
}

int hd_audio_stream_install(uintptr_t unity_base) {
  if (g_installed) return 1;
  /* Unity 6 uses the AAudio bridge and does not contain the 2022.3 FMOD
   * createSound entry below. */
  if (hd_build_is_unity6()) return 1;
  g_trace = hd_env_enabled("HD_SOUND_TRACE") ||
            hd_env_enabled("HD_AUDIOSPY");
  /* FMOD on this target rejects Unity's CREATESTREAM path with error 33.
   * Huntdown's resident-sample path is proven working, so keep the scoped
   * retry enabled unless a developer explicitly disables it. */
  g_stream_fallback = !hd_env_enabled("HD_DISABLE_STREAM_FALLBACK");
  if (!unity_base) return 0;

  uintptr_t target = unity_base + HD_CREATE_SOUND_RVA;
  if (memcmp((const void *)target, g_create_sound_signature,
             sizeof g_create_sound_signature) != 0) {
    fprintf(stderr,
            "[HD-AUDIO] assinatura createSound divergente em 0x%lx; "
            "hook recusado\n",
            (unsigned long)HD_CREATE_SOUND_RVA);
    return 0;
  }

  long raw_page_size = sysconf(_SC_PAGESIZE);
  size_t page_size = raw_page_size > 0 ? (size_t)raw_page_size : 4096U;
  void *trampoline = hd_make_create_sound_trampoline(target, page_size);
  if (!trampoline) {
    fprintf(stderr, "[HD-AUDIO] nao criou trampolim createSound\n");
    return 0;
  }

  uintptr_t target_page = target & ~((uintptr_t)page_size - 1U);
  if (mprotect((void *)target_page, page_size,
               PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
    munmap(trampoline, page_size);
    fprintf(stderr, "[HD-AUDIO] nao liberou pagina createSound\n");
    return 0;
  }
  g_create_sound_original = (HdCreateSound)trampoline;
  hook_arm64(target, (uintptr_t)hd_create_sound_hook);
  __builtin___clear_cache((char *)target_page,
                          (char *)target_page + page_size);
  mprotect((void *)target_page, page_size, PROT_READ | PROT_EXEC);

  g_installed = 1;
  fprintf(stderr,
          "[HD-AUDIO] createSound 0x%lx instalado (trace=%d fallback=%d)\n",
          (unsigned long)HD_CREATE_SOUND_RVA, g_trace, g_stream_fallback);
  return 1;
}
