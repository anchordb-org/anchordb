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
#include "db/db_format.h"
#include "file/file_meta.h"
#include "util/coding.h"
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ANCHORDB_NAMESPACE {

// Default index-block segment size.  The size is configurable per writer (see
// IndexBlockWriter's constructor); this is the value used when none is given.
inline constexpr size_t kDefaultBlockSize = 4096; // 4KB

// Index-block segment sizes are rounded up to a multiple of this value, so the
// uncompressed block size is always a 4KB multiple.  This is the decompressed
// size only: on-disk blocks may be compressed and need not be block-aligned.
inline constexpr size_t kIndexBlockAlignment = kDirectIOAlignment; // 4096

// An index block is never smaller than 4KB and never larger than 64KB.  These
// bounds exist because key_local_offset is a 16-bit block-local offset (so the
// whole segment must be addressable with uint16_t) and because the segment is
// aligned to kIndexBlockAlignment.
inline constexpr size_t kMinIndexBlockSize = kIndexBlockAlignment; // 4KB
inline constexpr size_t kMaxIndexBlockSize = 64 * 1024;            // 64KB

// Clamps `block_size` to [kMinIndexBlockSize, kMaxIndexBlockSize] and rounds it
// up to a kIndexBlockAlignment multiple.  Shared by the writer and reader so
// both agree on the segment size for a given configuration.
inline size_t round_index_block_size(size_t block_size) noexcept {
    if (block_size < kMinIndexBlockSize) { block_size = kMinIndexBlockSize; }
    if (block_size > kMaxIndexBlockSize) { block_size = kMaxIndexBlockSize; }
    const size_t aligned =
        ((block_size + kIndexBlockAlignment - 1) / kIndexBlockAlignment) *
        kIndexBlockAlignment;
    return aligned > kMaxIndexBlockSize ? kMaxIndexBlockSize : aligned;
}

// Fixed on-disk sizes of the index block layout below.  A slot is 19 bytes:
// key_local_offset (2B) + type (1B) + file_id (4B) + size (4B) +
// data_global_offset (8B).
inline constexpr size_t kIndexHeaderSize = 8;
inline constexpr size_t kIndexSlotSize = 19;

// Byte offsets of the fields within one forward slot.
inline constexpr size_t kSlotKeyOffsetOff = 0;     // key_local_offset (2B)
inline constexpr size_t kSlotTypeOff = 2;          // type (1B)
inline constexpr size_t kSlotFileIdOff = 3;        // file_id (4B)
inline constexpr size_t kSlotSizeOff = 7;          // size (4B)
inline constexpr size_t kSlotGlobalOffsetOff = 11; // data_global_offset (8B)

struct ValueHandle {
    EntryType type;
    // vLog file id for blob entries; always 0 for inline_ and tombstone
    // entries.
    uint32_t file_id;
    // For inline_ entries this is the absolute offset of the data record in
    // data.bin; for blob entries it is the offset of the value in the vLog file
    // identified by file_id.
    uint64_t offset;
    // For inline_ entries the total encoded size of the data record in
    // data.bin; for blob entries the size of the value in the vLog.  uint32_t
    // because oversized values go to the vLog, so an inline record always fits
    // in 32 bits.
    uint32_t size;
};

// Serializes an index block into a fixed-size, cache-friendly segment.
// Index block layout (little-endian)
// clang-format off
/*
    +-----------------------------------------------------------------------------------------------------------+
    | <size upto 64>KB ALIGNED SEGMENT BOUNDARY                                                                 |
    +-----------------------------------------------------------------------------------------------------------+
    | [HEADER (8 Bytes)]                                                                                        |
    |   - slot_count          (4 Bytes): Total elements in this block                                           |
    |   - free_space_boundary (4 Bytes): Byte offset marking the boundary where backward payload begins         |
    +-----------------------------------------------------------------------------------------------------------+
    | [FORWARD FIXED-SIZE SLOTS ARRAY (Grows forward from byte offset 8)]                                       |
    |   - Slot 0: [key_local_offset (2B) | type (1B) | file_id (4B) | size (4B) | data_global_offset (8B)]           |
    |   - Slot 1: [key_local_offset (2B) | type (1B) | file_id (4B) | size (4B) | data_global_offset (8B)]           |
    |   - Slot N: Totaling 19 bytes per slot.                                                                            |
    |                                                                                                                    |
    |       Field Metadata Definitions:                                                                                  |
    |         - type (1B): [0x0 = Tombstone] | [0x1 = Inline in data.bin] | [0x2 = Pointer to vLog]                     |
    |         - file_id (4B): vLog file id for blob entries; always 0 otherwise                                          |
    |         - size (4B): Data record size for inline entries, value size for blob entries                              |
    +-----------------------------------------------------------------------------------------------------------+
    |                                  ... UNUSED BLOCK SPACE ...                                               |
    +-----------------------------------------------------------------------------------------------------------+
    | [BACKWARD VARIABLE-LENGTH STRINGS PAYLOAD (Grows backward from absolute end of the block)]                |
    |   - String N: [key_length (2B)] [key_bytes (Variable)] [timestamp (8B: uint64_t)]                         |
    |   - String 0: Read sequentially using 2-byte key_local_offset pointers from forward slots.                |
    +-----------------------------------------------------------------------------------------------------------+
*/
// clang-format on
// The segment size is clamped to [4KB, 64KB] and rounded up to the nearest
// multiple of kIndexBlockAlignment by the constructor.
class IndexBlockWriter {
  public:
    // `block_size` is clamped to [4KB, 64KB] and aligned via
    // round_index_block_size().  `max_key_size` is the requested key limit but
    // is capped to block_size() / 4, so a key can never dominate a block.  Keys
    // longer than the effective limit are rejected by add().
    explicit IndexBlockWriter(size_t block_size = kDefaultBlockSize,
                              size_t max_key_size = kDefaultMaxKeySize)
        : block_size_(round_index_block_size(block_size)),
          requested_max_key_size_(max_key_size), buffer_(new char[block_size_]),
          free_space_boundary_(block_size_) {}

    IndexBlockWriter(const IndexBlockWriter &) = delete;
    IndexBlockWriter &operator=(const IndexBlockWriter &) = delete;

    ~IndexBlockWriter() { delete[] buffer_; }

    void reset() noexcept {
        slot_count_ = 0;
        free_space_boundary_ = block_size_;
        last_entry_offset_ = -1;
        sealed_ = false;
        finished_ = false;
    }

    // Retargets the writer to a new segment size for the next block.  Only
    // valid while the current block has no un-emitted entries; otherwise false
    // is returned and the writer is untouched.  On success the block state is
    // cleared, so the next add() starts a fresh block.
    bool set_block_size(size_t block_size);

    // Appends one entry.  Returns true if the entry was stored, or false if
    // the block is full/sealed or the key is too large to encode.  A false
    // return means the caller must emit this block and start a new one.
    bool add(std::string_view key, ValueHandle handle, uint64_t timestamp);

    bool sealed() const noexcept { return sealed_; }
    bool finished() const noexcept { return finished_; }

    // Seals the block, writes the header and returns the full segment.
    // Idempotent: safe to call more than once.
    std::string_view finish() noexcept {
        write_header();
        sealed_ = true;
        finished_ = true;
        return std::string_view(buffer_, block_size_);
    }

    // Configured (and aligned) segment size in bytes.
    size_t block_size() const noexcept { return block_size_; }

    // Effective maximum accepted key length: the requested max_key_size capped
    // to block_size() / 4.
    size_t max_key_size() const noexcept {
        const size_t cap = block_size_ / 4;
        return requested_max_key_size_ < cap ? requested_max_key_size_ : cap;
    }

    bool empty() const noexcept { return slot_count_ == 0; }

    // Greatest key stored in this block (the last successfully added key), or
    // an empty view if no entry was stored.  Returned as a zero-copy view into
    // this writer's buffer, so it is valid until reset()/destruction.
    std::string_view last_key() const noexcept {
        if (last_entry_offset_ < 0) { return std::string_view(); }
        const char *entry = buffer_ + last_entry_offset_;
        const uint16_t key_len = decode_fixed_16(entry);
        return std::string_view(entry + sizeof(uint16_t), key_len);
    }

    // Byte offset of the last stored entry within the segment, or -1 if empty.
    int64_t last_entry_offset() const noexcept { return last_entry_offset_; }

  private:
    // Encodes the header.  Must be called only when slot_count_ is final.
    void write_header() noexcept;

    size_t block_size_;
    size_t requested_max_key_size_;
    char *buffer_;
    uint32_t slot_count_{0};
    size_t free_space_boundary_;
    int64_t last_entry_offset_{-1};
    bool sealed_{false};
    bool finished_{false};
};

} // namespace ANCHORDB_NAMESPACE
