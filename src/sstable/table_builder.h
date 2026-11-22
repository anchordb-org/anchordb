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
#include "anchordb/status.h"
#include "db/db_format.h"
#include "file/file_meta.h"
#include "sstable/data_record.h"
#include "sstable/filter.h"
#include "sstable/index_block.h"
#include "sstable/summary.h"
#include "sstable/table_file_set.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ANCHORDB_NAMESPACE {

class Env;
class WritableFileWriter;

// Orchestrates the four files that make up one SSTable:
//
//   -Data.adb     inline and tombstone records, written through DataRecordWriter
//   -Index.adb    IndexBlockBuilder segments, sealed as each one fills
//   -Summary.adb  one entry per index block (greatest key, offset, stored size)
//   -Filter.adb   per-table filter built from the accepted user keys
//
// Keys are user keys, not internal keys.  Entries must arrive in index order:
// user key ascending and, for equal user keys, timestamp descending (newest
// version first).  add() enforces this and returns Status::invalid_argument
// when it is violated.
//
// Inline and tombstone entries are encoded into -Data.adb and the resulting
// record location becomes the entry's index handle.  Blob entries are not
// written to -Data.adb at all: the DB layer has already stored the value in the
// vLog and supplies a BlobHandle (file id, record offset, record size).
// TableBuilder writes that record-level handle into the index slot and never
// into the data file; the DB read path resolves the complete vLog record from
// that handle.
//
// The builder owns the output files it opens through `env` and writes
// them under the names in `files`.  finish() flushes and syncs -Data.adb and
// -Index.adb, writes -Summary.adb and (when enabled) -Filter.adb, and closes every
// file.  Atomic publication (writing temporary names and renaming them into
// place) is the caller's responsibility; if the builder fails or is destroyed
// before finish() succeeds, abort() closes and removes the partial files.
//
// File-operation errors are sticky, like DataRecordWriter: once status() is
// non-ok, add() and finish() keep returning it.  Logical rejections (ordering
// violations, oversized keys) do not poison the builder.
class TableBuilder {
  public:
    // Opens -Data.adb and -Index.adb under `files` and validates that the
    // required file names are present.  `comparator`, `compressor`,
    // `compression_level`, `filter`, `block_size` and `max_key_size` drive the
    // table format.  A null comparator falls back to bytewise ordering; a null
    // compressor stores index blocks verbatim; a null filter disables
    // -Filter.adb.  Check status() after construction; a failed open leaves the
    // builder unusable.  The DB/orchestrator layer derives these from Options.
    TableBuilder(Env *env, const TableFileSet &files, const Comparator &comparator, const Compressor &compressor,
                 int compression_level, size_t block_size, size_t max_key_size, const Filter &filter,
                 size_t writable_file_buf_size = kWritableFileBufSize);

    ~TableBuilder();

    TableBuilder(const TableBuilder &) = delete;
    TableBuilder &operator=(const TableBuilder &) = delete;

    // Appends one entry.  For Inline and Tombstone, `value` is the raw value
    // bytes and is encoded into -Data.adb (a tombstone normally has an empty
    // value, which readers ignore).  For Blob, `value` is the memtable's
    // encoded BlobHandle and only the index slot is written: the data file
    // never holds a blob value or a blob handle.
    Status add(std::string_view key, std::string_view value, EntryType type, uint64_t timestamp);

    // Appends one blob entry from an already-decoded BlobHandle.  The handle's
    // offset and size address the whole vLog record, starting at its header.
    // Only the index entry is written.
    Status add_blob(std::string_view key, const BlobHandle &handle, uint64_t timestamp);

    // Seals the trailing index block, syncs the data and index streams, writes
    // -Summary.adb and -Filter.adb, and closes all files.  Idempotent on success.
    Status finish();

    // Closes and removes every output file created so far.  A no-op after a
    // successful finish().
    void abort();

    // Sticky status: non-ok once an output operation has failed.
    const Status &status() const { return status_; }

    bool finished() const { return finished_; }
    bool empty() const { return entry_count_ == 0; }

    // Number of entries accepted by add()/add_blob().
    size_t entry_count() const { return entry_count_; }
    // Number of index blocks sealed and written to -Index.adb.
    size_t block_count() const { return block_count_; }

    // Bytes written to -Data.adb (inline/tombstone records only).
    uint64_t data_size() const { return data_size_; }
    // Bytes written to -Index.adb (stored, possibly compressed, block segments).
    uint64_t index_size() const { return index_size_; }
    // Encoded size of the summary built so far.
    uint64_t summary_size() const { return summary_.byte_size(); }
    // Bytes written to -Filter.adb; zero when filtering is disabled.
    uint64_t filter_size() const { return filter_size_; }

    // User-key range of the accepted entries.  Empty until the first add.
    std::string_view smallest_key() const { return smallest_key_; }
    std::string_view largest_key() const { return largest_key_; }

    // Timestamp range of the accepted entries.
    uint64_t min_ts() const { return min_ts_; }
    uint64_t max_ts() const { return max_ts_; }

  private:
    // Opens -Data.adb and -Index.adb.
    Status open_files_(const size_t buffer_size);

    // Validates ordering and that the key can fit in a fresh index block.  Kept
    // separate from append_index_() so a rejected entry writes no data record.
    Status check_addable_(std::string_view key, uint64_t timestamp) const;

    // Appends one entry to the index, sealing the current block first when it
    // is full, and records the entry in the filter and summary bookkeeping.
    Status append_index_(std::string_view key, const ValueHandle &handle, uint64_t timestamp);

    // Seals the current (non-empty) block, optionally compresses it, records it
    // in the summary and appends it to -Index.adb.
    Status emit_block_();

    // Writes `contents` to `filename` and syncs it.  Stores the byte count in
    // *size_out on success.
    Status write_file_(const std::string &filename, std::string_view contents, uint64_t *size_out);

    // Writes -Filter.adb when a filter is configured; a no-op otherwise.
    Status write_filter_();

    Comparator comparator_;
    Compressor compressor_;
    int compression_level_ = 0;
    IndexBlockBuilder block_writer_;
    SummaryBuilder summary_;
    FilterBuilder filter_builder_;

    Env *env_ = nullptr;
    TableFileSet files_;
    // Owned output streams.  data_writer_ wraps data_file_ but does not own it,
    // so it is deleted first.
    WritableFileWriter *data_file_ = nullptr;
    WritableFileWriter *index_file_ = nullptr;
    DataRecordWriter *data_writer_ = nullptr;

    // Scratch buffer holding the compressed form of the block being emitted.
    std::string compressed_block_;

    // Last accepted key/timestamp, used to enforce index ordering.  Only
    // meaningful once entry_count_ > 0 (an empty key is a legal key).
    std::string last_key_;
    uint64_t last_timestamp_ = 0;

    std::string smallest_key_;
    std::string largest_key_;
    uint64_t min_ts_ = 0;
    uint64_t max_ts_ = 0;

    size_t entry_count_ = 0;
    size_t block_count_ = 0;
    uint64_t data_size_ = 0;
    uint64_t index_size_ = 0;
    uint64_t filter_size_ = 0;

    Status status_;
    bool finished_ = false;
    bool aborted_ = false;
};

} // namespace ANCHORDB_NAMESPACE
