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
#include "sstable/data_record.h"
#include <cstddef>
#include <cstdint>
#include <string>

namespace ANCHORDB_NAMESPACE {

class SequentialFileReader;

// Suggested buffer size to request from Env when opening the
// SequentialFileReader that backs a RecordReader.  A vLog GC scan walks every
// record in a file front to back, so a bulk buffer amortizes the per-read
// overhead.  RecordReader keeps no buffer of its own: it assembles each record
// in the caller's scratch buffer and relies on the SequentialFileReader for
// read-ahead.
inline constexpr size_t kRecordReaderBufferSize = 1 << 20; // 1 MiB

// Largest total encoded record size a reader accepts.  RecordWriter (and the
// 32-bit location size in an index slot) cannot represent a record larger than
// this, so anything bigger is corruption rather than a recoverable record.
inline constexpr size_t kRecordReaderMaxRecordSize = 0xFFFFFFFFu;

// Sequential reader for the data-record format written by RecordWriter (see
// sstable/data_record.h for the on-disk layout).
//
// The reader keeps no buffer of its own.  Each record is assembled in the
// caller-supplied `scratch` buffer and decoded there, so the key/value views in
// a returned ParsedDataRecord point into `scratch` and are valid only until the
// next call to read_record() or until the caller reuses `scratch`.  Decoding
// (including the CRC check) happens inside the reader, so callers only ever see
// fully validated records.
//
// Typical use is a vLog garbage-collection scan: open a file, then loop until
// *eof is true.
//
// Return contract (mirrors WALReader):
//   * ok, *eof == false, *result set : one record was decoded.
//   * ok, *eof == true               : clean end of file, or a trailing partial
//                                      record (a torn write from a crash) that
//                                      is discarded.
//   * non-ok                         : corruption or I/O error; *eof == false.
//                                      The reader position is unspecified, so a
//                                      failed reader should not be reused.
//
// Not thread-safe: use a single reader per file.
class RecordReader {
  public:
    // Reads from an already-open sequential file positioned at the first
    // record.  `reader` is borrowed and must outlive this object.
    explicit RecordReader(SequentialFileReader *reader);

    RecordReader(const RecordReader &) = delete;
    RecordReader &operator=(const RecordReader &) = delete;

    ~RecordReader();

    // Reads, validates and decodes the next record into *result.  `scratch` is
    // the per-call record assembly buffer; the returned views point into it.
    Status read_record(ParsedDataRecord *result, bool *eof,
                       std::string *scratch);

    // Number of bytes read from the file so far.  Best-effort diagnostic
    // metric.
    uint64_t bytes_read() const { return bytes_read_; }

  private:
    // Appends exactly `n` bytes to *scratch.  Sets *eof when the file ends
    // first, leaving the bytes read so far appended.
    Status fill_(size_t n, std::string *scratch, bool *eof);

    // Appends one varint64 to *scratch, one byte at a time so the stream is
    // never read past the varint.  Sets *eof if the file ends mid-varint; the
    // caller discards the partial bytes in that case.
    Status read_varint_(std::string *scratch, uint64_t *value, bool *eof);

    SequentialFileReader *reader_;
    uint64_t bytes_read_ = 0;
};

} // namespace ANCHORDB_NAMESPACE
