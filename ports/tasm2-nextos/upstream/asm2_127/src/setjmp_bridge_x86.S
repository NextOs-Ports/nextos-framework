/*
 * Android 4.x x86 setjmp buffer layout (first six 32-bit words):
 *   0 return PC, 1 EBX, 2 ESP, 3 EBP, 4 ESI, 5 EDI.
 *
 * TASM2 1.2.7d predates Bionic's later cookie/checksum layout.  Never pass
 * this private guest buffer to glibc's setjmp/longjmp implementation.
 */
    .text
    .p2align 4
    .globl asm2_setjmp
    .type asm2_setjmp, @function
asm2_setjmp:
    movl 4(%esp), %ecx
    movl 0(%esp), %edx
    movl %edx, 0(%ecx)
    movl %ebx, 4(%ecx)
    movl %esp, 8(%ecx)
    movl %ebp, 12(%ecx)
    movl %esi, 16(%ecx)
    movl %edi, 20(%ecx)
    xorl %eax, %eax
    ret
    .size asm2_setjmp, .-asm2_setjmp

    .p2align 4
    .globl asm2_longjmp
    .type asm2_longjmp, @function
asm2_longjmp:
    movl 4(%esp), %ecx
    movl 8(%esp), %eax
    testl %eax, %eax
    jne 1f
    incl %eax
1:
    movl 4(%ecx), %ebx
    movl 12(%ecx), %ebp
    movl 16(%ecx), %esi
    movl 20(%ecx), %edi
    movl 8(%ecx), %esp
    ret
    .size asm2_longjmp, .-asm2_longjmp

    .section .note.GNU-stack,"",@progbits
