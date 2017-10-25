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
#ifndef __TEST_SHMEM_INIT_DATA_H
#define __TEST_SHMEM_INIT_DATA_H

#include <stdint.h>

#include <sel4/sel4.h>
#include <sel4utils/elf.h>

/* max test name size */
#define TEST_NAME_MAX 40

/* Increase if the sel4test-tests binary
 * has new loadable sections added */
#define MAX_REGIONS 4

/* Init data shared between sel4test-driver and the sel4test-tests app -- the
 * sel4test-driver creates a shmem page to be shared between the driver and the
 * test child processes, and uses this struct to pass the data in the shmem
 * page.
 *
 * This file is symlinked from the sel4test-driver into the sel4test child
 * process.
 *
 * all caps are in the sel4test-tests process' cspace */
typedef struct {
    /* page directory of the test process */
    seL4_CPtr page_directory;
    /* root cnode of the test process */
    seL4_CPtr root_cnode;
    /* tcb of the test process */
    seL4_CPtr tcb;
    /* the domain cap */
    seL4_CPtr domain;
    /* asid pool cap for the test process to use when creating new processes */
    seL4_CPtr asid_pool;
    seL4_CPtr asid_ctrl;
#ifdef CONFIG_IOMMU
    seL4_CPtr io_space;
#endif /* CONFIG_IOMMU */
#ifdef CONFIG_ARM_SMMU
    seL4_SlotRegion io_space_caps;
#endif
    /* cap to the sel4platsupport default timer irq handler */
    seL4_CPtr timer_irq_cap;
    /* cap to the sel4platsupport default timer device untyped */
    seL4_CPtr timer_dev_ut_cap;
    /* physical address of the timeout timer */
    uintptr_t timer_paddr;

    /* caps for clock timer */
    seL4_CPtr clock_timer_irq_cap;
    seL4_CPtr clock_timer_dev_ut_cap;
    uintptr_t clock_timer_paddr;

    /* Caps for the extra timer. */
    seL4_CPtr extra_timer_irq_cap;
    seL4_CPtr extra_timer_dev_ut_cap;
    uintptr_t extra_timer_paddr;

    /* cap to the sel4platsupport default timer io port */
    seL4_CPtr timer_io_port_cap;

    /* Paddr of the sel4platsupport default serial mmio region. */
    uintptr_t serial_paddr;
    /* cap to the sel4platsupport default serial irq handler */
    seL4_CPtr serial_irq_cap;
    /* cap to the sel4platsupport default serial physical frame */
    seL4_CPtr serial_frame_cap;
    /* cap to the sel4platsupport default serial I/O port; arch-specific. */
    seL4_CPtr serial_io_port_cap;

    /* size of the test processes cspace */
    seL4_Word cspace_size_bits;
    /* range of free slots in the cspace */
    seL4_SlotRegion free_slots;

    /* range of untyped memory in the cspace */
    seL4_SlotRegion untypeds;
    /* size of untyped that each untyped cap corresponds to
     * (size of the cap at untypeds.start is untyped_size_bits_lits[0]) */
    uint8_t untyped_size_bits_list[CONFIG_MAX_NUM_BOOTINFO_UNTYPED_CAPS];
    /* name of the test to run */
    char name[TEST_NAME_MAX];
    /* priority the test process is running at */
    int priority;

    /* List of elf regions in the test process image, this
     * is provided so the test process can launch copies of itself.
     *
     * Note: copies should not rely on state from the current process
     * or the image. Only use copies to run code functions, pass all
     * required state as arguments. */
    sel4utils_elf_region_t elf_regions[MAX_REGIONS];

    /* the number of elf regions */
    int num_elf_regions;

    /* the number of pages in the stack */
    int stack_pages;

    /* address of the stack */
    void *stack;

    /* freq of the tsc (for x86) */
    uint32_t tsc_freq;

    /* number of available cores */
    seL4_Word cores;
} test_init_data_t;

#endif
