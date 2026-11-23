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

#include "anchordb/comparator.h"
#include "anchordb/compressor.h"
#include "anchordb/env.h"
#include "anchordb/export.h"
#include "anchordb/filter.h"
#include <cstddef>
#include <cstdint>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

class Cache;

// Default ceiling on the decompressed size of one vLog blob returned by the
// read path.  A corrupt compressed record can claim an enormous uncompressed
// length in its codec metadata, so the resolver refuses values larger than this
// before allocating.  Zero disables the limit.
inline constexpr size_t kDefaultMaxVLogValueSize = 1024ull * 1024 * 1024; // 1 GiB

// Selects the open-addressing hash-map backend used for cache indexes (see
// src/cache/).  It affects cache performance, not on-disk format.
enum class ANCHORDB_EXPORT MapType {
    RobbinHood = 0x0,    // Robin Hood probing
    LinearProbing = 0x1, // linear probing
    Swiss = 0x2,         // Swiss-table grouped probing (default)
};

// Selects the replacement policy of the open-table cache (see src/cache/).
enum class ANCHORDB_EXPORT CacheType {
    LRU = 0x0,      // least recently used
    LIRS = 0x1,     // low inter-reference recency set
    ClockPro = 0x2, // CLOCK-Pro
    Arc = 0x3,      // adaptive replacement cache
};

// Configuration for a database handle.  Default-constructed Options are
// usable as-is: every interface and enum has a working default.
//
// Several knobs come in pairs: an enum selects an implementation
// (`active_compression_type`, `data_compression_type`, `vlog_compression_type`,
// `active_filter_type`, `active_map`, `table_cache_type`) and a matching
// interface member holds the resolved implementation (`compressor`,
// `data_compressor`, `vlog_compressor`, `filter`).  Use
// new_compressor()/new_filter() to materialize an implementation from an enum
// value.
struct ANCHORDB_EXPORT Options {
    Options() = default;

    // Environment used for all I/O.  Defaults to the platform Env; the database
    // does not take ownership.
    Env *env = Env::default_env();
    // Optional diagnostics logger.  Not owned by the database.
    Logger *info_log = nullptr;

    // Key ordering.  Defaults to byte-wise comparison, and must match the
    // comparator the database was created with.
    Comparator comparator = new_byte_wise_comparator();

    // Block compression.  `active_compression_type` selects the codec and
    // `compressor` holds the resolved implementation (a null compressor stores
    // blocks verbatim); `compression_level` is passed through to the codec.
    CompressionType active_compression_type = CompressionType::None;
    Compressor compressor = Compressor{};
    int compression_level = 1;

    // -Data.adb block compression.  Records are grouped into whole-record data
    // blocks; the whole block is compressed and framed on disk.  `data_block_size`
    // is the uncompressed target (a block may exceed it by one record);
    // `data_compressor` holds the resolved implementation (assign
    // new_compressor(data_compression_type)).  A null compressor stores blocks
    // framed but uncompressed.
    CompressionType data_compression_type = CompressionType::None;
    Compressor data_compressor = Compressor{};
    int data_compression_level = 1;
    size_t data_block_size = 32 * 1024;

    // vLog (WiscKey value-log) record compression.  This is independent of the
    // block compression above: each vLog record's value is compressed
    // individually, because a vLog record is already large enough for a codec
    // to work well.  `vlog_compressor` holds the resolved implementation
    // (assign new_compressor(vlog_compression_type)); a null compressor or
    // CompressionType::None writes blob values verbatim as EntryType::Blob.
    // The writer emits EntryType::Compressed only when compression shrinks the
    // value, and records the codec id in every vLog file header so files stay
    // readable after this option changes.
    CompressionType vlog_compression_type = CompressionType::None;
    Compressor vlog_compressor = Compressor{};
    int vlog_compression_level = 1;
    // Values smaller than this are stored as uncompressed Blob records even
    // when vLog compression is enabled.  Zero attempts compression for every
    // record; the writer still falls back to Blob when the result is not
    // smaller.
    size_t vlog_compression_min_size = 0;
    // Upper bound on the decompressed size of a blob returned by the read
    // path.  The resolver rejects larger values before allocating, so corrupt
    // compression metadata cannot trigger an enormous allocation.  Zero
    // disables the limit.  Wire this into BlobResolver when constructing it.
    size_t max_vlog_value_size = kDefaultMaxVLogValueSize;

    // Per-table filter.  `active_filter_type` selects the algorithm and
    // `filter` holds the resolved implementation (a null filter disables
    // filtering).  Defaults to filtering disabled; assign
    // new_filter(active_filter_type) to enable it.
    FilterType active_filter_type = FilterType::Bloom;
    Filter filter = Filter{};

    // Hash-map backend used by the cache implementations.
    MapType active_map = MapType::Swiss;

    // Open-table cache: capacity in bytes and the replacement policy used to
    // implement it (see TableCache).
    size_t table_cache_size = 8 * 1024 * 1024;
    CacheType table_cache_type = CacheType::LRU;
    // Shared vLog reader cache: capacity in bytes and the replacement policy
    // used for the pool of open vLog file descriptors in the DB read path.
    size_t vlog_reader_cache_size = 8 * 1024 * 1024;
    CacheType vlog_reader_cache_type = CacheType::LRU;
    // Block cache for decompressed -Data.adb blocks, sized by
    // block_cache_size.  Not owned by the database.  When null the DB creates
    // its own LRU cache of block_cache_size bytes and owns it; either way
    // TableCache/TableReader always get a non-null cache.  Set
    // block_cache_size to 0 to keep the same read path without retaining
    // blocks across calls.
    Cache *block_cache = nullptr;
    size_t block_cache_size = 8 * 1024 * 1024;
    CacheType block_cache_type = CacheType::LRU;

    // Size at which the active memtable is flushed to an on-disk table.
    size_t write_buffer_size = 4 * 1024 * 1024;
    // Values strictly larger than this are written to the value log (vLog) and
    // referenced from the memtable/SSTable by a BlobHandle.  Zero disables the
    // value log entirely, storing every value inline.
    size_t value_threshold = 4 * 1024;
    // Approximate maximum size of a vLog file before the writer rolls to the
    // next file id.  Zero disables rolling.
    uint64_t vlog_file_size = 64 * 1024 * 1024;
    // Logical size of an on-disk data block.
    size_t block_size = 4 * 1024;
    // Maximum accepted user-key length.  Oversized keys are rejected rather
    // than truncated when index entries are written.  The effective limit is
    // capped to a quarter of the index block size (which itself is clamped to
    // [4KB, 64KB]).
    size_t max_key_size = 4 * 1024;
    // Approximate maximum size of a table file before the table is split.
    size_t max_file_size = 2 * 1024 * 1024;
    // Number of LSM levels.  Must be at least 1.
    size_t num_levels = 7;
    // Maximum size in bytes of the MANIFEST before it is rewritten as a fresh
    // snapshot.  Zero disables automatic MANIFEST rotation.
    size_t manifest_max_file_size = 4 * 1024 * 1024;
    // Upper bound on concurrently open file descriptors.
    int max_open_files = 1000;
};

// Per-write tuning options.
struct ANCHORDB_EXPORT WriteOptions {
    WriteOptions() = default;

    // If true, fsync the write-ahead log before the write is acknowledged as
    // durable.
    bool sync = false;
};

} // namespace ANCHORDB_NAMESPACE
