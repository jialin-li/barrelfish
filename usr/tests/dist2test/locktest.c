/**
 * \file
 * \brief start for libdist2 tests
 */

/*
 * Copyright (c) 2011, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include <barrelfish/barrelfish.h>
#include <barrelfish/deferred.h>

#include <skb/skb.h>
#include <dist2/dist2.h>

static struct periodic_event lock_timer;

static bool locked = false;

static void lockit(void* arg) {

	errval_t err = SYS_ERR_OK;
	if(!locked) {
		debug_printf("lock\n");
		err = dist_lock("lock_test");
		assert(err_is_ok(err));
		locked = true;
	}
	else {
		debug_printf("unlock\n");
		err = dist_unlock("lock_test");
		assert(err_is_ok(err));
		locked = false;
	}
}




int main(int argc, char *argv[])
{
    errval_t err = SYS_ERR_OK;
    skb_client_connect();
    dist_init();

    debug_printf("create periodic event...\n");
    err = periodic_event_create(&lock_timer, get_default_waitset(),
            (300 * 1000),
            MKCLOSURE(lockit, &lock_timer));
    assert(err_is_ok(err));

    messages_handler_loop();

	return EXIT_SUCCESS;
}
