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

#include "sstable/summary.h"
#include "util/coding.h"
#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

#include "anchordb/anchordb_namespace.h"
#include "anchordb/comparator.h"

namespace ANCHORDB_NAMESPACE {

SummaryBuilder::SummaryBuilder(uint32_t block_size, uint32_t compressor_id, size_t max_key_size,
                               uint32_t data_compressor_id, uint32_t data_block_size)
    : block_size_(block_size), compressor_id_(compressor_id), data_compressor_id_(data_compressor_id),
      data_block_size_(data_block_size), max_key_size_(max_key_size) {
    write_header_();
}

void SummaryBuilder::write_header_() {
    char header_buf[kSummaryHeaderSize];
    encode_fixed_32(header_buf, kSummaryMagic);
    encode_fixed_32(header_buf + sizeof(uint32_t), kSummaryFormatVersion);
    encode_fixed_32(header_buf + 2 * sizeof(uint32_t), block_size_);
    encode_fixed_32(header_buf + 3 * sizeof(uint32_t), compressor_id_);
    encode_fixed_32(header_buf + 4 * sizeof(uint32_t), data_compressor_id_);
    encode_fixed_32(header_buf + 5 * sizeof(uint32_t), data_block_size_);
    buffer_.append(header_buf, kSummaryHeaderSize);
}

bool SummaryBuilder::add(std::string_view key, uint64_t block_offset, uint32_t compressed_size) {
    if (key.size() > max_key_size_) { return false; }

    char length_buf[kSummaryKeyLenSize];
    encode_fixed_32(length_buf, static_cast<uint32_t>(key.size()));
    buffer_.append(length_buf, kSummaryKeyLenSize);

    buffer_.append(key.data(), key.size());

    char offset_buf[kSummaryOffsetSize];
    encode_fixed_64(offset_buf, block_offset);
    buffer_.append(offset_buf, kSummaryOffsetSize);

    char compressed_size_buf[kSummaryCompressedSizeSize];
    encode_fixed_32(compressed_size_buf, compressed_size);
    buffer_.append(compressed_size_buf, kSummaryCompressedSizeSize);

    ++entry_count_;
    return true;
}

SummaryReader::SummaryReader(std::string_view buffer, Comparator comparator)
    : comparator_(comparator), block_size_(0), compressor_id_(0), data_compressor_id_(0), data_block_size_(0),
      valid_(true) {
    if (comparator_.compare == nullptr) { comparator_ = new_byte_wise_comparator(); }

    const char *p = buffer.data();
    const char *limit = p + buffer.size();

    if (static_cast<size_t>(limit - p) < kSummaryHeaderSize) {
        valid_ = false;
        return;
    }
    if (decode_fixed_32(p) != kSummaryMagic) {
        valid_ = false;
        return;
    }
    const uint32_t format_version = decode_fixed_32(p + sizeof(uint32_t));
    if (format_version != kSummaryFormatVersion) {
        valid_ = false;
        return;
    }
    block_size_ = decode_fixed_32(p + 2 * sizeof(uint32_t));
    compressor_id_ = decode_fixed_32(p + 3 * sizeof(uint32_t));
    data_compressor_id_ = decode_fixed_32(p + 4 * sizeof(uint32_t));
    data_block_size_ = decode_fixed_32(p + 5 * sizeof(uint32_t));
    p += kSummaryHeaderSize;
    if (block_size_ == 0 || data_block_size_ == 0) {
        valid_ = false;
        return;
    }

    while (p < limit) {
        if (static_cast<size_t>(limit - p) < kSummaryKeyLenSize) {
            valid_ = false;
            break;
        }
        const uint32_t key_len = decode_fixed_32(p);
        p += kSummaryKeyLenSize;

        if (static_cast<size_t>(limit - p) <
            static_cast<size_t>(key_len) + kSummaryOffsetSize + kSummaryCompressedSizeSize) {
            valid_ = false;
            break;
        }

        SummaryEntry entry;
        entry.key = std::string_view(p, key_len);
        p += key_len;
        entry.block_offset = decode_fixed_64(p);
        p += kSummaryOffsetSize;
        entry.compressed_size = decode_fixed_32(p);
        p += kSummaryCompressedSizeSize;

        entries_.push_back(entry);
    }

    if (!valid_) { entries_.clear(); }
}

bool SummaryReader::seek(std::string_view target, uint64_t *block_offset) const {
    return seek(target, block_offset, nullptr);
}

bool SummaryReader::seek(std::string_view target, uint64_t *block_offset, uint32_t *compressed_size) const {
    if (!valid_ || entries_.empty()) { return false; }

    const size_t lo = lower_bound(target);

    if (lo == entries_.size()) {
        // target is greater than every stored key.
        return false;
    }

    if (block_offset != nullptr) { *block_offset = entries_[lo].block_offset; }
    if (compressed_size != nullptr) { *compressed_size = entries_[lo].compressed_size; }
    return true;
}

size_t SummaryReader::lower_bound(std::string_view target) const {
    if (!valid_) { return entries_.size(); }

    // First key >= target.
    const auto it = std::lower_bound(
        entries_.begin(), entries_.end(), target,
        [this](const SummaryEntry &entry, std::string_view key) { return comparator_.compare(entry.key, key) < 0; });
    return static_cast<size_t>(it - entries_.begin());
}

} // namespace ANCHORDB_NAMESPACE
