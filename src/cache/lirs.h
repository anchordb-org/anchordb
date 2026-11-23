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

// LIRS (Low Inter-reference Recency Set) cache.  Stack S orders blocks by
// reuse distance and queue Q holds resident HIR blocks; LIR capacity is 95% of
// the total.
class LIRSCache {
  public:
    explicit LIRSCache(size_t capacity = 0, MapType t = MapType::Swiss)
        : capacity_(capacity), usage_(0), lir_usage_(0), map_(capacity, t) {

        // Allocate 95% capacity to LIR and remaining 5% to HIR
        lir_capacity_ = (capacity_ * 95) / 100;
        hir_capacity_ = capacity_ - lir_capacity_;
    }
    // Frees every entry still held by the cache. Callers must have released
    // all outstanding handles before destroying the cache.
    ~LIRSCache() { clear_all(); }

    LIRSCache(const LIRSCache &) = delete;
    LIRSCache &operator=(const LIRSCache &) = delete;

    LIRSCache(LIRSCache &&other) noexcept : capacity_(other.capacity_) {
        other.mutex_.lock();
        usage_ = other.usage_;
        lir_usage_ = other.lir_usage_;
        lir_capacity_ = other.lir_capacity_;
        hir_capacity_ = other.hir_capacity_;
        stack_ = std::move(other.stack_);
        hir_list_ = std::move(other.hir_list_);
        map_ = std::move(other.map_);
        other.mutex_.unlock();
    }

    LIRSCache &operator=(LIRSCache &&other) noexcept {
        if (this != &other) {
            this->mutex_.lock();
            other.mutex_.lock();

            capacity_ = other.capacity_;
            usage_ = other.usage_;
            lir_usage_ = other.lir_usage_;
            lir_capacity_ = other.lir_capacity_;
            hir_capacity_ = other.hir_capacity_;
            stack_ = std::move(other.stack_);
            hir_list_ = std::move(other.hir_list_);
            map_ = std::move(other.map_);

            this->mutex_.unlock();
            other.mutex_.unlock();
        }
        return *this;
    }

    void set_capacity(size_t capacity) {
        capacity_ = capacity;
        lir_capacity_ = (capacity_ * 95) / 100;
        hir_capacity_ = capacity_ - lir_capacity_;
    }

    LIRSHandle *insert(std::string_view key, uint64_t hash, void *value, size_t charge, CacheDeleter deleter);
    LIRSHandle *lookup(std::string_view key, uint64_t hash);
    void release(LIRSHandle *handle);
    void erase(std::string_view key, uint64_t hash);
    void prune();
    size_t total_charge() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return usage_;
    }

  private:
    void clear_all();
    void prune_stack();
    void evict();
    void ref(LIRSHandle *e);
    void unref(LIRSHandle *e);
    bool finish_erase(LIRSHandle *e);

    size_t capacity_;
    size_t lir_capacity_;
    size_t hir_capacity_;
    mutable std::mutex mutex_;
    size_t usage_;
    size_t lir_usage_;

    List<LIRSHandle *> stack_;    // LIRS Stack S
    List<LIRSHandle *> hir_list_; // LIRS Queue Q (resident HIR blocks)

    KeyIndex<LIRSHandle *> map_;
};

} // namespace ANCHORDB_NAMESPACE
