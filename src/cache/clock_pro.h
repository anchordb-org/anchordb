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
#include "cache/circular_ring.h"
#include "cache/key_index.h"
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

// CLOCK-Pro cache.  A single circular ring holds resident and non-resident
// entries, classified hot/cold; three hands (cold, hot, test) advance it to
// promote, demote and evict.
class ClockProCache {
  public:
    explicit ClockProCache(size_t capacity = 0, MapType t = MapType::Swiss)
        : capacity_(capacity), usage_(0), hot_usage_(0), map_(capacity, t) {

        // Cold capacity starts at full capacity and adapts at runtime.
        cold_capacity_ = capacity_;
    }
    // Frees every entry still held by the cache. Callers must have released
    // all outstanding handles before destroying the cache.
    ~ClockProCache() { prune(); }

    ClockProCache(const ClockProCache &) = delete;
    ClockProCache &operator=(const ClockProCache &) = delete;

    ClockProCache(ClockProCache &&other) noexcept : capacity_(other.capacity_) {
        other.mutex_.lock();
        usage_ = other.usage_;
        hot_usage_ = other.hot_usage_;
        cold_capacity_ = other.cold_capacity_;
        ring_ = std::move(other.ring_);
        hand_cold_ = other.hand_cold_;
        hand_hot_ = other.hand_hot_;
        hand_test_ = other.hand_test_;
        map_ = std::move(other.map_);

        other.hand_cold_ = nullptr;
        other.hand_hot_ = nullptr;
        other.hand_test_ = nullptr;
        other.mutex_.unlock();
    }

    ClockProCache &operator=(ClockProCache &&other) noexcept {
        if (this != &other) {
            this->mutex_.lock();
            other.mutex_.lock();

            capacity_ = other.capacity_;
            usage_ = other.usage_;
            hot_usage_ = other.hot_usage_;
            cold_capacity_ = other.cold_capacity_;
            ring_ = std::move(other.ring_);
            hand_cold_ = other.hand_cold_;
            hand_hot_ = other.hand_hot_;
            hand_test_ = other.hand_test_;
            map_ = std::move(other.map_);

            other.hand_cold_ = nullptr;
            other.hand_hot_ = nullptr;
            other.hand_test_ = nullptr;

            this->mutex_.unlock();
            other.mutex_.unlock();
        }
        return *this;
    }

    void set_capacity(size_t capacity) {
        capacity_ = capacity;
        cold_capacity_ = capacity_;
    }

    ClockProHandle *insert(std::string_view key, uint64_t hash, void *value, size_t charge, CacheDeleter deleter);
    ClockProHandle *lookup(std::string_view key, uint64_t hash);
    void release(ClockProHandle *handle);
    void erase(std::string_view key, uint64_t hash);
    void prune();
    size_t total_charge() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return usage_;
    }

  private:
    void hand_cold_step();
    void hand_hot_step();
    void hand_test_step();
    void evict();

    void ring_add(ClockProHandle *e);
    void ring_remove(ClockProHandle *e);

    void ref(ClockProHandle *e);
    void unref(ClockProHandle *e);
    bool finish_erase(ClockProHandle *e);

    size_t capacity_;
    size_t cold_capacity_;
    mutable std::mutex mutex_;
    size_t usage_;
    size_t hot_usage_;

    CircularRing ring_;
    ClockProHandle *hand_cold_ = nullptr;
    ClockProHandle *hand_hot_ = nullptr;
    ClockProHandle *hand_test_ = nullptr;

    KeyIndex<ClockProHandle *> map_;
};

} // namespace ANCHORDB_NAMESPACE
