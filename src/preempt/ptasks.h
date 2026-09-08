// ptasks.h: demo workloads for the preemptive scheduler. Each task spins
// forever on integer work and never yields; any interleaving of their
// progress counters is therefore caused by timer preemption alone.

#ifndef PTASKS_H
#define PTASKS_H

void ptask_a(void);
void ptask_b(void);
void ptask_c(void);

void ptasks_register(void);
int ptask_count(void);
unsigned long ptask_iters(int i);
const char *ptask_name(int i);

#endif  // PTASKS_H
