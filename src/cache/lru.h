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

#pragma once

#include "anchordb/options.h"
#include "cache/key_index.h"
#include "cache/list.h"
#include "cache/util.h"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

// LRU cache.  Entries currently held by a caller live on in_use_; released
// entries move to the lru_ list, from which the least-recently-used are evicted
// once usage exceeds capacity.
class LRUCache {
  public:
    explicit LRUCache(size_t capacity = 0, MapType t = MapType::Swiss)
        : capacity_(capacity), usage_(0), map_(capacity, t) {}
    ~LRUCache() { prune(); }

    LRUCache(const LRUCache &) = delete;
    LRUCache &operator=(const LRUCache &) = delete;

    LRUCache(LRUCache &&other) noexcept : capacity_(other.capacity_) {
        other.mutex_.lock();
        usage_ = other.usage_;
        lru_ = std::move(other.lru_);
        in_use_ = std::move(other.in_use_);
        map_ = std::move(other.map_);
        other.mutex_.unlock();
    }

    LRUCache &operator=(LRUCache &&other) noexcept {
        if (this != &other) {
            this->mutex_.lock();
            other.mutex_.lock();

            capacity_ = other.capacity_;
            usage_ = other.usage_;
            lru_ = std::move(other.lru_);
            in_use_ = std::move(other.in_use_);
            map_ = std::move(other.map_);

            this->mutex_.unlock();
            other.mutex_.unlock();
        }
        return *this;
    }

    void set_capacity(size_t capacity) { capacity_ = capacity; }

    LRUHandle *insert(std::string_view key, uint64_t hash, void *value, size_t charge, CacheDeleter deleter);
    LRUHandle *lookup(std::string_view key, uint64_t hash);
    void release(LRUHandle *handle);
    void erase(std::string_view key, uint64_t hash);
    void prune();
    size_t total_charge() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return usage_;
    }

  private:
    bool finish_erase(LRUHandle *e);
    void ref(LRUHandle *e);
    void unref(LRUHandle *e);

    size_t capacity_;
    mutable std::mutex mutex_;
    size_t usage_;

    List<LRUHandle *> lru_;
    List<LRUHandle *> in_use_;
    KeyIndex<LRUHandle *> map_;
};

} // namespace ANCHORDB_NAMESPACE
