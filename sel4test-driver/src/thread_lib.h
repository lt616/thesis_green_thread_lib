#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include <sel4utils/stack.h>
#include <sel4/types.h>

#include <allocman/bootstrap.h>
#include <allocman/vka.h>

#include "sync_prim.h"

/*
  Qianrui Zhao | axjllt@gmail.com
  Thesis work - 2017 S2
  User-level thread library API
*/

/* Limitation for number of threads */
#define MAX_CAPACITY 2000

// #define GREEN_BENCHMARK_BREAKDOWN

// #define GREEN_SLOW
#define GREEN_FAST
#define GREEN_VERY_FAST

int initialised;

int co_current_id;

void *co_current;
// void *co_next;

enum status_t {
    running = 0,
    ready = 1,
    waiting = 2
};


/*
    struct thread_context_t
    Structure saving machine context of a thread.
*/
struct thread_context_t /* SIZE == 62 bytes */
{
    /* stack pointer */
    uint32_t esp;

    /* instruction pointer */
    uint32_t eip;

    /* 8 gerneral purpose registers */
    uint32_t ebx;
    uint32_t ebp;
    uint32_t edi;
    uint32_t esi;
};
typedef struct thread_context_t thread_context_t;



/*
    struct thread_join_link_t
    Structure saving a join entry for function thread_join().
*/
struct thread_join_link_t {

    int caller; // thread calling thread_join()
    int joiner; // thread called by thread_join() as a parameter.

    /* A semaphore */
    int joinable;

    struct thread_join_link_t *next;
};
typedef struct thread_join_link_t thread_join_link_t;



/*
  struct thread_t
  Structure saving a thread.
*/
struct thread_t {

    thread_context_t *context;

    int t_id;

    void *stack_top;

    enum status_t status;

    void *(* start_routine)(void *);

    void *arg;

    thread_join_link_t *join;

    thread_join_link_t *joined_start;
    thread_join_link_t *joined_end;

    cspacepath_t slot;

    int if_defer;
    seL4_Word reply;

};
typedef struct thread_t thread_t;



/*
    struct thread_link_t
    Structre to make up a thread link.
*/
struct thread_link_t {

    int t_id;

    thread_t *t;

    struct thread_link_t *next;
};
typedef struct thread_link_t thread_link_t;



/*
    struct thread_waiter_t
    Structure to make up a thread waiter.
*/
struct thread_waiter_t {

    thread_link_t *linker_start;
    thread_link_t *linker_end;

    thread_link_t *linker;

    void *lock;

    struct thread_waiter_t *next;
};
typedef struct thread_waiter_t thread_waiter_t;



/*
    struct thread_pool_t
    Structure saving the thread pool.
*/
struct thread_pool_t {
    int num;

    int token;

    thread_link_t *t_running;

    int ready_num;
    thread_link_t *t_ready_start;
    thread_link_t *t_ready_end;

    int waiting_num;
#ifdef GREEN_SLOW
    thread_waiter_t *t_waiting_start;
    thread_waiter_t *t_waiting_end;
#endif

#ifdef GREEN_FAST
    // thread_lock_t *waiting_lock;

    void *t_waiting_start;
    void *t_waiting_end;
#endif

    thread_sync_prim_t *sync_prim_start;
    thread_sync_prim_t *sync_prim_end;

    int new_id;

    thread_t *addrs[MAX_CAPACITY];
    thread_t *(* get_thread)(struct thread_pool_t *self, int t_id);
    int (*get_new_id)(struct thread_pool_t *self);
    void (* set_reply_ep) (seL4_Word);
    int (* get_reply_ep) ();

    // void (* thread_creation)(void *);
};
typedef struct thread_pool_t thread_pool_t;

thread_pool_t *pool;

/* For debugging */
void print_context(int thread_id, int seq);
void switch_to_main(int thread_id);
void switch_context(void *new);
void swap_context(void *cur, void *new);
void bouncer(void);
void thread_update_token(int update);
void thread_sleep(void *sync_prim, void *arg);
void thread_wakeup(void *sync_prim, void *arg);
void thread_wakeup_wait(void *sync_prim);
/* Endfor debugging */

int inv_count;

/*
    thread_pool_t *thread_initial()

    Create and set thread pool.

    No parameter.

    Return value:
        Pointer to the thread pool if success;
        Null otherwise.
*/
thread_pool_t *thread_initial();


/*
	int thread_create(int stack_size, void *(* func)(void *), void *arg):

	Create a user-level thread, set its state to ready.

	Parameters:
		stack_size: the size of stack;
		func: start routine for the thread;
		arg: argument for the start routine.

	Return value:
		Thread id if success, which is a positive integer;
		0 if failed.
*/
int thread_create(allocman_t *allocman, vspace_t *vspace, void *(* func)(void *), void *arg);


/*
	int thread_exit(int thread_id):

	Destory current running thread.

	Parameter:

	Return value:
		1 if successfully deleted;
		0 if failed.
*/
int thread_exit();


/*
	int thread_yield(int (* func)(void *)):

	Thread call this function will go into waiting state. If there is any available token, a waiting 	thread will be invoked (if any). Otherwise a ready state thread will be invoked instead.

	Parameter:
		func: a function used to determine if there is any available token. It also update the 			token after successfully invoke another thread.

		Here a function is passed to check/ update the token so that many types of token can be used.

	Return value:
		thread_id if successfully invoke a ready or waiting thread, which is a integer equals to or greater than 0;
		-1 if failed.
*/
int thread_yield();


/*
	int thread_start(int thread_id):

	Start a thread in ready pool.

	Parameter:
		thread_id: a positive integer specifies which thread to start. The thread must in ready 				state.

	Return value:
		1 if successfully start a ready state thread;
		0 if failed.

*/
// int thread_start(int thread_id, vspace_t *vspace, void * (*func)(void *arg), void *arg, void **retval);




/*
	int thread_resume(int thread_id):

	Resume a specific thread in ready or waiting state.

	Parameter:

	Return value:
		1 if successfully resume a waiting state thread;
		0 if failed.
*/
int thread_resume(int thread_id);





/*
    int thread_join(int thread_id):

    Current thread yielded until the specific thread exit.
*/
int thread_join(int thread_id);





/*
	thread_t *thread_info(int thread_id):

	Return information about a specific thread.

	Parameter:
		thread_id: a positive integer specifies which thread to enquiry.

	Return value:
		A structure storing information about a thread;
		Null if the thread does not exist.
*/
void thread_info(int t_id);


/*
	thread_pool_t *thread_pool_info():

	Return information about the whole thread pool, including how many thread are there and 	which thread in what state.

	No Parameter.

	Return value:
		A stucture storing information about the thread pool;
		Null – no case to fail.
*/
void thread_pool_info(int detail);




/*
	thread_t *thread_self():

	Return information about current running thread.

	No parameter.

	Return value:
		A structure contains information about current running thread.
		Null if no such thread.
*/
void thread_self();

unsigned start_cycles_high, start_cycles_low, end_cycles_high, end_cycles_low;
uint64_t start_total, end_total;
uint64_t start, end;

static inline uint64_t
rdtsc(void)
{
    uint64_t ret;
    asm volatile("rdtsc" : "=A" (ret));

    return ret;
}

static inline void
rdtsc_start(void)
{
    asm volatile("CPUID\n\t"
                "RDTSC\n\t"
                "mov %%edx, %0\n\t"
                "mov %%eax, %1\n\t" : "=r" (start_cycles_high), "=r" (start_cycles_low)
                :: "%rax", "%rbx", "%rcx", "rdx");

    return;
}

static inline void
rdtsc_end(void)
{
    asm volatile("RDTSCP\n\t"
                "mov %%edx, %0\n\t"
                "mov %%eax, %1\n\t"
                "CPUID\n\t" : "=r" (end_cycles_high), "=r" (end_cycles_low)
                :: "%rax", "%rbx", "%rcx", "rdx");

    start = (((uint64_t) start_cycles_high << 32) | start_cycles_low);
    end = (((uint64_t) end_cycles_high << 32) | end_cycles_low);

    return;
}
