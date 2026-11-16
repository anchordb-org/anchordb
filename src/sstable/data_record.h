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
#include "anchordb/status.h"
#include "db/db_format.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ANCHORDB_NAMESPACE {

// Self-describing record format used by data.bin and the WiscKey value log
// (vLog).  The stateless encode/decode functions below are the single
// implementation of this layout; RecordWriter, RecordReader and
// RecordBufferReader all call them.
//
// data.bin stores inline and tombstone records, and an index slot addresses a
// record by its (offset, size) in data.bin.  The vLog stores blob records with
// the same layout, but a blob's index slot addresses only the value bytes
// inside a vLog record (file_id, value offset, value size), never the record
// header.
//
// clang-format off
/*
+------------+----------------+-----------+---------------+-----------+---------------------+-------------+
| crc32 (4B) | timestamp (8B) | type (1B) | key_size (2B) | key_bytes | value_size (Varint) | value_bytes |
+------------+----------------+-----------+---------------+-----------+---------------------+-------------+
*/
// clang-format on
//
// Field details:
//   - crc32:        masked CRC32 (see util/crc32.h) computed over every byte
//                   that follows the crc32 field (timestamp .. value_bytes).
//   - timestamp:    uint64_t, little-endian.
//   - type:         EntryType byte: tombstone (0x0), inline_ (0x1), or blob
//                   (0x2).
//   - key_size:     uint16_t byte count of key_bytes.  A key can never exceed
//                   the 64KB index-block segment limit, so a fixed 16-bit
//                   field is enough.
//   - value_size:   varint32 byte count of value_bytes.  A value can never
//                   exceed 4GB, so it fits in 32 bits.

inline constexpr size_t kDataRecordCrcSize = sizeof(uint32_t);       // 4
inline constexpr size_t kDataRecordTimestampSize = sizeof(uint64_t); // 8

// Fixed prefix before key_size: crc + timestamp + type.
inline constexpr size_t kDataRecordHeaderSize =
    kDataRecordCrcSize + kDataRecordTimestampSize + sizeof(uint8_t); // 13

// Fixed on-disk size of the key_size field.
inline constexpr size_t kDataRecordKeySizeSize = sizeof(uint16_t); // 2

// Largest key a record may carry.  The writer rejects a larger key and every
// decoder reports one as corruption.
inline constexpr size_t kDataRecordMaxKeySize = UINT16_MAX;

// A fully decoded record.  `key` and `value` are zero-copy views into the
// buffer that was decoded and must not outlive it.
struct ParsedDataRecord {
    std::string_view key;
    std::string_view value;
    EntryType type = EntryType::Inline;
    uint64_t timestamp = 0;

    // True when this record holds a large value that belongs in the vLog
    // instead of inline in data.bin.  In a vLog record the value bytes are
    // still in `value`; the index slot addresses them directly.
    bool is_blob() const { return type == EntryType::Blob; }
    // True when this record marks a deleted key.
    bool is_deletion() const { return type == EntryType::Tombstone; }
};

// Returns the exact number of bytes encode_data_record() appends for a record
// with the given key and value sizes.
size_t data_record_encoded_len(size_t key_size, size_t value_size);

// Appends one record to *dst and returns the number of bytes appended.
// `value_bytes` is the raw value (for a record written to the vLog, the blob
// payload).  `key` must not exceed kDataRecordMaxKeySize and `type` must be a
// valid EntryType; both are asserted.  The masked CRC is computed and patched
// in after the payload is written.
size_t encode_data_record(std::string *dst, uint64_t timestamp,
                          std::string_view key, std::string_view value_bytes,
                          EntryType type);

// Decodes one record from the front of *input and advances *input past it.  Any
// trailing bytes are left for the next call, so this can walk a buffer holding
// several records.  The views in *result point into the original buffer.
Status decode_data_record(std::string_view *input, ParsedDataRecord *result);

// Decodes exactly one complete record from `record` (no trailing bytes).
Status decode_data_record(std::string_view input, ParsedDataRecord *result);

} // namespace ANCHORDB_NAMESPACE
