/*
 * (C) 2021 The University of Chicago
 *
 * See COPYRIGHT in top-level directory.
 */
#include "yokan/server.h"
#include "provider.hpp"
#include "../common/types.h"
#include "../common/defer.hpp"
#include "../common/logging.h"
#include "../common/checks.h"
#include <numeric>
#include <cmath>

void yk_exists_ult(hg_handle_t h)
{
    hg_return_t hret;
    exists_in_t in;
    exists_out_t out;
    hg_addr_t origin_addr = HG_ADDR_NULL;

    out.ret = YOKAN_SUCCESS;

    DEFER(margo_destroy(h));
    DEFER(margo_respond(h, &out));

    margo_instance_id mid = margo_hg_handle_get_instance(h);
    CHECK_MID(mid, margo_hg_handle_get_instance);

    const struct hg_info* info = margo_get_info(h);
    yk_provider_t provider = (yk_provider_t)margo_registered_data(mid, info->id);
    CHECK_PROVIDER(provider);

    hret = margo_get_input(h, &in);
    CHECK_HRET_OUT(hret, margo_get_input);
    DEFER(margo_free_input(h, &in));

    if(in.origin) {
        hret = margo_addr_lookup(mid, in.origin, &origin_addr);
        CHECK_HRET_OUT(hret, margo_addr_lookup);
    } else {
        hret = margo_addr_dup(mid, info->addr, &origin_addr);
        CHECK_HRET_OUT(hret, margo_addr_dup);
    }
    DEFER(margo_addr_free(mid, origin_addr));

    yk_database* database = find_database(provider, &in.db_id);
    CHECK_DATABASE(database, in.db_id);
    CHECK_MODE_SUPPORTED(database, in.mode);

    yk_buffer_t buffer = provider->bulk_cache.get(
            provider->bulk_cache_data, in.size, HG_BULK_READWRITE);
    CHECK_BUFFER(buffer);
    DEFER(provider->bulk_cache.release(
            provider->bulk_cache_data, buffer));

    const size_t keys_offset = in.count * sizeof(size_t);
    size_t flags_offset      = 0; // computed later

    // transfer ksizes
    size_t sizes_to_transfer = in.count*sizeof(size_t);

    hret = margo_bulk_transfer(mid, HG_BULK_PULL, origin_addr,
            in.bulk, in.offset, buffer->bulk, 0, sizes_to_transfer);
    CHECK_HRET_OUT(hret, margo_bulk_transfer);

    // build buffer wrappers for key sizes
    auto ptr = buffer->data;
    auto ksizes = yokan::BasicUserMem<size_t>{
        reinterpret_cast<size_t*>(ptr),
        in.count
    };

    auto total_ksize = std::accumulate(ksizes.data, ksizes.data + in.count, (size_t)0);
    flags_offset = keys_offset + total_ksize;

    // check that there is no key of size 0
    auto min_key_size = std::accumulate(ksizes.data, ksizes.data + in.count,
                                        std::numeric_limits<size_t>::max(),
                                        [](const size_t& lhs, const size_t& rhs) {
                                            return std::min(lhs, rhs);
                                        });
    if(min_key_size == 0) {
        out.ret = YOKAN_ERR_INVALID_ARGS;
        return;
    }

    // check that the sizes found is consistent with in.size
    size_t flags_size = std::ceil(((double)in.count)/8.0);
    if(in.size < flags_offset + flags_size) {
        out.ret = YOKAN_ERR_INVALID_ARGS;
        return;
    }

    // transfer the actual keys from the client
    hret = margo_bulk_transfer(mid, HG_BULK_PULL, origin_addr,
            in.bulk, in.offset + keys_offset,
            buffer->bulk, keys_offset, total_ksize);
    CHECK_HRET_OUT(hret, margo_bulk_transfer);

    // create memory wrapper for keys
    auto keys = yokan::UserMem{ ptr + keys_offset, total_ksize };

    // create memory wrapper for flags
    auto flags = yokan::BitField{
        (uint8_t*)ptr + flags_offset,
        in.count
    };
    std::memset(flags.data, 0, flags_size);

    out.ret = static_cast<yk_return_t>(
            database->exists(in.mode, keys, ksizes, flags));

    if(out.ret == YOKAN_SUCCESS) {
        // transfer the bit field back the client
        hret = margo_bulk_transfer(mid, HG_BULK_PUSH, origin_addr,
                in.bulk, in.offset + flags_offset,
                buffer->bulk, flags_offset, flags_size);
        CHECK_HRET_OUT(hret, margo_bulk_transfer);
    }
}
DEFINE_MARGO_RPC_HANDLER(yk_exists_ult)

void yk_exists_direct_ult(hg_handle_t h)
{
    hg_return_t hret;
    exists_direct_in_t in;
    exists_direct_out_t out;

    std::vector<char> flags_data;

    in.keys.data = nullptr;
    in.keys.size = 0;
    in.sizes.sizes = nullptr;
    in.sizes.count = 0;

    out.ret = YOKAN_SUCCESS;
    out.flags.data = nullptr;
    out.flags.size = 0;

    DEFER(margo_destroy(h));
    DEFER(margo_respond(h, &out));

    margo_instance_id mid = margo_hg_handle_get_instance(h);
    CHECK_MID(mid, margo_hg_handle_get_instance);

    const struct hg_info* info = margo_get_info(h);
    yk_provider_t provider = (yk_provider_t)margo_registered_data(mid, info->id);
    CHECK_PROVIDER(provider);

    hret = margo_get_input(h, &in);
    CHECK_HRET_OUT(hret, margo_get_input);
    DEFER(margo_free_input(h, &in));

    yk_database* database = find_database(provider, &in.db_id);
    CHECK_DATABASE(database, in.db_id);
    CHECK_MODE_SUPPORTED(database, in.mode);

    auto count = in.sizes.count;
    auto ksizes = yokan::BasicUserMem<size_t>{ in.sizes.sizes, count };

    // check that there is no key of size 0
    auto min_key_size = std::accumulate(ksizes.data, ksizes.data + count,
                                        std::numeric_limits<size_t>::max(),
                                        [](const size_t& lhs, const size_t& rhs) {
                                            return std::min(lhs, rhs);
                                        });
    if(min_key_size == 0) {
        out.ret = YOKAN_ERR_INVALID_ARGS;
        return;
    }

    // create memory wrapper for flags
    size_t flags_size = std::ceil(((double)count)/8.0);
    flags_data.resize(flags_size);
    auto flags = yokan::BitField{ (uint8_t*)flags_data.data(), count };
    std::memset(flags.data, 0, flags_data.size());

    out.flags.data = flags_data.data();
    out.flags.size = flags_data.size();

    // create memory wrapper for keys
    auto keys = yokan::UserMem{ in.keys.data, in.keys.size };

    out.ret = static_cast<yk_return_t>(
            database->exists(in.mode, keys, ksizes, flags));
}
DEFINE_MARGO_RPC_HANDLER(yk_exists_direct_ult)
