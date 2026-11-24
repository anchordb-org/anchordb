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
#include "anchordb/status.h"
#include "db/entry_iterator.h"
#include "memtable/entry.h"
#include "memtable/skiplist.h"
#include "util/arena.h"
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

class MemTable;

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
// can hold several versions of one user key, ordered newest first.
//
// This yields unresolved entries: type()/timestamp() come straight from the
// stored key trailer, and value_ref() returns the stored bytes (a blob entry
// carries its encoded BlobHandle).  It never reads the vLog and does not hide
// tombstones; resolving and tombstone filtering happen in the DB layer.
class MemtableIterator final : public EntryIterator {
  public:
    explicit MemtableIterator(Table *table) : iter_(table) {}

    MemtableIterator(MemtableIterator &&other) noexcept
        : iter_(std::move(other.iter_)), status_(std::move(other.status_)) {}

    MemtableIterator &operator=(MemtableIterator &&other) {
        if (this != &other) {
            this->iter_ = std::move(other.iter_);
            this->status_ = std::move(other.status_);
        }
        return *this;
    }

    ~MemtableIterator() override = default;

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
    EntryType type() const override;
    uint64_t timestamp() const override;
    ValueRef value_ref() const override;
    Status status() const override { return status_; }

  private:
    typename Table::Iterator iter_;
    mutable Status status_;
};

// In-memory table backed by a skiplist and arena.
//
// The memtable stores exactly what it is given.  It does not decide whether a
// value belongs in the vLog: the DB layer makes that choice, writes the value
// to the vLog, and hands the memtable an EntryType::Blob entry whose value
// bytes are an encoded BlobHandle.  The memtable performs no file I/O and never
// resolves blobs.
class MemTable {
  public:
    explicit MemTable(Comparator comparator, const uint32_t arena_block_size, size_t max_key_size = kDefaultMaxKeySize)
        : comparator_(std::move(comparator)), arena_(arena_block_size), refs_{0}, max_key_size_(max_key_size),
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
    // entry().value() is exactly the bytes stored in the arena.  For
    // EntryType::Blob that is the encoded BlobHandle (see db_format.h), so a
    // flush can copy the handle into an SSTable index slot without touching the
    // vLog.  The returned MemtableEntry owns no bytes; its views are valid only
    // until next() or destruction.
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

    // Returns a bidirectional iterator over every stored entry, tombstones
    // included, yielding unresolved values.  The caller owns it.
    EntryIterator *new_iterator() { return new MemtableIterator(&table_); }

    // Returns a forward-only iterator over the raw stored entries.  Used by the
    // flush path, which needs EntryType, timestamp and the undecoded value.
    RawIterator new_raw_iterator() const { return RawIterator(&table_); }

    // Inserts one entry stamped with `timestamp`.  Returns
    // Status::invalid_argument when `key` exceeds max_key_size(), `type` is not
    // a valid EntryType, or a Blob entry's value is not an encoded BlobHandle.
    // Not thread-safe: writers must be serialized.
    Status add(uint64_t timestamp, EntryType type, std::string_view key, std::string_view value);

    // Maximum accepted key length in bytes.
    size_t max_key_size() const { return max_key_size_; }

    // Looks up `key` and returns the newest stored version without resolving
    // it.  Returns true and fills *out for any entry, including a tombstone;
    // returns false when the key is absent (leaving *out untouched) or on
    // corruption (setting *s).
    bool get_raw(const LookUpKey &key, ValueRef *out, Status *s);

  private:
    friend class MemtableIterator;

    ~MemTable() { assert(refs_.load(std::memory_order_relaxed) == 0); }

    Comparator comparator_;
    Arena arena_;
    std::atomic<int> refs_;
    size_t max_key_size_;
    Table table_;
};

} // namespace ANCHORDB_NAMESPACE
