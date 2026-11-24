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

#include "anchordb/cache.h"
#include "util/coding.h"
#include <cstddef>
#include <cstdint>
#include <string>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

// Allocates one id from a shared Cache and renders it as a fixed 8-byte key
// prefix. Independent cache consumers that share a Cache use this so their key
// spaces never overlap.
class CacheKeyNamespace {
  public:
    static constexpr size_t kPrefixSize = sizeof(uint64_t);

    // `cache` may be null; a null cache yields id 0 and callers treat that as
    // "no namespace" while caching is disabled.
    explicit CacheKeyNamespace(Cache *cache) : id_(cache != nullptr ? cache->new_id() : 0) {}

    uint64_t id() const { return id_; }

    void append_prefix(std::string *out) const {
        char buf[kPrefixSize];
        write_prefix(buf);
        out->append(buf, kPrefixSize);
    }

    void write_prefix(char *dest) const { encode_fixed_64(dest, id_); }

  private:
    uint64_t id_;
};

} // namespace ANCHORDB_NAMESPACE
