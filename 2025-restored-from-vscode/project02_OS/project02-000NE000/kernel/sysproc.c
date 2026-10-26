#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"


extern struct spinlock wait_lock;
extern struct proc proc[];
extern void freeproc(struct proc *p);
struct proc* allocproc(void);

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
  int   n;
  uint64 addr;

  argint(0, &n);
  struct proc *p = myproc();
  // lock heap growth across threads
  acquire(&p->svm->memlock);
  addr = p->sz;  // or p->sz if you keep it there
  if(growproc(n) < 0){
    release(&p->svm->memlock);
    return -1;
  }
  release(&p->svm->memlock);
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

uint64
sys_clone(void)
{
  uint64 fcn, a1, a2, stack;
  // just fetch, no <0 check
  argaddr(0, &fcn);
  argaddr(1, &a1);
  argaddr(2, &a2);
  argaddr(3, &stack);

  if(stack % PGSIZE)
    return -1;

  struct proc *p  = myproc();
  struct proc *np = allocproc();  // or allocproc_thread()

  if(!np)
    return -1;

  // share address space
  np->svm   = p->svm;
  np->ptype = PROC_THREAD;
  np->parent= p;
  np->ustack= (void*)stack;

  acquire(&p->svm->memlock);
  p->svm->refcnt++;
  release(&p->svm->memlock);

  memmove(np->trapframe, p->trapframe, sizeof(*p->trapframe));
  np->trapframe->epc = fcn;
  np->trapframe->a0  = a1;
  np->trapframe->a1  = a2;
  np->trapframe->sp  = stack + PGSIZE;

  for(int i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));
  np->state = RUNNABLE;

  return np->pid;
}

uint64
sys_join(void)
{
  uint64 ustack_ptr;
  argaddr(0, &ustack_ptr);

  struct proc *p = myproc();
  acquire(&wait_lock);
  for(;;){
    for(struct proc *t = proc; t < &proc[NPROC]; t++){
      if(t->parent == p
        && t->ptype == PROC_THREAD
        && t->state == ZOMBIE){
        int tid = t->pid;
        if(copyout(p->pagetable, ustack_ptr,
          (char*)&t->ustack,
          sizeof(void*)) < 0){
          release(&wait_lock);
          return -1;
        }
        freeproc(t);           // make sure this is non-static
        release(&wait_lock);
        return tid;
      }
    }
    sleep(p, &wait_lock);
  }
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
