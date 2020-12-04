/*
 * Copyright (c) 2013
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

/*
 * Process support.
 *
 * There is (intentionally) not much here; you will need to add stuff
 * and maybe change around what's already present.
 *
 * p_lock is intended to be held when manipulating the pointers in the
 * proc structure, not while doing any significant work with the
 * things they point to. Rearrange this (and/or change it to be a
 * regular lock) as needed.
 *
 * Unless you're implementing multithreaded user processes, the only
 * process that will have more than one thread is the kernel process.
 */

#include <types.h>
#include <spl.h>
#include <proc.h>
#include <current.h>
#include <addrspace.h>
#include <vnode.h>
#include <filetable.h>
#include <array.h>
#include <limits.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <kern/types.h>
#include <vfs.h>
#include <syscall.h>

/*
 * The process for the kernel; this holds all the kernel-only threads.
 */
struct proc *kproc;

/**
 * Helper function for pidtable_init() and pidtable_add(). 
 * Initialized given process's pid information
 */
static int pidInfo_init(struct proc *proc)
{
	proc->exit_code = (int)NULL;
	proc->proc_state = RUNNING;

	proc->pid_cv = cv_create("pid_cv");
	proc->pid_lock = lock_create("pid_lock");
	proc->children = array_create();
	if (proc->children == NULL)
	{
		return ENOMEM;
	}
	return 0;
}

/**
 * Helper function for proc_destroy.
 * clear proc's associated pid information
 */
static void pidInfo_destory(struct proc *proc)
{
	cv_destroy(proc->pid_cv);

	KASSERT(!lock_do_i_hold(proc->pid_lock));
	lock_destroy(proc->pid_lock);

	//destory children array
	int child_num = array_num(proc->children);
	for (int i = 0; i < child_num; i++)
	{
		array_remove(proc->children, 0);
	}
	KASSERT(array_num(proc->children) == 0);
	array_destroy(proc->children);
}

/**
 * Reset a specific pidtable entry with index pid
 */
void reset_pidtable_entry(pid_t pid)
{
	KASSERT(pid >= PID_MIN && pid <= PID_MAX);
	if (bitmap_isset(pidtable.occupied, pid))
	{
		bitmap_unmark(pidtable.occupied, pid);
	}
	array_set(pidtable.pid_procs, pid, NULL);
	pidtable.pid_available++;
}
// void reset_pidtable_entry(pid_t pid)
// {
// 	KASSERT(pid >= PID_MIN && pid <= PID_MAX);
// 	pidtable.pid_procs[pid] = NULL;
// 	pidtable.pid_available++;
// 	pidtable.occupied[pid] = FREE;
// }

/*
 * Create a proc structure.
 */
static struct proc *proc_create(const char *name)
{
	struct proc *proc;
	int ret;

	proc = kmalloc(sizeof(*proc));
	if (proc == NULL)
	{
		return NULL;
	}
	proc->p_name = kstrdup(name);
	if (proc->p_name == NULL)
	{
		kfree(proc);
		return NULL;
	}

	//proc->p_fdArray = fdArray_create();
	// if (proc->p_fdArray == NULL)
	// {
	// 	kfree(proc->p_name);
	// 	kfree(proc);
	// 	return NULL;
	// }

	threadarray_init(&proc->p_threads);
	spinlock_init(&proc->p_lock);

	/* VM fields */
	proc->p_addrspace = NULL;

	/* VFS fields */
	proc->p_cwd = NULL;

	//initialize the process's fileDescriptor array

	ret = fdArray_create(proc);
	if (ret)
	{
		kfree(proc->p_name);
		kfree(proc);
		return NULL;
	}
	ret = pidInfo_init(proc);
	if (ret)
	{
		kfree(proc->p_name);
		kfree(proc);
		return NULL;
	}
	return proc;
}

/*
 * Destroy a proc structure.
 *
 * Note: nothing currently calls this. Your wait/exit code will
 * probably want to do so.
 */
void proc_destroy(struct proc *proc)
{
	/*
	 * You probably want to destroy and null out much of the
	 * process (particularly the address space) at exit time if
	 * your wait/exit design calls for the process structure to
	 * hang around beyond process exit. Some wait/exit designs
	 * do, some don't.
	 */

	KASSERT(proc != NULL);
	KASSERT(proc != kproc);

	/*
	 * We don't take p_lock in here because we must have the only
	 * reference to this structure. (Otherwise it would be
	 * incorrect to destroy it.)
	 */

	/* VFS fields */
	if (proc->p_cwd)
	{
		VOP_DECREF(proc->p_cwd);
		proc->p_cwd = NULL;
	}

	/* VM fields */
	if (proc->p_addrspace)
	{
		/*
		 * If p is the current process, remove it safely from
		 * p_addrspace before destroying it. This makes sure
		 * we don't try to activate the address space while
		 * it's being destroyed.
		 *
		 * Also explicitly deactivate, because setting the
		 * address space to NULL won't necessarily do that.
		 *
		 * (When the address space is NULL, it means the
		 * process is kernel-only; in that case it is normally
		 * ok if the MMU and MMU- related data structures
		 * still refer to the address space of the last
		 * process that had one. Then you save work if that
		 * process is the next one to run, which isn't
		 * uncommon. However, here we're going to destroy the
		 * address space, so we need to make sure that nothing
		 * in the VM system still refers to it.)
		 *
		 * The call to as_deactivate() must come after we
		 * clear the address space, or a timer interrupt might
		 * reactivate the old address space again behind our
		 * back.
		 *
		 * If p is not the current process, still remove it
		 * from p_addrspace before destroying it as a
		 * precaution. Note that if p is not the current
		 * process, in order to be here p must either have
		 * never run (e.g. cleaning up after fork failed) or
		 * have finished running and exited. It is quite
		 * incorrect to destroy the proc structure of some
		 * random other process while it's still running...
		 */
		struct addrspace *as;

		if (proc == curproc)
		{
			as = proc_setas(NULL);
			as_deactivate();
		}
		else
		{
			as = proc->p_addrspace;
			proc->p_addrspace = NULL;
		}
		if ((vaddr_t)as % PAGE_SIZE == 0)
			as_destroy(as);
	}

	if (proc->pid_cv != NULL && proc->pid_lock != NULL)
		pidInfo_destory(proc);

	/* filetable fields */
	if (proc->fdArray != NULL)
	{
		fdArray_destory(proc->fdArray);

		lock_destroy(proc->fda_lock);
	}

	threadarray_cleanup(&proc->p_threads);
	spinlock_cleanup(&proc->p_lock);

	kfree(proc->p_name);
	kfree(proc);
}

/*
 * Create the process structure for the kernel.
 */
void proc_bootstrap(void)
{
	kproc = proc_create("[kernel]");
	if (kproc == NULL)
	{
		panic("proc_create for kproc failed\n");
	}
}

/*
 * Create a fresh proc for use by runprogram.
 *
 * It will have no address space and will inherit the current
 * process's (that is, the kernel menu's) current directory.
 */
struct proc *
proc_create_runprogram(const char *name)
{
	struct proc *newproc;
	int ret;

	newproc = proc_create(name);
	if (newproc == NULL)
	{
		return NULL;
	}

	/* VM fields */

	newproc->p_addrspace = NULL;

	/* VFS fields */

	/*
	 * Lock the current process to copy its current directory.
	 * (We don't need to lock the new process, though, as we have
	 * the only reference to it.)
	 */
	spinlock_acquire(&curproc->p_lock);
	if (curproc->p_cwd != NULL)
	{
		VOP_INCREF(curproc->p_cwd);
		newproc->p_cwd = curproc->p_cwd;
	}
	spinlock_release(&curproc->p_lock);

	/* filetable fields*/
	ret = proc_filetable_init(newproc);
	if (ret)
	{
		return NULL;
	}
	/*pid fields*/
	ret = pidtable_add(newproc, &newproc->pid);
	if (ret)
	{
		fdArray_destory(newproc->fdArray);

		lock_destroy(newproc->fda_lock);
		kfree(newproc);
		return NULL;
	}
	return newproc;
}

/*
 * Add a thread to a process. Either the thread or the process might
 * or might not be current.
 *
 * Turn off interrupts on the local cpu while changing t_proc, in
 * case it's current, to protect against the as_activate call in
 * the timer interrupt context switch, and any other implicit uses
 * of "curproc".
 */
int proc_addthread(struct proc *proc, struct thread *t)
{
	int result;
	int spl;

	KASSERT(t->t_proc == NULL);

	spinlock_acquire(&proc->p_lock);
	result = threadarray_add(&proc->p_threads, t, NULL);
	spinlock_release(&proc->p_lock);
	if (result)
	{
		return result;
	}
	spl = splhigh();
	t->t_proc = proc;
	splx(spl);
	return 0;
}

/*
 * Remove a thread from its process. Either the thread or the process
 * might or might not be current.
 *
 * Turn off interrupts on the local cpu while changing t_proc, in
 * case it's current, to protect against the as_activate call in
 * the timer interrupt context switch, and any other implicit uses
 * of "curproc".
 */
void proc_remthread(struct thread *t)
{
	struct proc *proc;
	unsigned i, num;
	int spl;

	proc = t->t_proc;
	KASSERT(proc != NULL);

	spinlock_acquire(&proc->p_lock);
	/* ugh: find the thread in the array */
	num = threadarray_num(&proc->p_threads);
	for (i = 0; i < num; i++)
	{
		if (threadarray_get(&proc->p_threads, i) == t)
		{
			threadarray_remove(&proc->p_threads, i);
			spinlock_release(&proc->p_lock);
			spl = splhigh();
			t->t_proc = NULL;
			splx(spl);
			return;
		}
	}
	/* Did not find it. */
	spinlock_release(&proc->p_lock);
	panic("Thread (%p) has escaped from its process (%p)\n", t, proc);
}

/*
 * Fetch the address space of (the current) process.
 *
 * Caution: address spaces aren't refcounted. If you implement
 * multithreaded processes, make sure to set up a refcount scheme or
 * some other method to make this safe. Otherwise the returned address
 * space might disappear under you.
 */
struct addrspace *
proc_getas(void)
{
	struct addrspace *as;
	struct proc *proc = curproc;

	if (proc == NULL)
	{
		return NULL;
	}

	spinlock_acquire(&proc->p_lock);
	as = proc->p_addrspace;
	spinlock_release(&proc->p_lock);
	return as;
}

/*
 * Change the address space of (the current) process. Return the old
 * one for later restoration or disposal.
 */
struct addrspace *
proc_setas(struct addrspace *newas)
{
	struct addrspace *oldas;
	struct proc *proc = curproc;

	KASSERT(proc != NULL);

	spinlock_acquire(&proc->p_lock);
	oldas = proc->p_addrspace;
	proc->p_addrspace = newas;
	spinlock_release(&proc->p_lock);
	return oldas;
}

/**
 * Helper functin for sys_fork()
 * create a new process 
 */
struct proc *create_fork_proc(const char *name)
{
	return proc_create(name);
}
/**
 * Initilize the filetable for a process. Assign the first 3 entries to stdin, stdout, and stderr respectively.
*/
int proc_filetable_init(struct proc *proc)
{
	struct file *f;
	struct fd_entry *fe;
	int ret;
	char *cons = NULL;

	struct vnode *stdin_vn, *stdout_vn, *stderr_vn;

	cons = kstrdup("con:");
	ret = vfs_open(cons, O_RDONLY, 0, &stdin_vn);
	if (ret)
	{
		fdArray_destory(proc->fdArray);

		lock_destroy(proc->fda_lock);
		return ret;
	}

	f = kmalloc(sizeof(struct file));
	f->file_lock = lock_create("file_lock");
	f->offset = 0;
	f->refcount = 1;
	f->status = O_RDONLY;
	f->valid = 1;
	f->vn = stdin_vn;

	fe = kmalloc(sizeof(struct fd_entry));
	fe->fd = 0;
	fe->file = f;
	filetable_add(&filetable, f);
	array_add(proc->fdArray, fe, NULL);

	cons = kstrdup("con:");
	ret = vfs_open(cons, O_WRONLY, 0, &stdout_vn);
	if (ret)
	{
		fdArray_destory(proc->fdArray);

		lock_destroy(proc->fda_lock);
		return ret;
	}
	f = kmalloc(sizeof(struct file));
	f->file_lock = lock_create("file_lock");
	f->offset = 0;
	f->refcount = 1;
	f->status = O_WRONLY;
	f->valid = 1;
	f->vn = stdout_vn;

	fe = kmalloc(sizeof(struct fd_entry));
	fe->fd = 1;
	fe->file = f;
	filetable_add(&filetable, f);
	array_add(proc->fdArray, fe, NULL);

	cons = kstrdup("con:");
	ret = vfs_open(cons, O_WRONLY, 0, &stderr_vn);
	if (ret)
	{
		fdArray_destory(proc->fdArray);

		lock_destroy(proc->fda_lock);
		return ret;
	}
	f = kmalloc(sizeof(struct file));
	f->file_lock = lock_create("file_lock");
	f->offset = 0;
	f->refcount = 1;
	f->status = O_WRONLY;
	f->valid = 1;
	f->vn = stderr_vn;

	fe = kmalloc(sizeof(struct fd_entry));
	fe->fd = 2;
	fe->file = f;
	filetable_add(&filetable, f);
	array_add(proc->fdArray, fe, NULL);

	return 0;
}

/**
 * initialize the pidtable, usd in kmain()
 */
void pidtable_init(struct pidtable *pidtable)
{
	int ret;
	// allocate space for pt_lock
	pidtable->pt_lock = lock_create("pt_lock");

	//set one avaliable spot for kproc to use
	pidtable->pid_available = 1;

	// Skip pid=0 since it has special meaning.
	pidtable->pid_next = PID_MIN;

	// add the kernel process to the table
	ret = pidInfo_init(kproc);
	if (ret)
	{
		panic("Unable to initialize pidtable");
	}
	pidtable->pid_procs = array_create();
	array_init(pidtable->pid_procs);
	array_setsize(pidtable->pid_procs, PID_MAX + 1);

	array_set(pidtable->pid_procs, pidtable->pid_next, kproc);

	pidtable->pid_available--;

	pidtable->occupied = bitmap_create(PID_MAX + 1);
	//mark the slots before PID_MIN as occupied
	for (unsigned index = 0; index < PID_MIN; ++index)
		bitmap_mark(pidtable->occupied, index);
	/* Initalize other spaces in the filetable  */
	for (int i = PID_MIN + 1; i < PID_MAX; i++)
	{
		reset_pidtable_entry(i);
	}
}
// void pidtable_init(struct pidtable *pidtable)
// {
// 	int ret;
// 	// allocate space for pidtable
// 	pidtable = kmalloc(sizeof(struct pidtable));
// 	if (pidtable == NULL)
// 	{
// 		panic("Unable to initialize pidtable");
// 	}

// 	// allocate space for pt_lock
// 	pidtable->pt_lock = lock_create("pt_lock");

// 	//set one avaliable spot for kproc to use
// 	pidtable->pid_available = 1;

// 	// Skip pid=0 since it has special meaning.
// 	pidtable->pid_next = PID_MIN;

// 	// add the kernel process to the table
// 	ret = pidInfo_init(kproc);
// 	if (ret)
// 	{
// 		panic("Unable to initialize pidtable");
// 	}
// 	pidtable->pid_procs[pidtable->pid_next] = kproc;
// 	pidtable->pid_available--;

// 	/* Initalize other spaces in the filetable  */
// 	for (int i = PID_MIN + 1; i < PID_MAX; i++)
// 	{
// 		reset_pidtable_entry(i);
// 	}
// }

/**
 * Assign a pid to a given process proc and add the process to pidtable
 * Return the assigned pid in retval.
 * On success,return 0.
 * On error, the corresponding error code is returned.
 */
int pidtable_add(struct proc *proc, int32_t *retval)
{
	pid_t next_pid;
	KASSERT(proc != NULL);

	lock_acquire(pidtable.pt_lock);

	//check pidtable's avilibility
	if (pidtable.pid_available < 1)
	{
		lock_release(pidtable.pt_lock);
		return ENPROC;
	}

	//add proc to its parent's children process array
	array_add(curproc->children, proc, NULL);

	//assign the next avaliable spot in pidtable to proc as its pid and return pid
	next_pid = pidtable.pid_next;
	*retval = next_pid;

	//pidtable.pid_procs[next_pid] = proc;
	//pidtable.occupied[next_pid] = OCCUPIED;
	array_set(pidtable.pid_procs, next_pid, proc);
	KASSERT(!bitmap_isset(pidtable.occupied, next_pid));
	bitmap_mark(pidtable.occupied, next_pid);

	pidtable.pid_available--;

	// iterate through avaliable spots to set pid_next to the smallest avaliable number in pidtable
	if (pidtable.pid_available > 0)
	{
		for (int i = next_pid; i < PID_MAX; ++i)
		{
			if (!bitmap_isset(pidtable.occupied, i))
			{
				pidtable.pid_next = i;
				break;
			}
			// if (pidtable.occupied[i] == FREE)
			// {
			// 	pidtable.pid_next = i;
			// 	break;
			// }
		}
	}

	lock_release(pidtable.pt_lock);
	return 0;
}
/**
 * Remove a specific entry pid from the pidtable
 */
void pidtable_remove(pid_t pid)
{
	KASSERT(pid >= PID_MIN && pid <= PID_MAX);
	lock_acquire(pidtable.pt_lock);
	reset_pidtable_entry(pid);
	lock_release(pidtable.pt_lock);
}

/**
 * Collect the status of each child program of proc, 
 * clear the child program or update the child program's status accordingly
 */
void updateChildState(struct proc *proc)
{
	KASSERT(proc != NULL);
	int child_num = array_num(proc->children);

	// iterate over all child of current process and update its pid information and
	// process status accordingly
	for (int i = 0; i < child_num; i++)
	{
		struct proc *childProc = array_get(proc->children, i);

		lock_acquire(childProc->pid_lock);

		int child_pid = childProc->pid;
		int child_state = childProc->proc_state;

		//check if each child has exited already

		//if the child has exited, clear its pid information.
		//since when the child exit, it keep its pid info for its parent
		if (child_state == ZOMBIE)
		{
			// Update the next avaliable pid
			lock_acquire(pidtable.pt_lock);
			if (child_pid < pidtable.pid_next)
			{
				pidtable.pid_next = child_pid;
			}

			// update the pidtable accordingly
			// pid at this location is not occupied anymore
			// the # of available pids is incremented
			reset_pidtable_entry(child_pid);
			lock_release(pidtable.pt_lock);

			lock_release(childProc->pid_lock);
			proc_destroy(childProc);
		}
		//if the child is still running, update its program state to ORPHAN to indicate its parent process has finished
		else if (child_state == RUNNING)
		{
			childProc->proc_state = ORPHAN;
			lock_release(childProc->pid_lock);
		}
		//should not reach here
		else
		{
			lock_release(childProc->pid_lock);
			panic("Modify state on a bad child process");
		}
	}
}
