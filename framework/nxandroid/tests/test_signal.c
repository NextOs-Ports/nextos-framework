/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxandroid.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define RESULT_MAGIC UINT32_C(0x4e585347)
#define TRACE_MAX 32u
#define IO_TIMEOUT_MS INT64_C(8000)
#define REAP_TIMEOUT_MS INT64_C(2000)

enum signal_case_id {
  SIGNAL_CASE_ACTIVE = 1,
  SIGNAL_CASE_EARLY = 2
};

typedef struct child_result {
  uint32_t magic;
  uint32_t case_id;
  int passed;
  int run_result;
  int abort_result;
  int destroy_result;
  int final_state;
  int term_seen;
  uint32_t trace_count;
  uint32_t rollback_count;
  uint32_t trace[TRACE_MAX];
  uint32_t rollback_trace[TRACE_MAX];
} child_result;

typedef struct fixture_adapter {
  int case_id;
  int ready_fd;
  int signal_read_fd;
  child_result *result;
} fixture_adapter;

typedef struct owned_child {
  pid_t pid;
  int pidfd;
} owned_child;

static volatile sig_atomic_t signal_seen = 0;
static volatile sig_atomic_t signal_write_fd = -1;

static const nxandroid_module_spec signal_modules[] = {
    {"synthetic-signal-module", NXANDROID_JNI_REQUIRED},
};

#define SIGNAL_STEP(phase_value, module_value, cycle_value, id_value,          \
                    rollback_value, terminal_value, group_value, close_value) \
  {                                                                            \
    phase_value, module_value, cycle_value, id_value, rollback_value,           \
        terminal_value, group_value, close_value                               \
  }

static const nxandroid_step signal_steps[] = {
    SIGNAL_STEP(NXANDROID_PHASE_MODULE_INITIALIZED, 0u, 0u, "signal-00", NULL,
                NXANDROID_TERMINAL_NONE, 0u, 0u),
    SIGNAL_STEP(NXANDROID_PHASE_MODULE_JNI, 0u, 0u, "signal-01", NULL,
                NXANDROID_TERMINAL_NONE, 0u, 0u),
    SIGNAL_STEP(NXANDROID_PHASE_ACTIVITY_CREATE, NXANDROID_NO_MODULE, 0u,
                "signal-02", "rollback-activity", NXANDROID_TERMINAL_NONE,
                1u, 0u),
    SIGNAL_STEP(NXANDROID_PHASE_GRAPHICS_REQUEST, NXANDROID_NO_MODULE, 1u,
                "signal-03", "rollback-graphics", NXANDROID_TERMINAL_NONE,
                2u, 0u),
    SIGNAL_STEP(NXANDROID_PHASE_SURFACE_UP, NXANDROID_NO_MODULE, 1u,
                "signal-04", "rollback-surface", NXANDROID_TERMINAL_NONE, 2u,
                0u),
    SIGNAL_STEP(NXANDROID_PHASE_GL_READY, NXANDROID_NO_MODULE, 1u, "signal-05",
                "rollback-gl", NXANDROID_TERMINAL_NONE, 2u, 0u),
    SIGNAL_STEP(NXANDROID_PHASE_RESUME, NXANDROID_NO_MODULE, 1u, "signal-06",
                "rollback-resume", NXANDROID_TERMINAL_NONE, 3u, 0u),
    SIGNAL_STEP(NXANDROID_PHASE_FOCUS_GAIN, NXANDROID_NO_MODULE, 1u,
                "signal-07", "rollback-focus", NXANDROID_TERMINAL_NONE, 4u,
                0u),
    SIGNAL_STEP(NXANDROID_PHASE_ENTRY, NXANDROID_NO_MODULE, 1u, "signal-08",
                NULL, NXANDROID_TERMINAL_NONE, 0u, 0u),
    SIGNAL_STEP(NXANDROID_PHASE_OBJECTS_READY, NXANDROID_NO_MODULE, 1u,
                "signal-09", NULL, NXANDROID_TERMINAL_NONE, 0u, 0u),
    SIGNAL_STEP(NXANDROID_PHASE_INPUT_ENABLE, NXANDROID_NO_MODULE, 1u,
                "signal-10", "rollback-input", NXANDROID_TERMINAL_NONE, 5u,
                0u),
    SIGNAL_STEP(NXANDROID_PHASE_RUN_LOOP, NXANDROID_NO_MODULE, 1u, "signal-11",
                NULL, NXANDROID_TERMINAL_NONE, 0u, 0u),
    SIGNAL_STEP(NXANDROID_PHASE_INPUT_DISABLE, NXANDROID_NO_MODULE, 1u,
                "signal-12", NULL, NXANDROID_TERMINAL_NONE, 0u, 5u),
    SIGNAL_STEP(NXANDROID_PHASE_FOCUS_LOSS, NXANDROID_NO_MODULE, 1u,
                "signal-13", NULL, NXANDROID_TERMINAL_NONE, 0u, 4u),
    SIGNAL_STEP(NXANDROID_PHASE_PAUSE, NXANDROID_NO_MODULE, 1u, "signal-14",
                NULL, NXANDROID_TERMINAL_NONE, 0u, 3u),
    SIGNAL_STEP(NXANDROID_PHASE_SAVE, NXANDROID_NO_MODULE, 0u, "signal-15",
                NULL, NXANDROID_TERMINAL_NONE, 0u, 0u),
    SIGNAL_STEP(NXANDROID_PHASE_SURFACE_DOWN, NXANDROID_NO_MODULE, 1u,
                "signal-16", NULL, NXANDROID_TERMINAL_NONE, 0u, 2u),
    SIGNAL_STEP(NXANDROID_PHASE_NATIVE_SHUTDOWN, NXANDROID_NO_MODULE, 0u,
                "signal-17", NULL, NXANDROID_TERMINAL_NONE, 0u, 1u),
    SIGNAL_STEP(NXANDROID_PHASE_TERMINAL, NXANDROID_NO_MODULE, 0u, "signal-18",
                NULL, NXANDROID_TERMINAL_RETURN, 0u, 0u),
};

static void handle_term(int selected_signal) {
  const unsigned char token = 1u;
  int saved_errno = errno;
  (void)selected_signal;
  signal_seen = 1;
  if (signal_write_fd >= 0)
    (void)write((int)signal_write_fd, &token, sizeof(token));
  errno = saved_errno;
}

static int64_t monotonic_milliseconds(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return -1;
  return (int64_t)now.tv_sec * INT64_C(1000) + now.tv_nsec / 1000000;
}

static int poll_until(int descriptor, short events, int64_t deadline_ms) {
  struct pollfd item;
  for (;;) {
    int64_t now = monotonic_milliseconds();
    int64_t remaining;
    int timeout;
    int status;
    if (now < 0)
      return -1;
    remaining = deadline_ms - now;
    if (remaining < 0)
      remaining = 0;
    timeout = remaining > INT_MAX ? INT_MAX : (int)remaining;
    item.fd = descriptor;
    item.events = events;
    item.revents = 0;
    status = poll(&item, 1u, timeout);
    if (status > 0) {
      if ((item.revents & events) != 0)
        return 1;
      if ((item.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        return 0;
      continue;
    }
    if (status == 0)
      return 0;
    if (errno != EINTR)
      return -1;
  }
}

static int read_exact_deadline(int descriptor, void *output, size_t size,
                               int64_t timeout_ms) {
  unsigned char *cursor = (unsigned char *)output;
  size_t completed = 0u;
  int64_t start = monotonic_milliseconds();
  int64_t deadline;
  if (start < 0 || timeout_ms < 0 || start > INT64_MAX - timeout_ms)
    return -1;
  deadline = start + timeout_ms;
  while (completed < size) {
    ssize_t count;
    int ready = poll_until(descriptor, POLLIN, deadline);
    if (ready != 1)
      return -1;
    count = read(descriptor, cursor + completed, size - completed);
    if (count > 0) {
      completed += (size_t)count;
      continue;
    }
    if (count == 0)
      return -1;
    if (errno != EINTR && errno != EAGAIN)
      return -1;
  }
  return 0;
}

static int write_all(int descriptor, const void *input, size_t size) {
  const unsigned char *cursor = (const unsigned char *)input;
  size_t completed = 0u;
  while (completed < size) {
    ssize_t count = write(descriptor, cursor + completed, size - completed);
    if (count > 0) {
      completed += (size_t)count;
      continue;
    }
    if (count < 0 && errno == EINTR)
      continue;
    return -1;
  }
  return 0;
}

static int wait_for_term(int descriptor) {
  int64_t start = monotonic_milliseconds();
  int64_t deadline;
  unsigned char buffer[32];
  if (start < 0 || start > INT64_MAX - IO_TIMEOUT_MS)
    return -1;
  deadline = start + IO_TIMEOUT_MS;
  while (!signal_seen) {
    int ready = poll_until(descriptor, POLLIN, deadline);
    if (ready != 1)
      return -1;
    for (;;) {
      ssize_t count = read(descriptor, buffer, sizeof(buffer));
      if (count > 0)
        continue;
      if (count < 0 && errno == EINTR)
        continue;
      if (count < 0 && errno == EAGAIN)
        break;
      if (count == 0)
        return -1;
      return -1;
    }
  }
  return 0;
}

static int append_phase(uint32_t *trace, uint32_t *count,
                        nxandroid_phase phase) {
  if (*count >= TRACE_MAX)
    return -1;
  trace[*count] = (uint32_t)phase;
  ++*count;
  return 0;
}

static int fixture_invoke(void *userdata, const nxandroid_step *step) {
  fixture_adapter *fixture = (fixture_adapter *)userdata;
  unsigned char ready = 'A';
  if (append_phase(fixture->result->trace, &fixture->result->trace_count,
                   step->phase) != 0)
    return 91;
  if (step->phase != NXANDROID_PHASE_RUN_LOOP)
    return 0;
  if (fixture->case_id != SIGNAL_CASE_ACTIVE ||
      write_all(fixture->ready_fd, &ready, sizeof(ready)) != 0 ||
      wait_for_term(fixture->signal_read_fd) != 0)
    return 92;
  return 0;
}

static int fixture_rollback(void *userdata, const nxandroid_step *step) {
  fixture_adapter *fixture = (fixture_adapter *)userdata;
  return append_phase(fixture->result->rollback_trace,
                      &fixture->result->rollback_count, step->phase);
}

static nxandroid_profile signal_profile(void) {
  nxandroid_profile profile;
  memset(&profile, 0, sizeof(profile));
  profile.api_version = NXANDROID_API_VERSION;
  profile.struct_size = sizeof(profile);
  profile.modules = signal_modules;
  profile.module_count = ARRAY_SIZE(signal_modules);
  profile.steps = signal_steps;
  profile.step_count = ARRAY_SIZE(signal_steps);
  return profile;
}

static nxandroid_ops signal_ops(fixture_adapter *fixture) {
  nxandroid_ops ops;
  memset(&ops, 0, sizeof(ops));
  ops.api_version = NXANDROID_API_VERSION;
  ops.struct_size = sizeof(ops);
  ops.invoke = fixture_invoke;
  ops.rollback = fixture_rollback;
  ops.userdata = fixture;
  return ops;
}

static int trace_count(const child_result *result, nxandroid_phase phase) {
  uint32_t index;
  int count = 0;
  for (index = 0u; index < result->trace_count; ++index) {
    if (result->trace[index] == (uint32_t)phase)
      ++count;
  }
  return count;
}

static int verify_active_result(const child_result *result) {
  static const nxandroid_phase shutdown_tail[] = {
      NXANDROID_PHASE_INPUT_DISABLE, NXANDROID_PHASE_FOCUS_LOSS,
      NXANDROID_PHASE_PAUSE,         NXANDROID_PHASE_SAVE,
      NXANDROID_PHASE_SURFACE_DOWN,  NXANDROID_PHASE_NATIVE_SHUTDOWN,
      NXANDROID_PHASE_TERMINAL,
  };
  size_t index;
  size_t tail_begin = ARRAY_SIZE(signal_steps) - ARRAY_SIZE(shutdown_tail);
  if (result->run_result != NXANDROID_OK ||
      result->final_state != NXANDROID_CONTEXT_COMPLETE ||
      result->destroy_result != NXANDROID_OK || result->term_seen != 1 ||
      result->trace_count != ARRAY_SIZE(signal_steps) ||
      result->rollback_count != 0u)
    return 0;
  for (index = 0u; index < ARRAY_SIZE(shutdown_tail); ++index) {
    nxandroid_phase phase = shutdown_tail[index];
    if (result->trace[tail_begin + index] != (uint32_t)phase ||
        trace_count(result, phase) != 1)
      return 0;
  }
  return trace_count(result, NXANDROID_PHASE_RUN_LOOP) == 1;
}

static int verify_early_result(const child_result *result) {
  uint32_t index;
  if (result->abort_result != NXANDROID_OK ||
      result->final_state != NXANDROID_CONTEXT_ABORTED ||
      result->destroy_result != NXANDROID_OK || result->term_seen != 1 ||
      result->trace_count != 3u || result->rollback_count != 1u ||
      result->rollback_trace[0] !=
          (uint32_t)NXANDROID_PHASE_ACTIVITY_CREATE)
    return 0;
  for (index = 0u; index < result->trace_count; ++index) {
    if (result->trace[index] == (uint32_t)NXANDROID_PHASE_ENTRY ||
        result->trace[index] == (uint32_t)NXANDROID_PHASE_OBJECTS_READY ||
        result->trace[index] == (uint32_t)NXANDROID_PHASE_INPUT_ENABLE ||
        result->trace[index] == (uint32_t)NXANDROID_PHASE_RUN_LOOP)
      return 0;
  }
  return 1;
}

static int install_term_handler(int write_descriptor) {
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_handler = handle_term;
  if (sigemptyset(&action.sa_mask) != 0)
    return -1;
  signal_seen = 0;
  signal_write_fd = (sig_atomic_t)write_descriptor;
  return sigaction(SIGTERM, &action, NULL);
}

static void child_case(int case_id, int ready_fd, int result_fd) {
  child_result result;
  fixture_adapter fixture;
  nxandroid_context *context = NULL;
  nxandroid_profile profile = signal_profile();
  nxandroid_ops ops;
  int signal_pipe[2] = {-1, -1};
  int create_result;
  unsigned char ready = 'E';
  size_t index;

  memset(&result, 0, sizeof(result));
  result.magic = RESULT_MAGIC;
  result.case_id = (uint32_t)case_id;
  result.run_result = NXANDROID_ESTATE;
  result.abort_result = NXANDROID_ESTATE;
  result.destroy_result = NXANDROID_ESTATE;
  result.final_state = NXANDROID_CONTEXT_FAILED;
  memset(&fixture, 0, sizeof(fixture));
  fixture.case_id = case_id;
  fixture.ready_fd = ready_fd;
  fixture.result = &result;

  if (pipe2(signal_pipe, O_CLOEXEC | O_NONBLOCK) != 0)
    goto finished;
  fixture.signal_read_fd = signal_pipe[0];
  if (install_term_handler(signal_pipe[1]) != 0)
    goto finished;
  ops = signal_ops(&fixture);
  create_result = nxandroid_context_create(&profile, &ops, &context);
  if (create_result != NXANDROID_OK)
    goto finished;

  if (case_id == SIGNAL_CASE_ACTIVE) {
    result.run_result = nxandroid_context_run(context);
    result.final_state = (int)nxandroid_context_get_state(context);
  } else {
    for (index = 0u; index < 3u; ++index) {
      if (nxandroid_context_step(context) != NXANDROID_OK)
        goto destroy_context;
    }
    if (write_all(ready_fd, &ready, sizeof(ready)) != 0 ||
        wait_for_term(signal_pipe[0]) != 0)
      goto destroy_context;
    result.abort_result = nxandroid_context_abort(context);
    result.final_state = (int)nxandroid_context_get_state(context);
  }

destroy_context:
  result.term_seen = signal_seen ? 1 : 0;
  result.destroy_result = nxandroid_context_destroy(&context);
  if (case_id == SIGNAL_CASE_ACTIVE)
    result.passed = verify_active_result(&result);
  else
    result.passed = verify_early_result(&result);

finished:
  signal_write_fd = -1;
  if (signal_pipe[0] >= 0)
    (void)close(signal_pipe[0]);
  if (signal_pipe[1] >= 0)
    (void)close(signal_pipe[1]);
  (void)write_all(result_fd, &result, sizeof(result));
  (void)close(ready_fd);
  (void)close(result_fd);
  _exit(result.passed ? 0 : 1);
}

static int read_namespace_link(const char *path, char *output,
                               size_t output_size) {
  ssize_t count;
  if (output_size < 2u)
    return -1;
  count = readlink(path, output, output_size - 1u);
  if (count <= 0 || (size_t)count >= output_size)
    return -1;
  output[count] = '\0';
  return 0;
}

static int require_private_namespace(void) {
  static const char *const kinds[] = {"pid", "user", "mnt"};
  static const char *const variables[] = {
      "NXBOOTSTRAP_TEST_HOST_PID_NS", "NXBOOTSTRAP_TEST_HOST_USER_NS",
      "NXBOOTSTRAP_TEST_HOST_MOUNT_NS"};
  char path[64];
  char current[128];
  char init_pid[128];
  size_t index;
  const char *marker = getenv("NXBOOTSTRAP_TEST_PRIVATE_PID_NS");
  if (marker == NULL || strcmp(marker, "1") != 0)
    return -1;
  for (index = 0u; index < ARRAY_SIZE(kinds); ++index) {
    const char *host = getenv(variables[index]);
    int count;
    if (host == NULL || host[0] == '\0')
      return -1;
    count = snprintf(path, sizeof(path), "/proc/self/ns/%s", kinds[index]);
    if (count < 0 || (size_t)count >= sizeof(path) ||
        read_namespace_link(path, current, sizeof(current)) != 0 ||
        strcmp(current, host) == 0)
      return -1;
  }
  if (read_namespace_link("/proc/self/ns/pid", current, sizeof(current)) != 0 ||
      read_namespace_link("/proc/1/ns/pid", init_pid, sizeof(init_pid)) != 0 ||
      strcmp(current, init_pid) != 0)
    return -1;
  return 0;
}

static int pidfd_open_exact(pid_t pid) {
#ifdef SYS_pidfd_open
  return (int)syscall(SYS_pidfd_open, pid, 0u);
#else
  (void)pid;
  errno = ENOSYS;
  return -1;
#endif
}

static int pidfd_send_exact(int descriptor, int selected_signal) {
#ifdef SYS_pidfd_send_signal
  return (int)syscall(SYS_pidfd_send_signal, descriptor, selected_signal, NULL,
                      0u);
#else
  (void)descriptor;
  (void)selected_signal;
  errno = ENOSYS;
  return -1;
#endif
}

static int probe_pidfd_authority(void) {
  int descriptor = pidfd_open_exact(getpid());
  int status;
  if (descriptor < 0)
    return -1;
  status = pidfd_send_exact(descriptor, 0);
  (void)close(descriptor);
  return status;
}

static int child_is_alive(const owned_child *child) {
  struct pollfd item;
  int status;
  item.fd = child->pidfd;
  item.events = POLLIN;
  item.revents = 0;
  status = poll(&item, 1u, 0);
  return status == 0;
}

static int reap_child(owned_child *child, int64_t timeout_ms,
                      int *wait_status) {
  int64_t start;
  int ready;
  pid_t waited;
  if (child->pid <= 0 || child->pidfd < 0)
    return -1;
  start = monotonic_milliseconds();
  if (start < 0 || start > INT64_MAX - timeout_ms)
    return -1;
  ready = poll_until(child->pidfd, POLLIN, start + timeout_ms);
  if (ready != 1)
    return 0;
  do {
    waited = waitpid(child->pid, wait_status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited != child->pid)
    return -1;
  child->pid = -1;
  return 1;
}

static void cleanup_target(owned_child *target) {
  int status = 0;
  int reaped;
  if (target->pid <= 0)
    goto close_pidfd;
  reaped = reap_child(target, 0, &status);
  if (reaped == 0) {
    (void)pidfd_send_exact(target->pidfd, SIGTERM);
    reaped = reap_child(target, 500, &status);
  }
  if (reaped == 0) {
    (void)pidfd_send_exact(target->pidfd, SIGKILL);
    (void)reap_child(target, REAP_TIMEOUT_MS, &status);
  }
close_pidfd:
  if (target->pidfd >= 0)
    (void)close(target->pidfd);
  target->pidfd = -1;
}

static int spawn_sibling(owned_child *sibling, int *control_fd,
                         int *ack_fd) {
  int control[2] = {-1, -1};
  int ack[2] = {-1, -1};
  pid_t pid;
  if (pipe2(control, O_CLOEXEC) != 0)
    return -1;
  if (pipe2(ack, O_CLOEXEC) != 0) {
    (void)close(control[0]);
    (void)close(control[1]);
    return -1;
  }
  pid = fork();
  if (pid < 0) {
    (void)close(control[0]);
    (void)close(control[1]);
    (void)close(ack[0]);
    (void)close(ack[1]);
    return -1;
  }
  if (pid == 0) {
    unsigned char command = 0u;
    unsigned char reply = 'I';
    (void)close(control[1]);
    (void)close(ack[0]);
    if (read(control[0], &command, sizeof(command)) == 1 && command == 'Q' &&
        write_all(ack[1], &reply, sizeof(reply)) == 0)
      _exit(0);
    _exit(2);
  }
  (void)close(control[0]);
  (void)close(ack[1]);
  sibling->pid = pid;
  sibling->pidfd = pidfd_open_exact(pid);
  *control_fd = control[1];
  *ack_fd = ack[0];
  if (sibling->pidfd < 0) {
    (void)close(*control_fd);
    (void)close(*ack_fd);
    (void)waitpid(pid, NULL, 0);
    sibling->pid = -1;
    return -1;
  }
  return 0;
}

static int finish_sibling(owned_child *sibling, int control_fd, int ack_fd) {
  unsigned char command = 'Q';
  unsigned char reply = 0u;
  int status = 0;
  int reaped;
  int ok = child_is_alive(sibling);

  if (write_all(control_fd, &command, sizeof(command)) != 0)
    ok = 0;
  (void)close(control_fd);
  if (read_exact_deadline(ack_fd, &reply, sizeof(reply), IO_TIMEOUT_MS) != 0 ||
      reply != 'I')
    ok = 0;
  (void)close(ack_fd);
  reaped = reap_child(sibling, REAP_TIMEOUT_MS, &status);
  if (reaped != 1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
    ok = 0;
  if (reaped != 1) {
    cleanup_target(sibling);
  } else {
    (void)close(sibling->pidfd);
    sibling->pidfd = -1;
  }
  return ok ? 0 : -1;
}

static int parent_verify_result(const child_result *result, int case_id) {
  if (result->magic != RESULT_MAGIC || result->case_id != (uint32_t)case_id ||
      !result->passed)
    return 0;
  return case_id == SIGNAL_CASE_ACTIVE ? verify_active_result(result)
                                       : verify_early_result(result);
}

static int run_signal_case(int case_id, const owned_child *sibling,
                           int sibling_control_fd, int sibling_ack_fd) {
  int launch[2] = {-1, -1};
  int ready[2] = {-1, -1};
  int report[2] = {-1, -1};
  owned_child target = {-1, -1};
  child_result result;
  unsigned char command = 'G';
  unsigned char ready_token = 0u;
  pid_t owned_pid = -1;
  int status = 0;
  int passed = 0;

  if (pipe2(launch, O_CLOEXEC) != 0 || pipe2(ready, O_CLOEXEC) != 0 ||
      pipe2(report, O_CLOEXEC) != 0)
    goto finished;
  target.pid = fork();
  if (target.pid < 0)
    goto finished;
  owned_pid = target.pid;
  if (target.pid == 0) {
    unsigned char launch_token = 0u;
    (void)close(launch[1]);
    (void)close(ready[0]);
    (void)close(report[0]);
    (void)close(sibling_control_fd);
    (void)close(sibling_ack_fd);
    if (read(launch[0], &launch_token, sizeof(launch_token)) != 1 ||
        launch_token != 'G')
      _exit(3);
    (void)close(launch[0]);
    child_case(case_id, ready[1], report[1]);
  }
  (void)close(launch[0]);
  launch[0] = -1;
  (void)close(ready[1]);
  ready[1] = -1;
  (void)close(report[1]);
  report[1] = -1;
  target.pidfd = pidfd_open_exact(target.pid);
  if (target.pidfd < 0 ||
      write_all(launch[1], &command, sizeof(command)) != 0)
    goto finished;
  (void)close(launch[1]);
  launch[1] = -1;

  if (read_exact_deadline(ready[0], &ready_token, sizeof(ready_token),
                          IO_TIMEOUT_MS) != 0 ||
      ready_token != (case_id == SIGNAL_CASE_ACTIVE ? 'A' : 'E') ||
      !child_is_alive(&target) || !child_is_alive(sibling))
    goto finished;

  if (pidfd_send_exact(target.pidfd, SIGTERM) != 0 ||
      read_exact_deadline(report[0], &result, sizeof(result), IO_TIMEOUT_MS) !=
          0 ||
      reap_child(&target, REAP_TIMEOUT_MS, &status) != 1 ||
      !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
      !parent_verify_result(&result, case_id) || !child_is_alive(sibling))
    goto finished;

  printf("nxandroid-signal: case=%s child_pid=%ld pidfd=%d PASS\n",
         case_id == SIGNAL_CASE_ACTIVE ? "active" : "early",
         (long)owned_pid, target.pidfd);
  passed = 1;

finished:
  if (launch[0] >= 0)
    (void)close(launch[0]);
  if (launch[1] >= 0)
    (void)close(launch[1]);
  if (ready[0] >= 0)
    (void)close(ready[0]);
  if (ready[1] >= 0)
    (void)close(ready[1]);
  if (report[0] >= 0)
    (void)close(report[0]);
  if (report[1] >= 0)
    (void)close(report[1]);
  cleanup_target(&target);
  return passed ? 0 : -1;
}

int main(void) {
  owned_child sibling = {-1, -1};
  int sibling_control_fd = -1;
  int sibling_ack_fd = -1;
  int active_result;
  int early_result;
  int sibling_result;

  if (require_private_namespace() != 0) {
    fprintf(stderr,
            "nxandroid-signal: SKIP private namespace contract missing\n");
    return 77;
  }
  if (probe_pidfd_authority() != 0) {
    fprintf(stderr, "nxandroid-signal: SKIP pidfd authority unavailable\n");
    return 77;
  }
  if (spawn_sibling(&sibling, &sibling_control_fd, &sibling_ack_fd) != 0) {
    fprintf(stderr, "nxandroid-signal: failed to create sibling proof\n");
    return 1;
  }

  active_result = run_signal_case(SIGNAL_CASE_ACTIVE, &sibling,
                                  sibling_control_fd, sibling_ack_fd);
  early_result = run_signal_case(SIGNAL_CASE_EARLY, &sibling,
                                 sibling_control_fd, sibling_ack_fd);
  sibling_result =
      finish_sibling(&sibling, sibling_control_fd, sibling_ack_fd);
  if (active_result != 0 || early_result != 0 || sibling_result != 0) {
    fprintf(stderr, "nxandroid-signal: FAIL active=%d early=%d sibling=%d\n",
            active_result, early_result, sibling_result);
    return 1;
  }

  puts("nxandroid_signal_test=PASS");
  puts("private_pid_namespace=1");
  puts("signal_authority=pidfd");
  puts("active_forward_exactly_once=1");
  puts("early_rollback_exactly_once=1");
  puts("sibling_intact=1");
  puts("guest_code_executed=0");
  puts("guest_initializers_executed=0");
  puts("guest_jni_onload_executed=0");
  puts("device_access=0");
  puts("network_access=0");
  puts("hardware_ran=0");
  return 0;
}
