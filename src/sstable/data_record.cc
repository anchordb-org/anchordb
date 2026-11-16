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

#include "sstable/data_record.h"
#include "util/coding.h"
#include "util/crc32.h"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

namespace {

// Byte offset of the CRC field within a record (the record starts with it).
constexpr size_t kCrcFieldOffset = 0;

// Byte offset at which the CRC-covered region begins (immediately after CRC).
constexpr size_t kCrcCoveredOffset = kDataRecordCrcSize;

} // namespace

size_t data_record_encoded_len(size_t key_size, size_t value_size) {
    return kDataRecordHeaderSize + kDataRecordKeySizeSize + key_size +
           static_cast<size_t>(varint_len(value_size)) + value_size;
}

size_t encode_data_record(std::string *dst, uint64_t timestamp,
                          std::string_view key, std::string_view value_bytes,
                          EntryType type) {
    assert(dst != nullptr);
    assert(key.size() <= kDataRecordMaxKeySize);
    assert(static_cast<uint8_t>(type) <= static_cast<uint8_t>(EntryType::Blob));

    const size_t start = dst->size();

    // Fixed header: CRC placeholder, timestamp, entry type.
    put_fixed_32(dst, 0); // CRC patched below once the payload exists.
    put_fixed_64(dst, timestamp);
    dst->push_back(static_cast<char>(static_cast<uint8_t>(type)));

    // Key.
    put_fixed_16(dst, static_cast<uint16_t>(key.size()));
    dst->append(key.data(), key.size());

    // Value.
    assert(value_bytes.size() <= UINT32_MAX);
    put_varint_32(dst, static_cast<uint32_t>(value_bytes.size()));
    dst->append(value_bytes.data(), value_bytes.size());

    // CRC covers timestamp through value_bytes.
    const char *payload = dst->data() + start + kCrcCoveredOffset;
    const size_t payload_size = dst->size() - (start + kCrcCoveredOffset);
    const uint32_t crc = mask(value(payload, payload_size));
    encode_fixed_32(&(*dst)[start + kCrcFieldOffset], crc);

    return dst->size() - start;
}

Status decode_data_record(std::string_view *input, ParsedDataRecord *result) {
    if (input == nullptr) {
        return Status::invalid_argument("decode_data_record: null input");
    }
    if (result == nullptr) {
        return Status::invalid_argument("decode_data_record: null result");
    }

    const std::string_view record = *input;
    if (record.size() < kDataRecordHeaderSize) {
        return Status::corruption("data record truncated: shorter than header",
                                  std::to_string(record.size()));
    }

    const uint32_t stored_crc =
        decode_fixed_32(record.data() + kCrcFieldOffset);
    std::string_view cursor = record.substr(kCrcCoveredOffset);

    const uint64_t timestamp = decode_fixed_64(cursor.data());
    cursor.remove_prefix(kDataRecordTimestampSize);

    const uint8_t type_byte = static_cast<uint8_t>(cursor[0]);
    cursor.remove_prefix(sizeof(uint8_t));
    if (type_byte > static_cast<uint8_t>(EntryType::Blob)) {
        return Status::corruption("data record invalid entry type");
    }

    if (cursor.size() < kDataRecordKeySizeSize) {
        return Status::corruption("data record truncated key");
    }
    const uint16_t key_size = decode_fixed_16(cursor.data());
    cursor.remove_prefix(kDataRecordKeySizeSize);
    if (key_size > cursor.size()) {
        return Status::corruption("data record truncated key");
    }
    const std::string_view key = cursor.substr(0, key_size);
    cursor.remove_prefix(key_size);

    uint32_t value_size = 0;
    if (!get_varint_32(&cursor, &value_size) || value_size > cursor.size()) {
        return Status::corruption("data record truncated value");
    }
    const std::string_view value_view =
        cursor.substr(0, static_cast<size_t>(value_size));
    cursor.remove_prefix(static_cast<size_t>(value_size));

    const size_t record_size = record.size() - cursor.size();
    const std::string_view covered =
        record.substr(kCrcCoveredOffset, record_size - kCrcCoveredOffset);
    if (mask(value(covered.data(), covered.size())) != stored_crc) {
        return Status::corruption("data record crc mismatch");
    }

    result->key = key;
    result->value = value_view;
    result->type = static_cast<EntryType>(type_byte);
    result->timestamp = timestamp;
    input->remove_prefix(record_size);
    return Status::ok();
}

Status decode_data_record(std::string_view input, ParsedDataRecord *result) {
    if (result == nullptr) {
        return Status::invalid_argument("decode_data_record: null result");
    }
    *result = ParsedDataRecord{};

    std::string_view cursor = input;
    Status s = decode_data_record(&cursor, result);
    if (!s.is_ok()) { return s; }
    if (!cursor.empty()) {
        *result = ParsedDataRecord{};
        return Status::corruption("trailing bytes after data record");
    }
    return Status::ok();
}

} // namespace ANCHORDB_NAMESPACE
