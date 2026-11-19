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

#include "db/wal.h"

#include "anchordb/env.h"
#include "util/coding.h"
#include "util/crc32.h"
#include <cstring>
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

WALReader::WALReader(SequentialFileReader *reader, size_t file_size, size_t max_record_size, bool verify_checksums)
    : reader_(reader), size_(file_size), max_record_size_(max_record_size), verify_checksums_(verify_checksums) {
    if (max_record_size_ == 0) { max_record_size_ = 1; }
    if (max_record_size_ > UINT32_MAX) { max_record_size_ = UINT32_MAX; }

    // No bytes at all: an empty log is valid.
    if (size_ == 0) { return; }
    if (reader_ == nullptr) {
        status_ = Status::invalid_argument("wal reader: null source");
        return;
    }
    if (size_ < kWALFileHeaderSize) {
        status_ = Status::corruption("wal: truncated file header");
        return;
    }

    // Consume and validate the file header once, up front.  The record stream
    // starts immediately after it.
    char header[kWALFileHeaderSize];
    std::string_view view;
    Status s = reader_->read(kWALFileHeaderSize, &view, header);
    if (!s.is_ok()) {
        status_ = s;
        return;
    }
    if (view.size() != kWALFileHeaderSize) {
        status_ = Status::corruption("wal: truncated file header");
        return;
    }
    // The file reader normally copies into header; move the bytes if an
    // implementation instead exposes its own buffer.
    if (view.data() != header) { std::memmove(header, view.data(), view.size()); }
    if (decode_fixed_32(header) != kWALFileMagic) {
        status_ = Status::corruption("wal: bad file magic");
        return;
    }
    if (decode_fixed_16(header + 4) != kWALFormatVersion) {
        status_ = Status::corruption("wal: unsupported format version");
        return;
    }
    pos_ = kWALFileHeaderSize;
}

WALReader::~WALReader() = default;

Status WALReader::read_exact_(size_t n) {
    // Bounds check against the known file size; no end-of-file probing.
    if (n > size_ - (pos_ + buffer_.size())) { return Status::corruption("wal: record truncated", std::to_string(n)); }

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
    if (!view.empty() && view.data() != dst) { std::memmove(dst, view.data(), view.size()); }
    if (view.size() != n) {
        buffer_.resize(old);
        return Status::corruption("wal: record short read", std::to_string(view.size()));
    }
    return Status::ok();
}

Status WALReader::next(std::string_view *record) {
    if (record == nullptr) { return Status::invalid_argument("wal reader: null result"); }
    *record = std::string_view();
    if (reader_ == nullptr) { return Status::invalid_argument("wal reader: null source"); }
    if (!status_.is_ok()) { return status_; }
    if (done()) { return Status::invalid_argument("wal reader: at end of log"); }
    buffer_.clear();

    // crc32 (4B) | payload_len (4B).
    Status s = read_exact_(kWALRecordHeaderSize);
    if (!s.is_ok()) { return s; }

    const uint32_t stored_crc = decode_fixed_32(buffer_.data());
    const uint32_t length = decode_fixed_32(buffer_.data() + 4);

    // A length above the configured bound is rejected before any buffer grows.
    if (length > max_record_size_) {
        return Status::corruption("wal: record length exceeds maximum", std::to_string(length));
    }

    if (length > 0) {
        s = read_exact_(length);
        if (!s.is_ok()) { return s; }
    }

    if (verify_checksums_) {
        uint32_t crc = value(buffer_.data() + 4, 4);
        if (length > 0) { crc = extend(crc, buffer_.data() + kWALRecordHeaderSize, length); }
        if (crc != stored_crc) { return Status::corruption("wal: record checksum mismatch"); }
    }

    *record = std::string_view(buffer_.data() + kWALRecordHeaderSize, length);
    pos_ += buffer_.size();
    return Status::ok();
}

} // namespace ANCHORDB_NAMESPACE
