// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBHASH 5

#define blockno2hash(blockno) blockno%NBHASH

struct bhash{
  struct spinlock lock;

  struct buf head;
};

struct {
  // struct spinlock lock;
  struct buf buf[NBUF];
  struct bhash hash[NBHASH];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  // struct buf head;
} bcache;

void
binit(void)
{
  struct buf *b;
  struct bhash *bhash;

  for(int i = 0; i < NBHASH; i++){
    initlock(&bcache.hash[i].lock, "bhcache");

    bcache.hash[i].head.prev = &bcache.hash[i].head;
    bcache.hash[i].head.next = &bcache.hash[i].head;
  }

  // Create linked list of buffers
  // initlock(&bcache.lock, "bcache");
  // for(b = bcache.buf; b < bcache.buf+NBUF; b++){
  for(int i = 0; i < NBUF; i++){
    b = &bcache.buf[i];
    bhash = &bcache.hash[blockno2hash(i)];

    b->next = bhash->head.next;
    b->prev = &bhash->head;
    initsleeplock(&b->lock, "buffer");
    bhash->head.next->prev = b;
    bhash->head.next = b;
    b->ticks = 0;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  struct buf *lru_b;
  struct bhash *bhash;

  // acquire(&bcache.lock);

  bhash = &bcache.hash[blockno2hash(blockno)];
  acquire(&bhash->lock);
  // Is the block already cached?
  for(b = bhash->head.next; b != &bhash->head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      // release(&bcache.lock);
      release(&bhash->lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  lru_b = bhash->head.prev;
  if(!lru_b)
    panic("lru_b is NULL");
  
  for(b = bhash->head.prev; b != &bhash->head; b = b->prev){
    if(b->refcnt == 0) {
setb:
      //should mv b to another bhash
      // if ((i != blockno2hash(blockno)) && (blockno2hash(b->blockno) != blockno2hash(blockno))){
      //   struct spinlock *__lock = &bhash->lock;
      //   bhash = &bcache.hash[blockno2hash(blockno)];
      //   acquire(&bhash->lock);

      //   b->next->prev = b->prev;
      //   b->prev->next = b->next;

      //   b->next = bhash->head.next;
      //   b->prev = &bhash->head;
      //   bhash->head.next->prev = b;
      //   bhash->head.next = b;

      //   release(__lock);  
      // }
      
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;

      // release(&bcache.lock);
      release(&bhash->lock);
      acquiresleep(&b->lock);
      return b;
    }

    if(lru_b->ticks > b->ticks)
      lru_b = b;
  }
  if(lru_b && 1)
    goto setb;
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  b->ticks = ticks;
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
  b->ticks = ticks;
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  struct bhash *bhash;
  bhash = &bcache.hash[blockno2hash(b->blockno)];
  
  // acquire(&bcache.lock);
  acquire(&bhash->lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bhash->head.next;
    b->prev = &bhash->head;
    bhash->head.next->prev = b;
    bhash->head.next = b;
  }
  
  // release(&bcache.lock);
  release(&bhash->lock);
}

void
bpin(struct buf *b) {
  struct bhash *bhash;
  bhash = &bcache.hash[blockno2hash(b->blockno)];

  acquire(&bhash->lock);
  b->refcnt++;
  release(&bhash->lock);
}

void
bunpin(struct buf *b) {
  struct bhash *bhash;
  bhash = &bcache.hash[blockno2hash(b->blockno)];

  acquire(&bhash->lock);
  b->refcnt--;
  release(&bhash->lock);
}


