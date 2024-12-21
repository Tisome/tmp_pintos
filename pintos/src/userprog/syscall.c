#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "userprog/process.h"

#include "filesys/filesys.h"
#include "devices/input.h"
#include "devices/shutdown.h"
#include "filesys/file.h"
#include "pagedir.h"
#include "stddef.h"
#include <float.h>
#include "threads/vaddr.h"

static void syscall_handler(struct intr_frame*);

void syscall_init(void) { intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall"); }

bool check_valid_addr(struct intr_frame* f, void* addr) {
  if (!is_user_vaddr(addr) || pagedir_get_page(thread_current()->pcb->pagedir, addr) == NULL) {
    syscall_exit(f, -1);
    return false;
  }
  return true;
}

void syscall_exec(struct intr_frame* f, const char* args1) {
  if (!check_valid_addr(f, args1) || !check_valid_addr(f, args1 + 0x04))
    return;
  pid_t pid = process_execute(args1);
  f->eax = pid;
}

void syscall_exit(struct intr_frame* f, int args1) {
  f->eax = args1;
  struct thread* cur = thread_current();
  set_exit_code(cur, args1);
  printf("%s: exit(%d)\n", cur->pcb->process_name, args1);
  process_exit();
}

void syscall_open(struct intr_frame* f, const char* file) {
  if (!check_valid_addr(f, file))
    return;
  f->eax = open_for_syscall(file);
}

void syscall_close(struct intr_frame* f, int fd) {
  if (!close_file(fd)) {
    f->eax = -1;
    return;
  }
}

void syscall_create(struct intr_frame* f, const char* file, unsigned initial_size) {
  if (!check_valid_addr(f, file))
    return;
  f->eax = filesys_create(file, initial_size);
}

void syscall_remove(struct intr_frame* f, const char* file) {
  if (!check_valid_addr(f, file))
    return;
  f->eax = filesys_remove(file);
}

int syscall_read(int fd, void* buffer, unsigned size) {
  if (fd == 0) {
    char* buffer_vector = (char*)buffer;
    for (unsigned i = 0; i < size; i++) {
      uint8_t inputc = input_getc();
      buffer_vector[i] = inputc;
    }
    return size;
  }
  struct process* pcb = thread_current()->pcb;
  if (pcb == NULL)
    return -1;
  struct file* file = fd_to_file(fd);
  if (file == NULL)
    return -1;
  lock_acquire(&pcb->file_list_lock);
  int flag = file_read(file, buffer, size);
  lock_release(&pcb->file_list_lock);
  return flag;
}

int syscall_write(int fd, const void* buffer, unsigned size) {
  if (fd == 1) {
    putbuf((const char*)buffer, size);
    return size;
  }

  struct file* file = fd_to_file(fd);
  if (file == NULL)
    return -1;
  struct process* pcb = thread_current()->pcb;
  if (pcb == NULL)
    return -1;
  lock_acquire(&pcb->file_list_lock);
  int write_size = file_write(file, buffer, size);
  lock_release(&pcb->file_list_lock);
  return write_size;
}

void syscall_file_size(struct intr_frame* f, int fd) {
  struct file* file = fd_to_file(fd);
  if (file == NULL) {
    f->eax = -1;
    return;
  }
  f->eax = file_length(file);
}

void syscall_seek(struct intr_frame* f, int fd, unsigned position) {
  struct file* file = fd_to_file(fd);
  if (file == NULL) {
    f->eax = -1;
    return;
  }
  file_seek(file, position);
}

void syscall_tell(struct intr_frame* f, int fd) {
  struct file* file = fd_to_file(fd);
  if (file == NULL) {
    f->eax = -1;
    return;
  }
  f->eax = file_tell(file);
}

static void syscall_handler(struct intr_frame* f UNUSED) {
  uint32_t* args = ((uint32_t*)f->esp);
  if (!check_valid_addr(f, (char*)args) || !check_valid_addr(f, (char*)(args + 0x04)))
    return;
  /*
   * The following print statement, if uncommented, will print out the syscall
   * number whenever a process enters a system call. You might find it useful
   * when debugging. It will cause tests to fail, however, so you should not
   * include it in your final submission.
   */

  /* printf("System call number: %d\n", args[0]); */
  int syscall_arg = args[0];
  switch (syscall_arg) {
    case SYS_HALT:
      shutdown_power_off();
      break;
    case SYS_EXEC:
      syscall_exec(f, (char*)args[1]);
      break;
    case SYS_EXIT:
      syscall_exit(f, args[1]);
      break;
    case SYS_OPEN:
      syscall_open(f, (char*)args[1]);
      break;
    case SYS_CLOSE:
      syscall_close(f, args[1]);
      break;
    case SYS_CREATE:
      syscall_create(f, (char*)args[1], args[2]);
      break;
    case SYS_REMOVE: 
      syscall_remove(f, (char*)args[1]); 
      break;
    case SYS_READ:
      if (!check_valid_addr(f, (char*)args[2]) || !check_valid_addr(f, (char*)(args + 0x10)) || !check_valid_addr(f, (char*)(args + 0x0c)) || !check_valid_addr(f, (char*)(args + 0x08)))
        return;
      f->eax = syscall_read(args[1], (char*)args[2], args[3]);
      break;
    case SYS_WRITE:
      if (!check_valid_addr(f, (char*)args[2]))
        return;
      f->eax = syscall_write(args[1], (char*)args[2], args[3]);
      break;
    case SYS_FILESIZE: 
      if (!check_valid_addr(f, (char*)(args + 0x08)))
        return; 
      syscall_file_size(f, args[1]);
      break;
    case SYS_SEEK:  
      syscall_seek(f, args[1], args[2]);
      break;
    case SYS_TELL:
      syscall_tell(f, args[1]);
      break;
    case SYS_WAIT:
      f->eax = process_wait(args[1]);
      break;
    case SYS_PRACTICE:
      f->eax = (int)args[1] + 1;
      break;
    case SYS_COMPUTE_E:
      f->eax = sys_sum_to_e((int)args[1]);
      break;
    case SYS_PT_CREATE:
      if (!check_valid_addr(f, (char*)(args + 0x10)))
        return;
      f->eax = pthread_execute((stub_fun)args[1], (pthread_fun)args[2], (void*)args[3]);
      break;
    case SYS_PT_EXIT:
      pthread_exit();
      break;
    case SYS_PT_JOIN:
      f->eax = pthread_join((tid_t)args[1]);
      break;
    case SYS_SEMA_INIT:
      f->eax = syscall_sema_init((char*)args[1], (int)args[2]);
      break;
    case SYS_SEMA_UP:
      f->eax = syscall_sema_up((char*)args[1]);
      break;
    case SYS_SEMA_DOWN:
      f->eax = syscall_sema_down((char*)args[1]);
      break;
    case SYS_LOCK_INIT:
      f->eax = syscall_lock_init((char*)args[1]);
      break;
    case SYS_LOCK_ACQUIRE:
      f->eax = syscall_lock_acquire((char*)args[1]);
      break;
    case SYS_LOCK_RELEASE:
      f->eax = syscall_lock_release((char*)args[1]);
      break;
    case SYS_GET_TID:
      f->eax = thread_current()->tid;
      break;
    default:
      break;
  }
}
