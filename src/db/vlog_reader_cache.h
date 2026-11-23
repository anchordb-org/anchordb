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
#include "anchordb/compressor.h"
#include "anchordb/options.h"
#include "anchordb/status.h"
#include "cache/cache_wrapper.h"
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ANCHORDB_NAMESPACE {

class Env;
class RandomAccessFileReader;

// Approximate cache charge of one open vLog reader: the reader object plus the
// kernel file handle and read-ahead state it keeps alive.
inline constexpr size_t kVLogReaderCacheCharge = 8 * 1024;

// One open vLog file plus the format metadata read from its header.  The
// header is read once when the file is opened (cache miss); every later record
// resolution reuses the resident codec and reads only the record bytes.
struct VLogFile {
    RandomAccessFileReader *reader = nullptr;
    uint32_t format_version = 0;
    // Codec id recorded in the vLog file header (0 = blobs stored verbatim).
    uint32_t codec_id = 0;
    // Resolved implementation of codec_id; disabled when codec_id is 0.
    Compressor compressor;
};

// Default capacity of the vLog reader cache.  At kVLogReaderCacheCharge per
// entry this keeps roughly 1024 readers open.
inline constexpr size_t kDefaultVLogReaderCacheSize = 8 * 1024 * 1024;

// Caches open VLogFile objects (reader plus header metadata) for vLog files by
// file name, so repeated blob reads reuse the descriptor, reader state, and
// codec instead of opening and re-reading the file header again.
//
// The backing Cache is injected and owned through a CacheWrapper.  Pass any
// Cache implementation (a built-in policy from new_cache(...) or a custom
// subclass of Cache) to plug in a replacement policy; this is the extension
// point for open-world use.  A reader is kept alive while a returned pin is
// outstanding and is deleted through the cache deleter once the last pin is
// released.
//
// Ownership: not reference counted.  It is an implementation detail of
// BlobResolver, which owns the single instance and must outlive every read.
//
// Not copyable.  The wrapper is internally synchronized by the Cache, but a
// single Handle must be released on the cache it came from.
class VLogReaderCache {
  public:
    // Takes ownership of `cache`; `cache` must be non-null until destruction.
    VLogReaderCache(Env *env, Cache *cache);

    VLogReaderCache(const VLogReaderCache &) = delete;
    VLogReaderCache &operator=(const VLogReaderCache &) = delete;

    // Returns a pinned open VLogFile for `filename`, opening it, validating its
    // header, and caching it on a miss.  The pin keeps the file alive and must
    // outlive every use of it.
    Status acquire(std::string_view filename, Cache::Handle *pin);

    // Removes `filename` from the cache.  The reader stays alive until any
    // outstanding read finishes.
    void erase(std::string_view filename);

    bool valid() const { return cache_.valid(); }

    size_t total_charge() const { return cache_.valid() ? cache_.total_charge() : 0; }

  private:
    // Cache deleter for a VLogFile* value.
    static void delete_entry(std::string_view key, void *value);

    Env *env_;
    CacheWrapper cache_;
};

} // namespace ANCHORDB_NAMESPACE
