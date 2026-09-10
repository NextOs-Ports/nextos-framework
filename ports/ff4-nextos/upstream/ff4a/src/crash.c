/*
 * crash.c -- handler de sinais. Unwind MANUAL pela cadeia de frame-pointer
 * (aarch64: x29=fp -> [fp]=fp anterior, [fp+8]=lr), mapeando cada endereço p/
 * libff4a+offset. Não usa backtrace() do glibc (falha em código mapeado à mão).
 */
#define _GNU_SOURCE
#include "crash.h"
#include "so_util.h"
#include "util.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>

static volatile int g_in_handler = 0;

static void mapaddr(uintptr_t a, char *out, size_t n) {
  uintptr_t base = so_text_base;
  if (base && a >= base && a < base + so_load_size)
    snprintf(out, n, "libff4a+0x%-8lx", (unsigned long)(a - base));
  else
    snprintf(out, n, "0x%-12lx", (unsigned long)a);
}

static int readable(uintptr_t p) {
  /* heurística: ponteiro plausível (userspace, alinhado). */
  return p > 0x10000 && (p & 7) == 0 && p < 0x0000800000000000ull;
}

static void handler(int sig, siginfo_t *si, void *uc_) {
  if (g_in_handler) _exit(139);
  g_in_handler = 1;
  ucontext_t *uc = (ucontext_t *)uc_;
  char b[64];

  uintptr_t pc = 0, lr = 0, fp = 0, sp = 0;
#if defined(__aarch64__)
  if (uc) {
    pc = uc->uc_mcontext.pc;
    sp = uc->uc_mcontext.sp;
    fp = uc->uc_mcontext.regs[29];
    lr = uc->uc_mcontext.regs[30];
  }
#endif
  mapaddr(pc, b, sizeof(b));
  debugPrintf("\n*** CRASH sig=%d (%s) fault=%p ***\n", sig, strsignal(sig),
              si ? si->si_addr : NULL);
  debugPrintf("  PC = %s\n", b);
  /* qual lib contém o PC? (varre /proc/self/maps) */
  {
    FILE *m = fopen("/proc/self/maps", "r");
    if (m) {
      char line[512];
      while (fgets(line, sizeof(line), m)) {
        unsigned long lo = 0, hi = 0;
        if (sscanf(line, "%lx-%lx", &lo, &hi) == 2 && pc >= lo && pc < hi) {
          size_t L = strlen(line);
          if (L && line[L - 1] == '\n') line[L - 1] = 0;
          debugPrintf("  PC lib: %s (PC-lo=0x%lx)\n", line,
                      (unsigned long)(pc - lo));
          break;
        }
      }
      fclose(m);
    }
  }
  mapaddr(lr, b, sizeof(b)); debugPrintf("  LR = %s\n", b);
  mapaddr(sp, b, sizeof(b)); debugPrintf("  SP = %s\n", b);
  mapaddr(fp, b, sizeof(b)); debugPrintf("  FP = %s\n", b);
#if defined(__aarch64__)
  if (uc)
    for (int i = 0; i <= 8; i++) {
      mapaddr(uc->uc_mcontext.regs[i], b, sizeof(b));
      debugPrintf("  x%-2d= %s\n", i, b);
    }
#endif

  /* unwind manual pela cadeia de fp */
  debugPrintf("backtrace (fp-chain):\n");
  mapaddr(pc, b, sizeof(b)); debugPrintf("  #00 %s\n", b);
  if (lr) { mapaddr(lr, b, sizeof(b)); debugPrintf("  #01 %s\n", b); }
  int depth = 2;
  uintptr_t cur = fp;
  while (cur && readable(cur) && depth < 40) {
    uintptr_t next = ((uintptr_t *)cur)[0];
    uintptr_t ret = ((uintptr_t *)cur)[1];
    if (!ret) break;
    mapaddr(ret, b, sizeof(b));
    debugPrintf("  #%02d %s\n", depth, b);
    if (next <= cur) break; /* fp deve subir */
    cur = next;
    depth++;
  }
  log_close();
  _exit(139);
}

void crash_init(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = handler;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGILL, &sa, NULL);
  sigaction(SIGABRT, &sa, NULL);
  sigaction(SIGFPE, &sa, NULL);
}
