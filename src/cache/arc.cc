// Copyright 2026 Tassok Imam Wasi

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0

// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "arc.h"
#include "sharded_cache.h"
#include <string_view>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

void ARCCache::remove_ghost_(ARCHandle *e) {
    HashedKey k{e->key(), e->hash};
    if (e->state == ARCHandle::State::kB1) {
        b1_.remove(k);
    } else if (e->state == ARCHandle::State::kB2) {
        b2_.remove(k);
    }

    map_.remove(k);
    free_entry(e);
}

// Evicts one resident entry from T1 or T2 into the matching ghost list,
// following ARC's REPLACE rule (T1 if it exceeds its target p_).
void ARCCache::replace(const HashedKey &key, bool in_b2) {
    size_t t1_size = t1_lru_.size() + t1_in_use_.size();
    if (t1_size > 0 && ((t1_size > p_) || (in_b2 && t1_size == p_))) {
        if (!t1_lru_.is_empty()) {
            ARCHandle *old = t1_lru_.back();
            t1_lru_.pop_back();

            // Demote from T1 to B1 (Ghost list)
            old->in_cache = false;
            old->state = ARCHandle::State::kB1;
            usage_ -= old->charge;
            install_value(old, nullptr, nullptr);

            b1_.push_front(HashedKey{old->key(), old->hash}, old);
        }
    } else {
        if (!t2_lru_.is_empty()) {
            ARCHandle *old = t2_lru_.back();
            t2_lru_.pop_back();

            // Demote from T2 to B2 (Ghost list)
            old->in_cache = false;
            old->state = ARCHandle::State::kB2;
            usage_ -= old->charge;
            install_value(old, nullptr, nullptr);

            b2_.push_front(HashedKey{old->key(), old->hash}, old);
        }
    }
}

ARCHandle *ARCCache::insert(std::string_view key, uint64_t hash, void *value, size_t charge, CacheDeleter deleter) {
    std::lock_guard<std::mutex> lock(mutex_);
    HashedKey hkey{key, hash};

    // A zero-capacity cache stores nothing.  Hand back an uncached node that
    // the caller's release() frees, matching LRU's capacity-0 behavior.
    if (capacity_ == 0) {
        ARCHandle *uncached = allocate_entry<ARCHandle>(key);
        if (uncached == nullptr) { return nullptr; }
        uncached->value = value;
        uncached->deleter = deleter;
        uncached->charge = charge;
        uncached->hash = hash;
        uncached->in_cache = false;
        uncached->state = ARCHandle::State::kT1;
        uncached->refs = 1; // only the returned handle
        return uncached;
    }

    ARCHandle *e = map_.get(hkey);

    if (e != nullptr && e->in_cache) {
        // Cache Hit: Key already present in T1 or T2.  The replaced value is
        // deferred while a Handle still pins the entry.
        usage_ -= e->charge;
        install_value(e, value, deleter);
        e->charge = charge;
        usage_ += charge;
        ref(e);
        return e;
    }

    // Allocate new handle
    ARCHandle *handle = allocate_entry<ARCHandle>(key);
    if (handle == nullptr) { return nullptr; }

    handle->value = value;
    handle->deleter = deleter;
    handle->charge = charge;
    handle->hash = hash;
    handle->in_cache = true;
    handle->refs = 2; // 1 for returned client handle, 1 for cache reference

    if (e != nullptr && !e->in_cache) {
        // Ghost Hit: Entry found in B1 or B2
        if (e->state == ARCHandle::State::kB1) {
            // Adapt target size p towards T1 expansion
            size_t delta = (b1_.size() >= b2_.size()) ? 1 : b2_.size() / b1_.size();
            p_ = std::min(capacity_, p_ + delta);
            replace(hkey, false);
            remove_ghost_(e);
        } else if (e->state == ARCHandle::State::kB2) {
            // Adapt target size p towards T2 expansion
            size_t delta = (b2_.size() >= b1_.size()) ? 1 : b1_.size() / b2_.size();
            p_ = (p_ > delta) ? p_ - delta : 0;
            replace(hkey, true);
            remove_ghost_(e);
        }

        // Promoted directly to T2 upon ghost hit.  Key the lists/map by the
        // handle's own copied key, not the caller's transient view.
        handle->state = ARCHandle::State::kT2;
        HashedKey handle_key{handle->key(), handle->hash};
        t2_in_use_.push_front(handle_key, handle);
        usage_ += charge;
        map_.insert(handle_key, handle);

    } else {
        // Complete Cache Miss
        size_t l1_size = t1_lru_.size() + t1_in_use_.size() + b1_.size();
        size_t l2_size = t2_lru_.size() + t2_in_use_.size() + b2_.size();

        if (l1_size == capacity_) {
            if (t1_lru_.size() + t1_in_use_.size() < capacity_) {
                if (!b1_.is_empty()) {
                    ARCHandle *g = b1_.back();
                    remove_ghost_(g);
                }
                replace(hkey, false);
            } else {
                if (!t1_lru_.is_empty()) {
                    ARCHandle *old = t1_lru_.back();
                    map_.remove(HashedKey{old->key(), old->hash});
                    finish_erase(old);
                }
            }
        } else if (l1_size < capacity_ && (l1_size + l2_size >= capacity_)) {
            if (l1_size + l2_size == 2 * capacity_) {
                if (!b2_.is_empty()) {
                    ARCHandle *g = b2_.back();
                    remove_ghost_(g);
                }
            }
            replace(hkey, false);
        }

        // New item starts in T1.  Use the handle's copied key so the map/list
        // entries do not alias the caller's transient string_view.
        handle->state = ARCHandle::State::kT1;
        HashedKey handle_key{handle->key(), handle->hash};
        t1_in_use_.push_front(handle_key, handle);
        usage_ += charge;
        map_.insert(handle_key, handle);
    }

    return handle;
}

ARCHandle *ARCCache::lookup(std::string_view key, uint64_t hash) {
    std::lock_guard<std::mutex> lock(mutex_);
    HashedKey hkey{key, hash};
    ARCHandle *e = map_.get(hkey);

    if (e != nullptr && e->in_cache) {
        ref(e);
    } else {
        e = nullptr; // Ghost entries cannot be looked up by callers
    }
    return e;
}

void ARCCache::release(ARCHandle *handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    unref(handle);
}

void ARCCache::erase(std::string_view key, uint64_t hash) {
    std::lock_guard<std::mutex> lock(mutex_);
    HashedKey hkey{key, hash};
    ARCHandle *e = map_.remove(hkey);

    if (e != nullptr) {
        if (e->in_cache) {
            finish_erase(e);
        } else {
            remove_ghost_(e);
        }
    }
}

void ARCCache::prune() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto purge_lru = [&](List<ARCHandle *> &lru_list) {
        while (!lru_list.is_empty()) {
            ARCHandle *e = lru_list.back();
            assert(e->refs == 1);
            map_.remove(HashedKey{e->key(), e->hash});
            finish_erase(e);
        }
    };

    purge_lru(t1_lru_);
    purge_lru(t2_lru_);

    while (!b1_.is_empty()) { remove_ghost_(b1_.back()); }
    while (!b2_.is_empty()) { remove_ghost_(b2_.back()); }
}

void ARCCache::ref(ARCHandle *e) {
    HashedKey k{e->key(), e->hash};
    if (e->refs == 1 && e->in_cache) {
        // Move node out of its current LRU list using state check
        if (e->state == ARCHandle::State::kT1) {
            t1_lru_.remove(k);
        } else if (e->state == ARCHandle::State::kT2) {
            t2_lru_.remove(k);
        }

        // Re-referenced item gets promoted/kept in T2 in-use
        e->state = ARCHandle::State::kT2;
        t2_in_use_.push_front(k, e);
    }
    e->refs++;
}

void ARCCache::unref(ARCHandle *e) {
    assert(e->refs > 0);
    e->refs--;
    if (e->refs == 0) {
        assert(!e->in_cache);
        if (e->value != nullptr) { (*e->deleter)(e->key(), e->value); }
        free_entry(e);
    } else if (e->in_cache && e->refs == 1) {
        flush_deferred(e);
        HashedKey k{e->key(), e->hash};
        // Move node from in-use to its corresponding LRU sub-list
        if (e->state == ARCHandle::State::kT1) {
            t1_in_use_.remove(k);
            t1_lru_.push_front(k, e);
        } else if (e->state == ARCHandle::State::kT2) {
            t2_in_use_.remove(k);
            t2_lru_.push_front(k, e);
        }
    }
}

bool ARCCache::finish_erase(ARCHandle *e) {
    if (e != nullptr && e->in_cache) {
        HashedKey k{e->key(), e->hash};
        if (e->state == ARCHandle::State::kT1) {
            if (t1_lru_.contains(k)) {
                t1_lru_.remove(k);
            } else {
                t1_in_use_.remove(k);
            }
        } else if (e->state == ARCHandle::State::kT2) {
            if (t2_lru_.contains(k)) {
                t2_lru_.remove(k);
            } else {
                t2_in_use_.remove(k);
            }
        }

        e->in_cache = false;
        usage_ -= e->charge;
        unref(e);
        return true;
    }
    return false;
}

Cache *new_arc_cache(size_t capacity, MapType t) { return new ShardedCache<ARCCache, ARCHandle>(capacity, t); }

} // namespace ANCHORDB_NAMESPACE
