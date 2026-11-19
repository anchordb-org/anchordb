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

#include "anchordb/anchordb_namespace.h"
#include "anchordb/cache.h"
#include "anchordb/status.h"
#include "cache/cache_wrapper.h"
#include <string_view>

namespace ANCHORDB_NAMESPACE {

// RAII pin on one Cache entry.  The pin releases the handle it holds on
// destruction, so callers never touch Cache::release() directly.  Move-only:
// ownership of the pin (and therefore the release) transfers on move.
template <typename V> class CachePin {
  public:
    CachePin() = default;
    CachePin(Cache *cache, Cache::Handle *handle) : cache_(cache), handle_(handle) {}

    ~CachePin() { reset(); }

    CachePin(const CachePin &) = delete;
    CachePin &operator=(const CachePin &) = delete;

    CachePin(CachePin &&other) noexcept : cache_(other.cache_), handle_(other.handle_) {
        other.cache_ = nullptr;
        other.handle_ = nullptr;
    }

    CachePin &operator=(CachePin &&other) noexcept {
        if (this != &other) {
            reset();
            cache_ = other.cache_;
            handle_ = other.handle_;
            other.cache_ = nullptr;
            other.handle_ = nullptr;
        }
        return *this;
    }

    bool valid() const { return handle_ != nullptr; }
    explicit operator bool() const { return valid(); }

    // The pinned value, or nullptr when the pin is empty.
    V *get() const { return valid() ? static_cast<V *>(cache_->value(handle_)) : nullptr; }
    V *operator->() const { return get(); }
    V &operator*() const { return *get(); }

    Cache::Handle *handle() const { return handle_; }

    // Releases the entry.  Safe to call on an empty pin.
    void reset() {
        if (handle_ != nullptr) { cache_->release(handle_); }
        cache_ = nullptr;
        handle_ = nullptr;
    }

  private:
    Cache *cache_ = nullptr;
    Cache::Handle *handle_ = nullptr;
};

// Looks up `key` in `cache`; on a miss calls `open(&value)`, charges
// `charge(value)`, and inserts the result.
//
// On success `*value` is always set.  When `pin` is valid the cache owns the
// value and the pin must outlive every use of it; when the pin is empty the
// cache declined the insertion and the caller owns `*value`.  The caller
// supplies the deleter the cache will invoke.
template <typename V, typename OpenFn, typename ChargeFn>
Status cache_lookup_or_open(CacheWrapper &cache, std::string_view key, CachePin<V> *pin, V **value, OpenFn &&open,
                            ChargeFn &&charge, void (*deleter)(std::string_view, void *)) {
    if (pin == nullptr || value == nullptr) { return Status::invalid_argument("cache_lookup_or_open: null output"); }

    pin->reset();
    *value = nullptr;

    Cache::Handle *hit = cache.lookup(key);
    if (hit != nullptr) {
        *pin = CachePin<V>(cache.raw(), hit);
        *value = pin->get();
        return Status::ok();
    }

    V *opened = nullptr;
    Status s = open(&opened);
    if (!s.is_ok()) { return s; }
    if (opened == nullptr) { return Status::corruption("cache_lookup_or_open: opener returned null"); }

    Cache::Handle *inserted = cache.insert(key, opened, charge(opened), deleter);
    *value = opened;
    if (inserted != nullptr) { *pin = CachePin<V>(cache.raw(), inserted); }
    return Status::ok();
}

} // namespace ANCHORDB_NAMESPACE
