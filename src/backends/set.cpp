/*
 * (C) 2021 The University of Chicago
 *
 * See COPYRIGHT in top-level directory.
 */
#include "rkv/rkv-backend.hpp"
#include "../common/linker.hpp"
#include "../common/allocator.hpp"
#include "../common/locks.hpp"
#include "../common/modes.hpp"
#include <nlohmann/json.hpp>
#include <abt.h>
#include <set>
#include <string>
#include <cstring>
#include <iostream>

namespace rkv {

using json = nlohmann::json;

template<typename KeyType>
struct SetKeyValueStoreCompare {

    // LCOV_EXCL_START
    static bool DefaultMemCmp(const void* lhs, size_t lhsize,
                              const void* rhs, size_t rhsize) {
        auto r = std::memcmp(lhs, rhs, std::min(lhsize, rhsize));
        if(r < 0) return true;
        if(r > 0) return false;
        if(lhsize < rhsize)
            return true;
        return false;
    }
    // LCOV_EXCL_STOP

    using cmp_type = bool (*)(const void*, size_t, const void*, size_t);

    cmp_type cmp = &DefaultMemCmp;

    SetKeyValueStoreCompare() = default;

    SetKeyValueStoreCompare(cmp_type comparator)
    : cmp(comparator) {}

    bool operator()(const KeyType& lhs, const KeyType& rhs) const {
        return cmp(lhs.data(), lhs.size(), rhs.data(), rhs.size());
    }

    bool operator()(const KeyType& lhs, const UserMem& rhs) const {
        return cmp(lhs.data(), lhs.size(), rhs.data, rhs.size);
    }

    bool operator()(const UserMem& lhs, const KeyType& rhs) const {
        return cmp(lhs.data, lhs.size, rhs.data(), rhs.size());
    }

    bool operator()(const UserMem& lhs, const UserMem& rhs) const {
        return cmp(lhs.data, lhs.size, rhs.data, rhs.size);
    }

    using is_transparent = int;
};

class SetKeyValueStore : public KeyValueStoreInterface {

    public:

    static Status create(const std::string& config, KeyValueStoreInterface** kvs) {
        json cfg;
        cmp_type cmp = comparator::DefaultMemCmp;
        rkv_allocator_init_fn key_alloc_init, node_alloc_init;
        rkv_allocator_t key_alloc, node_alloc;
        std::string key_alloc_conf;

        try {
            cfg = json::parse(config);
            if(!cfg.is_object())
                return Status::InvalidConf;
            // check use_lock
            bool use_lock = cfg.value("use_lock", true);
            cfg["use_lock"] = use_lock;
            // check comparator field
            if(!cfg.contains("comparator"))
                cfg["comparator"] = "default";
            auto comparator = cfg.value("comparator", "default");
            if(comparator != "default")
                cmp = Linker::load<cmp_type>(comparator);
            if(cmp == nullptr)
                return Status::InvalidConf;
            // check allocators
            if(!cfg.contains("allocators")) {
                cfg["allocators"]["key_allocator"] = "default";
                cfg["allocators"]["node_allocator"] = "default";
            } else if(!cfg["allocators"].is_object()) {
                return Status::InvalidConf;
            }

            auto& alloc_cfg = cfg["allocators"];

            // key allocator
            auto key_allocator_name = alloc_cfg.value("key_allocator", "default");
            auto key_allocator_config = alloc_cfg.value("key_allocator_config", json::object());
            alloc_cfg["key_allocator"] = key_allocator_name;
            alloc_cfg["key_allocator_config"] = key_allocator_config;
            if(key_allocator_name == "default")
                key_alloc_init = default_allocator_init;
            else
                key_alloc_init = Linker::load<decltype(key_alloc_init)>(key_allocator_name);
            if(key_alloc_init == nullptr) return Status::InvalidConf;
            key_alloc_init(&key_alloc, key_allocator_config.dump().c_str());

            // node allocator
            auto node_allocator_name = alloc_cfg.value("node_allocator", "default");
            auto node_allocator_config = alloc_cfg.value("node_allocator_config", json::object());
            alloc_cfg["node_allocator"] = node_allocator_name;
            alloc_cfg["node_allocator_config"] = node_allocator_config;
            if(node_allocator_name == "default")
                node_alloc_init = default_allocator_init;
            else
                node_alloc_init = Linker::load<decltype(node_alloc_init)>(node_allocator_name);
            if(node_alloc_init == nullptr) return Status::InvalidConf;
            node_alloc_init(&node_alloc, node_allocator_config.dump().c_str());

        } catch(...) {
            return Status::InvalidConf;
        }
        *kvs = new SetKeyValueStore(cfg, cmp, node_alloc, key_alloc);
        return Status::OK;
    }

    // LCOV_EXCL_START
    virtual std::string name() const override {
        return "set";
    }
    // LCOV_EXCL_STOP

    // LCOV_EXCL_START
    virtual std::string config() const override {
        return m_config.dump();
    }
    // LCOV_EXCL_STOP

    virtual bool supportsMode(int32_t mode) const override {
        // note: technically RKV_MODE_APPEND, NEW_ONLY, and EXIST_ONLY
        // are supported but they are useless for a backend that doesn't
        // store any value!
        return mode ==
            (mode & (
                     RKV_MODE_INCLUSIVE
                    |RKV_MODE_APPEND
        //            |RKV_MODE_CONSUME
        //            |RKV_MODE_WAIT
                    |RKV_MODE_NEW_ONLY
                    |RKV_MODE_EXIST_ONLY
                    |RKV_MODE_NO_PREFIX
                    |RKV_MODE_IGNORE_KEYS
                    |RKV_MODE_KEEP_LAST
                    |RKV_MODE_SUFFIX
                    )
            );
    }

    virtual void destroy() override {
        ScopedWriteLock lock(m_lock);
        m_db->clear();
    }

    virtual Status exists(int32_t mode, const UserMem& keys,
                          const BasicUserMem<size_t>& ksizes,
                          BitField& flags) const override {
        (void)mode;
        if(ksizes.size > flags.size) return Status::InvalidArg;
        size_t offset = 0;
        ScopedReadLock lock(m_lock);
        for(size_t i = 0; i < ksizes.size; i++) {
            const UserMem key{ keys.data + offset, ksizes[i] };
            if(offset + ksizes[i] > keys.size) return Status::InvalidArg;
            flags[i] = m_db->count(key) > 0;
            offset += ksizes[i];
        }
        return Status::OK;
    }

    virtual Status length(int32_t mode, const UserMem& keys,
                          const BasicUserMem<size_t>& ksizes,
                          BasicUserMem<size_t>& vsizes) const override {
        (void)mode;
        if(ksizes.size != vsizes.size) return Status::InvalidArg;
        size_t offset = 0;
        ScopedReadLock lock(m_lock);
        for(size_t i = 0; i < ksizes.size; i++) {
            const UserMem key{ keys.data + offset, ksizes[i] };
            if(offset + ksizes[i] > keys.size) return Status::InvalidArg;
            auto it = m_db->find(key);
            if(it == m_db->end()) vsizes[i] = KeyNotFound;
            else vsizes[i] = 0;
            offset += ksizes[i];
        }
        return Status::OK;
    }

    virtual Status put(int32_t mode,
                       const UserMem& keys,
                       const BasicUserMem<size_t>& ksizes,
                       const UserMem& vals,
                       const BasicUserMem<size_t>& vsizes) override {
        (void)mode;
        if(ksizes.size != vsizes.size) return Status::InvalidArg;
        if(vals.size != 0) return Status::InvalidArg;

        size_t key_offset = 0;

        size_t total_ksizes = std::accumulate(ksizes.data,
                                              ksizes.data + ksizes.size,
                                              0);
        if(total_ksizes > keys.size) return Status::InvalidArg;

        size_t total_vsizes = std::accumulate(vsizes.data,
                                              vsizes.data + vsizes.size,
                                              0);
        if(total_vsizes != 0) return Status::InvalidArg;

        ScopedWriteLock lock(m_lock);
        for(size_t i = 0; i < ksizes.size; i++) {
            m_db->emplace(keys.data + key_offset,
                          ksizes[i], m_key_allocator);
            key_offset += ksizes[i];
        }
        return Status::OK;
    }

    virtual Status get(int32_t mode,
                       bool packed, const UserMem& keys,
                       const BasicUserMem<size_t>& ksizes,
                       UserMem& vals,
                       BasicUserMem<size_t>& vsizes) override {
        (void)mode;
        if(ksizes.size != vsizes.size) return Status::InvalidArg;
        (void)packed;
        size_t key_offset = 0;
        ScopedReadLock lock(m_lock);

        for(size_t i = 0; i < ksizes.size; i++) {
            const UserMem key{ keys.data + key_offset, ksizes[i] };
            auto it = m_db->find(key);
            if(it == m_db->end()) {
                vsizes[i] = KeyNotFound;
            } else {
                vsizes[i] = 0;
            }
            key_offset += ksizes[i];
        }
        vals.size = 0;

        return Status::OK;
    }

    virtual Status erase(int32_t mode, const UserMem& keys,
                         const BasicUserMem<size_t>& ksizes) override {
        (void)mode;
        size_t offset = 0;
        ScopedReadLock lock(m_lock);
        for(size_t i = 0; i < ksizes.size; i++) {
            auto key = UserMem{ keys.data + offset, ksizes[i] };
            if(offset + ksizes[i] > keys.size) return Status::InvalidArg;
            auto it = m_db->find(key);
            if(it != m_db->end()) {
                m_db->erase(it);
            }
            offset += ksizes[i];
        }
        return Status::OK;
    }

    virtual Status listKeys(int32_t mode, bool packed, const UserMem& fromKey,
                            const UserMem& prefix,
                            UserMem& keys, BasicUserMem<size_t>& keySizes) const override {
        (void)mode;
        ScopedReadLock lock(m_lock);

        auto inclusive = mode & RKV_MODE_INCLUSIVE;

        using iterator = decltype(m_db->begin());
        iterator fromKeyIt;
        if(fromKey.size == 0) {
            fromKeyIt = m_db->begin();
        } else {
            fromKeyIt = inclusive ? m_db->lower_bound(fromKey) : m_db->upper_bound(fromKey);
        }

        const auto end = m_db->end();
        auto max = keySizes.size;
        size_t i = 0;
        size_t offset = 0;
        bool buf_too_small = false;

        for(auto it = fromKeyIt; it != end && i < max; ++it) {
            auto& key = *it;
            if(prefix.size != 0) {
                if(!checkPrefix(mode, key.data(), key.size(),
                            prefix.data, prefix.size))
                    continue;
            }
            auto umem = static_cast<char*>(keys.data) + offset;

            bool is_last = false;
            if(mode & RKV_MODE_KEEP_LAST) {
                auto next = it;
                ++next;
                is_last = (i+1 == max) || (next == end);
            }

            if(!packed) {

                size_t usize = keySizes[i];
                keySizes[i] = keyCopy(mode, umem, usize,
                                      key.data(), key.size(),
                                      prefix.size, is_last);
                offset += usize;

            } else { // if packed

                if(buf_too_small)
                    keySizes[i] = RKV_SIZE_TOO_SMALL;
                else {
                    keySizes[i] = keyCopy(mode, umem, keys.size - offset,
                                          key.data(), key.size(), prefix.size,
                                          is_last);
                    if(keySizes[i] == RKV_SIZE_TOO_SMALL)
                        buf_too_small = true;
                    else
                        offset += keySizes[i];
                }

            }
            i += 1;
        }

        keys.size = offset;
        for(; i < max; i++) {
            keySizes[i] = RKV_NO_MORE_KEYS;
        }

        return Status::OK;
    }

    virtual Status listKeyValues(int32_t mode,
                                 bool packed,
                                 const UserMem& fromKey,
                                 const UserMem& prefix,
                                 UserMem& keys,
                                 BasicUserMem<size_t>& keySizes,
                                 UserMem& vals,
                                 BasicUserMem<size_t>& valSizes) const override {
        ScopedReadLock lock(m_lock);

        auto inclusive = mode & RKV_MODE_INCLUSIVE;

        using iterator = decltype(m_db->begin());
        iterator fromKeyIt;
        if(fromKey.size == 0) {
            fromKeyIt = m_db->begin();
        } else {
            fromKeyIt = inclusive ? m_db->lower_bound(fromKey) : m_db->upper_bound(fromKey);
        }
        const auto end = m_db->end();
        auto max = keySizes.size;
        size_t i = 0;
        size_t key_offset = 0;
        bool key_buf_too_small = false;

        for(auto it = fromKeyIt; it != end && i < max; it++) {
            auto& key = *it;
            if(prefix.size != 0) {
                if(!checkPrefix(mode, key.data(), key.size(),
                            prefix.data, prefix.size))
                    continue;
            }
            auto key_umem = static_cast<char*>(keys.data) + key_offset;

            bool is_last = false;
            if(mode & RKV_MODE_KEEP_LAST) {
                auto next = it;
                ++next;
                is_last = (i+1 == max) || (next == end);
            }

            if(!packed) {

                size_t key_usize = keySizes[i];
                keySizes[i] = keyCopy(mode, key_umem, key_usize,
                                      key.data(), key.size(),
                                      prefix.size, is_last);
                key_offset += key_usize;

            } else { // not packed

                if(key_buf_too_small)
                    keySizes[i] = RKV_SIZE_TOO_SMALL;
                else {
                    keySizes[i] = keyCopy(mode, key_umem, keys.size - key_offset,
                                          key.data(), key.size(),
                                          prefix.size, is_last);
                    if(keySizes[i] == RKV_SIZE_TOO_SMALL)
                        key_buf_too_small = true;
                    else
                        key_offset += key.size();
                }

            }
            valSizes[i] = 0;
            i += 1;
        }
        keys.size = key_offset;
        vals.size = 0;
        for(; i < max; i++) {
            keySizes[i] = RKV_NO_MORE_KEYS;
            valSizes[i] = RKV_NO_MORE_KEYS;
        }

        return Status::OK;
    }

    ~SetKeyValueStore() {
        if(m_lock != ABT_RWLOCK_NULL)
            ABT_rwlock_free(&m_lock);
        delete m_db;
        m_key_allocator.finalize(m_key_allocator.context);
        m_node_allocator.finalize(m_node_allocator.context);
    }

    private:

    using key_type = std::basic_string<char, std::char_traits<char>,
                                       Allocator<char>>;
    using comparator = SetKeyValueStoreCompare<key_type>;
    using cmp_type = comparator::cmp_type;
    using allocator = Allocator<key_type>;
    using set_type = std::set<key_type, comparator, allocator>;

    SetKeyValueStore(json cfg,
                     cmp_type cmp_fun,
                     const rkv_allocator_t& node_allocator,
                     const rkv_allocator_t& key_allocator)
    : m_config(std::move(cfg))
    , m_node_allocator(node_allocator)
    , m_key_allocator(key_allocator)
    {
        if(m_config["use_lock"].get<bool>())
            ABT_rwlock_create(&m_lock);
        m_db = new set_type(cmp_fun, allocator(m_node_allocator));
    }

    set_type*       m_db;
    json            m_config;
    ABT_rwlock      m_lock = ABT_RWLOCK_NULL;
    rkv_allocator_t m_node_allocator;
    rkv_allocator_t m_key_allocator;
};

}

RKV_REGISTER_BACKEND(set, rkv::SetKeyValueStore);
