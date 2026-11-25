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
#include "anchordb/compressor.h"
#include "anchordb/status.h"
#include "cache/cache_key.h"
#include "sstable/data_block.h"
#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>

namespace ANCHORDB_NAMESPACE {

class RandomAccessFileReader;

// One decompressed and parsed -Data.adb block, the value stored in the cache.
// `bytes` owns the uncompressed block and `reader` parses records from it; the
// views the reader returns point into `bytes`, so an entry must not be moved or
// mutated after construction.  It is always heap-allocated and freed only by
// delete_entry() once the cache evicts or erases it and every pin is released.
struct CachedDataBlock {
    std::string bytes;
    DataBlockReader *reader = nullptr;
};

// Caches decompressed -Data.adb blocks for one table.  This is the single place
// that reads, decompresses and parses an inline block, so point lookups and
// iterators share both the code path and the cached result.
//
// The backing Cache is borrowed and must be non-null.  A zero-capacity Cache is
// still usable: the cache declines every insertion and each returned pin keeps
// the freshly loaded block alive, so blocks are reloaded per call while the
// ownership contract stays identical.  A non-null Cache may be shared by many
// BlockCache instances: the constructor allocates a private namespace id so
// their keys never collide.
//
// Ownership follows TableCache and VLogReaderCache: the cache owns each
// CachedDataBlock and frees it through delete_entry().  A caller keeps a block
// alive by holding the returned pin (Cache::Handle), and must not use the block
// after releasing it.
//
// Not copyable: a BlockCache owns a key-namespace id and is stored by value in
// the reader that borrows the cache.
class BlockCache {
  public:
    // `cache` is borrowed and must outlive this object; it must not be null.
    explicit BlockCache(Cache *cache) : cache_(cache), key_namespace_(cache) {
        assert(cache != nullptr && "BlockCache requires a cache");
    }

    BlockCache(const BlockCache &) = delete;
    BlockCache &operator=(const BlockCache &) = delete;
    // Constructed empty, then filled by the owning reader with the cache it
    // borrows.  The backend is borrowed, so moving only transfers the key
    // namespace and never takes ownership of it.
    BlockCache(BlockCache &&) = default;
    BlockCache &operator=(BlockCache &&) = default;

    bool valid() const { return cache_ != nullptr; }

    // Returns the parsed block at (`block_offset`, `stored_size`) in the file
    // accessed by `reader`.  `file_size` bounds the read.  The block is
    // decompressed with `codec` when one is active, and every header/length
    // check performed here runs before any buffer is sized from untrusted data.
    //
    // On success `*pin` owns the block and `*block` points at it; the block
    // stays valid until `*pin` is released or reassigned.  Both outputs are
    // null on failure.  There is no valid success with an empty pin.
    Status get(RandomAccessFileReader *reader, uint64_t file_size, const Compressor &codec, uint64_t block_offset,
               uint32_t stored_size, Cache::Handle *pin, CachedDataBlock **block) const;

    // Size of a key built by make_key(), so callers can size a stack buffer.
    static constexpr size_t kKeySize = CacheKeyNamespace::kPrefixSize + sizeof(uint64_t) + sizeof(uint32_t);

    // Returns a pin on the block at (`block_offset`, `stored_size`) if it is
    // already resident, or an empty handle on a miss.  `stored_size` is part of
    // the key, so the caller must pass the length the slot recorded.
    //
    // This is a cache-only probe: it never reads or parses anything, which lets
    // a caller that already has the block's location resolve it before deciding
    // to perform I/O.
    Cache::Handle lookup(uint64_t block_offset, uint32_t stored_size) const {
        char key[kKeySize];
        encode_key(block_offset, stored_size, key);
        return cache_->lookup(std::string_view(key, sizeof(key)));
    }

    // Takes ownership of `block` and publishes it under `key`, which must have
    // come from make_key() for `block`'s own location.  Returns a pin on the
    // cache slot either way: if a concurrent lookup already inserted `key`,
    // `block` is deleted and the resident entry is pinned instead, so the newly
    // read bytes are never preferred over the ones already parsed.
    //
    // `block` must be a fully built entry (see BlockCache::get's load path); an
    // empty pin means the cache declined it and `block` was deleted.
    Cache::Handle adopt(std::string_view key, CachedDataBlock *block) const;

    // Builds a cache entry from an uncompressed block, exactly as get() would
    // after reading and decompressing it.  Returns null (with *s set) when the
    // bytes do not parse.  The caller owns the result and publishes it with
    // adopt(), which is how a caller that performed its own read still
    // populates the cache.
    static CachedDataBlock *make_block(std::string bytes, Status *s);

    // This cache's key for the block at (`block_offset`, `stored_size`), sized
    // kKeySize.
    void make_key(uint64_t block_offset, uint32_t stored_size, char *key) const {
        encode_key(block_offset, stored_size, key);
    }

  private:
    // Cache deleter for a CachedDataBlock* value.
    static void delete_entry(std::string_view key, void *value);

    // Frees one entry: the cache deleter and the load path share this so a
    // block is released identically whichever path created it.
    static void destroy_entry(CachedDataBlock *block);

    // Builds this cache's namespaced key for one block location.
    void encode_key(uint64_t block_offset, uint32_t stored_size, char *key) const;

    Cache *cache_;
    CacheKeyNamespace key_namespace_;
};

} // namespace ANCHORDB_NAMESPACE
