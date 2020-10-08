/*
 * Driver code for airballoon problem
 */
#include <types.h>
#include <lib.h>
#include <thread.h>
#include <test.h>
#include <synch.h>
#include <current.h>

#define N_LORD_FLOWERKILLER 8
#define NROPES 16
static int ropes_left = NROPES;

/** Data structures 
 * ropeMapping is an array with length NROPES where each element is a pointer to a rope.
 * 		for example, ropeMapping[0] is rope #0, which is hook#0 as well
 * 		since we assume that rope number is the same as hook number
 * hook_connected and stack_connected can have value either 0 or 1.
 * 		1 indicates the rope is still connected through the hook or stack.
 *		0 indicates the rope has been severed on the hook or stack side.
 * stack_connected_to is the stack # a rope connected to.
 * 		for example, ropeMapping[0]->stack_conneted = 4 means
 * 		rope #0 is connected to stack #4.
 * stackMapping is an array storing the mapping from stack to rope.
 * 		for example, stackMapping[0]==12 means stack#0 is attached to rope#12.
 * 		stackMapping makes marigold and flowerkiller can quickly find which 
 * 		rope connected a specific stack
 */
struct Rope
{
	volatile unsigned hook_connected;
	volatile unsigned stack_connected;
	volatile unsigned stack_connected_to;
};

struct Rope *ropeMapping[NROPES];
static volatile int *stackMapping;

/* Synchronization primitives */
static struct lock *rope_lock[NROPES];	//an array of locks; each rope has its own lock
static struct lock *ropes_left_lock;	//lock to secure access to variable ropes_left
static struct lock *balloon_lock;		//lock to secure main_thread_done_cv
static struct lock *stack_lock[NROPES]; //elements in the array are locks corresponding to stacks in stackMapping

/**
 * main_thread_done_cv: main thread wait on this CV to wait for dandelion, marigold and flowerkillers 
 * 						to finish their work. Once all threads except main are done, balloon will signal 
 * 						main thread by this CV.
 * escape_sem: 			a semaphore initialized to 0. Once each thread from dendalion,marigold, or flowerkiller
 * 						(total 10 threads) is done, they will increase the escape_sem by 1. Thread balloon 
 * 						will try to decrement escape_sem 10 times before done and proceeding to notify
 * 						the main thread.
 * 				
*/
static struct cv *main_thread_done_cv;
static struct semaphore *escape_sem;

/*
 * Describe your design and any invariants or locking protocols
 * that must be maintained. Explain the exit conditions. How
 * do all threads know when they are done?
 * Design:
 * 		dandelion will server a random rope from hook by setting 
 * 		ropeMapping[hook_num]->hook_connected = 0; as well as 
 * 		ropes_left--;
 * 		marigold will server a random rope from stack by setting 
 * 		ropeMapping[rope_num]->stack_connected = 0; as well as 
 * 		ropes_left--;
 * 		flowerkiller will switch two different random ropes from stack
 * 		by swapping stackMapping[stack_num_1] = rope_2; as well as
 *		ropeMapping[rope_2]->stack_connected_to = stack_num_1; and apply
 *		the same for rope_1.
 *  	when there is only 1 rope left, dandelion and marigold will wait on a CV
 * 		until all flowerkillers exit and then sever the last rope.
 *		balloon will wait on dandelion and marigold to finish by tring to 
 *		decrease escape_sem by 2 times and notify the main thread that all other 
 *		threads are done by a CV main_thread_done_cv.
 * 
 * invariants:
 * 		the mapping of a rope to a stack should always be the same in 
 * 		both rope and stackMapping data structures.
 * 		In other words, if ropeMapping[rope_num]->stack_connected_to,
 * 		the following should remain true:
 * 			stackMapping[stack_connected_to]==rope_num
 * 
 * exit conditions:
 * 		Main thread will wait on the CV main_thread_done_cv before exit.
 * 		Dandelion and marigold will exit once ropes_left is 0(i.e.all ropes are severed).
 * 		Flowerkiller will exit once ropes_left is either 0 or 1.
 * 		Once dandelion, marigold, and all flowerkiller threads are done, balloon thread 
 * 		will be able to decrase escape_sem by 10 times and proceed to notify the main thread
 * 		that all other threads are done by CV main_thread_done_cv.	
 * 		
 */

/*
 * dandelion severed  a random rope from the hook
 */
static void
dandelion(void *p, unsigned long arg)
{
	(void)p;
	(void)arg;

	kprintf("Dandelion thread starting\n");

	while (1)
	{
		//dandelion will exit only once there are no ropes left
		lock_acquire(ropes_left_lock);
		if (ropes_left == 0)
		{
			lock_release(ropes_left_lock);
			goto done;
		}

		//get a random rope number dandelion want to sever
		int hook_num = random() % NROPES;

		//check if rope is connected. if so, severed it.
		//if not, move to the next rope
		lock_acquire(rope_lock[hook_num]);
		int rope_is_connected = ropeMapping[hook_num]->hook_connected && ropeMapping[hook_num]->stack_connected;
		if (rope_is_connected)
		{
			ropeMapping[hook_num]->hook_connected = 0;

			ropes_left--;
			kprintf("Dandelion severed rope %d\n", hook_num);
			lock_release(rope_lock[hook_num]);
			lock_release(ropes_left_lock);

			//pass control to other threads
			thread_yield();
		}
		else
		{
			lock_release(rope_lock[hook_num]);
			lock_release(ropes_left_lock);
		}
	}
done:
	kprintf("Dandelion thread done\n");
	V(escape_sem);
}

/*
 * marigold severed a random rope from the stack
 */
static void
marigold(void *p, unsigned long arg)
{
	(void)p;
	(void)arg;

	kprintf("Marigold thread starting\n");

	while (1)
	{
		//marigold will exit only once there are no ropes left
		lock_acquire(ropes_left_lock);

		if (ropes_left == 0)
		{
			lock_release(ropes_left_lock);
			goto done;
		}

		//get a random rope number marigold want to sever
		unsigned int stack_num = random() % NROPES;

		//find the corresponding rope# in stackMapping array
		lock_acquire(stack_lock[stack_num]);
		int rope_num = stackMapping[stack_num];

		lock_acquire(rope_lock[rope_num]);
		int rope_is_connected = ropeMapping[rope_num]->hook_connected && ropeMapping[rope_num]->stack_connected;

		//if the rope is conneted and connected on the selected stack, sever it
		if (rope_is_connected && ropeMapping[rope_num]->stack_connected_to == stack_num)
		{

			ropeMapping[rope_num]->stack_connected = 0;
			ropes_left--;
			//once severed a rope from the stack, release the lock and pass control to other threads
			kprintf("Marigold severed rope %d from stake %d\n", rope_num, stack_num);

			lock_release(rope_lock[rope_num]);
			lock_release(stack_lock[stack_num]);
			lock_release(ropes_left_lock);

			thread_yield();
		}
		else
		{
			//if rope#i is not the rope connected to the selected stack
			//simply release the lock and try the next rope

			lock_release(rope_lock[rope_num]);
			lock_release(stack_lock[stack_num]);
			lock_release(ropes_left_lock);
		}
	}
done:
	kprintf("Marigold thread done\n");
	V(escape_sem);
}

/*
 * flowerkiller switch two random ropes by stacks
 */
static void
flowerkiller(void *p, unsigned long arg)
{
	(void)p;
	(void)arg;

	kprintf("Lord FlowerKiller thread starting\n");

	while (1)
	{
		//flowerkiller will exit once there are only 1 rope left to switch
		lock_acquire(ropes_left_lock);

		if (ropes_left <= 1)
		{
			lock_release(ropes_left_lock);

			goto done;
		}
		else
		{
			lock_release(ropes_left_lock);
		}

		//get two different random rope numbers flowerkiller want to exchange
		int stack_num_1 = 100; //initalized to an impossible number
		int stack_num_2 = 100;
		//if flowerkiller accidently selects two identical stack numbers
		//reselect again
		while (stack_num_1 == stack_num_2)
		{
			stack_num_1 = random() % NROPES;
			stack_num_2 = random() % NROPES;
		}
		lock_acquire(stack_lock[stack_num_1]);
		lock_acquire(stack_lock[stack_num_2]);

		//find the two ropes that are connected on the two selected stacks
		int rope_1 = stackMapping[stack_num_1];
		lock_acquire(rope_lock[rope_1]);
		int rope_2 = stackMapping[stack_num_2];
		lock_acquire(rope_lock[rope_2]);

		//if both ropes are still connected, switch them by stacks
		int rope_1_is_connected = ropeMapping[rope_1]->hook_connected && ropeMapping[rope_1]->stack_connected;
		int rope_2_is_connected = ropeMapping[rope_2]->hook_connected && ropeMapping[rope_2]->stack_connected;

		if (rope_1_is_connected && rope_2_is_connected)
		{

			stackMapping[stack_num_1] = rope_2;
			ropeMapping[rope_2]->stack_connected_to = stack_num_1;
			kprintf("Lord FlowerKiller switched rope %d from stake %d to stake %d\n", rope_2, stack_num_2, stack_num_1);
			stackMapping[stack_num_2] = rope_1;
			ropeMapping[rope_1]->stack_connected_to = stack_num_2;
			kprintf("Lord FlowerKiller switched rope %d from stake %d to stake %d\n", rope_1, stack_num_1, stack_num_2);

			lock_release(stack_lock[stack_num_2]);
			lock_release(stack_lock[stack_num_1]);
			lock_release(rope_lock[rope_2]);
			lock_release(rope_lock[rope_1]);

			thread_yield();
		}
		else
		{
			lock_release(stack_lock[stack_num_2]);
			lock_release(stack_lock[stack_num_1]);
			lock_release(rope_lock[rope_2]);
			lock_release(rope_lock[rope_1]);
		}
	}
done:
	kprintf("Lord FlowerKiller thread done\n");
	V(escape_sem);
}

static void
balloon(void *p, unsigned long arg)
{
	(void)p;
	(void)arg;

	kprintf("Balloon thread starting\n");

	//wait until dandelion and marigold and flowerkiller are finished.
	for (int i = 0; i < 2 + N_LORD_FLOWERKILLER; i++)
	{
		P(escape_sem);
	}

	kprintf("Balloon freed and Prince Dandelion escapes!\n");
	kprintf("Balloon thread done\n");

	lock_acquire(balloon_lock);
	cv_signal(main_thread_done_cv, balloon_lock);
	lock_release(balloon_lock);
}

int airballoon(int nargs, char **args)
{

	int err = 0;
	int i;
	ropes_left = NROPES;
	(void)nargs;
	(void)args;
	(void)ropes_left;

	//initialize locks and CVs
	ropes_left_lock = lock_create("ropes_left_lock");
	for (int i = 0; i < NROPES; i++)
	{
		rope_lock[i] = lock_create("rope_lock");
		stack_lock[i] = lock_create("stack_lock");
	}
	balloon_lock = lock_create("balloon_lock");

	main_thread_done_cv = cv_create("main_thread_done_cv");
	escape_sem = sem_create("escape_sem", 0);

	//initialize data structures
	stackMapping = (int *)kmalloc(NROPES * sizeof(int));
	for (int i = 0; i < NROPES; i++)
	{
		ropeMapping[i] = kmalloc(sizeof(struct Rope));
		ropeMapping[i]->hook_connected = 1;
		ropeMapping[i]->stack_connected = 1;

		//originally every rope connected its own stack
		ropeMapping[i]->stack_connected_to = i;
		stackMapping[i] = i;
	}

	//start threads
	err = thread_fork("Marigold Thread",
					  NULL, marigold, NULL, 0);
	if (err)
		goto panic;

	err = thread_fork("Dandelion Thread",
					  NULL, dandelion, NULL, 0);
	if (err)
		goto panic;

	for (i = 0; i < N_LORD_FLOWERKILLER; i++)
	{
		err = thread_fork("Lord FlowerKiller Thread",
						  NULL, flowerkiller, NULL, 0);
		if (err)
			goto panic;
	}

	err = thread_fork("Air Balloon",
					  NULL, balloon, NULL, 0);
	if (err)
		goto panic;

	goto done;
panic:
	panic("airballoon: thread_fork failed: %s)\n",
		  strerror(err));

done:
	// wait until notified by balloon that all threads are done
	lock_acquire(balloon_lock);
	cv_wait(main_thread_done_cv, balloon_lock);
	lock_release(balloon_lock);

	//destory all locks, CVs and memories
	lock_destroy(ropes_left_lock);
	lock_destroy(balloon_lock);
	for (int i = 0; i < NROPES; i++)
	{
		lock_destroy(rope_lock[i]);
		lock_destroy(stack_lock[i]);
		kfree(ropeMapping[i]);
	}
	kfree((void *)stackMapping);
	cv_destroy(main_thread_done_cv);
	sem_destroy(escape_sem);

	kprintf("Main thread done\n");
	return 0;
}