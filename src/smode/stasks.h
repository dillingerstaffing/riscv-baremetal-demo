// stasks.h: task table for the smode module.

#ifndef STASKS_H
#define STASKS_H

// Register the module's tasks with the scheduler.
void stasks_register(void);
int stask_count(void);
unsigned long stask_iters(int i);
const char *stask_name(int i);

#endif  // STASKS_H
