#include "types.h"
#include "defs.h"    // mycpu(), context_switch()
#include "param.h"
#include "memlayout.h"
#include "proc.h"
#include "riscv.h"


// Basic dispatcher: FCFS/MLFQ 결정 전에 context switch 호출
void
scheduler(void)
{
    struct cpu *c = mycpu();
    struct proc *p;

    for (;;)
    {
        intr_on(); // 인터럽트 허용

        // 모든 프로세스 조사
        for (p = proc; p < &proc[NPROC]; p++)
        {
            acquire(&p->lock);
            if (p->state == RUNNABLE)
            {
                // RUNNABLE → RUNNING
                p->state = RUNNING;
                c->proc = p;

                // 실제 레지스터 스위칭: CPU context ←→ proc context
                context_switch(&c->context, &p->context);

                // 돌아오면 proc 초기화
                c->proc = 0;
            }
            release(&p->lock);
        }
    }
}