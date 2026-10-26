#include "types.h"
#include "defs.h"
#include "proc.h"
#include "trap.h"
#include "riscv.h"

volatile int ticks = 0;  // 전역 tick 카운터

// 타이머 인터럽트 핸들러
void
timer_interrupt(void)
{
    // tick 증가 
    ticks++;

    // 100ms 주기마다 강제 컨텍스트 스위치 준비
    if (ticks % 100 == 0) {
        struct proc *p = myproc();
        if (p && p->state == RUNNING) {
            // CPU 반납 요청
            yield();
        }
    }

    // 필요시 다른 타이머 관련 처리 추가
}