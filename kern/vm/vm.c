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
#include <spl.h>
#include <spinlock.h>
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <limits.h>
#include <vfs.h>
#include <kern/fcntl.h>
#include <elf.h>
#include <uio.h>
#include <vnode.h>
#include <kern/stat.h>
#include <cpu.h>
#include <array.h>
#include <mainbus.h>
#include <thread.h>
#include <threadlist.h>
#include <threadprivate.h>
#include "opt-synchprobs.h"
#include <wchan.h>
#include <signal.h>
#include <syscall.h>
/*
 * Wrap ram_stealmem in a spinlock.
 */
static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;
struct spinlock *coremap_lock;
int vm_initialized = 0;
struct frame *coremap;
paddr_t freepage;
struct vnode *swap_disk;
unsigned long total_frame_num;
struct bitmap *swap_disk_bitmap;
struct lock *swap_disk_lock;
/**
 * Set up the virtual memory during bootup
 */
void vm_bootstrap(void)
{

    coremap_lock = (struct spinlock *)kmalloc(sizeof(struct spinlock));
    spinlock_init(coremap_lock);

    // open swap disk
    char buf[10];
    strcpy(buf, "lhd0raw:");
    int err = vfs_open(buf, O_RDWR, 0664, &swap_disk);
    if (err)
    {
        panic("Can not open swap disk");
    }

    // get stat of and initialize swap disk
    struct stat s;
    VOP_STAT(swap_disk, &s);
    off_t swap_disk_npages = s.st_size / PAGE_SIZE;
    swap_disk_bitmap = bitmap_create(swap_disk_npages);
    KASSERT(swap_disk_bitmap != NULL);
    swap_disk_lock = lock_create("l");

    // determine the size of the ram
    paddr_t last = ram_getsize();
    // check that lastpaddr last is aligned
    KASSERT((last & PAGE_FRAME) == last);
    // calculate the total number of physical pages in the system based on lastpaddr last
    total_frame_num = last / PAGE_SIZE;

    //calculate coremap size
    size_t coremap_size = total_frame_num * sizeof(struct frame);
    size_t coremap_npages = (coremap_size + PAGE_SIZE - 1) / PAGE_SIZE;

    //kprintf("In vm_bootstrap,coremap_npages=%d", coremap_npages);

    // get physical memory for the coremap
    spinlock_acquire(&stealmem_lock);
    paddr_t coremap_paddr = ram_stealmem(coremap_npages);
    spinlock_release(&stealmem_lock);
    KASSERT(coremap_paddr != 0);

    //initialzie coremap
    coremap = (struct frame *)PADDR_TO_KVADDR(coremap_paddr);
    bzero(coremap, coremap_npages * PAGE_SIZE);

    freepage = ram_getfirstfree();
    bzero((void *)PADDR_TO_KVADDR(freepage), (last - freepage));
    KASSERT(freepage != 0);

    //initialize the addresses used by the coremap
    //mark all addresses before freepage as being used by the kernel(coremap)
    unsigned long i = 0;
    unsigned long freepageIndex = COREMAP_INDEX(freepage);
    for (; i < freepageIndex; ++i)
    {
        coremap[i].as = NULL;
        coremap[i].used = 1;
        coremap[i].kernel = 1;
        coremap[i].vaddr = PADDR_TO_KVADDR(i * PAGE_SIZE);
        coremap[i].size = 1;
    }

    //initialize the addresses free for users to use
    for (; i < total_frame_num; ++i)
    {
        coremap[i].as = NULL;
        coremap[i].used = 0;
        coremap[i].kernel = 0;
        coremap[i].vaddr = 0;
        coremap[i].size = 0;
    }

    //signal others that vm has finished initialization
    vm_initialized = 1;

    // flush the tlb
    int spl = splhigh();
    for (int i = 0; i < NUM_TLB; i++)
    {
        tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
    }
    splx(spl);
}

/**
 * Get contiguous physical pages for kernel to use 
 */
static paddr_t get_contiguous_ppages(unsigned long npages)
{

    KASSERT(npages != 0);
    //kprintf("Enter get_contiguous_ppages\n");
    unsigned long leftToFind = npages;

    int first_index = 0;

    //loop through each page to find required continuous pages
    for (unsigned long i = 0; i < total_frame_num; i++)
    {
        //break the loop once we found enough pages
        if (leftToFind == 0)
            break;
        else if (coremap[i].used == 0)
            leftToFind--;
        //reach the end but didn't find enough pages
        else if (i == total_frame_num - 1)
            break;
        else
        {
            leftToFind = npages;
            first_index = i + 1;
        }
    }

    // successfully find required number of contiguous pages
    if (leftToFind == 0)
    {
        //prepare the physical pages before return
        bzero((void *)PADDR_TO_KVADDR(first_index * PAGE_SIZE), PAGE_SIZE * npages);

        for (unsigned long i = 0; i < npages; i++)
        {
            KASSERT((first_index + i) < total_frame_num);
            KASSERT(coremap[first_index + i].used == 0);

            //mark the page in the coremap
            coremap[first_index + i].as = NULL;
            coremap[first_index + i].used = 1;
            coremap[first_index + i].kernel = 1;
            coremap[first_index + i].vaddr = PADDR_TO_KVADDR((first_index + i) * PAGE_SIZE);

            //record the size the contiguous blocks only at the starting address
            if (i == 0)
                coremap[first_index + i].size = (size_t)npages;
            else
                coremap[first_index + i].size = 0;
        }
    }
    else
    {
        panic("get_contiguous_ppages: no enough ram! \n");
    }

    return (paddr_t)(first_index * PAGE_SIZE);
}

/**
 * Evict a page in ram and allocate the new vaddr and addrspace to that page
 * This function is called when user program needs a physical page 
 * for newvaddr and newas when the ram is full
 */
paddr_t swap_page(vaddr_t newvaddr, struct addrspace *newas)
{

    static unsigned int start_idx = 0;

    //loop through each page
    for (unsigned int j = 0; j < total_frame_num; j++)
    {
        unsigned int i = (start_idx + j) % total_frame_num;

        //find a page is not used by kernel
        if (coremap[i].kernel == 0)
        {
            //check that the page we want to evict is used
            KASSERT(coremap[i].used == 1);
            KASSERT(coremap[i].as != NULL);
            KASSERT(coremap[i].vaddr != 0);
            KASSERT(coremap[i].size == 1);

            struct addrspace *as = coremap[i].as;
            struct pte *pte = NULL;
            vaddr_t vaddr = coremap[i].vaddr;

            //write the evicted page to disk and update its addrspace
            if (vaddr >= as->vbase1 && vaddr < as->vtop1)
            {
                pte = as->region1 + (vaddr - as->vbase1) / PAGE_SIZE;
            }
            else if (vaddr >= as->vbase2 && vaddr < as->vtop2)
            {
                pte = as->region2 + (vaddr - as->vbase2) / PAGE_SIZE;
            }
            else if (vaddr >= as->stackbase && vaddr < as->stacktop)
            {
                pte = as->stackregion + (vaddr - as->stackbase) / PAGE_SIZE;
            }
            else if (vaddr >= as->heapbase && vaddr < as->heaptop)
            {
                pte = as->heapregion + (vaddr - as->heapbase) / PAGE_SIZE;
            }
            else
            {
                panic("swap_page: frame's vaddr is not in the valid region ");
            }

            //check the page is allocated and is present in ram
            KASSERT(pte != NULL);
            KASSERT(pte->ppagenum == i * PAGE_SIZE);
            KASSERT(pte->allocated == 1);
            KASSERT(pte->present == 1);

            void *buf = (void *)PADDR_TO_KVADDR(i * PAGE_SIZE);

            lock_acquire(swap_disk_lock);
            unsigned swap_disk_index;
            int err = bitmap_alloc(swap_disk_bitmap, &swap_disk_index);
            lock_release(swap_disk_lock);
            if (err)
            {
                panic("swap_page: no enough ram");
            }

            //write the evicted page to disk
            struct iovec iov;
            struct uio ku;
            uio_kinit(&iov, &ku, buf, PAGE_SIZE, (off_t)(swap_disk_index * PAGE_SIZE), UIO_WRITE);
            err = VOP_WRITE(swap_disk, &ku);
            if (err)
            {
                panic("swap_page: cannot write to swap disk");
            }

            //change the evicted page's state in pagetable entry to not present on ram
            pte->ppagenum = 0;
            pte->present = 0;
            //record the swap_disk_index in swap_disk_offset
            pte->swap_disk_offset = swap_disk_index;

            //allocate the physical page to the new vaddr and addrspace
            bzero((void *)PADDR_TO_KVADDR(i * PAGE_SIZE), PAGE_SIZE);
            coremap[i].as = newas;
            coremap[i].used = 1;
            coremap[i].kernel = 0;
            coremap[i].vaddr = newvaddr;
            coremap[i].size = 1;

            uint32_t ehi, elo;
            int spl = splhigh();
            for (int j = 0; i < NUM_TLB; j++)
            {
                tlb_read(&ehi, &elo, j);
                if ((ehi & TLBHI_VPAGE) == vaddr)
                {
                    tlb_write(TLBHI_INVALID(j), TLBLO_INVALID(), j);
                }
            }
            splx(spl);

            start_idx = (i + 1) % total_frame_num;
            return (paddr_t)(i * PAGE_SIZE);
        }
    }

    panic("swap_page: no user page in physical memory");
}

/**
 * Helper function of alloc_kpages()
 * Get required number of physical pages 
 */
static paddr_t getppages(unsigned long npages)
{
    paddr_t paddr;

    if (!vm_initialized)
    {
        spinlock_acquire(&stealmem_lock);
        paddr = ram_stealmem(npages);
        spinlock_release(&stealmem_lock);
    }
    else
    {
        spinlock_acquire(coremap_lock);
        paddr = get_contiguous_ppages(npages);
        spinlock_release(coremap_lock);
    }
    KASSERT(paddr != 0);
    return paddr;
}

/**
 * Move a page on disk to ram
 */
static void movePageDiskToRam(paddr_t paddr, off_t swap_disk_offset)
{
    KASSERT((paddr & PAGE_FRAME) == paddr);

    lock_acquire(swap_disk_lock);
    struct iovec iov;
    struct uio ku;
    void *buf = (void *)PADDR_TO_KVADDR(paddr);
    uio_kinit(&iov, &ku, buf, PAGE_SIZE, (off_t)(swap_disk_offset * PAGE_SIZE), UIO_READ);
    int err = VOP_READ(swap_disk, &ku);
    if (err)
        panic("movePageDiskToRam: read from disk failed");

    bitmap_unmark(swap_disk_bitmap, swap_disk_offset);
    lock_release(swap_disk_lock);
}
/**
 * Allocate npages physical pages for kernel space
 */
vaddr_t alloc_kpages(unsigned npages)
{
    paddr_t pa = 0;
    pa = getppages(npages);
    if (pa == 0)
    {
        panic("alloc_kpages: cannot get paddr");
    }
    return PADDR_TO_KVADDR(pa);
}
/**
 * Free allocated kernel pages
 */
void free_kpages(vaddr_t vaddr)
{
    KASSERT(vaddr >= MIPS_KSEG0);

    paddr_t paddr = vaddr - MIPS_KSEG0;
    unsigned int index = COREMAP_INDEX(paddr);

    spinlock_acquire(coremap_lock);

    KASSERT(coremap[index].kernel == 1);
    KASSERT(coremap[index].as == NULL);
    KASSERT(coremap[index].vaddr == vaddr);
    KASSERT(coremap[index].size != 0);

    //get the contiguous pages size from the starting address
    size_t size = coremap[index].size;

    //free each allocated page one by one
    for (unsigned int i = index; i < index + size; ++i)
    {
        KASSERT(coremap[i].kernel == 1);
        KASSERT(coremap[i].as == NULL);
        KASSERT(coremap[i].vaddr == i * PAGE_SIZE + MIPS_KSEG0);
        KASSERT(coremap[i].used == 1);

        coremap[i].used = 0;
        coremap[i].kernel = 0;
        coremap[i].vaddr = 0;
        coremap[i].size = 0;
    }

    bzero((void *)vaddr, PAGE_SIZE * size);
    spinlock_release(coremap_lock);
}

/**
 * Nothing to do in vm_tlbshootdown_all since we have flushed the whole tlb on
 * context switch
 */
void vm_tlbshootdown_all(void)
{
}

/**
 * Nothing to do in vm_tlbshootdown since we have flushed the whole tlb on
 * context switch
 */
void vm_tlbshootdown(const struct tlbshootdown *ts)
{
    (void)ts;
}
/**
 * Get a single physical page for user program to use
 */
paddr_t get_single_ppage(vaddr_t vaddr, struct addrspace *as)
{

    KASSERT((vaddr & PAGE_FRAME) == vaddr);
    spinlock_acquire(coremap_lock);
    //loop through each page
    for (unsigned int i = 0; i < total_frame_num; i++)
    {
        //once we found a page that avaliable for user program to use, mark it
        if (coremap[i].used == 0 &&
            coremap[i].as == NULL &&
            coremap[i].kernel == 0 &&
            coremap[i].vaddr == 0 &&
            coremap[i].size == 0)
        {
            bzero((void *)PADDR_TO_KVADDR(i * PAGE_SIZE), PAGE_SIZE);

            coremap[i].as = as;
            coremap[i].used = 1;
            coremap[i].kernel = 0;
            coremap[i].vaddr = vaddr;
            coremap[i].size = 1;

            spinlock_release(coremap_lock);
            return (paddr_t)(i * PAGE_SIZE);
        }
    }
    spinlock_release(coremap_lock);

    //if cannot find one in ram, return 0 and then call swap_page
    return 0;
}
/**
 * If tlb still has spaces, write the new translation entry into tlb directly
 * If tlb is full, flush the whole tlb
 */
static int tlb_evict(uint32_t ehi, uint32_t elo)
{
    uint32_t entry_ehi, entry_elo;

    // turn off interrupt before manipulating the tlb
    int spl = splhigh();
    for (unsigned int i = 0; i < NUM_TLB; i++)
    {
        tlb_read(&entry_ehi, &entry_elo, i);

        //If tlb still has spaces, write to the empty spot
        if (!(entry_elo & TLBLO_VALID))
        {
            tlb_write(ehi, elo, i);
            // turn on interrupt before returning
            splx(spl);
            return 0;
        }
    }

    // if the tlb is full, flush the whole tlb
    for (unsigned int i = 0; i < NUM_TLB; i++)
    {
        tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
    }

    // write the new entry to the first spot
    tlb_write(ehi, elo, 0);

    // turn on interrupt before returning
    splx(spl);
    return 0;
}

/*
	* handle virtual machine fault

	@param:
		faulttype - the type of vm_fault: VM_FAULT_READONLY, VM_FAULT_READ, or VM_FAULT_WRITE
		faultaddress - the memory location address of where the fault happens
	@return:
		return 0 to indicate success
*/
int vm_fault(int faulttype, vaddr_t faultaddress)
{

    faultaddress &= PAGE_FRAME;

    // switch (faulttype)
    // {
    // case VM_FAULT_READONLY:

    // case VM_FAULT_READ:
    //     break;
    // case VM_FAULT_WRITE:
    //     break;
    // default:
    //     return EINVAL;
    // }

    // return invalid faulty type if the fault type if not one of the 3 mentioned above
    if (faulttype != VM_FAULT_READONLY && faulttype != VM_FAULT_READ && faulttype != VM_FAULT_WRITE)
    {
        return EINVAL;
    }

    if (curproc == NULL)
    {
        /*
		 * No process. This is probably a kernel fault early
		 * in boot. Return EFAULT so as to panic instead of
		 * getting into an infinite faulting loop.
		 */
        return EFAULT;
    }

    struct addrspace *as = proc_getas();
    if (as == NULL)
    {
        /*
		 * No address space set up. This is probably also a
		 * kernel fault early in boot.
		 */
        return EFAULT;
    }

    /* Assert that the address space has been set up properly. */
    // KASSERT(as->vbase1 != 0);
    // KASSERT(as->vtop1 != 0);
    // KASSERT(as->vbase2 != 0);
    // KASSERT(as->vtop2 != 0);
    // KASSERT(as->stackbase != 0);
    // KASSERT(as->stacktop != 0);
    // KASSERT(as->heapbase != 0);
    // KASSERT(as->heaptop != 0);
    // KASSERT((as->vbase1 & PAGE_FRAME) == as->vbase1);
    // KASSERT((as->vtop1 & PAGE_FRAME) == as->vtop1);
    // KASSERT((as->vbase2 & PAGE_FRAME) == as->vbase2);
    // KASSERT((as->vtop2 & PAGE_FRAME) == as->vtop2);
    // KASSERT((as->stackbase & PAGE_FRAME) == as->stackbase);
    // KASSERT((as->stacktop & PAGE_FRAME) == as->stacktop);
    // KASSERT((as->heapbase & PAGE_FRAME) == as->heapbase);
    // KASSERT((as->heaptop & PAGE_FRAME) == as->heaptop);

    //locate the page entry
    struct pte *pte = NULL;
    if (faultaddress >= as->vbase1 && faultaddress < as->vtop1)
    {
        pte = as->region1 + (faultaddress - as->vbase1) / PAGE_SIZE;
    }
    else if (faultaddress >= as->vbase2 && faultaddress < as->vtop2)
    {
        pte = as->region2 + (faultaddress - as->vbase2) / PAGE_SIZE;
    }
    else if (faultaddress >= as->stackbase && faultaddress < as->stacktop)
    {
        pte = as->stackregion + (faultaddress - as->stackbase) / PAGE_SIZE;
    }
    else if (faultaddress >= as->heapbase && faultaddress < as->heaptop)
    {
        pte = as->heapregion + (faultaddress - as->heapbase) / PAGE_SIZE;
    }
    else
    {
        kExit(SIGSEGV);
    }

    KASSERT(pte != NULL);
    /* make sure it's page-aligned */
    KASSERT((pte->vpagenum & PAGE_FRAME) == faultaddress);

    uint32_t ehi, elo;
    //if the vaddr is just defined, it won't be in ram or disk, we need to find a physical page for it first
    //and then write it to tlb
    if (pte->allocated == 0)
    {
        KASSERT(pte->present == 0);
        paddr_t physical_address = 0;
        // get a physical page
        physical_address = get_single_ppage(faultaddress, as);
        if (physical_address == 0)
        {
            physical_address = swap_page(faultaddress, as);
        }
        ehi = faultaddress;
        elo = physical_address | TLBLO_DIRTY | TLBLO_VALID;
        tlb_evict(ehi, elo);

        pte->allocated = 1;
        pte->ppagenum = physical_address;
        pte->present = 1;
        return 0;
    }
    //the physical page is in ram but not in tlb, just update the tlb
    else if (pte->present == 1)
    {
        KASSERT(pte->vpagenum == faultaddress);
        KASSERT(pte->ppagenum != 0);
        ehi = faultaddress;
        elo = pte->ppagenum | TLBLO_DIRTY | TLBLO_VALID;
        tlb_evict(ehi, elo);
        return 0;
    }

    //the physical page is on disk
    //we need to find a physical page for it
    //can copy it back from disk
    else
    {
        KASSERT(pte->allocated == 1 && pte->present == 0);
        KASSERT(pte->ppagenum == 0);
        //on disk
        paddr_t physical_addresss = 0;
        physical_addresss = get_single_ppage(pte->vpagenum, as);
        if (physical_addresss == 0)
        {
            physical_addresss = swap_page(pte->vpagenum, as);
        }
        movePageDiskToRam(physical_addresss, pte->swap_disk_offset);

        pte->ppagenum = physical_addresss;
        pte->present = 1;

        ehi = pte->vpagenum;
        elo = pte->ppagenum | TLBLO_DIRTY | TLBLO_VALID;
        tlb_evict(ehi, elo);
        return 0;
    }
}
