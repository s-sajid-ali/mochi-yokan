/*
 * (C) 2021 The University of Chicago
 *
 * See COPYRIGHT in top-level directory.
 */
#ifndef __RKV_COMMON_H
#define __RKV_COMMON_H

#include <uuid.h>
#include <stdint.h>
#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Error codes that can be returned by RKV functions.
 */
#define RKV_RETURN_VALUES                              \
    X(RKV_SUCCESS, "Success")                          \
    X(RKV_ERR_ALLOCATION, "Allocation error")          \
    X(RKV_ERR_INVALID_MID, "Invalid margo instance")   \
    X(RKV_ERR_INVALID_ARGS, "Invalid argument")        \
    X(RKV_ERR_INVALID_PROVIDER, "Invalid provider id") \
    X(RKV_ERR_INVALID_DATABASE, "Invalid database id") \
    X(RKV_ERR_INVALID_BACKEND, "Invalid backend type") \
    X(RKV_ERR_INVALID_CONFIG, "Invalid configuration") \
    X(RKV_ERR_INVALID_TOKEN, "Invalid token")          \
    X(RKV_ERR_FROM_MERCURY, "Mercury error")           \
    X(RKV_ERR_FROM_ARGOBOTS, "Argobots error")         \
    X(RKV_ERR_OP_UNSUPPORTED, "Unsupported operation") \
    X(RKV_ERR_OP_FORBIDDEN, "Forbidden operation")     \
    X(RKV_ERR_KEY_NOT_FOUND, "Key not found")          \
    X(RKV_ERR_BUFFER_SIZE, "Buffer too small")         \
    X(RKV_ERR_KEY_EXISTS, "Key exists")                \
    X(RKV_ERR_CORRUPTION, "Data corruption")           \
    X(RKV_ERR_IO, "IO error")                          \
    X(RKV_ERR_INCOMPLETE, "Imcomplete operation")      \
    X(RKV_ERR_TIMEOUT, "Timeout")                      \
    X(RKV_ERR_ABORTED, "Operation aborted")            \
    X(RKV_ERR_BUSY, "Busy")                            \
    X(RKV_ERR_EXPIRED, "Operation expired")            \
    X(RKV_ERR_TRY_AGAIN, "Try again")                  \
    X(RKV_ERR_OTHER, "Other error")

#define X(__err__, __msg__) __err__,
typedef enum rkv_return_t {
    RKV_RETURN_VALUES
} rkv_return_t;
#undef X

/**
 * @brief These two constants are used in returned value sizes
 * to indicate respectively that the buffer was too small to hold
 * the value, and that the key was not found.
 */
#define RKV_KEY_NOT_FOUND  (ULLONG_MAX)
#define RKV_SIZE_TOO_SMALL (ULLONG_MAX-1)
#define RKV_NO_MORE_KEYS   (ULLONG_MAX-2)

/**
 * @brief Identifier for a database.
 */
typedef struct rkv_database_id_t {
    uuid_t uuid;
} rkv_database_id_t;

/**
 * @brief Converts a rkv_database_id_t into a string.
 *
 * @param id Id to convert
 * @param out[37] Resulting null-terminated string
 */
static inline void rkv_database_id_to_string(
        rkv_database_id_t id,
        char out[37]) {
    uuid_unparse(id.uuid, out);
}

/**
 * @brief Converts a string into a rkv_database_id_t. The string
 * should be a 36-characters string + null terminator.
 *
 * @param in input string
 * @param id resulting id
 */
static inline void rkv_database_id_from_string(
        const char* in,
        rkv_database_id_t* id) {
    uuid_parse(in, id->uuid);
}

#ifdef __cplusplus
}
#endif

#endif
