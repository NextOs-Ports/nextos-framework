/*
 * ARMv7 host-ABI smoke test for the cross gate.
 *
 * This executable never opens or maps a guest ELF and never calls a guest
 * initializer.  It exercises only the native ARMHF build, the explicit
 * registry and the proven softfp -> hardfp libm boundary.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "nxloader_softfp.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if !defined(__arm__)
#error "test_armv7_cross.c must be compiled for 32-bit ARM"
#endif

typedef float(__attribute__((pcs("aapcs"))) * nx_softfp_float_unary_fn)(
    float);
typedef double(__attribute__((pcs("aapcs"))) * nx_softfp_double_binary_fn)(
    double, double);
typedef uint32_t(__attribute__((pcs("aapcs"))) * nx_softfp_stack_probe_fn)(
    float, double, float, double, float);
typedef void(__attribute__((pcs("aapcs"))) * nx_softfp_entry_sp_probe_fn)(
    uint32_t *, float, double, float, double, float);

static int failures;

#define CHECK(expression)                                                   \
  do {                                                                      \
    if (!(expression)) {                                                    \
      fprintf(stderr, "armv7-cross: CHECK failed at %s:%d: %s\n",          \
              __FILE__, __LINE__, #expression);                             \
      failures++;                                                           \
    }                                                                       \
  } while (0)

static uintptr_t read_stack_pointer(void) {
  uintptr_t value;
  __asm__ volatile("mov %0, sp" : "=r"(value));
  return value;
}

/* Five mixed FP arguments force the base AAPCS call to use both core
 * registers and the stack.  The probe is test-owned host code, not guest
 * code. */
static uint32_t NXLOADER_ARM_SOFTFP __attribute__((noinline))
nx_softfp_stack_probe(float first, double second, float third, double fourth,
                      float fifth) {
  if (first != 1.0f || second != 2.0 || third != 3.0f || fourth != 4.0 ||
      fifth != 5.0f)
    return UINT32_C(0xbad00002);
  return UINT32_C(0x51f7a11e);
}

/* A naked probe observes SP before a compiler prologue is allowed to adjust
 * it. Parameter names are explicitly unused because the result is solely the
 * AAPCS public-entry alignment. The gate compiles this file in ARM mode. */
static void NXLOADER_ARM_SOFTFP __attribute__((naked, noinline))
nx_softfp_entry_sp_probe(
    uint32_t *result __attribute__((unused)), float first __attribute__((unused)),
    double second __attribute__((unused)), float third __attribute__((unused)),
    double fourth __attribute__((unused)), float fifth __attribute__((unused))) {
  __asm__ volatile("and ip, sp, #7\n\tstr ip, [r0]\n\tbx lr");
}

/* Deterministic assembly is used instead of trusting register allocation for
 * the callee-saved VFP proof. r0 receives the registry-resolved softfp thunk.
 * The probe saves its caller's d8, seeds d8, performs a real BLX through that
 * thunk, verifies the exact 64-bit pattern and restores d8 before returning. */
static void __attribute__((naked, noinline)) nx_vfp_d8_preserved(
    uintptr_t thunk_address __attribute__((unused)),
    uint32_t *result __attribute__((unused))) {
  __asm__ volatile(
      "push {r4, r5, r6, lr}\n\t"
      "vpush {d8}\n\t"
      "mov r4, r0\n\t"
      "mov r5, r1\n\t"
      "movw r2, #0xcdef\n\t"
      "movt r2, #0x89ab\n\t"
      "movw r3, #0x4567\n\t"
      "movt r3, #0x0123\n\t"
      "vmov d8, r2, r3\n\t"
      "movw r0, #0x0000\n\t"
      "movt r0, #0x3f00\n\t" /* softfp float 0.5f in r0 */
      "blx r4\n\t"
      "vmov r2, r3, d8\n\t"
      "movw r0, #0xcdef\n\t"
      "movt r0, #0x89ab\n\t"
      "cmp r2, r0\n\t"
      "bne 1f\n\t"
      "movw r1, #0x4567\n\t"
      "movt r1, #0x0123\n\t"
      "cmp r3, r1\n\t"
      "moveq r0, #1\n\t"
      "movne r0, #0\n\t"
      "b 2f\n"
      "1:\n\t"
      "mov r0, #0\n"
      "2:\n\t"
      "str r0, [r5]\n\t"
      "vpop {d8}\n\t"
      "pop {r4, r5, r6, pc}\n\t");
}

static int nearly_equal_float(float left, float right, float tolerance) {
  float difference = left - right;
  if (difference < 0.0f)
    difference = -difference;
  return difference <= tolerance;
}

static int nearly_equal_double(double left, double right, double tolerance) {
  double difference = left - right;
  if (difference < 0.0)
    difference = -difference;
  return difference <= tolerance;
}

static uintptr_t lookup(nxloader_registry *registry, const char *name) {
  nxloader_registry_match match;
  nxloader_result result;
  memset(&match, 0, sizeof(match));
  match.struct_size = sizeof(match);
  result = nxloader_registry_lookup(registry, name, &match);
  CHECK(result == NXLOADER_OK);
  CHECK(match.address != 0);
  CHECK(match.provider != NULL);
  CHECK(match.provider != NULL &&
        strcmp(match.provider, "armv7-cross-softfp") == 0);
  return result == NXLOADER_OK ? match.address : 0;
}

int main(void) {
  nxloader_registry *registry = NULL;
  nxloader_registry_report report;
  nxloader_result result;
  nx_softfp_float_unary_fn guest_sinf = NULL;
  nx_softfp_double_binary_fn guest_pow = NULL;
  nx_softfp_stack_probe_fn volatile stack_probe = nx_softfp_stack_probe;
  nx_softfp_entry_sp_probe_fn volatile entry_sp_probe =
      nx_softfp_entry_sp_probe;
  uintptr_t address;
  uintptr_t sinf_address = 0;
  volatile float float_input = 0.5f;
  volatile double double_base = 2.0;
  volatile double double_exponent = 5.0;
  float float_result;
  double double_result;
  uint32_t stack_result;
  uint32_t entry_sp_alignment = UINT32_MAX;
  uint32_t vfp_d8_result = 0;

  CHECK(sizeof(void *) == 4);
  CHECK(nxloader_process_arch() == NXLOADER_ARCH_ARMV7);
  CHECK((read_stack_pointer() & (uintptr_t)7u) == 0);

  result = nxloader_registry_create(&registry);
  CHECK(result == NXLOADER_OK);
  if (result != NXLOADER_OK)
    return 1;

  memset(&report, 0, sizeof(report));
  report.struct_size = sizeof(report);
  result = nxloader_softfp_add_libm(registry, "armv7-cross-softfp", 100,
                                    &report);
  CHECK(result == NXLOADER_OK);
  CHECK(report.added >= 60);

  address = lookup(registry, "sinf");
  sinf_address = address;
  CHECK(sizeof(guest_sinf) == sizeof(address));
  if (address != 0 && sizeof(guest_sinf) == sizeof(address))
    memcpy(&guest_sinf, &address, sizeof(guest_sinf));

  address = lookup(registry, "pow");
  CHECK(sizeof(guest_pow) == sizeof(address));
  if (address != 0 && sizeof(guest_pow) == sizeof(address))
    memcpy(&guest_pow, &address, sizeof(guest_pow));

  CHECK(guest_sinf != NULL);
  CHECK(guest_pow != NULL);
  if (guest_sinf) {
    float_result = guest_sinf(float_input);
    CHECK(nearly_equal_float(float_result, 0.47942555f, 0.00001f));
  }
  if (guest_pow) {
    double_result = guest_pow(double_base, double_exponent);
    CHECK(nearly_equal_double(double_result, 32.0, 0.000000001));
  }

  if (sinf_address != 0)
    nx_vfp_d8_preserved(sinf_address, &vfp_d8_result);
  CHECK(sinf_address != 0 && vfp_d8_result == 1);

  stack_result = stack_probe(1.0f, 2.0, 3.0f, 4.0, 5.0f);
  if (stack_result != UINT32_C(0x51f7a11e))
    fprintf(stderr, "armv7-cross: stack probe result=0x%08x\n",
            stack_result);
  CHECK(stack_result == UINT32_C(0x51f7a11e));
  entry_sp_probe(&entry_sp_alignment, 1.0f, 2.0, 3.0f, 4.0, 5.0f);
  CHECK(entry_sp_alignment == 0);
  CHECK((read_stack_pointer() & (uintptr_t)7u) == 0);

  nxloader_registry_destroy(registry);
  if (failures != 0) {
    fprintf(stderr, "armv7-cross: FAIL failures=%d\n", failures);
    return 1;
  }
  puts("armv7-cross: PASS softfp_float=1 softfp_double=1 stack_align_8=1 "
       "vfp_d8_preserved=1 guest_elf_loaded=0 guest_initializers_executed=0");
  return 0;
}
