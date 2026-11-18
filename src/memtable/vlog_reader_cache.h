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
#include "anchordb/options.h"
#include "anchordb/status.h"
#include "cache/cache_wrapper.h"
#include <cstddef>
#include <string_view>

namespace ANCHORDB_NAMESPACE {

class Env;
class RandomAccessFileReader;

// Approximate cache charge of one open vLog reader: the reader object plus the
// kernel file handle and read-ahead state it keeps alive.
inline constexpr size_t kVLogReaderCacheCharge = 8 * 1024;

// Default capacity of the vLog reader cache.  At kVLogReaderCacheCharge per
// entry this keeps roughly 1024 readers open.
inline constexpr size_t kDefaultVLogReaderCacheSize = 8 * 1024 * 1024;

// Caches open RandomAccessFileReader objects for vLog files by file name, so
// repeated blob reads reuse the descriptor and reader state instead of opening
// the file again.
//
// The backing Cache is owned through a CacheWrapper; the replacement policy and
// hash-map backend are selected the same way as the table cache.  A reader is
// kept alive while a returned handle is outstanding and is deleted through the
// cache deleter once the last handle is released.
//
// Not copyable.  The wrapper is internally synchronized by the Cache, but a
// single Cache::Handle must be released on the cache it came from.
class VLogReaderCache {
  public:
    explicit VLogReaderCache(Env *env, size_t capacity = kDefaultVLogReaderCacheSize,
                             CacheType cache_type = CacheType::LRU, MapType map_type = MapType::Swiss);

    VLogReaderCache(const VLogReaderCache &) = delete;
    VLogReaderCache &operator=(const VLogReaderCache &) = delete;

    // Reads `n` bytes at `offset` from `filename`, reusing a cached reader when
    // one is already open and opening/caching it on a miss.  `*result` points
    // into `scratch` and follows RandomAccessFileReader::read's contract.
    Status read(std::string_view filename, uint64_t offset, size_t n, std::string_view *result, char *scratch);

    // Removes `filename` from the cache.  The reader stays alive until any
    // outstanding read finishes.
    void erase(std::string_view filename);

    size_t total_charge() const { return cache_.total_charge(); }

  private:
    // Looks up `filename`, opening and caching it on a miss.  On success *reader
    // is pinned by *handle; release_() must be called when done.
    Status find_reader_(std::string_view filename, Cache::Handle **handle, RandomAccessFileReader **reader);

    // Releases a handle returned by find_reader_().
    void release_(Cache::Handle *handle);

    // Cache deleter for a RandomAccessFileReader* value.
    static void delete_entry(std::string_view key, void *value);

    Env *env_;
    CacheWrapper cache_;
};

} // namespace ANCHORDB_NAMESPACE
