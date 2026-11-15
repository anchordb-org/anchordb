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

#include "sstable/idx_block_writer.h"
#include "util/coding.h"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

void IndexBlockWriter::write_header() noexcept {
    encode_fixed_32(buffer_, slot_count_);
    // The boundary is a block-local offset; 32 bits cover every configurable
    // block size, so the real value is stored directly (empty blocks store
    // block_size_).
    encode_fixed_32(buffer_ + sizeof(uint32_t),
                    static_cast<uint32_t>(free_space_boundary_));
}

bool IndexBlockWriter::set_block_size(size_t block_size) {
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

bool IndexBlockWriter::add(std::string_view key, ValueHandle handle,
                           uint64_t timestamp) {
    assert(!finished_);

    // Keys are bounded by the effective max_key_size (which is itself capped to
    // a quarter of the block); a rejected entry can never fit a segment and
    // must be handled by the caller.
    const size_t payload_size =
        sizeof(uint16_t) + key.size() + sizeof(uint64_t);
    if (finished_ || sealed_ || key.size() > max_key_size() ||
        payload_size > block_size_) {
        return false;
    }

    const size_t forward_end =
        kIndexHeaderSize + static_cast<size_t>(slot_count_) * kIndexSlotSize;
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

    // Inline and tombstone entries never reference a vLog file.
    const uint32_t file_id =
        handle.type == EntryType::Blob ? handle.file_id : 0;

    // Fixed-size slot grows up from the header.
    char *slot = buffer_ + forward_end;
    encode_fixed_16(slot + kSlotKeyOffsetOff,
                    static_cast<uint16_t>(new_boundary)); // key offset
    // Field order matches the documented slot layout.
    slot[kSlotTypeOff] = static_cast<char>(static_cast<uint8_t>(handle.type));
    encode_fixed_32(slot + kSlotFileIdOff, file_id);
    encode_fixed_32(slot + kSlotSizeOff, handle.size);
    encode_fixed_64(slot + kSlotGlobalOffsetOff, handle.offset);

    ++slot_count_;
    return true;
}

} // namespace ANCHORDB_NAMESPACE
