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
#include "sstable/table_reader.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ANCHORDB_NAMESPACE {

// Caches open TableReaders so repeated reads of the same table number avoid
// re-opening files and re-parsing the summary.  The backing Cache is chosen by
// Options::table_cache_type/size and owned through a CacheWrapper.
//
// Captured Options are forwarded unchanged to TableReader::open, so cached
// readers use the same comparator, compressor and filter the writer used.  The
// resident summary and filter are charged to the cache; index blocks are read
// and decompressed per lookup.
//
// A cached TableReader is kept alive while a lookup handle is outstanding, so
// the cache may evict and delete it as soon as get() releases its handle.
class TableCache {
  public:
    TableCache(const Options &options, std::string db_dir);
    ~TableCache();

    TableCache(const TableCache &) = delete;
    TableCache &operator=(const TableCache &) = delete;

    // Point lookup against table `table_number`, opening and caching the table
    // on first use.
    Status get(const ReadOptions &options, uint64_t table_number, std::string_view key, std::string *value,
               std::string *scratch);

    // Returns a heap-allocated iterator over `table_number` (caller deletes).
    // The iterator keeps its table reader alive independently of the cache.
    Status new_iterator(const ReadOptions &options, uint64_t table_number, Iterator **iter);

    // Drops the cached entry for `table_number` (e.g. after compaction).
    Status erase(uint64_t table_number);

    size_t total_charge() const { return cache_.total_charge(); }
    uint64_t namespace_id() const { return namespace_id_; }

  private:
    // Looks up or opens the table.  On success *reader is usable and *handle is
    // the cache handle pinning it; release the handle when done.  A null handle
    // means the cache did not take ownership, so the caller must delete the
    // reader.
    Status find_table(uint64_t table_number, Cache::Handle **handle, TableReader **reader);

    // Builds the namespaced cache key for a table number.
    std::string cache_key(uint64_t table_number) const;

    // Cache deleter for a TableReader* cache value.
    static void delete_entry(std::string_view key, void *value);

    Options options_;
    std::string db_dir_;
    CacheWrapper cache_;
    uint64_t namespace_id_;
};

} // namespace ANCHORDB_NAMESPACE
