/**
 * \file
 * \brief init process for child spawning
 */

/*
 * Copyright (c) 2007, 2008, 2009, 2010, 2016, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <aos/aos.h>
#include <aos/waitset.h>
#include <aos/morecore.h>
#include <aos/paging.h>
#include <mm/mm.h>
#include <spawn/spawn.h>

#include "coreboot.h"
#include "mem_alloc.h"
#include "rpc_server.h"

coreid_t my_core_id;
struct bootinfo *bi;

int main(int argc, char *argv[])
{
    errval_t err;

    /* Set the core id in the disp_priv struct */
    err = invoke_kernel_get_core_id(cap_kernel, &my_core_id);
    assert(err_is_ok(err));
    disp_set_core_id(my_core_id);
    
    debug_printf("init: on core %" PRIuCOREID " invoked as:", my_core_id);
    for (int i = 0; i < argc; i++) {
       printf(" %s", argv[i]);
    }
    printf("\n");

    /* First argument contains the bootinfo location, if it's not set */
    bi = (struct bootinfo*)strtol(argv[1], NULL, 10);
    if (!bi) {
        assert(my_core_id > 0);
    }

    /* Initialize paging. */
    // err = paging_init();
    // if (err_is_fail(err)) {
    //     DEBUG_ERR(err, "paging_init");
    // }

    /* Initialize the default slot allocator. */
    // err = slot_alloc_init();
    // if (err_is_fail(err)) {
    //     DEBUG_ERR(err, "slot_alloc_init");
    // }

    genpaddr_t remaining_mem_base = 0;
    gensize_t remaining_mem_size = 0;
    if (my_core_id == 0) {
        err = initialize_ram_alloc(&remaining_mem_base, &remaining_mem_size);
        if(err_is_fail(err)){
            DEBUG_ERR(err, "initialize_ram_alloc");
        }
    }

    void* urpc_buf;
    CHECK("mapping URPC frame into vspace",
            map_urpc_frame_to_vspace(&urpc_buf, 2u * BASE_PAGE_SIZE, my_core_id));

    // Divide buffer into two half: one part for core 0 and other for core 1
    // Each part is BASE_PAGE_SIZE long

    // Initialize first half of the buffer
    void *first_half = urpc_buf;
    *((char*) first_half) = 'C';
    first_half += 2*sizeof(char);
    *((char*) first_half) = 'C';

    // Intialize the second half of the buffer
    void *second_half = (urpc_buf+BASE_PAGE_SIZE);
    *((char*) second_half) = 'C';
    second_half += 2*sizeof(char);
    *((char*) second_half) = 'C';

    write_to_urpc(urpc_buf, remaining_mem_base, remaining_mem_size, bi,
            my_core_id);
    CHECK("forging RAM cap & retrieving bi from URPC frame",
            read_from_urpc(urpc_buf, &bi, my_core_id));
    CHECK("start core 1", start_core(1, my_core_id, bi));

    if (my_core_id == 1) {
        err = initialize_ram_alloc(&remaining_mem_base, &remaining_mem_size);
        if(err_is_fail(err)){
            DEBUG_ERR(err, "initialize_ram_alloc");
        }
        
        CHECK("reading modules from URPC",
                read_modules(urpc_buf, bi, my_core_id));
    }

    CHECK("Retype selfep from dispatcher", cap_retype(cap_selfep, cap_dispatcher, 0, ObjType_EndPoint, 0, 1));

    struct lmp_chan* lc = (struct lmp_chan*) malloc(sizeof(struct lmp_chan));
    CHECK("Create channel for parent", lmp_chan_accept(lc, DEFAULT_LMP_BUF_WORDS, NULL_CAP));

    CHECK("Create Slot", lmp_chan_alloc_recv_slot(lc));
    CHECK("COpy to initep", cap_copy(cap_initep, lc->local_cap));

    CHECK("lmp_chan_register_recv child",
            lmp_chan_register_recv(lc, get_default_waitset(),
                    MKCLOSURE((void*) local_recv_handler, lc)));

    if (my_core_id == 0) {
        // Spawn "Hello" on core 0.
        CHECK("spawning hello",
                spawn_load_by_name("hello",
                        (struct spawninfo*) malloc(sizeof(struct spawninfo)), my_core_id));
    } else {
        // Spawn "Byebye" on core 1.
        CHECK("spawning byebye",
                spawn_load_by_name("byebye",
                        (struct spawninfo*) malloc(sizeof(struct spawninfo)), my_core_id));
    }

    debug_printf("Message handler loop\n");
    // Hang around
    struct waitset *default_ws = get_default_waitset();
    while (true) {
        err = event_dispatch(default_ws);
        if (err_is_fail(err)) {
            DEBUG_ERR(err, "in event_dispatch");
            abort();
        }
    }

    return EXIT_SUCCESS;
}
