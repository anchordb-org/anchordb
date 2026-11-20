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

#include "sstable/table_reader.h"
#include "anchordb/env.h"
#include "cache/typed_cache.h"
#include "db/vlog_reader_cache.h"
#include "file/filename.h"
#include "sstable/data_record.h"
#include <cassert>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

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
    if (vlog_cache_ != nullptr) { vlog_cache_->unref(); }
}

void TableReader::unref() {
    const int old = refs_.fetch_sub(1, std::memory_order_acq_rel);
    assert(old > 0);
    if (old == 1) { delete this; }
}

size_t TableReader::cache_charge() const {
    return summary_buffer_.size() + filter_buffer_.size() + kTableCacheEntryOverhead;
}

Status TableReader::open(const Options &options, const TableFileSet &files, std::string db_dir,
                         VLogReaderCache *vlog_cache, TableReader **result) {
    if (result == nullptr) { return Status::invalid_argument("TableReader::open: null result"); }
    if (options.env == nullptr) { return Status::invalid_argument("TableReader::open: options.env is null"); }

    TableReader *reader = new TableReader();
    reader->env_ = options.env;
    reader->comparator_ = options.comparator;
    if (reader->comparator_.compare == nullptr) { reader->comparator_ = new_byte_wise_comparator(); }
    reader->compressor_ = options.compressor;
    reader->filter_spec_ = options.filter;
    reader->db_dir_ = std::move(db_dir);
    reader->vlog_cache_ = vlog_cache;
    if (reader->vlog_cache_ != nullptr) { reader->vlog_cache_->ref(); }

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

Status TableReader::get(const ReadOptions &options, std::string_view key, std::string *value,
                        std::string *scratch) const {
    (void)options;
    if (value == nullptr) { return Status::invalid_argument("TableReader::get: null value"); }
    if (scratch == nullptr) { return Status::invalid_argument("TableReader::get: null scratch"); }

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

    const ValueHandle handle = reader.entry(slot).handle;
    if (handle.type == EntryType::Blob) { return read_blob_record_(handle, key, value, scratch); }
    return read_inline_record_(handle, key, value, scratch);
}

// Bidirectional iterator over a TableReader.  It keeps one (possibly
// compressed) index block resident at a time and spans block boundaries.
// Tombstone entries are skipped; blob values are resolved through the reader's
// vLog descriptor pool.
class TableIterator : public Iterator {
  public:
    TableIterator(TableReader *owner, const ReadOptions &options)
        : owner_(owner), comparator_(owner->comparator_), block_count_(owner->summary_->size()) {
        (void)options;
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
        skip_forward();
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
        skip_backward();
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
        skip_forward();
    }

    void next() override {
        assert(valid_);
        ++slot_;
        skip_forward();
    }

    void prev() override {
        assert(valid_);
        if (slot_ > 0) {
            --slot_;
            skip_backward();
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
        skip_backward();
    }

    std::string_view key() const override {
        assert(valid_);
        return key_;
    }

    std::string_view value() const override {
        assert(valid_);
        return value_;
    }

    Status status() const override { return status_; }

  private:
    bool visible(size_t slot) const { return block_reader_->entry(slot).handle.type != EntryType::Tombstone; }

    void invalidate() {
        valid_ = false;
        delete block_reader_;
        block_reader_ = nullptr;
        key_ = std::string_view();
        value_ = std::string_view();
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

    void materialize_current() {
        const IndexBlockEntry entry = block_reader_->entry(slot_);
        key_ = entry.key;
        value_buf_.clear();

        Status s;
        if (entry.handle.type == EntryType::Blob) {
            s = owner_->read_blob_record_(entry.handle, key_, &value_buf_, &scratch_);
        } else {
            s = owner_->read_inline_record_(entry.handle, key_, &value_buf_, &scratch_);
        }
        if (!s.is_ok()) {
            status_ = s;
            invalidate();
            return;
        }
        value_ = value_buf_;
        valid_ = true;
    }

    void skip_forward() {
        while (true) {
            if (block_reader_ == nullptr) {
                invalidate();
                return;
            }
            if (slot_ < block_reader_->size()) {
                if (visible(slot_)) {
                    materialize_current();
                    return;
                }
                ++slot_;
                continue;
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

    void skip_backward() {
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
                    continue;
                }
                slot_ = block_reader_->size() - 1;
                continue;
            }
            if (visible(slot_)) {
                materialize_current();
                return;
            }
            if (slot_ == 0) {
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
            --slot_;
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

    std::string value_buf_;
    std::string scratch_;
    std::string_view key_;
    std::string_view value_;
    Status status_;
};

Iterator *TableReader::new_iterator(const ReadOptions &options) {
    if (summary_ == nullptr || !summary_->valid()) { return new_error_iterator(Status::corruption("invalid table")); }
    if (summary_->empty()) { return new_empty_iterator(); }
    return new TableIterator(this, options);
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

Status TableReader::read_inline_record_(const ValueHandle &handle, std::string_view expected_key, std::string *value,
                                        std::string *scratch) const {
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
    if (record.type == EntryType::Tombstone) { return Status::not_found("tombstone", std::string(expected_key)); }
    value->assign(record.value.data(), record.value.size());
    return Status::ok();
}

Status TableReader::read_blob_record_(const ValueHandle &handle, std::string_view expected_key, std::string *value,
                                      std::string *scratch) const {
    if (handle.type != EntryType::Blob) {
        return Status::invalid_argument("table reader: non-blob handle passed to blob reader");
    }
    if (handle.size < kDataRecordHeaderSize) {
        return Status::corruption("table reader: vLog record size too small", std::to_string(handle.size));
    }
    if (vlog_cache_ == nullptr) {
        return Status::not_supported("table reader: no vLog reader cache", std::to_string(handle.file_id));
    }

    const std::string filename = vlog_filename(db_dir_, handle.file_id);
    // Use the live file size, not RandomAccessFileReader::size(): a reader
    // cached for this file may predate later appends to the same vLog file.
    uint64_t file_size = 0;
    Status s = env_->get_file_size(filename, &file_size);
    if (!s.is_ok()) { return s; }
    if (handle.offset > file_size || handle.size > file_size - handle.offset) {
        return Status::corruption("table reader: vLog record out of range", std::to_string(handle.offset));
    }

    CachePin<RandomAccessFileReader> pin;
    s = vlog_cache_->open_reader(filename, &pin);
    if (!s.is_ok()) { return s; }

    std::string_view view;
    s = read_exact(pin.get(), handle.offset, handle.size, scratch, &view);
    if (!s.is_ok()) { return s; }

    ParsedDataRecord record;
    s = decode_single_record(view, &record);
    if (!s.is_ok()) { return s; }
    if (!record.is_blob()) { return Status::corruption("table reader: vLog record is not a blob"); }
    if (record.key != expected_key) { return Status::corruption("table reader: vLog record key mismatch"); }
    value->assign(record.value.data(), record.value.size());
    return Status::ok();
}

} // namespace ANCHORDB_NAMESPACE
