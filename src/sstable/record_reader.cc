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

#include "sstable/record_reader.h"
#include "anchordb/env.h"
#include "sstable/data_record.h"
#include "util/coding.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

namespace {

// Grow the assembly buffer in bounded steps.  A record length is read from the
// file before its payload, so a corrupt length must not trigger a huge
// allocation when the file actually ends early.
constexpr size_t kFillChunkSize = 64 * 1024;

} // namespace

RecordReader::RecordReader(SequentialFileReader *reader) : reader_(reader) {}

RecordReader::~RecordReader() = default;

Status RecordReader::fill_(size_t n, std::string *scratch, bool *eof) {
    *eof = false;
    const size_t start = scratch->size();

    size_t got = 0;
    while (got < n) {
        const size_t want = std::min(n - got, kFillChunkSize);
        scratch->resize(start + got + want);

        char *dst = scratch->data() + start + got;
        std::string_view view;
        Status s = reader_->read(want, &view, dst);
        if (!s.is_ok()) { return s; }
        bytes_read_ += view.size();

        if (view.empty()) {
            *eof = true;
            scratch->resize(start + got);
            return Status::ok();
        }
        // The reader normally copies into dst; move the bytes if an
        // implementation instead exposes its own buffer.
        if (view.data() != dst) { std::memmove(dst, view.data(), view.size()); }
        got += view.size();
    }
    return Status::ok();
}

Status RecordReader::read_varint_(std::string *scratch, uint64_t *value,
                                  bool *eof) {
    *eof = false;
    const size_t start = scratch->size();

    // A varint64 is at most 10 bytes.  Read it one byte at a time so the stream
    // is never read past the varint.  A buffered SequentialFileReader serves
    // these small reads from its read-ahead buffer.
    for (size_t i = 0; i < 10; ++i) {
        bool at_eof = false;
        Status s = fill_(1, scratch, &at_eof);
        if (!s.is_ok()) { return s; }
        if (at_eof) {
            *eof = true;
            return Status::ok();
        }

        std::string_view pending(scratch->data() + start,
                                 scratch->size() - start);
        if (get_varint_64(&pending, value)) { return Status::ok(); }
    }
    return Status::corruption("data record truncated varint");
}

Status RecordReader::read_record(ParsedDataRecord *result, bool *eof,
                                 std::string *scratch) {
    if (result == nullptr || eof == nullptr || scratch == nullptr) {
        return Status::invalid_argument("record_reader: null argument");
    }
    *result = ParsedDataRecord{};
    *eof = false;
    if (reader_ == nullptr) {
        return Status::invalid_argument("record_reader: null source");
    }
    scratch->clear();

    // crc (4B) | timestamp (8B) | type (1B).
    bool at_eof = false;
    Status s = fill_(kDataRecordHeaderSize, scratch, &at_eof);
    if (!s.is_ok()) { return s; }
    if (at_eof) {
        *eof = true;
        scratch->clear();
        return Status::ok();
    }

    // key_size varint | key bytes.
    uint64_t key_size = 0;
    s = read_varint_(scratch, &key_size, &at_eof);
    if (!s.is_ok()) { return s; }
    if (at_eof) {
        *eof = true;
        scratch->clear();
        return Status::ok();
    }
    if (key_size > kDataRecordMaxKeySize) {
        return Status::corruption("data record key too large",
                                  std::to_string(key_size));
    }
    s = fill_(static_cast<size_t>(key_size), scratch, &at_eof);
    if (!s.is_ok()) { return s; }
    if (at_eof) {
        *eof = true;
        scratch->clear();
        return Status::ok();
    }

    // value_size varint | value bytes.
    uint64_t value_size = 0;
    s = read_varint_(scratch, &value_size, &at_eof);
    if (!s.is_ok()) { return s; }
    if (at_eof) {
        *eof = true;
        scratch->clear();
        return Status::ok();
    }
    const size_t prefix_len = scratch->size();
    if (value_size > kRecordReaderMaxRecordSize ||
        prefix_len > kRecordReaderMaxRecordSize ||
        value_size > kRecordReaderMaxRecordSize - prefix_len) {
        return Status::corruption("data record size exceeds maximum",
                                  std::to_string(value_size));
    }
    s = fill_(static_cast<size_t>(value_size), scratch, &at_eof);
    if (!s.is_ok()) { return s; }
    if (at_eof) {
        *eof = true;
        scratch->clear();
        return Status::ok();
    }

    return decode_data_record(
        std::string_view(scratch->data(), scratch->size()), result);
}

} // namespace ANCHORDB_NAMESPACE
