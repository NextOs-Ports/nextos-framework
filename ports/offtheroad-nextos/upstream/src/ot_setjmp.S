/*
 * setjmp_bridge.S -- self-contained setjmp/longjmp for the Bionic guest.
 *
 * The guest reserves a Bionic jmp_buf (256 bytes on arm64).  glibc's
 * __sigsetjmp writes a 312-byte __jmp_buf_tag whenever the signal mask is
 * saved, so forwarding would smash 56 bytes of the guest's stack frame.  We
 * therefore implement the save/restore ourselves: callee-saved GPRs, fp/lr,
 * sp and d8-d15 fit in 168 bytes, well inside the guest's buffer.  The signal
 * mask is not saved -- the game only uses this for libpng/libjpeg error
 * unwinding, which never runs with a modified mask.
 *
 * Layout (byte offsets in the guest buffer):
 *   0   x19 x20      64  x27 x28     104 d8  d9     136 d12 d13
 *   16  x21 x22      80  x29 x30     120 d10 d11    152 d14 d15
 *   32  x23 x24      96  sp
 *   48  x25 x26
 */
	.text
	.balign 4

	.global ot_setjmp
	.type ot_setjmp, %function
ot_setjmp:
	stp	x19, x20, [x0, #0]
	stp	x21, x22, [x0, #16]
	stp	x23, x24, [x0, #32]
	stp	x25, x26, [x0, #48]
	stp	x27, x28, [x0, #64]
	stp	x29, x30, [x0, #80]
	mov	x1, sp
	str	x1, [x0, #96]
	stp	d8,  d9,  [x0, #104]
	stp	d10, d11, [x0, #120]
	stp	d12, d13, [x0, #136]
	stp	d14, d15, [x0, #152]
	mov	w0, #0
	ret
	.size ot_setjmp, .-ot_setjmp

	.global ot_longjmp
	.type ot_longjmp, %function
ot_longjmp:
	ldp	x19, x20, [x0, #0]
	ldp	x21, x22, [x0, #16]
	ldp	x23, x24, [x0, #32]
	ldp	x25, x26, [x0, #48]
	ldp	x27, x28, [x0, #64]
	ldp	x29, x30, [x0, #80]
	ldr	x2, [x0, #96]
	mov	sp, x2
	ldp	d8,  d9,  [x0, #104]
	ldp	d10, d11, [x0, #120]
	ldp	d12, d13, [x0, #136]
	ldp	d14, d15, [x0, #152]
	cmp	w1, #0
	csinc	w0, w1, wzr, ne
	br	x30
	.size ot_longjmp, .-ot_longjmp

	.section .note.GNU-stack,"",%progbits
