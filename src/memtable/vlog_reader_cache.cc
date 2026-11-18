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

#include "memtable/vlog_reader_cache.h"

#include "anchordb/cache.h"
#include "anchordb/env.h"
#include "anchordb/status.h"
#include <string>
#include <string_view>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

namespace {

Cache *make_cache(CacheType cache_type, size_t capacity, MapType map_type) {
    switch (cache_type) {
    case CacheType::LIRS: return new_lirs_cache(capacity, map_type);
    case CacheType::ClockPro: return new_clock_pro_cache(capacity, map_type);
    case CacheType::Arc: return new_arc_cache(capacity, map_type);
    case CacheType::LRU:
    default: return new_lru_cache(capacity, map_type);
    }
}

} // namespace

VLogReaderCache::VLogReaderCache(Env *env, size_t capacity, CacheType cache_type, MapType map_type)
    : env_(env), cache_(make_cache(cache_type, capacity, map_type)) {}
Status VLogReaderCache::read(std::string_view filename, uint64_t offset, size_t n, std::string_view *result,
                             char *scratch) {
    Cache::Handle *handle = nullptr;
    RandomAccessFileReader *reader = nullptr;
    Status s = find_reader_(filename, &handle, &reader);
    if (!s.is_ok()) { return s; }

    s = reader->read(offset, n, result, scratch);
    release_(handle);
    return s;
}

Status VLogReaderCache::find_reader_(std::string_view filename, Cache::Handle **handle,
                                     RandomAccessFileReader **reader) {
    if (handle == nullptr || reader == nullptr) { return Status::invalid_argument("vlog reader cache: null output"); }
    if (env_ == nullptr) { return Status::not_supported("vlog reader cache: no env"); }

    Cache::Handle *cached = cache_.lookup(filename);
    if (cached != nullptr) {
        *handle = cached;
        *reader = static_cast<RandomAccessFileReader *>(cache_.value(cached));
        return Status::ok();
    }

    std::string name(filename);
    RandomAccessFileReader *opened = nullptr;
    Status s = env_->new_random_access_file_reader(name, &opened);
    if (!s.is_ok()) { return s; }

    Cache::Handle *inserted = cache_.insert(name, opened, kVLogReaderCacheCharge, &delete_entry);
    if (inserted == nullptr) {
        delete opened;
        return Status::io_error("vlog reader cache: insert failed", name);
    }

    *handle = inserted;
    *reader = opened;
    return Status::ok();
}

void VLogReaderCache::release_(Cache::Handle *handle) {
    if (handle != nullptr) { cache_.release(handle); }
}
void VLogReaderCache::erase(std::string_view filename) { cache_.erase(filename); }

void VLogReaderCache::delete_entry(std::string_view, void *value) {
    delete static_cast<RandomAccessFileReader *>(value);
}

} // namespace ANCHORDB_NAMESPACE
