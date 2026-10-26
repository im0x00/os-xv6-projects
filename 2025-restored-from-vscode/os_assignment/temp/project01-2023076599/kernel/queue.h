#pragma once
#ifndef QUEUE_H
#define QUEUE_H

#include "types.h"
#include "param.h"

// Singly-linked FIFO queue of struct proc*
struct queue {
    struct proc *head;
    struct proc *tail;
};

// Initialize a queue to empty
static inline void
queue_init(struct queue *q)
{
    q->head = q->tail = NULL;
}

// Enqueue process p onto tail of q
void enqueue(struct queue *q, struct proc *p);

// Dequeue process from head of q, or return NULL if empty
struct proc *dequeue(struct queue *q);

// Global ready queues
extern struct queue fcfs_queue;
extern struct queue mlfq[NMLFQ_LEVELS];

void remove_proc(struct queue *q, struct proc *p);
#endif // QUEUE_H