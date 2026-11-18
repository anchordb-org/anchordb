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
#include "util/coding.h"
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ANCHORDB_NAMESPACE {

// A single memtable entry. Wire format (both lengths are varint32):
// clang-format off
/*
+-------------------+-------------+----------------+-----------+---------------------+-------------+
| key_size (Varint) | key_bytes   | timestamp (8B) | type (1B) | value_size (Varint) | value_bytes |
+-------------------+-------------+----------------+-----------+---------------------+-------------+
*/
// clang-format on
//
// `key_bytes` is the user key. The class owns no bytes: encode() writes into
// caller-provided storage, while decode() and decode_key() produce views into
// the encoded buffer.
class MemtableEntry {
  public:
    // Fixed sizes of the fields that follow the user key.
    static constexpr size_t kTimestampSize = sizeof(uint64_t);            // 8
    static constexpr size_t kTypeSize = sizeof(uint8_t);                  // 1
    static constexpr size_t kKeyTrailerSize = kTimestampSize + kTypeSize; // 9

    MemtableEntry() = default;
    MemtableEntry(std::string_view key, uint64_t timestamp, EntryType type, std::string_view value)
        : key_(key), timestamp_(timestamp), type_(type), value_(value) {}

    // Exact encoded size of the key portion, including its varint32 length:
    //   varint_len(key_size) + key_size + kKeyTrailerSize
    static size_t key_encoded_size(size_t key_size) {
        return static_cast<size_t>(varint_len(key_size)) + key_size + kKeyTrailerSize;
    }

    // Exact encoded size of a complete entry:
    //   key_encoded_size(key_size) + varint_len(value_size) + value_size
    static size_t encoded_size(size_t key_size, size_t value_size) {
        return key_encoded_size(key_size) + static_cast<size_t>(varint_len(value_size)) + value_size;
    }

    // Encoded size of this entry.
    size_t encoded_size() const { return encoded_size(key_.size(), value_.size()); }

    // Writes | key | timestamp | type | and returns a pointer just past the
    // type byte.  The caller is responsible for the key_size prefix.
    char *encode_key(char *dst) const;

    // Writes the complete entry and returns a pointer just past the value.
    char *encode(char *dst) const;

    // Decodes exactly one complete entry.  Returns false when the input is
    // truncated, has trailing bytes, or carries an invalid EntryType.
    static bool decode(std::string_view encoded, MemtableEntry *out);

    // Fast key-only decode from the raw start of an entry.  Returns a pointer
    // to the value_size field (one past the type byte), or nullptr when the key
    // length prefix or type byte is invalid.  `out->value()` is left empty.
    static const char *decode_key(const char *entry, MemtableEntry *out);

    // Returns the user-key view of an entry without decoding the trailer.
    static std::string_view key_view(const char *entry);

    // Decodes the length-prefixed value that starts at `value_field`.
    static std::string_view decode_value(const char *value_field);

    std::string_view key() const { return key_; }
    uint64_t timestamp() const { return timestamp_; }
    EntryType type() const { return type_; }
    std::string_view value() const { return value_; }

  private:
    std::string_view key_;
    uint64_t timestamp_ = 0;
    EntryType type_ = EntryType::Inline;
    std::string_view value_;
};

// Builds the key portion of a memtable entry for point and range seeks:
//   | varint32(key_size) | user_key | timestamp (8B) | type (1B) |
//
// The leading key_size is not part of the logical lookup key.  It is required
// because Skiplist::seek() compares the lookup buffer against stored entries
// through the same KeyComparator, and stored entries start with key_size.  A
// lookup key that omitted the prefix would be parsed as if its first key byte
// were the length, producing wrong ordering.
//
// A seek normally passes UINT64_MAX and EntryType::Blob so the lookup key sorts
// before every stored version of the same user key.
//
// Uses a small-buffer optimization: keys up to ~190 bytes fit in the inline
// `space_` array, avoiding a heap allocation.
class LookUpKey {
  public:
    LookUpKey(std::string_view user_key, uint64_t timestamp, EntryType type);

    LookUpKey(const LookUpKey &) = delete;
    LookUpKey &operator=(const LookUpKey &) = delete;

    ~LookUpKey() {
        if (start_ != space_) delete[] start_;
    }

    // Returns the full memtable key including the varint32 length prefix.
    std::string_view memtable_key() const { return std::string_view(start_, end_ - start_); }
    // Returns just the user_key portion.
    std::string_view user_key() const {
        return std::string_view(key_start_, end_ - key_start_ - MemtableEntry::kKeyTrailerSize);
    }
    uint64_t timestamp() const { return timestamp_; }
    EntryType type() const { return type_; }

  private:
    const char *start_;     // beginning of the memtable key (varint32 prefix)
    const char *key_start_; // beginning of the user key
    const char *end_;       // one past the end of the encoded key
    uint64_t timestamp_;
    EntryType type_;
    char space_[200]; // inline storage for small-buffer optimization
};

} // namespace ANCHORDB_NAMESPACE
