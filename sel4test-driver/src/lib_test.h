#include <stdio.h>
#include <stdlib.h>

#include <sel4utils/vspace.h>
#include <sel4utils/stack.h>
#include <sel4utils/process.h>

#include <sync/mutex.h>

#include <platsupport/plat/timer.h>
#include <sel4platsupport/plat/timer.h>
#include <sel4platsupport/arch/io.h>
#include <sel4platsupport/platsupport.h>
#include <sel4platsupport/timer.h>

#include "thread_lib.h"


/* Added */
#define WAIT 2000
#define SEND_WAIT 2001
#define IMMD 2002
#define INIT 2003
#define TMNT 2004
#define PRODUCER 2005
#define CONSUMER 2006
#define DEFER 2007
#define PRODUCER_CONTI 2008
#define CONSUMER_CONTI 2009
#define HELPER 2010


#define CLIENT_MAX 10000

// #define BENCHMARK_BREAKDOWN_BEFORE
// #define BENCHMARK_BREAKDOWN_MID
// #define BENCHMARK_BREAKDOWN_IPC
#define BENCHMARK_BREAKDOWN_SIGNAL
#define BENCHMARK_ENTIRE

// #define DEMO

#define CLIENT_MULTIPLE
// #define CLIENT_SINGLE

#define WAIT_SEND_WAIT
// #define LIGHT_SEND_WAIT
// #define CONSUMER_PRODUCER
// #define IMMD_DEFER
// #define EVENT_DRIVEN
// #define VARY_SYNC_PRIMS

/* configuration for different modes */
// #define SEL4_THREAD
#define GREEN_THREAD
// #define EVENT_CONTINUATION

#define THREAD_LOCK
// #define THREAD_SEMAPHORE
// #define THREAD_CV

// #define SEL4_SLOW
#define SEL4_FAST
// #define SEL4_GREEN

/* Variables for test */
int test_seq;
int client_count;
int server_count;
int terminate_num;
unsigned signal_start_cycles_high, signal_start_cycles_low, signal_end_cycles_high, signal_end_cycles_low;
uint64_t UNUSED signal_start, UNUSED signal_end;
int started;

seL4_Word client_ep;
int even;

#ifdef IMMD_DEFER
thread_lock_t *lock_global;
seL4_timer_t *timer;
#endif

#ifdef EVENT_DRIVEN
seL4_timer_t *timer;
#endif

#ifdef WAIT_SEND_WAIT
#ifdef SEL4_THREAD
seL4_CPtr tokenps[1000];
sync_mutex_t tokens[1000];
#endif
#ifdef GREEN_THREAD
thread_lock_t *tokens[1000];
#endif
#endif

/* Vars for testing */
int wait_count;

/* Configuration for consumer-producer */
#ifdef CONSUMER_PRODUCER

int buffer;
int buffer_limit;

#ifdef GREEN_THREAD
thread_lock_t *lock_global;
thread_lock_t *producer_list;
thread_lock_t *consumer_list;
#endif

/* Collect experiment data */
#ifdef BENCHMARK_BREAKDOWN_BEFORE
uint64_t before[6000];
int before_cur;
#endif

uint64_t ipc[6000];
int ipc_cur;
#ifdef BENCHMARK_BREAKDOWN_IPC
#ifdef CLIENT_SINGLE
uint64_t ipc_half[6000];
int ipc_half_cur;

uint64_t ipc_starts[6000];
uint64_t ipc_ends[6000];
uint64_t ipc_half_starts[6000];
uint64_t ipc_half_ends[6000];

uint64_t ipc_As[6000];
uint64_t ipc_Bs[6000];
uint64_t ipc_Cs[6000];
uint64_t ipc_Ds[6000];
uint64_t ipc_Es[6000];
uint64_t ipc_Fs[6000];
uint64_t ipc_Gs[6000];
uint64_t ipc_Hs[6000];
uint64_t ipc_Is[6000];
uint64_t ipc_Js[6000];

int ipc_test_cur;

#endif
#endif

#ifdef SEL4_THREAD
sync_mutex_t lock_global;
seL4_CPtr notification;
seL4_CPtr producer_list;
seL4_CPtr consumer_list;
#endif // -- SEL4_THREAD

#endif // -- CONSUMER_PRODUCER

/* Universal lock for CV */
thread_lock_t *lock_universe;


/* Helper functions for sel4 thread */
sel4utils_thread_t *sel4_threads[CLIENT_MAX];
int sel4_thread_id_new;
int sel4_thread_num;
seL4_Word global_reply_ep;

void sel4_thread_initial();
void sel4_thread_new();

int get_new_server_id();


/* Helper functions for user-level thread */
int client_new;
int client_num;

int client_barrier();
void initial_client_pool(int num);
int initial_client();

#ifdef CLIENT_SINGLE
int order_is[6000];
int order_js[6000];

int order_i_cur;
int order_j_cur;
#endif

#ifdef EVENT_CONTINUATION
#define MAX_CONTI 1000

struct continuation_t {
    int order_i;
    int order_j;

    int occupied;
};
typedef struct continuation_t continuation_t;

continuation_t producer_continuations[MAX_CONTI];
continuation_t consumer_continuations[MAX_CONTI];

int producer_last_conti;
int consumer_last_conti;

int producer_start_conti;
int consumer_start_conti;

seL4_Word helper_ep;
#endif
