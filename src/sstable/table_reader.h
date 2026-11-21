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

#include "anchordb/anchordb_namespace.h"
#include "anchordb/comparator.h"
#include "anchordb/compressor.h"
#include "anchordb/filter.h"
#include "anchordb/iterator.h"
#include "anchordb/options.h"
#include "anchordb/status.h"
#include "sstable/filter.h"
#include "sstable/index_block.h"
#include "sstable/summary.h"
#include "sstable/table_file_set.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ANCHORDB_NAMESPACE {

class VLogReaderCache;

// Point-read view over one SSTable.
//
// open() loads and parses the table's summary and (when configured and
// present) its filter, and keeps both resident.  A get() then follows the read
// path the on-disk format prescribes:
//
//   filter -> summary -> index block -> data record / vLog record
//
//   1. The filter is consulted first.  A definite miss returns
//      Status::not_found without touching -Index.adb or -Data.adb.
//   2. SummaryReader maps the key to the first index block whose greatest key
//      is >= the target.
//   3. That block is read through RandomAccessFileReader, decompressed when the
//      table uses an active compressor, and parsed by IndexBlockReader.
//   4. The index slot's ValueHandle selects the payload:
//        Inline    read the data record from -Data.adb and decode it
//        Tombstone read the tombstone from -Data.adb and report not_found
//        Blob      read the complete vLog record named by
//                  (file_id, offset, size), starting at its header
//
// Blob records may reference different vLog files over the life of one table.
// The reader resolves them through a DB-owned VLogReaderCache that it borrows,
// so readers for the same vLog file are opened once and reused across tables
// instead of being owned per table.  The cache must outlive the reader.
//
// A TableReader owns its table files and is non-copyable.  Its on-disk view is
// immutable after a successful open(); only the reference count changes
// afterwards.
class TableReader {
  public:
    // Opens `files` and loads the summary and filter.  `db_dir` is the database
    // directory used to build vLog file names; pass an empty string when tables
    // are opened without a directory prefix.  `vlog_cache` is the DB-owned,
    // shared cache used to resolve blob entries; the reader borrows it and it
    // must outlive the reader (null disables blob reads).  `comparator`,
    // `compressor` and `filter` describe the table format and are supplied by
    // the DB/orchestrator layer from Options.  On success stores a
    // heap-allocated reader in *result; the caller owns it.  On failure returns
    // a non-ok Status and leaves *result untouched.
    static Status open(Env *env, const Comparator &comparator, const Compressor &compressor, const Filter &filter,
                       const TableFileSet &files, std::string db_dir, VLogReaderCache *vlog_cache,
                       TableReader **result);

    ~TableReader();

    TableReader(const TableReader &) = delete;
    TableReader &operator=(const TableReader &) = delete;

    // Manual reference counting.  open() returns one reference; unref() drops
    // it and deletes the reader at zero.  Iterators and caches each hold a
    // reference so they can outlive one another safely.
    void ref() { refs_.fetch_add(1, std::memory_order_relaxed); }
    void unref();

    // Point lookup.  On success copies the value bytes into *value and returns
    // Status::ok().  Returns Status::not_found when the key is absent or its
    // newest entry is a tombstone.  `scratch` is reusable per-call storage for
    // the raw data or vLog record.
    Status get(const ReadOptions &options, std::string_view key, std::string *value, std::string *scratch) const;

    // Returns a heap-allocated bidirectional iterator over the whole table
    // (caller deletes).  Tombstones are skipped; blobs are resolved through the
    // vLog.  The iterator holds its own reference to this reader.  An empty
    // table yields an empty iterator; a malformed table yields an error
    // iterator.
    Iterator *new_iterator(const ReadOptions &options);

    // Parity of the resident summary size for cache charging.
    size_t summary_size() const { return summary_buffer_.size(); }
    // Resident filter size, or zero when the table has no usable filter.
    size_t filter_size() const { return filter_buffer_.size(); }
    // Charge to use when caching this reader (resident summary + filter plus
    // fixed per-entry overhead).
    size_t cache_charge() const;
    // Index-block segment size recovered from the summary header.
    size_t block_size() const { return block_size_; }

  private:
    friend class TableIterator;

    TableReader() = default;

    // Opens the data/index files, loads and parses the summary, and loads the
    // optional filter.
    Status init_(const TableFileSet &files);

    // Reads the index block at `block_offset` (on-disk length
    // `compressed_size`) and decompresses it when needed.  *out points into
    // *stored_buf or *plain_buf, whichever holds the parseable block.
    Status read_index_block_(uint64_t block_offset, uint32_t compressed_size, std::string *stored_buf,
                             std::string *plain_buf, std::string_view *out) const;

    // Reads and validates the inline/tombstone data record selected by
    // `handle`.
    Status read_inline_record_(const ValueHandle &handle, std::string_view expected_key, std::string *value,
                               std::string *scratch) const;

    // Reads and validates the vLog record selected by `handle`.  The handle's
    // offset and size cover the whole record, starting at its header.
    Status read_blob_record_(const ValueHandle &handle, std::string_view expected_key, std::string *value,
                             std::string *scratch) const;

    std::atomic<int> refs_{0};

    Comparator comparator_;
    Compressor compressor_;
    Filter filter_spec_;
    Env *env_ = nullptr;
    std::string db_dir_;

    // Resident summary and its parser.  summary_buffer_ must outlive summary_.
    std::string summary_buffer_;
    SummaryReader *summary_ = nullptr;

    // Resident filter and its parser.  Empty/unparseable filters leave filter_
    // null, which makes every lookup consult the index.
    std::string filter_buffer_;
    FilterReader *filter_ = nullptr;

    RandomAccessFileReader *index_reader_ = nullptr;
    RandomAccessFileReader *data_reader_ = nullptr;
    uint64_t index_file_size_ = 0;
    uint64_t data_file_size_ = 0;
    size_t block_size_ = 0;

    // Borrowed DB-owned vLog reader cache; must outlive this reader.
    VLogReaderCache *vlog_cache_ = nullptr;
};

} // namespace ANCHORDB_NAMESPACE
