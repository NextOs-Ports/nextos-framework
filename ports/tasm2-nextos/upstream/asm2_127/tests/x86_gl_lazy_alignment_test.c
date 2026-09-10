#include <stdint.h>
#include <stdio.h>

extern uint32_t asm2_x86_gl_alignment_stub(uint32_t a, uint32_t b,
                                           uint32_t c, uint32_t d);
extern void asm2_x86_gl_alignment_float_stub(int location, float value);

volatile uint32_t asm2_x86_test_resolver_stack_mod16 = UINT32_MAX;
volatile uint32_t asm2_x86_test_resolver_index = 0;
void *volatile asm2_x86_test_target = NULL;
volatile int asm2_x86_test_float_location = 0;
volatile float asm2_x86_test_float_value = 0.0f;

uint32_t asm2_x86_gl_alignment_target(uint32_t a, uint32_t b, uint32_t c,
                                      uint32_t d) {
  return (a * 3u) ^ (b * 5u) ^ (c * 7u) ^ (d * 11u);
}

void asm2_x86_gl_alignment_float_target(int location, float value) {
  asm2_x86_test_float_location = location;
  asm2_x86_test_float_value = value;
}

/*
 * Keep the resolver entry naked so the measurement observes the stack at the
 * exact ASM-to-C ABI boundary, before a compiler-generated prologue.
 */
__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl asm2_x86_gl_resolve_index\n"
    ".type asm2_x86_gl_resolve_index, @function\n"
    "asm2_x86_gl_resolve_index:\n"
    "movl %esp, %edx\n"
    "andl $15, %edx\n"
    "movl %edx, asm2_x86_test_resolver_stack_mod16\n"
    "movl 4(%esp), %edx\n"
    "movl %edx, asm2_x86_test_resolver_index\n"
    "movl asm2_x86_test_target, %eax\n"
    "ret\n"
    ".size asm2_x86_gl_resolve_index, "
    ".-asm2_x86_gl_resolve_index\n"
    ".p2align 4\n"
    ".globl asm2_x86_gl_alignment_stub\n"
    ".type asm2_x86_gl_alignment_stub, @function\n"
    "asm2_x86_gl_alignment_stub:\n"
    "pushl $0x12345678\n"
    "jmp asm2_x86_gl_lazy_common\n"
    ".size asm2_x86_gl_alignment_stub, .-asm2_x86_gl_alignment_stub\n"
    ".p2align 4\n"
    ".globl asm2_x86_gl_alignment_float_stub\n"
    ".type asm2_x86_gl_alignment_float_stub, @function\n"
    "asm2_x86_gl_alignment_float_stub:\n"
    "pushl $0x87654321\n"
    "jmp asm2_x86_gl_lazy_common\n"
    ".size asm2_x86_gl_alignment_float_stub, "
    ".-asm2_x86_gl_alignment_float_stub\n");

int main(void) {
  const uint32_t a = 0x12345678u;
  const uint32_t b = 0x01020304u;
  const uint32_t c = 0x11223344u;
  const uint32_t d = 0xa5a5a5a5u;
  asm2_x86_test_target = asm2_x86_gl_alignment_target;
  uint32_t observed = asm2_x86_gl_alignment_stub(a, b, c, d);
  uint32_t expected = asm2_x86_gl_alignment_target(a, b, c, d);

  if (asm2_x86_test_resolver_stack_mod16 != 12u) {
    fprintf(stderr,
            "x86 lazy GL resolver ABI misaligned: esp%%16=%u expected=12\n",
            asm2_x86_test_resolver_stack_mod16);
    return 1;
  }
  if (asm2_x86_test_resolver_index != 0x12345678u) {
    fprintf(stderr, "x86 lazy GL resolver index corrupt: %08x\n",
            asm2_x86_test_resolver_index);
    return 2;
  }
  if (observed != expected) {
    fprintf(stderr,
            "x86 lazy GL tail-call corrupt: observed=%08x expected=%08x\n",
            observed, expected);
    return 3;
  }

  asm2_x86_test_target = asm2_x86_gl_alignment_float_target;
  asm2_x86_gl_alignment_float_stub(37, -0.625f);
  if (asm2_x86_test_resolver_stack_mod16 != 12u ||
      asm2_x86_test_resolver_index != 0x87654321u ||
      asm2_x86_test_float_location != 37 ||
      asm2_x86_test_float_value != -0.625f) {
    fprintf(stderr,
            "x86 lazy GL float ABI corrupt: esp%%16=%u index=%08x "
            "location=%d value=%f\n",
            asm2_x86_test_resolver_stack_mod16,
            asm2_x86_test_resolver_index, asm2_x86_test_float_location,
            (double)asm2_x86_test_float_value);
    return 4;
  }

  printf("ASM2_X86_GL_ALIGNMENT_OK esp_mod16=%u index=%08x value=%08x "
         "float=%d/%f\n",
         asm2_x86_test_resolver_stack_mod16,
         asm2_x86_test_resolver_index, observed,
         asm2_x86_test_float_location, (double)asm2_x86_test_float_value);
  return 0;
}
