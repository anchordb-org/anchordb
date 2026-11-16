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

#include "anchordb/iterator.h"
#include "anchordb/status.h"
#include "db/db_format.h"
#include "memory/arena.h"
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

// Decodes a length-prefixed slice: varint32 length followed by the payload.
inline std::string_view get_len_prefixed_slice(const char *data) {
    uint32_t len;
    const char *p = data;
    p = get_varint_32_ptr(p, p + 5, &len);
    return std::string_view(p, len);
}

// Encodes `target` into `scratch` as a length-prefixed internal key.  Iterator
// seeks use the highest sequence number and the largest EntryType, so the
// encoded key sorts before every stored version of `target`.
inline char *encode_key(std::string *scratch, std::string_view target) {
    scratch->clear();
    put_varint_32(scratch, target.size() + 8);
    scratch->append(target.data(), target.size());
    put_fixed_64(scratch, pack_seq_and_type(k_max_seq_num, EntryType::Blob));
    return scratch->data();
}

// Orders length-prefixed internal keys the same way InternalKeyComparator does:
// user key ascending, then the packed (sequence, type) trailer descending.
// The value bytes that follow the trailer are not part of the comparison.
struct KeyComparator {
    const Comparator comparator;
    explicit KeyComparator(const Comparator &c) : comparator(c) {}
    int operator()(const char *aptr, const char *bptr) const {
        std::string_view a = get_len_prefixed_slice(aptr);
        std::string_view b = get_len_prefixed_slice(bptr);
        const std::string_view a_user(a.data(), a.size() - 8);
        const std::string_view b_user(b.data(), b.size() - 8);
        int r = comparator.compare(a_user, b_user);
        if (r != 0) { return r; }
        const uint64_t a_tag = decode_fixed_64(a.data() + a.size() - 8);
        const uint64_t b_tag = decode_fixed_64(b.data() + b.size() - 8);
        if (a_tag > b_tag) { return -1; }
        if (a_tag < b_tag) { return 1; }
        return 0;
    }
};

using Table = Skiplist<const char *, KeyComparator>;

// Iterator over a memtable's skiplist, decoding length-prefixed keys/values.
// Blob values are resolved from the vLog on demand; the returned view points
// into the iterator's scratch buffer.
class MemtableIterator : public Iterator {
  public:
    MemtableIterator(Table *table, const MemTable *memtable)
        : memtable_(memtable), iter_(table) {}

    MemtableIterator(MemtableIterator &&other) noexcept
        : memtable_(other.memtable_), iter_(std::move(other.iter_)),
          tmp_(std::move(other.tmp_)),
          value_scratch_(std::move(other.value_scratch_)),
          status_(std::move(other.status_)) {}

    MemtableIterator &operator=(MemtableIterator &&other) {
        if (this != &other) {
            this->memtable_ = other.memtable_;
            this->iter_ = std::move(other.iter_);
            this->tmp_ = std::move(other.tmp_);
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
        iter_.seek(encode_key(&tmp_, key));
    }
    void seek_to_first() override { iter_.seek_to_first(); }
    void seek_to_last() override { iter_.seek_to_last(); }
    std::string_view key() const override {
        return get_len_prefixed_slice(iter_.key());
    }
    std::string_view value() const override;
    Status status() const override { return status_; }

  private:
    const MemTable *memtable_;
    typename Table::Iterator iter_;
    std::string tmp_;                   // scratch buffer for seek key encoding
    mutable std::string value_scratch_; // resolved blob value
    mutable Status status_;
};

// In-memory table backed by a skiplist and arena.
//
// When `vlog_writer` is non-null, values larger than `value_threshold` are
// appended to the writer's vLog file and the memtable stores a BlobHandle as
// the entry value.  The caller owns the writer and must keep it alive for the
// memtable's lifetime.  `env` and `vlog_dbname` are needed to reopen the
// numbered vLog file by `file_id` when a blob value is read back.
class MemTable {
  public:
    explicit MemTable(Comparator comparator, const uint32_t arena_block_size,
                      size_t max_key_size = kDefaultMaxKeySize,
                      DataRecordWriter *vlog_writer = nullptr,
                      uint32_t vlog_file_id = 0,
                      size_t value_threshold = 4 * 1024, Env *env = nullptr,
                      std::string vlog_dbname = std::string())
        : comparator_(std::move(comparator)), arena_(arena_block_size),
          refs_{0}, max_key_size_(max_key_size),
          value_threshold_(value_threshold), vlog_writer_(vlog_writer),
          vlog_file_id_(vlog_file_id), env_(env),
          vlog_dbname_(std::move(vlog_dbname)),
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

    size_t approximate_memory_usage() const { return arena_.memory_usage(); }

    Iterator *new_iterator() { return new MemtableIterator(&table_, this); }

    // Inserts one entry.  Returns Status::invalid_argument when `key` exceeds
    // max_key_size().  Not thread-safe: writers must be serialized.
    Status add(seq_num seq, EntryType type, std::string_view key,
               std::string_view value);

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
    Status read_blob_(const BlobHandle &handle, std::string_view expected_key,
                      std::string *value) const;

    Comparator comparator_;
    Arena arena_;
    std::atomic<int> refs_;
    size_t max_key_size_;
    size_t value_threshold_;
    DataRecordWriter *vlog_writer_;
    uint32_t vlog_file_id_;
    Env *env_;
    std::string vlog_dbname_;
    Table table_;
};

} // namespace ANCHORDB_NAMESPACE
