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

#include "db/wal_reader.h"

#include "anchordb/env.h"
#include "util/coding.h"
#include "util/crc32.h"
#include <string>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

WALReader::WALReader(SequentialFileReader *reader, size_t max_record_size, bool verify_checksums)
    : reader_(reader), max_record_size_(max_record_size), verify_checksums_(verify_checksums) {
    if (max_record_size_ == 0) { max_record_size_ = 1; }
    if (max_record_size_ > UINT32_MAX) { max_record_size_ = UINT32_MAX; }
}

WALReader::~WALReader() = default;

Status WALReader::read_file_header_(bool *empty) {
    *empty = false;

    char header[kWALFileHeaderSize];
    std::string_view view;
    Status s = reader_->read(kWALFileHeaderSize, &view, header);
    if (!s.is_ok()) { return s; }
    bytes_read_ += view.size();

    if (view.size() == 0) {
        // No bytes at all: an empty log is valid.
        *empty = true;
        return Status::ok();
    }
    if (view.size() < kWALFileHeaderSize) { return Status::corruption("wal: truncated file header"); }
    if (decode_fixed_32(view.data()) != kWALFileMagic) { return Status::corruption("wal: bad file magic"); }
    if (decode_fixed_16(view.data() + 4) != kWALFormatVersion) {
        return Status::corruption("wal: unsupported format version");
    }
    return Status::ok();
}

Status WALReader::read_record(std::string_view *record, bool *eof, std::string *scratch) {
    *record = std::string_view();
    *eof = false;

    if (!header_read_) {
        bool empty = false;
        Status s = read_file_header_(&empty);
        if (!s.is_ok()) { return s; }
        header_read_ = true;
        if (empty) {
            *eof = true;
            return Status::ok();
        }
    }

    // Read the fixed-size record header.  SequentialFileReader::read copies
    // into the caller's buffer and reports a short count only at end of file.
    char header[kWALRecordHeaderSize];
    std::string_view hview;
    Status s = reader_->read(kWALRecordHeaderSize, &hview, header);
    if (!s.is_ok()) { return s; }
    bytes_read_ += hview.size();

    if (hview.size() < kWALRecordHeaderSize) {
        // Clean end of log, or a header cut short by a crash.  Either way the
        // incomplete record is discarded.
        *eof = true;
        return Status::ok();
    }

    const uint32_t stored_crc = decode_fixed_32(header);
    const uint32_t length = decode_fixed_32(header + 4);

    if (length > max_record_size_) {
        return Status::corruption("wal: record length exceeds maximum", std::to_string(length));
    }

    std::string_view payload;
    if (length > 0) {
        scratch->resize(length);
        s = reader_->read(length, &payload, scratch->data());
        if (!s.is_ok()) { return s; }
        bytes_read_ += payload.size();
        if (payload.size() != length) {
            // Torn payload at the tail: the header was complete but the payload
            // was cut short by a crash.  Treat as end of log.
            *eof = true;
            return Status::ok();
        }
    }

    if (verify_checksums_) {
        uint32_t crc = value(header + 4, 4);
        if (length > 0) { crc = extend(crc, payload.data(), payload.size()); }
        if (crc != stored_crc) { return Status::corruption("wal: record checksum mismatch"); }
    }

    *record = payload;
    return Status::ok();
}

} // namespace ANCHORDB_NAMESPACE
