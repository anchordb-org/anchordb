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

#include "sstable/index_block.h"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "anchordb/anchordb_namespace.h"
#include "anchordb/comparator.h"

namespace ANCHORDB_NAMESPACE {

void IndexBlockBuilder::write_header() noexcept {
    encode_fixed_32(buffer_, slot_count_);
    // The boundary is a block-local offset; 32 bits cover every configurable
    // block size, so the real value is stored directly (empty blocks store
    // block_size_).
    encode_fixed_32(buffer_ + sizeof(uint32_t), static_cast<uint32_t>(free_space_boundary_));
}

bool IndexBlockBuilder::set_block_size(size_t block_size) {
    // Resizing would invalidate offsets already written, so it is safe only
    // when the block is empty or already finished.
    if (slot_count_ != 0 && !finished_) { return false; }

    const size_t new_size = round_index_block_size(block_size);
    if (new_size != block_size_) {
        char *new_buffer = new char[new_size];
        delete[] buffer_;
        buffer_ = new_buffer;
        block_size_ = new_size;
    }
    reset();
    return true;
}

bool IndexBlockBuilder::add(std::string_view key, ValueHandle handle, uint64_t timestamp) {
    assert(!finished_);

    // Keys are bounded by the effective max_key_size (which is itself capped to
    // a quarter of the block); a rejected entry can never fit a segment and
    // must be handled by the caller.
    const size_t payload_size = sizeof(uint16_t) + key.size() + sizeof(uint64_t);
    if (finished_ || sealed_ || key.size() > max_key_size() || payload_size > block_size_) { return false; }

    const size_t forward_end = kIndexHeaderSize + static_cast<size_t>(slot_count_) * kIndexSlotSize;
    const size_t required = kIndexSlotSize + payload_size;
    const size_t available = free_space_boundary_ - forward_end;

    // Not enough room for this entry: seal the block and let the caller start a
    // new one.  The rejected entry is not stored.
    if (required > available) {
        write_header();
        sealed_ = true;
        return false;
    }

    // Payload grows down from the free-space boundary.
    const size_t new_boundary = free_space_boundary_ - payload_size;
    if (new_boundary > UINT16_MAX) {
        write_header();
        sealed_ = true;
        return false;
    }
    char *payload = buffer_ + new_boundary;
    encode_fixed_16(payload, static_cast<uint16_t>(key.size()));
    payload += sizeof(uint16_t);
    std::memcpy(payload, key.data(), key.size());
    payload += key.size();
    encode_fixed_64(payload, timestamp);
    free_space_boundary_ = new_boundary;
    last_entry_offset_ = static_cast<int64_t>(new_boundary);

    // Fixed-size slot grows up from the header.  The 4-byte field is a vLog
    // file id for blobs and an absolute data-block offset for inline/tombstone
    // entries.
    const uint32_t file_id = handle.file_id;
    char *slot = buffer_ + forward_end;
    encode_fixed_16(slot + kSlotKeyOffsetOff,
                    static_cast<uint16_t>(new_boundary)); // key offset
    // Field order matches the documented slot layout.
    slot[kSlotTypeOff] = static_cast<char>(static_cast<uint8_t>(handle.type));
    encode_fixed_32(slot + kSlotFileOrBlockOffsetOff, file_id);
    encode_fixed_32(slot + kSlotSizeOff, handle.size);
    encode_fixed_32(slot + kSlotDataOffsetOff, handle.data_offset);

    ++slot_count_;
    return true;
}

IndexBlockReader::IndexBlockReader(std::string_view block, size_t block_size)
    : data_(block.data()), size_(block.size()), block_size_(round_index_block_size(block_size)), slot_count_(0),
      valid_(false) {
    // A block is always exactly one configured (aligned) segment.  Only the
    // first block_size_ bytes are parsed; extra trailing bytes are ignored.
    if (size_ < kIndexHeaderSize || size_ < block_size_) { return; }

    slot_count_ = decode_fixed_32(data_);
    const uint32_t free_space_boundary = decode_fixed_32(data_ + sizeof(uint32_t));

    const size_t slots_end = kIndexHeaderSize + static_cast<size_t>(slot_count_) * kIndexSlotSize;
    if (slots_end > block_size_ || free_space_boundary > block_size_) { return; }

    // The backward payload must start at or after the forward slot array once
    // at least one entry has been stored.
    if (slot_count_ != 0 && free_space_boundary < slots_end) { return; }

    // Validate every entry so callers can decode without bounds checks.
    for (size_t i = 0; i < slot_count_; ++i) {
        const char *slot = data_ + kIndexHeaderSize + i * kIndexSlotSize;
        const uint16_t key_off = decode_fixed_16(slot);

        // Payloads grow down from the free-space boundary, so every payload
        // starts at or above it; it must hold at least its 2-byte length
        // prefix and stay inside the segment.
        if (key_off < free_space_boundary || static_cast<size_t>(key_off) + sizeof(uint16_t) > block_size_) { return; }

        const uint8_t type = static_cast<uint8_t>(slot[kSlotTypeOff]);
        if (type > static_cast<uint8_t>(EntryType::Blob)) { return; }

        const uint16_t key_len = decode_fixed_16(data_ + key_off);
        const size_t key_start = static_cast<size_t>(key_off) + sizeof(uint16_t);
        if (key_start + key_len + sizeof(uint64_t) > block_size_) { return; }
    }

    valid_ = true;
}

size_t IndexBlockReader::lower_bound(std::string_view target, const Comparator &cmp) const {
    size_t lo = 0;
    size_t hi = slot_count_;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (cmp.compare(key(mid), target) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

IndexBlockEntry IndexBlockReader::entry(size_t index) const {
    assert(valid_);
    assert(index < slot_count_);

    const char *slot = data_ + kIndexHeaderSize + index * kIndexSlotSize;
    const uint16_t key_off = decode_fixed_16(slot);
    const char *payload = data_ + key_off;
    const uint16_t key_len = decode_fixed_16(payload);

    IndexBlockEntry result;
    result.key = std::string_view(payload + sizeof(uint16_t), key_len);
    result.handle.type = static_cast<EntryType>(static_cast<uint8_t>(slot[kSlotTypeOff]));
    result.handle.file_id = decode_fixed_32(slot + kSlotFileOrBlockOffsetOff);
    result.handle.size = decode_fixed_32(slot + kSlotSizeOff);
    result.handle.data_offset = decode_fixed_32(slot + kSlotDataOffsetOff);
    result.timestamp = decode_fixed_64(payload + sizeof(uint16_t) + key_len);
    return result;
}

bool IndexBlockReader::decode_value(std::string_view value, ValueHandle *handle, uint64_t *timestamp) {
    if (value.size() != kIndexValueSize) { return false; }

    const char *p = value.data();
    if (handle != nullptr) {
        handle->type = static_cast<EntryType>(static_cast<uint8_t>(p[0]));
        handle->file_id = decode_fixed_32(p + 1);
        handle->size = decode_fixed_32(p + 1 + sizeof(uint32_t));
        handle->data_offset = decode_fixed_32(p + 1 + 2 * sizeof(uint32_t));
    }
    if (timestamp != nullptr) { *timestamp = decode_fixed_64(p + 1 + 3 * sizeof(uint32_t)); }
    return true;
}

namespace {

// Iterates the entries of an index block in ascending key order.  key() exposes
// the entry key and value() exposes the packed handle+timestamp record.
class IndexBlockIter : public Iterator {
  public:
    IndexBlockIter(const IndexBlockReader &reader, Comparator comparator)
        : reader_(reader), comparator_(comparator), index_(reader.size()) {
        set_current();
    }

    bool valid() const override { return index_ < reader_.size(); }

    void seek_to_first() override {
        index_ = reader_.empty() ? reader_.size() : 0;
        set_current();
    }

    void seek_to_last() override {
        index_ = reader_.empty() ? reader_.size() : reader_.size() - 1;
        set_current();
    }

    void seek(std::string_view target) override {
        index_ = reader_.lower_bound(target, comparator_);
        set_current();
    }

    void next() override {
        assert(valid());
        ++index_;
        set_current();
    }

    void prev() override {
        assert(valid());
        // Moving before the first entry makes the iterator invalid.
        index_ = (index_ == 0) ? reader_.size() : index_ - 1;
        set_current();
    }

    std::string_view key() const override {
        assert(valid());
        return key_;
    }

    std::string_view value() const override {
        assert(valid());
        return value_;
    }

    Status status() const override { return Status::ok(); }

  private:
    void set_current() {
        if (index_ < reader_.size()) {
            const IndexBlockEntry current = reader_.entry(index_);
            key_ = current.key;
            value_buf_[0] = static_cast<char>(static_cast<uint8_t>(current.handle.type));
            encode_fixed_32(value_buf_ + 1, current.handle.file_id);
            encode_fixed_32(value_buf_ + 1 + sizeof(uint32_t), current.handle.size);
            encode_fixed_32(value_buf_ + 1 + 2 * sizeof(uint32_t), current.handle.data_offset);
            encode_fixed_64(value_buf_ + 1 + 3 * sizeof(uint32_t), current.timestamp);
            value_ = std::string_view(value_buf_, kIndexValueSize);
        } else {
            key_ = std::string_view();
            value_ = std::string_view();
        }
    }

    const IndexBlockReader &reader_;
    const Comparator comparator_;
    size_t index_;
    std::string_view key_;
    std::string_view value_;
    // Backing storage for value_.  Stable because the iterator is not copyable
    // or movable.
    char value_buf_[kIndexValueSize]{};
};

} // namespace

Iterator *IndexBlockReader::new_iterator(Comparator comparator) const {
    if (!valid_) { return new_error_iterator(Status::corruption("bad index block contents")); }
    if (empty()) { return new_empty_iterator(); }
    if (comparator.compare == nullptr) { comparator = new_byte_wise_comparator(); }
    return new IndexBlockIter(*this, comparator);
}

} // namespace ANCHORDB_NAMESPACE
