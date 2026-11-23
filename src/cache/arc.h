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

// ARC (Adaptive Replacement Cache).  Resident entries live in T1 (recent) or
// T2 (frequent); evicted entries become "ghosts" in B1/B2 whose hit pattern
// adapts the target size p_ of T1.
class ARCCache {
  public:
    explicit ARCCache(size_t capacity = 0, MapType t = MapType::Swiss)
        : capacity_(capacity), usage_(0), p_(0), map_(capacity, t) {}

    ~ARCCache() { prune(); }

    ARCCache(const ARCCache &) = delete;
    ARCCache &operator=(const ARCCache &) = delete;

    ARCCache(ARCCache &&other) noexcept : capacity_(other.capacity_) {
        other.mutex_.lock();
        usage_ = other.usage_;
        p_ = other.p_;
        t1_lru_ = std::move(other.t1_lru_);
        t1_in_use_ = std::move(other.t1_in_use_);
        t2_lru_ = std::move(other.t2_lru_);
        t2_in_use_ = std::move(other.t2_in_use_);
        b1_ = std::move(other.b1_);
        b2_ = std::move(other.b2_);
        map_ = std::move(other.map_);
        other.mutex_.unlock();
    }

    ARCCache &operator=(ARCCache &&other) noexcept {
        if (this != &other) {
            this->mutex_.lock();
            other.mutex_.lock();

            capacity_ = other.capacity_;
            usage_ = other.usage_;
            p_ = other.p_;
            t1_lru_ = std::move(other.t1_lru_);
            t1_in_use_ = std::move(other.t1_in_use_);
            t2_lru_ = std::move(other.t2_lru_);
            t2_in_use_ = std::move(other.t2_in_use_);
            b1_ = std::move(other.b1_);
            b2_ = std::move(other.b2_);
            map_ = std::move(other.map_);

            this->mutex_.unlock();
            other.mutex_.unlock();
        }
        return *this;
    }

    void set_capacity(size_t capacity) { capacity_ = capacity; }

    ARCHandle *insert(std::string_view key, uint64_t hash, void *value, size_t charge, CacheDeleter deleter);
    ARCHandle *lookup(std::string_view key, uint64_t hash);
    void release(ARCHandle *handle);
    void erase(std::string_view key, uint64_t hash);
    void prune();
    size_t total_charge() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return usage_;
    }

  private:
    bool finish_erase(ARCHandle *e);
    void ref(ARCHandle *e);
    void unref(ARCHandle *e);
    void replace(const HashedKey &key, bool in_b2);
    void remove_ghost_(ARCHandle *e);

    size_t capacity_;
    mutable std::mutex mutex_;
    size_t usage_;
    size_t p_; // Target size for T1

    // T1: Recent entries (split into unreferenced LRU and client-referenced
    // in_use)
    List<ARCHandle *> t1_lru_;
    List<ARCHandle *> t1_in_use_;

    // T2: Frequent entries (split into unreferenced LRU and client-referenced
    // in_use)
    List<ARCHandle *> t2_lru_;
    List<ARCHandle *> t2_in_use_;

    // B1 & B2: Ghost entries (metadata only, value evicted)
    List<ARCHandle *> b1_;
    List<ARCHandle *> b2_;

    KeyIndex<ARCHandle *> map_;
};

} // namespace ANCHORDB_NAMESPACE
