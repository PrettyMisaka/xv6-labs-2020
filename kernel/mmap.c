#include "types.h"
#include "param.h"
#include "riscv.h"
#include "fcntl.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"

struct vma vma[NVMA];
struct spinlock vma_lock; // handle used and list data

void mmapinit() 
{
    struct vma *v;

    initlock(&vma_lock, "vmalock");
    for(v = vma; v < &vma[NPROC]; v++) {
        initlock(&v->lock, "mmap");
        v->used = 0;
    }
}

struct vma* alloc_vma()
{
    struct vma *v;
    acquire(&vma_lock);
    for(v = vma; v < &vma[NPROC]; v++) {
        if(v->used == 0)
            goto found;
    }
    release(&vma_lock);
    return 0;

found:
    v->used = 1;
    v->page_map = 0;
    v->proc_vma_next = 0;
    v->shared_vma_head = 0;
    v->shared_vma_next = 0;
    release(&vma_lock);

    return v;
}

void add_vma_to_proc(struct proc *p, struct vma *v)
{
    struct vma * _v = p->proc_vma_next;
    if(_v == 0){
        p->proc_vma_next = v;
        return ;
    }
    acquire(&vma_lock);
    while(_v->proc_vma_next != 0)
        _v = _v->proc_vma_next;
    _v->proc_vma_next = v;
    release(&vma_lock);
    return ;
}

uint64 do_mmap(uint64 vaddr, int length, int prot, int flags,
                  struct file *f, int offset)
{
    struct proc *p = myproc();
    struct vma *v;

    if(vaddr != 0 && vaddr < p->msz)
        return -1;

    if((!f->writable && prot & PROT_WRITE) && flags == MAP_SHARED)
        return -1;

    v = alloc_vma();
    acquire(&v->lock);
    v->p = p;
    v->f = f;
    filedup(f);
    v->data.flag = flags;
    v->data.prot = prot;
    v->data.len = length;
    v->data.f_offset = offset;
    if(v->vaddr == 0){
        v->vaddr = PGROUNDDOWN(p->msz);
        p->msz = PGROUNDUP(p->msz + length);
    }else{
        panic("do_mmap: vaddr should be 0!");
    }
    release(&v->lock);

    add_vma_to_proc(p, v);

    if(flags == MAP_SHARED)
    {
        struct vma * _v;
        acquire(&vma_lock);
        for(_v = vma; _v < &vma[NPROC]; _v++) {
            if(_v == v)
                continue;

            acquire(&_v->lock);
            if(_v->used == 0 || _v->data.flag != MAP_SHARED){
                release(&_v->lock);
                continue;
            }
            if(memcmp(&v->data, &_v->data, sizeof(struct vma_data)) == 0){
                release(&_v->lock);
                goto add_to_list;
            }
            release(&_v->lock);
        }

        v->shared_vma_head = v;
        release(&vma_lock);
        return v->vaddr;

add_to_list:
        _v = _v->shared_vma_head;
        if(_v == 0)
            panic("MAP_SHARED vma head don't exist");
        v->shared_vma_head = _v;
        while(_v->shared_vma_next != 0)
            _v = _v->shared_vma_next;
        _v->shared_vma_next = v;
        release(&vma_lock);
        return v->vaddr;
    }

    return v->vaddr;
}

int check_mmap_page_and_alloc(uint64 stval)
{
    struct proc *p = myproc();
    struct vma *v = p->proc_vma_next;
    struct file *f;
    uint64 vaddr = PGROUNDDOWN(stval);
    char * paddr;
    int offset;

    while(v != 0){
        if(v->vaddr <= stval && stval < v->vaddr + v->data.len)
            goto alloc_page;
        v = v->proc_vma_next;
    }
    return -1;
alloc_page:
    //if flag is MAP_SHARED, search if alloc phy page
    offset = stval - v->vaddr;
    paddr = 0;
    if(walkaddr(v->p->pagetable, vaddr))
        return 0;
    if(v->data.flag == MAP_SHARED){
        struct vma *_v = v->shared_vma_head;

        while(_v != 0){
            if(_v != v){
                paddr = (void *)walkaddr(_v->p->pagetable, PGROUNDDOWN(_v->vaddr + offset));
                if(paddr != 0)
                    break;
            }
            _v = _v->shared_vma_next;
        }

        if(paddr != 0){
            if(mappages(v->p->pagetable, vaddr, PGSIZE, (uint64)paddr, (v->data.prot << 1)|PTE_U) != 0){
                panic("check_mmap_page_and_alloc: mmapages failed!");
            }
            return 0;
        }
    }

    f = v->f;
    paddr = kalloc();
    if(paddr == 0)
        return -1;
    if(mappages(v->p->pagetable, vaddr, PGSIZE, (uint64)paddr, (v->data.prot << 1)|PTE_U) != 0){
        panic("check_mmap_page_and_alloc: mmapages failed!");
    }
    memset(paddr, 0, PGSIZE);
    ilock(f->ip);
    readi(f->ip, 1, vaddr, PGROUNDDOWN(offset) + v->data.f_offset, PGSIZE);
    iunlock(f->ip);

    return 0;
}

void free_vma(struct proc *p, struct vma *v)
{
    struct file *f;
    uint64 vpage = PGROUNDDOWN(v->vaddr);
    uint64 vpage_max;
    pte_t *pte;
    int len;
    int w;
    int free_page = 1;

    f = v->f;
    w = (v->data.prot & PROT_WRITE && f->writable)?1:0;

    if(v->data.flag == MAP_SHARED){
        acquire(&vma_lock);
        struct vma *_v = v->shared_vma_head;
        struct vma *_v_n_head = _v;
        if(_v == v){
            if(v->shared_vma_next == 0){
                free_page = 0;
                goto skip_shared_unlink;
            }
            _v = _v_n_head = _v->shared_vma_next;
            //update head
            while(_v != 0){
                _v->shared_vma_head = _v_n_head;
                _v = _v->shared_vma_next;
            }
            goto skip_shared_unlink;
        }
        
        while(_v != 0){
            if(_v->shared_vma_next == v){
                _v->shared_vma_next = v->shared_vma_next;
                break;
            }
            _v = _v->shared_vma_next;
        }
skip_shared_unlink:
        v->shared_vma_next = 0;
        v->shared_vma_head = v;
        release(&vma_lock);
    }

    vpage_max = PGROUNDDOWN(vpage + v->data.len);
    for (uint64 _vaddr = vpage; _vaddr < PGROUNDUP(vpage + v->data.len); _vaddr += PGSIZE)
    {
        pte = walk(p->pagetable, _vaddr, 0);
        if(pte == 0 || 
            (*pte & PTE_V) == 0 ||
            (*pte & PTE_U) == 0 )
            continue;

        if(w){
            len = PGSIZE;
            if(vpage_max == _vaddr){
                len = v->data.len - (_vaddr - vpage);
            }
            begin_op();
            ilock(f->ip);
            writei(f->ip, 1, _vaddr, v->data.f_offset+_vaddr-vpage, len);
            iunlock(f->ip);
            end_op();
        }

        uvmunmap(p->pagetable, _vaddr, 1, free_page);
    }    
}

int do_munmap(uint64 vaddr, int length)
{
    struct proc *p = myproc();
    struct vma *v = p->proc_vma_next;
    struct vma *_v = 0;
    uint64 vpage = PGROUNDDOWN(vaddr);
    // uint64 pa;

    while(v != 0){
        if(v->vaddr <= vpage && vpage < v->vaddr + v->data.len)
            goto munmap_page;
        _v = v;
        v = v->proc_vma_next;
    }
    return -1;

munmap_page:
    if(length < v->data.len)
        return -1;

    free_vma(p, v);
    acquire(&vma_lock);
    if(_v == 0){
        p->proc_vma_next = v->proc_vma_next;
    }else{
        _v->proc_vma_next = v->proc_vma_next;
    }
    v->proc_vma_next = 0;
    release(&vma_lock);

    return 0;
}

void free_proc_vma(struct proc *p)
{
    struct vma *v = p->proc_vma_next;

    while(v != 0){
        free_vma(p, v);

        v = v->proc_vma_next;
    }
}

