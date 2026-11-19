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

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

Cache::~Cache() = default;

Cache *new_cache(CacheType type, size_t capacity, MapType t) {
    switch (type) {
    case CacheType::LIRS: return new_lirs_cache(capacity, t);
    case CacheType::ClockPro: return new_clock_pro_cache(capacity, t);
    case CacheType::Arc: return new_arc_cache(capacity, t);
    case CacheType::LRU:
    default: return new_lru_cache(capacity, t);
    }
}

} // namespace ANCHORDB_NAMESPACE
