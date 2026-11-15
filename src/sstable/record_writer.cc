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

#include "sstable/record_writer.h"
#include "anchordb/env.h"
#include "sstable/data_record.h"
#include <cstdint>
#include <string>
#include <string_view>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

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

RecordWriter::RecordWriter(WritableFileWriter *dest, uint64_t initial_size,
                           uint64_t seal_size)
    : dest_(dest), size_(initial_size), seal_size_(seal_size) {}

Status RecordWriter::add(std::string_view key, std::string_view value_bytes,
                         EntryType type, uint64_t timestamp,
                         RecordLocation *out) {
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

Status RecordWriter::append(std::string_view encoded_record,
                            RecordLocation *out) {
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

Status RecordWriter::flush() {
    if (dest_ == nullptr) {
        return Status::invalid_argument("record_writer: null destination");
    }
    if (!status_.is_ok()) { return status_; }

    Status s = dest_->flush();
    if (!s.is_ok()) { status_ = s; }
    return status_;
}

Status RecordWriter::sync() {
    if (dest_ == nullptr) {
        return Status::invalid_argument("record_writer: null destination");
    }
    if (!status_.is_ok()) { return status_; }

    Status s = dest_->sync();
    if (!s.is_ok()) { status_ = s; }
    return status_;
}

Status RecordWriter::close() {
    if (dest_ == nullptr) {
        return Status::invalid_argument("record_writer: null destination");
    }

    // Always attempt the close so the underlying file descriptor is released,
    // even when an earlier operation failed.  A prior error stays sticky.
    Status s = dest_->close();
    if (status_.is_ok()) { status_ = s; }
    return status_;
}

} // namespace ANCHORDB_NAMESPACE
