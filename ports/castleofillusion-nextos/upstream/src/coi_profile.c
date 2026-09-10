/*
 * coi_profile.c -- profiler de amostragem da THREAD DE RENDER.
 *
 * Por que existe: a medição de 06/08 no R36T/ArkOS (Mali-G31) mostrou
 * `busy=30ms` de CPU REAL da thread num frame de 35ms, com 21 draw calls e
 * `swapwait=5ms`. Ou seja: o port não é limitado por fill-rate, por vsync nem
 * por banda de textura — é limitado por CPU. Baixar resolução ou textura não
 * daria fps nenhum. Para ganhar fps é preciso saber ONDE a CPU queima, e o
 * `perf` não existe no firmware.
 *
 * Como funciona: um POSIX timer preso à thread de render (SIGEV_THREAD_ID) e
 * ancorado no relógio de CPU DELA (CLOCK_THREAD_CPUTIME_ID) dispara SIGPROF a
 * cada COI_PROFILE_US microssegundos de CPU consumida. O handler lê o PC do
 * ucontext, acha em qual mapeamento ele cai (tabela lida UMA vez no start) e
 * incrementa dois contadores: o da biblioteca e o do balde de 256 B dentro
 * dela. Nada de alocar, travar ou imprimir dentro do handler.
 *
 * A atribuição é feita NO HANDLER de propósito: fazê-la depois, por histograma
 * de páginas, perdia metade das amostras por saturação de tabela. E os
 * mapeamentos ANÔNIMOS entram na tabela — os módulos do so-loader (libfmodex.so
 * e a própria engine) vivem em heap mmap, sem caminho no /proc/self/maps.
 *
 * Saída: `[PROF]` com o custo por biblioteca e os offsets mais quentes, que
 * viram nome de função no PC com
 *   aarch64-linux-gnu-nm -C --defined-only lib/libViewer_GP.so | sort
 *
 * Gate: COI_PROFILE=1 (default OFF — no release não existe timer nem handler).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

extern volatile uintptr_t g_load_base;

#define PROF_MAPS   96
#define PROF_SHIFT  8                      /* baldes de 256 B */
#define PROF_BUCK   65536                  /* teto: 16 MB por biblioteca */

/* baldes alocados por mapeamento, do TAMANHO dele: com um array fixo de 1 MB o
 * histograma só enxergava o começo do .text e a engine (8 MB) ficava cega
 * justamente na parte que interessa. */
static struct {
  uintptr_t lo, hi;
  char nm[48];
  unsigned long n;
  unsigned *buck;
  unsigned *wbuck;                         /* janela: zerada a cada swap */
  unsigned long wn;
  unsigned nbuck;
} g_map[PROF_MAPS];
static int g_map_n;
static volatile unsigned long g_n_tot, g_n_lost;
/* CHAMADOR: quando a amostra cai na libc, o LR (x30) aponta para quem chamou —
 * strcmp/strchr/memcpy são FOLHA, não mexem no LR. Sem isso o perfil diz "a
 * libc custa 47%" e não diz de quem é a culpa. */
static int g_libc_map = -1;
static struct { uintptr_t lo, hi; const char *nm; unsigned *buck; unsigned nbuck; unsigned long n; } g_caller[PROF_MAPS];
static int g_caller_n;

static void prof_handler(int sig, siginfo_t *si, void *uc) {
  (void)sig; (void)si;
  uintptr_t pc = (uintptr_t)((ucontext_t *)uc)->uc_mcontext.pc;
  g_n_tot++;
  for (int i = 0; i < g_map_n; i++) {
    if (pc >= g_map[i].lo && pc < g_map[i].hi) {
      g_map[i].n++;
      unsigned b = (unsigned)((pc - g_map[i].lo) >> PROF_SHIFT);
      if (g_map[i].buck && b < g_map[i].nbuck) g_map[i].buck[b]++;
      g_map[i].wn++;
      if (g_map[i].wbuck && b < g_map[i].nbuck) g_map[i].wbuck[b]++;
      if (i == g_libc_map) {
        uintptr_t lr = (uintptr_t)((ucontext_t *)uc)->uc_mcontext.regs[30];
        for (int j = 0; j < g_caller_n; j++)
          if (lr >= g_caller[j].lo && lr < g_caller[j].hi) {
            g_caller[j].n++;
            unsigned cb = (unsigned)((lr - g_caller[j].lo) >> PROF_SHIFT);
            if (g_caller[j].buck && cb < g_caller[j].nbuck) g_caller[j].buck[cb]++;
            break;
          }
      }
      return;
    }
  }
  g_n_lost++;                                /* mapeado depois do start */
}

static void prof_load_maps(void) {
  FILE *f = fopen("/proc/self/maps", "r");
  if (!f) return;
  char ln[400];
  while (fgets(ln, sizeof ln, f) && g_map_n < PROF_MAPS) {
    unsigned long lo, hi; char perm[8], path[256]; path[0] = 0;
    int got = sscanf(ln, "%lx-%lx %7s %*x %*s %*d %255s", &lo, &hi, perm, path);
    if (got < 3 || perm[2] != 'x') continue;   /* só executável */
    char anon[48]; const char *b;
    if (got < 4 || !path[0]) {
      snprintf(anon, sizeof anon, "anon@%lx:%luMB", lo, (hi - lo) >> 20);
      b = anon;
    } else {
      b = strrchr(path, '/');
      b = b ? b + 1 : path;
    }
    int k = -1;
    for (int i = 0; i < g_map_n; i++)
      if (strcmp(g_map[i].nm, b) == 0) { k = i; break; }
    if (k < 0) {
      k = g_map_n++;
      snprintf(g_map[k].nm, sizeof g_map[k].nm, "%s", b);
      g_map[k].lo = lo; g_map[k].hi = hi;
    }
    if (lo < g_map[k].lo) g_map[k].lo = lo;
    if (hi > g_map[k].hi) g_map[k].hi = hi;
  }
  fclose(f);
  for (int i = 0; i < g_map_n; i++) {
    unsigned nb = (unsigned)((g_map[i].hi - g_map[i].lo) >> PROF_SHIFT) + 1;
    if (nb > PROF_BUCK) nb = PROF_BUCK;
    g_map[i].buck = (unsigned *)calloc(nb, sizeof(unsigned));
    g_map[i].wbuck = (unsigned *)calloc(nb, sizeof(unsigned));
    g_map[i].nbuck = g_map[i].buck ? nb : 0;
    if (strncmp(g_map[i].nm, "libc", 4) == 0) g_libc_map = i;
    if (g_caller_n < PROF_MAPS) {
      g_caller[g_caller_n].lo = g_map[i].lo;
      g_caller[g_caller_n].hi = g_map[i].hi;
      g_caller[g_caller_n].nm = g_map[i].nm;
      g_caller[g_caller_n].buck = (unsigned *)calloc(nb, sizeof(unsigned));
      g_caller[g_caller_n].nbuck = g_caller[g_caller_n].buck ? nb : 0;
      g_caller_n++;
    }
  }
}

/* chamado da thread de render (1ª passada pelo SwapBuffers) */
void coi_profile_start(void) {
  static int done = 0;
  if (done) return;
  done = 1;
  if (!getenv("COI_PROFILE")) return;
  prof_load_maps();
  if (!g_map_n) return;

  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = prof_handler;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigaction(SIGPROF, &sa, NULL);

  const char *us = getenv("COI_PROFILE_US");
  long ival = us ? atol(us) : 1000;            /* 1 ms de CPU por amostra */
  if (ival < 200) ival = 200;

  struct sigevent sev;
  memset(&sev, 0, sizeof sev);
  sev.sigev_notify = SIGEV_THREAD_ID;
  sev.sigev_signo = SIGPROF;
  sev._sigev_un._tid = (int)syscall(SYS_gettid);
  /* timer_create vive na librt em glibc antiga. Resolvemos em runtime para o
   * binário de release NÃO ganhar um NEEDED por causa de uma ferramenta de
   * bancada que fica desligada. */
  int (*p_create)(clockid_t, struct sigevent *, timer_t *) =
      dlsym(RTLD_DEFAULT, "timer_create");
  int (*p_settime)(timer_t, int, const struct itimerspec *, struct itimerspec *) =
      dlsym(RTLD_DEFAULT, "timer_settime");
  if (!p_create || !p_settime) {
    void *rt = dlopen("librt.so.1", RTLD_NOW);
    if (rt) {
      if (!p_create) p_create = dlsym(rt, "timer_create");
      if (!p_settime) p_settime = dlsym(rt, "timer_settime");
    }
  }
  if (!p_create || !p_settime) {
    fprintf(stderr, "[PROF] timer_create indisponivel (librt)\n");
    return;
  }
  timer_t t;
  if (p_create(CLOCK_THREAD_CPUTIME_ID, &sev, &t) != 0) {
    fprintf(stderr, "[PROF] timer_create falhou\n");
    return;
  }
  struct itimerspec its;
  its.it_interval.tv_sec = 0;
  its.it_interval.tv_nsec = ival * 1000;
  its.it_value = its.it_interval;
  p_settime(t, 0, &its, NULL);
  fprintf(stderr, "[PROF] amostrando a thread de render a cada %ld us de CPU "
                  "(%d mapeamentos, base do jogo=0x%lx)\n",
          ival, g_map_n, (unsigned long)g_load_base);
}

/* chamado do relatório periódico do [PERF]; perfil acumulado, não zera */
void coi_profile_report(void) {
  unsigned long tot = g_n_tot;
  if (!g_map_n || tot < 50) return;
  fprintf(stderr, "[PROF] amostras=%lu (fora dos mapeamentos conhecidos=%lu)\n",
          tot, g_n_lost);
  int order[PROF_MAPS]; int on = 0;
  for (int i = 0; i < g_map_n; i++) if (g_map[i].n) order[on++] = i;
  for (int a = 0; a < on; a++)
    for (int b = a + 1; b < on; b++)
      if (g_map[order[b]].n > g_map[order[a]].n) {
        int t = order[a]; order[a] = order[b]; order[b] = t;
      }
  for (int k = 0; k < on && k < 8; k++) {
    int i = order[k];
    fprintf(stderr, "[PROF]   %-34s %6lu (%.1f%%)\n", g_map[i].nm, g_map[i].n,
            100.0 * g_map[i].n / tot);
  }
  /* dentro das 3 maiores, os offsets mais quentes */
  for (int k = 0; k < on && k < 3; k++) {
    int i = order[k];
    for (int q = 0; q < 6; q++) {
      unsigned best = 0; int bb = -1;
      for (unsigned b = 0; b < g_map[i].nbuck; b++)
        if (g_map[i].buck[b] > best) { best = g_map[i].buck[b]; bb = (int)b; }
      if (bb < 0 || !best) break;
      fprintf(stderr, "[PROF]     %s+0x%06lx  %u (%.1f%%)\n", g_map[i].nm,
              (unsigned long)bb << PROF_SHIFT, best, 100.0 * best / tot);
      g_map[i].buck[bb] = 0;                 /* consome p/ achar o próximo */
    }
  }
  /* quem CHAMA a libc */
  if (g_libc_map >= 0) {
    fprintf(stderr, "[PROF] chamadores da libc:\n");
    for (int q = 0; q < 6; q++) {
      unsigned best = 0; int bi = -1, bb = -1;
      for (int j = 0; j < g_caller_n; j++)
        for (unsigned b = 0; b < g_caller[j].nbuck; b++)
          if (g_caller[j].buck[b] > best) { best = g_caller[j].buck[b]; bi = j; bb = (int)b; }
      if (bi < 0 || !best) break;
      fprintf(stderr, "[PROF]     <- %s+0x%06lx  %u (%.1f%%)\n", g_caller[bi].nm,
              (unsigned long)bb << PROF_SHIFT, best, 100.0 * best / tot);
      g_caller[bi].buck[bb] = 0;
    }
  }
}

/* ===== PERFIL DO CARREGAMENTO =====
 * Um loading é um "frame" que dura segundos. O perfil acumulado não serve para
 * ele: a tela de título, que roda por minutos, afoga a amostra do load. Aqui a
 * janela é zerada a cada swap, então quando um frame passa do limiar o que
 * sobrou na janela é EXATAMENTE o que aquele carregamento queimou.
 * Chamado do SwapBuffers com a duração do frame que acabou. */
void coi_profile_frame(double frame_ms) {
  if (!g_map_n) return;
  if (frame_ms >= 1000.0) {
    unsigned long tot = 0;
    for (int i = 0; i < g_map_n; i++) tot += g_map[i].wn;
    if (tot >= 20) {
      fprintf(stderr, "[PROFLOAD] frame de %.1fs — %lu amostras de CPU:\n",
              frame_ms / 1000.0, tot);
      for (int q = 0; q < 4; q++) {          /* bibliotecas */
        unsigned long best = 0; int bi = -1;
        for (int i = 0; i < g_map_n; i++) if (g_map[i].wn > best) { best = g_map[i].wn; bi = i; }
        if (bi < 0 || !best) break;
        fprintf(stderr, "[PROFLOAD]   %-30s %lu (%.1f%%)\n", g_map[bi].nm, best,
                100.0 * best / tot);
        /* offsets quentes dentro dela */
        for (int k = 0; k < 5; k++) {
          unsigned b2 = 0; int bb = -1;
          for (unsigned b = 0; b < g_map[bi].nbuck; b++)
            if (g_map[bi].wbuck[b] > b2) { b2 = g_map[bi].wbuck[b]; bb = (int)b; }
          if (bb < 0 || !b2) break;
          fprintf(stderr, "[PROFLOAD]       +0x%06lx  %u (%.1f%%)\n",
                  (unsigned long)bb << PROF_SHIFT, b2, 100.0 * b2 / tot);
          g_map[bi].wbuck[bb] = 0;
        }
        g_map[bi].wn = 0;
      }
    }
  }
  for (int i = 0; i < g_map_n; i++) {
    g_map[i].wn = 0;
    if (g_map[i].wbuck) memset(g_map[i].wbuck, 0, (size_t)g_map[i].nbuck * sizeof(unsigned));
  }
}
