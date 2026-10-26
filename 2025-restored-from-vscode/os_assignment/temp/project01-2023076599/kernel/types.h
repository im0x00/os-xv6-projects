typedef unsigned int   uint;
typedef unsigned short ushort;
typedef unsigned char  uchar;

typedef unsigned char uint8;
typedef unsigned short uint16;
typedef unsigned int  uint32;
typedef unsigned long uint64;

typedef uint64    pte_t;         // a single PTE
typedef pte_t *   pagetable_t;   // the root of a page table

typedef uint64 pde_t;
#ifndef NULL
#define NULL ((void*)0)
#endif