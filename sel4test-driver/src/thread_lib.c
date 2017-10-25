#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sel4utils/stack.h>

#include "thread_lib.h"
// #include "sync_prim.h"

#define _(x) #x



/*
    Function switch_context
    void switch_context(void *new);
*/
asm(	".text"				);
asm(	".globl "_(switch_context)		);
asm(	".type "_(switch_context)", @function"	);
asm(	_(switch_context)":"			);

asm(	"movl 4(%esp), %edx"			);

asm(	"movl (%edx), %esp"		);	/* restore ESP */

asm(	"pushl 4(%edx)"         );	/* restore EIP */

asm(	"ret"                   );	// return to new coro
asm(	".size "_(switch_context)", . - "_(switch_context));


/*
    Function swap_context
    void swap_context(void *cur, void *new);
*/
asm(	".text"				);
asm(	".globl "_(swap_context)		);
asm(	".type "_(swap_context)", @function"	);
asm(	_(swap_context)":"			);

asm(	"movl 4(%esp), %ecx"			);	// save reg-vars/framepointer
asm(	"movl 8(%esp), %edx"			);

asm(    "pushl %ebp"            );
asm(    "pushl %ebx"            );
asm(	"pushl %esi"			);
asm(	"pushl %edi"			);

asm(	"movl %esp, (%ecx)"    	);	/* save ESP */
asm(	"movl (%edx), %esp"		);	/* restore ESP */

asm(	"movl $1f, 4(%ecx)"		);	/* save EIP */
asm(	"pushl 4(%edx)"         );	/* restore EIP */
asm(	"ret"                   );	// return to new coro

asm(	"1:"				    );
asm(    "popl %edi"             );
asm(    "popl %esi"             );
asm(    "popl %ebx"             );
asm(    "popl %ebp"             );

asm(    "ret"                   );
asm(	".size "_(swap_context)", . - "_(swap_context));


/*
    Function bouncer
    void bouncer(void)
*/
asm(	".text"				);
asm(	".globl "_(bouncer)		);
asm(	".type "_(bouncer)", @function"	);
asm(	_(bouncer)":"			);

asm(	"popl %eax"			);	// save reg-vars/framepointer
asm(	"popl %ebx"			);

asm(    "pushl $0"            );
asm(    "xorl %ebp, %ebp"         );
asm(	"pushl %eax"			);
asm(	"call *%ebx"			);

/* Call the function for exit */
asm(    "call thread_exit"      );

asm(	".size "_(bouncer)", . - "_(bouncer));


/*
    function retrieve a used thread id
    return available thread id if successful;
    return 0 otherwise.
*/
int
get_new_id(thread_pool_t *self) {

    // int i = 1;

    /* thread id starts from 1 [to MAX_CAPACITY] */
    // for (i = 1;i < MAX_CAPACITY;i ++) {
    //     if (self->addrs[i] == NULL)
    //         return i;
    // }

    // return 0;

    return pool->new_id ++;
}



/*
    function map thread id to its addr.
*/
thread_t *
get_thread(thread_pool_t *self, int t_id)
{
    assert(t_id >= 0);

    thread_t *res = self->addrs[t_id];
    return res == NULL ? 0 : res;
}



void
set_reply_ep(seL4_Word reply)
{
    thread_t *cur = pool->get_thread(pool, co_current_id);
    assert(cur != NULL);
    assert(! cur->if_defer);

    cur->if_defer = 1;

    cur->reply = reply;

    return;
}



int
get_reply_ep(seL4_Word *reply)
{
    thread_t *cur = pool->get_thread(pool, co_current_id);
    assert(cur != NULL);
    int if_defer = cur->if_defer ? 1 : 0;

    cur->if_defer = 0;
    *reply = cur->reply;

    return if_defer;
}



void
stack_push(void **stack_p, uint32_t v)
{
	uint32_t *stack = *stack_p;

	stack--;

	*stack = v;
	*stack_p = stack;
}



/*
    functions manipulating join blocks.
*/
void
append_join_block(thread_join_link_t *linker, thread_join_link_t **start, thread_join_link_t **end)
{
    if (*end == NULL) {
        *start = linker;
        *end = linker;
    } else {
        (*end) ->next = linker;
        linker->next = NULL;
        *end = linker;
    }

    return;
}



void
delete_join_block(thread_join_link_t *linker, thread_join_link_t **start, thread_join_link_t **end)
{
    assert(*start != NULL && *end != NULL);

    thread_join_link_t *cur = *start;
    thread_join_link_t *pre = NULL;

    while (cur != NULL) {
        if (cur == linker) {
            if (cur == *start && cur == *end) {
                *start = NULL;
                *end = NULL;
            } else if (cur == *start) {
                *start = cur->next;
            } else if (cur == *end) {
                pre->next = cur->next;
                *end = pre;
            } else {
                pre->next = cur->next;
            }

            cur->next = NULL;
        }

        pre = cur;
        cur = cur->next;
    }

    return;
}



/*
    functions switching thread status.
*/
void
append_ready(thread_link_t *linker, thread_link_t **start, thread_link_t **end)
{
    if (*end == NULL) {
        *start = linker;
        *end = linker;
    } else {
        (*end)->next = linker;
        linker->next = NULL;
        *end = linker;
    }

    return;
}



#ifdef GREEN_SLOW
void
append_waiting(thread_waiter_t *linker, thread_waiter_t **start, thread_waiter_t **end)
{
    if (*end == NULL) {
        *start = linker;
        *end = linker;
    } else {
        (*end)->next = linker;
        linker->next = NULL;
        *end = linker;
    }

    return;
}
#endif



#ifdef GREEN_FAST
void
append_waiting(thread_link_t *linker, thread_link_t **start, thread_link_t **end)
{

    if (*end == NULL) {
        *start = linker;
        *end = linker;
    } else {
        (*end)->next = linker;
        linker->next = NULL;
        *end = linker;
    }
    pool->waiting_num ++;

    return;
}
#endif



#ifdef GREEN_FAST
/* Changed need to be applied on syncprim_des */
void
append_lock(void *sync_prim, thread_link_t *linker, void **start, void **end)
{
    // assert(*start != NULL && *end != NULL

    thread_sync_prim_t *prim = (thread_sync_prim_t *) sync_prim;

    /* check if this lock is already in the waiting list */
    if (prim->waiting_start == NULL) {
        if (*end == NULL) {
            *start = prim;
            *end = prim;
        } else {
            ((thread_sync_prim_t *) (*end))->next = prim;
            ((thread_sync_prim_t *) prim)->pre = *end;
            *end = prim;
        }
    }

    append_waiting(linker,
                (thread_link_t **) &((thread_sync_prim_t *) sync_prim)->waiting_start,
                (thread_link_t **) &((thread_sync_prim_t *) sync_prim)->waiting_end);

    return;
}
#endif



thread_link_t *
extract_ready(int t_id, thread_link_t **start, thread_link_t **end)
{
    // assert(*start != NULL && *end != NULL)
    if (*start == NULL && *end == NULL)
        return NULL;

    thread_link_t *cur = *start;
    thread_link_t *pre = NULL;

    while (cur != NULL) {
        if (cur->t_id == t_id) {
            if (cur == *start && cur == *end) {
                *start = NULL;
                *end = NULL;
            } else if (cur == *start) {
                *start = cur->next;
            } else if (cur == *end) {
                pre->next = cur->next;
                *end = pre;
            } else {
                pre->next = cur->next;
            }

            cur->next = NULL;
            return cur;
        }

        pre = cur;
        cur = cur->next;
    }

    return NULL;
}



#ifdef GREEN_SLOW
thread_link_t *
extract_waiting(void *sync_prim, thread_waiter_t **start, thread_waiter_t **end)
{
    // assert(*start != NULL && *end != NULL);
    if (*start == NULL && *end == NULL)
        return NULL;

    thread_waiter_t *cur = *start;
    thread_waiter_t *pre = NULL;

    while (cur != NULL) {
        if (cur->lock == sync_prim) {
            if (cur == *start && cur == *end) {
                *start = NULL;
                *end = NULL;
            } else if (cur == *start) {
                *start = cur->next;
            } else if (cur == *end) {
                pre->next = cur->next;
                *end = pre;
            } else {
                pre->next = cur->next;
            }

            cur->next = NULL;
            return cur->linker;
        }

        pre = cur;
        cur = cur->next;
    }

    return NULL;
}
#endif



thread_link_t *
extract_thread(thread_t *t, thread_link_t **start, thread_link_t **end)
{

    assert(*start != NULL && *end != NULL);

    thread_link_t *cur = *start;
    thread_link_t *pre = NULL;

    while (cur != NULL) {
        if (cur->t == t) {
            if (cur == *start && cur == *end) {
                *start = NULL;
                *end = NULL;
            } else if (cur == *start) {
                *start = cur->next;
            } else if (cur == *end) {
                pre->next = cur->next;
                *end = pre;
            } else {
                pre->next = cur->next;
            }

            cur->next = NULL;
            return cur;
        }

        pre = cur;
        cur = cur->next;
    }

    return NULL;
}



void
running_to_ready(thread_link_t *linker)
{
    linker->t->status = ready;

    append_ready(linker, &pool->t_ready_start, &pool->t_ready_end);

    pool->ready_num ++;

    // pool->t_running = NULL;

    return;
}



#ifdef GREEN_SLOW
void
running_to_waiting(thread_t *t, void *lock)
{
    t->status = waiting;

    thread_waiter_t *waiter = malloc(sizeof(thread_waiter_t));
    if (waiter == NULL) {
        printf("Error: Cannot malloc a thread waiter.\n");
        return;
    }

    waiter->linker = pool->t_running;
    waiter->lock = lock;
    append_waiting(waiter, &pool->t_waiting_start, &pool->t_waiting_end);

    pool->waiting_num ++;


    return;
}
#endif



#ifdef GREEN_FAST
void running_to_waiting(thread_link_t *linker, void *lock)
{
    linker->t->status = waiting;

    append_lock(lock, linker, &pool->t_waiting_start, &pool->t_waiting_end);

    pool->waiting_num ++;

    return;
}
#endif



#ifdef GREEN_VERY_FAST
void
running_to_lock_waiting(thread_link_t *linker, thread_sync_prim_t *sync_prim)
{
    linker->t->status = waiting;
    append_waiting(linker, (thread_link_t **) &(sync_prim->waiting_start), (thread_link_t **) &(sync_prim->waiting_end));

    return;
}
#endif



int
waiting_to_running(thread_link_t *linker)
{

    assert(pool->waiting_num > 0);

    linker->t->status = running;

    pool->t_running = linker;
    pool->waiting_num --;

    return 1;
}



int
ready_to_running(thread_t *t)
{

    assert(pool->ready_num > 0);

    t->status = running;

    thread_link_t *linker = extract_thread(t, &pool->t_ready_start, &pool->t_ready_end);
    if (linker == NULL) {
        printf("Error: Cannot find thread %d when changing it from ready to running state.\n", t->t_id);
        return 0;
    }

    pool->t_running = linker;

    pool->ready_num --;

    return 1;
}



#ifdef GREEN_FAST
thread_link_t *
extract_first_waiting(thread_link_t **start, thread_link_t **end)
{

    if (*start == NULL && *end == NULL) {
        return NULL;
    }

    thread_link_t *cur = *start;

    if (*start == *end) {
        *start = NULL;
        *end = NULL;
    } else {
        *start = cur->next;
    }

    cur->next = NULL;

    return cur;
}



thread_link_t *
extract_waiting(void *sync_prim, void **start, void **end)
{
    // assert(*start != NULL && *end != NULL);
    if (*start == NULL && *end == NULL) {
        return NULL;
    }

    thread_sync_prim_t *prim = (thread_sync_prim_t *) sync_prim;

    thread_link_t *res = NULL;
    res = extract_first_waiting(
        (thread_link_t **) &(((thread_sync_prim_t *) sync_prim)->waiting_start),
        (thread_link_t **) &(((thread_sync_prim_t *) sync_prim)->waiting_end));

    /* check if the sync_prim has an empty waiting queue now */
    if (prim->waiting_start == NULL) {
        // change to a linked list
        if (prim->pre == NULL && prim->next == NULL) {
            *start = NULL;
            *end = NULL;
        } else if (prim->pre == NULL) {
            prim->next->pre = NULL;
            *start = prim->next;
            prim->next = NULL;
        } else if (prim->next == NULL) {
            prim->pre->next = NULL;
            *end = prim->pre;
            prim->pre = NULL;
        } else {
            prim->pre->next = prim->next;
            prim->next->pre = prim->pre;
            prim->pre = NULL;
            prim->next = NULL;
        }
    }

    // thread_sync_prim_t *cur = *start;
    // thread_sync_prim_t *pre = NULL;
    // thread_link_t *res = NULL;
    //
    // while (cur != NULL) {
    //
    //     if (cur == sync_prim) {
    //
    //         res = extract_first_waiting(
    //             (thread_link_t **) &(cur->waiting_start),
    //             (thread_link_t **) &(cur->waiting_end));
    //
    //         /* if the waiitng_queue is empty, drop it [the syn_prim] */
    //         if (cur->waiting_start == NULL) {
    //             if (cur == *start && cur == *end) {
    //                 *start = NULL;
    //                 *end = NULL;
    //             } else if (cur == *start) {
    //                 *start = cur->next;
    //             } else if (cur == *end) {
    //                 pre->next = cur->next;
    //                 *end = pre;
    //             } else {
    //                 pre->next = cur->next;
    //             }
    //
    //             cur->next = NULL;
    //         }
    //
    //         return res;
    //     }
    //
    //     pre = cur;
    //     cur = cur->next;
    // }

    return res;
}



#ifdef GREEN_VERY_FAST
thread_link_t *
extract_lock_waiting(thread_sync_prim_t *sync_prim)
{
    return extract_first_waiting(
                (thread_link_t **) &(sync_prim->waiting_start),
                (thread_link_t **) &(sync_prim->waiting_end)
    );

}
#endif

    // thread_link_t *
    // extract_waiting(thread_link_t * new_wait_linker, void *sync_prim, void **start, void **end)
    // {
    //     // assert(*start != NULL && *end != NULL);
    //     if (*start == NULL && *end == NULL)
    //         return NULL;
    //
    //     thread_sync_prim_t *cur = *start;
    //     // thread_sync_prim_t *pre = NULL;
    //     printf("%p %p\n", cur, sync_prim);
    //
    //     while (cur != NULL) {
    //
    //         if (cur == sync_prim) {
    //             printf("FOUND!\n");
    //             // if (cur == *start && cur == *end) {
    //             //     *start = NULL;
    //             //     *end = NULL;
    //             // } else if (cur == *start) {
    //             //     *start = cur->next;
    //             // } else if (cur == *end) {
    //             //     pre->next = cur->next;
    //             //     *end = pre;
    //             // } else {
    //             //     pre->next = cur->next;
    //             // }
    //             //
    //             // cur->next = NULL;
    //             printf("FORMER START %p END %p\n", (*start), (*end));
    //             return extract_first_waiting(new_wait_linker,
    //                         (thread_link_t **) &(cur->waiting_start),
    //                         (thread_link_t **) &(cur->waiting_end));
    //         }
    //
    //         // pre = cur;
    //         cur = cur->next;
    //     }
    //
    //
    //
    //     return NULL;
    // }
    //


    // thread_link_t *
    // switch_waiting(thread_link_t *cur, void *sync_prim, void **start, void **end)
    // {
    //     printf("start %p end %p\n", (*start), (*end));
    //     // thread_link_t *ret = extract_waiting(sync_prim, start, end);
    //     printf("Latter start %p end %p\n", (*start), (*end));
    //
    //
    // return ret;
    // }
    #endif



/*
    functions for scheduling.
*/
#ifdef GREEN_SLOW
int
thread_scheduler_ready(thread_t *cur, void *sync_prim, void *arg, int if_sleep)
{
    // printf("READY THREAD NUM: %d\n", pool->ready_num);
    if (pool->ready_num) {
        /*int t_id = pool->t_ready_start->t_id;*/

        thread_t *new = pool->t_ready_start->t;
        assert(new != NULL);

        /* set current running thread id */
        /*co_current_id = t_id;*/

        /* set cur thread to ready or waiting */
        if (if_sleep) {
            running_to_waiting(cur, sync_prim);
        } else {
            running_to_ready(cur);
        }

        ready_to_running(new);

        co_current_id = new->t_id;


        swap_context((void *) cur->context, (void *) new->context);

        return 1;
    }

    return 2;
}







int
thread_scheduler(void *sync_prim, void *arg, int if_sleep)
{

    /* get current running thread */
    thread_t *cur = pool->t_running->t;
    assert(cur != NULL);

    if (sync_prim == NULL) {
        return thread_scheduler_ready(cur, sync_prim, arg, if_sleep);
    } else {

#ifdef GREEN_BENCHMARK_BREAKDOWN
rdtsc_start();
#endif

        thread_link_t *new_linker = extract_waiting(sync_prim, &pool->t_waiting_start, &pool->t_waiting_end);
        /* pick up a ready thread iff no thread waiting on the lock */
        // thread_link_t new = extract_waiting(cur, sync_prim, &pool->t_waiting_start, &pool->t_waiting_end);
        if (new_linker == NULL) {
            return thread_scheduler_ready(cur, sync_prim, arg, if_sleep);
        }

#ifdef GREEN_BENCHMARK_BREAKDOWN
rdtsc_end();
printf("COLLECTION - EXTRACT_WAITING %llu\n", (end - start));
rdtsc_start();
#endif

        thread_t *new = new_linker->t;
        assert(new != NULL);

        if (if_sleep) {
            running_to_waiting(cur, sync_prim);
        } else {
            running_to_ready(cur);
        }

        assert(pool->t_waiting_start != NULL);

#ifdef GREEN_BENCHMARK_BREAKDOWN
rdtsc_end();
printf("COLLECTION - RUNNING_TO_WAITING %llu\n", (end - start));

rdtsc_start();
#endif

        waiting_to_running(new_linker);

        co_current_id = new->t_id;

#ifdef GREEN_BENCHMARK_BREAKDOWN
rdtsc_end();
printf("COLLECTION - WAITING_TO_RUNNING %llu\n", (end - start));
#endif

        swap_context((void *) cur->context, (void *) new->context);

        return 0;
    }

    /* version 01: FIFO,waiting thread first */
    // if (pool->waiting_num)
    //     return pool->t_waiting_start->t_id;

    // if (pool->ready_num)
    //     return pool->t_ready_start->t_id;

    // thread_create(pool->start_routine);

    assert(0);
    return -1;
}
#endif



#ifdef GREEN_FAST
int
thread_scheduler_ready(thread_link_t *cur, void *sync_prim, void *arg, int if_sleep)
{
    // printf("READY THREAD NUM: %d\n", pool->ready_num);
    if (pool->ready_num) {
        /*int t_id = pool->t_ready_start->t_id;*/

        thread_t *new = pool->t_ready_start->t;
        assert(new != NULL);

        /* set current running thread id */
        /*co_current_id = t_id;*/

        /* set cur thread to ready or waiting */
        if (if_sleep) {
            running_to_waiting(cur, sync_prim);
        } else {
            running_to_ready(cur);
        }

        ready_to_running(new);

        co_current_id = new->t_id;

printf("CO_CURRENT_ID: %d\n", co_current_id);

        swap_context((void *) cur->t->context, (void *) new->context);

        return 1;
    }

    printf("DEADLOCK\n");
    exit(0);
    // loop

    return 2;
}



/* only for wakeup & signal */
int
thread_scheduler(void *sync_prim, void *arg, int if_sleep)
{

    /* get current running thread */

    /*thread_t *cur = pool->get_thread(pool, co_current_id);
    assert(cur != NULL);*/

    thread_link_t *cur = pool->t_running;
    assert(cur != NULL);

    if (sync_prim == NULL) {
        return thread_scheduler_ready(cur, sync_prim, arg, if_sleep);
    } else {

#ifdef GREEN_BENCHMARK_BREAKDOWN
rdtsc_start();
#endif

        // thread_link_t *new = switch_waiting(cur, sync_prim, &pool->t_waiting_start, &pool->t_waiting_end);
        thread_link_t *new_linker = extract_waiting(sync_prim, &pool->t_waiting_start, &pool->t_waiting_end);
        /* pick up a ready thread iff no thread waiting on the lock */
        if (new_linker == NULL) {
            return thread_scheduler_ready(cur, sync_prim, arg, if_sleep);
        }

#ifdef GREEN_BENCHMARK_BREAKDOWN
rdtsc_end();
printf("COLLECTION - EXTRACT_WAITING %llu\n", (end - start));
rdtsc_start();
#endif

        thread_t *new = new_linker->t;
        assert(new != NULL);

        if (if_sleep) {
            running_to_waiting(cur, sync_prim);
        } else {
            running_to_ready(cur);
        }

#ifdef GREEN_BENCHMARK_BREAKDOWN
rdtsc_end();
printf("COLLECTION - RUNNING_TO_WAITING %llu\n", (end - start));

rdtsc_start();
#endif

        waiting_to_running(new_linker);

        co_current_id = new->t_id;
printf("CO_CURRENT_ID: %d\n", co_current_id);

#ifdef GREEN_BENCHMARK_BREAKDOWN
rdtsc_end();
printf("COLLECTION - WAITING_TO_RUNNING %llu\n", (end - start));
#endif

        swap_context((void *) cur->t->context, (void *) new_linker->t->context);

        return 0;
    }

    /* version 01: FIFO,waiting thread first */
    // if (pool->waiting_num)
    //     return pool->t_waiting_start->t_id;

    // if (pool->ready_num)
    //     return pool->t_ready_start->t_id;

    // thread_create(pool->start_routine);

    assert(0);
    return -1;
}
#endif



void
print_single_thread(int t_id)
{
    thread_t *t = pool->get_thread(pool, t_id);
    printf("==== Thread ID \t Stack top \t Thread status ====\n");
    printf("==== %d \t\t %p \t\t ", t_id, t);

    switch (t->status) {
        case running:
            printf("running\n");
        break;

        case ready:
            printf("ready\n");
        break;

        case waiting:
            printf("waiting\n");
        break;

        default:
        break;
    }

    printf("=====================\n");

    return;
}



void
thread_destory(thread_t *cur)
{

    free(cur->context);
    free(cur->join);

    // assert(cur->joined_start == NULL && cur->joined_end == NULL);
    free(cur);
    return;
}



void
print_thread_detail()
{
    thread_link_t *cur;
    thread_waiter_t *cur_waiter;
    int count = 0;

    printf("\n");

    printf("==== Sequence \t Thread ID \t Thread Address \t Thread status ====\n");

    /* print out running thread */
    // if (pool->running_num)
        printf("==== #%d \t %d \t\t %p \t\t running\n", 1, pool->t_running->t_id, pool->get_thread(pool, pool->t_running->t_id));

    count += 1;

    /* print out ready threads */
    for (cur = pool->t_ready_start;cur != NULL;cur = cur->next)
        printf("==== #%d \t %d \t\t %p \t\t ready\n", ++ count, cur->t_id, pool->get_thread(pool, cur->t_id));

    /* print out waiting threads */
    for (cur_waiter = pool->t_waiting_start;cur_waiter != NULL;cur_waiter = cur_waiter->next)
        printf("==== #%d \t %d \t\t %p \t\t waiting\n", ++ count, cur_waiter->linker->t_id, pool->get_thread(pool, cur_waiter->linker->t_id));

    return;
}



void *
thread_create_stack(void *stack_top, void * (*func)(void *arg), void *arg)
{
    if (stack_top == NULL) {
        printf("Error: Invalid stack address \n");
        return NULL;
    } else if (!IS_ALIGNED((uintptr_t) stack_top, 4)) {
      /* Stack has to be aligned to 16-bytes */
        printf("Error: Invalid stack alignment. Stack has to be 16-bytes aligned \n");
        return NULL;
    }

    return stack_top;
}



void
print_context(int thread_id, int seq)
{
    thread_t *t = pool->get_thread(pool, thread_id);
    thread_context_t *context = t->context;
    printf("That is %d=>\n", seq);
    printf("EBX: %u\n", context->ebx);
    printf("ESP: %u\n", context->esp);
    printf("EBP: %u\n", context->ebp);
    printf("EDI: %u\n", context->edi);
    printf("ESI: %u\n", context->esi);
    printf("EIP: %u\n", context->eip);

    return;

}



/*w
    Synchronization primitives
*/
void
thread_sleep(void *sync_prim, void *arg)
{
    if (pool->ready_num) {
// rdtsc_start();
        thread_link_t *cur = pool->t_running;
        assert(cur != NULL);

        /* save current thread into waiting queue */
#ifdef GREEN_VERY_FAST
        running_to_lock_waiting(cur, sync_prim);
#else
        running_to_waiting(cur, sync_prim);
#endif
        /* pick up a thread from ready queue */
        assert(pool->t_ready_start != NULL&& pool->t_ready_end != NULL);
        thread_link_t *new = pool->t_ready_start;

        if (pool->t_ready_end == pool->t_ready_start) {
            pool->t_ready_start = NULL;
            pool->t_ready_end = NULL;
        } else {
            pool->t_ready_start = new->next;
        }

        new->t->status = running;
        pool->t_running = new;

        pool->ready_num --;

        co_current_id = new->t->t_id;
// rdtsc_end();
// printf("COLLECTION - sleep %llu\n", (end - start));

// rdtsc_start();
        swap_context((void *) cur->t->context, (void *) new->t->context);
// rdtsc_end();
// printf("COLLECTION - CONTEXT SWITCHING %llu %llu %llu\n", (end - start), start, end);

        return;
    }

    // printf("current thread: %d\n", pool->t_running->t->t_id);
    // printf("Something happened, may be deadlock.\n");
    // exit(0);

    /* Return failture if cannot sleep */

    return;
}

void
thread_wakeup(void *sync_prim, void *arg)
{

// rdtsc_start();
    assert(sync_prim != NULL);


#ifdef GREEN_VERY_FAST
    thread_link_t *new = extract_lock_waiting((thread_sync_prim_t *) sync_prim);
#else
    thread_link_t *new = extract_waiting(sync_prim, &pool->t_waiting_start, &pool->t_waiting_end);
#endif
    /* If nothing to wakeup */
    if (new == NULL) {
        // rdtsc_end();
        // printf("COLLECTION - wakeup early %llu\n", (end - start));


        return;
    }

    // thread_link_t *cur = pool->t_running;
    // assert(cur != NULL);

    running_to_ready(new); 

    // waiting_to_running(new);

    // co_current_id = new->t->t_id;

    // rdtsc_end();
    // printf("COLLECTION - wakeup full %llu\n", (end - start));

// rdtsc_start();
    // swap_context((void *) cur->t->context, (void *) new->t->context);
// rdtsc_end();
// printf("COLLECTION - CONTEXT SWITCHING %llu %llu %llu\n", (end - start), start, end);
    return;
}



void
thread_wakeup_wait(void *sync_prim)
{

    assert(sync_prim != NULL);
    thread_link_t *cur = pool->t_running;

    assert(cur != NULL);

#ifdef GREEN_VERY_FAST
    thread_link_t *new = extract_lock_waiting((thread_sync_prim_t *) sync_prim);
#else
    thread_link_t *new = extract_waiting(sync_prim, &pool->t_waiting_start, &pool->t_waiting_end);
#endif

    /* If nothing to wakeup */
    if (new == NULL) {
        return;
    }

#ifdef GREEN_VERY_FAST
    running_to_lock_waiting(cur, (thread_sync_prim_t *) sync_prim);
#else
    running_to_waiting(cur, sync_prim);
#endif

    waiting_to_running(new);

    co_current_id = new->t->t_id;

// rdtsc_start();

    swap_context((void *) cur->t->context, (void *) new->t->context);

// rdtsc_end();
// printf("COLLECTION - CONTEXT SWITCHING %llu %llu %llu\n", (end - start), start, end);

    return;
}



// void
// thread_sleep(void *sync_prim, void *arg)
// {
//
// }




thread_pool_t *
thread_initial()
{
    pool = malloc(sizeof(thread_pool_t));
    if (pool == NULL) {
        printf("Error: Failed to allocate memory for pool.\n");
        return NULL;
    }

    /* Assign thread 0 - current running thread [main] */
    thread_t *t = malloc(sizeof(thread_t));
    if (t == NULL) {
        printf("Error: Cannot allocate a new thread.\n");
        return NULL;
    }

    t->context = malloc(sizeof(thread_context_t));
    if (t->context == NULL) {
        printf("Error: Cannot malloc context.\n");
        return 0;
    }

    t->join = NULL;
    t->t_id = 0;

    co_current_id = 0;
    co_current = t;

    thread_link_t *linker = malloc(sizeof(thread_link_t));
    if (linker == NULL) {
        printf("Error: Cannot malloc a new thread linker.\n");
        return 0;
    }

    linker->t = t;
    linker->next = NULL;

    pool->new_id = 1;

    pool->num = 1;
    pool->ready_num = 0;
    pool->waiting_num = 0;

    pool->t_running = linker;
    pool->t_ready_start = NULL;
    pool->t_ready_end = NULL;
    pool->t_waiting_start = NULL;
    pool->t_waiting_end = NULL;
    pool->sync_prim_start = NULL;
    pool->sync_prim_end = NULL;

    // pool->t_join_start = NULL;
    // pool->t_join_end = NULL;

    int i = 0;
    for (i = 0;i < MAX_CAPACITY;i ++)
        pool->addrs[i] = NULL;

    pool->addrs[0] = t;

    pool->get_thread = get_thread;
    pool->get_new_id = get_new_id;
    pool->set_reply_ep = set_reply_ep;
    pool->get_reply_ep = get_reply_ep;

    initialised = 1;

    return pool;
}



int
thread_create(allocman_t *allocman, vspace_t *vspace, void *(* func)(void *), void *arg)
{

    int error;

    if (! initialised) {
        printf("Error: Thread pool has not been initialised.\n");
        return 0;
    }

     thread_t *t = malloc(sizeof(thread_t));
     if (t == NULL) {
         printf("Error: Failed to allocate a new thread.\n");
         return 0;
     }

     /* Initialise join */
     t->join = NULL;
     t->joined_start = NULL;
     t->joined_end = NULL;

    /* Assign id to the new thread */
     int t_id = pool->get_new_id(pool);
     if (! t_id) {
         printf("Error: Already exceed the thread limitation. No more thread can be created.\n");
         return 0;
     }

     t->t_id = t_id;
     t->status = ready;
     t->start_routine = func;
     t->arg = arg;
     t->if_defer = 0;

     error = allocman_cspace_alloc(allocman, &t->slot);
     assert(error == 0);

     t->context = malloc(sizeof(thread_context_t));
     if (t->context == NULL) {
         printf("Error: Cannot malloc context.\n");
         return 0;
     }

     void *stack_top = vspace_new_stack(vspace);
     if (stack_top == NULL) {
         printf("Error: Failed to allocate new stack.\n");
         return 0;
     }

     /* assign instruction pointer */
     void *stack_base = stack_top - 65536;

     *(uint32_t *)stack_base = (uint32_t) t->context;

     stack_push(&stack_top, (uint32_t) func);
     stack_push(&stack_top, (uint32_t) arg);

     /* assign new stack */
     t->stack_top = stack_top;
     t->context->esp = (uint32_t) stack_top;

     t->context->eip = (uint32_t) bouncer;

     t->join = NULL;

     /* Add new thread to pool */
     pool->addrs[t_id] = t;

     /* create a new thread linker */
     thread_link_t *link = malloc(sizeof(thread_link_t));
     if (link == NULL) {
         printf("Error: Failed to allocate a new thread linker.\n");
         return 0;
     }

     link->t = t;
     link->next = NULL;

     pool->num ++;
     pool->ready_num ++;

     if (pool->t_ready_end == NULL) {
         pool->t_ready_start = link;
         pool->t_ready_end = link;
     } else {
         pool->t_ready_end->next = link;
         pool->t_ready_end = link;
     }

     return t_id;
}



int
thread_exit() {

    thread_t *cur = pool->get_thread(pool, co_current_id);
    assert(cur != NULL);

    /* inform all the caller that the join is going to exit */
    thread_join_link_t *cur_join = cur->joined_start;
    for (cur_join = cur->joined_start;cur_join != NULL;cur_join = cur_join->next)
        cur_join->joinable = 1;

    // while (cur->joined_start != NULL) {
    //     printf("test\n");
    //     thread_yield();
    //     printf("exit\n");
    // }

    /* clean up the thread and exit */
    // printf("That's the end of thread %d\n", co_current_id);

    pool->addrs[cur->t_id] = NULL;

    thread_destory(cur);

    pool->num --;

    int t_id;
    if (pool->ready_num)
        t_id = pool->t_ready_start->t->t_id;
    else
        t_id = 0;

    /* get new thread */
    thread_t *new = pool->get_thread(pool, t_id);

    /* set current running thread id */
    co_current_id = t_id;

    if (pool->ready_num)
        ready_to_running(new);
    else
        new->status = running;

    switch_context((void *) new->context);

    return 1;
}



int
thread_yield()
{

    if (pool->ready_num)
        return thread_resume(pool->t_ready_start->t->t_id);
    else
        return thread_resume(0);
}



int
thread_resume(int t_id)
{

    assert(pool->t_running->t->t_id != t_id);
    /* get current thread */
    thread_t *cur = pool->t_running->t;

    /* get new thread */
    thread_t *new = pool->get_thread(pool, t_id);
    assert(new != NULL);

    if (pool->t_running->t->t_id)
        running_to_ready(pool->t_running);
    else
        cur->status = ready;

    /* set current running thread id */
    co_current_id = t_id;

    if (t_id)
        assert(ready_to_running(new));
    else
        new->status = running;

    swap_context((void *) cur->context, (void *) new->context);

    return 1;
}




int
thread_join(int thread_id)
{
    thread_t *cur = pool->get_thread(pool, co_current_id);
    assert(cur->join == NULL);

    thread_t *new = pool->get_thread(pool, thread_id);
    if (new == NULL) {
        if (thread_id >= pool->new_id) {
            printf("Thread %d does not exist. Cannot join!\n", thread_id);
            assert(0);
        } else {
            return 1;
        }
    }

    cur->join = malloc(sizeof(thread_join_link_t));
    if (cur->join == NULL) {
        printf("Error: Cannnot malloc a new join block.\n");
        return 0;
    }

    cur->join->caller = pool->t_running->t->t_id;
    cur->join->joiner = thread_id;
    cur->join->joinable = 0;
    append_join_block(cur->join, &new->joined_start, &new->joined_end);

    while (! cur->join->joinable) {

        if (new->status == ready) {
            thread_resume(thread_id);
        } else if (new->status == waiting) {
            thread_yield();
        } else {
            printf("Thread %d cannot join itself!\n", thread_id);
            exit(0);
        }

        // if (new->status != waiting) {
            /* yi bo ling ren zhi xi de cao zuo */
            // thread_resume(thread_id);
        // }
    }

    /* cancel join block inside join thread */
    delete_join_block(cur->join, &new->joined_start, &new->joined_end);

    cur->join = NULL;

    return 1;
}




void
thread_pool_info(int detail)
{

    /* print out information for thread pool */
    printf("==== Thread pool ====\n");
    printf("==== %d threads in total\n", pool->num);

    for (int i = 1;i < pool->num;i ++) {
        thread_info(i);
    }

    //_t
    // printf("==== %d threads in running state\n", 1);
    // printf("==== %d threads in ready state\n", pool->ready_num);
    // printf("==== %d threads in waiting state\n", pool->waiting_num);

    // if (detail)
        // print_thread_detail();

    assert(detail == 1);

    printf("=====================\n");

    return;
}



void
thread_info(int t_id)
{
    assert(t_id > 0);

    printf("==== Thread %d ====\n", t_id);

    thread_t *t = pool->get_thread(pool, t_id);

    printf("==== Thread Address: %p\n", t);

    char status[20];
    switch (t->status) {
        case running:
            strcpy(status, "running");
        break;

        case ready:
            strcpy(status, "ready");
        break;

        case waiting:
            strcpy(status, "waiting");
        break;
    }

    printf("==== Thread Status: %s\n", status);

    printf("=====================\n");

    return;
}



void
thread_self()
{

    print_single_thread(co_current_id);

    return;
}
