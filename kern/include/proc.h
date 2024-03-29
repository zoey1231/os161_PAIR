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

#ifndef _PROC_H_
#define _PROC_H_

/*
 * Definition of a process.
 *
 * Note: curproc is defined by <current.h>.
 */

#include <spinlock.h>
#include <thread.h> /* required for struct threadarray */
#include <filetable.h>
#include <limits.h>

/**
 * process state
 */
#define READY 0	  // process is runnable
#define RUNNING 1 // process is running
#define ZOMBIE 2  // process has exited, waiting its parent to collect its exit state
#define ORPHAN 3  // parent has exited but the process is still running

#define OCCUPIED 1
#define FREE 0

struct addrspace;
struct vnode;

/*
 * Process structure.
 */
struct proc
{
	char *p_name;				  /* Name of this process */
	struct spinlock p_lock;		  /* Lock for this structure */
	struct threadarray p_threads; /* Threads in this process */

	/* VM */
	struct addrspace *p_addrspace; /* virtual address space */

	/* VFS */
	struct vnode *p_cwd; /* current working directory */
	struct filetable *proc_ft;

	/* add more material here as needed */
	/* filetable */
	//struct fileDescriptorArray *p_fdArray; /* array to keep track of fd of each file in the file table for this process*/
	struct array *fdArray;
	struct lock *fda_lock;

	/* PID */
	pid_t pid;
	int exit_code;
	unsigned int proc_state;
	struct lock *pid_lock;
	struct cv *pid_cv; /* To allow for processes to sleep on waitpid */
	struct array *children;
};

/**
 * pidtable structure
 * All arrays have PID_MAX+1 entries so that pid is equal to arrays' index
 */
struct pidtable
{
	struct lock *pt_lock;
	struct array *pid_procs;
	struct bitmap *occupied;
	//struct proc *pid_procs[PID_MAX + 1]; /* Array to hold processes */
	//unsigned int occupied[PID_MAX + 1];	 /*Array to indicate if an entry(i.e.,a PID) in pidtable is occupied or not*/
	int pid_available; /* Number of available pid spaces */
	int pid_next;	   /* Lowest free PID */
};
//pidtable is accessible for all processes
extern struct pidtable pidtable;

/* This is the process structure for the kernel and for kernel-only threads. */
extern struct proc *kproc;

/* Call once during system startup to allocate data structures. */
void proc_bootstrap(void);

/* Create a fresh process for use by runprogram(). */
struct proc *proc_create_runprogram(const char *name);

/* Destroy a process. */
void proc_destroy(struct proc *proc);

/* Attach a thread to a process. Must not already have a process. */
int proc_addthread(struct proc *proc, struct thread *t);

/* Detach a thread from its process. */
void proc_remthread(struct thread *t);

/* Fetch the address space of the current process. */
struct addrspace *proc_getas(void);

/* Change the address space of the current process, and return the old one. */
struct addrspace *proc_setas(struct addrspace *);

/*Filetable*/
/* Initialize the filetable of the current process*/
int proc_filetable_init(struct proc *proc);

/*PID*/

struct proc *create_fork_proc(const char *name);

/* Initialize the shared pidtable */
void pidtable_init(struct pidtable *);

/* Add a process to the pidtable and return assigned pid in the 2nd argument */
int pidtable_add(struct proc *, int32_t *);

/* Remove a given pid from the pidtable */
void pidtable_remove(pid_t);

void updateChildState(struct proc *proc);

/* Reset a specific pidtable entry with index pid*/
void reset_pidtable_entry(pid_t pid);
#endif /* _PROC_H_ */
