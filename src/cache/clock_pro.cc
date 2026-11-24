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

#include "clock_pro.h"
#include "sharded_cache.h"

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

ClockProHandle *ClockProCache::insert(std::string_view key, uint64_t hash, void *value, size_t charge,
                                      CacheDeleter deleter) {
    std::lock_guard<std::mutex> lock(mutex_);

    // A zero-capacity cache stores nothing.  Hand back an uncached node that
    // the caller's release() frees, matching LRU's capacity-0 behavior and
    // avoiding a ring that can never be drained by evict().
    if (capacity_ == 0) {
        ClockProHandle *uncached = allocate_entry<ClockProHandle>(key);
        if (uncached == nullptr) { return nullptr; }
        uncached->value = value;
        uncached->deleter = deleter;
        uncached->charge = charge;
        uncached->hash = hash;
        uncached->in_cache = false;
        uncached->is_resident = true;
        uncached->is_hot = false;
        uncached->ref_bit = false;
        uncached->refs = 1; // only the returned handle
        return uncached;
    }

    ClockProHandle *e = map_.get(HashedKey{key, hash});

    if (e != nullptr) {
        if (e->is_resident) {
            // Overwrite existing resident entry payload (old value deferred if
            // it is still pinned).
            usage_ -= e->charge;
            if (e->is_hot) { hot_usage_ -= e->charge; }
        } else {
            // Non-resident history hit -> Promote to HOT entry
            if (cold_capacity_ > e->charge) {
                cold_capacity_ -= e->charge;
            } else {
                cold_capacity_ = 0;
            }
            e->is_hot = true;
        }

        install_value(e, value, deleter);
        e->charge = charge;
        e->is_resident = true;
        e->ref_bit = true;
        usage_ += charge;

        if (e->is_hot) { hot_usage_ += charge; }

        ref(e);
        evict();
        return e;
    }

    // Allocate new node
    e = allocate_entry<ClockProHandle>(key);
    if (e == nullptr) { return nullptr; }

    e->value = value;
    e->deleter = deleter;
    e->charge = charge;
    e->hash = hash;
    e->in_cache = true;
    e->is_resident = true;
    e->is_hot = false;
    e->ref_bit = false;
    e->refs = 2; // 1 for returned handle, 1 for cache internal structure

    usage_ += charge;
    ring_add(e);

    map_.insert(HashedKey{e->key(), e->hash}, e);

    evict();
    return e;
}

ClockProHandle *ClockProCache::lookup(std::string_view key, uint64_t hash) {
    std::lock_guard<std::mutex> lock(mutex_);
    ClockProHandle *e = map_.get(HashedKey{key, hash});

    if (e != nullptr && e->is_resident) {
        e->ref_bit = true;
        ref(e);
    } else {
        e = nullptr; // History nodes return null on lookup
    }

    return e;
}

void ClockProCache::release(ClockProHandle *handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    unref(handle);
}

void ClockProCache::erase(std::string_view key, uint64_t hash) {
    std::lock_guard<std::mutex> lock(mutex_);
    ClockProHandle *e = map_.remove(HashedKey{key, hash});
    finish_erase(e);
}

void ClockProCache::prune() {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!ring_.is_empty()) {
        ClockProHandle *e = ring_.head();
        map_.remove(HashedKey{e->key(), e->hash});
        finish_erase(e);
    }
}

void ClockProCache::evict() {
    // The cold hand makes no progress when every node it visits is hot.  Bound
    // a full traversal and then let the hot hand demote, otherwise an all-hot
    // ring with usage_ > capacity_ would spin forever (e.g. small capacities
    // where a single charge exceeds capacity).
    size_t steps_without_progress = 0;
    while (usage_ > capacity_ && !ring_.is_empty()) {
        const size_t before = usage_;
        hand_cold_step();
        if (usage_ < before) {
            steps_without_progress = 0;
            continue;
        }
        if (++steps_without_progress > ring_.size()) {
            hand_hot_step();
            steps_without_progress = 0;
        }
    }
}

void ClockProCache::hand_cold_step() {
    if (ring_.is_empty()) return;

    ClockProHandle *e = hand_cold_;
    if (e == nullptr) {
        e = ring_.head();
        hand_cold_ = e;
    }

    if (!e->is_hot) {
        if (e->ref_bit) {
            // Second chance granted -> Promote to HOT
            e->is_hot = true;
            e->ref_bit = false;
            hot_usage_ += e->charge;

            // Balance hot/cold ratio if hot capacity exceeded
            while (hot_usage_ > (capacity_ - cold_capacity_)) { hand_hot_step(); }
        } else {
            // Evict cold entry payload
            if (e->is_resident) {
                // Defer the payload if a Handle still pins this entry.
                install_value(e, nullptr, nullptr);
                e->is_resident = false;
                usage_ -= e->charge;
            } else {
                // Remove non-resident history node completely. ring_remove()
                // advances hand_cold_ past the freed node.  Drop the map entry
                // first: finish_erase() frees the node, and a dangling map entry
                // would be dereferenced by the next lookup/insert.
                map_.remove(HashedKey{e->key(), e->hash});
                finish_erase(e);
                return;
            }
        }
    }

    hand_cold_ = ring_.step(e);
}

void ClockProCache::hand_hot_step() {
    if (ring_.is_empty()) return;

    ClockProHandle *e = hand_hot_;
    if (e == nullptr) {
        e = ring_.head();
        hand_hot_ = e;
    }

    if (e->is_hot) {
        if (e->ref_bit) {
            e->ref_bit = false;
        } else {
            // Demote HOT entry to COLD
            e->is_hot = false;
            hot_usage_ -= e->charge;
        }
    }

    hand_hot_ = ring_.step(e);
}

void ClockProCache::hand_test_step() {
    if (ring_.is_empty()) return;

    ClockProHandle *e = hand_test_;
    if (e == nullptr) {
        e = ring_.head();
        hand_test_ = e;
    }

    // Test hand reclaims non-resident COLD history nodes that were never
    // re-referenced. ring_remove() advances hand_test_ past the freed node.
    if (!e->is_hot && !e->is_resident) {
        // Remove the map entry before finish_erase() frees the node.
        map_.remove(HashedKey{e->key(), e->hash});
        finish_erase(e);
        return;
    }

    hand_test_ = ring_.step(e);
}

void ClockProCache::ring_add(ClockProHandle *e) { ring_.push_back(HashedKey{e->key(), e->hash}, e); }

void ClockProCache::ring_remove(ClockProHandle *e) {
    // Advance any hand that points at the node being unlinked so it never
    // dangles after the node is freed.
    ClockProHandle *next = ring_.step(e);
    if (next == e) {
        next = nullptr; // e was the only node in the ring
    }
    if (hand_cold_ == e) { hand_cold_ = next; }
    if (hand_hot_ == e) { hand_hot_ = next; }
    if (hand_test_ == e) { hand_test_ = next; }

    ring_.remove(e);
}

void ClockProCache::ref(ClockProHandle *e) { e->refs++; }

void ClockProCache::unref(ClockProHandle *e) {
    assert(e->refs > 0);
    e->refs--;
    if (e->refs == 0) {
        assert(!e->in_cache);
        if (e->is_resident && e->deleter && e->value) { (*e->deleter)(e->key(), e->value); }
        free_entry(e);
    } else if (e->in_cache && e->refs == 1) {
        flush_deferred(e);
    }
}

bool ClockProCache::finish_erase(ClockProHandle *e) {
    if (e != nullptr) {
        if (e->is_resident) {
            usage_ -= e->charge;
            if (e->is_hot) { hot_usage_ -= e->charge; }
        }
        ring_remove(e);
        e->in_cache = false;
        unref(e);
    }
    return e != nullptr;
}

Cache *new_clock_pro_cache(size_t capacity, MapType mt) {
    return new ShardedCache<ClockProCache, ClockProHandle>(capacity, mt);
}

} // namespace ANCHORDB_NAMESPACE
