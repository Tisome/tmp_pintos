#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list fifo_ready_list;

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* Idle thread. */
static struct thread* idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread* initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame {
  void* eip;             /* Return address. */
  thread_func* function; /* Function to call. */
  void* aux;             /* Auxiliary data for function. */
};

/* Statistics. */
static long long idle_ticks;   /* # of timer ticks spent idle. */
static long long kernel_ticks; /* # of timer ticks in kernel threads. */
static long long user_ticks;   /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4          /* # of timer ticks to give each thread. */
static unsigned thread_ticks; /* # of timer ticks since last yield. */

static void init_thread(struct thread*, const char* name, int priority);
static bool is_thread(struct thread*) UNUSED;
static void* alloc_frame(struct thread*, size_t size);
static void schedule(void);
static void thread_enqueue(struct thread* t);
static tid_t allocate_tid(void);
void thread_switch_tail(struct thread* prev);

static void kernel_thread(thread_func*, void* aux);
static void idle(void* aux UNUSED);
static struct thread* running_thread(void);

static struct thread* next_thread_to_run(void);
static struct thread* thread_schedule_fifo(void);
static struct thread* thread_schedule_prio(void);
static struct thread* thread_schedule_fair(void);
static struct thread* thread_schedule_mlfqs(void);
static struct thread* thread_schedule_reserved(void);

bool schedule_started;
fixed_point_t load_avg;
extern bool is_mls;

/* Determines which scheduler the kernel should use.
   Controlled by the kernel command-line options
    "-sched=fifo", "-sched=prio",
    "-sched=fair". "-sched=mlfqs"
   Is equal to SCHED_FIFO by default. */
enum sched_policy active_sched_policy;

/* Selects a thread to run from the ready list according to
   some scheduling policy, and returns a pointer to it. */
typedef struct thread* scheduler_func(void);

/* Jump table for dynamically dispatching the current scheduling
   policy in use by the kernel. */
scheduler_func* scheduler_jump_table[8] = {thread_schedule_fifo,     thread_schedule_prio,
                                           thread_schedule_fair,     thread_schedule_mlfqs,
                                           thread_schedule_reserved, thread_schedule_reserved,
                                           thread_schedule_reserved, thread_schedule_reserved};

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void thread_init(void) {
  ASSERT(intr_get_level() == INTR_OFF);

  lock_init(&tid_lock);
  list_init(&fifo_ready_list);
  list_init(&all_list);

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread();
  init_thread(initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void thread_start(void) {
  /* Create the idle thread. */
  struct semaphore idle_started;
  schedule_started=true;
  load_avg = fix_int (0);
  sema_init(&idle_started, 0);
  thread_create("idle", PRI_MIN, idle, &idle_started);

  /* Start preemptive thread scheduling. */
  intr_enable();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down(&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void thread_tick(void) {
  struct thread* t = thread_current();

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pcb != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return();
}

/* Prints thread statistics. */
void thread_print_stats(void) {
  printf("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n", idle_ticks, kernel_ticks,
         user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t thread_create(const char* name, int priority, thread_func* function, void* aux) {
  struct thread* t;
  struct kernel_thread_frame* kf;
  struct switch_entry_frame* ef;
  struct switch_threads_frame* sf;
  tid_t tid;

  ASSERT(function != NULL);

  /* Allocate thread. */
  t = palloc_get_page(PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread(t, name, priority);
  tid = t->tid = allocate_tid();
  t->blocked_ticks = 0;

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame(t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame(t, sizeof *ef);
  ef->eip = (void (*)(void))kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame(t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  /* Add to run queue. */
  thread_unblock(t);

  if (thread_current()->priority < priority)
    thread_yield();

  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void thread_block(void) {
  ASSERT(!intr_context());
  ASSERT(intr_get_level() == INTR_OFF);

  thread_current()->status = THREAD_BLOCKED;
  schedule();
}

/* Determines whether a thread is blocked and, if so, decrements
   its blocked_ticks value until it reaches 0. */
void check_blocked(struct thread* t, void* aux UNUSED) {
  if (t->status == THREAD_BLOCKED && t->blocked_ticks > 0) {
    t->blocked_ticks--;
    if (t->blocked_ticks == 0)
      thread_unblock(t);
  }
}

/* Places a thread on the ready structure appropriate for the
   current active scheduling policy.
   
   This function must be called with interrupts turned off. */
static void thread_enqueue(struct thread* t) {
  ASSERT(intr_get_level() == INTR_OFF);
  ASSERT(is_thread(t));

  if (active_sched_policy == SCHED_FIFO) 
    list_push_back(&fifo_ready_list, &t->elem);
  else if(active_sched_policy == SCHED_PRIO)
    list_insert_ordered (&fifo_ready_list, &t->elem, (list_less_func *) &thread_cmp_priority, NULL);
  else if(active_sched_policy == SCHED_FAIR)
    list_insert_ordered (&fifo_ready_list, &t->elem, (list_less_func *) &thread_cmp_priority, NULL);
  else
    PANIC("Unimplemented scheduling policy value: %d", active_sched_policy);
}


/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void thread_unblock(struct thread* t) {
  enum intr_level old_level;

  ASSERT(is_thread(t));

  old_level = intr_disable();
  ASSERT(t->status == THREAD_BLOCKED);
  list_insert_ordered (&fifo_ready_list, &t->elem, (list_less_func *) &thread_cmp_priority, NULL);
  //thread_enqueue(t);
  t->status = THREAD_READY;
  intr_set_level(old_level);
}

/* Thread holds the lock */
void thread_hold_lock(struct lock *lock) {
  enum intr_level old_level = intr_disable ();
  list_insert_ordered (&thread_current ()->locks, &lock->elem, lock_cmp_priority, NULL);
  if (lock->max_priority > thread_current ()->priority) {
    thread_current ()->priority = lock->max_priority;
    thread_yield ();
  }
  intr_set_level (old_level);
}

/* Donate priority to a thread */
void thread_donate_priority (struct thread *t) {
  enum intr_level old_level = intr_disable ();
  thread_update_priority (t);
  if (t->status == THREAD_READY) {
    list_remove (&t->elem);
    list_insert_ordered (&fifo_ready_list, &t->elem, thread_cmp_priority, NULL);
  }
  intr_set_level (old_level);
}

/* Update priority of a thread */
void thread_update_priority (struct thread *t) {
  enum intr_level old_level = intr_disable ();
  int max_priority = t->base_priority;
  int lock_priority;
  if (!list_empty (&t->locks)) {
    list_sort (&t->locks, lock_cmp_priority, NULL);
    lock_priority = list_entry (list_front (&t->locks), struct lock, elem)->max_priority;
    if (lock_priority > max_priority)
      max_priority = lock_priority;
  }
  t->priority = max_priority;
  intr_set_level (old_level);
}

/* Comparison function for thread priorities */
bool thread_cmp_priority (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
  return list_entry(a, struct thread, elem)->priority > list_entry(b, struct thread, elem)->priority;
}

/* Comparison function for lock priorities */
bool lock_cmp_priority (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
  return list_entry (a, struct lock, elem)->max_priority > list_entry (b, struct lock, elem)->max_priority;
}

/* Increase recent CPU usage of a thread */
void thread_fair_increase_recent_cpu (void) {
  ASSERT (active_sched_policy == SCHED_FAIR);
  ASSERT (intr_context ());

  struct thread *cur_thread = thread_current ();
  if (cur_thread == idle_thread)
    return;
  cur_thread->recent_cpu = fix_add_mix (cur_thread->recent_cpu, 1);
}

/* Update load average and recent CPU usage of threads */
void thread_fair_update_load_avg_and_recent_cpu (void) {
  ASSERT (active_sched_policy == SCHED_FAIR);
  ASSERT (intr_context ());
  // Number of ready threads
  size_t ready_num = list_size (&fifo_ready_list);
  if (thread_current () != idle_thread)
    ++ready_num;
  // Calculate system load average
  load_avg = fix_add (fix_unscale (fix_scale (load_avg, 59), 60), fix_unscale (fix_int (ready_num), 60));

  struct thread *t;
  struct list_elem *e = list_begin (&all_list);
  // Iterate through all threads
  for (; e != list_end (&all_list); e = list_next (e)) {
    t = list_entry(e, struct thread, allelem);
    if (t != idle_thread) {
      t->recent_cpu = fix_mul (fix_div (fix_scale (load_avg, 2), fix_add_mix (fix_scale (load_avg, 2), 1)), t->recent_cpu);
      thread_fair_update_priority (t);
    }
  }
}

/* Update priority of a thread based on recent CPU usage */
void thread_fair_update_priority (struct thread *t) {
  if (t == idle_thread)
    return;
  ASSERT (active_sched_policy == SCHED_FAIR);

  if(is_mls)
    t->priority = fix_trunc (fix_sub_mix (fix_sub (fix_int (PRI_MAX), fix_unscale (t->recent_cpu, 4)), t->nice));
  else t->priority = fix_trunc (fix_sub (fix_int (PRI_MAX), fix_unscale (t->recent_cpu, 4)));
  // Ensure priority is within valid range
  if (t->priority < PRI_MIN)
    t->priority = PRI_MIN;
  else if (t->priority > PRI_MAX)
    t->priority = PRI_MAX;
}


/* Returns the name of the running thread. */
const char* thread_name(void) { return thread_current()->name; }

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread* thread_current(void) {
  struct thread* t = running_thread();

  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT(is_thread(t));
  ASSERT(t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t thread_tid(void) { return thread_current()->tid; }

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void thread_exit(void) {
  ASSERT(!intr_context());

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_switch_tail(). */
  intr_disable();
  list_remove(&thread_current()->allelem);
  thread_current()->status = THREAD_DYING;
  schedule();
  NOT_REACHED();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void thread_yield(void) {
  if(!schedule_started)
    return;
  struct thread* cur = thread_current();
  enum intr_level old_level;

  ASSERT(!intr_context());

  old_level = intr_disable();
  if (cur != idle_thread)
    list_insert_ordered (&fifo_ready_list, &cur->elem, (list_less_func *) &thread_cmp_priority, NULL);
    //thread_enqueue(cur);
  cur->status = THREAD_READY;
  schedule();
  intr_set_level(old_level);
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void thread_foreach(thread_action_func* func, void* aux) {
  struct list_elem* e;

  ASSERT(intr_get_level() == INTR_OFF);

  for (e = list_begin(&all_list); e != list_end(&all_list); e = list_next(e)) {
    struct thread* t = list_entry(e, struct thread, allelem);
    func(t, aux);
  }
}

/* 设置当前线程的优先级 */
void thread_set_priority(int new_priority) {
  if (active_sched_policy == SCHED_FAIR)
    return;
  enum intr_level old_level = intr_disable ();
  struct thread *cur_thread = thread_current ();
  int old_priority = cur_thread->priority;
  cur_thread->base_priority = new_priority;
  if (list_empty (&cur_thread->locks) || new_priority > old_priority) {
    cur_thread->priority = new_priority;
    thread_yield ();
  }
  intr_set_level (old_level);
}

/* userprog中杀死指定线程 */
void kill_thread(struct thread* t) {
  ASSERT(!intr_context());
  ASSERT(t != NULL);
  ASSERT(t != initial_thread);

  enum intr_level old_level = intr_disable();
  list_remove(&t->allelem);
  list_remove(&t->elem);
  t->status = THREAD_DYING;
  palloc_free_page(t);
  intr_set_level(old_level);
}

/* Returns the current thread's priority. */
int thread_get_priority(void) { return thread_current()->priority; }

/* Sets the current thread's nice value to NICE. */
void thread_set_nice(int nice UNUSED) { /* Not yet implemented. */
  thread_current ()->nice = nice;
  thread_fair_update_priority (thread_current ());
  thread_yield ();
}

/* Returns the current thread's nice value. */
int thread_get_nice(void) {
  /* Not yet implemented. */
  return thread_current ()->nice;
}

/* Returns 100 times the system load average. */
int thread_get_load_avg(void) {
  /* Not yet implemented. */
  return fix_round (fix_scale (load_avg, 100));
}

/* Returns 100 times the current thread's recent_cpu value. */
int thread_get_recent_cpu(void) {
  /* Not yet implemented. */
  return fix_round (fix_scale (thread_current ()->recent_cpu, 100));
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void idle(void* idle_started_ UNUSED) {
  struct semaphore* idle_started = idle_started_;
  idle_thread = thread_current();
  sema_up(idle_started);

  for (;;) {
    /* Let someone else run. */
    intr_disable();
    thread_block();

    /* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
    asm volatile("sti; hlt" : : : "memory");
  }
}

/* Function used as the basis for a kernel thread. */
static void kernel_thread(thread_func* function, void* aux) {
  ASSERT(function != NULL);

  intr_enable(); /* The scheduler runs with interrupts off. */
  function(aux); /* Execute the thread function. */
  thread_exit(); /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread* running_thread(void) {
  uint32_t* esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the current thread. */
  asm("mov %%esp, %0" : "=g"(esp));
  return pg_round_down(esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool is_thread(struct thread* t) { return t != NULL && t->magic == THREAD_MAGIC; }

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void init_thread(struct thread* t, const char* name, int priority) {
  enum intr_level old_level;

  ASSERT(t != NULL);
  ASSERT(PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT(name != NULL);

  /* Initialize the thread's fields */
  memset(t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy(t->name, name, sizeof t->name);
  t->stack = (uint8_t*)t + PGSIZE;
  t->priority = priority;
#ifdef USERPROG
  t->pcb = NULL;
#endif
  t->magic = THREAD_MAGIC;

  /* Additional fields for priority donation and MLFQS scheduling */
  t->base_priority = priority;
  list_init (&t->locks);
  t->locks_wait = NULL;
  if(is_mls)
    t->nice = 0;
  else if(priority==56)
    t->nice=0;
  else if(priority==48)
    t->nice=1;
  else if(priority==40)
    t->nice=2;
  else if(priority==32)
    t->nice=3;
  else if(priority==24)
    t->nice=4;
  else if(priority==16)
    t->nice=5;
  else if(priority==8)
    t->nice=6;
  else if(priority==0)
    t->nice=7;
  t->recent_cpu = fix_int (0);

  /* Insert the thread into the global list of all threads */
  old_level = intr_disable();
  list_insert_ordered (&all_list, &t->allelem, (list_less_func*) &thread_cmp_priority, NULL);
  intr_set_level(old_level);
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void* alloc_frame(struct thread* t, size_t size) {
  /* Stack data is always allocated in word-size units. */
  ASSERT(is_thread(t));
  ASSERT(size % sizeof(uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* First-in first-out scheduler */
static struct thread* thread_schedule_fifo(void) {
  if (!list_empty(&fifo_ready_list))
    return list_entry(list_pop_front(&fifo_ready_list), struct thread, elem);
  else
    return idle_thread;
}

/* Strict priority scheduler */
static struct thread* thread_schedule_prio(void) {
  if (!list_empty(&fifo_ready_list))
    return list_entry(list_pop_front(&fifo_ready_list), struct thread, elem);
  else
    return idle_thread;
}

/* Fair priority scheduler */
static struct thread* thread_schedule_fair(void) {
  if (!list_empty(&fifo_ready_list))
    return list_entry(list_pop_front(&fifo_ready_list), struct thread, elem);
  else
    return idle_thread;
}

/* Multi-level feedback queue scheduler */
static struct thread* thread_schedule_mlfqs(void) {
  PANIC("Unimplemented scheduler policy: \"-sched=mlfqs\"");
}

/* Not an actual scheduling policy — placeholder for empty
 * slots in the scheduler jump table. */
static struct thread* thread_schedule_reserved(void) {
  PANIC("Invalid scheduler policy value: %d", active_sched_policy);
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread* next_thread_to_run(void) {
  if (!list_empty(&fifo_ready_list))
    return list_entry(list_pop_front(&fifo_ready_list), struct thread, elem);
  else
    return idle_thread;
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_switch() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void thread_switch_tail(struct thread* prev) {
  struct thread* cur = running_thread();

  ASSERT(intr_get_level() == INTR_OFF);

  /* Mark the current thread as running */
  cur->status = THREAD_RUNNING;

  /* Start new time slice */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space */
  process_activate();
#endif

  /* If the thread we switched from is dying, free its memory */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) {
    ASSERT(prev != cur);
    palloc_free_page(prev);
  }
}

/* Schedules a new thread.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_switch_tail()
   has completed. */
static void schedule(void) {
  struct thread* cur = running_thread();
  struct thread* next = next_thread_to_run();
  struct thread* prev = NULL;

  ASSERT(intr_get_level() == INTR_OFF);
  ASSERT(cur->status != THREAD_RUNNING);
  ASSERT(is_thread(next));

  if (cur != next)
    prev = switch_threads(cur, next);
  thread_switch_tail(prev);
}

/* Returns a tid to use for a new thread. */
static tid_t allocate_tid(void) {
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire(&tid_lock);
  tid = next_tid++;
  lock_release(&tid_lock);

  return tid;
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof(struct thread, stack);