# os161_PAIR
Assignments for CPEN331 Operating Systems. Assignments consisted of code reading exercises and writing codes.

### OS/161
OS/161 is a simplified operating system which includes a standalone kernel and a simple userland, all written in C. OS/161 OS runs on a machine simulator, System/161.

### Assignment 1
* Built, compiled, and got familiar with OS/161
### Assignment 2
* Implemented blocking locks and condition variables in OS161 based on existing implementation of mutexes and semaphores.
### Assignment 3
* Enforced mutual exclusion and thread-to-thread signaling using the synchronization primitives implemented in A2. 
### Assignment 4
* Implemented several new system calls in OS161 that allow user programs to use files: open(), read(), write(), lseek(), close(), dup2(), chdir(), and __getcwd()
* Understood how to safely transport the arguments (copy in/out) between the user land and the kernel.
* Learnt about and use the virtual file system interface.
### Assignment 5
* Implemented fork and exec system calls in OS161. 
* Learnt about the process abstraction and Unix and mechanisms of new process creation
* Learnt about enforcing protection and safety in a multiprocess operating system
### Assignment 6
* Learnt about the functions, benefits and costs of virtual memory
* Understood the hardware and software mechanisms needed to implement virtual memory
* Implemented full virtual memory support in OS161:
  * Handling of TLB faults
  * Abstractions and data structures for virtual address spaces
  * Page tables (handling large page tables)
  * Paging to disk.
