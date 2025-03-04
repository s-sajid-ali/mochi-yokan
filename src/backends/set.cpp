/*
 * (C) 2021 The University of Chicago
 *
 * See COPYRIGHT in top-level directory.
 */
#include "yokan/backend.hpp"
#include "yokan/watcher.hpp"
#include "yokan/util/locks.hpp"
#include "../common/linker.hpp"
#include "../common/allocator.hpp"
#include "../common/modes.hpp"
#include "util/key-copy.hpp"
#include <nlohmann/json.hpp>
#include <abt.h>
#include <set>
#include <string>
#include <cstring>
#include <iostream>

namespace yokan {

using json = nlohmann::json;

template<typename KeyType>
struct SetDatabaseCompare {

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

    SetDatabaseCompare() = default;

    SetDatabaseCompare(cmp_type comparator)
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

class SetDatabase : public DatabaseInterface {

    public:

    static Status create(const std::string& config, DatabaseInterface** kvs) {
        json cfg;
        cmp_type cmp = comparator::DefaultMemCmp;
        yk_allocator_init_fn key_alloc_init, node_alloc_init;
        yk_allocator_t key_alloc, node_alloc;
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
        *kvs = new SetDatabase(cfg, cmp, node_alloc, key_alloc);
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
        // note: technically YOKAN_MODE_APPEND, NEW_ONLY, and EXIST_ONLY
        // are supported but they are useless for a backend that doesn't
        // store any value!
        return mode ==
            (mode & (
                     YOKAN_MODE_INCLUSIVE
                    |YOKAN_MODE_APPEND
                    |YOKAN_MODE_CONSUME
                    |YOKAN_MODE_WAIT
                    |YOKAN_MODE_NOTIFY
                    |YOKAN_MODE_NEW_ONLY
                    |YOKAN_MODE_EXIST_ONLY
                    |YOKAN_MODE_NO_PREFIX
                    |YOKAN_MODE_IGNORE_KEYS
                    |YOKAN_MODE_KEEP_LAST
                    |YOKAN_MODE_SUFFIX
#ifdef YOKAN_HAS_LUA
                    |YOKAN_MODE_LUA_FILTER
#endif
                    |YOKAN_MODE_IGNORE_DOCS
                    |YOKAN_MODE_FILTER_VALUE
                    |YOKAN_MODE_LIB_FILTER
                    |YOKAN_MODE_NO_RDMA
                    )
            );
    }

    virtual void destroy() override {
        ScopedWriteLock lock(m_lock);
        m_db->clear();
    }

    virtual Status count(int32_t mode, uint64_t* c) const override {
        (void)mode;
        ScopedReadLock lock(m_lock);
        *c = m_db->size();
        return Status::OK;
    }

    virtual Status exists(int32_t mode, const UserMem& keys,
                          const BasicUserMem<size_t>& ksizes,
                          BitField& flags) const override {
        (void)mode;
        if(ksizes.size > flags.size) return Status::InvalidArg;
        size_t offset = 0;
        auto mode_wait = mode & YOKAN_MODE_WAIT;
        ScopedReadLock lock(m_lock);
        for(size_t i = 0; i < ksizes.size; i++) {
            const UserMem key{ keys.data + offset, ksizes[i] };
            if(offset + ksizes[i] > keys.size) return Status::InvalidArg;
            retry:
            auto it = m_db->find(key);
            if(it != m_db->end()) {
                flags[i] = true;
            } else if(mode_wait) {
                m_watcher.addKey(key);
                lock.unlock();
                auto ret = m_watcher.waitKey(key);
                lock.lock();
                if(ret == KeyWatcher::KeyPresent)
                    goto retry;
                else
                    return Status::TimedOut;
            } else {
                flags[i] = false;
            }
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
        auto mode_wait = mode & YOKAN_MODE_WAIT;
        ScopedReadLock lock(m_lock);
        for(size_t i = 0; i < ksizes.size; i++) {
            if(offset + ksizes[i] > keys.size) return Status::InvalidArg;
            const UserMem key{ keys.data + offset, ksizes[i] };
            retry:
            auto it = m_db->find(key);
            if(it != m_db->end()) {
                vsizes[i] = 0;
            } else if(mode_wait) {
                m_watcher.addKey(key);
                lock.unlock();
                auto ret = m_watcher.waitKey(key);
                lock.lock();
                if(ret == KeyWatcher::KeyPresent)
                    goto retry;
                else
                    return Status::TimedOut;
            } else {
                vsizes[i] = KeyNotFound;
            }
            offset += ksizes[i];
        }
        return Status::OK;
    }

    virtual Status put(int32_t mode,
                       const UserMem& keys,
                       const BasicUserMem<size_t>& ksizes,
                       const UserMem& vals,
                       const BasicUserMem<size_t>& vsizes) override {
        if(ksizes.size != vsizes.size) return Status::InvalidArg;
        if(vals.size != 0) return Status::InvalidArg;

        const auto mode_notify     = mode & YOKAN_MODE_NOTIFY;
        const auto mode_exist_only = mode & YOKAN_MODE_EXIST_ONLY;
        const auto mode_new_only   = mode & YOKAN_MODE_NEW_ONLY;

        size_t key_offset = 0;

        size_t total_ksizes = std::accumulate(ksizes.data,
                                              ksizes.data + ksizes.size,
                                              (size_t)0);
        if(total_ksizes > keys.size) return Status::InvalidArg;

        size_t total_vsizes = std::accumulate(vsizes.data,
                                              vsizes.data + vsizes.size,
                                              (size_t)0);
        if(total_vsizes != 0) return Status::InvalidArg;

        if(mode_exist_only) {
            if(ksizes.size == 1) {
                if(m_db->find(key_type{keys.data, ksizes[0], m_key_allocator}) == m_db->end())
                    return Status::NotFound;
            }
            return Status::OK;
        }

        if(mode_new_only) {
            if(ksizes.size == 1) {
                if(m_db->find(key_type{keys.data, ksizes[0], m_key_allocator}) != m_db->end())
                    return Status::KeyExists;
            }
        }

        ScopedWriteLock lock(m_lock);
        for(size_t i = 0; i < ksizes.size; i++) {
            m_db->emplace(keys.data + key_offset,
                          ksizes[i], m_key_allocator);
            key_offset += ksizes[i];
            if(mode_notify)
                m_watcher.notifyKey({ keys.data + key_offset, ksizes[i] });
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

        auto mode_wait = mode & YOKAN_MODE_WAIT;

        for(size_t i = 0; i < ksizes.size; i++) {
            const UserMem key{ keys.data + key_offset, ksizes[i] };
            retry:
            auto it = m_db->find(key);
            if(it != m_db->end()) {
                vsizes[i] = 0;
            } else if(mode_wait) {
                m_watcher.addKey(key);
                lock.unlock();
                auto ret = m_watcher.waitKey(key);
                lock.lock();
                if(ret == KeyWatcher::KeyPresent)
                    goto retry;
                else
                    return Status::TimedOut;
            } else {
                vsizes[i] = KeyNotFound;
            }
            key_offset += ksizes[i];
        }
        vals.size = 0;
        if(mode & YOKAN_MODE_CONSUME) {
            lock.unlock();
            return erase(mode, keys, ksizes);
        }
        return Status::OK;
    }

    virtual Status erase(int32_t mode, const UserMem& keys,
                         const BasicUserMem<size_t>& ksizes) override {
        size_t offset = 0;
        auto mode_wait = mode & YOKAN_MODE_WAIT;
        ScopedReadLock lock(m_lock);
        for(size_t i = 0; i < ksizes.size; i++) {
            auto key = UserMem{ keys.data + offset, ksizes[i] };
            if(offset + ksizes[i] > keys.size) return Status::InvalidArg;
            retry:
            auto it = m_db->find(key);
            if(it != m_db->end()) {
                m_db->erase(it);
            } else if(mode_wait) {
                m_watcher.addKey(key);
                lock.unlock();
                auto ret = m_watcher.waitKey(key);
                lock.lock();
                if(ret == KeyWatcher::KeyPresent)
                    goto retry;
                else
                    return Status::TimedOut;
            }
            offset += ksizes[i];
        }
        return Status::OK;
    }

    virtual Status listKeys(int32_t mode, bool packed,
                            const UserMem& fromKey,
                            const std::shared_ptr<KeyValueFilter>& filter,
                            UserMem& keys, BasicUserMem<size_t>& keySizes) const override {
        (void)mode;
        ScopedReadLock lock(m_lock);

        auto inclusive = mode & YOKAN_MODE_INCLUSIVE;

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
            if(!filter->check(key.data(), key.size(), nullptr, 0)) {
                if(filter->shouldStop(key.data(), key.size(), nullptr, 0))
                    break;
                continue;
            }
            auto umem = static_cast<char*>(keys.data) + offset;

            bool is_last = false;
            if(mode & YOKAN_MODE_KEEP_LAST) {
                auto next = it;
                ++next;
                is_last = (i+1 == max) || (next == end);
            }

            if(!packed) {

                size_t usize = keySizes[i];
                keySizes[i] = keyCopy(mode, is_last, filter, umem, usize,
                                      key.data(), key.size());
                offset += usize;

            } else { // if packed

                if(buf_too_small)
                    keySizes[i] = YOKAN_SIZE_TOO_SMALL;
                else {
                    keySizes[i] = keyCopy(mode, is_last, filter, umem, keys.size - offset,
                                          key.data(), key.size());
                    if(keySizes[i] == YOKAN_SIZE_TOO_SMALL)
                        buf_too_small = true;
                    else
                        offset += keySizes[i];
                }

            }
            i += 1;
        }

        keys.size = offset;
        for(; i < max; i++) {
            keySizes[i] = YOKAN_NO_MORE_KEYS;
        }

        return Status::OK;
    }

    virtual Status listKeyValues(int32_t mode,
                                 bool packed,
                                 const UserMem& fromKey,
                                 const std::shared_ptr<KeyValueFilter>& filter,
                                 UserMem& keys,
                                 BasicUserMem<size_t>& keySizes,
                                 UserMem& vals,
                                 BasicUserMem<size_t>& valSizes) const override {
        ScopedReadLock lock(m_lock);

        auto inclusive = mode & YOKAN_MODE_INCLUSIVE;

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
        size_t val_offset = 0;
        bool key_buf_too_small = false;
        bool val_buf_too_small = false;

        for(auto it = fromKeyIt; it != end && i < max; it++) {
            auto& key = *it;
            if(!filter->check(key.data(), key.size(), nullptr, 0)) {
                if(filter->shouldStop(key.data(), key.size(), nullptr, 0))
                    break;
                continue;
            }
            auto key_umem = static_cast<char*>(keys.data) + key_offset;
            auto val_umem = static_cast<char*>(vals.data) + val_offset;

            bool is_last = false;
            if(mode & YOKAN_MODE_KEEP_LAST) {
                auto next = it;
                ++next;
                is_last = (i+1 == max) || (next == end);
            }

            if(!packed) {

                size_t key_usize = keySizes[i];
                size_t val_usize = valSizes[i];
                keySizes[i] = keyCopy(mode, is_last, filter, key_umem, key_usize,
                                      key.data(), key.size());
                valSizes[i] = filter->valCopy(val_umem, val_usize, "", 0);
                key_offset += key_usize;
                val_offset += val_usize;

            } else { // not packed

                size_t key_usize = keys.size - key_offset;
                size_t val_usize = vals.size - val_offset;

                if(key_buf_too_small) {
                    keySizes[i] = YOKAN_SIZE_TOO_SMALL;
                } else {
                    keySizes[i] = keyCopy(mode, is_last, filter, key_umem, key_usize,
                                          key.data(), key.size());
                    if(keySizes[i] != YOKAN_SIZE_TOO_SMALL)
                        key_offset += keySizes[i];
                    else
                        key_buf_too_small = true;
                }
                if(val_buf_too_small) {
                    valSizes[i] = YOKAN_SIZE_TOO_SMALL;
                } else {
                    valSizes[i] = filter->valCopy(val_umem, val_usize, "", 0);
                    if(valSizes[i] != YOKAN_SIZE_TOO_SMALL)
                        val_offset += valSizes[i];
                    else
                        val_buf_too_small = true;
                }
            }
            i += 1;
        }
        keys.size = key_offset;
        vals.size = 0;
        for(; i < max; i++) {
            keySizes[i] = YOKAN_NO_MORE_KEYS;
            valSizes[i] = YOKAN_NO_MORE_KEYS;
        }

        return Status::OK;
    }

    ~SetDatabase() {
        if(m_lock != ABT_RWLOCK_NULL)
            ABT_rwlock_free(&m_lock);
        delete m_db;
        m_key_allocator.finalize(m_key_allocator.context);
        m_node_allocator.finalize(m_node_allocator.context);
    }

    private:

    using key_type = std::basic_string<char, std::char_traits<char>,
                                       Allocator<char>>;
    using comparator = SetDatabaseCompare<key_type>;
    using cmp_type = comparator::cmp_type;
    using allocator = Allocator<key_type>;
    using set_type = std::set<key_type, comparator, allocator>;

    SetDatabase(json cfg,
                     cmp_type cmp_fun,
                     const yk_allocator_t& node_allocator,
                     const yk_allocator_t& key_allocator)
    : m_config(std::move(cfg))
    , m_node_allocator(node_allocator)
    , m_key_allocator(key_allocator)
    {
        if(m_config["use_lock"].get<bool>())
            ABT_rwlock_create(&m_lock);
        m_db = new set_type(cmp_fun, allocator(m_node_allocator));
    }

    set_type*          m_db;
    json               m_config;
    ABT_rwlock         m_lock = ABT_RWLOCK_NULL;
    yk_allocator_t    m_node_allocator;
    yk_allocator_t    m_key_allocator;
    mutable KeyWatcher m_watcher;
};

}

YOKAN_REGISTER_BACKEND(set, yokan::SetDatabase);
