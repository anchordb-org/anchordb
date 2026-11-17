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

#include "anchordb/cache.h"
#include "anchordb/status.h"
#include "sstable/table_cache.h"
#include "sstable/table_file_set.h"
#include "util/coding.h"
#include <string>
#include <string_view>
#include <utility>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

namespace {

// Builds the concrete Cache implementation selected by the options.
Cache *make_cache(const Options &options) {
    switch (options.table_cache_type) {
    case CacheType::LIRS: return new_lirs_cache(options.table_cache_size, options.active_map);
    case CacheType::ClockPro: return new_clock_pro_cache(options.table_cache_size, options.active_map);
    case CacheType::Arc: return new_arc_cache(options.table_cache_size, options.active_map);
    case CacheType::LRU:
    default: return new_lru_cache(options.table_cache_size, options.active_map);
    }
}

} // namespace

TableCache::TableCache(const Options &options, std::string db_dir)
    : options_(options), db_dir_(std::move(db_dir)), cache_(make_cache(options_)), namespace_id_(cache_.new_id()) {}

TableCache::~TableCache() = default;

std::string TableCache::cache_key(uint64_t table_number) const {
    std::string key;
    key.reserve(2 * sizeof(uint64_t));
    char buf[sizeof(uint64_t)];
    encode_fixed_64(buf, namespace_id_);
    key.append(buf, sizeof(buf));
    encode_fixed_64(buf, table_number);
    key.append(buf, sizeof(buf));
    return key;
}

void TableCache::delete_entry(std::string_view, void *value) { static_cast<TableReader *>(value)->unref(); }

Status TableCache::find_table(uint64_t table_number, Cache::Handle **handle, TableReader **reader) {
    if (handle == nullptr || reader == nullptr) {
        return Status::invalid_argument("TableCache::find_table: null output");
    }

    const std::string key = cache_key(table_number);
    Cache::Handle *cached = cache_.lookup(key);
    if (cached != nullptr) {
        *handle = cached;
        *reader = static_cast<TableReader *>(cache_.value(cached));
        return Status::ok();
    }

    const TableFileSet files = make_table_file_set(db_dir_, table_number);
    TableReader *opened = nullptr;
    Status s = TableReader::open(options_, files, db_dir_, &opened);
    if (!s.is_ok()) { return s; }

    Cache::Handle *inserted = cache_.insert(key, opened, opened->cache_charge(), &delete_entry);
    // A cache implementation that declines the entry returns no handle; the
    // caller then owns `opened` through the null-handle convention.
    *handle = inserted;
    *reader = opened;
    return Status::ok();
}

Status TableCache::get(const ReadOptions &options, uint64_t table_number, std::string_view key, std::string *value,
                       std::string *scratch) {
    Cache::Handle *handle = nullptr;
    TableReader *reader = nullptr;
    Status s = find_table(table_number, &handle, &reader);
    if (!s.is_ok()) { return s; }

    Status result = reader->get(options, key, value, scratch);

    // Releasing the handle may delete the reader, so it must be the last use.
    if (handle != nullptr) {
        cache_.release(handle);
    } else {
        reader->unref();
    }
    return result;
}

Status TableCache::new_iterator(const ReadOptions &options, uint64_t table_number, Iterator **iter) {
    if (iter == nullptr) { return Status::invalid_argument("TableCache::new_iterator: null iterator output"); }

    Cache::Handle *handle = nullptr;
    TableReader *reader = nullptr;
    Status s = find_table(table_number, &handle, &reader);
    if (!s.is_ok()) { return s; }

    *iter = reader->new_iterator(options);

    // The iterator holds its own reader reference, so the cache pin (or the
    // caller's reference when the cache declined the entry) can be dropped.
    if (handle != nullptr) {
        cache_.release(handle);
    } else {
        reader->unref();
    }
    return Status::ok();
}

Status TableCache::erase(uint64_t table_number) {
    cache_.erase(cache_key(table_number));
    return Status::ok();
}

} // namespace ANCHORDB_NAMESPACE
