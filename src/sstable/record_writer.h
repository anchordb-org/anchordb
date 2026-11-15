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
#include <cstdint>
#include <string_view>

namespace ANCHORDB_NAMESPACE {

class WritableFileWriter;

// Where a record landed inside a record file: the absolute byte offset and the
// total encoded size.  This is exactly what an index slot records for an inline
// entry in data.bin.  For a blob entry the index slot instead addresses the
// value bytes inside a vLog record.
struct RecordLocation {
    uint64_t offset = 0;
    uint32_t size = 0;
};

// Append-only writer for the shared record format (see sstable/data_record.h
// for the wire layout).  The same writer serves both data.bin and the WiscKey
// value log (vLog); it does not care which file `dest` refers to because both
// use identical record entries.
//
// The writer frames records: callers hand it a key, value, type and timestamp,
// and it encodes and appends the record.  It does not own `dest`; the caller
// opens the intended file through Env and keeps it alive.  It tracks the
// running append offset.  For a long-lived append-only file such as the vLog,
// seal_size > 0 makes sealed() report true once the file should be rolled; the
// roll policy is left to the caller.  Destruction does not flush or close
// `dest`; call flush(), sync() or close() explicitly.
//
// File-operation errors are sticky: once append(), flush(), sync() or close()
// fails, later calls return the same Status without attempting further writes,
// so a caller cannot unknowingly continue writing after a partial write.
// close() is the exception: it still attempts to close the underlying file so
// the descriptor is released, then returns the sticky status.
//
// Not thread-safe: use a single writer per file, serialized by the caller.
class RecordWriter {
  public:
    // `initial_size` seeds the offset for an appendable (resumed) file; pass 0
    // for a freshly created file.  A `seal_size` of 0 disables sealing.
    RecordWriter(WritableFileWriter *dest, uint64_t initial_size = 0,
                 uint64_t seal_size = 0);

    RecordWriter(const RecordWriter &) = delete;
    RecordWriter &operator=(const RecordWriter &) = delete;

    // Encodes one record and appends it.  `value_bytes` is the raw value (for a
    // record written to the vLog, the blob payload).  The key limit and entry
    // type are checked before anything is written.  On success fills *out (if
    // non-null) with the record's location.
    Status add(std::string_view key, std::string_view value_bytes,
               EntryType type, uint64_t timestamp, RecordLocation *out);

    // Appends a record that has already been encoded in this format, for
    // callers that already hold the bytes (for example a compaction copying an
    // unchanged record between data files).  The bytes are not revalidated.
    // Returns Status::invalid_argument if the record cannot fit in a 32-bit
    // location.
    Status append(std::string_view encoded_record, RecordLocation *out);

    // Bytes appended so far (also the offset of the next record).
    uint64_t size() const { return size_; }

    // True once size() has reached the seal threshold.  Always false when
    // sealing is disabled.
    bool sealed() const { return seal_size_ != 0 && size_ >= seal_size_; }

    // Sticky status of the writer: non-ok once a file operation has failed.
    const Status &status() const { return status_; }

    Status flush();
    Status sync();
    Status close();

  private:
    WritableFileWriter *dest_;
    uint64_t size_;
    uint64_t seal_size_;
    Status status_;
};

} // namespace ANCHORDB_NAMESPACE
