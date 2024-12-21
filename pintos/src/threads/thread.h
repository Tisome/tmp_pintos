#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/synch.h"
#include "threads/fixed-point.h"

/* States in a thread's life cycle. */
enum thread_status {
  THREAD_RUNNING, /* Running thread. */
  THREAD_READY,   /* Not running but ready to run. */
  THREAD_BLOCKED, /* Waiting for an event to trigger. */
  THREAD_DYING    /* About to be destroyed. */
};

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t)-1) /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0      /* Lowest priority. */
#define PRI_DEFAULT 31 /* Default priority. */
#define PRI_MAX 63     /* Highest priority. */

/* A kernel thread or user process.
   Each thread structure is stored in its own 4 kB page. The thread structure
   itself sits at the very bottom of the page (at offset 0). The rest of the
   page is reserved for the thread's kernel stack, which grows downward from
   the top of the page (at offset 4 kB). */
struct thread {
  /* Owned by thread.c. */
  tid_t tid;                 /* Thread identifier. */
  enum thread_status status; /* Thread state. */
  char name[16];             /* Name (for debugging purposes). */
  uint8_t* stack;            /* Saved stack pointer. */
  int priority;              /* Priority. */
  struct list_elem allelem;  /* List element for all threads list. */

  /* Shared between thread.c and synch.c. */
  struct list_elem elem; /* List element. */

  /* Additional fields for priority donation and MLFQS scheduling */
  int64_t blocked_ticks;
  int base_priority;
  struct list locks;
  struct lock* locks_wait;
  int nice;
  fixed_point_t recent_cpu;

#ifdef USERPROG
  /* Owned by process.c. */
  struct process* pcb; /* Process control block if this thread is a userprog */
  void* upage;
  struct list_elem p_elem;
#endif

  /* Owned by thread.c. */
  unsigned magic; /* Detects stack overflow. */
};

/* Types of schedulers that the user can request the kernel
 * use to schedule threads at runtime. */
enum sched_policy {
  SCHED_FIFO,  // First-in, first-out scheduler
  SCHED_PRIO,  // Strict-priority scheduler with round-robin tiebreaking
  SCHED_FAIR,  // Implementation-defined fair scheduler
  SCHED_MLFQS, // Multi-level Feedback Queue Scheduler
};
#define SCHED_DEFAULT SCHED_FIFO

/* Determines which scheduling policy the kernel should use.
 * Controlled by the kernel command-line options:
 *  "-sched-default", "-sched-fair", "-sched-mlfqs", "-sched-fifo".
 * Defaults to SCHED_FIFO. */
extern enum sched_policy active_sched_policy;

/* Initializes the thread subsystem. */
void thread_init(void);

/* Starts the thread scheduler. */
void thread_start(void);

/* Called by the timer interrupt handler at each timer tick. */
void thread_tick(void);

/* Prints thread statistics. */
void thread_print_stats(void);

/* Creates a new thread. */
typedef void thread_func(void* aux);
tid_t thread_create(const char* name, int priority, thread_func*, void*);

/* Blocks the current thread. */
void thread_block(void);

/* Unblocks the specified thread. */
void thread_unblock(struct thread*);

/* Checks if a thread is blocked. */
void check_blocked(struct thread* t, void* aux UNUSED);

/* Functions for priority donation and MLFQS scheduling */
void thread_hold_lock(struct lock* lock);
void thread_donate_priority(struct thread* t);
void thread_update_priority(struct thread* t);
bool thread_cmp_priority(const struct list_elem* a, const struct list_elem* b, void* aux UNUSED);
bool lock_cmp_priority(const struct list_elem* a, const struct list_elem* b, void* aux UNUSED);
void thread_fair_increase_recent_cpu(void);
void thread_fair_update_load_avg_and_recent_cpu(void);
void thread_fair_update_priority(struct thread* t);
void kill_thread(struct thread* t);

/* Returns the currently running thread. */
struct thread* thread_current(void);

/* Returns the thread's identifier. */
tid_t thread_tid(void);

/* Returns the thread's name. */
const char* thread_name(void);

/* Exits the current thread. */
void thread_exit(void) NO_RETURN;

/* Yields the CPU to another thread. */
void thread_yield(void);

/* Applies a given function to all threads. */
typedef void thread_action_func(struct thread* t, void* aux);
void thread_foreach(thread_action_func*, void*);

/* Gets the priority of the current thread. */
int thread_get_priority(void);

/* Sets the priority of the current thread. */
void thread_set_priority(int);

/* Gets the "nice" value of the current thread. */
int thread_get_nice(void);

/* Sets the "nice" value of the current thread. */
void thread_set_nice(int);

/* Gets the recent CPU usage of the current thread. */
int thread_get_recent_cpu(void);

/* Gets the system's average load. */
int thread_get_load_avg(void);

#endif /* THREADS_THREAD_H */
