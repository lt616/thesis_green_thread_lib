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

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include <sel4/sel4.h>
#include <vka/object.h>
#include <vka/capops.h>
#include <sel4utils/util.h>
#include <sel4utils/mapping.h>

#include "../helpers.h"
#include "frame_type.h"

int touch_data(void *vaddr, char old_data, char new_data, size_t size_bits) {
    char *data = (char*)vaddr;
    /* we walk backwards testing each byte of the frame to ensure a couple of things
     1. a frame of the correct size is mapped in
     2. for larger frames that no part of the large frame shares a region with another part.
        this could happen with ARM large pages and super sections as they are comprised of
        16 entries in a paging structure, and not just a single entry in a higher level structure
     */
    for (size_t i = BIT(size_bits) - 1; i > 1; i--) {
        test_assert(data[i] == old_data);
        data[i] = new_data;
        test_assert(data[i] == new_data);
    }
    return sel4test_get_result();
}

static int
test_frame_exported(env_t env)
{
    /* Reserve a location that is aligned and large enough to map our
     * largest kind of frame */
    void *vaddr;
    reservation_t reserve = vspace_reserve_range_aligned(&env->vspace, VSPACE_RV_SIZE, VSPACE_RV_ALIGN_BITS,
                                                         seL4_AllRights, 1, &vaddr);
    test_assert(reserve.res);

    /* loop through frame sizes, allocate, map and touch them until we run out
     * of memory. */
    size_t mem_total = 0;
    int err;
    for (int i = 0; i < ARRAY_SIZE(frame_types); i++) {
        bool once = false;
        while (1) {
            /* Allocate the frame */
            seL4_CPtr frame = vka_alloc_frame_leaky(&env->vka, frame_types[i].size_bits);
            if (!frame) {
                break;
            }
            once = true;
            mem_total += BIT(frame_types[i].size_bits);

            uintptr_t cookie = 0;
            err = vspace_map_pages_at_vaddr(&env->vspace, &frame, &cookie, (void*)vaddr, 1, frame_types[i].size_bits, reserve);
            test_assert(err == seL4_NoError);

            /* Touch the memory */
            char *data = (char*)vaddr;
            test_assert(touch_data(vaddr, 0, 'U', frame_types[i].size_bits));

            err = seL4_ARCH_Page_Remap(frame,
                                       env->page_directory,
                                       seL4_AllRights,
                                       seL4_ARCH_Default_VMAttributes);
            test_assert(!err);
            /* ensure the memory is what it was before and touch it again */
            test_assert(touch_data(vaddr, 'U', 'V', frame_types[i].size_bits));

            vspace_unmap_pages(&env->vspace, (void*)vaddr, 1, frame_types[i].size_bits, VSPACE_PRESERVE);
            test_assert(err == seL4_NoError);
        }
        test_assert(once);
    }
    return sel4test_get_result();
}
DEFINE_TEST(FRAMEEXPORTS0001, "Test that we can access all exported frames", test_frame_exported)

#if defined(CONFIG_ARCH_ARM)
/* XN support is only implemented for ARM currently. */

/* Function that generates a fault. If we're mapped XN we should instruction
 * fault at the start of the function. If not we should data fault on 0x42.
 */
static int fault(seL4_Word arg1, seL4_Word arg2, seL4_Word arg3, seL4_Word arg4)
{
    *(char*)0x42 = 'c';
    return 0;
}

/* Wait for a VM fault originating on the given EP the return the virtual
 * address it occurred at. Returns the sentinel 0xffffffff if the message
 * received was not a VM fault.
 */
static int handle(seL4_CPtr fault_ep, seL4_Word arg2, seL4_Word arg3, seL4_Word arg4)
{
    seL4_MessageInfo_t info = seL4_Recv(fault_ep, NULL);
    if (seL4_MessageInfo_get_label(info) == seL4_Fault_VMFault) {
        return (int)seL4_GetMR(1);
    } else {
        return (int)0xffffffff;
    }
}

static int test_xn(env_t env, seL4_ArchObjectType frame_type)
{
    int err;
    /* Find the size of the frame type we want to test. */
    int sz_bits = 0;
    for (unsigned int i = 0; i < ARRAY_SIZE(frame_types); i++) {
        if (frame_types[i].type == frame_type) {
            sz_bits = frame_types[i].size_bits;
            break;
        }
    }
    test_assert(sz_bits != 0);

    /* Get ourselves a frame. */
    seL4_CPtr frame_cap = vka_alloc_frame_leaky(&env->vka, sz_bits);
    test_assert(frame_cap != seL4_CapNull);

    /* Map it in */
    uintptr_t cookie = 0;
    void *dest = vspace_map_pages(&env->vspace, &frame_cap, &cookie, seL4_AllRights, 1, sz_bits, 1);
    test_assert(dest != NULL);

    /* Set up a function we're going to have another thread call. Assume that
     * the function is no more than 100 bytes long.
     */
    memcpy(dest, (void*)fault, 100);

    /* Unify the instruction and data caches so our code is seen */
    seL4_ARM_Page_Unify_Instruction(frame_cap, 0, BIT(sz_bits));

    /* First setup a fault endpoint.
     */
    seL4_CPtr fault_ep = vka_alloc_endpoint_leaky(&env->vka);
    cspacepath_t path;
    vka_cspace_make_path(&env->vka, fault_ep, &path);
    test_assert(fault_ep != seL4_CapNull);

    /* Then setup the thread that will, itself, fault. */
    helper_thread_t faulter;
    create_helper_thread(env, &faulter);
    set_helper_priority(&faulter, 100);
    err = seL4_TCB_Configure(faulter.thread.tcb.cptr,
                             fault_ep,
                             seL4_PrioProps_new(100, 100),
                             env->cspace_root,
                             seL4_CapData_Guard_new(0, seL4_WordBits - env->cspace_size_bits),
                             env->page_directory, seL4_NilData,
                             faulter.thread.ipc_buffer_addr,
                             faulter.thread.ipc_buffer);
    start_helper(env, &faulter, dest, 0, 0, 0 ,0);

    /* Now a fault handler that will catch and diagnose its fault. */
    helper_thread_t handler;
    create_helper_thread(env, &handler);
    set_helper_priority(&handler, 100);
    start_helper(env, &handler, handle, fault_ep, 0, 0, 0);

    /* Wait for the fault to happen */
    void *res = (void*)(seL4_Word)wait_for_helper(&handler);

    test_assert(res == (void*)0x42);

    cleanup_helper(env, &handler);
    cleanup_helper(env, &faulter);

    /* Now let's remap the page with XN set and confirm that we can't execute
     * in it any more.
     */
    err = seL4_ARM_Page_Remap(frame_cap, env->page_directory, seL4_AllRights,
                              seL4_ARM_Default_VMAttributes | seL4_ARM_ExecuteNever);
    test_assert(err == 0);

    /* The page should still contain our code from before. */
    test_assert(!memcmp(dest, (void*)fault, 100));

    /* We need to reallocate a fault EP because the thread cleanup above
     * inadvertently destroys the EP we were using.
     */
    fault_ep = vka_alloc_endpoint_leaky(&env->vka);
    test_assert(fault_ep != seL4_CapNull);

    /* Recreate our two threads. */
    create_helper_thread(env, &faulter);
    set_helper_priority(&faulter, 100);
    err = seL4_TCB_Configure(faulter.thread.tcb.cptr,
                             fault_ep,
                             seL4_PrioProps_new(100, 100),
                             env->cspace_root,
                             seL4_CapData_Guard_new(0, seL4_WordBits - env->cspace_size_bits),
                             env->page_directory, seL4_NilData,
                             faulter.thread.ipc_buffer_addr,
                             faulter.thread.ipc_buffer);
    start_helper(env, &faulter, dest, 0, 0, 0 ,0);
    create_helper_thread(env, &handler);
    set_helper_priority(&handler, 100);
    start_helper(env, &handler, handle, fault_ep, 0, 0, 0);

    /* Wait for the fault to happen */
    res = (void*)(seL4_Word)wait_for_helper(&handler);

    /* Confirm that, this time, we faulted at the start of the XN-mapped page. */
    test_assert(res == (void*)dest);

    /* Resource tear down. */
    cleanup_helper(env, &handler);
    cleanup_helper(env, &faulter);

    return sel4test_get_result();
}

static int test_xn_small_frame(env_t env)
{
    return test_xn(env, seL4_ARM_SmallPageObject);
}
DEFINE_TEST(FRAMEXN0001, "Test that we can map a small frame XN", test_xn_small_frame)

static int test_xn_large_frame(env_t env)
{
    return test_xn(env, seL4_ARM_LargePageObject);
}
DEFINE_TEST(FRAMEXN0002, "Test that we can map a large frame XN", test_xn_large_frame)

#endif
#ifdef CONFIG_HAVE_TIMER
static int test_device_frame_ipcbuf(env_t env)
{
    cspacepath_t path;
    cspacepath_t frame_path;
    int error;
    error = vka_cspace_alloc_path(&env->vka, &path);
    vka_cspace_make_path(&env->vka, env->timer->frame.cptr, &frame_path);
    vka_cnode_copy(&path, &frame_path, seL4_AllRights);
    test_assert(error == 0);

    helper_thread_t other;
    create_helper_thread(env, &other);
    /* Try and create a thread with a device frame as its IPC buffer */
    error = seL4_TCB_Configure(other.thread.tcb.cptr,
                               0,
                               seL4_PrioProps_new(100, 100),
                               env->cspace_root,
                               seL4_CapData_Guard_new(0, seL4_WordBits - env->cspace_size_bits),
                               env->page_directory, seL4_NilData,
                               other.thread.ipc_buffer_addr,
                               path.capPtr);
    test_neq(error, 0);
    cleanup_helper(env, &other);

    return sel4test_get_result();
}
DEFINE_TEST(FRAMEDIPC0001, "Test that we cannot create a thread with an IPC buffer that is a frame", test_device_frame_ipcbuf)

static int wait_func(seL4_Word ep)
{
    seL4_Send(ep, seL4_MessageInfo_new(0, 0, 0, 0));
    seL4_Send(ep, seL4_MessageInfo_new(0, 0, 0, 0));
    return 0;
}

static int test_switch_device_frame_ipcbuf(env_t env)
{
    cspacepath_t path;
    cspacepath_t frame_path;
    int error;
    seL4_CPtr ep;
    error = vka_cspace_alloc_path(&env->vka, &path);
    vka_cspace_make_path(&env->vka, env->timer->frame.cptr, &frame_path);
    vka_cnode_copy(&path, &frame_path, seL4_AllRights);
    test_assert(error == 0);

    ep = vka_alloc_endpoint_leaky(&env->vka);
    test_assert(ep != seL4_CapNull);

    helper_thread_t other;
    create_helper_thread(env, &other);
    /* start the thread and make sure it works */
    start_helper(env, &other, (helper_fn_t)wait_func, (seL4_Word)ep, 0, 0, 0);
    seL4_Recv(ep, NULL);
    /* now switch its IPC buffer, which should fail */
    error = seL4_TCB_SetIPCBuffer(other.thread.tcb.cptr, other.thread.ipc_buffer_addr, path.capPtr);
    test_neq(error, 0);
    /* thread should still be working */
    seL4_Recv(ep, NULL);
    /* all done */
    wait_for_helper(&other);
    cleanup_helper(env, &other);
    return sel4test_get_result();
}
DEFINE_TEST(FRAMEDIPC0002, "Test that we cannot switch a threads IPC buffer to a device frame", test_switch_device_frame_ipcbuf)
#endif /* CONFIG_HAVE_TIMER */
