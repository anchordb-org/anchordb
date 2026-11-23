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
#include "anchordb/iterator.h"
#include "db/db_format.h"
#include "util/coding.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ANCHORDB_NAMESPACE {

// Default target size of one uncompressed -Data.adb block.  A block holds whole
// records only; it may exceed the target by at most one record.
inline constexpr size_t kDefaultDataBlockSize = 32 * 1024; // 32KB

// Hard ceiling on an uncompressed data block.  This bounds the buffer the
// reader allocates after decompression, so corrupt compression metadata cannot
// request an unbounded allocation.  A record larger than this cannot be stored
// inline; the DB layer must route it to the vLog.
inline constexpr size_t kMaxDataBlockSize = 64 * 1024 * 1024; // 64MB

// On-disk header for one -Data.adb block: the uncompressed byte length.  The
// stored (possibly compressed) length is carried by the index slot, so the
// block itself needs no length field.
inline constexpr size_t kDataBlockHeaderSize = sizeof(uint32_t);

// Appends the fixed-width block header for `uncompressed_size` to *dst.
inline void append_data_block_header(std::string *dst, uint32_t uncompressed_size) {
    put_fixed_32(dst, uncompressed_size);
}

// Parses a block header from the front of `stored`.  On success sets
// *uncompressed_size and *payload to the bytes that follow the header.
inline bool parse_data_block_header(std::string_view stored, uint32_t *uncompressed_size, std::string_view *payload) {
    if (stored.size() < kDataBlockHeaderSize) { return false; }
    if (uncompressed_size != nullptr) { *uncompressed_size = decode_fixed_32(stored.data()); }
    if (payload != nullptr) { *payload = stored.substr(kDataBlockHeaderSize); }
    return true;
}

// Builds one uncompressed -Data.adb block by concatenating whole data records
// (the same record layout written by DataRecordWriter).  Like IndexBlockBuilder
// it is an in-memory segment builder: it does not perform I/O and does not
// compress.  TableBuilder compresses and frames the finished block, and records
// its location in the index.
//
// Only Inline and Tombstone records belong in a data block.  Blob values live
// in the vLog and are addressed directly from the index, and Compressed is a
// vLog-only entry type.
class DataBlockBuilder {
  public:
    enum class AddResult {
        Added,   // record stored in the current block
        Full,    // current block is non-empty and the record would overflow it
        Invalid, // record type/key/size cannot be stored in a data block
    };

    // `target_size` is clamped to [1, kMaxDataBlockSize].
    explicit DataBlockBuilder(size_t target_size = kDefaultDataBlockSize);

    DataBlockBuilder(const DataBlockBuilder &) = delete;
    DataBlockBuilder &operator=(const DataBlockBuilder &) = delete;

    // Appends one whole record.  A record is never split across blocks.  When
    // the current block is non-empty and the new record would push it past
    // target_size(), nothing is written and Full is returned so the caller can
    // flush and retry.  An empty block accepts a single oversized record (up to
    // kMaxDataBlockSize).  On Added, *in_block_offset (when non-null) receives
    // the byte offset of the record inside the block.
    AddResult add(std::string_view key, std::string_view value, EntryType type, uint64_t timestamp,
                  uint32_t *in_block_offset);

    bool empty() const { return buffer_.empty(); }
    bool full() const { return sealed_ || buffer_.size() >= target_size_; }
    bool sealed() const { return sealed_; }

    // Uncompressed bytes built so far.
    size_t size() const { return buffer_.size(); }
    // Target (uncompressed) block size.
    size_t target_size() const { return target_size_; }
    // Number of records appended to the current block.
    size_t record_count() const { return record_count_; }

    // Seals the block and returns its uncompressed bytes.  Idempotent.
    std::string_view finish();

    // Clears the block for reuse.
    void reset();

  private:
    size_t target_size_;
    std::string buffer_;
    size_t record_count_ = 0;
    bool sealed_ = false;
};

// One decoded data-block record.  The views point into the block buffer the
// reader was constructed from and must not outlive it.
struct DataBlockEntry {
    std::string_view key;
    std::string_view value;
    EntryType type = EntryType::Inline;
    uint64_t timestamp = 0;
    // Byte offset of the record inside the uncompressed block.
    uint32_t in_block_offset = 0;
};

// Parses and validates one uncompressed -Data.adb block.  This is the data-block
// counterpart of IndexBlockReader: it owns no I/O, decompresses nothing, and
// exposes entries plus an Iterator over them.  Every record is CRC-checked while
// the block is parsed, so a malformed block is rejected by valid().
class DataBlockReader {
  public:
    explicit DataBlockReader(std::string_view block);

    DataBlockReader(const DataBlockReader &) = delete;
    DataBlockReader &operator=(const DataBlockReader &) = delete;

    bool valid() const { return valid_; }

    size_t size() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }

    const DataBlockEntry &entry(size_t index) const { return entries_[index]; }

    // Index of the first entry whose key is >= `target` under `cmp`, or size().
    size_t lower_bound(std::string_view target, const Comparator &cmp) const;

    // Finds the entry that starts at `in_block_offset`.  Returns false when no
    // record starts there (an index slot pointing into the middle of a record is
    // corruption).
    bool find_offset(uint32_t in_block_offset, size_t *index) const;

    // Heap-allocated iterator over the block in record order: key() is the user
    // key and value() is the raw record value.  The caller owns it and must keep
    // this reader (and its block buffer) alive.  A malformed block yields an
    // error iterator; an empty block yields an empty iterator.
    Iterator *new_iterator(Comparator comparator = Comparator{}) const;

  private:
    std::vector<DataBlockEntry> entries_;
    bool valid_ = false;
};

} // namespace ANCHORDB_NAMESPACE
