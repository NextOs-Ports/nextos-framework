#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "android_callbacks.h"

#define GL2JNI_ACTIVITY_CLASS "com/gameloft/glf/GL2JNIActivity"

void debugPrintf(const char *format, ...) {
  (void)format;
}

static void require(int condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "failure: %s\n", message);
    exit(1);
  }
}

static int invoke_lifecycle(struct asm2_android_state *state,
                            const char *class_name, const char *name,
                            const char *signature, int is_static) {
  asm2_jni_value result;
  memset(&result, 0, sizeof(result));
  return asm2_android_method_callback(
      state, class_name, name, signature, is_static, NULL, NULL, NULL,
      &result);
}

struct exit_probe {
  struct asm2_android_state *state;
  _Atomic unsigned calls;
  _Atomic int nested_handled;
  _Atomic int saw_exit_request;
  _Atomic int saw_done_too_early;
  void *expected_environment;
  int reenter;
};

static void ASM2_GUEST_PCS guest_game_options_exit(void *environment,
                                                    void *class_handle) {
  struct exit_probe *probe = class_handle;
  if (!probe || environment != probe->expected_environment)
    abort();

  atomic_fetch_add_explicit(&probe->calls, 1u, memory_order_relaxed);
  atomic_store_explicit(
      &probe->saw_exit_request,
      asm2_android_get_lifecycle_request(probe->state) ==
          ASM2_ANDROID_LIFECYCLE_EXIT,
      memory_order_relaxed);
  atomic_store_explicit(
      &probe->saw_done_too_early,
      asm2_android_game_options_exit_done(probe->state),
      memory_order_relaxed);

  if (probe->reenter) {
    int handled =
        invoke_lifecycle(probe->state, GL2JNI_ACTIVITY_CLASS, "sExitGame",
                         "()V", 1);
    atomic_store_explicit(&probe->nested_handled, handled,
                          memory_order_relaxed);
  }
}

static void initialize_probe(struct asm2_android_state *state,
                             struct exit_probe *probe, int reenter) {
  asm2_android_state_init(state);
  memset(probe, 0, sizeof(*probe));
  probe->state = state;
  probe->expected_environment = state;
  probe->reenter = reenter;
  atomic_init(&probe->calls, 0u);
  atomic_init(&probe->nested_handled, 0);
  atomic_init(&probe->saw_exit_request, 0);
  atomic_init(&probe->saw_done_too_early, 0);
  asm2_android_bind_game_options_exit(
      state, guest_game_options_exit, probe->expected_environment, probe);
}

static void test_exact_dispatch_and_reentrancy(void) {
  struct asm2_android_state state;
  struct exit_probe probe;
  initialize_probe(&state, &probe, 1);

  require(ASM2_ANDROID_RESTART_EXIT_CODE == 75,
          "restart status remains launcher-reserved code 75");
  require(asm2_android_get_lifecycle_request(&state) ==
              ASM2_ANDROID_LIFECYCLE_NONE,
          "lifecycle starts at NONE");
  require(!asm2_android_lifecycle_requested(&state),
          "initial lifecycle is not terminal");
  require(!asm2_android_game_options_exit_done(&state),
          "GameOptions exit starts idle");

  require(!invoke_lifecycle(&state, GL2JNI_ACTIVITY_CLASS, "sExitGame",
                            "()I", 1),
          "sExitGame wrong signature is not the 1.2.7d callback");
  require(!invoke_lifecycle(&state, GL2JNI_ACTIVITY_CLASS, "sExitGame",
                            "()V", 0),
          "sExitGame instance method is rejected");
  require(!invoke_lifecycle(&state, "com/gameloft/glf/OtherActivity",
                            "sExitGame", "()V", 1),
          "sExitGame wrong class is rejected");
  require(asm2_android_get_lifecycle_request(&state) ==
              ASM2_ANDROID_LIFECYCLE_NONE,
          "rejected methods do not alter lifecycle");

  require(invoke_lifecycle(&state, GL2JNI_ACTIVITY_CLASS, "sExitGame",
                           "()V", 1),
          "exact static sExitGame()V is handled");
  require(asm2_android_get_lifecycle_request(&state) ==
              ASM2_ANDROID_LIFECYCLE_EXIT,
          "sExitGame requests EXIT");
  require(asm2_android_lifecycle_requested(&state),
          "EXIT is terminal");
  require(atomic_load_explicit(&probe.calls, memory_order_relaxed) == 1u,
          "GameOptions.ExitGame is dispatched exactly once");
  require(atomic_load_explicit(&probe.nested_handled,
                               memory_order_relaxed) == 1,
          "reentrant sExitGame remains a handled Java call");
  require(atomic_load_explicit(&probe.saw_exit_request,
                               memory_order_relaxed) == 1,
          "EXIT is published before reentrant GameOptions callback");
  require(atomic_load_explicit(&probe.saw_done_too_early,
                               memory_order_relaxed) == 0,
          "GameOptions exit is not marked done while it is running");
  require(asm2_android_game_options_exit_done(&state),
          "GameOptions exit is marked done after callback return");

  require(invoke_lifecycle(&state, GL2JNI_ACTIVITY_CLASS, "sExitGame",
                           "()V", 1),
          "duplicate sExitGame remains handled");
  require(invoke_lifecycle(&state, GL2JNI_ACTIVITY_CLASS, "RestartGame",
                           "()V", 1),
          "late RestartGame remains handled");
  require(atomic_load_explicit(&probe.calls, memory_order_relaxed) == 1u,
          "duplicate terminal callbacks cannot reenter GameOptions");
  require(asm2_android_get_lifecycle_request(&state) ==
              ASM2_ANDROID_LIFECYCLE_EXIT,
          "first terminal callback wins");
}

static void test_restart_and_missing_guest_fail_safe(void) {
  struct asm2_android_state restart_state;
  struct exit_probe restart_probe;
  initialize_probe(&restart_state, &restart_probe, 1);

  require(!invoke_lifecycle(&restart_state, GL2JNI_ACTIVITY_CLASS,
                            "RestartGame", "(I)V", 1),
          "RestartGame wrong signature is rejected");
  require(!invoke_lifecycle(&restart_state, GL2JNI_ACTIVITY_CLASS,
                            "RestartGame", "()V", 0),
          "RestartGame instance method is rejected");
  require(invoke_lifecycle(&restart_state, GL2JNI_ACTIVITY_CLASS,
                           "RestartGame", "()V", 1),
          "exact static RestartGame()V is handled");
  require(asm2_android_get_lifecycle_request(&restart_state) ==
              ASM2_ANDROID_LIFECYCLE_RESTART,
          "RestartGame requests RESTART");
  require(atomic_load_explicit(&restart_probe.calls,
                               memory_order_relaxed) == 0u,
          "RestartGame never dispatches GameOptions.ExitGame");
  require(!asm2_android_game_options_exit_done(&restart_state),
          "RestartGame leaves GameOptions exit undispatched");
  require(invoke_lifecycle(&restart_state, GL2JNI_ACTIVITY_CLASS,
                           "sExitGame", "()V", 1),
          "sExitGame after restart remains handled");
  require(atomic_load_explicit(&restart_probe.calls,
                               memory_order_relaxed) == 0u,
          "restart cannot be converted into an exit callback");
  require(asm2_android_get_lifecycle_request(&restart_state) ==
              ASM2_ANDROID_LIFECYCLE_RESTART,
          "RESTART remains the first terminal request");

  struct asm2_android_state missing_state;
  asm2_android_state_init(&missing_state);
  asm2_android_bind_game_options_exit(&missing_state, NULL, NULL, NULL);
  require(invoke_lifecycle(&missing_state, GL2JNI_ACTIVITY_CLASS,
                           "sExitGame", "()V", 1),
          "sExitGame is fail-safe without guest onExit symbol");
  require(asm2_android_get_lifecycle_request(&missing_state) ==
              ASM2_ANDROID_LIFECYCLE_EXIT,
          "missing guest callback still requests a safe EXIT");
  require(asm2_android_game_options_exit_done(&missing_state),
          "missing guest callback is marked complete");
}

struct concurrent_call {
  struct asm2_android_state *state;
  const char *name;
  _Atomic int *start;
  int handled;
};

static void *concurrent_callback(void *opaque) {
  struct concurrent_call *call = opaque;
  while (!atomic_load_explicit(call->start, memory_order_acquire))
    sched_yield();
  call->handled = invoke_lifecycle(
      call->state, GL2JNI_ACTIVITY_CLASS, call->name, "()V", 1);
  return NULL;
}

static void run_concurrent_terminal_test(int mixed_requests) {
  enum { THREAD_COUNT = 32 };
  struct asm2_android_state state;
  struct exit_probe probe;
  initialize_probe(&state, &probe, 1);
  _Atomic int start;
  atomic_init(&start, 0);
  pthread_t threads[THREAD_COUNT];
  struct concurrent_call calls[THREAD_COUNT];

  for (size_t index = 0; index < THREAD_COUNT; ++index) {
    calls[index] = (struct concurrent_call){
        .state = &state,
        .name = mixed_requests && (index & 1u) ? "RestartGame"
                                               : "sExitGame",
        .start = &start,
        .handled = 0,
    };
    require(pthread_create(&threads[index], NULL, concurrent_callback,
                           &calls[index]) == 0,
            "create concurrent lifecycle caller");
  }
  atomic_store_explicit(&start, 1, memory_order_release);
  for (size_t index = 0; index < THREAD_COUNT; ++index) {
    require(pthread_join(threads[index], NULL) == 0,
            "join concurrent lifecycle caller");
    require(calls[index].handled,
            "every exact concurrent lifecycle callback is handled");
  }

  enum asm2_android_lifecycle_request request =
      asm2_android_get_lifecycle_request(&state);
  if (!mixed_requests) {
    require(request == ASM2_ANDROID_LIFECYCLE_EXIT,
            "concurrent sExitGame requests converge on EXIT");
    require(atomic_load_explicit(&probe.calls, memory_order_relaxed) == 1u,
            "concurrent sExitGame dispatches guest onExit once");
    require(asm2_android_game_options_exit_done(&state),
            "concurrent guest onExit completes");
  } else if (request == ASM2_ANDROID_LIFECYCLE_EXIT) {
    require(atomic_load_explicit(&probe.calls, memory_order_relaxed) == 1u,
            "EXIT winner dispatches guest onExit once");
    require(asm2_android_game_options_exit_done(&state),
            "EXIT winner marks guest onExit done");
  } else {
    require(request == ASM2_ANDROID_LIFECYCLE_RESTART,
            "mixed terminal race has a valid first winner");
    require(atomic_load_explicit(&probe.calls, memory_order_relaxed) == 0u,
            "RESTART winner never dispatches guest onExit");
    require(!asm2_android_game_options_exit_done(&state),
            "RESTART winner leaves guest onExit idle");
  }
}

int main(void) {
  test_exact_dispatch_and_reentrancy();
  test_restart_and_missing_guest_fail_safe();
  run_concurrent_terminal_test(0);
  run_concurrent_terminal_test(1);
  puts("Android lifecycle: exact callbacks, once-only exit and restart OK");
  return 0;
}
