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
#include "anchordb/options.h"
#include "cache/util.h"
#include "util/hash.h"
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

// Splits one replacement policy into kNumShards shards, picking the shard
// from the top hash bits. Each policy has to provide the same small per-shard
// interface over its own entry type:
//
//   Entry *insert(std::string_view, uint64_t, void *, size_t, CacheDeleter);
//   Entry *lookup(std::string_view, uint64_t);
//   void   release(Entry *);
//   void   erase(std::string_view, uint64_t);
//   void   prune();
//   size_t total_charge() const;
//
// That way the sharded Cache facade lives here once, not in every policy.
template <typename Shard, typename Entry> class ShardedCache final : public Cache {
  public:
    explicit ShardedCache(size_t capacity, MapType t) {
        const size_t per_shard = (capacity + (kNumShards - 1)) / kNumShards;
        shard_.reserve(kNumShards);
        for (int s = 0; s < kNumShards; ++s) { shard_.emplace_back(per_shard, t); }
    }

    ~ShardedCache() override = default;

    Handle insert(std::string_view key, void *value, size_t charge,
                  void (*deleter)(std::string_view key, void *value)) override {
        const uint64_t hash = hash_slice(key);
        Entry *e = shard_[shard_of(hash)].insert(key, hash, value, charge, deleter);
        return e != nullptr ? make_handle(e) : Handle();
    }

    Handle lookup(std::string_view key) override {
        const uint64_t hash = hash_slice(key);
        Entry *e = shard_[shard_of(hash)].lookup(key, hash);
        return e != nullptr ? make_handle(e) : Handle();
    }

    void erase(std::string_view key) override {
        const uint64_t hash = hash_slice(key);
        shard_[shard_of(hash)].erase(key, hash);
    }

    void prune() override {
        for (Shard &s : shard_) { s.prune(); }
    }

    size_t total_charge() const override {
        size_t total = 0;
        for (const Shard &s : shard_) { total += s.total_charge(); }
        return total;
    }

  private:
    void release_(void *entry) override {
        Entry *e = static_cast<Entry *>(entry);
        shard_[shard_of(e->hash)].release(e);
    }

    void *value_(void *entry) override { return static_cast<Entry *>(entry)->value; }

    static uint64_t hash_slice(std::string_view key) { return xx_hash64(key); }

    static size_t shard_of(uint64_t hash) { return hash >> (64 - kNumShardBits); }

    std::vector<Shard> shard_;
};

} // namespace ANCHORDB_NAMESPACE
