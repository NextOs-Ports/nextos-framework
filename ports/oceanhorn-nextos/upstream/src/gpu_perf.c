/* gpu_perf.c — pin de desempenho da GPU Mali-450 (Amlogic /sys/class/mpgpu).
 *
 * Herdado do launcher antigo (run.sh), agora no binário para a restauração
 * ser garantida pelo mesmo processo que joga: o scaler do Mali-450 desligava
 * pixel processors e segurava 400 MHz entre jobs do MESMO quadro — a mesma
 * cena caía de 24–25 para 14–16 fps no Amlogic-old.  Mantém todos os PPs e o
 * maior nível oficial do driver enquanto o jogo roda; uma guarda térmica
 * recua um nível antes do trip "hot" do kernel e volta quando esfria.  Nada
 * de resolução/shader/efeito é alterado.  Tudo é restaurado em ocean_gpu_
 * perf_shutdown(), chamado no caminho único de saída (antes do _exit).
 *
 * Em aparelho sem /sys/class/mpgpu (RK3326/KMSDRM etc.) vira no-op.
 * OCEAN_GPU_PERFORMANCE=0 desliga; OCEAN_GPU_THERMAL_HIGH/LOW ajustam a
 * guarda (padrão 82000/76000 mC).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#define GPU_DIR "/sys/class/mpgpu"
#define THERM_PATH "/sys/class/thermal/thermal_zone0/temp"

static char g_old_min_freq[32], g_old_min_pp[32];
static char g_target_freq[32], g_cool_freq[32], g_max_pp[32];
static int g_pinned = 0;
static volatile int g_therm_run = 0;
static pthread_t g_therm_thread;

static int rd(const char *path, char *out, size_t cap) {
  FILE *f = fopen(path, "r");
  if (!f) return -1;
  if (!fgets(out, (int)cap, f)) { fclose(f); return -1; }
  fclose(f);
  out[strcspn(out, "\n")] = 0;
  return out[0] ? 0 : -1;
}

static int wr(const char *path, const char *val) {
  FILE *f = fopen(path, "w");
  if (!f) return -1;
  int ok = fprintf(f, "%s\n", val) > 0;
  fclose(f);
  return ok ? 0 : -1;
}

static long num(const char *s) {
  char *end = NULL;
  long v = strtol(s, &end, 10);
  return (end && end != s) ? v : -1;
}

static void *therm_loop(void *arg) {
  (void)arg;
  long hi = 82000, lo = 76000;
  const char *e;
  if ((e = getenv("OCEAN_GPU_THERMAL_HIGH")) && num(e) > 0) hi = num(e);
  if ((e = getenv("OCEAN_GPU_THERMAL_LOW")) && num(e) > 0) lo = num(e);
  if (hi <= lo) { hi = 82000; lo = 76000; }
  int cooled = 0;
  char buf[32];
  while (g_therm_run) {
    long t = rd(THERM_PATH, buf, sizeof buf) == 0 ? num(buf) : 0;
    if (t < 0) t = 0;
    if (!cooled && t >= hi) {
      wr(GPU_DIR "/min_freq", g_cool_freq);
      cooled = 1;
      fprintf(stderr, "[gpu] proteção térmica: %ldmC, GPU nível %s\n",
              t, g_cool_freq);
    } else if (cooled && t <= lo) {
      wr(GPU_DIR "/min_freq", g_target_freq);
      cooled = 0;
      fprintf(stderr, "[gpu] GPU restaurada ao nível %s (%ldmC)\n",
              g_target_freq, t);
    }
    for (int i = 0; i < 10 && g_therm_run; i++) usleep(500 * 1000);
  }
  return NULL;
}

void ocean_gpu_perf_init(void) {
  const char *e = getenv("OCEAN_GPU_PERFORMANCE");
  if (e && strcmp(e, "0") == 0) return;
  if (access(GPU_DIR "/min_freq", W_OK) != 0 ||
      access(GPU_DIR "/min_pp", W_OK) != 0)
    return;                       /* sem mpgpu gravável = no-op */
  if (rd(GPU_DIR "/min_freq", g_old_min_freq, sizeof g_old_min_freq) != 0 ||
      rd(GPU_DIR "/min_pp",  g_old_min_pp,  sizeof g_old_min_pp)  != 0 ||
      rd(GPU_DIR "/max_freq", g_target_freq, sizeof g_target_freq) != 0 ||
      rd(GPU_DIR "/max_pp",  g_max_pp,      sizeof g_max_pp)      != 0)
    return;
  long tf = num(g_target_freq);
  if (tf <= 0) return;
  snprintf(g_cool_freq, sizeof g_cool_freq, "%ld", tf - 1);
  if (wr(GPU_DIR "/min_freq", g_target_freq) != 0 ||
      wr(GPU_DIR "/min_pp", g_max_pp) != 0) {
    /* aplicação parcial: volta tudo já */
    wr(GPU_DIR "/min_freq", g_old_min_freq);
    wr(GPU_DIR "/min_pp", g_old_min_pp);
    return;
  }
  g_pinned = 1;
  fprintf(stderr, "[gpu] desempenho: frequência nível %s, %s PPs\n",
          g_target_freq, g_max_pp);
  if (access(THERM_PATH, R_OK) == 0) {
    g_therm_run = 1;
    if (pthread_create(&g_therm_thread, NULL, therm_loop, NULL) != 0)
      g_therm_run = 0;
  }
}

void ocean_gpu_perf_shutdown(void) {
  if (g_therm_run) {
    g_therm_run = 0;
    pthread_join(g_therm_thread, NULL);
  }
  if (g_pinned) {
    wr(GPU_DIR "/min_freq", g_old_min_freq);
    wr(GPU_DIR "/min_pp", g_old_min_pp);
    g_pinned = 0;
    fprintf(stderr, "[gpu] valores originais restaurados\n");
  }
}
