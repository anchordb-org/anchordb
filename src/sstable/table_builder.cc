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
#include "sstable/table_builder.h"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

TableBuilder::TableBuilder(const Options &options, const TableFileSet &files, const size_t writable_file_buf_size)
    : comparator_(options.comparator), compressor_(options.compressor), compression_level_(options.compression_level),
      block_writer_(options.block_size, options.max_key_size),
      summary_(static_cast<uint32_t>(block_writer_.block_size()), compressor_.compress != nullptr ? compressor_.id : 0,
               block_writer_.max_key_size()),
      filter_builder_(options.filter), env_(options.env), files_(files) {
    if (comparator_.compare == nullptr) { comparator_ = new_byte_wise_comparator(); }
    if (env_ == nullptr) {
        status_ = Status::invalid_argument("table builder: null env");
        return;
    }
    if (files_.data.empty() || files_.index.empty() || files_.summary.empty()) {
        status_ = Status::invalid_argument("table builder: missing data, index or summary file name");
        return;
    }

    Status s = open_files_(writable_file_buf_size);
    if (!s.is_ok()) { status_ = s; }
}

TableBuilder::~TableBuilder() {
    // A builder destroyed before finish() owns partial files; remove them.
    if (!finished_ && !aborted_) { abort(); }
    delete data_writer_;
    delete index_file_;
    delete data_file_;
}

Status TableBuilder::open_files_(const size_t buffer_size) {
    WritableFileWriter *raw = nullptr;
    Status s = env_->new_writable_file_writer(files_.data, &raw, buffer_size);
    if (!s.is_ok()) { return s; }
    data_file_ = raw;

    raw = nullptr;
    s = env_->new_writable_file_writer(files_.index, &raw, buffer_size);
    if (!s.is_ok()) { return s; }
    index_file_ = raw;

    // new_writable_file_writer() truncates, so the data file starts empty.
    data_writer_ = new DataRecordWriter(data_file_, 0);
    return Status::ok();
}

Status TableBuilder::check_addable_(std::string_view key, uint64_t timestamp) const {
    // Reject anything that could never fit in a single index block, so the
    // retry after a block flush in append_index_() is guaranteed to succeed.
    const size_t payload_size = sizeof(uint16_t) + key.size() + sizeof(uint64_t);
    if (key.size() > block_writer_.max_key_size() || payload_size > block_writer_.block_size()) {
        return Status::invalid_argument("table builder: key too large");
    }

    if (entry_count_ > 0) {
        const int cmp = comparator_.compare(key, last_key_);
        if (cmp < 0) { return Status::invalid_argument("table builder: keys must be added in ascending order"); }
        if (cmp == 0 && timestamp > last_timestamp_) {
            return Status::invalid_argument("table builder: timestamps must decrease for equal keys");
        }
    }
    return Status::ok();
}

Status TableBuilder::add(std::string_view key, std::string_view value, EntryType type, uint64_t timestamp) {
    if (!status_.is_ok()) { return status_; }
    if (finished_ || aborted_) { return Status::invalid_argument("table builder: already finished"); }

    // A blob value already lives in the vLog.  The memtable hands it over as an
    // encoded BlobHandle, which belongs in the index only; writing it to
    // data.bin would duplicate the handle and mislabel it as an inline record.
    if (type == EntryType::Blob) {
        BlobHandle handle;
        if (!parse_blob_handle(value, &handle)) {
            return Status::invalid_argument("table builder: blob entry requires an encoded BlobHandle");
        }
        return add_blob(key, handle, timestamp);
    }

    Status s = check_addable_(key, timestamp);
    if (!s.is_ok()) { return s; }

    // Only now that the index can accept the entry is the data record written.
    DataRecordLocation location;
    s = data_writer_->add(key, value, type, timestamp, &location);
    if (!s.is_ok()) {
        // Sticky only for real output failures; a rejected size or type wrote
        // nothing and leaves the builder usable.
        if (!s.is_invalid_argument()) { status_ = s; }
        return s;
    }
    data_size_ = data_writer_->size();

    ValueHandle handle;
    handle.type = type;
    handle.file_id = 0; // Inline and tombstone entries never reference the vLog.
    handle.offset = location.offset;
    handle.size = location.size;
    return append_index_(key, handle, timestamp);
}

Status TableBuilder::add_blob(std::string_view key, const BlobHandle &handle, uint64_t timestamp) {
    if (!status_.is_ok()) { return status_; }
    if (finished_ || aborted_) { return Status::invalid_argument("table builder: already finished"); }

    Status s = check_addable_(key, timestamp);
    if (!s.is_ok()) { return s; }

    ValueHandle value_handle;
    value_handle.type = EntryType::Blob;
    value_handle.file_id = handle.file_id;
    value_handle.offset = handle.offset;
    value_handle.size = handle.size;
    return append_index_(key, value_handle, timestamp);
}

Status TableBuilder::append_index_(std::string_view key, const ValueHandle &handle, uint64_t timestamp) {
    if (!block_writer_.add(key, handle, timestamp)) {
        // The block is full.  Seal it and retry on a fresh block;
        // check_addable_() guarantees the entry fits in an empty block.
        if (block_writer_.empty()) {
            status_ = Status::invalid_argument("table builder: index block rejected an empty block entry");
            return status_;
        }
        Status s = emit_block_();
        if (!s.is_ok()) { return s; }
        if (!block_writer_.add(key, handle, timestamp)) {
            status_ = Status::invalid_argument("table builder: entry too large for index block");
            return status_;
        }
    }

    if (entry_count_ == 0) {
        smallest_key_.assign(key);
        smallest_timestamp_ = timestamp;
        largest_timestamp_ = timestamp;
    }
    largest_key_.assign(key);
    if (timestamp < smallest_timestamp_) { smallest_timestamp_ = timestamp; }
    if (timestamp > largest_timestamp_) { largest_timestamp_ = timestamp; }

    last_key_.assign(key);
    last_timestamp_ = timestamp;
    ++entry_count_;
    filter_builder_.add(key);
    return Status::ok();
}

Status TableBuilder::emit_block_() {
    assert(!block_writer_.empty());

    const uint64_t block_offset = index_size_;
    const std::string_view block = block_writer_.finish();
    assert(block.size() == block_writer_.block_size());

    // Compress when configured; the summary records the stored (compressed)
    // length so the reader can fetch exactly this block back.
    std::string_view stored = block;
    if (compressor_.compress != nullptr) {
        // The summary records the codec id, so an active codec must identify
        // itself; otherwise a reader cannot tell how the block was stored.
        if (compressor_.id == 0) {
            status_ = Status::invalid_argument("table builder: compressor id required");
            return status_;
        }
        if (!compressor_.compress(block.data(), block.size(), &compressed_block_, compression_level_)) {
            status_ = Status::io_error(std::string_view("table builder: index block compression failed"));
            return status_;
        }
        stored = compressed_block_;
    }
    if (stored.size() > UINT32_MAX) {
        status_ = Status::invalid_argument("table builder: stored index block too large");
        return status_;
    }

    // Record the block in the summary before the writer is reset (last_key()
    // is a view into the writer's buffer).
    if (!summary_.add(block_writer_.last_key(), block_offset, static_cast<uint32_t>(stored.size()))) {
        status_ = Status::invalid_argument("table builder: separator key too large for summary index");
        return status_;
    }

    Status s = index_file_->append(stored);
    if (!s.is_ok()) {
        status_ = s;
        return status_;
    }
    index_size_ += stored.size();
    ++block_count_;
    block_writer_.reset();
    return Status::ok();
}

Status TableBuilder::write_file_(const std::string &filename, std::string_view contents, uint64_t *size_out) {
    WritableFileWriter *raw = nullptr;
    Status s = env_->new_writable_file_writer(filename, &raw);
    if (!s.is_ok()) { return s; }

    s = raw->append(contents);
    if (s.is_ok()) { s = raw->sync(); }

    // Always release the descriptor, but keep the first error.
    Status close_status = raw->close();
    delete raw;
    if (s.is_ok()) { s = close_status; }
    if (!s.is_ok()) { return s; }

    if (size_out != nullptr) { *size_out = contents.size(); }
    return Status::ok();
}

Status TableBuilder::write_filter_() {
    if (!filter_builder_.enabled()) { return Status::ok(); }
    if (files_.filter.empty()) {
        return Status::invalid_argument("table builder: filter enabled but no filter file name");
    }
    return write_file_(files_.filter, filter_builder_.finish(), &filter_size_);
}

Status TableBuilder::finish() {
    if (!status_.is_ok()) { return status_; }
    if (finished_) { return Status::ok(); }
    if (aborted_) { return Status::invalid_argument("table builder: aborted"); }

    if (!block_writer_.empty()) {
        Status s = emit_block_();
        if (!s.is_ok()) {
            // The table is already unusable; release the streams before
            // returning.  abort() still removes the partial files.
            (void)data_writer_->close();
            (void)index_file_->close();
            return s;
        }
    }

    // Make the record and index streams durable before writing the metadata
    // that describes them.
    Status s = data_writer_->sync();
    if (s.is_ok()) { s = index_file_->sync(); }
    if (s.is_ok()) { s = write_file_(files_.summary, summary_.finish(), nullptr); }
    if (s.is_ok()) { s = write_filter_(); }

    // Always release the open descriptors and preserve the first error.
    Status close_data = data_writer_->close();
    Status close_index = index_file_->close();
    if (s.is_ok()) { s = close_data; }
    if (s.is_ok()) { s = close_index; }

    if (!s.is_ok()) {
        status_ = s;
        return status_;
    }
    finished_ = true;
    return Status::ok();
}

void TableBuilder::abort() {
    if (finished_ || aborted_) { return; }
    aborted_ = true;

    // Closing first releases the descriptors even when an earlier operation
    // failed; removal is best-effort because a file may never have been
    // created.
    if (data_writer_ != nullptr) { (void)data_writer_->close(); }
    if (index_file_ != nullptr) { (void)index_file_->close(); }
    delete data_writer_;
    delete index_file_;
    delete data_file_;
    data_writer_ = nullptr;
    index_file_ = nullptr;
    data_file_ = nullptr;

    if (env_ == nullptr) { return; }
    (void)env_->remove_file(files_.data);
    (void)env_->remove_file(files_.index);
    (void)env_->remove_file(files_.summary);
    if (!files_.filter.empty()) { (void)env_->remove_file(files_.filter); }
}

} // namespace ANCHORDB_NAMESPACE
