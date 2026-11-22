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
#include "anchordb/comparator.h"
#include "anchordb/compressor.h"
#include "anchordb/filter.h"
#include "anchordb/options.h"
#include "anchordb/status.h"
#include "cache/cache_wrapper.h"
#include "sstable/table_reader.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ANCHORDB_NAMESPACE {

class EntryIterator;

// Caches open TableReaders so repeated reads of the same table number avoid
// re-opening files and re-parsing the summary.  The backing Cache is injected
// and owned through a CacheWrapper: pass any Cache implementation (a built-in
// policy from new_cache(...) or a custom subclass) to plug in a replacement
// policy.
//
// `comparator`, `compressor` and `filter` are forwarded unchanged to
// TableReader::open, so cached readers use the same ordering and codecs the
// writer used.  The DB/orchestrator layer derives these (and the cache
// sizes/policies) from Options.  The resident summary and filter are charged to
// the cache; index blocks are read and decompressed per lookup.
//
// A cached TableReader is kept alive while a PinnedHandle on it is outstanding,
// so the cache may evict and delete it once the handle is released.
class TableCache {
  public:
    // Takes ownership of `table_cache`; it must be non-null until destruction.
    TableCache(Env *env, std::string db_dir, const Comparator &comparator, const Compressor &compressor,
               const Filter &filter, Cache *table_cache);

    TableCache(const TableCache &) = delete;
    TableCache &operator=(const TableCache &) = delete;

    // Point lookup against table `table_number`, opening and caching the table
    // on first use.  Fills *out with the unresolved version (a tombstone is
    // returned as an entry); the DB layer resolves any blob handle.
    Status get_raw(uint64_t table_number, std::string_view key, ValueRef *out, std::string *scratch);

    // Returns a heap-allocated unresolved entry iterator over `table_number`
    // (caller deletes).  The iterator keeps its table reader alive
    // independently of the cache.
    Status new_iterator(uint64_t table_number, EntryIterator **iter);

    // Drops the cached entry for `table_number` (e.g. after compaction).
    Status erase(uint64_t table_number);

    size_t total_charge() const { return cache_.valid() ? cache_.total_charge() : 0; }
    uint64_t namespace_id() const { return namespace_id_; }

  private:
    // Looks up or opens the table.  On success `*pin` owns the reader and
    // `*reader` points at it.
    Status find_table(uint64_t table_number, Cache::PinnedHandle *pin, TableReader **reader);

    // Builds the namespaced cache key for a table number.
    std::string cache_key(uint64_t table_number) const;

    // Cache deleter for a TableReader* cache value.
    static void delete_entry(std::string_view key, void *value);

    Env *env_;
    Comparator comparator_;
    Compressor compressor_;
    Filter filter_;
    std::string db_dir_;
    CacheWrapper cache_;
    uint64_t namespace_id_;
};

} // namespace ANCHORDB_NAMESPACE
