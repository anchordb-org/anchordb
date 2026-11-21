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

#include "anchordb/cache.h"
#include "anchordb/status.h"
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

// Owning, non-virtual facade over a Cache: forwards each operation and deletes
// the wrapped cache on destruction.  Holding it by value keeps forwarding
// inlinable, leaving one virtual dispatch per call.
class CacheWrapper {
  public:
    explicit CacheWrapper(Cache *cache) : cache_(cache) {}

    CacheWrapper(const CacheWrapper &) = delete;
    CacheWrapper &operator=(const CacheWrapper &) = delete;

    CacheWrapper(CacheWrapper &&other) noexcept : cache_(other.cache_) { other.cache_ = nullptr; }
    CacheWrapper &operator=(CacheWrapper &&other) noexcept {
        if (this != &other) {
            delete cache_;
            cache_ = other.cache_;
            other.cache_ = nullptr;
        }
        return *this;
    }

    ~CacheWrapper() { delete cache_; }

    bool valid() const { return cache_ != nullptr; }

    Cache::PinnedHandle insert(std::string_view key, void *value, size_t charge,
                               void (*deleter)(std::string_view key, void *value)) {
        return cache_->insert(key, value, charge, deleter);
    }

    Cache::PinnedHandle lookup(std::string_view key) { return cache_->lookup(key); }

    void erase(std::string_view key) { cache_->erase(key); }

    uint64_t new_id() { return cache_->new_id(); }

    void prune() { cache_->prune(); }

    size_t total_charge() const { return cache_->total_charge(); }

  private:
    Cache *cache_;
};

// Looks up `key` in `cache`; on a miss calls `open(&value)`, charges
// `charge(value)`, and inserts the result.
//
// On success `*pin` owns the entry and `*value`, when non-null, points at its
// value.  The caller supplies the deleter used by the cache (and as a defensive
// fallback should an implementation ever decline an insertion).
template <typename V, typename OpenFn, typename ChargeFn>
Status cache_lookup_or_open(CacheWrapper &cache, std::string_view key, Cache::PinnedHandle *pin, V **value,
                            OpenFn &&open, ChargeFn &&charge, void (*deleter)(std::string_view, void *)) {
    if (pin == nullptr) { return Status::invalid_argument("cache_lookup_or_open: null pin"); }

    pin->reset();
    if (value != nullptr) { *value = nullptr; }

    if (Cache::PinnedHandle hit = cache.lookup(key)) {
        *pin = std::move(hit);
        if (value != nullptr) { *value = static_cast<V *>(pin->value()); }
        return Status::ok();
    }

    V *opened = nullptr;
    Status s = open(&opened);
    if (!s.is_ok()) { return s; }
    if (opened == nullptr) { return Status::corruption("cache_lookup_or_open: opener returned null"); }

    *pin = cache.insert(key, opened, charge(opened), deleter);
    if (!*pin) {
        // The cache declined ownership, so free the opened value ourselves.
        deleter(key, opened);
        return Status::io_error("cache_lookup_or_open: insert declined");
    }
    if (value != nullptr) { *value = static_cast<V *>(pin->value()); }
    return Status::ok();
}

} // namespace ANCHORDB_NAMESPACE
