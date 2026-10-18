	.file	"bba_compare.c"
	.option nopic
	.attribute arch, "rv64i2p1_m2p0_a2p1_c2p0_zicsr2p0"
	.attribute unaligned_access, 0
	.attribute stack_align, 16
	.text
	.align	1
	.globl	below_signed
	.type	below_signed, @function
below_signed:
	slt	a0,a0,a1
	ret
	.size	below_signed, .-below_signed
	.align	1
	.globl	below_unsigned
	.type	below_unsigned, @function
below_unsigned:
	sltu	a0,a0,a1
	ret
	.size	below_unsigned, .-below_unsigned
	.ident	"GCC: (13.2.0-11ubuntu1+12) 13.2.0"
