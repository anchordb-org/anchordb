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
#include <utility>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

TableBuilder::TableBuilder(Env *env, const TableFileSet &files, const Comparator &comparator,
                           const Compressor &compressor, int compression_level, size_t block_size,
                           const Compressor &data_compressor, int data_compression_level, size_t data_block_size,
                           size_t max_key_size, const Filter &filter, const size_t writable_file_buf_size)
    : comparator_(comparator), compressor_(compressor), compression_level_(compression_level),
      data_compressor_(data_compressor), data_compression_level_(data_compression_level),
      block_writer_(block_size, max_key_size), data_block_(data_block_size),
      summary_(static_cast<uint32_t>(block_writer_.block_size()), compressor_.compress != nullptr ? compressor_.id : 0,
               block_writer_.max_key_size(), data_compressor_.compress != nullptr ? data_compressor_.id : 0,
               static_cast<uint32_t>(data_block_.target_size())),
      filter_builder_(filter), env_(env), files_(files) {
    if (comparator_.compare == nullptr) { comparator_ = new_byte_wise_comparator(); }
    if (env_ == nullptr) {
        status_ = Status::invalid_argument("table builder: null env");
        return;
    }
    if (files_.data.empty() || files_.index.empty() || files_.summary.empty()) {
        status_ = Status::invalid_argument("table builder: missing data, index or summary file name");
        return;
    }
    // An active codec must both identify itself (the summary records its id)
    // and report decompressed lengths (the reader needs it).
    if (data_compressor_.compress != nullptr &&
        (data_compressor_.id == 0 || data_compressor_.get_uncompressed_length == nullptr ||
         data_compressor_.uncompress == nullptr)) {
        status_ = Status::invalid_argument("table builder: invalid data compressor");
        return;
    }

    Status s = open_files_(writable_file_buf_size);
    if (!s.is_ok()) { status_ = s; }
}

TableBuilder::~TableBuilder() {
    // A builder destroyed before finish() owns partial files; remove them.
    if (!finished_ && !aborted_) { abort(); }
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

    // Compressed is a vLog record type.  The index carries only a logical Blob
    // handle; whether that record is compressed is internal to the vLog.
    if (type == EntryType::Compressed) {
        return Status::invalid_argument("table builder: compressed entries are vlog-only");
    }

    // A blob value already lives in the vLog.  The memtable hands it over as an
    // encoded BlobHandle, which belongs in the index only; writing it to
    // -Data.adb would duplicate the handle and mislabel it as an inline record.
    if (type == EntryType::Blob) {
        BlobHandle handle;
        if (!parse_blob_handle(value, &handle)) {
            return Status::invalid_argument("table builder: blob entry requires an encoded BlobHandle");
        }
        return add_blob(key, handle, timestamp);
    }

    Status s = check_addable_(key, timestamp);
    if (!s.is_ok()) { return s; }

    // Append to the current data block.  A record is never split; when the
    // block is full, flush it and retry on a fresh block.
    DataBlockBuilder::AddResult result = DataBlockBuilder::AddResult::Full;
    uint32_t in_block_offset = 0;
    while (result == DataBlockBuilder::AddResult::Full) {
        result = data_block_.add(key, value, type, timestamp, &in_block_offset);
        if (result == DataBlockBuilder::AddResult::Full) {
            s = flush_data_block_();
            if (!s.is_ok()) { return s; }
        }
    }
    if (result != DataBlockBuilder::AddResult::Added) {
        return Status::invalid_argument("table builder: record too large for a data block");
    }

    note_key_(key, timestamp);
    PendingEntry pending;
    pending.key.assign(key);
    pending.type = type;
    pending.timestamp = timestamp;
    pending.in_block_offset = in_block_offset;
    pending_.push_back(std::move(pending));

    // Bound memory: seal the block as soon as the target size is reached.
    if (data_block_.full()) { return flush_data_block_(); }
    return Status::ok();
}

Status TableBuilder::add_blob(std::string_view key, const BlobHandle &handle, uint64_t timestamp) {
    if (!status_.is_ok()) { return status_; }
    if (finished_ || aborted_) { return Status::invalid_argument("table builder: already finished"); }

    Status s = check_addable_(key, timestamp);
    if (!s.is_ok()) { return s; }

    // Flush buffered inline entries first: the blob's index slot must follow
    // them in key order.
    s = flush_data_block_();
    if (!s.is_ok()) { return s; }

    ValueHandle value_handle;
    value_handle.type = EntryType::Blob;
    value_handle.file_id = handle.file_id;
    // The vLog record offset occupies the 32-bit data_offset field, so the whole
    // vLog file is hard-capped at kMaxRecordFileSize.
    if (handle.offset > UINT32_MAX) {
        return Status::invalid_argument("table builder: vlog record offset exceeds 32 bits",
                                        std::to_string(handle.offset));
    }
    value_handle.data_offset = static_cast<uint32_t>(handle.offset);
    value_handle.size = handle.size;
    s = append_index_(key, value_handle, timestamp);
    if (!s.is_ok()) { return s; }
    note_key_(key, timestamp);
    return Status::ok();
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

    return Status::ok();
}

void TableBuilder::note_key_(std::string_view key, uint64_t timestamp) {
    if (entry_count_ == 0) {
        smallest_key_.assign(key);
        min_ts_ = timestamp;
        max_ts_ = timestamp;
    }
    largest_key_.assign(key);
    if (timestamp < min_ts_) { min_ts_ = timestamp; }
    if (timestamp > max_ts_) { max_ts_ = timestamp; }

    last_key_.assign(key);
    last_timestamp_ = timestamp;
    ++entry_count_;
    filter_builder_.add(key);
}

Status TableBuilder::flush_data_block_() {
    if (data_block_.empty()) {
        pending_.clear();
        return Status::ok();
    }

    const uint64_t block_offset = data_size_;
    const std::string_view block = data_block_.finish();

    std::string_view stored = block;
    if (data_compressor_.compress != nullptr) {
        if (!data_compressor_.compress(block.data(), block.size(), &compressed_data_block_, data_compression_level_)) {
            status_ = Status::io_error(std::string_view("table builder: data block compression failed"));
            return status_;
        }
        // The table-level data codec id is recorded in the summary, so every
        // block must be stored through that codec even when it does not shrink.
        stored = compressed_data_block_;
    }
    const size_t stored_block_size = kDataBlockHeaderSize + stored.size();
    if (stored_block_size > UINT32_MAX) {
        status_ = Status::invalid_argument("table builder: stored data block too large");
        return status_;
    }

    // | uncompressed_size (4B) | stored bytes |; the index slot records the
    // full stored size (header included) and the block's absolute offset.
    data_block_frame_.clear();
    data_block_frame_.reserve(kDataBlockHeaderSize + stored.size());
    append_data_block_header(&data_block_frame_, static_cast<uint32_t>(block.size()));
    data_block_frame_.append(stored.data(), stored.size());

    // An inline index slot stores the block's absolute offset in a 32-bit
    // field, so the whole -Data.adb file is capped at kMaxRecordFileSize.
    // Check before writing: a rejected block must not extend the file past the
    // limit, which would leave an offset that no index slot could address.
    if (!record_file_fits(data_size_, data_block_frame_.size())) {
        status_ = Status::invalid_argument("table builder: data file would exceed the 4 GiB size limit",
                                           std::to_string(data_size_));
        return status_;
    }

    Status s = data_file_->append(data_block_frame_);
    if (!s.is_ok()) {
        status_ = s;
        return status_;
    }
    data_size_ += data_block_frame_.size();

    // Every buffered entry points into this block: the 4-byte field is the
    // absolute data-block offset, data_offset is the in-block record offset,
    // and size is the stored block size.
    for (const PendingEntry &entry : pending_) {
        ValueHandle handle;
        handle.type = entry.type;
        handle.file_id = static_cast<uint32_t>(block_offset);
        handle.data_offset = entry.in_block_offset;
        handle.size = static_cast<uint32_t>(stored_block_size);
        s = append_index_(entry.key, handle, entry.timestamp);
        if (!s.is_ok()) { return s; }
    }

    pending_.clear();
    data_block_.reset();
    compressed_data_block_.clear();
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

    // Flush the trailing data block first; its entries may seal index blocks.
    Status s = flush_data_block_();
    if (s.is_ok() && !block_writer_.empty()) { s = emit_block_(); }
    if (!s.is_ok()) {
        // The table is already unusable; release the streams before returning.
        // abort() still removes the partial files.
        (void)data_file_->close();
        (void)index_file_->close();
        return s;
    }

    // Make the record and index streams durable before writing the metadata
    // that describes them.
    s = data_file_->sync();
    if (s.is_ok()) { s = index_file_->sync(); }
    if (s.is_ok()) { s = write_file_(files_.summary, summary_.finish(), nullptr); }
    if (s.is_ok()) { s = write_filter_(); }

    // Always release the open descriptors and preserve the first error.
    Status close_data = data_file_->close();
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
    if (data_file_ != nullptr) { (void)data_file_->close(); }
    if (index_file_ != nullptr) { (void)index_file_->close(); }
    delete index_file_;
    delete data_file_;
    index_file_ = nullptr;
    data_file_ = nullptr;

    if (env_ == nullptr) { return; }
    (void)env_->remove_file(files_.data);
    (void)env_->remove_file(files_.index);
    (void)env_->remove_file(files_.summary);
    if (!files_.filter.empty()) { (void)env_->remove_file(files_.filter); }
}

} // namespace ANCHORDB_NAMESPACE
