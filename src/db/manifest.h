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
#include "anchordb/env.h"
#include "db/version_edit.h"

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

// RAII owner of the writable MANIFEST file and its record log.  Encapsulates
// the header-vs-append distinction: create() makes a fresh file (LogWriter
// writes the file header) and open_append() reopens an existing file at its
// current end.  Destruction closes the file, so no caller has to remember to
// close and free the writer on every error path.
//
// This class is transport only: it appends VersionEdit records but does not
// validate or apply them.  Those semantics belong to VersionSet.
class ManifestFile {
  public:
    // Creates a fresh MANIFEST at `path`, writing the file header.  On success
    // `*result` owns the file; on failure it is left null.
    static Status create(Env *env, const std::string &path, ManifestFile **result);

    // Opens an existing MANIFEST for appending after its current contents.  The
    // caller must have validated the existing record stream (normally by
    // replaying it first).
    static Status open_append(Env *env, const std::string &path, ManifestFile **result);

    ~ManifestFile();

    ManifestFile(const ManifestFile &) = delete;
    ManifestFile &operator=(const ManifestFile &) = delete;

    // Appends one edit as a MANIFEST record.  See VersionEdit::append_to().
    Status append(const VersionEdit &edit);

    Status flush();
    Status sync();

    // Bytes written so far, used for rotation decisions.
    uint64_t bytes_written() const;

    // Flushes and closes the file.  Also invoked by the destructor, whose
    // result is intentionally ignored.
    Status close();

  private:
    ManifestFile() = default;

    WritableFileWriter *file_ = nullptr;
    ManifestWriter *log_ = nullptr;
};

// RAII owner of a sequential MANIFEST reader and its record log.  Opens the
// file, validates the header and file size, and yields VersionEdit records one
// at a time.  Destruction releases the reader.
//
// The header is validated when the iterator is constructed: a failure then is
// reported by the first next() (and position() stays 0).  A later failure means
// a truncated record tail; position() then points at the start of the failed
// record, i.e. the last complete-record boundary, which recovery can truncate
// to.
class ManifestIterator {
  public:
    // Opens `path` for sequential replay.  Fails with corruption when the file
    // is shorter than a MANIFEST header.
    static Status open(Env *env, const std::string &path, ManifestIterator **result);

    ~ManifestIterator();

    ManifestIterator(const ManifestIterator &) = delete;
    ManifestIterator &operator=(const ManifestIterator &) = delete;

    bool done() const { return log_->done(); }

    // Reads and decodes the next record into `edit`.  See LogReader::next().
    Status next(VersionEdit *edit);

    // Absolute offset of the next record; after a failed next(), the end of the
    // last complete record.
    uint64_t position() const { return log_->position(); }
    uint64_t file_size() const { return size_; }

  private:
    ManifestIterator() = default;

    SequentialFileReader *reader_ = nullptr;
    ManifestReader *log_ = nullptr;
    uint64_t size_ = 0;
};

} // namespace ANCHORDB_NAMESPACE
