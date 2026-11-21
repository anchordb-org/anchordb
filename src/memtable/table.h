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
#include "anchordb/iterator.h"
#include "anchordb/status.h"
#include "db/vlog_reader_cache.h"
#include "memory/arena.h"
#include "memtable/entry.h"
#include "memtable/skiplist.h"
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

class Env;
class MemTable;
class DataRecordWriter;

// Orders memtable entries by user key ascending (via the user comparator), then
// timestamp descending (newest first), then EntryType descending.  The value
// bytes that follow the key portion are not part of the comparison.
struct KeyComparator {
    const Comparator comparator;
    explicit KeyComparator(const Comparator &c) : comparator(c) {}
    int operator()(const char *aptr, const char *bptr) const {
        MemtableEntry a;
        MemtableEntry b;
        MemtableEntry::decode_key(aptr, &a);
        MemtableEntry::decode_key(bptr, &b);

        int r = comparator.compare(a.key(), b.key());
        if (r != 0) { return r; }
        if (a.timestamp() > b.timestamp()) { return -1; }
        if (a.timestamp() < b.timestamp()) { return 1; }
        const uint8_t at = static_cast<uint8_t>(a.type());
        const uint8_t bt = static_cast<uint8_t>(b.type());
        if (at > bt) { return -1; }
        if (at < bt) { return 1; }
        return 0;
    }
};

using Table = Skiplist<const char *, KeyComparator>;

// Iterator over a memtable's skiplist.  key() returns the user key; a memtable
// can hold several versions of one user key, ordered newest first.  Blob values
// are resolved from the vLog on demand; the returned view points into the
// iterator's scratch buffer.
class MemtableIterator : public Iterator {
  public:
    MemtableIterator(Table *table, const MemTable *memtable) : memtable_(memtable), iter_(table) {}

    MemtableIterator(MemtableIterator &&other) noexcept
        : memtable_(other.memtable_), iter_(std::move(other.iter_)), value_scratch_(std::move(other.value_scratch_)),
          status_(std::move(other.status_)) {}

    MemtableIterator &operator=(MemtableIterator &&other) {
        if (this != &other) {
            this->memtable_ = other.memtable_;
            this->iter_ = std::move(other.iter_);
            this->value_scratch_ = std::move(other.value_scratch_);
            this->status_ = std::move(other.status_);
        }
        return *this;
    }

    ~MemtableIterator() = default;

    bool valid() const override { return iter_.valid(); }
    void next() override { iter_.next(); }
    void prev() override { iter_.prev(); }
    void seek(std::string_view key) override {
        LookUpKey lookup(key, UINT64_MAX, EntryType::Blob);
        iter_.seek(lookup.memtable_key().data());
    }
    void seek_to_first() override { iter_.seek_to_first(); }
    void seek_to_last() override { iter_.seek_to_last(); }
    std::string_view key() const override { return MemtableEntry::key_view(iter_.key()); }
    std::string_view value() const override;
    Status status() const override { return status_; }

  private:
    const MemTable *memtable_;
    typename Table::Iterator iter_;
    mutable std::string value_scratch_; // resolved blob value
    mutable Status status_;
};

// In-memory table backed by a skiplist and arena.
//
// When `vlog_writer` is non-null, values larger than `value_threshold` are
// appended to the writer's vLog file and the memtable stores a BlobHandle as
// the entry value.  The caller owns the writer and must keep it alive for the
// memtable's lifetime.  `env` and `vlog_dbname` are needed to reopen the
// numbered vLog file by `file_id` when a blob value is read back;
// `vlog_reader_cache` is the DB-owned reader cache the memtable borrows for
// those reads and must outlive it.  It is required; pass nullptr explicitly to
// disable blob reads.
class MemTable {
  public:
    explicit MemTable(Comparator comparator, const uint32_t arena_block_size, VLogReaderCache *vlog_reader_cache,
                      size_t max_key_size = kDefaultMaxKeySize, DataRecordWriter *vlog_writer = nullptr,
                      uint32_t vlog_file_id = 0, size_t value_threshold = 4 * 1024, Env *env = nullptr,
                      std::string vlog_dbname = std::string())
        : comparator_(std::move(comparator)), arena_(arena_block_size), refs_{0}, max_key_size_(max_key_size),
          value_threshold_(value_threshold), vlog_writer_(vlog_writer), vlog_file_id_(vlog_file_id), env_(env),
          vlog_dbname_(std::move(vlog_dbname)), vlog_reader_cache_(vlog_reader_cache),
          table_(KeyComparator(comparator_), &arena_) {}

    MemTable(const MemTable &) = delete;
    MemTable &operator=(const MemTable &) = delete;

    // Reference counting for shared ownership.  Atomic so a reader holding a
    // reference may release it while another thread holds or drops its own.
    void ref() { refs_.fetch_add(1, std::memory_order_relaxed); }
    void unref() {
        const int old = refs_.fetch_sub(1, std::memory_order_acq_rel);
        assert(old >= 1);
        if (old == 1) { delete this; }
    }

    // Forward-only iterator over the raw stored entries of this memtable.
    //
    // Unlike MemtableIterator, this resolves nothing: entry().value() is
    // exactly the bytes stored in the arena.  For EntryType::Blob that is the
    // encoded BlobHandle (see db_format.h), so a flush can copy the handle
    // into an SSTable index slot without touching the vLog.  The returned
    // MemtableEntry owns no bytes; its views are valid only until next() or
    // destruction.
    //
    // The memtable must be immutable while this iterator is used, consistent
    // with the memtable's single-writer contract.
    class RawIterator {
      public:
        explicit RawIterator(const Table *table) : iter_(table) {}

        RawIterator(RawIterator &&) noexcept = default;
        RawIterator &operator=(RawIterator &&) noexcept = default;

        bool valid() const { return iter_.valid(); }
        void seek_to_first() { iter_.seek_to_first(); }
        void next() { iter_.next(); }

        // Decodes the current entry.  Returns a default-constructed entry when
        // the iterator is invalid; on a malformed key it returns a default
        // entry and sets status() to corruption.
        MemtableEntry entry() const;
        Status status() const { return status_; }

      private:
        Table::Iterator iter_;
        mutable Status status_;
    };

    size_t approximate_memory_usage() const { return arena_.memory_usage(); }

    Iterator *new_iterator() { return new MemtableIterator(&table_, this); }

    // Returns a forward-only iterator over the raw stored entries.  Used by the
    // flush path, which needs EntryType, timestamp and the undecoded value.
    RawIterator new_raw_iterator() const { return RawIterator(&table_); }

    // Inserts one entry stamped with `timestamp`.  Returns
    // Status::invalid_argument when `key` exceeds max_key_size() or `type` is
    // not a valid EntryType.  Not thread-safe: writers must be serialized.
    Status add(uint64_t timestamp, EntryType type, std::string_view key, std::string_view value);

    // Maximum accepted key length in bytes.
    size_t max_key_size() const { return max_key_size_; }

    // Looks up `key`.  Returns true and fills *value for a live entry, false
    // with *s set to not_found for a tombstone, or false with *s untouched when
    // the key is absent.
    bool get(const LookUpKey &key, std::string *value, Status *s);

  private:
    friend class MemtableIterator;

    ~MemTable() { assert(refs_.load(std::memory_order_relaxed) == 0); }

    // Reads and decodes the vLog record referenced by `handle`, copies its
    // value into *value and validates the stored key and entry type.
    Status read_blob_(const BlobHandle &handle, std::string_view expected_key, std::string *value) const;

    Comparator comparator_;
    Arena arena_;
    std::atomic<int> refs_;
    size_t max_key_size_;
    size_t value_threshold_;
    DataRecordWriter *vlog_writer_;
    uint32_t vlog_file_id_;
    Env *env_;
    std::string vlog_dbname_;
    // Borrowed DB-owned vLog reader cache, keyed by file name.  Must outlive the
    // memtable; null disables blob reads.  Mutable because blob reads go
    // through a const memtable view.
    mutable VLogReaderCache *vlog_reader_cache_;
    Table table_;
};

} // namespace ANCHORDB_NAMESPACE
