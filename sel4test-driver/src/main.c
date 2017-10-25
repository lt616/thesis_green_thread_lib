/*
 * Copyright 2017, Data61
 * Commonwealth Scientific and Industrial Research Organisation (CSIRO)
 * ABN 41 687 119 230.
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(DATA61_BSD)
 */

/* Include Kconfig variables. */
#include <autoconf.h>

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <limits.h>

#include <allocman/bootstrap.h>
#include <allocman/vka.h>

#include <sel4platsupport/timer.h>
#include <sel4platsupport/plat/serial.h>

#include <sel4debug/register_dump.h>
#include <sel4platsupport/device.h>
#include <sel4platsupport/platsupport.h>
#include <sel4utils/vspace.h>
#include <sel4utils/stack.h>
#include <sel4utils/process.h>

#include <simple/simple.h>
#include <simple-default/simple-default.h>

#include <utils/util.h>

#include <vka/object.h>
#include <vka/capops.h>
#include <vka/object_capops.h>

#include <sel4/types_gen.h>

#include <vspace/vspace.h>

#include "test.h"

#define TESTS_APP "sel4test-tests"

#include "lib_test.h"

#include <sync/mutex.h>
#include <sync/sem.h>
#include <sync/condition_var.h>
#include <sync/bin_sem.h>
#include <sel4bench/sel4bench.h>
#include <platsupport/plat/timer.h>
// #include <sel4platsupport/plat/timer.h>
// #include <sel4platsupport/arch/io.h>
// #include <sel4platsupport/platsupport.h>
// #include <sel4platsupport/timer.h>



/* ammount of untyped memory to reserve for the driver (32mb) */
#define DRIVER_UNTYPED_MEMORY (1 << 27)
/* Number of untypeds to try and use to allocate the driver memory.
 * if we cannot get 32mb with 16 untypeds then something is probably wrong */
#define DRIVER_NUM_UNTYPEDS 16

/* dimensions of virtual memory for the allocator to use */
#define ALLOCATOR_VIRTUAL_POOL_SIZE ((1 << seL4_PageBits) * 1500)

/* static memory for the allocator to bootstrap with */
#define ALLOCATOR_STATIC_POOL_SIZE ((1 << seL4_PageBits) * 20)
static char allocator_mem_pool[ALLOCATOR_STATIC_POOL_SIZE];

/* static memory for virtual memory bootstrapping */
static sel4utils_alloc_data_t data;

/* environment encapsulating allocation interfaces etc */
static struct env env;
/* the number of untyped objects we have to give out to processes */
static int num_untypeds;
/* list of untypeds to give out to test processes */
static vka_object_t untypeds[CONFIG_MAX_NUM_BOOTINFO_UNTYPED_CAPS];
/* list of sizes (in bits) corresponding to untyped */
static uint8_t untyped_size_bits_list[CONFIG_MAX_NUM_BOOTINFO_UNTYPED_CAPS];

allocman_t *allocman;
seL4_Word reply_eps[CLIENT_MAX];

#define IPCBUF_FRAME_SIZE_BITS 12
#define IPCBUF_VADDR 0x7000000



seL4_CPtr ep_object;

/* initialise our runtime environment */
static void
init_env(env_t env)
{
    reservation_t virtual_reservation;
    int error;

    /* create an allocator */
    allocman = bootstrap_use_current_simple(&env->simple, ALLOCATOR_STATIC_POOL_SIZE, allocator_mem_pool);
    if (allocman == NULL) {
        ZF_LOGF("Failed to create allocman");
    }

    /* create a vka (interface for interacting with the underlying allocator) */
    allocman_make_vka(&env->vka, allocman);

    /* create a vspace (virtual memory management interface). We pass
     * boot info not because it will use capabilities from it, but so
     * it knows the address and will add it as a reserved region */
    error = sel4utils_bootstrap_vspace_with_bootinfo_leaky(&env->vspace,
                                                           &data, simple_get_pd(&env->simple),
                                                           &env->vka, platsupport_get_bootinfo());
    if (error) {
        ZF_LOGF("Failed to bootstrap vspace");
    }

    /* fill the allocator with virtual memory */
    void *vaddr;
    virtual_reservation = vspace_reserve_range(&env->vspace,
                                               ALLOCATOR_VIRTUAL_POOL_SIZE, seL4_AllRights, 1, &vaddr);
    if (virtual_reservation.res == 0) {
        ZF_LOGF("Failed to provide virtual memory for allocator");
    }

    bootstrap_configure_virtual_pool(allocman, vaddr,
                                     ALLOCATOR_VIRTUAL_POOL_SIZE, simple_get_pd(&env->simple));

    error = vka_alloc_endpoint(&env->vka, &env->endpoint);
    ZF_LOGF_IFERR(error, "Failed to allocate new endpoint object.\n");

    env->client_num ++;

}



/* Free a list of objects */
static void
free_objects(vka_object_t *objects, unsigned int num)
{
    for (unsigned int i = 0; i < num; i++) {
        vka_free_object(&env.vka, &objects[i]);
    }
}

/* Allocate untypeds till either a certain number of bytes is allocated
 * or a certain number of untyped objects */
static unsigned int
allocate_untypeds(vka_object_t *untypeds, size_t bytes, unsigned int max_untypeds)
{
    unsigned int num_untypeds = 0;
    size_t allocated = 0;

    /* try to allocate as many of each possible untyped size as possible */
    for (uint8_t size_bits = seL4_WordBits - 1; size_bits > PAGE_BITS_4K; size_bits--) {
        /* keep allocating until we run out, or if allocating would
         * cause us to allocate too much memory*/
        while (num_untypeds < max_untypeds &&
               allocated + BIT(size_bits) <= bytes &&
               vka_alloc_untyped(&env.vka, size_bits, &untypeds[num_untypeds]) == 0) {
            allocated += BIT(size_bits);
            num_untypeds++;
        }
    }
    return num_untypeds;
}

/* extract a large number of untypeds from the allocator */
static unsigned int
populate_untypeds(vka_object_t *untypeds)
{
    /* First reserve some memory for the driver */
    vka_object_t reserve[DRIVER_NUM_UNTYPEDS];
    unsigned int reserve_num = allocate_untypeds(reserve, DRIVER_UNTYPED_MEMORY, DRIVER_NUM_UNTYPEDS);

    /* Now allocate everything else for the tests */
    unsigned int num_untypeds = allocate_untypeds(untypeds, UINT_MAX, ARRAY_SIZE(untyped_size_bits_list));
    /* Fill out the size_bits list */
    for (unsigned int i = 0; i < num_untypeds; i++) {
        untyped_size_bits_list[i] = untypeds[i].size_bits;
    }

    /* Return reserve memory */
    free_objects(reserve, reserve_num);

    /* Return number of untypeds for tests */
    if (num_untypeds == 0) {
        ZF_LOGF("No untypeds for tests!");
    }

    return num_untypeds;
}

/* copy untyped caps into a processes cspace, return the cap range they can be found in */
static seL4_SlotRegion
copy_untypeds_to_process(sel4utils_process_t *process, vka_object_t *untypeds, int num_untypeds)
{
    seL4_SlotRegion range = {0};

    for (int i = 0; i < num_untypeds; i++) {
        seL4_CPtr slot = sel4utils_copy_cap_to_process(process, &env.vka, untypeds[i].cptr);

        /* set up the cap range */
        if (i == 0) {
            range.start = slot;
        }
        range.end = slot;
    }
    assert((range.end - range.start) + 1 == num_untypeds);
    return range;
}

/* map the init data into the process, and send the address via ipc */
static void *
send_init_data(env_t env, seL4_CPtr endpoint, sel4utils_process_t *process)
{
    /* map the cap into remote vspace */
    void *remote_vaddr = vspace_map_pages(&process->vspace, &env->init_frame_cap_copy, NULL, seL4_AllRights, 1, PAGE_BITS_4K, 1);
    assert(remote_vaddr != 0);

    /* now send a message telling the process what address the data is at */
    /* seL4_MessageInfo_t info = seL4_MessageInfo_new(seL4_Fault_NullFault, 0, 0, 1);
    seL4_SetMR(0, (seL4_Word) remote_vaddr);
    seL4_Send(endpoint, info); */

    return remote_vaddr;
}

/* copy the caps required to set up the sel4platsupport default timer */
static void
copy_timer_caps(test_init_data_t *init, env_t env, sel4utils_process_t *test_process)
{
    /* Copy PS default timer's IRQ cap to child process. */
    init->timer_irq_cap = sel4utils_copy_cap_to_process(test_process, &env->vka, env->timer_objects.timer_irq_path.capPtr);
    ZF_LOGF_IF(init->timer_irq_cap == 0,
               "Failed to copy PS default timer IRQ cap to test child "
               "process.");

    /* untyped cap for timer device untyped */
    init->timer_paddr = env->timer_objects.timer_paddr;
    init->timer_dev_ut_cap = sel4utils_copy_cap_to_process(test_process, &env->vka, env->timer_objects.timer_dev_ut_obj.cptr);
    ZF_LOGF_IF(init->timer_dev_ut_cap == 0,
               "Failed to copy PS default timer device-ut to test child.");

    arch_copy_timer_caps(init, env, test_process);
}

static void
copy_serial_caps(test_init_data_t *init, env_t env, sel4utils_process_t *test_process)
{
    init->serial_irq_cap = sel4utils_copy_cap_to_process(test_process, &env->vka,
                                               env->serial_objects.serial_irq_path.capPtr);
    ZF_LOGF_IF(init->serial_irq_cap == 0,
               "Failed to copy PS default serial IRQ cap to test child "
               "process.");

    arch_copy_serial_caps(init, env, test_process);
}

/* Run a single test.
 * Each test is launched as its own process. */
int
run_test(struct testcase *test)
{
    UNUSED int error;
    sel4utils_process_t test_process;

    /* Test intro banner. */
    printf("  %s\n", test->name);

    error = sel4utils_configure_process(&test_process, &env.vka, &env.vspace,
                                        env.init->priority, TESTS_APP);
    assert(error == 0);

    /* set up caps about the process */
    env.init->stack_pages = CONFIG_SEL4UTILS_STACK_SIZE / PAGE_SIZE_4K;
    env.init->stack = test_process.thread.stack_top - CONFIG_SEL4UTILS_STACK_SIZE;
    env.init->page_directory = sel4utils_copy_cap_to_process(&test_process, &env.vka, test_process.pd.cptr);
    env.init->root_cnode = SEL4UTILS_CNODE_SLOT;
    env.init->tcb = sel4utils_copy_cap_to_process(&test_process, &env.vka, test_process.thread.tcb.cptr);
    env.init->domain = sel4utils_copy_cap_to_process(&test_process, &env.vka, simple_get_init_cap(&env.simple, seL4_CapDomain));
    env.init->asid_pool = sel4utils_copy_cap_to_process(&test_process, &env.vka, simple_get_init_cap(&env.simple, seL4_CapInitThreadASIDPool));
    env.init->asid_ctrl = sel4utils_copy_cap_to_process(&test_process, &env.vka, simple_get_init_cap(&env.simple, seL4_CapASIDControl));
#ifdef CONFIG_IOMMU
    env.init->io_space = sel4utils_copy_cap_to_process(&test_process, &env.vka, simple_get_init_cap(&env.simple, seL4_CapIOSpace));
#endif /* CONFIG_IOMMU */
#ifdef CONFIG_ARM_SMMU
    env.init->io_space_caps = arch_copy_iospace_caps_to_process(&test_process, &env);
#endif
    env.init->cores = simple_get_core_count(&env.simple);
    /* setup data about untypeds */
    env.init->untypeds = copy_untypeds_to_process(&test_process, untypeds, num_untypeds);
    copy_timer_caps(env.init, &env, &test_process);
    copy_serial_caps(env.init, &env, &test_process);
    /* copy the fault endpoint - we wait on the endpoint for a message
     * or a fault to see when the test finishes */
    seL4_CPtr endpoint = sel4utils_copy_cap_to_process(&test_process, &env.vka, test_process.fault_endpoint.cptr);

    /* WARNING: DO NOT COPY MORE CAPS TO THE PROCESS BEYOND THIS POINT,
     * AS THE SLOTS WILL BE CONSIDERED FREE AND OVERRIDDEN BY THE TEST PROCESS. */
    /* set up free slot range */
    env.init->cspace_size_bits = CONFIG_SEL4UTILS_CSPACE_SIZE_BITS;
    env.init->free_slots.start = endpoint + 1;
    env.init->free_slots.end = (1u << CONFIG_SEL4UTILS_CSPACE_SIZE_BITS);
    assert(env.init->free_slots.start < env.init->free_slots.end);
    /* copy test name */
    strncpy(env.init->name, test->name + strlen("TEST_"), TEST_NAME_MAX);
    /* ensure string is null terminated */
    env.init->name[TEST_NAME_MAX - 1] = '\0';
#ifdef CONFIG_DEBUG_BUILD
    seL4_DebugNameThread(test_process.thread.tcb.cptr, env.init->name);
#endif

    /* set up args for the test process */
    char endpoint_string[WORD_STRING_SIZE];
    char sel4test_name[] = { TESTS_APP };
    char *argv[] = {sel4test_name, endpoint_string};
    snprintf(endpoint_string, WORD_STRING_SIZE, "%lu", (unsigned long)endpoint);
    /* spawn the process */
    error = sel4utils_spawn_process_v(&test_process, &env.vka, &env.vspace,
                            ARRAY_SIZE(argv), argv, 1);
    assert(error == 0);

    /* send env.init_data to the new process */
    void *remote_vaddr = send_init_data(&env, test_process.fault_endpoint.cptr, &test_process);

    /* wait on it to finish or fault, report result */
    seL4_MessageInfo_t info = seL4_Recv(test_process.fault_endpoint.cptr, NULL);

    int result = seL4_GetMR(0);
    if (seL4_MessageInfo_get_label(info) != seL4_Fault_NullFault) {
        sel4utils_print_fault_message(info, test->name);
        sel4debug_dump_registers(test_process.thread.tcb.cptr);
        result = FAILURE;
    }

    /* unmap the env.init data frame */
    vspace_unmap_pages(&test_process.vspace, remote_vaddr, 1, PAGE_BITS_4K, NULL);

    /* reset all the untypeds for the next test */
    for (int i = 0; i < num_untypeds; i++) {
        cspacepath_t path;
        vka_cspace_make_path(&env.vka, untypeds[i].cptr, &path);
        vka_cnode_revoke(&path);
    }

    /* destroy the process */
    sel4utils_destroy_process(&test_process, &env.vka);

    test_assert(result == SUCCESS);
    return result;
}


/* Run a client process.
 * Modification based on run_    seL4_MessageInfo_t info = seL4_MessageInfo_new(seL4_Fault_NullFault, 0, 0, 1);
test() */
int
run_test_new(char *client_name)
{
    UNUSED int error;
    sel4utils_process_t test_process;

    /* Test intro banner. */
    printf("  %s\n", client_name);

    error = sel4utils_configure_process(&test_process, &env.vka, &env.vspace,
                                        env.init->priority, TESTS_APP);
    assert(error == 0);

    /* set up caps about the process */
    /* env.init->stack_pages = CONFIG_SEL4UTILS_STACK_SIZE / PAGE_SIZE_4K;
    env.init->stack = test_process.thread.stack_top - CONFIG_SEL4UTILS_STACK_SIZE;
    env.init->page_directory = sel4utils_copy_cap_to_process(&test_process, &env.vka, test_process.pd.cptr);
    env.init->root_cnode = SEL4UTILS_CNODE_SLOT;
    env.init->tcb = sel4utils_copy_cap_to_process(&test_process, &env.vka, test_process.thread.tcb.cptr);
    env.init->domain = sel4utils_copy_cap_to_process(&test_process, &env.vka, simple_get_init_cap(&env.simple, seL4_CapDomain));
    env.init->asid_pool = sel4utils_copy_cap_to_process(&test_process, &env.vka, simple_get_init_cap(&env.simple, seL4_CapInitThreadASIDPool));
    env.init->asid_ctrl = sel4utils_copy_cap_to_process(&test_process, &env.vka, simple_get_init_cap(&env.simple, seL4_CapASIDControl));
#ifdef CONFIG_IOMMU
    env.init->io_space = sel4utils_copy_cap_to_process(&test_process, &env.vka, simple_get_init_cap(&env.simple, seL4_CapIOSpace));
#endif *//* CONFIG_IOMMU */
/*#ifdef CONFIG_ARM_SMMU
    env.init->io_space_caps = arch_copy_iospace_caps_to_process(&test_process, &env);
#endif
    env.initthread_initial->cores = simple_get_core_count(&env.simple);
    *//* setup data about untypeds */
//    env.init->untypeds = copy_untypeds_to_process(&test_process, untypeds, num_untypeds);
//    copy_timer_caps(env.init, &env, &test_process);
//    copy_serial_caps(env.init, &env, &test_process);
    /* copy the fault endpoint - we wait on the endpoint for a message
     * or a fault to see when the test finishes */
    //seL4_CPtr endpoint = sel4utils_copy_cap_to_process(&test_process, &env.vka, env.client_cptr);

    cspacepath_t ep_cap_path;
    seL4_CPtr endpoint;
    vka_cspace_make_path(&env.vka, env.endpoint.cptr, &ep_cap_path);

    endpoint = sel4utils_mint_cap_to_process(&test_process, ep_cap_path, seL4_AllRights, seL4_CapData_Badge_new(0X61));
    printf("initial: %d %d\n", endpoint, env.endpoint.cptr);
    /* WARNING: DO NOT COPY MORE CAPS TO THE PROCESS BEYOND THIS POINT,
     * AS THE SLOTS WILL BE CONSIDERED FREE AND OVERRIDDEN BY THE TEST PROCESS. */
    /* set up free slot range */
    //env.init->cspace_size_bits = CONFIG_SEL4UTILS_CSPACE_SIZE_BITS;
    //env.init->free_slots.start = endpoint + 1;
    //env.init->free_slots.end = (1u << CONFIG_SEL4UTILS_CSPACE_SIZE_BITS);
    //assert(env.init->free_slots.start < env.init->free_slots.end);
    /* copy test name */
    //strncpy(env.init->name, client_name, TEST_NAME_MAX);
    /* ensure string is null terminated */
    //env.init->name[TEST_NAME_MAX - 1] = '\0';
//#ifdef CONFIG_DEBUG_BUILD
//    seL4_DebugNameThread(test_process.thread.tcb.cptr, env.init->name);
//#endif

    /* set up args for the test process */
    char endpoint_string[WORD_STRING_SIZE];
    char sel4test_name[] = { TESTS_APP };
    char *argv[] = {sel4test_name, endpoint_string};
    snprintf(endpoint_string, WORD_STRING_SIZE, "%lu", (unsigned long)endpoint);

    /* spawn the process */
    error = sel4utils_spawn_process_v(&test_process, &env.vka, &env.vspace,
                            ARRAY_SIZE(argv), argv, 1);
    assert(error == 0);

    /* send env.init_data to the new process */
    void *remote_vaddr = send_init_data(&env, test_process.fault_endpoint.cptr, &test_process);

    /* wait on it to finish or fault, report result */

    /* unmap the env.init data frame */
    vspace_unmap_pages(&test_process.vspace, remote_vaddr, 1, PAGE_BITS_4K, NULL);

    /* reset all the untypeds for the next test */
    for (int i = 0; i < num_untypeds; i++) {
        cspacepath_t path;
        vka_cspace_make_path(&env.vka, untypeds[i].cptr, &path);
        vka_cnode_revoke(&path);
    }

    /* destroy the process */
    //sel4utils_destroy_process(&test_process, &env.vka);

    //test_assert(result == SUCCESS);
    return SUCCESS;
}



#ifdef LIGHT_SEND_WAIT
void light_send_wait() {
    int id, id_want;
    // seL4_CPtr token_want, token_provide;

    id = get_new_server_id();
    id_want = (id == client_count - 1) ? 0 : id + 1;

    // token_want = tokenps[id_want];
    // token_provide = tokenps[id];

    // printf("server %d starts\n", id);
    seL4_Yield();

if (id == 1)
    start = rdtsc();
    // printf("server %d confirms\n", id);
    seL4_Yield();

if (id == 1)
    end = rdtsc();



    if (id == 0) {
#ifdef BENCHMARK_ENTIRE
// printf("start\n");
// start = rdtsc();
#endif
        // printf("server %d lock %d\n", id, token_want);
        while (! tokenms[id_want]) {
            // printf("server %d unlock %d\n", id, token_provide);
            seL4_Yield();
        }

        tokenms[id_want] = 0;
        // sync_mutex_lock(token_want);
        // seL4_Wait(token_want, NULL);
    }
    // printf("COLLECTION - half: %llu %llu %llu\n", (end - start), start, end);

    for (int i = 0;i < 2;i ++) {
        // sync_mutex_unlock(token_provide);
        // seL4_Signal(token_provide);
        // printf("server %d unlock %d\n", id, token_provide);
        tokenms[id] = 1;


        while (! tokenms[id_want]) {
            // printf("server %d lock %d\n", id, token_want);
            seL4_Yield();
        }

        tokenms[id_want] = 0;
        // sync_mutex_lock(token_want);
        // seL4_Wait(token_want, NULL);

    }

    // printf("server %d terminates %d\n", id, terminate_num);
    terminate_num ++;

    if (terminate_num == 1) {
#ifdef BENCHMARK_ENTIRE
// end = rdtsc();
printf("COLLECTION - total %llu %llu %llu\n", (end - start), start, end);
#endif
        printf("end of test\n");
    }
    // printf("%d\n", terminate_num);

    exit(0);
}
#endif



/*
    if received initial data from all clients, multicast responses to all of them.
*/
void
process_message_multicast()
{
    int i;
    seL4_MessageInfo_t reply;

    for (i = 1;i < client_num;i ++) {
        seL4_SetMR(0, i);
        seL4_SetMR(1, 0);

        reply = seL4_MessageInfo_new(INIT, 0, 0, 1);
        seL4_SetMR(0, i);
        seL4_Send(reply_eps[i], reply);
    }

    return;
}



// sel4utils_thread_t *
// sel4_thread_creation()
// {
//     int error;
//
//     /* created with seL4_Untyped_Retype() */
//     sel4utils_thread_t *tcb = malloc(sizeof(sel4utils_thread_t));
//     if (tcb == NULL) {
//         printf("Error: Cannot malloc a tcb.\n");
//         return NULL;
//     }
//
//     /* get cspace root cnode */
//     seL4_CPtr cspace_cap;
//     cspace_cap = simple_get_cnode(&env.simple);

    /* get vspace root page directory */
    // seL4_CPtr pd_cap;
    // pd_cap = simple_get_pd(&env.simple);

    // /* create a new TCB */
    // vka_object_t tcb_object = {0};
    // error = vka_alloc_tcb(&vka, &tcb_object);
    // if (error) {
    //     printf("Error: Failed to allocate new TCB.\n");
    //     return NULL;
    // }
    //
    // /* create and map an ipc buffer */
    // vka_object_t ipc_frame_object;
    //
    // error = vka_alloc_frame(&vka, IPCBUF_FRAME_SIZE_BITS, &ipc_frame_object);
    // if (error) {
    //     printf("Error: Failed to alloc a frame for the IPC buffer.\n");
    //     return NULL;
    // }

    // err = sel4utils_configure_thread(&_vka, &_vspace, &_vspace, seL4_CapNull, 253,
    		                    //  simple_get_cnode(&_simple), seL4_NilData, &thread);
    /* configure with seL4_TCB_Configure() */
//
//     return tcb;
// }
void *test_t1(void *arg)
{
    for (int i=0; i<10; i++) {
        int *p = malloc(20);
        free(p);
    }

    printf("ttttr\n");

    thread_exit();

    return arg;
}


void *test_t0(void *arg)
{
    for (int i=0; i<10; i++) {
        int *p = malloc(20);
        free(p);
    }
    printf("ttttr\n");
    // int t_id = thread_create(&env.vspace, test_t1, NULL);
    // thread_resume(t_id);

    thread_exit();

    return arg;
}


/*
    return if_defer;
*/
#ifdef GREEN_THREAD

#ifdef CONSUMER_PRODUCER
int
process_message(seL4_MessageInfo_t info, seL4_MessageInfo_t **reply, seL4_Word *reply_ep, void *sync_prim, uint64_t *ipc_start)
{
    int label = seL4_MessageInfo_get_label(info);
    seL4_MessageInfo_t temp;
    int client_id, error;
    int UNUSED if_defer = 1;

#ifdef CLIENT_SINGLE
    int order_i, order_j;
#endif

    switch(label) {
        case INIT:

        client_id = initial_client();

        error = allocman_cspace_alloc(allocman, &pool->t_running->t->slot);
        assert(error == 0);

        /* check if OK to multi-cast */
        error = vka_cnode_saveCaller(&pool->t_running->t->slot);
        if (error != seL4_NoError) {
            printf("device_timer_save_caller_as_waiter failed to save caller.");
        }

        reply_eps[client_id] = (seL4_Word) pool->t_running->t->slot.offset;

        if (client_barrier()) {
            seL4_MessageInfo_t reply;

            seL4_SetMR(0, 0);
            seL4_SetMR(1, 1);

            reply = seL4_MessageInfo_new(INIT, 0, 0, 2);
            seL4_Send(reply_eps[0], reply);

            process_message_multicast();
        }


        return -1;
        break;

        case PRODUCER:
#ifdef BENCHMARK_ENTIRE
if (! started) {
    printf("first producer!\n");
    started = 1;
    start = rdtsc();
}
#endif
            /* save reply ep */
            printf("RECV a producer: %d %d thread %p\n", seL4_GetMR(0), seL4_GetMR(1), pool->t_running);

#ifdef CLIENT_MULTIPLE
#ifdef BENCHMARK_BREAKDOWN_BEFORE
uint64_t sc_start, sc_end;
sc_start = rdtsc();
#endif
            error = vka_cnode_saveCaller(&pool->t_running->t->slot);
            if (error != seL4_NoError) {
                printf("device_timer_save_caller_as_waiter failed to save caller.");
            }
#ifdef BENCHMARK_BREAKDOWN_BEFORE
sc_end = rdtsc();
before[before_cur ++] = sc_end - sc_start;
#endif
#endif

#ifdef CLIENT_SINGLE
            order_i = seL4_GetMR(0);
            order_j = seL4_GetMR(1);
#endif

            // thread_lock_acquire(lock_global);

            while (buffer == buffer_limit) {
                // thread_lock_release(lock_global);

                thread_sleep(producer_list, NULL);
                wait_count ++;
                // printf("wait count %d\n", wait_count);
                // thread_lock_acquire(lock_global);
            }

            assert(buffer < buffer_limit);

            buffer ++;
            printf("get one from producer %p!\n", pool->t_running);
            printf("buffer now: %d\n", buffer);

            thread_wakeup(consumer_list, NULL);

            // thread_lock_release(lock_global);

#ifdef CLIENT_SINGLE
            order_is[order_i_cur ++] = order_i;
            order_js[order_j_cur ++] = order_j;
#endif

#ifdef CLIENT_MULTIPLE
#ifdef BENCHMARK_BREAKDOWN_IPC
*ipc_start = rdtsc();
#endif

            temp = seL4_MessageInfo_new(PRODUCER, 0, 0, 1);
            *reply = &temp;

            return if_defer;
#endif

#ifdef CLIENT_SINGLE
            return -1;
#endif
        break;

        case CONSUMER:
#ifdef BENCHMARK_ENTIRE
if (! started) {
    printf("first consumer!\n");
    started = 1;
    start = rdtsc();
}
#endif

            printf("RECV a consumer: %d %d thread %p\n", seL4_GetMR(0), seL4_GetMR(1), pool->t_running);

#ifdef CLIENT_MULTIPLE
            /* save reply ep */
#ifdef BENCHMARK_BREAKDOWN_BEFORE
uint64_t sc_start1, sc_end1;

sc_start1 = rdtsc();
#endif
            error = vka_cnode_saveCaller(&pool->t_running->t->slot);
            if (error != seL4_NoError) {
                printf("device_timer_save_caller_as_waiter failed to save caller.");
            }
#ifdef BENCHMARK_BREAKDOWN_BEFORE
sc_end1 = rdtsc();
before[before_cur ++] = sc_end1 - sc_start1;
#endif
#endif

#ifdef CLIENT_SINGLE
            order_i = seL4_GetMR(0);
            order_j = seL4_GetMR(1);
#endif

            // thread_lock_acquire(lock_global);

            while (buffer == 0) {

                // thread_lock_release(lock_global);
                thread_sleep(consumer_list, NULL);
                wait_count ++;
                // printf("wait count consumer: %p\n", pool->t_running);
                // thread_lock_acquire(lock_global);
            }

            assert(buffer > 0);
            buffer --;

            printf("take one by consumer %p!\n", pool->t_running);
            printf("buffer now: %d\n", buffer);

            thread_wakeup(producer_list, NULL);

            // thread_lock_release(lock_global);

#ifdef CLIENT_SINGLE
            order_is[order_i_cur ++] = order_i;
            order_js[order_j_cur ++] = order_j;
#endif

#ifdef CLIENT_MULTIPLE
#ifdef BENCHMARK_BREAKDOWN_IPC
*ipc_start = rdtsc();
#endif

            temp = seL4_MessageInfo_new(CONSUMER, 0, 0, 1);
            *reply = &temp;

            return if_defer;
#endif

#ifdef CLIENT_SINGLE
#ifdef BENCHMARK_BREAKDOWN_IPC
*ipc_start = rdtsc();
/* Breakpoint E */
ipc_Es[ipc_test_cur] = *ipc_start;
#endif
            temp = seL4_MessageInfo_new(CONSUMER, 0, 0, 1);
            *reply = &temp;
            return 1;
#endif
        break;

        case TMNT:

        terminate_num ++;

        printf("TMNT\n");
#ifdef BENCHMARK_ENTIRE
#ifdef CLIENT_MULTIPLE
if (terminate_num == client_num) {
#endif

#ifdef CLIENT_SINGLE
if (terminate_num == 1) {
#endif
    end = rdtsc();
    printf("COLLECTION - total time: %llu start: %llu end: %llu %d\n", (end - start), start, end, wait_count);
#ifdef BENCHMARK_BREAKDOWN_BEFORE
    for (int i = 0;i < before_cur;i ++)
        printf("COLLECTION - before %llu\n", before[i]);
#endif

#ifdef BENCHMARK_BREAKDOWN_IPC
    // for (int i = 0;i < ipc_cur;i ++)
    //     printf("COLLECTION - ipc %llu\n", ipc[i]);
#ifdef CLIENT_SINGLE
    for (int i = 0;i < ipc_test_cur;i ++) {
        printf("COLLECTION - Client to consumer %llu start %llu end %llu\n", (ipc_Bs[i] - ipc_As[i]), ipc_As[i], ipc_Bs[i]);
        printf("COLLECTION - Client to producer %llu start %llu end %llu\n", (ipc_Ds[i] - ipc_Cs[i]), ipc_Cs[i], ipc_Ds[i]);
        printf("COLLECTION - Consumer to client %llu start %llu end %llu\n", (ipc_Fs[i] - ipc_Es[i]), ipc_Es[i], ipc_Fs[i]);
    }
#endif
#endif
    printf("end of test\n");
    thread_exit();
}
#endif

            temp = seL4_MessageInfo_new(TMNT, 0, 0, 1);
            *reply = &temp;

            thread_exit();
            return 1;
        break;

        case IMMD:
        printf("IMMD instruction\n");
        break;

    }

    return -1;
}
#endif

#ifdef WAIT_SEND_WAIT // - green thread
int
process_message(seL4_MessageInfo_t info, seL4_MessageInfo_t **reply, seL4_Word *reply_ep, void *sync_prim, uint64_t *ipc_start)
{
    int label = seL4_MessageInfo_get_label(info);
    seL4_MessageInfo_t temp;
    int client_id, error;

    switch(label) {
        case INIT:
            client_id = initial_client();

            cspacepath_t slot_temp;

            error = allocman_cspace_alloc(allocman, &slot_temp);
            assert(error == 0);

            /* check if OK to multi-cast */
            error = vka_cnode_saveCaller(&slot_temp);
            if (error != seL4_NoError) {
                printf("device_timer_save_caller_as_waiter failed to save caller.");
            }

            reply_eps[client_id] = (seL4_Word) slot_temp.offset;

            if (client_barrier()) {
                seL4_MessageInfo_t reply;

                reply = seL4_MessageInfo_new(INIT, 0, 0, 1);
                seL4_SetMR(0, 0);
                seL4_Send(reply_eps[0], reply);

                process_message_multicast();
            }


            return -1;
            break;



            case WAIT:
            printf("Receive a wait request from %d %d %p\n", seL4_GetMR(0), seL4_GetMR(1), seL4_GetIPCBuffer());

            /* save reply_endpoint */
            error = vka_cnode_saveCaller(&pool->t_running->t->slot);
            if (error != seL4_NoError) {
                printf("device_timer_save_caller_as_waiter failed to save caller.");
            }

            /* acquire token */
            thread_lock_t *token_want = (thread_lock_t *) seL4_GetMR(2);
            // printf("sync prim %p\n", token_want);
            thread_lock_acquire(token_want);

            printf("test01\n");
            printf("Complete a wait request\n");


            /* Reply msg */
            temp = seL4_MessageInfo_new(WAIT, 0, 0, 0);
            *reply = &temp;

            return 1;

            break;



            case SEND_WAIT:
#ifdef BENCHMARK_ENTIRE
if (! started) {
    started = 1;
    start = rdtsc();
}
#endif
            printf("Receive a send_wait request from %d %d %p\n", seL4_GetMR(0), seL4_GetMR(1), seL4_GetIPCBuffer());

            /* save reply_endpoint */
            error = vka_cnode_saveCaller(&pool->t_running->t->slot);
            if (error != seL4_NoError) {
                printf("device_timer_save_caller_as_waiter failed to save caller.");
            }

            /* release token */
            thread_lock_release((thread_lock_t *) seL4_GetMR(3));

            /* acquire token */
            thread_lock_acquire((thread_lock_t *) seL4_GetMR(2));

            printf("Complete a send_wait request\n");

            /* Reply msg */
            temp = seL4_MessageInfo_new(SEND_WAIT, 0, 0, 0);
            *reply = &temp;

            return 1;

            break;


        case TMNT:

        client_id = seL4_GetMR(0);
        printf("Client %d terminates\n", client_id);

        terminate_num ++;

        if (terminate_num == 1) {
#ifdef BENCHMARK_ENTIRE
end = rdtsc();
printf("COLLECTION - total %llu %llu %llu\n", (end - start), start, end);
#endif
            printf("end of test\n");
            // printf("end of test\n");
        }
        return -1;

        break;

        case IMMD:
        printf("IMMD instruction\n");
        break;

    }

    return 0;
}

#endif



void *
server_loop(void *sync_prim)
{
    // thread_pool_info();

    seL4_MessageInfo_t info = seL4_Recv(env.endpoint.cptr, NULL);

    seL4_MessageInfo_t *reply = NULL;
    seL4_Word reply_ep;
    int res;

    uint64_t UNUSED ipc_start, UNUSED ipc_end;

    while (1) {
        ipc_start = 0;
        ipc_end = 0;
        /* if_reply */
        res = process_message(info, &reply, &reply_ep, sync_prim, &ipc_start);
        if (res == 1) {
// #ifdef CLIENT_MULTIPLE
            seL4_Send(pool->t_running->t->slot.offset, *reply);
            info = seL4_Recv(env.endpoint.cptr, NULL);
// #ifdef BENCHMARK_BREAKDOWN_IPC
// ipc_end = rdtsc();
// ipc[ipc_cur ++] = ipc_end - ipc_start;
// #endif
// #endif

// #ifdef CLIENT_SINGLE
            // seL4_Send(client_ep, *reply);
            // info = seL4_Recv(env.endpoint.cptr, NULL);
// #ifdef BENCHMARK_BREAKDOWN_IPC
// /* Breakpoint B */
// ipc_end = rdtsc();
// ipc_Bs[ipc_test_cur] = ipc_end;
// #endif
// #endif

        } else if (res == 0) {

            assert(reply != NULL);
            info = seL4_ReplyRecv(env.endpoint.cptr, *reply, NULL);
        } else {
            info = seL4_Recv(env.endpoint.cptr, NULL);
// #ifdef CLIENT_SINGLE
// #ifdef BENCHMARK_BREAKDOWN_IPC
// /* Breakpoint D */
// ipc_end = rdtsc();
// ipc_Ds[ipc_test_cur] = ipc_end;
// #endif
// #endif
        }
    }

    return sync_prim;
}
#endif



#ifdef SEL4_THREAD

#ifdef CONSUMER_PRODUCER

seL4_Word
process_message(seL4_MessageInfo_t info, cspacepath_t *slot, seL4_MessageInfo_t **reply, void *sync_prim, void *lock, uint64_t *ipc_start)
{
    int label = seL4_MessageInfo_get_label(info);
    seL4_MessageInfo_t temp;
    int client_id, error;

#ifdef CLIENT_SINGLE
    int order_i, order_j;
#endif

    switch(label) {
        case INIT:
            client_id = initial_client();

            cspacepath_t slot_temp;

            error = allocman_cspace_alloc(allocman, &slot_temp);
            assert(error == 0);

            /* check if OK to multi-cast */
            error = vka_cnode_saveCaller(&slot_temp);
            if (error != seL4_NoError) {
                printf("device_timer_save_caller_as_waiter failed to save caller.");
            }

            reply_eps[client_id] = (seL4_Word) slot_temp.offset;

            if (client_barrier()) {
                seL4_MessageInfo_t reply;

                seL4_SetMR(0, 0);
                seL4_SetMR(1, 1);

                reply = seL4_MessageInfo_new(INIT, 0, 0, 2);
                seL4_Send(reply_eps[0], reply);
                process_message_multicast();
            }


            return 0;
            break;

            case PRODUCER:
#ifdef BENCHMARK_ENTIRE
if (! started) {
    printf("First producer\n");
    started = 1;
    start = rdtsc();
}
#endif
            printf("RECV a producer: %d %d %p\n", seL4_GetMR(0), seL4_GetMR(1), seL4_GetIPCBuffer());

#ifdef CLIENT_MULTIPLE
#ifdef SEL4_GREEN
#ifdef BENCHMARK_BREAKDOWN_BEFORE
uint64_t sc_start, sc_end;
sc_start = rdtsc();
#endif

            error = vka_cnode_saveCaller(slot);
            if (error != seL4_NoError) {
                printf("device_timer_save_caller_as_waiter failed to save caller.");
            }

#ifdef BENCHMARK_BREAKDOWN_BEFORE
sc_end = rdtsc();
before[before_cur ++] = sc_end - sc_start;
#endif
#endif
#endif

#ifdef CLIENT_SINGLE
            order_i = seL4_GetMR(0);
            order_j = seL4_GetMR(1);
#endif

            sync_mutex_lock(&lock_global);

            while (buffer == buffer_limit) {
                sync_mutex_unlock(&lock_global);
                seL4_Wait(producer_list, NULL);
                wait_count ++;
                // printf("wait count: %d\n", wait_count);
                sync_mutex_lock(&lock_global);
            }

            assert(buffer < buffer_limit);

            buffer ++;
            printf("get one from producer %p!\n", seL4_GetIPCBuffer());
            printf("buffer now: %d\n", buffer);

            // printf("before signal: %p\n", seL4_GetIPCBuffer());
            seL4_Signal(consumer_list);
            // printf("after signal: %p\n", seL4_GetIPCBuffer());
            // printf("seL4_Signal\n");
            sync_mutex_unlock(&lock_global);

#ifdef CLIENT_SINGLE
            order_is[order_i_cur ++] = order_i;
            order_js[order_j_cur ++] = order_j;
#endif

#ifdef CLIENT_MULTIPLE
#ifdef BENCHMARK_BREAKDOWN_IPC
*ipc_start = rdtsc();
ipc_half_starts[ipc_half_cur] = *ipc_start;
#endif
            temp = seL4_MessageInfo_new(PRODUCER, 0, 0, 1);
            *reply = &temp;

            return 1;
#endif

#ifdef CLIENT_SINGLE
                return -1;
#endif

        break;

        case CONSUMER:
#ifdef BENCHMARK_ENTIRE
if (! started) {
    printf("first consumer!\n");
    started = 1;
    start = rdtsc();

}
#endif

    printf("RECV a consumer: %d %d %p\n", seL4_GetMR(0), seL4_GetMR(1), seL4_GetIPCBuffer());

#ifdef CLIENT_MULTIPLE
#ifdef SEL4_GREEN
#ifdef BENCHMARK_BREAKDOWN_BEFORE
uint64_t sc_start1, sc_end1;
sc_start1 = rdtsc();
#endif

            error = vka_cnode_saveCaller(slot);
            if (error != seL4_NoError) {
                printf("device_timer_save_caller_as_waiter failed to save caller.");
            }

#ifdef BENCHMARK_BREAKDOWN_BEFORE
sc_end1 = rdtsc();
before[before_cur ++] = sc_end1 - sc_start1;
#endif
#endif
#endif

#ifdef CLIENT_SINGLE
            order_i = seL4_GetMR(0);
            order_j = seL4_GetMR(1);
#endif

            sync_mutex_lock(&lock_global);
            while (buffer == 0) {
                sync_mutex_unlock(&lock_global);
                seL4_Wait(consumer_list, NULL);
                wait_count ++;
                sync_mutex_lock(&lock_global);
            }

            assert(buffer > 0);
            buffer --;

            printf("take one by consumer %p!\n", seL4_GetIPCBuffer());
            printf("buffer snow: %d\n", buffer);

            // printf("before signal: %p\n", seL4_GetIPCBuffer());
            seL4_Signal(producer_list);
            // printf("after signal: %p\n", seL4_GetIPCBuffer());

            sync_mutex_unlock(&lock_global);

#ifdef CLIENT_SINGLE
            order_is[order_i_cur ++] = order_i;
            order_js[order_j_cur ++] = order_j;
#endif


#ifdef CLIENT_MULTIPLE
#ifdef BENCHMARK_BREAKDOWN_IPC
*ipc_start = rdtsc();
ipc_starts[ipc_cur] = *ipc_start;
#endif
            temp = seL4_MessageInfo_new(CONSUMER, 0, 0, 1);
            *reply = &temp;

            return 1;
#endif

#ifdef CLIENT_SINGLE
#ifdef BENCHMARK_BREAKDOWN_IPC
/* Breakpoint E */
*ipc_start = rdtsc();
ipc_Es[ipc_test_cur] = *ipc_start;
#endif
                temp = seL4_MessageInfo_new(CONSUMER, 0, 0, 1);
                *reply = &temp;
                return 1;
#endif

        break;


        case TMNT:

        client_id = seL4_GetMR(0);
        // printf("Client %d terminates\n", client_id);

        terminate_num ++;

#ifdef BENCHMARK_ENTIRE
#ifdef CLIENT_MULTIPLE
        if (terminate_num == client_num) {
#endif

#ifdef CLIENT_SINGLE
        if (terminate_num == 1) {
#endif
            end = rdtsc();
            printf("COLLECTION - total time %llu %llu %llu %d\n", (end - start), start, end, wait_count);
#ifdef BENCHMARK_BREAKDOWN_BEFORE
    for (int i = 0;i < before_cur;i ++)
        printf("COLLECTION - before %llu\n", before[i]);
#endif

#ifdef BENCHMARK_BREAKDOWN_IPC
    // for (int i = 0;i < ipc_cur;i ++)
        // printf("COLLECTION - ipc %llu\n", ipc[i]);

        for (int i = 1;i < ipc_test_cur;i ++) {
            printf("COLLECTION - Client to consumer %llu start %llu end %llu\n", (ipc_Bs[i] - ipc_As[i]), ipc_As[i], ipc_Bs[i]);
            printf("COLLECTION - Client to producer %llu start %llu end %llu\n", (ipc_Ds[i] - ipc_Cs[i]), ipc_Cs[i], ipc_Ds[i]);
            printf("COLLECTION - Consumer to client %llu start %llu end %llu\n", (ipc_Fs[i] - ipc_Es[i]), ipc_Es[i], ipc_Fs[i]);
        }
#ifdef CLIENT_SINGLE
    // for (int i = 0;i < ipc_half_cur;i ++)
        // printf("COLLECTION - ipc half %llu\n", ipc_half[i]);

#endif
#endif
            printf("end of test\n");
            // printf("end of test\n");
        }
#endif

        break;

        case IMMD:
        printf("IMMD instruction\n");
        break;

    }

    return 0;
}

#endif

#ifdef WAIT_SEND_WAIT
seL4_Word
process_message(seL4_MessageInfo_t info, cspacepath_t *slot, seL4_MessageInfo_t **reply, void *sync_prim, void *lock, uint64_t *ipc_start)
{
    int label = seL4_MessageInfo_get_label(info);
    seL4_MessageInfo_t temp;
    int client_id, error;
    seL4_CPtr token_want, token_provide;

    switch(label) {
        case INIT:
            client_id = initial_client();

            cspacepath_t slot_temp;

            error = allocman_cspace_alloc(allocman, &slot_temp);
            assert(error == 0);

            /* check if OK to multi-cast */
            error = vka_cnode_saveCaller(&slot_temp);
            if (error != seL4_NoError) {
                printf("device_timer_save_caller_as_waiter failed to save caller.");
            }

            reply_eps[client_id] = (seL4_Word) slot_temp.offset;

            if (client_barrier()) {
                seL4_MessageInfo_t reply;

                reply = seL4_MessageInfo_new(INIT, 0, 0, 1);
                seL4_SetMR(0, 0);
                seL4_Send(reply_eps[0], reply);

                process_message_multicast();
            }


            return 0;
            break;



            case WAIT:
            // printf("Receive a wait request from %d %d %d %p\n", seL4_GetMR(0), seL4_GetMR(1), seL4_GetMR(2), seL4_GetIPCBuffer());


            /* save reply_endpoint */

            /* acquire token */
            token_want = (seL4_CPtr) seL4_GetMR(2);

            // sync_mutex_lock(token_want);
            seL4_Wait(token_want, NULL);


            /* Reply msg */
            temp = seL4_MessageInfo_new(WAIT, 0, 0, 0);
            *reply = &temp;

            return 1;

            break;



            case SEND_WAIT:
            // printf("Receive a send_wait request from %d %d %d %p\n", seL4_GetMR(0), seL4_GetMR(1), seL4_GetMR(3), seL4_GetIPCBuffer());

if (! started) {
    printf("start\n");
    started = 1;
    start = rdtsc();
}

            /* save reply_endpoint */

            /* release token */
            token_provide = (seL4_CPtr) seL4_GetMR(3);
            token_want = (seL4_CPtr) seL4_GetMR(2);

            // sync_mutex_unlock((sync_mutex_t *) seL4_GetMR(3));
            seL4_Signal(token_provide);

            /* acquire token */
            // sync_mutex_lock((sync_mutex_t *) seL4_GetMR(2));
            seL4_Wait(token_want, NULL);

            // printf("Complete a send_wait request %d %d %p\n", seL4_GetMR(0), seL4_GetMR(1), seL4_GetIPCBuffer());

            /* Reply msg */
#ifdef BENCHMARK_BREAKDOWN_IPC
*ipc_start = rdtsc();
#endif
            temp = seL4_MessageInfo_new(SEND_WAIT, 0, 0, 0);
            *reply = &temp;

            return 1;

            break;


        case TMNT:

        client_id = seL4_GetMR(0);
        // printf("Client %d terminates\n", client_id);

        terminate_num ++;

        if (terminate_num == 1) {
#ifdef BENCHMARK_ENTIRE
            end = rdtsc();
            printf("COLLECTION - total %llu %llu %llu\n", (end - start), start, end);
#endif

            printf("end of test\n");
            // printf("end of test\n");
        }

        break;

        case IMMD:
        printf("IMMD instruction\n");
        break;

    }

    return 0;
}
#endif




void
server_loop(void *sync_prim, void *lock)
{
    assert(sync_prim != NULL);

    seL4_MessageInfo_t *reply = NULL;
    int res;
    int error;
    cspacepath_t slot;

    UNUSED uint64_t ipc_start, ipc_end;

    seL4_MessageInfo_t info = seL4_Recv(env.endpoint.cptr, NULL);
    // /* Breakpoint D_0 */
    // ipc_start = rdtsc();
    // ipc_Bs[0] = ipc_start;

    error = allocman_cspace_alloc(allocman, &slot);
    assert(error == 0);

    while (1) {
        ipc_start =0;
        ipc_end = 0;
        res = process_message(info, &slot, &reply, sync_prim, lock, &ipc_start);

        if (res == 1) {
// #ifdef CLIENT_MULTIPLE
#ifdef SEL4_GREEN
            seL4_Send(slot.offset, *reply);
            info = seL4_Recv(env.endpoint.cptr, NULL);
// #ifdef BENCHMARK_BREAKDOWN_IPC
// ipc_end = rdtsc();
// ipc_ends[ipc_cur] = ipc_end;
// ipc[ipc_cur ++] = ipc_end - ipc_start;
// #endif
#endif

#ifdef SEL4_FAST
            info = seL4_ReplyRecv(env.endpoint.cptr, *reply, NULL);
#ifdef BENCHMARK_BREAKDOWN_IPC
ipc_end = rdtsc();
// ipc_ends[ipc_cur] = ipc_end;
// ipc[ipc_cur ++] = ipc_end - ipc_start;
printf("COLLECTION - IPC %llu %llu %llu\n", (ipc_end - ipc_start), ipc_start, ipc_end);
#endif
#endif
// #endif
//
// #ifdef CLIENT_SINGLE
//             seL4_Send(client_ep, *reply);
//             info = seL4_Recv(env.endpoint.cptr, NULL);
// #ifdef BENCHMARK_BREAKDOWN_IPC
// /* Breakpoint D */
// ipc_end = rdtsc();
// ipc_Ds[ipc_test_cur] = ipc_end;
// #endif
// #endif

        } else {
            info = seL4_Recv(env.endpoint.cptr, NULL);
// #ifdef CLIENT_SINGLE
// #ifdef BENCHMARK_BREAKDOWN_IPC
// /* Breakpoint B */
// ipc_end = rdtsc();
// ipc_Bs[ipc_test_cur] = ipc_end;
// #endif
// #endif
        }

    }

    return;
}

int
sel4_threads_initial(int num)
{
    void *lock = NULL;
    sel4utils_thread_t *tcb;
    seL4_CPtr cspace_cap;
    int i, error;

    /* initial sel4 thread environment */
    sel4_thread_initial();

    printf("sel4 env initialized successfully\n");

#ifdef THREAD_LOCK
    /* initial mutex */
    sync_mutex_t sync_prim;
    seL4_CPtr notification;
    notification = vka_alloc_notification_leaky(&env.vka);
    sync_mutex_init(&sync_prim, notification);

    sync_mutex_lock(&sync_prim);
#endif

#ifdef THREAD_SEMAPHORE
    sync_sem_t sync_prim;
    sync_sem_new(&env.vka, &sync_prim, 0);
#endif

#ifdef THREAD_CV
    sync_cv_t sync_prim;
    sync_bin_sem_t bin_sem;
    sync_bin_sem_new(&env.vka, &bin_sem, 1);
    sync_cv_new(&env.vka, &sync_prim);
    lock = &bin_sem;
#endif

#ifdef CONSUMER_PRODUCER
    notification = vka_alloc_notification_leaky(&env.vka);
    producer_list = vka_alloc_notification_leaky(&env.vka);
    consumer_list = vka_alloc_notification_leaky(&env.vka);
    sync_mutex_init(&lock_global, notification);
#endif

    cspace_cap = simple_get_cnode(&env.simple);

    for (i = 0;i < num;i ++) {

        tcb = malloc(sizeof(sel4utils_thread_t));
        if (tcb == NULL) {
            printf("Cannot allocate a new sel4 thread.\n");
            return 0;
        }

        error = sel4utils_configure_thread(&env.vka, &env.vspace, &env.vspace, seL4_CapNull,
                                       seL4_MaxPrio, cspace_cap, seL4_NilData, tcb);
        assert(! error);

        error = sel4utils_start_thread(tcb, (sel4utils_thread_entry_fn) server_loop, (void *) &sync_prim, lock, 1);
        assert(! error);

        sel4_thread_new(i, tcb);
    }

    return 1;
}
#endif



#ifdef EVENT_CONTINUATION
void
save_continuation(int order_i, int order_j, int type)
{

    int *last_conti;
    int *start_conti;
    continuation_t *continuations;

    if (type == PRODUCER) {
        last_conti = &producer_last_conti;
        start_conti = &producer_start_conti;
        continuations = producer_continuations;
    } else {
        last_conti = &consumer_last_conti;
        start_conti = &consumer_start_conti;
        continuations = consumer_continuations;
    }

    int new_conti = *last_conti;
    continuations += new_conti;
    *last_conti = (*last_conti == MAX_CONTI - 1) ? 0 : *last_conti + 1;

    assert(continuations->occupied == 0);

    continuations->order_i = order_i;
    continuations->order_j = order_j;
    continuations->occupied = 1;

    if (*start_conti == -1)
        *start_conti = new_conti;

    return;
}



void
restore_continuation(int cur_conti, int *order_i, int *order_j, int type)
{
    assert(cur_conti < MAX_CONTI && cur_conti >= 0);

    continuation_t *continuations;

    if (type == PRODUCER) {
        continuations = producer_continuations;
    } else {
        continuations = consumer_continuations;
    }

    continuations += cur_conti;
    assert(continuations->occupied);

    *order_i = continuations->order_i;
    *order_j = continuations->order_j;
    continuations->occupied = 0;

    return;
}


int
retrieve_start_conti(int type)
{
    int *start_conti;
    continuation_t *continuations;

    if (type == PRODUCER) {
        start_conti = &producer_start_conti;
        continuations = producer_continuations;
    } else {
        start_conti = &consumer_start_conti;
        continuations = consumer_continuations;
    }

    int res = *start_conti;
    if (res == -1)
        return res;

    int next_start_conti = (*start_conti == MAX_CONTI - 1) ? 0 : *start_conti + 1;
    continuations += next_start_conti;

    *start_conti = (continuations->occupied) ? next_start_conti : -1;

    return res;
}



int
process_message (seL4_MessageInfo_t info, seL4_MessageInfo_t **reply, uint64_t *ipc_start) {
    int label = seL4_MessageInfo_get_label(info);
    seL4_MessageInfo_t temp;

    switch(label) {

            case PRODUCER:
#ifdef BENCHMARK_ENTIRE
if (! started) {
    printf("First producer\n");
    started = 1;
    start = rdtsc();
}
#endif

            // printf("RECV a producer: %d %d %p\n", seL4_GetMR(0), seL4_GetMR(1), seL4_GetIPCBuffer());


            if (buffer == buffer_limit) {
                /* save continuation */

                save_continuation(seL4_GetMR(0), seL4_GetMR(1), PRODUCER);

                wait_count ++;

                return -1;
            } else {

                int order_i = seL4_GetMR(0);
                int order_j = seL4_GetMR(1);

                assert(buffer < buffer_limit);
                buffer ++;

                int start_conti = retrieve_start_conti(CONSUMER);
                if (start_conti > -1) {
#ifdef BENCHMARK_BREAKDOWN_IPC
/* Breakpoint G */
*ipc_start = rdtsc();
ipc_Gs[ipc_test_cur] = *ipc_start;
#endif

                    seL4_MessageInfo_t info_conti = seL4_MessageInfo_new(CONSUMER_CONTI, 0, 0, 1);
                    seL4_SetMR(0, start_conti);
                    seL4_Send(helper_ep, info_conti);
                }

                order_is[order_i_cur ++] = order_i;
                order_js[order_j_cur ++] = order_j;
// #ifdef BENCHMARK_BREAKDOWN_IPC
// *ipc_start = rdtsc();
// #endif
                // temp = seL4_MessageInfo_new(PRODUCER, 0, 0, 1);
                // *reply = &temp;

                return -1;
            }


        break;

        case PRODUCER_CONTI:
            if (buffer == buffer_limit) {

                wait_count ++;

                return -1;
            } else {
                /* restore and remove continuation */
                /* Actually nothing for producer_conti */
                int order_i;
                int order_j;

                restore_continuation(seL4_GetMR(0), &order_i, &order_j, PRODUCER);

                assert(buffer < buffer_limit);
                buffer ++;

                int start_conti = retrieve_start_conti(CONSUMER);
                if (start_conti > -1) {
                    seL4_MessageInfo_t info_conti = seL4_MessageInfo_new(CONSUMER_CONTI, 0, 0, 1);
                    seL4_SetMR(0, start_conti);
                    seL4_Send(helper_ep, info_conti);
                }

                order_is[order_i_cur ++] = order_i;
                order_js[order_j_cur ++] = order_j;

                return -1;
            }
        break;

        case CONSUMER:
#ifdef BENCHMARK_ENTIRE
if (! started) {
    printf("first consumer!\n");
    started = 1;
    start = rdtsc();
}
#endif

    // printf("RECV a consumer: %d %d %p\n", seL4_GetMR(0), seL4_GetMR(1), seL4_GetIPCBuffer());


            if (buffer == 0) {
                /* save continuation */
// rdtsc_start();
                save_continuation(seL4_GetMR(0), seL4_GetMR(1), CONSUMER);
// rdtsc_end();
// printf("Time consumption for saving continuation %llu\n", (end - start));

                wait_count ++;

                return -1;
            } else {
                /* restore and save continuation */
                int order_i = seL4_GetMR(0);
                int order_j = seL4_GetMR(1);

                assert(buffer > 0);
                buffer --;

                int start_conti = retrieve_start_conti(PRODUCER);

                if (start_conti > -1) {
                    seL4_MessageInfo_t info_conti = seL4_MessageInfo_new(PRODUCER_CONTI, 0, 0, 1);
                    seL4_SetMR(0, start_conti);
                    seL4_Send(helper_ep, info_conti);
                }

                order_is[order_i_cur ++] = order_i;
                order_js[order_j_cur ++] = order_j;

                return -1;
            }

        break;

        case CONSUMER_CONTI:
        if (buffer == 0) {
            /* save continuation */
            save_continuation(seL4_GetMR(0), seL4_GetMR(1), CONSUMER);

            wait_count ++;
            printf("IMPOSSIBLE!\n");
            return -1;
        } else {
            /* restore and save continuation */
            int order_i;
            int order_j;

// uint64_t UNUSED r_start, r_end;
// rdtsc_start();
            restore_continuation(seL4_GetMR(0), &order_i, &order_j, CONSUMER);
// rdtsc_end();
// printf("Time consumption for restoring continuation %llu\n", (end -  start));

            assert(buffer > 0);
            buffer --;

            int start_conti = retrieve_start_conti(PRODUCER);
            if (start_conti > -1) {
                seL4_MessageInfo_t info_conti = seL4_MessageInfo_new(PRODUCER_CONTI, 0, 0, 1);
                seL4_SetMR(0, start_conti);
                seL4_Send(helper_ep, info_conti);
            }

            order_is[order_i_cur ++] = order_i;
            order_js[order_j_cur ++] = order_j;

#ifdef BENCHMARK_BREAKDOWN_IPC
*ipc_start = rdtsc();
ipc_Es[ipc_test_cur] = *ipc_start;
#endif

            temp = seL4_MessageInfo_new(PRODUCER, 0, 0, 1);
            *reply = &temp;

            return 1;
        }
        break;

        case TMNT:

        // printf("Client %d terminates\n", client_id);

        terminate_num ++;
#ifdef BENCHMARK_ENTIRE
        if (terminate_num == 1) {
            end = rdtsc();
            printf("COLLECTION - total time %llu %llu %llu %d\n", (end - start), start, end, wait_count);
#ifdef BENCHMARK_BREAKDOWN_BEFORE
    for (int i = 0;i < before_cur;i ++)
        printf("COLLECTION - before %llu\n", before[i]);
#endif

#ifdef BENCHMARK_BREAKDOWN_IPC
        for (int i = 0;i < ipc_test_cur;i ++) {
            printf("COLLECTION - Client to consumer %llu start %llu end %llu\n", (ipc_Bs[i] - ipc_As[i]), ipc_As[i], ipc_Bs[i]);
            printf("COLLECTION - Client to producer %llu start %llu end %llu\n", (ipc_Ds[i] - ipc_Cs[i]), ipc_Cs[i], ipc_Ds[i]);
            printf("COLLECTION - Producer to helper %llu start %llu end %llu\n", (ipc_Hs[i] - ipc_Gs[i]), ipc_Gs[i], ipc_Hs[i]);
            printf("COLLECTION - Helper to producer %llu start %llu end %llu\n", (ipc_Js[i] - ipc_Is[i]), ipc_Is[i], ipc_Js[i]);
            printf("COLLECTION - Consumer to client %llu start %llu end %llu\n", (ipc_Fs[i] - ipc_Es[i]), ipc_Es[i], ipc_Fs[i]);
        }
#endif
            printf("end of test\n");
        }
#endif

        break;

        case IMMD:
        printf("IMMD instruction\n");
        break;

    }

    return 0;
}



void server_loop( void ) {

    seL4_MessageInfo_t *reply = NULL;
    int res;

    seL4_MessageInfo_t info = seL4_Recv(env.endpoint.cptr, NULL);

    // UNUSED unsigned ipc_start_high = 0, ipc_start_low = 0, ipc_end_high, ipc_end_low;
    UNUSED uint64_t ipc_start, ipc_end;

    while (1) {
        ipc_start =0;
        ipc_end = 0;
        res = process_message(info, &reply, &ipc_start);

        if (res == 1) {
            seL4_Send(client_ep, *reply);
            info = seL4_Recv(env.endpoint.cptr, NULL);

#ifdef BENCHMARK_BREAKDOWN_IPC
/* Breakpoint B */
ipc_end = rdtsc();
ipc_Bs[ipc_test_cur] = ipc_end;
#endif
        } else {
            info = seL4_Recv(env.endpoint.cptr, NULL);
#ifdef BENCHMARK_BREAKDOWN_IPC
if (! ipc_Ds[ipc_test_cur]) {
/* Breakpoint D */
ipc_end = rdtsc();
ipc_Ds[ipc_test_cur] = ipc_end;
} else {
/* Breakpoint J */
ipc_end = rdtsc();
ipc_Js[ipc_test_cur] = ipc_end;
}
#endif
        }

    }

    return;
}



void
helper_program( void )
{
    seL4_MessageInfo_t UNUSED info_reply, info_tmnt, info_helper;

#ifdef BENCHMARK_BREAKDOWN_IPC
    uint64_t ipc_start, ipc_end;
#endif

    while (1) {
        info_reply = seL4_Recv(helper_ep, NULL);

#ifdef BENCHMARK_BREAKDOWN_IPC
/* Breakpoint H */
ipc_end = rdtsc();
ipc_Hs[ipc_test_cur] = ipc_end;
#endif

#ifdef BENCHMARK_BREAKDOWN_IPC
/* Breakpoint I */
ipc_start = rdtsc();
ipc_Is[ipc_test_cur] = ipc_start;
#endif

        int conti = seL4_GetMR(0);
        info_helper = seL4_MessageInfo_new(seL4_MessageInfo_get_label(info_reply),
                                    0, 0, 1);
        seL4_SetMR(0, conti);
        seL4_Send(env.endpoint.cptr, info_helper);
    }

    return;
}



void
helper_initial( void ) {
    int error;
    sel4utils_thread_t *tcb;
    seL4_CPtr cspace_cap;

    tcb = malloc(sizeof(sel4utils_thread_t));
    if (tcb == NULL) {
        printf("Cannot allocate a new sel4 thread.\n");
    }

    cspace_cap = simple_get_cnode(&env.simple);

    error = sel4utils_configure_thread(&env.vka, &env.vspace, &env.vspace, seL4_CapNull,
                                       seL4_MaxPrio, cspace_cap, seL4_NilData, tcb);
    assert(! error);

    error = sel4utils_start_thread(tcb, (sel4utils_thread_entry_fn) helper_program, NULL, NULL, 1);
    assert(! error);
}
#endif







#ifdef CLIENT_SINGLE
void
client_program( void )
{
    seL4_MessageInfo_t UNUSED info_reply, info_tmnt, info_consumer, info_producer;

    // seL4_MessageInfo_t info_init = seL4_MessageInfo_new(INIT, 0, 0, 1);
    // info_reply = seL4_Call(env.endpoint.cptr, info_init);
#ifdef BENCHMARK_BREAKDOWN_IPC
    uint64_t ipc_start, ipc_end;
#endif

    for (int i = 0;i < 10;i ++) {
        for (int j = 0;j < 1;j ++) {
#ifdef BENCHMARK_BREAKDOWN_IPC
            /* Breakpoint A */
            ipc_start = rdtsc();
            ipc_As[ipc_test_cur] = ipc_start;
#endif
            info_consumer = seL4_MessageInfo_new(CONSUMER, 0, 0, 2);
            seL4_SetMR(0, i);
            seL4_SetMR(0, j);

            seL4_Send(env.endpoint.cptr, info_consumer);
        }

        for (int j = 0;j < 1;j ++) {
#ifdef BENCHMARK_BREAKDOWN_IPC
            /* Breakpoint B */
            ipc_start = rdtsc();
            ipc_Cs[ipc_test_cur] = ipc_start;
#endif
            info_producer = seL4_MessageInfo_new(PRODUCER, 0, 0, 2);
            seL4_SetMR(0, i);
            seL4_SetMR(0, j);
            seL4_Send(env.endpoint.cptr, info_producer);
        }

        info_reply = seL4_Recv(client_ep, NULL);
#ifdef BENCHMARK_BREAKDOWN_IPC
        /* Breakpoint F */
        ipc_end = rdtsc();
        ipc_Fs[ipc_test_cur ++] = ipc_end;
#endif
    }

    info_tmnt = seL4_MessageInfo_new(TMNT, 0, 0, 0);
    info_reply = seL4_Call(env.endpoint.cptr, info_tmnt);

    return;
}
#endif



#ifdef CLIENT_MULTIPLE

#ifdef WAIT_SEND_WAIT
void
client_program( void )
{
    seL4_MessageInfo_t UNUSED info_reply, info_tmnt, info_request;
    seL4_MessageInfo_t info_init = seL4_MessageInfo_new(INIT, 0, 0, 1);
    info_reply = seL4_Call(env.endpoint.cptr, info_init);
    int id = seL4_GetMR(0);

    /* find the token it want */
    int id_want = (id == client_count - 1) ? 0 : id + 1;
#ifdef SEL4_THREAD
    // int token_want = (int) &tokens[id_want];
    int token_want = (int) tokenps[id_want];
#endif

#ifdef GREEN_THREAD
    int token_want = (int) tokens[id_want];
#endif

    /* find the token it provide */
#ifdef SEL4_THREAD
    // int token_provide = (int) &tokens[id];
    int token_provide = (int) tokenps[id];
#endif

#ifdef GREEN_THREAD
    int token_provide = (int) tokens[id];
#endif
    printf("sync prim (client) %d %d %p %p\n", id, id_want, (sync_mutex_t *) token_want, (sync_mutex_t *) token_provide);

    /* client id 0 send the WAIT request first */
    if (id != (client_count - 1)) {
        info_request = seL4_MessageInfo_new(WAIT, 0, 0, 4);
        seL4_SetMR(0, id);
        seL4_SetMR(1, 0);
        seL4_SetMR(2, token_want);
        seL4_SetMR(3, token_provide);
        info_reply = seL4_Call(env.endpoint.cptr, info_request);
    }

    for (int i = 0;i < 50;i ++) {
        info_request = seL4_MessageInfo_new(SEND_WAIT, 0, 0, 4);
        seL4_SetMR(0, id);
        seL4_SetMR(1, i);
        seL4_SetMR(2, token_want);
        seL4_SetMR(3, token_provide);
        info_reply = seL4_Call(env.endpoint.cptr, info_request);
    }

    info_tmnt = seL4_MessageInfo_new(TMNT, 0, 0, 1);
    seL4_SetMR(0, id);
    info_reply = seL4_Call(env.endpoint.cptr, info_tmnt);

}

#else

void
client_program( void )
{

#ifdef CONSUMER_PRODUCER
    seL4_MessageInfo_t UNUSED info_reply, info_tmnt, info_consumer, info_producer;
#endif

    int id;

    seL4_MessageInfo_t info_init = seL4_MessageInfo_new(INIT, 0, 0, 1);
    info_reply = seL4_Call(env.endpoint.cptr, info_init);
    id = seL4_GetMR(0);

#ifdef CONSUMER_PRODUCER
    if (id % 2 == 1) {
        /* This is a producer */
        for (int i = 0;i < 10;i ++) {
            info_producer = seL4_MessageInfo_new(PRODUCER, 0, 0, 2);
            seL4_SetMR(0, id);
            seL4_SetMR(1, i);
            info_reply = seL4_Call(env.endpoint.cptr, info_producer);
        }
    } else {
        /* This is a consumer */
        for (int i = 0;i < 10;i ++) {
            info_consumer = seL4_MessageInfo_new(CONSUMER, 0, 0, 2);
            seL4_SetMR(0, id);
            seL4_SetMR(1, i);
            info_reply = seL4_Call(env.endpoint.cptr, info_consumer);
        }
    }

#endif

#ifdef IMMD_DEFER
    if (id % 2 == 1) {
        /* This is a immdiate*/
        for (int i = 0;i < 5000;i ++) {
            info_producer = seL4_MessageInfo_new(IMMD, 0, 0, 2);
            seL4_SetMR(0, id);
            seL4_SetMR(1, i);
            info_reply = seL4_Call(env.endpoint.cptr, info_producer);
        }
    } else {
        /* This is a defer */
        for (int i = 0;i < 1;i ++) {
            info_consumer = seL4_MessageInfo_new(DEFER, 0, 0, 2);
            seL4_SetMR(0, id);
            seL4_SetMR(1, i);
            info_reply = seL4_Call(env.endpoint.cptr, info_consumer);
        }
    }

#endif

    info_tmnt = seL4_MessageInfo_new(TMNT, 0, 0, 1);
    seL4_SetMR(0, id);
    info_reply = seL4_Call(env.endpoint.cptr, info_tmnt);

    return;
}
#endif
#endif


int
client_initial()
{
    int error;
    sel4utils_thread_t *tcb;
    seL4_CPtr cspace_cap;

    tcb = malloc(sizeof(sel4utils_thread_t));
    if (tcb == NULL) {
        printf("Cannot allocate a new sel4 thread.\n");
        return 0;
    }

    cspace_cap = simple_get_cnode(&env.simple);

    error = sel4utils_configure_thread(&env.vka, &env.vspace, &env.vspace, seL4_CapNull,
                                       seL4_MaxPrio, cspace_cap, seL4_NilData, tcb);
    assert(! error);
    printf("client threads initialized successfully\n");

    error = sel4utils_start_thread(tcb, (sel4utils_thread_entry_fn) client_program, NULL, NULL, 1);
    assert(! error);

    return 1;
}


#ifdef IMMD_DEFER

seL4_Word
process_message(seL4_MessageInfo_t info, seL4_MessageInfo_t **reply)
{
    int label = seL4_MessageInfo_get_label(info);
    seL4_MessageInfo_t temp;
    int client_id, error;

    switch(label) {

    case INIT:
        client_id = initial_client();

        cspacepath_t slot_temp;

        error = allocman_cspace_alloc(allocman, &slot_temp);
        assert(error == 0);

        /* check if OK to multi-cast */
        error = vka_cnode_saveCaller(&slot_temp);
        if (error != seL4_NoError) {
            printf("device_timer_save_caller_as_waiter failed to save caller.");
        }

        reply_eps[client_id] = (seL4_Word) slot_temp.offset;

        if (client_barrier()) {
            seL4_MessageInfo_t reply;

            seL4_SetMR(0, 0);
            seL4_SetMR(1, 1);

            reply = seL4_MessageInfo_new(INIT, 0, 0, 2);
            seL4_Send(reply_eps[0], reply);
            process_message_multicast();
        }

        return -1;
    break;

    case IMMD:
#ifdef BENCHMARK_ENTIRE
if (! started) {
    started = 1;
    rdtsc_start();
}
#endif
        // printf("IMMD\n");


        temp = seL4_MessageInfo_new(IMMD, 0, 0, 1);
        *reply = &temp;

        return 0;
    break;

    case DEFER:
#ifdef BENCHMARK_ENTIRE
if (! started) {
    started = 1;
    rdtsc_start();
}
#endif
        // printf("DEFER\n");

        error = vka_cnode_saveCaller(&pool->t_running->t->slot);
        if (error != seL4_NoError) {
            printf("device_timer_save_caller_as_waiter failed to save caller.");
        }
        //
        // seL4_CPtr notification;
        // notification = vka_alloc_notification_leaky(&env.vka);
        //
        // seL4_timer_t *timer = sel4platsupport_get_default_timer(&env.vka, &env.vspace, &env.simple,
        //                                                  notification);

        timer_oneshot_relative(timer->timer, 19 * NS_IN_MS);
        uint64_t timer_start = timer_get_time(timer->timer);
        // rdtsc_start();
        for (; ;) {
            uint64_t timer_end = timer_get_time(timer->timer);
            if (timer_end == 0 || timer_end > timer_start) {
                break;
            }
            // printf("printf\n");
            thread_sleep(lock_global, NULL);
        };
// rdtsc_end();
// printf("COLLECTION - MS * 9 %llu %llu %llu\n", (end - start), start, end);


        temp = seL4_MessageInfo_new(DEFER, 0, 0, 1);
        *reply = &temp;
        return 1;
    break;

    case TMNT:
    // client_id = seL4_GetMR(0);
    // printf("Client %d terminates\n", client_id);

        terminate_num ++;

        if (terminate_num == client_num) {
#ifdef BENCHMARK_ENTIRE
rdtsc_end();
printf("COLLECTION - total time %llu %d 19 1 5000\n", (end - start), inv_count);
#endif
            printf("end of test\n");
        }

        thread_exit();
        return -1;
    break;

    }

    return -1;
}


void *
server_loop(void *sync_prim)
{

    seL4_MessageInfo_t *reply = NULL;
    int res;

    seL4_MessageInfo_t info = seL4_Recv(env.endpoint.cptr, NULL);

    while (1) {
        res = process_message(info, &reply);
        if (res == 1) {
            seL4_Send(pool->t_running->t->slot.offset, *reply);
            info = seL4_Recv(env.endpoint.cptr, NULL);
        } else if (res == 0) {
            seL4_Reply(*reply);
            thread_wakeup(lock_global, NULL);
            info = seL4_Recv(env.endpoint.cptr, NULL);
            // info = seL4_ReplyRecv(env.endpoint.cptr, *reply, NULL);
        } else {
            info = seL4_Recv(env.endpoint.cptr, NULL);
        }
    }

    return sync_prim;
}
#endif



#ifdef EVENT_DRIVEN

seL4_Word
process_message(seL4_MessageInfo_t info, seL4_MessageInfo_t **reply)
{
    int label = seL4_MessageInfo_get_label(info);
    seL4_MessageInfo_t temp;
    int client_id, error;

    switch(label) {

    case INIT:
        client_id = initial_client();

        cspacepath_t slot_temp;

        error = allocman_cspace_alloc(allocman, &slot_temp);
        assert(error == 0);

        /* check if OK to multi-cast */
        error = vka_cnode_saveCaller(&slot_temp);
        if (error != seL4_NoError) {
            printf("device_timer_save_caller_as_waiter failed to save caller.");
        }

        reply_eps[client_id] = (seL4_Word) slot_temp.offset;

        if (client_barrier()) {
            seL4_MessageInfo_t reply;

            seL4_SetMR(0, 0);
            seL4_SetMR(1, 1);

            reply = seL4_MessageInfo_new(INIT, 0, 0, 2);
            seL4_Send(reply_eps[0], reply);
            process_message_multicast();
        }


        return -1;
    break;

    case IMMD:
#ifdef BENCHMARK_ENTIRE
if (! started) {
    started = 1;
    rdtsc_start();
}
#endif
        // printf("IMMD\n");
        temp = seL4_MessageInfo_new(IMMD, 0, 0, 1);
        *reply = &temp;

        return 0;
    break;

    case DEFER:
#ifdef BENCHMARK_ENTIRE
if (! started) {
    started = 1;
    rdtsc_start();
}
#endif

        // printf("DEFER\n");
rdtsc_start();
        timer_oneshot_relative(timer->timer, 11 * NS_IN_MS);
        uint64_t timer_start = timer_get_time(timer->timer);
        for (; ;) {
            uint64_t timer_end = timer_get_time(timer->timer);
            if (timer_end == 0 || timer_end > timer_start) {
                break;
            }
        };

        // pstimer_t *local_timer = tsc_get_timer(timer->timer);
        //
        // timer_start = timer_get_time(local_timer);
        // uint64_t timer_end = timer_get_time(local_timer);
        //
        // printf("start timer: %llu\n", timer_start);
        // printf("end timer: %llu\n", timer_end);

rdtsc_end();
printf("COLLECTION - MS * 11 %llu %llu %llu\n", (end - start), start, end);


        temp = seL4_MessageInfo_new(DEFER, 0, 0, 1);
        *reply = &temp;
        return 0;
    break;

    case TMNT:
    // client_id = seL4_GetMR(0);
    // printf("Client %d terminates\n", client_id);

        terminate_num ++;

        if (terminate_num == client_num) {
#ifdef BENCHMARK_ENTIRE
rdtsc_end();
printf("COLLECTION - total time %llu 19 1 5000\n", (end - start));
#endif
            printf("end of test\n");
        }

        // thread_exit();

        return -1;
    break;

    }

    return -1;
}


void
server_loop(void)
{

    seL4_MessageInfo_t *reply = NULL;
    int res;

    seL4_MessageInfo_t info = seL4_Recv(env.endpoint.cptr, NULL);

    while (1) {
        res = process_message(info, &reply);
        if (res == 1) {
            // seL4_Send(slot.offset, *reply);
            // info = seL4_Recv(env.endpoint.cptr, NULL);
            info = seL4_ReplyRecv(env.endpoint.cptr, *reply, NULL);
        } else if (res == 0) {
            // seL4_Reply(*reply);
            // info = seL4_Recv(env.endpoint.cptr, NULL);
            info = seL4_ReplyRecv(env.endpoint.cptr, *reply, NULL);
        } else {
            info = seL4_Recv(env.endpoint.cptr, NULL);
        }
    }

    return;
}
#endif



void *
test_func(void *arg1)
{
    printf("Great!\n");

    thread_exit();

    return arg1;
}



// #ifdef SEL4_THREAD
// int
// sel4_thread_start(sel4utils_thread_t *thread, void *sync_prim)
// {
//     int error;
//
//     /* Resume with seL4_TCB_Resume */
//     printf("%p\n", (void *) env.endpoint.cptr);
//     error = sel4utils_start_thread(thread, (sel4utils_thread_entry_fn) server_loop, sync_prim, NULL, 0);
//     if (error) {
//         return 0;
//     }
//
//     return 1;
// }
// #endif



/*
    Encapsulate.
*/
#if  defined(GREEN_THREAD) || defined(IMMD_DEFER)
int
thread_creation(void *sync_prim, int num)
{
    assert(num > 0);
    int i, res = 0;

    for (i = 0;i < num;i ++)
        res = thread_create(allocman, &env.vspace, server_loop, sync_prim);

    return res;
}
#endif


void *main_continued(void *arg UNUSED)
{

    /* elf region data */
    int num_elf_regions;
    sel4utils_elf_region_t elf_regions[MAX_REGIONS];

    /* Print welcome banner. */
    printf("\n");
    printf("seL4 Test\n");
    printf("=========\n");
    printf("\n");

    /* allocate lots of untyped memory for tests to use */
    num_untypeds = populate_untypeds(untypeds);

    /* create a frame that will act as the init data, we can then map that
     * in to target processes */
    env.init = (test_init_data_t *) vspace_new_pages(&env.vspace, seL4_AllRights, 1, PAGE_BITS_4K);
    assert(env.init != NULL);

    /* copy the cap to map into the remote process */
    cspacepath_t src, dest;
    vka_cspace_make_path(&env.vka, vspace_get_cap(&env.vspace, env.init), &src);


    UNUSED int error = vka_cspace_alloc(&env.vka, &env.init_frame_cap_copy);
    assert(error == 0);
    vka_cspace_make_path(&env.vka, env.init_frame_cap_copy, &dest);
    error = vka_cnode_copy(&dest, &src, seL4_AllRights);
    assert(error == 0);

    /* copy the untyped size bits list across to the init frame */
    memcpy(env.init->untyped_size_bits_list, untyped_size_bits_list, sizeof(uint8_t) * num_untypeds);

    /* parse elf region data about the test image to pass to the tests app */
    num_elf_regions = sel4utils_elf_num_regions(TESTS_APP);
    assert(num_elf_regions < MAX_REGIONS);
    sel4utils_elf_reserve(NULL, TESTS_APP, elf_regions);

    /* copy the region list for the process to clone itself */
    memcpy(env.init->elf_regions, elf_regions, sizeof(sel4utils_elf_region_t) * num_elf_regions);
    env.init->num_elf_regions = num_elf_regions;

    /* setup init data that won't change test-to-test */
    env.init->priority = seL4_MaxPrio;
    plat_init(&env);

    /* now run the tests */
    //sel4test_run_tests("sel4test", run_test);
    // sel4test_run_tests_new("sel4test", run_test_new);

    asm volatile("CPUID\n\t");

    client_count = 100;
printf("start\n");

#ifdef EVENT_CONTINUATION
/* Create an endpoint */
vka_object_t ep_object = {0};
error = vka_alloc_endpoint(&env.vka, &ep_object);
ZF_LOGF_IFERR(error, "Failed to allocate new endpoint object.\n");

cspacepath_t helper_slot;
vka_cspace_make_path(&env.vka, ep_object.cptr, &helper_slot);
helper_ep = helper_slot.offset;

printf("helper finished %d\n", helper_ep);
#endif

#ifdef WAIT_SEND_WAIT
#ifdef SEL4_THREAD
    /* create a series of tokens */
    for (int i = 0;i < client_count;i ++) {
        // seL4_CPtr notification;
        // notification = vka_alloc_notification_leaky(&env.vka);
        // sync_mutex_init(&tokens[i], notification);

        // sync_mutex_lock(&tokens[i]);

        tokenps[i] = vka_alloc_notification_leaky(&env.vka);
    }
    printf("tokens initialized successful\n");
#endif

#ifdef GREEN_THREAD
    /* create a series of tokens */
    for (int i = 0;i < client_count;i ++) {
        tokens[i] = thread_lock_create();
        tokens[i]->held = 1;
    }
#endif
#endif

#ifdef CLIENT_MULTIPLE
    for (int i = 0;i < client_count;i ++)
        client_initial();
#endif



#ifdef CLIENT_SINGLE
/* Create an endpoint */
vka_object_t ep_object1 = {0};
error = vka_alloc_endpoint(&env.vka, &ep_object1);
ZF_LOGF_IFERR(error, "Failed to allocate new endpoint object.\n");

cspacepath_t client_slot;
vka_cspace_make_path(&env.vka, ep_object1.cptr, &client_slot);
client_ep = client_slot.offset;

printf("client finished %d\n", client_ep);

    client_initial();
#endif

#ifdef EVENT_CONTINUATION
    helper_initial();

    initial_client_pool(client_count);

    server_loop();
#endif

#ifdef IMMD_DEFER
    thread_initial();

    initial_client_pool(client_count);

    seL4_CPtr notification;
    notification = vka_alloc_notification_leaky(&env.vka);

    timer = sel4platsupport_get_default_timer(&env.vka, &env.vspace, &env.simple,
                                                     notification);

    int t_id = thread_creation(NULL, client_count);
    thread_resume(t_id);

#endif

#ifdef EVENT_DRIVEN
    initial_client_pool(client_count);

    seL4_CPtr notification;
    notification = vka_alloc_notification_leaky(&env.vka);

    timer = sel4platsupport_get_default_timer(&env.vka, &env.vspace, &env.simple,
                                                     notification);

    server_loop();
#endif

#ifdef GREEN_THREAD
    thread_initial();
    initial_client_pool(client_count);
printf("initialized\n");


    //
    // sel4bench_init();
    //
    // ccnt_t num = sel4bench_get_cycle_count();
    // printf("cycle count: %llu.\n", num);
    // exit(0);
    // pool->thread_creation = thread_creation;


#ifdef THREAD_LOCK
    thread_lock_t *sync_prim = thread_lock_create();
    assert(sync_prim != NULL);
    sync_prim->held = 1;
#endif

#ifdef THREAD_SEMAPHORE
    thread_semaphore_t *sync_prim = thread_semaphore_create();
    assert(sync_prim != NULL);
#endif

#ifdef THREAD_CV
    int token;
    int *sync_prim = &token;
#endif

    int t_id = thread_creation((void *) sync_prim, client_count);
    thread_resume(t_id);

    printf("end of test\n");

#endif // -- GREEN_THREAD

#ifdef SEL4_THREAD


    /* initial clients pool */
    initial_client_pool(client_count);

    sel4_threads_initial(client_count);



    // printf("end of test\n");

    // int res = seL4_TCB_Resume(thread.tcb.cptr);
    // printf("RESULT CAN BE SHOWN: %d.\n", res);

#endif // -- SEL4_THREAD

    // server_loop(NULL);
    // while (1) {
    //     printf("TESTTT\n");
    // }

    /* Qianrui Zhao thesis */
    //run_test_new("client 01");

    return NULL;
}


void *
cannot_believe(void *arg)
{
    printf("CHAN\n");
    printf("CHECK CHECK\n");

    thread_self();

    thread_exit();

    return arg;
}


void
benchmark_modeswitch_costs(void)
{
    uint64_t UNUSED tokernel_start, UNUSED tokernel_end, UNUSED touser_start, UNUSED touser_end;

    tokernel_start = rdtsc();
    seL4_DebugHalt();
    touser_end = rdtsc();
    tokernel_end = seL4_GetMR(4);
    tokernel_end <<= 32;
    tokernel_end |= seL4_GetMR(3);

    touser_start = seL4_GetMR(5);
    touser_start <<= 32;
    touser_start |= seL4_GetMR(6);

   printf("Stamps: To kernel: start %llu, end %llu, interval %llu; To user: start %llu, end %llu, interval %llu.\n",
           tokernel_start, tokernel_end, (tokernel_end - tokernel_start), touser_start, touser_end, (touser_end - touser_start));
}



/* test functions */
void test_lock_list()
{
    thread_initial();

    int test_num = 200;

    thread_lock_t *lock_list[test_num];

    for (int i = 0;i < test_num;i ++)
        lock_list[i] = thread_lock_create();

    assert(pool->sync_prim_start != NULL);
    assert(pool->sync_prim_end != NULL);

    for (int i = 50;i < test_num;i ++)
        thread_lock_destory(lock_list[i]);

    for (int i = 49;i > -1;i --)
        thread_lock_destory(lock_list[i]);

    assert(pool->sync_prim_start == NULL);
    assert(pool->sync_prim_end == NULL);
}



void *
test_nested_join_func(void *test_num)
{
    printf("Thread %d start to run..\n", pool->t_running->t->t_id);

    int new_num = (*(int *) test_num) - 1;
    if (new_num > 0) {
        int t_id = thread_create(allocman, &env.vspace, test_nested_join_func, &new_num);
        printf("Thread %d created thread %d.\n", pool->t_running->t->t_id, t_id);

        // thread_yield();
        printf("Thread %d continue..\n", pool->t_running->t->t_id);

        thread_join(t_id);
    }

    // thread_pool_info(1);
    printf("Thread %d exit.\n", pool->t_running->t->t_id);

    return NULL;
}



void
test_nested_join(int test_num)
{
    thread_initial();

    int t_id = thread_create(allocman, &env.vspace, test_nested_join_func, &test_num);
    printf("Created thread %d\n", t_id);

    thread_join(t_id);

    printf("Test join - nested join passed.\n");

    return;
}



void *
test_multi_join_func(void *num)
{
    int t_id = pool->t_running->t->t_id;

    printf("Thread %d starts..\n", t_id);

    printf("Perform calculation..\n");

    *((int *) num) += t_id;

    printf("Thread %d exits.\n", t_id);

    return NULL;
}



void
test_multi_join(int test_num)
{
    thread_initial();

    int thread_ids[test_num];
    int sum = 0;

    for (int i = 0;i < test_num;i ++)
         thread_ids[i] = thread_create(allocman, &env.vspace, test_multi_join_func, &sum);

    for (int i = 0;i < test_num;i ++)
        thread_join(thread_ids[i]);

    int sum_thread_ids = 0;
    for (int i = 0;i < test_num;i ++)
        sum_thread_ids += thread_ids[i];


    printf("Test join - multi join ");
    if (sum == sum_thread_ids)
        printf("passed\n");
    else
        printf("failed\n");

    return;
}



int main(void)
{
    int error;
    seL4_BootInfo *info = platsupport_get_bootinfo();

#ifdef CONFIG_DEBUG_BUILD
    seL4_DebugNameThread(seL4_CapInitThreadTCB, "sel4test-driver");
#endif

    compile_time_assert(init_data_fits_in_ipc_buffer, sizeof(test_init_data_t) < PAGE_SIZE_4K);
    /* initialise libsel4simple, which abstracts away which kernel version
     * we are running on */
    simple_default_init_bootinfo(&env.simple, info);

    /* initialise the test environment - allocator, cspace manager, vspace
     * manager, timer
     */
    init_env(&env);

    /* Allocate slots for, and obtain the caps for, the hardware we will be
     * using, in the same function.
     */
    sel4platsupport_init_default_serial_caps(&env.vka, &env.vspace, &env.simple, &env.serial_objects);

    /* Construct a vka wrapper for returning the serial frame. We need to
     * create this wrapper as the actual vka implementation will only
     * allocate/return any given device frame once. As we already allocated it
     * in init_serial_caps when we the platsupport_serial_setup_simple attempts
     * to allocate it will fail. This wrapper just returns a copy of the one
     * we already allocated, whilst passing all other requests on to the
     * actual vka
     */
    vka_t serial_vka = env.vka;
    serial_vka.utspace_alloc_at = arch_get_serial_utspace_alloc_at(&env);

    /* enable serial driver */
    platsupport_serial_setup_simple(&env.vspace, &env.simple, &serial_vka);

    /* init_timer_caps calls acpi_init(), which does unconditional printfs,
     * so it can't go before platsupport_serial_setup_simple().
    */
    error = sel4platsupport_init_default_timer_caps(&env.vka, &env.vspace, &env.simple, &env.timer_objects);
    ZF_LOGF_IF(error, "Failed to init default timer caps");
    simple_print(&env.simple);

    /* switch to a bigger, safer stack with a guard page
     * before starting the tests */
    printf("Switching to a safer, bigger stack... ");
    fflush(stdout);

    // printf("first IPC buffer: %p\n", seL4_GetIPCBuffer());

    /* test thread library */
    // thread_initial();
    // if (pool != NULL)
    //     printf("Create a pool.\n");
    //
    // void *arg = NULL;
    // printf("What..");
    // printf("New thread id: %d\n", thread_create(&env.vspace, test_func, arg));
    // printf("New thread id: %d\n", thread_create(&env.vspace, cannot_believe, arg));
    // printf("New thread id: %d\n", thread_create(&env.vspace, cannot_believe, arg));
    // thread_pool_info();
    // thread_info(1);
    // thread_yield();
    //
    // thread_pool_info();
    //
    // printf("end\n");
    /* test thread library end */

    /* Get execution time for kernel context-switching */
    // uint64_t save_start, save_end, restore_start, restore_end;
    // uint32_t save_elapsed, restore_elapsed;
    //
    // for (int i = 0;i < 1050;i ++) {
    //
    // save_start = rdtsc();
    // seL4_SetMR(0, 0xDEADBEEF);
    // seL4_DebugHalt();
    // save_end = seL4_GetMR(5);
    // save_end <<= 32;
    // save_end |= seL4_GetMR(6);
    //
    // save_elapsed = save_end - save_start;
    //
    // restore_start = seL4_GetMR(3);
    // restore_start <<= 32;
    // restore_start |= seL4_GetMR(4);
    // restore_end = rdtsc();
    //
    // restore_elapsed = restore_end - restore_start;
    //
    // printf("%d\n", i);
    //
    // printf("COLLECTION - Got RDTSC values: save %u, restore %u.\n"
    //         "COLLECTION - \tSave: start %u_%u, end %u_%u.\n"
    //         "COLLECTION - \tEnd: start %u_%u, end %u_%u.\n",
    //         save_elapsed, restore_elapsed,
    //         (uint32_t)((save_start >> 32) & UINT32_MAX),
    //         (uint32_t)((save_start) & UINT32_MAX),
    //         (uint32_t)((save_end >> 32) & UINT32_MAX),
    //         (uint32_t)((save_end) & UINTseL4_G2_MAX),
    //         (uint32_t)((restore_start >> 32) & UINT32_MAX),
    //         (uint32_t)((restore_start) & UINT32_MAX),
    //         (uint32_t)((restore_end >> 32) & UINT32_MAX),
    //         (uint32_t)((restore_end) & UINT32_MAX));
    // }

    /* Benchmark call for mode switch */
    // printf("Here, before\n");
    // for (int i = 0;i < 1000;i ++)
    //     benchmark_modeswitch_costs();
    // printf("Here, after\n");
    // printf("end of test\n");

    /* Test functions */
    // printf("Test for adding/ removing lock from list starts:\n");
    // test_lock_list();

    /* Demo functions */
    printf("\nTests begin\n");

    // printf("Test join - multi join\n");
    // printf("------------ start ------------\n");
    // test_multi_join(5);
    // printf("------------ end ------------\n\n");

    // printf("Test join - nested join\n");
    // printf("------------ start ------------\n");
    // test_nested_join(20);
    // printf("------------ end ------------\n\n");

    // printf("the end of test\n");

    printf("Test synchronous primitives, sleep and wakeup - producer_consumer scenario\n");
    printf("------------ start ------------\n");
    void *res;
    error = sel4utils_run_on_stack(&env.vspace, main_continued, NULL, &res);
    test_assert_fatal(error == 0);
    test_assert_fatal(res == 0);
    printf("------------ end ------------\n\n");

    // void *res;
    // error = sel4utils_run_on_stack(&env.vspace, main_continued, NULL, &res);
    // test_assert_fatal(error == 0);
    // test_assert_fatal(res == 0);

    return 0;
}
