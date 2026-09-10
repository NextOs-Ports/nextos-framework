/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#include "crash_diag.h"

#include <signal.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

static volatile sig_atomic_t g_phase = CRASH_PHASE_BOOT;
static volatile sig_atomic_t g_frame;
static volatile sig_atomic_t g_handling;
static uintptr_t g_guest_base;
static uintptr_t g_guest_end;

static const char *phase_name(sig_atomic_t phase) {
  switch (phase) {
  case CRASH_PHASE_BOOT: return "boot";
  case CRASH_PHASE_RELOCATE: return "relocate";
  case CRASH_PHASE_RESOLVE: return "resolve";
  case CRASH_PHASE_FINALIZE: return "finalize";
  case CRASH_PHASE_CONSTRUCTORS: return "constructors";
  case CRASH_PHASE_GL: return "gl";
  case CRASH_PHASE_AUDIO: return "audio";
  case CRASH_PHASE_JNI: return "jni";
  case CRASH_PHASE_SET_DIRS: return "set-dirs";
  case CRASH_PHASE_APP_LAUNCH: return "app-launch";
  case CRASH_PHASE_MUSIC: return "music";
  case CRASH_PHASE_NATIVE_INTERFACE: return "native-interface";
  case CRASH_PHASE_APP_SETUP: return "app-setup";
  case CRASH_PHASE_VIEW_SIZE: return "view-size";
  case CRASH_PHASE_APP_ACTIVE: return "app-active";
  case CRASH_PHASE_EVENTS: return "events";
  case CRASH_PHASE_CONTROLS: return "controls";
  case CRASH_PHASE_JNI_UPDATE: return "jni-update";
  case CRASH_PHASE_UPDATE: return "update";
  case CRASH_PHASE_DRAW: return "draw";
  case CRASH_PHASE_CURSOR: return "cursor";
  case CRASH_PHASE_PRESENT_ALPHA: return "present-alpha";
  case CRASH_PHASE_FRAME_PROBE: return "frame-probe";
  case CRASH_PHASE_FINISH: return "finish";
  case CRASH_PHASE_SWAP: return "swap";
  case CRASH_PHASE_SHUTDOWN: return "shutdown";
  default: return "unknown";
  }
}

static void append_char(char *buffer, size_t capacity, size_t *length,
                        char value) {
  if (*length < capacity)
    buffer[(*length)++] = value;
}

static void append_text(char *buffer, size_t capacity, size_t *length,
                        const char *text) {
  while (text && *text)
    append_char(buffer, capacity, length, *text++);
}

static void append_decimal(char *buffer, size_t capacity, size_t *length,
                           unsigned long value) {
  char digits[3 * sizeof(value) + 1];
  size_t count = 0;
  do {
    digits[count++] = (char)('0' + value % 10UL);
    value /= 10UL;
  } while (value && count < sizeof(digits));
  while (count)
    append_char(buffer, capacity, length, digits[--count]);
}

static void append_hex(char *buffer, size_t capacity, size_t *length,
                       uintptr_t value) {
  static const char hex[] = "0123456789abcdef";
  char digits[2 * sizeof(value)];
  size_t count = 0;
  append_text(buffer, capacity, length, "0x");
  do {
    digits[count++] = hex[value & 0xfu];
    value >>= 4;
  } while (value && count < sizeof(digits));
  while (count)
    append_char(buffer, capacity, length, digits[--count]);
}

static int guest_offset(uintptr_t address, uintptr_t *offset) {
  if (!g_guest_base || address < g_guest_base || address >= g_guest_end)
    return 0;
  *offset = address - g_guest_base;
  return 1;
}

static void crash_handler(int signal_number, siginfo_t *info, void *context) {
  char line[512];
  size_t length = 0;
  uintptr_t pc = 0;
  uintptr_t lr = 0;
  uintptr_t sp = 0;
  uintptr_t offset = 0;

  if (g_handling)
    _exit(128 + signal_number);
  g_handling = 1;

#if defined(__aarch64__)
  if (context) {
    const ucontext_t *uc = (const ucontext_t *)context;
    pc = (uintptr_t)uc->uc_mcontext.pc;
    sp = (uintptr_t)uc->uc_mcontext.sp;
    lr = (uintptr_t)uc->uc_mcontext.regs[30];
  }
#else
  (void)context;
#endif

  append_text(line, sizeof(line), &length, "=== Swordigo crash sig=");
  append_decimal(line, sizeof(line), &length, (unsigned long)signal_number);
  append_text(line, sizeof(line), &length, " phase=");
  append_text(line, sizeof(line), &length, phase_name(g_phase));
  append_text(line, sizeof(line), &length, " frame=");
  append_decimal(line, sizeof(line), &length, (unsigned long)g_frame);
  append_text(line, sizeof(line), &length, " fault=");
  append_hex(line, sizeof(line), &length,
             info ? (uintptr_t)info->si_addr : (uintptr_t)0);
  append_text(line, sizeof(line), &length, " pc=");
  append_hex(line, sizeof(line), &length, pc);
  if (guest_offset(pc, &offset)) {
    append_text(line, sizeof(line), &length, " guest+");
    append_hex(line, sizeof(line), &length, offset);
  }
  append_text(line, sizeof(line), &length, " lr=");
  append_hex(line, sizeof(line), &length, lr);
  if (guest_offset(lr, &offset)) {
    append_text(line, sizeof(line), &length, " guest-lr+");
    append_hex(line, sizeof(line), &length, offset);
  }
  append_text(line, sizeof(line), &length, " sp=");
  append_hex(line, sizeof(line), &length, sp);
  append_text(line, sizeof(line), &length, " ===\n");
  (void)write(STDERR_FILENO, line, length);
  _exit(128 + signal_number);
}

void crash_diag_install(uintptr_t guest_base, size_t guest_size) {
  struct sigaction action;
  uintptr_t end = guest_base + guest_size;

  g_guest_base = guest_base;
  g_guest_end = end >= guest_base ? end : UINTPTR_MAX;
  g_handling = 0;
  memset(&action, 0, sizeof(action));
  sigemptyset(&action.sa_mask);
  action.sa_sigaction = crash_handler;
  action.sa_flags = SA_SIGINFO | SA_RESETHAND;
  (void)sigaction(SIGSEGV, &action, NULL);
  (void)sigaction(SIGBUS, &action, NULL);
  (void)sigaction(SIGILL, &action, NULL);
  (void)sigaction(SIGABRT, &action, NULL);
}

void crash_diag_set_phase(CrashPhase phase) {
  g_phase = (sig_atomic_t)phase;
}

void crash_diag_set_frame(unsigned frame) {
  g_frame = (sig_atomic_t)frame;
}
