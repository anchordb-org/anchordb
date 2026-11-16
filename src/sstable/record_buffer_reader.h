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
#include <string_view>

namespace ANCHORDB_NAMESPACE {

// Sequential decoder for records that are already in memory.
//
// Construct it over a buffer holding one or more complete records, such as the
// bytes fetched with a RandomAccessFileReader at an index handle's offset and
// size.  next() decodes one record at a time and advances through the buffer;
// it works the same for a single record or many.
//
// The buffer is borrowed and must outlive the reader.  Returned key/value views
// point into it.  A truncated or malformed record is reported as corruption; a
// caller that fetches exact index ranges should never see a partial record.  On
// error, next() leaves the read position unchanged.
//
// Not thread-safe: use a single reader per buffer.
//
// Typical loop:
//   bool done = false;
//   while (!done) {
//       ParsedDataRecord record;
//       Status s = reader.next(&record, &done);
//       if (!s.is_ok()) { ... }
//       if (done) break;
//       use(record);
//   }
class RecordBufferReader {
  public:
    explicit RecordBufferReader(std::string_view buffer) : buffer_(buffer) {}

    RecordBufferReader(const RecordBufferReader &) = delete;
    RecordBufferReader &operator=(const RecordBufferReader &) = delete;

    // Decodes the next record.  Sets *done=false and fills *result when a
    // record was decoded, or sets *done=true when the buffer is exhausted.
    Status next(ParsedDataRecord *result, bool *done);

    // True when no bytes remain.
    bool done() const { return pos_ >= buffer_.size(); }

    // Bytes not yet consumed.
    size_t remaining() const { return buffer_.size() - pos_; }

    // Restarts decoding from the beginning of the buffer.
    void reset() { pos_ = 0; }

  private:
    std::string_view buffer_;
    size_t pos_ = 0;
};

} // namespace ANCHORDB_NAMESPACE
