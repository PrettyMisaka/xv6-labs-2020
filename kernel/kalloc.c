// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end, int cpu);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.
extern int ncpu;

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
  char name[6];
} kmem[NCPU];

void
kinit()
{
  int step = ((PGROUNDUP(PHYSTOP) - PGROUNDUP((uint64)end)) >> PGSHIFT)/ncpu;
  char *_start = end, *_end = end + step*PGSIZE;

  for (int i = 0; i < ncpu; i++){
    snprintf(kmem[i].name, 6, "kmem%d", i);
    initlock(&kmem[i].lock, kmem[i].name);

    // freerange(end, (void*)PHYSTOP);
    freerange(_start, _end, i);

    _start = _end;
    _end = (i + 1 == ncpu)?((char*)PHYSTOP): _end + step*PGSIZE;
  }
}

void
__kfree(void *pa, int cpu)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem[cpu].lock);
  r->next = kmem[cpu].freelist;
  kmem[cpu].freelist = r;
  release(&kmem[cpu].lock);
}

void
freerange(void *pa_start, void *pa_end, int cpu)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    __kfree(p, cpu);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  push_off();
  int cpu = cpuid();
  pop_off();

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem[cpu].lock);
  r->next = kmem[cpu].freelist;
  kmem[cpu].freelist = r;
  release(&kmem[cpu].lock);
}

void *kalloc_from_other_cpu(int cpu)
{
  struct run *r = 0;

  for (int i = 0; i < ncpu; i++){
    if(cpu == i)
      continue;
    
    if(kmem[i].freelist == 0)
      continue;
    
    acquire(&kmem[i].lock);

    r = kmem[i].freelist;
    if(r)
      kmem[i].freelist = r->next;

    release(&kmem[i].lock);

    if(r)
      return (void*)r;
  }

  return (void*)r;
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  push_off();
  int cpu = cpuid();
  pop_off();

  acquire(&kmem[cpu].lock);
  r = kmem[cpu].freelist;
  if(r)
    kmem[cpu].freelist = r->next;
  release(&kmem[cpu].lock);

  if(r == 0)
    r = kalloc_from_other_cpu(cpu);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
