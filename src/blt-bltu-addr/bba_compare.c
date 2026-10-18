// bba_compare.c: which branch instruction the compiler picks for a
// signed vs unsigned range check. Compiled to assembly only, never run.

int below_signed(long a, long b) {
    if (a < b)
        return 1;
    return 0;
}

int below_unsigned(unsigned long a, unsigned long b) {
    if (a < b)
        return 1;
    return 0;
}
