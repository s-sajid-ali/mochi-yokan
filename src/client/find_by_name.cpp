/*
 * (C) 2021 The University of Chicago
 *
 * See COPYRIGHT in top-level directory.
 */
#include <vector>
#include <array>
#include <numeric>
#include <cstring>
#include <cmath>
#include "client.h"
#include "../common/defer.hpp"
#include "../common/types.h"
#include "../common/logging.h"
#include "../common/checks.h"

extern "C" yk_return_t yk_database_find_by_name(
        yk_client_t client,
        hg_addr_t addr,
        uint16_t provider_id,
        const char* db_name,
        yk_database_id_t* database_id)

{
    margo_instance_id mid = client->mid;
    yk_return_t ret = YOKAN_SUCCESS;
    hg_return_t hret = HG_SUCCESS;
    find_by_name_in_t in;
    find_by_name_out_t out;
    hg_handle_t handle = HG_HANDLE_NULL;

    in.db_name = (char*)db_name;

    hret = margo_create(mid, addr, client->find_by_name_id, &handle);
    CHECK_HRET(hret, margo_create);
    DEFER(margo_destroy(handle));

    hret = margo_provider_forward(provider_id, handle, &in);
    CHECK_HRET(hret, margo_provider_forward);

    hret = margo_get_output(handle, &out);
    CHECK_HRET(hret, margo_get_output);

    ret = static_cast<yk_return_t>(out.ret);
    if(ret == YOKAN_SUCCESS)
        *database_id = out.db_id;

    hret = margo_free_output(handle, &out);
    CHECK_HRET(hret, margo_free_output);

    return ret;
}
