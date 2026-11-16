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
#include "data_record.h"
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

namespace {

// A record's location is stored as a uint32_t size, so reject anything larger
// up front rather than silently truncating it.
Status check_record_size(size_t record_len) {
    if (record_len > UINT32_MAX) {
        return Status::invalid_argument(
            "record_writer: record exceeds 32-bit size",
            std::to_string(record_len));
    }
    return Status::ok();
}

} // namespace

DataRecordWriter::DataRecordWriter(WritableFileWriter *dest,
                                   uint64_t initial_size, uint64_t seal_size)
    : dest_(dest), size_(initial_size), seal_size_(seal_size) {}

Status DataRecordWriter::add(std::string_view key, std::string_view value_bytes,
                             EntryType type, uint64_t timestamp,
                             DataRecordLocation *out) {
    if (!status_.is_ok()) { return status_; }
    if (dest_ == nullptr) {
        return Status::invalid_argument("record_writer: null destination");
    }
    if (static_cast<uint8_t>(type) > static_cast<uint8_t>(EntryType::Blob)) {
        return Status::invalid_argument("record_writer: invalid entry type");
    }
    if (key.size() > kDataRecordMaxKeySize) {
        return Status::invalid_argument("record_writer: key too large",
                                        std::to_string(key.size()));
    }

    const size_t record_len =
        data_record_encoded_len(key.size(), value_bytes.size());
    Status s = check_record_size(record_len);
    if (!s.is_ok()) { return s; }

    std::string record;
    record.reserve(record_len);
    encode_data_record(&record, timestamp, key, value_bytes, type);
    return append(record, out);
}

Status DataRecordWriter::append(std::string_view encoded_record,
                                DataRecordLocation *out) {
    if (!status_.is_ok()) { return status_; }
    if (dest_ == nullptr) {
        return Status::invalid_argument("record_writer: null destination");
    }

    Status s = check_record_size(encoded_record.size());
    if (!s.is_ok()) { return s; }

    s = dest_->append(encoded_record);
    if (!s.is_ok()) {
        status_ = s;
        return status_;
    }

    if (out != nullptr) {
        out->offset = size_;
        out->size = static_cast<uint32_t>(encoded_record.size());
    }
    size_ += encoded_record.size();
    return Status::ok();
}

Status DataRecordWriter::flush() {
    if (dest_ == nullptr) {
        return Status::invalid_argument("record_writer: null destination");
    }
    if (!status_.is_ok()) { return status_; }

    Status s = dest_->flush();
    if (!s.is_ok()) { status_ = s; }
    return status_;
}

Status DataRecordWriter::sync() {
    if (dest_ == nullptr) {
        return Status::invalid_argument("record_writer: null destination");
    }
    if (!status_.is_ok()) { return status_; }

    Status s = dest_->sync();
    if (!s.is_ok()) { status_ = s; }
    return status_;
}

Status DataRecordWriter::close() {
    if (dest_ == nullptr) {
        return Status::invalid_argument("record_writer: null destination");
    }

    // Always attempt the close so the underlying file descriptor is released,
    // even when an earlier operation failed.  A prior error stays sticky.
    Status s = dest_->close();
    if (status_.is_ok()) { status_ = s; }
    return status_;
}

DataRecordReader::DataRecordReader(SequentialFileReader *reader)
    : reader_(reader) {
    if (reader_ != nullptr) { size_ = reader_->size(); }
}

DataRecordReader::~DataRecordReader() = default;

Status DataRecordReader::read_exact_(size_t n) {
    // Bounds check against the known file size; no end-of-file probing.
    if (n > size_ - (pos_ + buffer_.size())) {
        return Status::corruption("data record truncated", std::to_string(n));
    }

    const size_t old = buffer_.size();
    buffer_.resize(old + n);

    char *dst = buffer_.data() + old;
    std::string_view view;
    Status s = reader_->read(n, &view, dst);
    if (!s.is_ok()) {
        buffer_.resize(old);
        return s;
    }

    // The file reader normally copies into dst; move the bytes if an
    // implementation instead exposes its own buffer.
    if (!view.empty() && view.data() != dst) {
        std::memmove(dst, view.data(), view.size());
    }
    if (view.size() != n) {
        buffer_.resize(old);
        return Status::corruption("data record short read",
                                  std::to_string(view.size()));
    }
    return Status::ok();
}

Status DataRecordReader::read_varint_32_(uint32_t *value) {
    const size_t start = buffer_.size();

    // A varint32 is at most 5 bytes.  Read it one byte at a time so the stream
    // is never read past the varint.
    for (int i = 0; i < 5; ++i) {
        Status s = read_exact_(1);
        if (!s.is_ok()) { return s; }

        std::string_view pending(buffer_.data() + start,
                                 buffer_.size() - start);
        if (get_varint_32(&pending, value)) { return Status::ok(); }
    }
    return Status::corruption("data record truncated varint");
}

Status DataRecordReader::next(ParsedDataRecord *result) {
    if (result == nullptr) {
        return Status::invalid_argument("record_reader: null result");
    }
    *result = ParsedDataRecord{};
    if (reader_ == nullptr) {
        return Status::invalid_argument("record_reader: null source");
    }
    if (done()) {
        return Status::invalid_argument("record_reader: at end of file");
    }
    buffer_.clear();

    // crc (4B) | timestamp (8B) | type (1B).
    Status s = read_exact_(kDataRecordHeaderSize);
    if (!s.is_ok()) { return s; }
    const uint8_t type =
        static_cast<uint8_t>(buffer_[kDataRecordHeaderSize - 1]);
    if (type > static_cast<uint8_t>(EntryType::Blob)) {
        return Status::corruption("data record invalid entry type");
    }

    // key_size (uint16_t) | key bytes.
    s = read_exact_(kDataRecordKeySizeSize);
    if (!s.is_ok()) { return s; }
    const uint16_t key_size =
        decode_fixed_16(buffer_.data() + kDataRecordHeaderSize);
    s = read_exact_(key_size);
    if (!s.is_ok()) { return s; }

    // value_size (varint32) | value bytes.
    uint32_t value_size = 0;
    s = read_varint_32_(&value_size);
    if (!s.is_ok()) { return s; }
    if (value_size > size_ - (pos_ + buffer_.size())) {
        return Status::corruption("data record truncated value",
                                  std::to_string(value_size));
    }
    s = read_exact_(value_size);
    if (!s.is_ok()) { return s; }

    // The fields just read account for exactly one encoded record.
    const size_t record_len = data_record_encoded_len(key_size, value_size);
    if (buffer_.size() != record_len) {
        return Status::corruption("data record length mismatch");
    }
    buffer_.resize(record_len);
    pos_ += record_len;

    return decode_data_record(std::string_view(buffer_.data(), record_len),
                              result);
}

Status DataRecordBufferReader::next(ParsedDataRecord *result, bool *done) {
    if (result == nullptr || done == nullptr) {
        return Status::invalid_argument("record_buffer_reader: null argument");
    }
    *result = ParsedDataRecord{};
    *done = false;

    if (pos_ >= buffer_.size()) {
        *done = true;
        return Status::ok();
    }

    std::string_view cursor = buffer_.substr(pos_);
    Status s = decode_data_record(&cursor, result);
    if (!s.is_ok()) { return s; }

    pos_ = buffer_.size() - cursor.size();
    return Status::ok();
}

} // namespace ANCHORDB_NAMESPACE
