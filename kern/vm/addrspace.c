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

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <addrspace.h>
#include <vm.h>
#include <bitmap.h>
#include <spl.h>
#include <mips/tlb.h>
#include <elf.h>
#include <current.h>
#include <proc.h>
#include <cpu.h>
#include <uio.h>
#include <vnode.h>
#include <bitmap.h>
#define VM_STACKPAGES 18
#define VM_HEAPPAGES 18

/*
 * Note! If OPT_DUMBVM is set, as is the case until you start the VM
 * assignment, this file is not compiled or linked or in any way
 * used. The cheesy hack versions in dumbvm.c are used instead.
 */

struct addrspace *
as_create(void)
{
	struct addrspace *as;

	as = kmalloc(sizeof(struct addrspace));
	if (as == NULL)
	{
		return NULL;
	}

	/*
	 * Initialize as needed.
	 */
	as->vbase1 = 0;
	as->vtop1 = 0;
	as->region1 = NULL;

	as->vbase2 = 0;
	as->vtop2 = 0;
	as->region2 = NULL;

	as->stacktop = 0;
	as->stackbase = 0;
	as->stackregion = NULL;

	as->heaptop = 0;
	as->heapbase = 0;
	as->heapregion = NULL;

	as->as_lock = lock_create("as_lock");
	return as;
}

//for simplicity, when copy a page
//we find a new physical page for it and copy the old page to the new one
static void
pte_copy(struct pte *old, struct pte *new, struct addrspace *newas)
{
	new->vpagenum = old->vpagenum;

	// copy the page if old page is present initialize a new page otherwise
	if (old->present)
	{
		paddr_t physical_addr = 0;
		// copy memory blocks if old as is already present or read from memory otherwise
		if (old->present)
		{
			// get a new physical page
			physical_addr = get_single_ppage(new->vpagenum, newas);

			// evict a physical page if now space is left
			if (physical_addr == 0)
				physical_addr = swap_page(new->vpagenum, newas);
			new->ppagenum = physical_addr;

			// do the actual copy from old as to the new as
			memmove((void *)PADDR_TO_KVADDR(new->ppagenum), (const void *)PADDR_TO_KVADDR(old->ppagenum), PAGE_SIZE);

			// set up flags accordingly
			new->allocated = 1;
			new->present = 1;
			new->flag = old->flag;
			new->swap_disk_offset = 0;
		}
		else
		{
			KASSERT(old->present == 0);
			// get a new physical page
			physical_addr = get_single_ppage(new->vpagenum, newas);

			// evict a physical page if now space is left
			if (physical_addr == 0)
				physical_addr = swap_page(new->vpagenum, newas);

			new->ppagenum = physical_addr;
			lock_acquire(swap_disk_lock);
			struct iovec iov;
			struct uio ku;
			void *buf = (void *)PADDR_TO_KVADDR(new->ppagenum);
			uio_kinit(&iov, &ku, buf, PAGE_SIZE, (off_t)(old->swap_disk_offset * PAGE_SIZE), UIO_READ);
			int err = VOP_READ(swap_disk, &ku);
			lock_release(swap_disk_lock);
			if (err)
				panic("pte_copy: read from disk failed");

			// set up flags accordingly
			new->allocated = 1;
			new->present = 1;
			new->flag = old->flag;
			new->swap_disk_offset = 0;
		}
	}
	else
	{
		new->ppagenum = 0;
		new->allocated = 0;
		new->present = 0;
		new->flag = old->flag;
		new->swap_disk_offset = 0;
	}
}

/**
 * Destory a pagetable entry pte structure
 * which including freeing the physical page, the coremap entry
 * and if the page is on disk, also free the disk page
 */
static void destroy_pte(struct addrspace *as, struct pte *pte)
{
	// do nothing if the page table entry is not allocated
	if (!pte->allocated)
		return;

	// if the pte is present, wipe out the corresponding space in memory or simply unmark
	// the bitmap otherwise
	if (pte->present)
	{
		spinlock_acquire(coremap_lock);
		KASSERT(pte->ppagenum != 0);
		struct frame *f = coremap + COREMAP_INDEX(pte->ppagenum);
		KASSERT(f->used == 1);
		KASSERT(f->as == as);
		KASSERT(f->kernel == 0);
		KASSERT(f->vaddr == pte->vpagenum);
		KASSERT(f->size == 1);

		bzero((void *)PADDR_TO_KVADDR(pte->ppagenum), PAGE_SIZE);
		f->as = NULL;
		f->used = 0;
		f->kernel = 0;
		f->vaddr = 0;
		f->size = 0;
		spinlock_release(coremap_lock);
	}
	else
	{
		lock_acquire(swap_disk_lock);
		bitmap_unmark(swap_disk_bitmap, pte->swap_disk_offset);
		lock_release(swap_disk_lock);
	}

	// initialize fields accordingly for reuse
	pte->vpagenum = 0;
	pte->ppagenum = 0;
	pte->allocated = 0;
	pte->present = 0;
	pte->flag = 0;
	pte->swap_disk_offset = 0;
}

/**
 * intialize heap region as as_define_heap is called
 */
static void setHeapRegion(struct addrspace *as)
{
	for (unsigned int i = 0; i < VM_HEAPPAGES; ++i)
	{
		(as->heapregion)[i].vpagenum = as->heapbase + i * PAGE_SIZE;
		(as->heapregion)[i].ppagenum = 0;
		(as->heapregion)[i].allocated = 0;
		(as->heapregion)[i].present = 0;
		(as->heapregion)[i].flag = PF_W | PF_R;
		(as->heapregion)[i].swap_disk_offset = 0;
	}
}

/**
 * Define heap region
 */
int as_define_heap(struct addrspace *as, vaddr_t *heapptr)
{
	lock_acquire(as->as_lock);
	// set up the starting address of heapbase
	if (as->vtop1 > as->vtop2)
	{
		as->heapbase = as->vtop1;
	}
	else
	{
		as->heapbase = as->vtop2;
	}

	// allocate space for heap region
	as->heaptop = as->heapbase;
	as->heapregion = (struct pte *)kmalloc(VM_HEAPPAGES * sizeof(struct pte));

	setHeapRegion(as);
	*heapptr = as->heapbase;

	lock_release(as->as_lock);
	return 0;
}

/**
 * copy an address space
 * allocate a new address space to contain duplicate information as the old 
 * address space
 * @param:
 * 		old - contains the old address space to copy
 * 		ret - contains the new address space location on return 
 * @return:
 * 		return 0 to indicate success
 */
int as_copy(struct addrspace *old, struct addrspace **ret)
{
	struct addrspace *newas;
	newas = as_create();
	if (newas == NULL)
	{
		return ENOMEM;
	}

	newas->as_lock = lock_create("as_lock");

	newas->vbase1 = old->vbase1;
	newas->vtop1 = old->vtop1;
	size_t region1_size = (old->vtop1 - old->vbase1) / PAGE_SIZE;
	newas->region1 = (struct pte *)kmalloc(region1_size * sizeof(struct pte));

	newas->vbase2 = old->vbase2;
	newas->vtop2 = old->vtop2;
	size_t region2_size = (old->vtop2 - old->vbase2) / PAGE_SIZE;
	newas->region2 = (struct pte *)kmalloc(region2_size * sizeof(struct pte));

	//stack region
	newas->stacktop = old->stacktop;
	newas->stackbase = old->stackbase;
	size_t stack_size = (old->stacktop - old->stackbase) / PAGE_SIZE;
	newas->stackregion = (struct pte *)kmalloc(stack_size * (sizeof(struct pte)));

	//heap region
	newas->heaptop = old->heaptop;
	newas->heapbase = old->heapbase;
	size_t heap_size = 0;
	newas->heapregion = NULL;

	heap_size = (old->heaptop - old->heapbase) / PAGE_SIZE;
	newas->heapregion = (struct pte *)kmalloc(heap_size * sizeof(struct pte));

	lock_acquire(old->as_lock);
	for (unsigned int i = 0; i < region1_size; ++i)
	{
		pte_copy(old->region1 + i, newas->region1 + i, newas);
	}

	for (unsigned int i = 0; i < region2_size; ++i)
	{
		pte_copy(old->region2 + i, newas->region2 + i, newas);
	}

	for (unsigned int i = 0; i < stack_size; ++i)
	{
		pte_copy(old->stackregion + i, newas->stackregion + i, newas);
	}

	for (unsigned int i = 0; i < heap_size; ++i)
	{
		pte_copy(old->heapregion + i, newas->heapregion + i, newas);
	}

	lock_release(old->as_lock);
	*ret = newas;
	return 0;
}

/**
 * Clear the given address space 
 * @param:
 * 		as - address space memory location to deallocate
 */
void as_destroy(struct addrspace *as)
{
	lock_acquire(as->as_lock);

	// clear region 1
	int page_num = (as->vtop1 - as->vbase1) / PAGE_SIZE;
	// iterate over total page numbers and clear pte accordingly
	for (int i = 0; i < page_num; i++)
	{
		destroy_pte(as, as->region1 + i);
	}
	kprintf("clear region 1\n");
	kfree(as->region1);
	as->region1 = NULL;

	// clear region 2
	page_num = (as->vtop2 - as->vbase2) / PAGE_SIZE;
	for (int i = 0; i < page_num; i++)
	{
		destroy_pte(as, as->region2 + i);
	}
	kfree(as->region2);
	as->region2 = NULL;

	// clear stack region
	page_num = (as->stacktop - as->stackbase) / PAGE_SIZE;
	for (int i = 0; i < page_num; i++)
	{
		destroy_pte(as, as->stackregion + i);
	}
	kfree(as->stackregion);
	as->stackregion = NULL;

	// clear heap region
	page_num = (as->heaptop - as->heapbase) / PAGE_SIZE;
	for (int i = 0; i < page_num; i++)
	{
		destroy_pte(as, as->heapregion + i);
	}
	kfree(as->heapregion);
	as->heapregion = NULL;

	// turn off interrupts
	int spl = splhigh();
	for (int i = 0; i < NUM_TLB; ++i)
	{
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}
	// turn on interrupts
	splx(spl);

	lock_release(as->as_lock);
	lock_destroy(as->as_lock);
	as->as_lock = NULL;
	kfree(as);
}
/**
 * Flush the whole tlb
 */
static void flush_tlb(void)
{
	int spl = splhigh();
	for (unsigned int i = 0; i < NUM_TLB; i++)
	{
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}
	splx(spl);
}
/**
 * When context switch, flush the whole tlb
 */
void as_activate(void)
{

	flush_tlb();
}

void as_deactivate(void)
{

	flush_tlb();
}

/**
 * Just define the vaddr for each region but not allocate any physical page for it
 * only allocate ppage in vm fault
 */
int as_define_region(struct addrspace *as, vaddr_t vaddr, size_t sz,
					 int readable, int writeable, int executable)
{

	lock_acquire(as->as_lock);

	/* Align the region. First, the base... */
	sz += vaddr & ~(vaddr_t)PAGE_FRAME;
	vaddr &= PAGE_FRAME;

	/* compute the size */
	sz = (sz + PAGE_FRAME - 1) & PAGE_FRAME;

	size_t npages = sz / PAGE_SIZE;

	if (as->region1 == NULL)
	{
		as->vbase1 = vaddr;
		as->vtop1 = vaddr + npages * PAGE_SIZE;
		as->region1 = (struct pte *)kmalloc(npages * (sizeof(struct pte *)));

		for (unsigned int i = 0; i < npages; i++)
		{
			(as->region1)[i].vpagenum = vaddr + i * PAGE_FRAME;
			(as->region1)[i].ppagenum = 0;
			(as->region1)[i].allocated = 0;
			(as->region1)[i].present = 0;
			(as->region1)[i].flag = readable | writeable | executable;
			(as->region1)[i].swap_disk_offset = 0;
		}
		lock_release(as->as_lock);
		return 0;
	}
	else if (as->region2 == NULL)
	{
		as->vbase2 = vaddr;
		as->vtop2 = vaddr + npages * PAGE_SIZE;
		as->region2 = (struct pte *)kmalloc(npages * (sizeof(struct pte *)));

		for (unsigned int i = 0; i < npages; i++)
		{
			(as->region2)[i].vpagenum = vaddr + i * PAGE_FRAME;
			(as->region2)[i].ppagenum = 0;
			(as->region2)[i].allocated = 0;
			(as->region2)[i].present = 0;
			(as->region2)[i].flag = readable | writeable | executable;
			(as->region2)[i].swap_disk_offset = 0;
		}
		lock_release(as->as_lock);
		return 0;
	}
	panic("as_define_region: should not define more than 2 regions");
}

int as_prepare_load(struct addrspace *as)
{
	(void)as;
	return 0;
}

int as_complete_load(struct addrspace *as)
{
	(void)as;
	return 0;
}

int as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
	lock_acquire(as->as_lock);

	as->stacktop = USERSTACK;
	as->stackbase = USERSTACK - STACK_PAGES * PAGE_SIZE;
	KASSERT(as->stackbase != 0);
	// as->stackregion = (struct pte *)kmalloc(STACK_PAGES * sizeof(struct pte));

	// for (unsigned int i = 0; i < STACK_PAGES; ++i)
	// {
	// 	(as->stackregion)[i].vpagenum = as->stackbase + i * PAGE_SIZE;
	// 	(as->stackregion)[i].ppagenum = 0;
	// 	(as->stackregion)[i].allocated = 0;
	// 	(as->stackregion)[i].present = 0;
	// 	(as->stackregion)[i].flag = PF_W | PF_R;
	// 	(as->stackregion)[i].swap_disk_offset = 0;
	// }

	/* Initial user-level stack pointer */
	*stackptr = USERSTACK;
	lock_release(as->as_lock);

	return 0;
}
