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

#include "sstable/table_cache.h"
#include "anchordb/cache.h"
#include "anchordb/status.h"
#include "sstable/table_file_set.h"
#include "util/coding.h"
#include <string>
#include <string_view>
#include <utility>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

TableCache::TableCache(const Options &options, std::string db_dir)
    : options_(options), db_dir_(std::move(db_dir)),
      vlog_cache_(new VLogReaderCache(options_.env, options_.vlog_reader_cache_size, options_.vlog_reader_cache_type,
                                      options_.active_map)),
      cache_(new_cache(options_.table_cache_type, options_.table_cache_size, options_.active_map)),
      namespace_id_(cache_.new_id()) {}

TableCache::~TableCache() {
    if (vlog_cache_ != nullptr) { vlog_cache_->unref(); }
}

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

Status TableCache::find_table(uint64_t table_number, CachePin<TableReader> *pin, TableReader **reader) {
    if (pin == nullptr || reader == nullptr) { return Status::invalid_argument("TableCache::find_table: null output"); }

    const std::string key = cache_key(table_number);
    auto open = [this, table_number](TableReader **out) {
        return TableReader::open(options_, make_table_file_set(db_dir_, table_number), db_dir_, vlog_cache_, out);
    };
    auto charge = [](TableReader *opened) { return opened->cache_charge(); };

    return cache_lookup_or_open<TableReader>(cache_, key, pin, reader, open, charge, &delete_entry);
}

Status TableCache::get(const ReadOptions &options, uint64_t table_number, std::string_view key, std::string *value,
                       std::string *scratch) {
    CachePin<TableReader> pin;
    TableReader *reader = nullptr;
    Status s = find_table(table_number, &pin, &reader);
    if (!s.is_ok()) { return s; }

    Status result = reader->get(options, key, value, scratch);

    // A valid pin released by its destructor may delete the reader, so this
    // must be the last use.
    if (!pin.valid()) { reader->unref(); }
    return result;
}

Status TableCache::new_iterator(const ReadOptions &options, uint64_t table_number, Iterator **iter) {
    if (iter == nullptr) { return Status::invalid_argument("TableCache::new_iterator: null iterator output"); }

    CachePin<TableReader> pin;
    TableReader *reader = nullptr;
    Status s = find_table(table_number, &pin, &reader);
    if (!s.is_ok()) { return s; }

    *iter = reader->new_iterator(options);

    // The iterator holds its own reader reference, so the cache pin (or the
    // caller's reference when the cache declined the entry) can be dropped.
    if (!pin.valid()) { reader->unref(); }
    return Status::ok();
}

Status TableCache::erase(uint64_t table_number) {
    cache_.erase(cache_key(table_number));
    return Status::ok();
}

} // namespace ANCHORDB_NAMESPACE
