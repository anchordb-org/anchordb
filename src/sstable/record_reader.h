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
#include <string>

namespace ANCHORDB_NAMESPACE {

class SequentialFileReader;

// Sequential reader for the data-record format written by RecordWriter (see
// sstable/data_record.h for the on-disk layout).  The same layout is used by
// data.bin and the vLog, so this reader serves both.
//
// next() reads exactly the bytes of one record through the borrowed
// SequentialFileReader: the fixed header, the uint16_t key_size, the key, the
// varint32 value_size, and the value.  It assembles those bytes in an internal
// buffer, advances the position by data_record_encoded_len(key_size,
// value_size), and decodes the record with decode_data_record.  The
// SequentialFileReader provides read-ahead buffering, so the small field reads
// do not each turn into a file-system read.  It does not own the file: `reader`
// is borrowed and must outlive this object.
//
// The file size comes from SequentialFileReader::size(), so there is no end-of
// -file flag: call next() while !done().  The key/value views in a returned
// ParsedDataRecord point into the reader's internal buffer and are valid only
// until the next call to next().
//
// Return contract:
//   * ok   : one record was decoded and *result is set.
//   * !ok  : corruption or I/O error.  The reader position is unspecified, so a
//            failed reader should not be reused.
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

    // Reads, validates and decodes the next record into *result.  Must not be
    // called once done() is true.
    Status next(ParsedDataRecord *result);

    // True once position() has reached the file size.
    bool done() const { return pos_ >= size_; }

    // Absolute file offset of the next record.
    size_t position() const { return pos_; }
    size_t size() const { return size_; }

  private:
    // Reads exactly `n` bytes and appends them to buffer_.
    Status read_exact_(size_t n);

    // Reads one varint32 into *value, one byte at a time so it never reads past
    // the varint.
    Status read_varint_32_(uint32_t *value);

    SequentialFileReader *reader_;
    size_t size_ = 0;
    size_t pos_ = 0;
    // Bytes of the record currently being decoded; returned views point here.
    std::string buffer_;
};

} // namespace ANCHORDB_NAMESPACE
