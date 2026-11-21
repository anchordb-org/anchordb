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

#include "anchordb/export.h"
#include "anchordb/options.h"
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

// Abstract in-memory block cache.
//
// A Cache maps keys to caller-owned values and tracks a single `charge` per
// entry (typically the entry's approximate size in bytes) so implementations
// can bound total memory.  Values are opaque to the cache: it never inspects or
// frees them directly, calling the supplied `deleter` instead.
//
// Entry lifetime: insert()/lookup() return a PinnedHandle that owns the entry
// for its scope.  An entry is only reclaimed by the cache (or by erase()) after
// every outstanding PinnedHandle has been destroyed, so callers never call
// release() themselves.
//
// Thread-safety: implementations are internally synchronized; a single Cache
// may be shared across threads.  A given PinnedHandle, however, is not
// thread-safe and must be destroyed on the same Cache it came from.
//
// Obtain an instance via new_lru_cache(), new_lirs_cache() or
// new_clock_pro_cache().
class ANCHORDB_EXPORT Cache {
  public:
    Cache() = default;

    Cache(const Cache &) = delete;
    Cache &operator=(const Cache &) = delete;

    virtual ~Cache();

    // Opaque, implementation-defined token identifying one cached entry.  Treat
    // it as an opaque pointer; its layout is private to the implementation.
    struct Handle {};

    // Owning, move-only view of one cache entry.  The entry stays resident
    // until every PinnedHandle on it is destroyed; the destructor releases the
    // entry through the cache, so clients never call release().  An empty
    // handle means the lookup missed.
    class PinnedHandle {
      public:
        PinnedHandle() = default;
        PinnedHandle(PinnedHandle &&other) noexcept;
        PinnedHandle &operator=(PinnedHandle &&other) noexcept;
        ~PinnedHandle();

        PinnedHandle(const PinnedHandle &) = delete;
        PinnedHandle &operator=(const PinnedHandle &) = delete;

        // The pinned value, or nullptr when empty.
        void *value() const;
        explicit operator bool() const { return handle_ != nullptr; }

        // Releases the entry.  Safe to call on an empty handle.
        void reset();

      private:
        friend class Cache;
        PinnedHandle(Cache *cache, Handle *handle);

        Cache *cache_ = nullptr;
        Handle *handle_ = nullptr;
    };

    // Inserts the mapping key -> value, charging `charge` against the cache's
    // capacity, and returns a handle pinning the entry.
    //
    // If an entry for `key` already exists it is replaced.  `deleter` is
    // invoked with the key and value when the entry is finally evicted, erased
    // or destroyed; it is responsible for freeing `value`.
    virtual PinnedHandle insert(std::string_view key, void *value, size_t charge,
                                void (*deleter)(std::string_view key, void *value)) = 0;

    // Returns a handle pinning the cached mapping of `key`, or an empty handle
    // on a miss.
    virtual PinnedHandle lookup(std::string_view key) = 0;

    // Removes the mapping for `key`, if present.  The entry itself is kept
    // alive until every outstanding PinnedHandle to it has been destroyed.
    virtual void erase(std::string_view key) = 0;

    // Returns a fresh numeric id, unique within this cache instance.  Clients
    // that share one cache can partition the key space by allocating an id up
    // front and prefixing it to their keys.
    virtual uint64_t new_id() = 0;

    // Discards entries that are not currently pinned by a handle, reducing
    // memory usage on memory-constrained machines.
    virtual void prune() = 0;

    // Returns the combined charge of all entries currently resident in the
    // cache.
    virtual size_t total_charge() const = 0;

  protected:
    // Wraps a raw handle produced by an implementation's shard in an owning
    // PinnedHandle.  Implementations call this from insert()/lookup().
    PinnedHandle make_pinned(Handle *handle) { return PinnedHandle(this, handle); }

  private:
    // Invoked by PinnedHandle.  Not part of the public interface: releasing the
    // last handle makes the entry reclaimable, and clients reach this only
    // through the handle's destructor/reset().
    virtual void release_(Handle *handle) = 0;
    virtual void *value_(Handle *handle) = 0;
};

inline Cache::PinnedHandle::PinnedHandle(Cache *cache, Handle *handle) : cache_(cache), handle_(handle) {}

inline Cache::PinnedHandle::PinnedHandle(PinnedHandle &&other) noexcept : cache_(other.cache_), handle_(other.handle_) {
    other.cache_ = nullptr;
    other.handle_ = nullptr;
}

inline Cache::PinnedHandle &Cache::PinnedHandle::operator=(PinnedHandle &&other) noexcept {
    if (this != &other) {
        reset();
        cache_ = other.cache_;
        handle_ = other.handle_;
        other.cache_ = nullptr;
        other.handle_ = nullptr;
    }
    return *this;
}

inline Cache::PinnedHandle::~PinnedHandle() { reset(); }

inline void *Cache::PinnedHandle::value() const { return handle_ != nullptr ? cache_->value_(handle_) : nullptr; }

inline void Cache::PinnedHandle::reset() {
    if (handle_ != nullptr) { cache_->release_(handle_); }
    cache_ = nullptr;
    handle_ = nullptr;
}

// Creates a cache using the replacement policy selected by `type`.  `capacity`
// bounds the sum of entry charges; `t` selects the hash-map backend used for
// the index.  This is the shared policy-dispatch used by the table and vLog
// reader caches.
ANCHORDB_EXPORT Cache *new_cache(CacheType type, size_t capacity, MapType t);

// Creates a cache using the LRU replacement policy.  `capacity` bounds the sum
// of entry charges; `t` selects the hash-map backend used for the index.
ANCHORDB_EXPORT Cache *new_lru_cache(size_t capacity, MapType t);
// Creates a cache using the LIRS replacement policy (see src/cache/lirs.h).
ANCHORDB_EXPORT Cache *new_lirs_cache(size_t capacity, MapType t);
// Creates a cache using the CLOCK-Pro replacement policy (see
// src/cache/clock_pro.h).
ANCHORDB_EXPORT Cache *new_clock_pro_cache(size_t capacity, MapType t);
// Creates a cache using the ARC replacement policy (see src/cache/arc.h).
ANCHORDB_EXPORT Cache *new_arc_cache(size_t capacity, MapType t);

} // namespace ANCHORDB_NAMESPACE
