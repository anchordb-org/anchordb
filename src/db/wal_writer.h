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

#include "anchordb/status.h"
#include "db/wal_format.h"
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

class WritableFileWriter;

// Writes a stream of records to an AnchorDB write-ahead log (see
// db/wal_format.h for the on-disk format).
//
// The writer emits the file header when it is constructed, so it must be given
// a fresh (empty) file.  append() writes exactly one record as one header plus
// payload, with a checksum covering the length and payload.  Call flush() (hand
// buffered bytes to the file layer) and/or sync() (flush + fsync) before
// acknowledging writes as durable.
//
// A record larger than `max_record_size` is rejected with
// Status::invalid_argument and nothing is written; this logical rejection does
// not poison the writer.  File-operation errors are sticky: once append(),
// flush() or sync() fails, every later call returns the same Status without
// touching the file again.
//
// Not thread-safe: use a single writer per file, serialized by the caller.
class WALWriter {
  public:
    // `max_record_size` bounds one record's payload and is also the read-side
    // corruption guard.  Zero is clamped up to one byte, and values above
    // UINT32_MAX are clamped down (the record length field is 32-bit).
    explicit WALWriter(WritableFileWriter *dest, size_t max_record_size = kWALDefaultMaxRecordSize);

    WALWriter(const WALWriter &) = delete;
    WALWriter &operator=(const WALWriter &) = delete;

    ~WALWriter();

    // Appends one record.  The record may be empty.  Returns the sticky
    // status; if a previous operation failed this returns that error and does
    // not write anything.
    Status append(std::string_view record);

    // Flushes buffered bytes to the underlying file.
    Status flush();

    // Flushes and fsyncs the underlying file (makes prior appends durable).
    Status sync();

    // Sticky status of the writer: non-ok once any I/O operation has failed.
    const Status &status() const { return status_; }

    // Number of physical bytes handed to the file so far (file header, record
    // headers and payloads).  Best-effort diagnostic metric.
    uint64_t bytes_written() const { return bytes_written_; }

  private:
    // Encodes and appends one record; updates the sticky status.
    Status append_record_(std::string_view payload);

    WritableFileWriter *dest_;
    size_t max_record_size_;
    Status status_;
    uint64_t bytes_written_ = 0;
};

} // namespace ANCHORDB_NAMESPACE
