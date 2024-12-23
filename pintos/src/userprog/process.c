#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

static thread_func start_process NO_RETURN;
static thread_func start_pthread NO_RETURN;
static bool load(const char* file_name, void (**eip)(void), void** esp);
bool setup_thread(void** esp);

static struct list thread_block_list;
static struct lock prog_lock;
static struct lock file_lock;

static void args_push_stack(const char* file_name, void** if_esp) {
  void* esp = *if_esp;
  int argc = 0;
  int args_num = 0;
  char* token = NULL;
  unsigned int size = strlen(file_name) + 1;
  char* rest = malloc(size);
  const int max_argv_size = 50;
  char** argv = malloc(sizeof(char*) * max_argv_size);
  strlcpy(rest, file_name, size);

  while ((token = strtok_r(rest, " ", &rest))) {
    int size = strlen(token) + 1;
    esp -= size;
    argv[argc] = esp;
    strlcpy(esp, token, size);
    args_num += size;
    argc++;
  }
  args_num = args_num + sizeof(char*) * (argc + 1) + sizeof(char**) + sizeof(int);
  int temp = args_num % 0x10;
  if (temp > 0) {
    int align_size = 0x10 - temp;
    esp -= align_size;
    memset(esp, 0, align_size);
  }

  esp -= 0x04;
  *(char**)esp = 0;

  for (int i = argc - 1; i >= 0; i--) {
    esp -= sizeof(char*);
    *(char**)esp = argv[i];
  }
  esp -= 0x04;
  *(char***)esp = (esp + 0x04);
  esp -= 0x04;
  *(int*)esp = argc;
  esp -= 0x04;
  memset(esp, 0, 4);

  free(argv);
  *if_esp = esp;
}

static struct thread_block* get_thread_block(tid_t tid) {
  lock_acquire(&prog_lock);
  for (struct list_elem* e = list_begin(&thread_block_list); e != list_end(&thread_block_list); e = list_next(e)) {
    struct thread_block* block = list_entry(e, struct thread_block, elem);
    if (block->tid == tid) {
      lock_release(&prog_lock);
      return block;
    }
  }
  lock_release(&prog_lock);
  return NULL;
}

static void remove_thread_block(tid_t pid) {
  lock_acquire(&prog_lock);
  struct list_elem* e = list_begin(&thread_block_list);
  while (e != list_end(&thread_block_list)) {
    struct thread_block* block = list_entry(e, struct thread_block, elem);
    if (block->pid == pid) {
      struct list_elem* temp = e;
      e = list_next(e);
      list_remove(temp);
      free(block);
    }
    else e = list_next(e);
  }
  lock_release(&prog_lock);
}

static struct prog_lock_block* get_prog_lock_block(int id) {
  struct process* pcb = thread_current()->pcb;
  struct list* prog_lock_list = &pcb->prog_lock_list;
  enum intr_level old_level = intr_disable();
  for (struct list_elem* e = list_begin(prog_lock_list); e != list_end(prog_lock_list);
       e = list_next(e)) {
    struct prog_lock_block* block = list_entry(e, struct prog_lock_block, elem);
    if (block->id == id) {
      intr_set_level(old_level);
      return block;
    }
  }
  intr_set_level(old_level);
  return NULL;
}

static struct prog_sema_block* get_prog_sema_block(int id) {
  struct list* prog_sema_list = &thread_current()->pcb->prog_sema_list;
  enum intr_level old_level = intr_disable();
  for (struct list_elem* e = list_begin(prog_sema_list); e != list_end(prog_sema_list);
       e = list_next(e)) {
    struct prog_sema_block* block = list_entry(e, struct prog_sema_block, elem);
    if (block->id == id) {
      intr_set_level(old_level);
      return block;
    }
  }
  intr_set_level(old_level);
  return NULL;
}

void set_exit_code(struct thread* t, int code) {
  struct thread_block* block = get_thread_block(t->tid);
  if (block != NULL)
    block->exit_code = code;
}

static char* get_argv(const char* file_name_) {
  int num = strlen(file_name_) + 1;
  char* file_name = malloc(num);
  strlcpy(file_name, file_name_, num);
  char* argv_name = strtok_r(file_name, " ", &file_name);
  return argv_name;
}

int file_to_fd(struct file* file) {
  struct process* pcb = thread_current()->pcb;
  if (pcb == NULL)
    return -1;
  struct file_list_elem* e = malloc(sizeof(struct file_list_elem));
  e->fd = pcb->next_fd++;
  e->file = file;
  lock_acquire(&pcb->file_list_lock);
  list_push_back(&pcb->all_files_list, &e->elem);
  lock_release(&pcb->file_list_lock);
  return e->fd;
}

struct file* fd_to_file(int fd) {
  struct process* pcb = thread_current()->pcb;
  if (pcb == NULL)
    return NULL;
  struct list_elem* e;
  struct file* file = NULL;
  lock_acquire(&pcb->file_list_lock);
  for (e = list_begin(&pcb->all_files_list); e != list_end(&pcb->all_files_list); e = list_next(e)) {
    struct file_list_elem* file_list_elem = list_entry(e, struct file_list_elem, elem);
    if (file_list_elem->fd == fd) {
      file = file_list_elem->file;
      break;
    }
  }
  lock_release(&pcb->file_list_lock);
  return file;
}

int open_for_syscall(const char* file) {
  lock_acquire(&file_lock);
  struct file* opened_file = filesys_open(file);
  lock_release(&file_lock);
  if (opened_file == NULL)
    return -1;
  return file_to_fd(opened_file);
}

bool close_file(int fd) {
  struct process* pcb = thread_current()->pcb;
  if (pcb == NULL)
    return false;
  struct list_elem* e;
  bool success = false;
  struct file* file = NULL;
  lock_acquire(&pcb->file_list_lock);
  for (e = list_begin(&pcb->all_files_list); e != list_end(&pcb->all_files_list); e = list_next(e)) {
    struct file_list_elem* file_list_elem = list_entry(e, struct file_list_elem, elem);
    if (file_list_elem->fd == fd) {
      file = file_list_elem->file;
      list_remove(e);
      success = true;
      break;
    }
  }
  lock_release(&pcb->file_list_lock);

  if (!success)
    return false;
  lock_acquire(&file_lock);
  file_close(file);
  lock_release(&file_lock);
  return success;
}

bool syscall_sema_init(char* sema, int val) {
  if (sema == NULL || val < 0)
    return false;
  struct process* pcb = thread_current()->pcb;
  struct prog_sema_block* block = malloc(sizeof(struct prog_sema_block));
  enum intr_level old_level = intr_disable();
  list_push_back(&pcb->prog_sema_list, &block->elem);
  *sema = pcb->next_sema_id++;
  intr_set_level(old_level);
  block->id = *sema;
  sema_init(&block->sema, val);
  return true;
}

bool syscall_sema_up(char* sema) {
  if (sema == NULL)
    return false;
  struct prog_sema_block* block = get_prog_sema_block(*sema);
  if (block == NULL)
    return false;
  sema_up(&block->sema);
  return true;
}

bool syscall_sema_down(char* sema) {
  if (sema == NULL)
    return false;
  struct prog_sema_block* block = get_prog_sema_block(*sema);
  if (block == NULL)
    return false;
  sema_down(&block->sema);
  return true;
}

bool syscall_lock_init(char* lock) {
  if (lock == NULL)
    return false;
  struct process* pcb = thread_current()->pcb;
  struct prog_lock_block* block = malloc(sizeof(struct prog_lock_block));
  enum intr_level old_level = intr_disable();
  *lock = pcb->next_lock_id++;
  list_push_back(&pcb->prog_lock_list, &block->elem);
  intr_set_level(old_level);
  block->id = *lock;
  lock_init(&block->lock);
  return true;
}

bool syscall_lock_acquire(char* lock) {
  if (lock == NULL)
    return false;
  struct prog_lock_block* block = get_prog_lock_block(*lock);
  if (block == NULL || lock_held_by_current_thread(&block->lock))
    return false;
  lock_acquire(&block->lock);
  return true;
}

bool syscall_lock_release(char* lock) {
  if (lock == NULL)
    return false;
  struct prog_lock_block* block = get_prog_lock_block(*lock);
  if (block == NULL || !lock_held_by_current_thread(&block->lock))
    return false;
  lock_release(&block->lock);
  return true;
}

/* Initializes user programs in the system by ensuring the main
   thread has a minimal PCB so that it can execute and wait for
   the first user process. Any additions to the PCB should be also
   initialized here if main needs those members */
void userprog_init(void) {
  struct thread* t = thread_current();
  bool success;

  /* Allocate process control block
     It is imoprtant that this is a call to calloc and not malloc,
     so that t->pcb->pagedir is guaranteed to be NULL (the kernel's
     page directory) when t->pcb is assigned, because a timer interrupt
     can come at any time and activate our pagedir */
  t->pcb = calloc(sizeof(struct process), 1);
  success = t->pcb != NULL;

  lock_init(&file_lock);
  lock_init(&prog_lock);
  list_init(&thread_block_list);

  /* Kill the kernel if we did not succeed */
  ASSERT(success);
}

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   process id, or TID_ERROR if the thread cannot be created. */
pid_t process_execute(const char* file_name) {
  char* fn_copy;
  tid_t tid;
  struct thread_block* thread_block;

  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page(0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy(fn_copy, file_name, PGSIZE);

  thread_block = malloc(sizeof(struct thread_block));
  thread_block->was_waited = false;
  thread_block->exit_code = -1;
  if (thread_current()->pcb == NULL || thread_current()->pcb->main_thread == NULL)
    thread_block->pid = thread_current()->tid;
  else thread_block->pid = thread_current()->pcb->main_thread->tid;
  thread_block->load_success = false;
  sema_init(&thread_block->semapth, 0);
  sema_init(&thread_block->load_semapth, 0);
  lock_acquire(&prog_lock);
  list_push_back(&thread_block_list, &thread_block->elem);
  lock_release(&prog_lock);

  /* Create a new thread to execute FILE_NAME. */
  tid= thread_create(file_name, PRI_DEFAULT, start_process, fn_copy);
  thread_block->tid = tid;
  if (tid == TID_ERROR)
    palloc_free_page(fn_copy);

  sema_down(&thread_block->load_semapth);
  if (!thread_block->load_success)
    return TID_ERROR;

  return tid;
}

/* A thread function that loads a user process and starts it
   running. */
static void start_process(void* file_name_) {
  char* file_name = (char*)file_name_;
  struct thread* t = thread_current();
  struct intr_frame if_;
  bool success, pcb_success;
  char* argv = get_argv(file_name);
  struct thread_block* block;

  /* Allocate process control block */
  struct process* new_pcb = malloc(sizeof(struct process));
  success = pcb_success = new_pcb != NULL;

  /* Initialize process control block */
  if (success) {
    // Ensure that timer_interrupt() -> schedule() -> process_activate()
    // does not try to activate our uninitialized pagedir
    new_pcb->pagedir = NULL;
    t->pcb = new_pcb;

    new_pcb->next_fd = 2;
    lock_init(&new_pcb->file_list_lock);
    list_init(&new_pcb->all_files_list);
    list_init(&new_pcb->all_threads);
    sema_init(&new_pcb->semapth, 0);
    list_init(&new_pcb->prog_lock_list);
    list_init(&new_pcb->prog_sema_list);
    new_pcb->next_lock_id = 1;
    new_pcb->next_sema_id = 1;

    // Continue initializing the PCB as normal
    new_pcb->main_thread = t;
    strlcpy(new_pcb->process_name, argv, sizeof(new_pcb->process_name));
  }
  block = get_thread_block(t->tid);
  /* Initialize interrupt frame and load executable. */
  if (success) {
    memset(&if_, 0, sizeof if_);
    if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
    if_.cs = SEL_UCSEG;
    if_.eflags = FLAG_IF | FLAG_MBS;
    block->load_success = success = load(argv, &if_.eip, &if_.esp);
  }
  sema_up(&block->load_semapth);
  /* Handle failure with succesful PCB malloc. Must free the PCB */
  if (!success && pcb_success) {
    // Avoid race where PCB is freed before t->pcb is set to NULL
    // If this happens, then an unfortuantely timed timer interrupt
    // can try to activate the pagedir, but it is now freed memory
    struct process* pcb_to_free = t->pcb;
    t->pcb = NULL;
    free(pcb_to_free);
  }

  if (success) {
    args_push_stack(file_name, &if_.esp);
    free(argv);
  }

  /* Clean up. Exit on failure or jump to userspace */
  palloc_free_page(file_name);
  if (!success)
    thread_exit();

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm("fsave (%0)" : : "g"(&if_.fpu_reg));
  asm volatile("movl %0, %%esp; jmp intr_exit" : : "g"(&if_) : "memory");
  NOT_REACHED();
}

/* Waits for process with PID child_pid to die and returns its exit status.
   If it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If child_pid is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given PID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int process_wait(pid_t child_pid) {
  struct thread_block* block = get_thread_block(child_pid);
  if (block == NULL)
    return -1;
  if (block->was_waited)
    return -1;

  block->was_waited = true;
  pid_t pid = block->pid;
  tid_t cur_pid;
  if (thread_current()->pcb == NULL || thread_current()->pcb->main_thread == NULL)
    cur_pid = thread_current()->tid;
  else cur_pid = thread_current()->pcb->main_thread->tid;
  if (pid != cur_pid)
    return -1;
  sema_down(&block->semapth);
  int code = block->exit_code;
  lock_acquire(&prog_lock);
  list_remove(&block->elem);
  lock_release(&prog_lock);
  return code;
}

/* Free the current process's resources. */
void process_exit(void) {
  struct thread* cur = thread_current();
  uint32_t* pd;

  struct thread_block* block = get_thread_block(cur->tid);
  struct process* pcb = cur->pcb;

  /* If this thread does not have a PCB, don't worry */
  if (pcb == NULL) {
    if (block != NULL)
      sema_up(&block->semapth);
    thread_exit();
    NOT_REACHED();
  }

  while (!list_empty(&pcb->prog_lock_list)) {
    struct list_elem* e = list_pop_back(&pcb->prog_lock_list);
    struct prog_lock_block* block = list_entry(e, struct prog_lock_block, elem);
    free(block);
  }
  while (!list_empty(&pcb->prog_sema_list)) {
    struct list_elem* e = list_pop_back(&pcb->prog_sema_list);
    struct prog_sema_block* block = list_entry(e, struct prog_sema_block, elem);
    free(block);
  }
  lock_acquire(&file_lock);
  lock_acquire(&pcb->file_list_lock);
  while (!list_empty(&pcb->all_files_list)) {
    struct list_elem* e = list_pop_back(&pcb->all_files_list);
    struct file_list_elem* file_list_elem = list_entry(e, struct file_list_elem, elem);
    file_close(file_list_elem->file);
    free(file_list_elem);
  }
  lock_release(&pcb->file_list_lock);
  enum intr_level old_level = intr_disable();
  // 关闭所有子线程
  struct list* all_threads = &pcb->all_threads;
  for (struct list_elem* e = list_begin(all_threads); e != list_end(all_threads); e = list_next(e)) {
    struct thread* t = list_entry(e, struct thread, p_elem);
    struct thread_block* block = get_thread_block(t->tid);
    if (block != NULL) {
      sema_up(&block->semapth);
    }
    list_remove(&block->elem);
  }
  if (!is_main_thread(cur, pcb)) {
    struct thread* main_thread = cur->pcb->main_thread;
    struct thread_block* main_block = get_thread_block(main_thread->tid);
    if (main_block != NULL)
      sema_up(&main_block->semapth);
    kill_thread(main_thread);
  }
  while (!list_empty(all_threads)) {
    struct list_elem* e = list_pop_front(all_threads);
    struct thread* t = list_entry(e, struct thread, p_elem);
    if (cur->tid == t->tid)
      continue;
    kill_thread(t);
  }

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pcb->pagedir;

  file_close(cur->pcb->file);
  intr_set_level(old_level);
  lock_release(&file_lock);

  if (pd != NULL) {
    /* Correct ordering here is crucial.  We must set
         cur->pcb->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
    cur->pcb->pagedir = NULL;
    pagedir_activate(NULL);
    pagedir_destroy(pd);
  }

  /* Free the PCB of this process and kill this thread
     Avoid race where PCB is freed before t->pcb is set to NULL
     If this happens, then an unfortuantely timed timer interrupt
     can try to activate the pagedir, but it is now freed memory */
  struct process* pcb_to_free = cur->pcb;
  cur->pcb = NULL;
  free(pcb_to_free);

  if (block != NULL)
    sema_up(&block->semapth);
  remove_thread_block(cur->tid);

  thread_exit();
}

/* Sets up the CPU for running user code in the current
   thread. This function is called on every context switch. */
void process_activate(void) {
  struct thread* t = thread_current();

  /* Activate thread's page tables. */
  if (t->pcb != NULL && t->pcb->pagedir != NULL)
    pagedir_activate(t->pcb->pagedir);
  else
    pagedir_activate(NULL);

  /* Set thread's kernel stack for use in processing interrupts.
     This does nothing if this is not a user process. */
  tss_update();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32 /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32 /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32 /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16 /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr {
  unsigned char e_ident[16];
  Elf32_Half e_type;
  Elf32_Half e_machine;
  Elf32_Word e_version;
  Elf32_Addr e_entry;
  Elf32_Off e_phoff;
  Elf32_Off e_shoff;
  Elf32_Word e_flags;
  Elf32_Half e_ehsize;
  Elf32_Half e_phentsize;
  Elf32_Half e_phnum;
  Elf32_Half e_shentsize;
  Elf32_Half e_shnum;
  Elf32_Half e_shstrndx;
};

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr {
  Elf32_Word p_type;
  Elf32_Off p_offset;
  Elf32_Addr p_vaddr;
  Elf32_Addr p_paddr;
  Elf32_Word p_filesz;
  Elf32_Word p_memsz;
  Elf32_Word p_flags;
  Elf32_Word p_align;
};

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL 0           /* Ignore. */
#define PT_LOAD 1           /* Loadable segment. */
#define PT_DYNAMIC 2        /* Dynamic linking info. */
#define PT_INTERP 3         /* Name of dynamic loader. */
#define PT_NOTE 4           /* Auxiliary info. */
#define PT_SHLIB 5          /* Reserved. */
#define PT_PHDR 6           /* Program header table. */
#define PT_STACK 0x6474e551 /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1 /* Executable. */
#define PF_W 2 /* Writable. */
#define PF_R 4 /* Readable. */

static bool setup_stack(void** esp);
static bool validate_segment(const struct Elf32_Phdr*, struct file*);
static bool load_segment(struct file* file, off_t ofs, uint8_t* upage, uint32_t read_bytes,
                         uint32_t zero_bytes, bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool load(const char* file_name, void (**eip)(void), void** esp) {
  struct thread* t = thread_current();
  struct Elf32_Ehdr ehdr;
  struct file* file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pcb->pagedir = pagedir_create();
  if (t->pcb->pagedir == NULL)
    goto done;
  process_activate();

  /* Open executable file. */
  file = filesys_open(file_name);
  if (file == NULL) {
    printf("load: %s: open failed\n", file_name);
    goto done;
  }

  /* Read and verify executable header. */
  if (file_read(file, &ehdr, sizeof ehdr) != sizeof ehdr ||
      memcmp(ehdr.e_ident, "\177ELF\1\1\1", 7) || ehdr.e_type != 2 || ehdr.e_machine != 3 ||
      ehdr.e_version != 1 || ehdr.e_phentsize != sizeof(struct Elf32_Phdr) || ehdr.e_phnum > 1024) {
    printf("load: %s: error loading executable\n", file_name);
    goto done;
  }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) {
    struct Elf32_Phdr phdr;

    if (file_ofs < 0 || file_ofs > file_length(file))
      goto done;
    file_seek(file, file_ofs);

    if (file_read(file, &phdr, sizeof phdr) != sizeof phdr)
      goto done;
    file_ofs += sizeof phdr;
    switch (phdr.p_type) {
      case PT_NULL:
      case PT_NOTE:
      case PT_PHDR:
      case PT_STACK:
      default:
        /* Ignore this segment. */
        break;
      case PT_DYNAMIC:
      case PT_INTERP:
      case PT_SHLIB:
        goto done;
      case PT_LOAD:
        if (validate_segment(&phdr, file)) {
          bool writable = (phdr.p_flags & PF_W) != 0;
          uint32_t file_page = phdr.p_offset & ~PGMASK;
          uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
          uint32_t page_offset = phdr.p_vaddr & PGMASK;
          uint32_t read_bytes, zero_bytes;
          if (phdr.p_filesz > 0) {
            /* Normal segment.
                     Read initial part from disk and zero the rest. */
            read_bytes = page_offset + phdr.p_filesz;
            zero_bytes = (ROUND_UP(page_offset + phdr.p_memsz, PGSIZE) - read_bytes);
          } else {
            /* Entirely zero.
                     Don't read anything from disk. */
            read_bytes = 0;
            zero_bytes = ROUND_UP(page_offset + phdr.p_memsz, PGSIZE);
          }
          if (!load_segment(file, file_page, (void*)mem_page, read_bytes, zero_bytes, writable))
            goto done;
        } else
          goto done;
        break;
    }
  }

  /* Set up stack. */
  if (!setup_stack(esp))
    goto done;

  /* Start address. */
  *eip = (void (*)(void))ehdr.e_entry;

  success = true;

done:
  /* We arrive here whether the load is successful or not. */
  lock_acquire(&file_lock);
  if (success) {
    file_deny_write(file);
    t->pcb->file = file;
  } 
  else file_close(file);
  lock_release(&file_lock);
  return success;
}

/* load() helpers. */

static bool install_page(void* upage, void* kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool validate_segment(const struct Elf32_Phdr* phdr, struct file* file) {
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
    return false;

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off)file_length(file))
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz)
    return false;

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;

  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr((void*)phdr->p_vaddr))
    return false;
  if (!is_user_vaddr((void*)(phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool load_segment(struct file* file, off_t ofs, uint8_t* upage, uint32_t read_bytes,
                         uint32_t zero_bytes, bool writable) {
  ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT(pg_ofs(upage) == 0);
  ASSERT(ofs % PGSIZE == 0);

  file_seek(file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) {
    /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
    size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
    size_t page_zero_bytes = PGSIZE - page_read_bytes;

    /* Get a page of memory. */
    uint8_t* kpage = palloc_get_page(PAL_USER);
    if (kpage == NULL)
      return false;

    /* Load this page. */
    if (file_read(file, kpage, page_read_bytes) != (int)page_read_bytes) {
      palloc_free_page(kpage);
      return false;
    }
    memset(kpage + page_read_bytes, 0, page_zero_bytes);

    /* Add the page to the process's address space. */
    if (!install_page(upage, kpage, writable)) {
      palloc_free_page(kpage);
      return false;
    }

    /* Advance. */
    read_bytes -= page_read_bytes;
    zero_bytes -= page_zero_bytes;
    upage += PGSIZE;
  }
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool setup_stack(void** esp) {
  uint8_t* kpage;
  bool success = false;

  kpage = palloc_get_page(PAL_USER | PAL_ZERO);
  if (kpage != NULL) {
    success = install_page(((uint8_t*)PHYS_BASE) - PGSIZE, kpage, true);
    if (success)
      *esp = PHYS_BASE;
    else
      palloc_free_page(kpage);
  }
  return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool install_page(void* upage, void* kpage, bool writable) {
  struct thread* t = thread_current();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page(t->pcb->pagedir, upage) == NULL &&
          pagedir_set_page(t->pcb->pagedir, upage, kpage, writable));
}

/* Returns true if t is the main thread of the process p */
bool is_main_thread(struct thread* t, struct process* p) { return p->main_thread == t; }

/* Gets the PID of a process */
pid_t get_pid(struct process* p) { return (pid_t)p->main_thread->tid; }

/* Creates a new stack for the thread and sets up its arguments.
   Stores the thread's entry point into *EIP and its initial stack
   pointer into *ESP. Handles all cleanup if unsuccessful. Returns
   true if successful, false otherwise.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. You may find it necessary to change the
   function signature. */
bool setup_thread(void** esp) {
  uint8_t* kpage;
  bool success = false;
  enum intr_level old_level = intr_disable();
  struct thread* cur = thread_current();
  if (cur->pcb != NULL && cur->pcb->pagedir != NULL) {
    kpage = palloc_get_page(PAL_USER | PAL_ZERO);
    if (kpage != NULL) {
      void* base = PHYS_BASE;
      int max_pthread_num = 1000000;
      for (int i = 0; i < max_pthread_num; i++) {
        base -= PGSIZE;
        if (!pagedir_is_accessed(cur->pcb->pagedir, (uint8_t*)base - PGSIZE))
          break;
      }
      if (cur->pcb->pagedir != NULL)
        success = install_page(((uint8_t*)base) - PGSIZE, kpage, true);
      if (success) {
        cur->upage = ((uint8_t*)base) - PGSIZE;
        *esp = base;
      }
      else palloc_free_page(kpage);
    }
  }
  intr_set_level(old_level);
  return success;
}
/* Starts a new thread with a new user stack running SF, which takes
   TF and ARG as arguments on its user stack. This new thread may be
   scheduled (and may even exit) before pthread_execute () returns.
   Returns the new thread's TID or TID_ERROR if the thread cannot
   be created properly.

   This function will be implemented in Project 2: Multithreading and
   should be similar to process_execute (). For now, it does nothing.
   */
tid_t pthread_execute(stub_fun sf, pthread_fun tf, void* arg) {
  tid_t tid;
  struct start_pthread_args* start_pthread_args;
  struct thread_block* thread_block = malloc(sizeof(struct thread_block));
  thread_block->exit_code = -1;
  thread_block->was_waited = false;
  thread_block->pid = thread_current()->pcb->main_thread->tid;
  thread_block->load_success = false;
  lock_acquire(&prog_lock);
  list_push_back(&thread_block_list, &thread_block->elem);
  lock_release(&prog_lock);
  sema_init(&thread_block->semapth, 0);
  sema_init(&thread_block->load_semapth, 0);

  start_pthread_args = malloc(sizeof(struct start_pthread_args));
  start_pthread_args->pcb = thread_current()->pcb;
  start_pthread_args->tf = tf;
  start_pthread_args->sf = sf;
  start_pthread_args->arg = arg;

  const char* file_name = (char*)tf;
  thread_block->tid = tid = thread_create(file_name, PRI_DEFAULT, start_pthread, (void*)start_pthread_args);
  if (tid == TID_ERROR)
    free(start_pthread_args);
  sema_down(&thread_block->load_semapth);
  if (!thread_block->load_success)
    return TID_ERROR;
  return tid;
}
/* A thread function that creates a new user thread and starts it
   running. Responsible for adding itself to the list of threads in
   the PCB.

   This function will be implemented in Project 2: Multithreading and
   should be similar to start_process (). For now, it does nothing. */
static void start_pthread(void* exec_) {
  struct start_pthread_args* start_pthread_args = (struct start_pthread_args*)exec_;
  struct thread* cur = thread_current();
  struct intr_frame if_;
  struct thread_block* block = get_thread_block(cur->tid);
  cur->pcb = start_pthread_args->pcb;
  process_activate();

  memset(&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  if_.eip = (void (*)(void))start_pthread_args->sf;
  block->load_success = setup_thread(&if_.esp);

  if (block->load_success) {
    list_push_back(&cur->pcb->all_threads, &cur->p_elem);
    int align_size = 0x08;
    if_.esp -= align_size;
    memset(if_.esp, 0, align_size);

    if_.esp -= sizeof(start_pthread_args->arg);
    *(void**)if_.esp = start_pthread_args->arg;
    if_.esp -= sizeof(start_pthread_args->tf);
    *(void**)if_.esp = start_pthread_args->tf;
    if_.esp -= 0x04;
    memset(if_.esp, 0, 4);
  } 
  else {
    sema_up(&block->load_semapth);
    pthread_exit();
  }
  sema_up(&block->load_semapth);
  asm("fsave (%0)" : : "g"(&if_.fpu_reg));
  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile("movl %0, %%esp; jmp intr_exit" : : "g"(&if_) : "memory");
  NOT_REACHED();
}
/* Waits for thread with TID to die, if that thread was spawned
   in the same process and has not been waited on yet. Returns TID on
   success and returns TID_ERROR on failure immediately, without
   waiting.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
tid_t pthread_join(tid_t tid) {
  if (tid == thread_current()->pcb->main_thread->tid) {
    sema_down(&thread_current()->pcb->semapth);
    return tid;
  }
  struct thread_block* block = get_thread_block(tid);
  if (block == NULL)
    return TID_ERROR;
  if (block->pid != thread_current()->pcb->main_thread->tid)
    return TID_ERROR;
  if (block->was_waited)
    return TID_ERROR;
  block->was_waited = true;
  sema_down(&block->semapth);
  return tid;
}
/* Free the current thread's resources. Most resources will
   be freed on thread_exit(), so all we have to do is deallocate the
   thread's userspace stack. Wake any waiters on this thread.

   The main thread should not use this function. See
   pthread_exit_main() below.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
void pthread_exit(void) {
  struct thread* cur = thread_current();
  if (is_main_thread(cur, cur->pcb)) {
    pthread_exit_main();
    return;
  }
  struct thread_block* block = get_thread_block(cur->tid);
  if (block != NULL)
    sema_up(&block->semapth);
  if (cur->p_elem.next != NULL && cur->p_elem.prev != NULL)
    list_remove(&cur->p_elem);
  if (cur->pcb->pagedir != NULL) {
    void* upage = cur->upage;
    uint8_t* kpage = pagedir_get_page(cur->pcb->pagedir, upage);
    if (kpage != NULL)
      palloc_free_page(kpage);
    pagedir_clear_page(cur->pcb->pagedir, upage);
  }
  thread_exit();
}
/* Only to be used when the main thread explicitly calls pthread_exit.
   The main thread should wait on all threads in the process to
   terminate properly, before exiting itself. When it exits itself, it
   must terminate the process in addition to all necessary duties in
   pthread_exit.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
void pthread_exit_main(void) {
  struct process* p = thread_current()->pcb;
  sema_up(&p->semapth);
  while (1) {
    if (list_empty(&p->all_threads))
      break;
    struct list_elem* e = list_pop_front(&p->all_threads);
    if (e == NULL)
      break;
    struct thread* t = list_entry(e, struct thread, p_elem);
    pthread_join(t->tid);
  }
  struct thread_block* block = get_thread_block(thread_current()->tid);
  if (block != NULL) {
    sema_up(&block->semapth);
    printf("%s: exit(%d)\n", thread_current()->pcb->process_name, 0);
  }
  thread_exit();
}
