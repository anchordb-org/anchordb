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

#include "lru.h"
#include "sharded_cache.h"

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

LRUHandle *LRUCache::insert(std::string_view key, uint64_t hash, void *value, size_t charge, CacheDeleter deleter) {
    std::lock_guard<std::mutex> lock(mutex_);
    LRUHandle *e = allocate_entry<LRUHandle>(key);
    if (e == nullptr) { return nullptr; }

    e->value = value;
    e->deleter = deleter;
    e->charge = charge;
    e->hash = hash;
    e->in_cache = false;
    e->refs = 1; // for the returned handle

    if (capacity_ > 0) {
        e->refs++; // for the cache's reference
        e->in_cache = true;
        usage_ += charge;

        // Detach any previous entry for this key before linking the new one, so
        // the in-use list index never holds two nodes for the same key (which
        // would make push_front a no-op and leave the new entry untracked).
        LRUHandle *old = map_.remove(HashedKey{e->key(), e->hash});
        finish_erase(old);

        // The returned handle keeps the entry in use until the caller releases
        // it, so it starts on the in-use list.
        in_use_.push_front(HashedKey{e->key(), e->hash}, e);
        map_.insert(HashedKey{e->key(), e->hash}, e);
    }

    while (usage_ > capacity_ && !lru_.is_empty()) {
        LRUHandle *old = lru_.back();
        assert(old->refs == 1);
        map_.remove(HashedKey{old->key(), old->hash});
        bool erased = finish_erase(old);
        if (!erased) { assert(erased); }
    }

    return e;
}

LRUHandle *LRUCache::lookup(std::string_view key, uint64_t hash) {
    std::lock_guard<std::mutex> lock(mutex_);
    LRUHandle *e = map_.get(HashedKey{key, hash});
    if (e != nullptr) { ref(e); }
    return e;
}

void LRUCache::release(LRUHandle *handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    unref(handle);
}

void LRUCache::erase(std::string_view key, uint64_t hash) {
    std::lock_guard<std::mutex> lock(mutex_);
    LRUHandle *e = map_.remove(HashedKey{key, hash});
    finish_erase(e);
}

void LRUCache::prune() {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!lru_.is_empty()) {
        LRUHandle *e = lru_.back();
        assert(e->refs == 1);
        map_.remove(HashedKey{e->key(), e->hash});
        bool erased = finish_erase(e);
        if (!erased) assert(erased);
    }
}

void LRUCache::ref(LRUHandle *e) {
    if (e->refs == 1 && e->in_cache) {
        lru_.remove(HashedKey{e->key(), e->hash});
        in_use_.push_front(HashedKey{e->key(), e->hash}, e);
    }
    e->refs++;
}

void LRUCache::unref(LRUHandle *e) {
    assert(e->refs > 0);
    e->refs--;
    if (e->refs == 0) {
        assert(!e->in_cache);
        (*e->deleter)(e->key(), e->value);
        free_entry(e);
    } else if (e->in_cache && e->refs == 1) {
        in_use_.remove(HashedKey{e->key(), e->hash});
        lru_.push_front(HashedKey{e->key(), e->hash}, e);
    }
}

bool LRUCache::finish_erase(LRUHandle *e) {
    if (e != nullptr) {
        assert(e->in_cache);
        HashedKey k{e->key(), e->hash};
        if (lru_.contains(k)) {
            lru_.remove(k);
        } else {
            in_use_.remove(k);
        }
        e->in_cache = false;
        usage_ -= e->charge;
        unref(e);
    }
    return e != nullptr;
}

Cache *new_lru_cache(size_t capacity, MapType t) { return new ShardedCache<LRUCache, LRUHandle>(capacity, t); }

} // namespace ANCHORDB_NAMESPACE
