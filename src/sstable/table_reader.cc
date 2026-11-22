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

#include "anchordb/env.h"
#include "db/entry_iterator.h"
#include "sstable/data_record.h"
#include "sstable/table_reader.h"
#include <cassert>
#include <cstring>
#include <string>
#include <string_view>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

namespace {

// Upper bound on a summary or filter file we are willing to load into memory.
constexpr uint64_t kMaxSummaryFileSize = 64ull * 1024 * 1024;
constexpr uint64_t kMaxFilterFileSize = 64ull * 1024 * 1024;

// Fixed byte overhead charged per cached reader, on top of the resident
// summary and filter bytes.
constexpr size_t kTableCacheEntryOverhead = 256;

// Reads exactly `n` bytes at `offset` into *buf and points *out at them.
Status read_exact(RandomAccessFileReader *reader, uint64_t offset, size_t n, std::string *buf, std::string_view *out) {
    *out = std::string_view();
    if (n == 0) {
        buf->clear();
        return Status::ok();
    }

    buf->resize(n);
    std::string_view view;
    Status s = reader->read(offset, n, &view, buf->data());
    if (!s.is_ok()) { return s; }
    if (view.size() < n) { return Status::corruption("table reader: short read", std::to_string(view.size())); }
    if (view.data() != buf->data()) { std::memmove(buf->data(), view.data(), n); }
    *out = std::string_view(buf->data(), n);
    return Status::ok();
}

// Decodes exactly one complete record from `bytes`, rejecting trailing data.
Status decode_single_record(std::string_view bytes, ParsedDataRecord *result) {
    DataRecordBufferReader reader(bytes);
    bool done = false;
    Status s = reader.next(result, &done);
    if (!s.is_ok()) { return s; }
    if (done) { return Status::corruption("table reader: missing record"); }
    if (!reader.done()) { return Status::corruption("table reader: trailing bytes after record"); }
    return Status::ok();
}

} // namespace

TableReader::~TableReader() {
    delete summary_;
    delete filter_;
    delete index_reader_;
    delete data_reader_;
}

void TableReader::unref() {
    const int old = refs_.fetch_sub(1, std::memory_order_acq_rel);
    assert(old > 0);
    if (old == 1) { delete this; }
}

size_t TableReader::cache_charge() const {
    return summary_buffer_.size() + filter_buffer_.size() + kTableCacheEntryOverhead;
}

Status TableReader::open(Env *env, const Comparator &comparator, const Compressor &compressor, const Filter &filter,
                         const TableFileSet &files, TableReader **result) {
    if (result == nullptr) { return Status::invalid_argument("TableReader::open: null result"); }
    if (env == nullptr) { return Status::invalid_argument("TableReader::open: env is null"); }

    TableReader *reader = new TableReader();
    reader->env_ = env;
    reader->comparator_ = comparator;
    if (reader->comparator_.compare == nullptr) { reader->comparator_ = new_byte_wise_comparator(); }
    reader->compressor_ = compressor;
    reader->filter_spec_ = filter;

    Status s = reader->init_(files);
    if (!s.is_ok()) {
        delete reader;
        return s;
    }

    reader->refs_.store(1, std::memory_order_relaxed);
    *result = reader;
    return Status::ok();
}

Status TableReader::init_(const TableFileSet &files) {
    if (files.data.empty() || files.index.empty() || files.summary.empty()) {
        return Status::invalid_argument("TableReader: missing data, index or summary file name");
    }

    uint64_t summary_size = 0;
    uint64_t index_size = 0;
    uint64_t data_size = 0;
    Status s = env_->get_file_size(files.summary, &summary_size);
    if (!s.is_ok()) { return s; }
    s = env_->get_file_size(files.index, &index_size);
    if (!s.is_ok()) { return s; }
    s = env_->get_file_size(files.data, &data_size);
    if (!s.is_ok()) { return s; }
    if (summary_size > kMaxSummaryFileSize) {
        return Status::corruption("table reader: summary file too large", std::to_string(summary_size));
    }

    s = env_->new_random_access_file_reader(files.index, &index_reader_);
    if (!s.is_ok()) { return s; }
    s = env_->new_random_access_file_reader(files.data, &data_reader_);
    if (!s.is_ok()) { return s; }
    index_file_size_ = index_size;
    data_file_size_ = data_size;

    RandomAccessFileReader *summary_reader = nullptr;
    s = env_->new_random_access_file_reader(files.summary, &summary_reader);
    if (!s.is_ok()) { return s; }
    std::string_view summary_view;
    s = read_exact(summary_reader, 0, static_cast<size_t>(summary_size), &summary_buffer_, &summary_view);
    delete summary_reader;
    if (!s.is_ok()) { return s; }

    summary_ = new SummaryReader(summary_buffer_, comparator_);
    if (!summary_->valid()) { return Status::corruption("table reader: invalid summary index"); }

    // The summary records which codec compressed the index blocks.  Reading a
    // table with any other codec (or with compression disabled) would silently
    // misinterpret the stored bytes, so reject it up front.
    const uint32_t stored_compressor_id = summary_->compressor_id();
    if (stored_compressor_id != compressor_.id) {
        return Status::corruption("table reader: compressor id mismatch",
                                  std::to_string(stored_compressor_id) + " != " + std::to_string(compressor_.id));
    }
    if (stored_compressor_id != 0 &&
        (compressor_.uncompress == nullptr || compressor_.get_uncompressed_length == nullptr)) {
        return Status::corruption("table reader: compressor unavailable for table");
    }

    // The summary header pins the index-block segment size for this table, and
    // its entries are the source of truth for the block layout.  Validate that
    // every entry maps inside -Index.adb before any read uses it.
    block_size_ = summary_->block_size();
    if (block_size_ == 0) { return Status::corruption("table reader: zero index block size"); }
    for (size_t i = 0; i < summary_->size(); ++i) {
        const SummaryEntry &entry = summary_->entry(i);
        if (entry.compressed_size == 0 || entry.block_offset > index_size ||
            entry.compressed_size > index_size - entry.block_offset) {
            return Status::corruption("table reader: summary entry out of range", std::to_string(entry.block_offset));
        }
    }

    // The filter is optional: a missing or unreadable filter simply disables
    // filtering instead of failing the open.
    const bool filter_configured = filter_spec_.key_may_match != nullptr && filter_spec_.name != nullptr;
    if (filter_configured && !files.filter.empty()) {
        uint64_t filter_size = 0;
        Status fs = env_->get_file_size(files.filter, &filter_size);
        if (fs.is_ok() && filter_size > 0) {
            if (filter_size > kMaxFilterFileSize) {
                return Status::corruption("table reader: filter file too large", std::to_string(filter_size));
            }
            RandomAccessFileReader *filter_reader = nullptr;
            fs = env_->new_random_access_file_reader(files.filter, &filter_reader);
            if (!fs.is_ok()) { return fs; }
            std::string_view filter_view;
            fs = read_exact(filter_reader, 0, static_cast<size_t>(filter_size), &filter_buffer_, &filter_view);
            delete filter_reader;
            if (!fs.is_ok()) { return fs; }

            FilterReader *parsed = new FilterReader(filter_spec_, filter_buffer_);
            if (parsed->valid()) {
                filter_ = parsed;
            } else {
                delete parsed;
            }
        }
    }

    return Status::ok();
}

Status TableReader::get_raw(std::string_view key, ValueRef *out, std::string *scratch) const {
    if (out == nullptr) { return Status::invalid_argument("TableReader::get_raw: null value ref"); }
    if (scratch == nullptr) { return Status::invalid_argument("TableReader::get_raw: null scratch"); }

    // A definite filter miss means the key is absent, so skip all I/O.
    if (filter_ != nullptr && !filter_->key_may_match(key)) {
        return Status::not_found("key not in table filter", std::string(key));
    }

    const size_t block_index = summary_->lower_bound(key);
    if (block_index >= summary_->size()) { return Status::not_found("key not in table range", std::string(key)); }
    const SummaryEntry &entry = summary_->entry(block_index);

    std::string stored_buf;
    std::string plain_buf;
    std::string_view block;
    Status s = read_index_block_(entry.block_offset, entry.compressed_size, &stored_buf, &plain_buf, &block);
    if (!s.is_ok()) { return s; }

    IndexBlockReader reader(block, block_size_);
    if (!reader.valid()) { return Status::corruption("table reader: invalid index block"); }

    const size_t slot = reader.lower_bound(key, comparator_);
    if (slot >= reader.size() || comparator_.compare(reader.key(slot), key) != 0) {
        return Status::not_found("key not found", std::string(key));
    }

    return make_value_ref_(reader.entry(slot), scratch, out);
}

// Bidirectional iterator over a TableReader's index.  It walks index slots in
// key order and spans block boundaries, but never reads a data record or a vLog
// record just to position: type()/timestamp() come from the resident index slot,
// and value_ref() lazily reads the data record only for inline and tombstone
// entries.  Blob entries yield their handle untouched.
class TableIterator final : public EntryIterator {
  public:
    TableIterator(TableReader *owner)
        : owner_(owner), comparator_(owner->comparator_), block_count_(owner->summary_->size()) {
        owner_->ref();
    }

    ~TableIterator() override { owner_->unref(); }

    TableIterator(const TableIterator &) = delete;
    TableIterator &operator=(const TableIterator &) = delete;

    bool valid() const override { return valid_; }

    void seek_to_first() override {
        status_ = Status::ok();
        valid_ = false;
        if (block_count_ == 0) {
            invalidate();
            return;
        }
        load_block(0);
        if (!status_.is_ok()) { return; }
        slot_ = 0;
        settle_forward();
    }

    void seek_to_last() override {
        status_ = Status::ok();
        valid_ = false;
        if (block_count_ == 0) {
            invalidate();
            return;
        }
        load_block(block_count_ - 1);
        if (!status_.is_ok()) { return; }
        slot_ = block_reader_->size();
        settle_backward();
    }

    void seek(std::string_view target) override {
        status_ = Status::ok();
        valid_ = false;
        if (block_count_ == 0) {
            invalidate();
            return;
        }

        const size_t block_index = owner_->summary_->lower_bound(target);
        if (block_index >= owner_->summary_->size()) {
            invalidate();
            return;
        }
        load_block(block_index);
        if (!status_.is_ok()) { return; }
        slot_ = block_reader_->lower_bound(target, comparator_);
        settle_forward();
    }

    void next() override {
        assert(valid_);
        ++slot_;
        settle_forward();
    }

    void prev() override {
        assert(valid_);
        if (slot_ > 0) {
            --slot_;
            valid_ = true;
            return;
        }
        if (block_index_ == 0) {
            invalidate();
            return;
        }
        load_block(block_index_ - 1);
        if (!status_.is_ok()) {
            invalidate();
            return;
        }
        slot_ = block_reader_->size();
        settle_backward();
    }

    std::string_view key() const override {
        assert(valid_);
        return block_reader_->entry(slot_).key;
    }

    EntryType type() const override {
        assert(valid_);
        return block_reader_->entry(slot_).handle.type;
    }

    uint64_t timestamp() const override {
        assert(valid_);
        return block_reader_->entry(slot_).timestamp;
    }

    ValueRef value_ref() const override {
        assert(valid_);
        ValueRef ref;
        Status s = owner_->make_value_ref_(block_reader_->entry(slot_), &scratch_, &ref);
        if (!s.is_ok()) {
            status_ = s;
            return ValueRef();
        }
        return ref;
    }

    Status status() const override { return status_; }

  private:
    void invalidate() {
        valid_ = false;
        delete block_reader_;
        block_reader_ = nullptr;
    }

    void load_block(size_t block_index) {
        block_index_ = block_index;
        delete block_reader_;
        block_reader_ = nullptr;

        // The summary gives this block's absolute offset and on-disk length.
        const SummaryEntry &entry = owner_->summary_->entry(block_index);
        std::string_view block;
        Status s =
            owner_->read_index_block_(entry.block_offset, entry.compressed_size, &stored_buf_, &plain_buf_, &block);
        if (!s.is_ok()) {
            status_ = s;
            invalidate();
            return;
        }
        block_reader_ = new IndexBlockReader(block, owner_->block_size_);
        if (!block_reader_->valid()) {
            status_ = Status::corruption("invalid index block");
            invalidate();
        }
    }

    // Positions on the next in-range slot, crossing block boundaries.
    void settle_forward() {
        while (true) {
            if (block_reader_ == nullptr) {
                invalidate();
                return;
            }
            if (slot_ < block_reader_->size()) {
                valid_ = true;
                return;
            }
            if (block_index_ + 1 >= block_count_) {
                invalidate();
                return;
            }
            load_block(block_index_ + 1);
            if (!status_.is_ok()) {
                invalidate();
                return;
            }
            slot_ = 0;
        }
    }

    // Positions on the previous in-range slot, crossing block boundaries.
    void settle_backward() {
        while (true) {
            if (block_reader_ == nullptr) {
                invalidate();
                return;
            }
            if (slot_ >= block_reader_->size()) {
                // Positioned just past the end of a block: step to its last
                // entry, or move to the previous block if it is empty.
                if (block_reader_->size() == 0) {
                    if (block_index_ == 0) {
                        invalidate();
                        return;
                    }
                    load_block(block_index_ - 1);
                    if (!status_.is_ok()) {
                        invalidate();
                        return;
                    }
                    slot_ = block_reader_->size();
                    continue;
                }
                slot_ = block_reader_->size() - 1;
                continue;
            }
            valid_ = true;
            return;
        }
    }

    TableReader *owner_;
    Comparator comparator_;
    size_t block_count_;

    size_t block_index_ = 0;
    IndexBlockReader *block_reader_ = nullptr;
    // On-disk (compressed) bytes of the resident block and, when decompressed,
    // the plain bytes the reader parses.
    std::string stored_buf_;
    std::string plain_buf_;

    size_t slot_ = 0;
    bool valid_ = false;

    mutable std::string scratch_;
    mutable Status status_;
};

EntryIterator *TableReader::new_iterator() {
    if (summary_ == nullptr || !summary_->valid()) {
        return new_error_entry_iterator(Status::corruption("invalid table"));
    }
    if (summary_->empty()) { return new_empty_entry_iterator(); }
    return new TableIterator(this);
}

Status TableReader::read_index_block_(uint64_t block_offset, uint32_t compressed_size, std::string *stored_buf,
                                      std::string *plain_buf, std::string_view *out) const {
    *out = std::string_view();
    if (block_offset > index_file_size_ || compressed_size > index_file_size_ - block_offset) {
        return Status::corruption("table reader: index block out of range", std::to_string(block_offset));
    }

    std::string_view stored;
    Status s = read_exact(index_reader_, block_offset, compressed_size, stored_buf, &stored);
    if (!s.is_ok()) { return s; }

    // A zero compressor id means the block is stored verbatim regardless of
    // the function pointers the caller supplied.
    if (compressor_.id == 0 || compressor_.uncompress == nullptr) {
        if (stored.size() != block_size_) {
            return Status::corruption("table reader: index block not block sized", std::to_string(stored.size()));
        }
        *out = stored;
        return Status::ok();
    }
    if (compressor_.get_uncompressed_length == nullptr) {
        return Status::corruption("table reader: compressor lacks length reporting");
    }

    size_t uncompressed_size = 0;
    if (!compressor_.get_uncompressed_length(stored.data(), stored.size(), &uncompressed_size)) {
        return Status::corruption("table reader: bad compressed index block length");
    }
    if (uncompressed_size != block_size_) {
        return Status::corruption("table reader: index block not block sized after decompression",
                                  std::to_string(uncompressed_size));
    }
    plain_buf->resize(uncompressed_size);
    if (!compressor_.uncompress(stored.data(), stored.size(), plain_buf->data())) {
        return Status::corruption("table reader: index block decompression failed");
    }
    *out = std::string_view(plain_buf->data(), uncompressed_size);
    return Status::ok();
}

Status TableReader::read_inline_value_(const ValueHandle &handle, std::string_view expected_key, std::string *scratch,
                                       std::string_view *value) const {
    if (value == nullptr) { return Status::invalid_argument("table reader: null inline value output"); }
    *value = std::string_view();
    if (handle.type == EntryType::Blob) {
        return Status::invalid_argument("table reader: blob handle passed to inline reader");
    }
    if (handle.size < kDataRecordHeaderSize) {
        return Status::corruption("table reader: data record size too small", std::to_string(handle.size));
    }
    if (handle.offset >= data_file_size_ || handle.size > data_file_size_ - handle.offset) {
        return Status::corruption("table reader: data record out of range", std::to_string(handle.offset));
    }

    std::string_view view;
    Status s = read_exact(data_reader_, handle.offset, handle.size, scratch, &view);
    if (!s.is_ok()) { return s; }

    ParsedDataRecord record;
    s = decode_single_record(view, &record);
    if (!s.is_ok()) { return s; }
    if (record.key != expected_key) { return Status::corruption("table reader: data record key mismatch"); }
    if (record.type != handle.type) { return Status::corruption("table reader: index/data record metadata mismatch"); }
    *value = record.value;
    return Status::ok();
}

Status TableReader::make_value_ref_(const IndexBlockEntry &entry, std::string *scratch, ValueRef *out) const {
    // Reset first so a reused output cannot retain a stale inline view (or a
    // blob handle) from a previous call.
    *out = ValueRef();
    out->type = entry.handle.type;
    out->timestamp = entry.timestamp;
    if (entry.handle.type == EntryType::Blob) {
        out->blob.file_id = entry.handle.file_id;
        out->blob.size = entry.handle.size;
        out->blob.offset = entry.handle.offset;
        return Status::ok();
    }

    std::string_view inline_value;
    Status s = read_inline_value_(entry.handle, entry.key, scratch, &inline_value);
    if (!s.is_ok()) { return s; }
    out->inline_value = inline_value;
    return Status::ok();
}

} // namespace ANCHORDB_NAMESPACE
