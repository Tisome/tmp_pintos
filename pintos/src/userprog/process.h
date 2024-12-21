#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include <stdint.h>

#include "filesys/filesys.h"

// At most 8MB can be allocated to the stack
// These defines will be used in Project 2: Multithreading
#define MAX_STACK_PAGES (1 << 11)
#define MAX_THREADS 127

/* PIDs and TIDs are the same type. PID should be
   the TID of the main thread of the process */
typedef tid_t pid_t;

/* Thread functions (Project 2: Multithreading) */
typedef void (*pthread_fun)(void*);
typedef void (*stub_fun)(pthread_fun, void*);

struct start_pthread_args {
  struct process* pcb;
  pthread_fun tf;
  stub_fun sf;
  void* arg;
};

struct prog_sema_block {
  struct list_elem elem;
  struct semaphore sema;
  int id;
};

struct prog_lock_block {
  struct list_elem elem;
  struct lock lock;
  int id;
};

struct file_list_elem {
  struct list_elem elem;
  struct file* file;
  int fd;
};

struct thread_block {
  tid_t tid;
  tid_t pid;
  int exit_code;
  bool was_waited;
  struct list_elem elem;
  struct semaphore semapth;
  struct semaphore load_semapth;
  bool load_success;
};


/* The process control block for a given process. Since
   there can be multiple threads per process, we need a separate
   PCB from the TCB. All TCBs in a process will have a pointer
   to the PCB, and the PCB will have a pointer to the main thread
   of the process, which is `special`. */
struct process {
  /* Owned by process.c. */
  uint32_t* pagedir;          /* Page directory. */
  char process_name[16];      /* Name of the main thread */
  struct thread* main_thread; /* Pointer to main thread */

  struct list all_files_list;
  struct lock file_list_lock;
  int next_fd;
  struct list all_threads;
  struct semaphore semapth;
  struct list prog_lock_list;
  int next_sema_id;
  struct file* file;
  int next_lock_id;
  struct list prog_sema_list;

};

void userprog_init(void);

pid_t process_execute(const char* file_name);
int process_wait(pid_t);
void process_exit(void);
void process_activate(void);

bool is_main_thread(struct thread*, struct process*);
pid_t get_pid(struct process*);

tid_t pthread_execute(stub_fun, pthread_fun, void*);
tid_t pthread_join(tid_t);
void pthread_exit(void);
void pthread_exit_main(void);

void set_exit_code(struct thread* t, int code);
int file_to_fd(struct file* file);
struct file* fd_to_file(int fd);
int open_for_syscall(const char* file);
bool close_file(int fd);
bool syscall_sema_init(char* sema, int val);
bool syscall_sema_up(char* sema);
bool syscall_sema_down(char* sema);
bool syscall_lock_init(char* lock);
bool syscall_lock_acquire(char* lock);
bool syscall_lock_release(char* lock);

#endif /* userprog/process.h */
