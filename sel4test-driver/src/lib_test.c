#include <stdio.h>
#include <stdlib.h>

#include "lib_test.h"



void
sel4_thread_new (int index, sel4utils_thread_t *tcb)
{
    sel4_threads[index ++] = tcb;
}



void
sel4_thread_initial ()
{
    sel4_thread_num = 0;
}



int
client_barrier ()
{
    return client_num == client_new;
}



void
initial_client_pool (int num)
{
    client_new = 0;
    client_num = num;

    server_count = 0;

    test_seq = 0;

    terminate_num = 0;

    started = 0;

    even = 0;

#ifdef CLIENT_SINGLE
    order_i_cur = 0;
    order_j_cur = 0;
#endif

#ifdef IMMD_DEFER
    lock_global = thread_lock_create();
    assert(lock_global != NULL);

    inv_count = 0;
#endif

#ifdef GREEN_THREAD
    lock_universe = thread_lock_create();
    assert(lock_universe != NULL);

    lock_universe->held = 0;
#endif

#ifdef CONSUMER_PRODUCER
    buffer = 0;
    buffer_limit = 1;

    wait_count = num / 2;

#ifdef BENCHMARK_BREAKDOWN_BEFORE
    before_cur = 0;
#endif

#ifdef BENCHMARK_BREAKDOWN_IPC
    ipc_cur =  0;
    memset(ipc, 0, 6000 * sizeof(uint64_t)); 

    // ipc_test_cur = 0;

    // memset(ipc_As, 0, 6000 * sizeof(uint64_t));
    // memset(ipc_Bs, 0, 6000 * sizeof(uint64_t));
    // memset(ipc_Cs, 0, 6000 * sizeof(uint64_t));
    // memset(ipc_Ds, 0, 6000 * sizeof(uint64_t));
    // memset(ipc_Es, 0, 6000 * sizeof(uint64_t));
    // memset(ipc_Fs, 0, 6000 * sizeof(uint64_t));
    // memset(ipc_Gs, 0, 6000 * sizeof(uint64_t));
    // memset(ipc_Hs, 0, 6000 * sizeof(uint64_t));
    // memset(ipc_Is, 0, 6000 * sizeof(uint64_t));
    // memset(ipc_Js, 0, 6000 * sizeof(uint64_t));

#endif

#ifdef GREEN_THREAD

    lock_global = thread_lock_create();
    assert(lock_global != NULL);
    lock_global->held = 0;

    producer_list = thread_lock_create();
    assert(producer_list != NULL);
    producer_list->held = 0;

    consumer_list = thread_lock_create();
    assert(consumer_list != NULL);
    consumer_list->held = 0;


#endif
#endif

#ifdef EVENT_CONTINUATION
    memset(producer_continuations, 0, MAX_CONTI * sizeof(continuation_t));
    memset(consumer_continuations, 0, MAX_CONTI * sizeof(continuation_t));

    producer_start_conti = -1;
    consumer_start_conti = -1;

    producer_last_conti = 0;
    consumer_last_conti = 0;

#endif

}



int
get_new_server_id()
{
    return server_count ++;
}



int
initial_client ()
{
    return client_new ++;
}
