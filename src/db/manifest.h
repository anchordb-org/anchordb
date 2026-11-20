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

// AnchorDB MANIFEST (format version 1).
//
// The MANIFEST is a generic record log (see db/log.h for the framing) whose
// records are VersionEdit payloads.  These wrappers bind LogWriter/LogReader to
// the manifest file magic and format version, so no other component needs to
// carry those values or can read a MANIFEST with another log's settings.

// 'A''N''D''M' on disk (little-endian encoding of this value).
inline constexpr uint32_t kManifestFileMagic = 0x4D444E41u;
inline constexpr uint16_t kManifestFormatVersion = 1;
inline constexpr size_t kManifestFileHeaderSize = kLogFileHeaderSize;
inline constexpr size_t kManifestRecordHeaderSize = kLogRecordHeaderSize;
inline constexpr size_t kManifestDefaultMaxRecordSize = kLogDefaultMaxRecordSize;

// Writes the MANIFEST record stream.  See LogWriter for the full contract.
class ManifestWriter final : public LogWriter {
  public:
    explicit ManifestWriter(WritableFileWriter *dest, size_t max_record_size = kManifestDefaultMaxRecordSize)
        : LogWriter(dest, kManifestFileMagic, kManifestFormatVersion, max_record_size) {}

    // Appends to an existing MANIFEST whose header has already been validated
    // (normally by replaying it).  See LogWriter's LogAppendTag constructor.
    ManifestWriter(WritableFileWriter *dest, LogAppendTag, uint64_t existing_bytes,
                   size_t max_record_size = kManifestDefaultMaxRecordSize)
        : LogWriter(dest, kLogAppend, existing_bytes, max_record_size) {}
};

// Reads a MANIFEST record stream written by ManifestWriter.  See LogReader for
// the full contract.
class ManifestReader final : public LogReader {
  public:
    ManifestReader(SequentialFileReader *reader, size_t file_size,
                   size_t max_record_size = kManifestDefaultMaxRecordSize, bool verify_checksums = true)
        : LogReader(reader, file_size, kManifestFileMagic, kManifestFormatVersion, max_record_size, verify_checksums) {}
};

} // namespace ANCHORDB_NAMESPACE
