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

#include "db/wal_writer.h"

#include "anchordb/env.h"
#include "util/coding.h"
#include "util/crc32.h"
#include <string>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

WALWriter::WALWriter(WritableFileWriter *dest, size_t max_record_size)
    : dest_(dest), max_record_size_(max_record_size) {
    // The record length field is 32-bit; a zero budget would reject every
    // record, including the empty one.
    if (max_record_size_ == 0) { max_record_size_ = 1; }
    if (max_record_size_ > UINT32_MAX) { max_record_size_ = UINT32_MAX; }

    // Write the file header up front so even an empty log is self-identifying.
    char header[kWALFileHeaderSize];
    encode_fixed_32(header, kWALFileMagic);
    encode_fixed_16(header + 4, kWALFormatVersion);
    Status s = dest_->append(std::string_view(header, kWALFileHeaderSize));
    if (!s.is_ok()) {
        status_ = s;
    } else {
        bytes_written_ += kWALFileHeaderSize;
    }
}

WALWriter::~WALWriter() = default;

Status WALWriter::append_record_(std::string_view payload) {
    // Header: crc32(4B) | payload_len(4B).  The checksum covers the length and
    // the payload, i.e. every byte after the checksum field.
    char header[kWALRecordHeaderSize];
    encode_fixed_32(header + 4, static_cast<uint32_t>(payload.size()));

    uint32_t crc = value(header + 4, 4);
    if (!payload.empty()) { crc = extend(crc, payload.data(), payload.size()); }
    encode_fixed_32(header, crc);

    Status s = dest_->append(std::string_view(header, kWALRecordHeaderSize));
    if (!s.is_ok()) {
        status_ = s;
        return status_;
    }
    if (!payload.empty()) {
        s = dest_->append(payload);
        if (!s.is_ok()) {
            status_ = s;
            return status_;
        }
    }

    bytes_written_ += kWALRecordHeaderSize + payload.size();
    return Status::ok();
}

Status WALWriter::append(std::string_view record) {
    if (!status_.is_ok()) { return status_; }

    // A record is never split.  Reject an oversized record before writing any
    // part of it; a logical rejection does not poison the writer.
    if (record.size() > max_record_size_) {
        return Status::invalid_argument("wal: record exceeds maximum size", std::to_string(record.size()));
    }
    return append_record_(record);
}

Status WALWriter::flush() {
    if (!status_.is_ok()) { return status_; }
    Status s = dest_->flush();
    if (!s.is_ok()) { status_ = s; }
    return status_;
}

Status WALWriter::sync() {
    if (!status_.is_ok()) { return status_; }
    Status s = flush();
    if (!s.is_ok()) { return s; }
    s = dest_->sync();
    if (!s.is_ok()) { status_ = s; }
    return status_;
}

} // namespace ANCHORDB_NAMESPACE
