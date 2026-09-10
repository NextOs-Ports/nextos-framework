#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "audio_backend_policy.h"
#include "jni_shim.h"
#include "util.h"

/*
 * The embedded SDL keeps its Android audio driver and writes PCM through the
 * original JNI audioOpen/audioWrite callbacks. The host side feeds either
 * pacat or aplay; selection happens before either server backend is entered.
 */
static FILE *g_audio_sink;
static enum ss_audio_backend g_primary_backend = SS_AUDIO_BACKEND_NONE;
static enum ss_audio_backend g_active_backend = SS_AUDIO_BACKEND_NONE;
static int g_alsa_available;
static int g_allow_alsa_recovery;
static int g_recovery_attempted;
static int g_last_sample_rate;
static int g_last_audio_format;
static int g_last_channels;
static int g_last_frames;

static int command_available(const char *name) {
  const char *path = getenv("PATH");
  if (!path || !*path)
    path = "/usr/local/bin:/usr/bin:/bin";

  size_t name_length = strlen(name);
  while (*path) {
    const char *end = strchr(path, ':');
    size_t directory_length = end ? (size_t)(end - path) : strlen(path);
    if (directory_length) {
      char candidate[512];
      if (directory_length + 1 + name_length + 1 <= sizeof(candidate)) {
        memcpy(candidate, path, directory_length);
        candidate[directory_length] = '/';
        memcpy(candidate + directory_length + 1, name, name_length + 1);
        if (access(candidate, X_OK) == 0)
          return 1;
      }
    }
    if (!end)
      break;
    path = end + 1;
  }
  return 0;
}

static int ensure_pulse_server(void) {
  const char *configured = getenv("PULSE_SERVER");
  if (configured && *configured)
    return 1;

  const char *cands[3];
  int n = 0;
  static char rt[256];
  const char *xrd = getenv("XDG_RUNTIME_DIR");
  if (xrd && *xrd) {
    snprintf(rt, sizeof(rt), "%s/pulse/native", xrd);
    cands[n++] = rt;
  }
  cands[n++] = "/run/pulse/native";
  cands[n++] = "/var/run/pulse/native";
  for (int i = 0; i < n; i++) {
    struct stat st;
    if (stat(cands[i], &st) == 0 && S_ISSOCK(st.st_mode)) {
      char v[300];
      snprintf(v, sizeof(v), "unix:%s", cands[i]);
      setenv("PULSE_SERVER", v, 1);
      debugPrintf("audio: PULSE_SERVER=%s (fix HOME override)\n", v);
      return 1;
    }
  }
  return 0;
}

static void select_audio_backend(void) {
  const char *explicit_driver = getenv("SUMMERTIME_AUDIO_DRIVER");
  const char *inherited_driver = getenv("SDL_AUDIODRIVER");
  int pulse_available =
      command_available("pacat") && ensure_pulse_server();
  g_alsa_available = command_available("aplay");
  int escaped = 0;
  int explicit_choice = 0;

  if (ss_audio_parse_backend(explicit_driver, NULL) < 0) {
    debugPrintf("audio: SUMMERTIME_AUDIO_DRIVER inválido '%s'; usando auto\n",
                explicit_driver);
  }

  g_primary_backend = ss_audio_choose_backend(
      explicit_driver, inherited_driver, pulse_available, g_alsa_available,
      getenv("SUMMERTIME_AUDIO_KEEP_INHERITED_PULSE") != NULL, &escaped,
      &explicit_choice);
  g_allow_alsa_recovery =
      !explicit_choice && g_primary_backend == SS_AUDIO_BACKEND_PULSE &&
      g_alsa_available;

  debugPrintf("audio: policy explicit=%s inherited=%s pulse=%s alsa=%s "
              "selected=%s\n",
              explicit_driver && *explicit_driver ? explicit_driver : "auto",
              inherited_driver && *inherited_driver ? inherited_driver : "auto",
              pulse_available ? "yes" : "no",
              g_alsa_available ? "yes" : "no",
              ss_audio_backend_name(g_primary_backend));
  if (escaped) {
    debugPrintf("audio: Pulse herdado evitado; ALSA direto selecionado antes "
                "de abrir o backend de servidor\n");
  }
}

static void block_sigpipe_for_audio_thread(void) {
  sigset_t blocked;
  sigemptyset(&blocked);
  sigaddset(&blocked, SIGPIPE);
  pthread_sigmask(SIG_BLOCK, &blocked, NULL);
}

static void close_audio_sink(void) {
  if (!g_audio_sink)
    return;
  FILE *sink = g_audio_sink;
  g_audio_sink = NULL;
  int status = pclose(sink);
  if (getenv("SUMMERTIME_VERBOSE"))
    debugPrintf("audio: sink %s fechado status=%d\n",
                ss_audio_backend_name(g_active_backend), status);
}

static int open_audio_sink(enum ss_audio_backend backend, int sample_rate,
                           int audio_format, int channels,
                           int desired_frames) {
  const char *pulse_format = "s16le";
  const char *alsa_format = "S16_LE";
  if (audio_format == 3) {
    pulse_format = "u8";
    alsa_format = "U8";
  } else if (audio_format == 4) {
    pulse_format = "float32le";
    alsa_format = "FLOAT_LE";
  }

  close_audio_sink();
  g_active_backend = backend;
  if (backend == SS_AUDIO_BACKEND_NONE)
    return -1;

  char command[320];
  if (backend == SS_AUDIO_BACKEND_ALSA) {
    snprintf(command, sizeof(command),
             "aplay -q -t raw -f %s -r %d -c %d --buffer-time=250000 - "
             "2>/dev/null",
             alsa_format, sample_rate, channels);
  } else {
    snprintf(command, sizeof(command),
             "pacat --playback --rate=%d --channels=%d --format=%s "
             "--latency-msec=250 --client-name=summertimesaga 2>/dev/null",
             sample_rate, channels, pulse_format);
  }

  g_audio_sink = popen(command, "w");
  if (g_audio_sink)
    setvbuf(g_audio_sink, NULL, _IONBF, 0);
  debugPrintf("audio_open: rate=%d ch=%d fmt=%s frames=%d backend=%s %s\n",
              sample_rate, channels, pulse_format, desired_frames,
              ss_audio_backend_name(backend),
              g_audio_sink ? "OK" : "FAIL");
  return g_audio_sink ? 0 : -1;
}

static int audio_open(int sampleRate, int audioFormat, int channels,
                      int desiredFrames) {
  block_sigpipe_for_audio_thread();
  setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), -12);
  g_last_sample_rate = sampleRate;
  g_last_audio_format = audioFormat;
  g_last_channels = channels;
  g_last_frames = desiredFrames;
  g_recovery_attempted = 0;
  return open_audio_sink(g_primary_backend, sampleRate, audioFormat, channels,
                         desiredFrames);
}

static int write_audio_sink(const void *data, int length) {
  errno = 0;
  size_t written = fwrite(data, 1, (size_t)length, g_audio_sink);
  int flush_result = fflush(g_audio_sink);
  if (written == (size_t)length && flush_result == 0)
    return 0;
  debugPrintf("audio: write backend=%s falhou bytes=%zu/%d errno=%d\n",
              ss_audio_backend_name(g_active_backend), written, length, errno);
  return -1;
}

static void audio_write(void *data, int len_bytes) {
  if (!g_audio_sink || !data || len_bytes <= 0)
    return;
  block_sigpipe_for_audio_thread();
  if (write_audio_sink(data, len_bytes) != 0) {
    close_audio_sink();
    if (g_active_backend == SS_AUDIO_BACKEND_PULSE &&
        g_allow_alsa_recovery && !g_recovery_attempted) {
      g_recovery_attempted = 1;
      debugPrintf("audio: pacat encerrou; recuperando via ALSA/aplay\n");
      if (open_audio_sink(SS_AUDIO_BACKEND_ALSA, g_last_sample_rate,
                          g_last_audio_format, g_last_channels,
                          g_last_frames) == 0 &&
          write_audio_sink(data, len_bytes) == 0) {
        debugPrintf("audio: recuperação ALSA confirmada\n");
      } else {
        close_audio_sink();
        g_active_backend = SS_AUDIO_BACKEND_NONE;
      }
    } else {
      g_active_backend = SS_AUDIO_BACKEND_NONE;
    }
  }
  if (getenv("SUMMERTIME_VERBOSE")) {
    static long n = 0, total = 0;
    total += len_bytes;
    if ((n++ % 200) == 0)
      debugPrintf("audio_write: %ld calls, %ld KB total\n", n, total / 1024);
  }
}

static void audio_close(void) {
  close_audio_sink();
}

void summertime_audio_init(void) {
  select_audio_backend();
  jni_shim_set_audio_cb(audio_open, audio_write, audio_write, audio_close);
  debugPrintf("audio: sink wired (%s)\n",
              ss_audio_backend_name(g_primary_backend));
}
