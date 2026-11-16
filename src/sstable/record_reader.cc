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
#include "sstable/data_record.h"
#include "sstable/record_reader.h"
#include "util/coding.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

RecordReader::RecordReader(SequentialFileReader *reader) : reader_(reader) {
    if (reader_ != nullptr) { size_ = reader_->size(); }
}

RecordReader::~RecordReader() = default;

Status RecordReader::read_exact_(size_t n) {
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

Status RecordReader::read_varint_32_(uint32_t *value) {
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

Status RecordReader::next(ParsedDataRecord *result) {
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

} // namespace ANCHORDB_NAMESPACE
