/*
 * (C) 2021 The University of Chicago
 *
 * See COPYRIGHT in top-level directory.
 */
#ifndef __YOKAN_MODES_HPP
#define __YOKAN_MODES_HPP

#include "yokan/common.h"
#include "config.h"
#include <cstring>
#ifdef YOKAN_HAS_LUA
#include <sol/sol.hpp>
#endif

namespace yokan {

/**
 * This class uses the provided mode to validate a key
 * against a prefix. If the mode does not match
 * YOKAN_MODE_SUFFIX, it considers "prefix" as a prefix and
 * checks if the key starts with that prefix. Otherwise it
 * considers "prefix" as a suffix and checks if the key ends
 * with that suffix.
 */
struct Filter {

    int32_t     m_mode;
    const void* m_filter;
    size_t      m_fsize;
#ifdef YOKAN_HAS_LUA
    sol::state* m_lua = nullptr;
#endif


    Filter(int32_t mode, const void* filter, size_t fsize)
    : m_mode(mode)
    , m_filter(filter)
    , m_fsize(fsize) {
#ifdef YOKAN_HAS_LUA
        if(m_mode & YOKAN_MODE_LUA_FILTER) {
            m_lua = new sol::state{};
            m_lua->open_libraries(sol::lib::base);
            m_lua->open_libraries(sol::lib::string);
            m_lua->open_libraries(sol::lib::math);
        }
#endif
    }

    Filter(const Filter&) = delete;

    Filter(Filter&& other)
    : m_mode(other.m_mode)
    , m_filter(other.m_filter)
    , m_fsize(other.m_fsize)
#ifdef YOKAN_HAS_LUA
    , m_lua(other.m_lua)
#endif
    {
#ifdef YOKAN_HAS_LUA
        other.m_lua = nullptr;
#endif
    }

    ~Filter() {
#ifdef YOKAN_HAS_LUA
        if(m_lua) delete m_lua;
#endif
    }

    bool check(const void* key, size_t ksize, const void* val, size_t vsize) const {
#ifdef YOKAN_HAS_LUA
        if(m_mode & YOKAN_MODE_LUA_FILTER) {
            (*m_lua)["__key__"] = std::string_view(static_cast<const char*>(key), ksize);
            (*m_lua)["__value__"] = std::string_view(static_cast<const char*>(val), vsize);
            auto result = m_lua->do_string(std::string_view{ static_cast<const char*>(m_filter), m_fsize});
            if(!result.valid()) return false;
            return static_cast<bool>(result);
        }
#endif
        (void)val;
        (void)vsize;
        if(m_fsize > ksize)
            return false;
        if(!(m_mode & YOKAN_MODE_SUFFIX)) {
            return std::memcmp(key, m_filter, m_fsize) == 0;
        } else {
            return std::memcmp(((const char*)key)+ksize-m_fsize, m_filter, m_fsize) == 0;
        }
    }
};

/**
 * This function will copy a key into a buffer according to the mode
 * provided, i.e. eliminating prefix or suffix or ignoring the copy
 * as needed.
 */
static inline size_t keyCopy(int32_t mode,
        void* dst, size_t max_dst_size,
        const void* key, size_t ksize,
        size_t filter_size,
        bool is_last = false) {
    if(mode & YOKAN_MODE_IGNORE_KEYS) {
        if(!(is_last && (mode & YOKAN_MODE_KEEP_LAST)))
            return 0;
    }
    if(!(mode & YOKAN_MODE_NO_PREFIX)) { // don't eliminate prefix/suffix
        if(max_dst_size < ksize) return YOKAN_SIZE_TOO_SMALL;
        std::memcpy(dst, key, ksize);
        return ksize;
    } else { // eliminate prefix/suffix
        auto final_ksize = ksize - filter_size;
        if(max_dst_size < final_ksize)
            return YOKAN_SIZE_TOO_SMALL;
        if(mode & YOKAN_MODE_SUFFIX) { // eliminate suffix
            std::memcpy(dst, (const char*)key, final_ksize);
        } else { // eliminate prefix
            std::memcpy(dst, (const char*)key + filter_size, final_ksize);
        }
        return final_ksize;
    }
}

/**
 * @brief This function is provided for symmetry with keyCopy,
 * and in case we every need value copies to depend on a mode.
 */
static inline size_t valCopy(int32_t mode,
        void* dst, size_t max_dst_size,
        const void* val, size_t vsize) {
    (void)mode;
    if(max_dst_size < vsize) return YOKAN_SIZE_TOO_SMALL;
    std::memcpy(dst, val, vsize);
    return vsize;
}

}
#endif
