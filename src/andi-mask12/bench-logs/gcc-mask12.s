	.file	"m.c"
	.option nopic
	.attribute arch, "rv64i2p1_m2p0_a2p1_c2p0"
	.attribute unaligned_access, 0
	.attribute stack_align, 16
	.text
	.align	1
	.globl	mask12
	.type	mask12, @function
mask12:
	slli	a0,a0,52
	srli	a0,a0,52
	ret
	.size	mask12, .-mask12
	.align	1
	.globl	mask11
	.type	mask11, @function
mask11:
	li	a5,4096
	addi	a5,a5,-2048
	and	a0,a0,a5
	ret
	.size	mask11, .-mask11
	.align	1
	.globl	test_andi
	.type	test_andi, @function
test_andi:
	andi	a0,a0,2047
	ret
	.size	test_andi, .-test_andi
	.ident	"GCC: (13.2.0-11ubuntu1+12) 13.2.0"
