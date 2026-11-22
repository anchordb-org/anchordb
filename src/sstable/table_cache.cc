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
#include "db/entry_iterator.h"
#include "sstable/table_cache.h"
#include "sstable/table_file_set.h"
#include "util/coding.h"
#include <string>
#include <string_view>
#include <utility>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

TableCache::TableCache(Env *env, std::string db_dir, const Comparator &comparator, const Compressor &compressor,
                       const Filter &filter, Cache *table_cache)
    : env_(env), comparator_(comparator), compressor_(compressor), filter_(filter), db_dir_(std::move(db_dir)),
      cache_(table_cache), namespace_id_(cache_.valid() ? cache_.new_id() : 0) {}

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

Status TableCache::find_table(uint64_t table_number, Cache::PinnedHandle *pin, TableReader **reader) {
    if (pin == nullptr || reader == nullptr) { return Status::invalid_argument("TableCache::find_table: null output"); }
    if (!cache_.valid()) { return Status::invalid_argument("TableCache::find_table: no cache"); }

    const std::string key = cache_key(table_number);
    auto open = [this, table_number](TableReader **out) {
        return TableReader::open(env_, comparator_, compressor_, filter_, make_table_file_set(db_dir_, table_number),
                                 out);
    };
    auto charge = [](TableReader *opened) { return opened->cache_charge(); };

    return cache_lookup_or_open<TableReader>(cache_, key, pin, reader, open, charge, &delete_entry);
}

Status TableCache::get_raw(uint64_t table_number, std::string_view key, ValueRef *out, std::string *scratch) {
    Cache::PinnedHandle pin;
    TableReader *reader = nullptr;
    Status s = find_table(table_number, &pin, &reader);
    if (!s.is_ok()) { return s; }

    return reader->get_raw(key, out, scratch);
}

Status TableCache::new_iterator(uint64_t table_number, EntryIterator **iter) {
    if (iter == nullptr) { return Status::invalid_argument("TableCache::new_iterator: null iterator output"); }

    Cache::PinnedHandle pin;
    TableReader *reader = nullptr;
    Status s = find_table(table_number, &pin, &reader);
    if (!s.is_ok()) { return s; }

    // The iterator holds its own reader reference, so the pin can be dropped.
    *iter = reader->new_iterator();
    return Status::ok();
}

Status TableCache::erase(uint64_t table_number) {
    if (!cache_.valid()) { return Status::invalid_argument("TableCache::erase: no cache"); }
    cache_.erase(cache_key(table_number));
    return Status::ok();
}

} // namespace ANCHORDB_NAMESPACE
