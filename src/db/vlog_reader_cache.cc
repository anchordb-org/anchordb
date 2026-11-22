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
#include <string>
#include <string_view>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

VLogReaderCache::VLogReaderCache(Env *env, Cache *cache) : env_(env), cache_(cache) {}

Status VLogReaderCache::open(std::string_view filename, Cache::PinnedHandle *pin) {
    if (pin == nullptr) { return Status::invalid_argument("vlog reader cache: null pin"); }
    if (env_ == nullptr) { return Status::not_supported("vlog reader cache: no env"); }
    if (!cache_.valid()) { return Status::invalid_argument("vlog reader cache: no cache"); }

    auto open = [this, filename](RandomAccessFileReader **out) {
        return env_->new_random_access_file_reader(std::string(filename), out);
    };
    auto charge = [](RandomAccessFileReader *) { return kVLogReaderCacheCharge; };

    return cache_lookup_or_open<RandomAccessFileReader>(cache_, filename, pin, /*value=*/nullptr, open, charge,
                                                        &delete_entry);
}

void VLogReaderCache::erase(std::string_view filename) {
    if (cache_.valid()) { cache_.erase(filename); }
}

void VLogReaderCache::delete_entry(std::string_view, void *value) {
    delete static_cast<RandomAccessFileReader *>(value);
}

} // namespace ANCHORDB_NAMESPACE
