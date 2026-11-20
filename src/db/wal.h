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

#include "db/log.h"
#include <cstddef>
#include <cstdint>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

// AnchorDB write-ahead log (format version 1).
//
// The WAL is a generic record log (see db/log.h for the framing) whose records
// are WAL entries.  These wrappers bind the generic LogWriter/LogReader to the
// WAL file magic and format version, so callers cannot accidentally read one
// with another log's settings.  The MANIFEST version-edit log uses the same
// LogWriter/LogReader with its own magic and version.

// 'A''N''D''W' on disk (little-endian encoding of this value).
inline constexpr uint32_t kWALFileMagic = 0x57444E41u;
inline constexpr uint16_t kWALFormatVersion = 1;
inline constexpr size_t kWALFileHeaderSize = kLogFileHeaderSize;
inline constexpr size_t kWALRecordHeaderSize = kLogRecordHeaderSize;
inline constexpr size_t kWALDefaultMaxRecordSize = kLogDefaultMaxRecordSize;

// Writes a stream of WAL records.  See LogWriter for the full contract.
class WALWriter final : public LogWriter {
  public:
    explicit WALWriter(WritableFileWriter *dest, size_t max_record_size = kWALDefaultMaxRecordSize)
        : LogWriter(dest, kWALFileMagic, kWALFormatVersion, max_record_size) {}
};

// Reads the record stream written by WALWriter.  See LogReader for the full
// contract.
class WALReader final : public LogReader {
  public:
    WALReader(SequentialFileReader *reader, size_t file_size, size_t max_record_size = kWALDefaultMaxRecordSize,
              bool verify_checksums = true)
        : LogReader(reader, file_size, kWALFileMagic, kWALFormatVersion, max_record_size, verify_checksums) {}
};

} // namespace ANCHORDB_NAMESPACE
