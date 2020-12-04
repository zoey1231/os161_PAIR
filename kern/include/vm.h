/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _VM_H_
#define _VM_H_

/*
 * VM system-related definitions.
 *
 * You'll probably want to add stuff here.
 */

#include <machine/vm.h>
#include <vnode.h>
#include <bitmap.h>
#include <synch.h>
#include <array.h>
#include <addrspace.h>
#include <types.h>

struct addrspace;

// struct for frametable entry
struct frame
{
    struct addrspace *as; //if the physcical page is used by the kernel, as=NULL; otherwise, if it's used in a page table entry, as points to the address space containing that page table.
    uint8_t used;         //used to indicate the physical page is free or not
    uint8_t kernel;       //1 indicates the physical page is used by the kernel and 0 indicates it's used by the user process
    vaddr_t vaddr;
    size_t size;
};

extern int vm_initialized;
extern struct frame *coremap;
extern paddr_t freepage;
extern struct spinlock *coremap_lock;

extern struct vnode *swap_disk;
extern struct bitmap *swap_disk_bitmap;
extern struct lock *swap_disk_lock;
extern unsigned long total_frame_num;

#define COREMAP_INDEX(PADDR) ((PADDR & PAGE_FRAME) >> 12)

/* Fault-type arguments to vm_fault() */
#define VM_FAULT_READ 0     /* A read was attempted */
#define VM_FAULT_WRITE 1    /* A write was attempted */
#define VM_FAULT_READONLY 2 /* A write to a readonly page was attempted*/

/* Initialization function */
void vm_bootstrap(void);

/* Fault handling function called by trap code */
int vm_fault(int faulttype, vaddr_t faultaddress);

/* Allocate/free kernel heap pages (called by kmalloc/kfree) */
vaddr_t alloc_kpages(unsigned npages);
void free_kpages(vaddr_t addr);

/* TLB shootdown handling called from interprocessor_interrupt */
void vm_tlbshootdown_all(void);
void vm_tlbshootdown(const struct tlbshootdown *);

//get a physical page for use in vmfault
paddr_t get_single_ppage(vaddr_t vaddr, struct addrspace *as);

paddr_t swap_page(vaddr_t newvaddr, struct addrspace *newas);

#endif /* _VM_H_ */
