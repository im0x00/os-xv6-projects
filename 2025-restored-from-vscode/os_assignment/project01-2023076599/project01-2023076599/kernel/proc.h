// kernel/proc.h

#ifndef PROC_H
#define PROC_H

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"

// Saved registers for kernel context switches.
// Must match layout of context in swtch.S.
struct context {
  uint64 ra;
  uint64 sp;

  // callee-saved
  uint64 s0;
  uint64 s1;
  uint64 s2;
  uint64 s3;
  uint64 s4;
  uint64 s5;
  uint64 s6;
  uint64 s7;
  uint64 s8;
  uint64 s9;
  uint64 s10;
  uint64 s11;
};

// Per-CPU state
struct cpu {
  struct proc *proc;        // The process running on this cpu, or null.
  struct context context;   // swtch here to enter scheduler
  int noff;                 // Depth of push_off() nesting.
  int intena;               // Were interrupts enabled before push_off()?
};

extern struct cpu cpus[NCPU];
extern int ncpu;

// Saved user registers for trampoline.S.
// Trap entry/exit code arranges to save user registers in a trapframe.
// The trapframe sits in its own page, so it can be write-protected
// by the user kernel to isolate user memory from kernel.  
struct trapframe {
  // registers as pushed by low-level trap entry code
  uint64 kernel_satp;   // kernel page table
  uint64 kernel_sp;     // top of process's kernel stack
  uint64 kernel_trap;   // usertrap()
  uint64 epc;           // saved user program counter
  uint64 kernel_hartid; // saved kernel tp

  // saved user registers
  uint64 ra;
  uint64 sp;
  uint64 gp;
  uint64 tp;
  uint64 t0;
  uint64 t1;
  uint64 t2;
  uint64 s0;
  uint64 s1;
  uint64 a0;
  uint64 a1;
  uint64 a2;
  uint64 a3;
  uint64 a4;
  uint64 a5;
  uint64 a6;
  uint64 a7;
  uint64 s2;
  uint64 s3;
  uint64 s4;
  uint64 s5;
  uint64 s6;
  uint64 s7;
  uint64 s8;
  uint64 s9;
  uint64 s10;
  uint64 s11;
  uint64 t3;
  uint64 t4;
  uint64 t5;
  uint64 t6;
};

enum procstate { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

// Per-process state
struct proc {
  struct spinlock lock;        // Lock to protect this struct
  enum procstate state;        // Process state
  void *chan;                  // If sleeping, why
  int killed;                  // If non-zero, have been killed
  int xstate;                  // Exit status to be returned to parent's wait
  int pid;                     // Process ID

  // wait_lock must be held when using these:
  struct proc *parent;         // Parent process
  struct trapframe *trapframe; // data page for trampoline.S
  pagetable_t pagetable;       // User page table
  uint64 kstack;               // Virtual address of kernel stack

  // saved user memory size
  uint64 sz;                   // Size of process memory (bytes)
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)
  
  struct context context;
  // ────────────────────────────────────────────────────────────────────
  // Scheduler metadata:
  uint64 creation_time;        // tick when first became RUNNABLE (for FCFS)
  int     level;               // -1=FCFS, 0..2=MLFQ queues
  int     priority;            // 0 (high) .. 3 (low), only for level 2
  int     time_quantum;        // remaining ticks in current slice
  // ────────────────────────────────────────────────────────────────────
  struct proc *next;
};

// Scheduler mode
enum sched_mode { MODE_FCFS, MODE_MLFQ };
extern enum sched_mode current_mode;
extern uint64 global_tick;
void boost_all(void);

struct ptable {
  struct spinlock lock;
  struct proc      proc[NPROC];
};
extern struct ptable ptable;        // 선언만!

// 기존 루프(for(p = proc; …) )용 alias 포인터 선언
extern struct proc *const proc;

#endif // PROC_H