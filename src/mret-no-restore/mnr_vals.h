// mnr_vals.h: the two sentinel sets for the mret-no-restore module.
//
// Sixteen distinct nonzero 64-bit values, no overlap between the
// pre-trap set (loaded by the pre-trap code into a0..a7) and the
// handler set (written by the trap handler into a0..a7). One header
// included by both the asm trap entry and the C main, so the values
// the handler writes and the values the checks compare against
// cannot drift apart. No UL suffixes: the same tokens must assemble
// as immediates in the .S file.
#ifndef MNR_VALS_H
#define MNR_VALS_H

#define PRE_A0 0x1111111111111111
#define PRE_A1 0x2222222222222222
#define PRE_A2 0x3333333333333333
#define PRE_A3 0x4444444444444444
#define PRE_A4 0x5555555555555555
#define PRE_A5 0x6666666666666666
#define PRE_A6 0x7777777777777777
#define PRE_A7 0x8888888888888888

#define POST_A0 0x9999999999999999
#define POST_A1 0xaaaaaaaaaaaaaaaa
#define POST_A2 0xbbbbbbbbbbbbbbbb
#define POST_A3 0xcccccccccccccccc
#define POST_A4 0xdddddddddddddddd
#define POST_A5 0xeeeeeeeeeeeeeeee
#define POST_A6 0xffffffffffffffff
#define POST_A7 0x123456789abcdef0

#endif  // MNR_VALS_H
