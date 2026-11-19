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

#include "db/vlog_reader_cache.h"

#include "anchordb/env.h"
#include "anchordb/status.h"
#include <cassert>
#include <string>
#include <string_view>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

VLogReaderCache::VLogReaderCache(Env *env, size_t capacity, CacheType cache_type, MapType map_type)
    : env_(env), cache_(new_cache(cache_type, capacity, map_type)) {}

void VLogReaderCache::unref() {
    const int old = refs_.fetch_sub(1, std::memory_order_acq_rel);
    assert(old > 0);
    if (old == 1) { delete this; }
}

Status VLogReaderCache::read(std::string_view filename, uint64_t offset, size_t n, std::string_view *result,
                             char *scratch) {
    CachePin<RandomAccessFileReader> pin;
    Status s = open_reader(filename, &pin);
    if (!s.is_ok()) { return s; }
    return pin->read(offset, n, result, scratch);
}

Status VLogReaderCache::open_reader(std::string_view filename, CachePin<RandomAccessFileReader> *pin) {
    if (pin == nullptr) { return Status::invalid_argument("vlog reader cache: null pin"); }
    if (env_ == nullptr) { return Status::not_supported("vlog reader cache: no env"); }

    RandomAccessFileReader *opened = nullptr;
    auto open = [this, filename](RandomAccessFileReader **out) {
        return env_->new_random_access_file_reader(std::string(filename), out);
    };
    auto charge = [](RandomAccessFileReader *) { return kVLogReaderCacheCharge; };

    Status s =
        cache_lookup_or_open<RandomAccessFileReader>(cache_, filename, pin, &opened, open, charge, &delete_entry);
    if (!s.is_ok()) { return s; }
    if (!pin->valid()) {
        delete opened;
        return Status::io_error("vlog reader cache: insert failed", std::string(filename));
    }
    return Status::ok();
}

void VLogReaderCache::erase(std::string_view filename) { cache_.erase(filename); }

void VLogReaderCache::delete_entry(std::string_view, void *value) {
    delete static_cast<RandomAccessFileReader *>(value);
}

} // namespace ANCHORDB_NAMESPACE
