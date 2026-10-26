#include "types.h"      // uint, uchar, 등 기본 타입
#include "riscv.h"      // r_mhartid(), r_mstatus() 등
#include "defs.h"       // syscall(), acquire(), release() 등
#include "param.h"      // NPROC
#include "memlayout.h"  // 가상 메모리 레이아웃
#include "spinlock.h"   // struct spinlock
#include "proc.h"       // struct proc, myproc(), fork(), wait(), exit()

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return wait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int n;

  argint(0, &n);
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if(n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}


uint64
sys_kill(void)
{
  int pid;
  argint(0, &pid);
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;
  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}


// set the priority of a given pid (only affects level 2)
// argint returns void, so we call it as a statement
uint64
sys_setpriority(void)
{
  int pid, pr;
  argint(0, &pid);
  argint(1, &pr);
  if (pr < 0 || pr > 3)
    return -1;
  struct proc *p;
  for (p = &ptable.proc[0]; p < &ptable.proc[NPROC]; p++) {
    if (p->pid == pid) {
      p->priority = pr;
      return 0;
    }
  }
  return -1;
}


// yield the CPU
uint64
sys_yield(void)
{
  yield();
  return 0;
}

// get current scheduling level (MLFQ) or 99 for FCFS
uint64
sys_getlev(void)
{
  int lev = myproc()->level;
  return (lev < 0) ? 99 : lev;
}



// switch scheduler into MLFQ mode
uint64
sys_mlfqmode(void)
{
  if(current_mode == MODE_MLFQ)
    return -1;
  current_mode = MODE_MLFQ;
  struct proc *p;
  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == RUNNABLE){
      p->level = 0;
      p->priority = 3;
      p->time_quantum = 2*0 + 1;
    }
  }
  release(&ptable.lock);
  return 0;
}

// switch scheduler into FCFS mode
uint64
sys_fcfsmode(void)
{
  if(current_mode == MODE_FCFS)
    return -1;
  current_mode = MODE_FCFS;
  struct proc *p;
  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == RUNNABLE){
      p->level = -1;
      p->priority = -1;
      p->time_quantum = -1;
    }
  }
  release(&ptable.lock);
  return 0;
}
