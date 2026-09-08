// tasks.c: three demo workloads that share the CPU round-robin style.
// Each task does a little real work, prints a status line, then yields.
// The interleaved output proves the scheduler is actually switching.

#include "uart.h"
#include "sched.h"
#include "tasks.h"

#define ITERATIONS 5

// Task 1: periodic heartbeat, the classic "am I alive" check.
static void task_heartbeat(void) {
    for (unsigned long i = 1; i <= ITERATIONS; i++) {
        uart_puts("[heartbeat] tick ");
        uart_put_dec(i);
        uart_puts(" (task 1 of 3)\n");
        sched_yield();
    }
}

// Task 2: computes Fibonacci numbers, showing tasks keep private state
// (locals a and b) across yields because each task has its own stack.
static void task_fibonacci(void) {
    unsigned long a = 0, b = 1;

    for (unsigned long i = 1; i <= ITERATIONS; i++) {
        uart_puts("[fibonacci] fib(");
        uart_put_dec(i);
        uart_puts(") = ");
        uart_put_dec(b);
        uart_puts(" (task 2 of 3)\n");
        unsigned long next = a + b;
        a = b;
        b = next;
        sched_yield();
    }
}

// Task 3: spinner with a busy loop, simulating a task doing real work
// while the others still get their turns.
static void task_spinner(void) {
    const char *frames = "|/-\\";

    for (unsigned long i = 0; i < ITERATIONS; i++) {
        uart_puts("[spinner] working ");
        uart_putc(frames[i % 4]);
        uart_puts(" (task 3 of 3)\n");
        for (volatile unsigned long d = 0; d < 200000; d++)
            ;  // burn cycles: pretend this is real work
        sched_yield();
    }
}

void tasks_register(void) {
    task_create(task_heartbeat, "heartbeat");
    task_create(task_fibonacci, "fibonacci");
    task_create(task_spinner, "spinner");
}
