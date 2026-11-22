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
#include "anchordb/env.h"
#include "anchordb/filter.h"
#include "anchordb/status.h"
#include "db/db_format.h"
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

class EntryIterator;

// Point-read view over one SSTable.
//
// open() loads and parses the table's summary and (when configured and
// present) its filter, and keeps both resident.  A get_raw() then follows the
// read path the on-disk format prescribes:
//
//   filter -> summary -> index block -> data record (inline/tombstone)
//                                     -> ValueRef handle (blob)
//
//   1. The filter is consulted first.  A definite miss returns
//      Status::not_found without touching -Index.adb or -Data.adb.
//   2. SummaryReader maps the key to the first index block whose greatest key
//      is >= the target.
//   3. That block is read through RandomAccessFileReader, decompressed when the
//      table uses an active compressor, and parsed by IndexBlockReader.
//   4. The index slot's ValueHandle selects the payload:
//        Inline    read the data record from -Data.adb and expose it
//        Tombstone read the tombstone from -Data.adb and expose it
//        Blob      return the vLog handle without reading the vLog
//
// The reader performs no vLog I/O.  The DB layer resolves blob handles through
// its BlobResolver, so all tables share one resolution path and one pool of open
// vLog descriptors.
//
// A TableReader owns its table files and is non-copyable.  Its on-disk view is
// immutable after a successful open(); only the reference count changes
// afterwards.
class TableReader {
  public:
    // Opens `files` and loads the summary and filter.  `comparator`,
    // `compressor` and `filter` describe the table format and are supplied by
    // the DB/orchestrator layer from Options.  On success stores a
    // heap-allocated reader in *result; the caller owns it.  On failure returns
    // a non-ok Status and leaves *result untouched.
    static Status open(Env *env, const Comparator &comparator, const Compressor &compressor, const Filter &filter,
                       const TableFileSet &files, TableReader **result);

    ~TableReader();

    TableReader(const TableReader &) = delete;
    TableReader &operator=(const TableReader &) = delete;

    // Manual reference counting.  open() returns one reference; unref() drops
    // it and deletes the reader at zero.  Iterators and caches each hold a
    // reference so they can outlive one another safely.
    void ref() { refs_.fetch_add(1, std::memory_order_relaxed); }
    void unref();

    // Point lookup.  On success fills *out with the newest stored version,
    // unresolved.  A tombstone is returned as an entry (type Tombstone), not as
    // not_found, so the DB layer can shadow older versions.  Returns
    // Status::not_found only when the key is absent from this table.  `scratch`
    // is required reusable per-call storage for the raw data record; *out's
    // inline view points into it.
    Status get_raw(std::string_view key, ValueRef *out, std::string *scratch) const;

    // Returns a heap-allocated bidirectional iterator over the whole table
    // (caller deletes).  It yields unresolved entries, tombstones included, in
    // key order; blob entries carry their handle and are not read from the
    // vLog.  The iterator holds its own reference to this reader.  An empty
    // table yields an empty iterator; a malformed table yields an error
    // iterator.
    EntryIterator *new_iterator();

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
    // `handle`, exposing its value bytes through *value.  The view points into
    // `scratch`.
    Status read_inline_value_(const ValueHandle &handle, std::string_view expected_key, std::string *scratch,
                              std::string_view *value) const;

    // Fills *out from one index slot: inline/tombstone values are read into
    // `scratch`, blob entries are returned as a handle with no vLog read.  Single
    // implementation shared by get_raw() and the table iterator.
    Status make_value_ref_(const IndexBlockEntry &entry, std::string *scratch, ValueRef *out) const;

    std::atomic<int> refs_{0};

    Comparator comparator_;
    Compressor compressor_;
    Filter filter_spec_;
    Env *env_ = nullptr;

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
};

} // namespace ANCHORDB_NAMESPACE
