/*
 * (C) 2021 The University of Chicago
 *
 * See COPYRIGHT in top-level directory.
 */
#include <stdio.h>
#include <sstream>
#include <margo.h>
#include <yokan/server.h>
#include <yokan/admin.h>
#include <nlohmann/json.hpp>
#include "available-backends.h"
#include "munit/munit.h"

using json = nlohmann::json;

struct test_context {
    margo_instance_id mid;
    hg_addr_t         addr;
    const char*       backend_type;
    const char*       backend_config;
};

static const uint16_t provider_id = 42;

static void* test_context_setup(const MunitParameter params[], void* user_data)
{
    (void) params;
    (void) user_data;
    margo_instance_id mid;
    hg_addr_t         addr;
    // create margo instance
    mid = margo_init("ofi+tcp", MARGO_SERVER_MODE, 0, 0);
    munit_assert_not_null(mid);
    // set log level
    margo_set_global_log_level(MARGO_LOG_CRITICAL);
    margo_set_log_level(mid, MARGO_LOG_CRITICAL);
    // get address of current process
    hg_return_t hret = margo_addr_self(mid, &addr);
    munit_assert_int(hret, ==, HG_SUCCESS);
    // create test context
    struct test_context* context = (struct test_context*)calloc(1, sizeof(*context));
    munit_assert_not_null(context);
    context->mid  = mid;
    context->addr = addr;
    context->backend_type = munit_parameters_get(params, "backend");
    context->backend_config = find_backend_config_for(context->backend_type);
    return context;
}

static void test_context_tear_down(void* fixture)
{
    struct test_context* context = (struct test_context*)fixture;
    // free address
    margo_addr_free(context->mid, context->addr);
    // we are not checking the return value of the above function with
    // munit because we need margo_finalize to be called no matter what.
    margo_finalize(context->mid);
}

static MunitResult test_provider_register(const MunitParameter params[], void* data)
{
    (void)params;
    (void)data;
    struct test_context* context = (struct test_context*)data;
    // register yk provider
    struct yk_provider_args args = YOKAN_PROVIDER_ARGS_INIT;
    yk_provider_t provider;

    yk_return_t ret = yk_provider_register(
            context->mid, provider_id, &args,
            &provider);
    munit_assert_int(ret, ==, YOKAN_SUCCESS);

    ret = yk_provider_destroy(provider);
    munit_assert_int(ret, ==, YOKAN_SUCCESS);

    return MUNIT_OK;
}


static MunitResult test_provider_register_multi(const MunitParameter params[], void* data)
{
    (void)params;
    (void)data;
    struct test_context* context = (struct test_context*)data;
    // register yk provider
    struct yk_provider_args args = YOKAN_PROVIDER_ARGS_INIT;
    yk_provider_t providerA, providerB, providerC;

    yk_return_t ret = yk_provider_register(
            context->mid, provider_id, &args,
            &providerA);
    munit_assert_int(ret, ==, YOKAN_SUCCESS);

    ret = yk_provider_register(
            context->mid, provider_id+1, &args,
            &providerB);
    munit_assert_int(ret, ==, YOKAN_SUCCESS);

    ret = yk_provider_register(
            context->mid, provider_id+1, &args,
            &providerC);
    munit_assert_int(ret, ==, YOKAN_ERR_INVALID_PROVIDER);

    return MUNIT_OK;
}

static MunitResult test_provider_config(const MunitParameter params[], void* data)
{
    (void)params;
    (void)data;
    struct test_context* context = (struct test_context*)data;
    yk_provider_t provider;

    std::string bad_config = "{ab434";
    std::stringstream ss;
    ss << "{\"databases\":["
       << "{\"type\":\"" << context->backend_type
       << "\",\"config\":" << context->backend_config
       << "}]}";
    std::string good_config = ss.str();

    struct yk_provider_args args = YOKAN_PROVIDER_ARGS_INIT;
    args.config = bad_config.c_str();

    yk_return_t ret = yk_provider_register(
            context->mid, provider_id, &args,
            &provider);
    munit_assert_int(ret, ==, YOKAN_ERR_INVALID_CONFIG);

    args.config = good_config.c_str();
    ret = yk_provider_register(
            context->mid, provider_id, &args,
            &provider);
    munit_assert_int(ret, ==, YOKAN_SUCCESS);

    char* config = yk_provider_get_config(provider);
    munit_assert_not_null(config);

    auto json_config = json::parse(config);
    free(config);
    munit_assert_true(json_config.contains("databases"));
    munit_assert_true(json_config["databases"].is_array());
    munit_assert_size(json_config["databases"].size(), ==, 1);
    auto& db_entry = json_config["databases"][0];
    munit_assert_true(db_entry.is_object());
    munit_assert_true(db_entry.contains("type"));
    munit_assert_string_equal(db_entry["type"].get<std::string>().c_str(), context->backend_type);
    munit_assert_true(db_entry.contains("__id__"));
    munit_assert_true(db_entry["__id__"].is_string());
    munit_assert_true(db_entry.contains("config"));
    munit_assert_true(db_entry["config"].is_object());

    ret = yk_provider_destroy(provider);
    munit_assert_int(ret, ==, YOKAN_SUCCESS);

    return MUNIT_OK;
}

static MunitParameterEnum test_params[] = {
  { (char*)"backend", (char**)available_backends },
  { NULL, NULL }
};

static MunitTest test_suite_tests[] = {
    { (char*) "/provider", test_provider_register,
        test_context_setup, test_context_tear_down, MUNIT_TEST_OPTION_NONE, test_params },
    { (char*) "/provider/multi", test_provider_register_multi,
        test_context_setup, test_context_tear_down, MUNIT_TEST_OPTION_NONE, test_params },
    { (char*) "/provider/config", test_provider_config,
        test_context_setup, test_context_tear_down, MUNIT_TEST_OPTION_NONE, test_params },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite test_suite = {
    (char*) "/yk/provider", test_suite_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int main(int argc, char* argv[MUNIT_ARRAY_PARAM(argc + 1)]) {
    return munit_suite_main(&test_suite, (void*) "yk", argc, argv);
}
