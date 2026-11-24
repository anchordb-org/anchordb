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

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <string_view>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

const int kNumShardBits = 4;
const int kNumShards = 1 << kNumShardBits;

// Frees a cached value; the cache calls this once the entry is reclaimed.
using CacheDeleter = void (*)(std::string_view key, void *value);

// A key paired with its precomputed 64-bit hash, passed by value throughout the
// cache maps and lists.
struct HashedKey {
    std::string_view key;
    uint64_t hash;
};

// A value replaced in place while its entry is still pinned by a Handle.  It is
// retained until the last Handle is released so callers holding the old pointer
// never observe freed memory.
struct DeferredValue {
    CacheDeleter deleter{nullptr};
    void *value{nullptr};
    DeferredValue *next{nullptr};
};

// Metadata shared by every policy entry. The key itself lives in the tail of
// the concrete entry allocation and is referenced through `key_view`.
struct EntryBase {
    void *value{nullptr};
    CacheDeleter deleter{nullptr};

    uint32_t refs{0};
    bool in_cache{false}; // True if referenced in map / retrievable
    size_t charge{0};
    uint64_t hash{0};

    DeferredValue *deferred{nullptr};
    std::string_view key_view{};

    std::string_view key() const { return key_view; }
};

// Frees any values deferred while the entry was pinned.  Safe to call with an
// empty list.  Callers invoke this only once no Handle can still reach the
// replaced values (cache-only or fully-unreferenced entry).
inline void flush_deferred(EntryBase *e) {
    DeferredValue *d = e->deferred;
    while (d != nullptr) {
        DeferredValue *next = d->next;
        if (d->deleter != nullptr) { d->deleter(e->key(), d->value); }
        delete d;
        d = next;
    }
    e->deferred = nullptr;
}

// Installs a new value, preserving the previous one until it is safe to free.
// If the entry is pinned, the old value is deferred; otherwise it is deleted
// immediately as before.
inline void install_value(EntryBase *e, void *value, CacheDeleter deleter) {
    if (e->value != nullptr && e->deleter != nullptr) {
        if (e->refs > 1) {
            DeferredValue *d = new (std::nothrow) DeferredValue{e->deleter, e->value, e->deferred};
            if (d != nullptr) {
                e->deferred = d;
            } else {
                (*e->deleter)(e->key(), e->value);
            }
        } else {
            (*e->deleter)(e->key(), e->value);
        }
    }
    e->value = value;
    e->deleter = deleter;
}

// Allocates an entry with room for `key` appended after the object, then points
// its `key_view` at that tail.  Returns nullptr on overflow or allocation
// failure.  Value-initialisation applies the entry's default member
// initialisers.
template <typename Entry> Entry *allocate_entry(std::string_view key) {
    if (key.size() > std::numeric_limits<size_t>::max() - sizeof(Entry)) { return nullptr; }
    void *raw = std::malloc(sizeof(Entry) + key.size());
    if (raw == nullptr) { return nullptr; }

    Entry *e = new (raw) Entry();
    char *key_bytes = static_cast<char *>(raw) + sizeof(Entry);
    if (!key.empty()) { std::memcpy(key_bytes, key.data(), key.size()); }
    e->key_view = std::string_view(key_bytes, key.size());
    return e;
}

// Releases an entry, freeing any deferred values first.
template <typename Entry> void free_entry(Entry *e) {
    flush_deferred(e);
    e->~Entry();
    std::free(e);
}

struct LRUHandle : EntryBase {};

struct LIRSHandle : EntryBase {
    bool is_resident{false}; // True if holds value & consumes charge
    bool is_lir{false};      // True = LIR block, False = HIR block
    bool in_stack{false};    // True if in S stack
    bool in_hir_list{false}; // True if in Q list (resident HIRs)
};

struct ClockProHandle : EntryBase {
    ClockProHandle *prev{nullptr};
    ClockProHandle *next{nullptr};

    bool is_resident{false}; // True if holding allocated payload
    bool is_hot{false};      // True = Hot item, False = Cold item
    bool ref_bit{false};     // Second-chance bit
};

struct ARCHandle : EntryBase {
    // Which ARC list the handle currently belongs to (T1/T2 resident, B1/B2
    // ghost).
    enum class State : uint8_t { kT1, kT2, kB1, kB2 };
    State state{State::kT1};
};

} // namespace ANCHORDB_NAMESPACE
