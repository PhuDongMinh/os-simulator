#ifndef SCHED_H
#define SCHED_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MLQ_SCHED
#define MLQ_SCHED
#endif

#define MAX_PRIO 140

int queue_empty(void);

void init_scheduler(void); // Initialize the scheduler
void finish_scheduler(void); // Finish, Clean the scheduler

/* Get the next process from ready queue */
struct pcb_t * get_proc(void);

/* Put a process back to run queue */
void put_proc(struct pcb_t * proc); // End time slice

/* Add a new process to ready queue */
void add_proc(struct pcb_t * proc);

#ifdef __cplusplus
}
#endif

#endif
