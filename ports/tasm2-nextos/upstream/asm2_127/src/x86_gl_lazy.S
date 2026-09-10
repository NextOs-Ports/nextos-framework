    .text
    .p2align 4
    .globl asm2_x86_gl_lazy_common
    .type asm2_x86_gl_lazy_common, @function
asm2_x86_gl_lazy_common:
    /*
     * Entry stack: [slot-index, original-return-address, original-args...].
     * pushal protects the incoming register state while the C resolver talks
     * to Box32/SDL.  Replace the saved EAX with its result, remove the private
     * slot word, and tail-jump with the caller's original stack untouched.
     */
    pushal
    /*
     * Android x86 follows the SysV i386 16-byte call-site alignment.  The
     * private slot plus pushal leave ESP at 8 mod 16 here.  Reserve one word
     * before pushing the resolver argument so the C callee enters at 12 mod
     * 16, exactly as GCC-generated i386 code expects.
     */
    subl $4, %esp
    pushl 36(%esp)
    call asm2_x86_gl_resolve_index
    addl $8, %esp
    movl %eax, 28(%esp)
    popal
    addl $4, %esp
    jmp *%eax
    .size asm2_x86_gl_lazy_common, .-asm2_x86_gl_lazy_common

    .section .note.GNU-stack,"",@progbits
