/* Old Android/Bionic ARM jmp_buf bridge.
 *
 * The guest allocates 64 32-bit words and expects the historical Bionic
 * layout.  A glibc jmp_buf has a different private layout, so forwarding
 * setjmp/longjmp through dlsym is unsafe.  We use the Bionic register layout
 * but intentionally implement the no-signal-mask variant: the game receives
 * the exact non-local-jump semantics without coupling to either libc's
 * sigset_t representation.
 */

    .syntax unified
    .fpu neon
    .text
    .align 2

    .global asm2_setjmp
    .type asm2_setjmp, %function
asm2_setjmp:
    ldr     r1, .Lmagic
    str     r1, [r0, #0]
    add     r1, r0, #(19 * 4)
    stmia   r1, {r4-r14}
    add     r1, r0, #(2 * 4)
    vstmia  r1, {d8-d15}
    vmrs    r1, fpscr
    str     r1, [r0, #(18 * 4)]
    mov     r0, #0
    bx      lr
    .size asm2_setjmp, .-asm2_setjmp

    .global asm2_longjmp
    .type asm2_longjmp, %function
asm2_longjmp:
    ldr     r2, .Lmagic
    ldr     r3, [r0, #0]
    cmp     r2, r3
    bne     .Linvalid
    add     r2, r0, #(2 * 4)
    vldmia  r2, {d8-d15}
    ldr     r2, [r0, #(18 * 4)]
    vmsr    fpscr, r2
    add     r2, r0, #(19 * 4)
    ldmia   r2, {r4-r14}
    cmp     sp, #0
    cmpne   lr, #0
    beq     .Linvalid
    movs    r0, r1
    moveq   r0, #1
    bx      lr
.Linvalid:
    bl      abort
    b       .Linvalid
    .size asm2_longjmp, .-asm2_longjmp

    .align 2
.Lmagic:
    .word   0x4278f500

    .section .note.GNU-stack,"",%progbits
