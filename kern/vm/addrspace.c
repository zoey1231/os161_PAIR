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
#include <mips/tlb.h>

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

	as->vbase1 = 0;
	as->vtop1 = 0;
	as->region1 = NULL;

	as->vbase2 = 0;
	as->vtop2 = 0;
	as->region2 = NULL;

	as->stacktop = 0;
	as->stackbase = 0;
	as->stackregion = NULL;

	as->heapbase = 0;
	as->heaptop = 0;
	as->heapregion = NULL;

	as->as_lock = lock_create("l");

	return as;
}

int as_copy(struct addrspace *old, struct addrspace **ret)
{
	struct addrspace *newas;

	newas = as_create();
	if (newas == NULL)
	{
		return ENOMEM;
	}

	newas->as_lock = lock_create("l");

	newas->vbase1 = old->vbase1;
	newas->vtop1 = old->vtop1;
	size_t region1_sz = (old->vtop1 - old->vbase1) / PAGE_SIZE;
	newas->region1 = (struct pte *)kmalloc(region1_sz * sizeof(struct pte));

	newas->vbase2 = old->vbase2;
	newas->vtop2 = old->vtop2;
	size_t region2_sz = (old->vtop2 - old->vbase2) / PAGE_SIZE;
	newas->region2 = (struct pte *)kmalloc(region2_sz * sizeof(struct pte));

	newas->stacktop = old->stacktop;
	newas->stackbase = old->stackbase;
	size_t stack_sz = (old->stacktop - old->stackbase) / PAGE_SIZE;
	newas->stackregion = (struct pte *)kmalloc(stack_sz * sizeof(struct pte));

	//add heap support
	newas->heaptop = old->heaptop;
	newas->heapbase = old->heapbase;
	size_t heap_sz = 0;
	newas->heapregion = NULL;
	//if(old->heapregion != NULL){
	heap_sz = (old->heaptop - old->heapbase) / PAGE_SIZE;
	newas->heapregion = (struct pte *)kmalloc(heap_sz * sizeof(struct pte));
	//}

	lock_acquire(old->as_lock);
	for (unsigned int i = 0; i < region1_sz; ++i)
	{
		pte_copy(old->region1 + i, newas->region1 + i, newas);
	}

	for (unsigned int i = 0; i < region2_sz; ++i)
	{
		pte_copy(old->region2 + i, newas->region2 + i, newas);
	}

	//if(old->heapregion != NULL){
	for (unsigned int i = 0; i < stack_sz; ++i)
	{
		pte_copy(old->stackregion + i, newas->stackregion + i, newas);
	}
	//}
	for (unsigned int i = 0; i < heap_sz; ++i)
	{
		pte_copy(old->heapregion + i, newas->heapregion + i, newas);
	}

	lock_release(old->as_lock);
	*ret = newas;
	return 0;
}

void as_destroy(struct addrspace *as)
{
	lock_acquire(as->as_lock);
	for (unsigned int i = 0; i < (as->vtop1 - as->vbase1) / PAGE_SIZE; ++i)
	{
		destroy_pte(as, as->region1 + i);
	}
	//kprintf("clear region 1\n");
	kfree(as->region1);
	as->region1 = NULL;
	for (unsigned int i = 0; i < (as->vtop2 - as->vbase2) / PAGE_SIZE; ++i)
	{
		destroy_pte(as, as->region2 + i);
	}
	kfree(as->region2);
	as->region2 = NULL;
	for (unsigned int i = 0; i < (as->stacktop - as->stackbase) / PAGE_SIZE; ++i)
	{
		destroy_pte(as, as->stackregion + i);
	}
	kfree(as->stackregion);
	as->stackregion = NULL;

	//add heap support
	for (unsigned int i = 0; i < (as->heaptop - as->heapbase) / PAGE_SIZE; ++i)
	{
		destroy_pte(as, as->heapregion + i);
	}
	kfree(as->heapregion);
	as->stackregion = NULL;

	int spl = splhigh();
	for (int i = 0; i < NUM_TLB; ++i)
	{
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}
	splx(spl);

	lock_release(as->as_lock);
	lock_destroy(as->as_lock);
	as->as_lock = NULL;
	kfree(as);
}

void as_activate(void)
{
	struct addrspace *as;

	as = curproc_getas();
	if (as == NULL)
	{
		/*
		 * Kernel thread without an address space; leave the
		 * prior address space in place.
		 */
		return;
	}

	/*
	 * Write this.
	 */
	int spl = splhigh();

	uint32_t index;

	for (index = 0; index < NUM_TLB; index++)
	{
		tlb_write(TLBHI_INVALID(index), TLBLO_INVALID(), index);
	}

	splx(spl);
}

void as_deactivate(void)
{
	/*
	 * Write this. For many designs it won't need to actually do
	 * anything. See proc.c for an explanation of why it (might)
	 * be needed.
	 */

	int spl = splhigh();

	for (unsigned int i = 0; i < NUM_TLB; i++)
	{
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
}

/*
 * Set up a segment at virtual address VADDR of size MEMSIZE. The
 * segment in memory extends from VADDR up to (but not including)
 * VADDR+MEMSIZE.
 *
 * The READABLE, WRITEABLE, and EXECUTABLE flags are set if read,
 * write, or execute permission should be set on the segment. At the
 * moment, these are ignored. When you write the VM system, you may
 * want to implement them.
 */
int as_define_region(struct addrspace *as, vaddr_t vaddr, size_t sz,
					 int readable, int writeable, int executable)
{
	/*
	 * Write this.
	 */

	(void)as;
	(void)vaddr;
	(void)sz;
	(void)readable;
	(void)writeable;
	(void)executable;
	return EUNIMP;
}

int as_prepare_load(struct addrspace *as)
{
	/*
	 * Write this.
	 */

	(void)as;
	return 0;
}

int as_complete_load(struct addrspace *as)
{
	/*
	 * Write this.
	 */

	(void)as;
	return 0;
}

int as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
	/*
	 * Write this.
	 */

	lock_acquire(as->as_lock);
	as->stacktop = USERSTACK;
	as->stackbase = USERSTACK - STACK_PAGES * PAGE_SIZE;

	as->stackregion = (struct pte *)kmalloc(STACK_PAGES * sizeof(struct pte));

	for (unsigned int i = 0; i < STACK_PAGES; ++i)
	{
		(as->stackregion)[i].vpagenum = as->stackbase + i * PAGE_SIZE;
		(as->stackregion)[i].ppagenum = 0;
		(as->stackregion)[i].allocated = 0;
		(as->stackregion)[i].present = 0;
		(as->stackregion)[i].flag = PF_W | PF_R;
		(as->stackregion)[i].swap_disk_offset = 0;
	}
	*stackptr = USERSTACK;
	lock_release(as->as_lock);

	return 0;
}
