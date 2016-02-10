/*
 * Copyright (C) 2013-2014 Argonne National Laboratory, Department of Energy,
 * UChicago Argonne, LLC and The HDF Group.
 * All rights reserved.
 *
 * The full copyright notice, including terms governing use, modification,
 * and redistribution, is contained in the COPYING file that can be
 * found at the root of the source code distribution tree.
 */

#include "mercury_test.h"

#include <stdio.h>
#include <stdlib.h>

extern hg_id_t hg_test_rpc_open_id_g;

#define COMPLETION_MAGIC 123456

static hg_return_t
hg_test_rpc_forward_cb(const struct hg_cb_info *callback_info)
{
    //hg_handle_t handle = callback_info->handle;
    int *ptr          = callback_info->arg;
    hg_request_t *request = (hg_request_t *) ptr;
    hg_return_t ret = HG_SUCCESS;

    ptr++;
    if (callback_info->ret != HG_CANCELLED)
    {
        fprintf(stderr, "Callback was not cancelled: %d\n",
                callback_info->ret);
        *ptr = 0;
    }
    else
    {
        *ptr = COMPLETION_MAGIC;
    }

    hg_request_complete(request);

    return ret;
}

/******************************************************************************/
int
main(int argc, char *argv[])
{
    hg_class_t *hg_class = NULL;
    hg_context_t *context = NULL;
    hg_request_class_t *request_class = NULL;
    hg_request_t *request = NULL;
    hg_handle_t handle;
    na_addr_t addr;
    rpc_open_in_t  rpc_open_in_struct;
    void *data[2];

    hg_const_string_t rpc_open_path = MERCURY_TESTING_TEMP_DIRECTORY "/test.h5";
    rpc_handle_t rpc_open_handle;
    hg_return_t hg_ret;

    /* Initialize the interface (for convenience, shipper_test_client_init
     * initializes the network interface with the selected plugin)
     */
    hg_class = HG_Test_client_init(argc, argv, &addr, NULL, &context,
            &request_class);

    request = hg_request_create(request_class);

    hg_ret = HG_Create(hg_class, context, addr, hg_test_rpc_open_id_g, &handle);
    if (hg_ret != HG_SUCCESS) {
        fprintf(stderr, "Could not start call\n");
        return EXIT_FAILURE;
    }

    /* Fill input structure */
    rpc_open_handle.cookie = 12345;
    rpc_open_in_struct.path = rpc_open_path;
    rpc_open_in_struct.handle = rpc_open_handle;

    data[0] = request;
    data[1] = 0;

    /* Forward call to remote addr and get a new request */
    printf("Forwarding rpc_open, op id: %u...\n", hg_test_rpc_open_id_g);
    hg_ret = HG_Forward(handle,
                        hg_test_rpc_forward_cb,
                        data,
                        &rpc_open_in_struct);
    if (hg_ret != HG_SUCCESS) {
        fprintf(stderr, "Could not forward call\n");
        return EXIT_FAILURE;
    }

    printf("Cancelling...\n");    
    hg_ret = HG_Cancel(handle);
    if (hg_ret != HG_SUCCESS)
    {
        fprintf(stderr, "HG_Cancel failed: %d\n", hg_ret);
        return EXIT_FAILURE;
    }
    // hg_request_wait(request, HG_MAX_IDLE_TIME, NULL);
    
    printf("HG_Destroy...\n");            
    /* Complete */
    hg_ret = HG_Destroy(handle);
    if (hg_ret != HG_SUCCESS) {
        fprintf(stderr, "Could not complete\n");
        return EXIT_FAILURE;
    }

    if (data[1] != (void*)COMPLETION_MAGIC)
    {
        fprintf(stderr, "callback wasn't called\n");
        return EXIT_FAILURE;
    }

    hg_request_destroy(request);

    HG_Test_finalize(hg_class);

    return EXIT_SUCCESS;
}
