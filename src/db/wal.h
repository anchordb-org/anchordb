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
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

class WritableFileWriter;
class SequentialFileReader;

// AnchorDB write-ahead log on-disk format (format version 1).
//
// A 6-byte file header is followed by a run of records.  One record is one
// atomic WAL entry: it is written as a single unit and returned by the reader
// as a single view.  Records are never split; a record larger than the
// configured maximum is rejected by the writer instead.
//
// File header (6 bytes, once at offset 0):
//   +-------------------+------------------+
//   | file_magic (4B LE)| version (2B LE)  |
//   +-------------------+------------------+
//
// Record (8-byte header followed by `payload_len` payload bytes):
//   +-------------------+------------------+---------+
//   | crc32 (4B LE)     | payload_len (4B) | payload |
//   +-------------------+------------------+---------+
//     raw CRC32           LE                 N bytes
//
//   - crc32 covers `payload_len` followed by the payload: everything after the
//     checksum field.
//   - The length is fixed-width so the reader can size the payload before
//     decoding it.  There is no per-record magic: the file header identifies
//     the stream and the checksum validates each record.
// ---------------------------------------------------------------------------

// 'A''N''D''W' on disk (little-endian encoding of this value).
inline constexpr uint32_t kWALFileMagic = 0x57444E41u;
inline constexpr uint16_t kWALFormatVersion = 1;
inline constexpr size_t kWALFileHeaderSize = 4 + 2; // magic, version

inline constexpr size_t kWALRecordHeaderSize = 4 + 4; // crc32, payload_len

// Default upper bound on one record's payload.  This is a corruption guard: a
// length above it is rejected before any buffer is resized.
inline constexpr size_t kWALDefaultMaxRecordSize = 64u << 20; // 64 MiB

// Writes a stream of records to an AnchorDB write-ahead log (see
// the on-disk format above).
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

// Reads the record stream written by WALWriter (see the on-disk format above).
//
// The file size is supplied by the caller, obtained from Env::get_file_size at
// the time the stream is opened, and bounds every read: there is no end-of-file
// flag.  The file header is consumed and validated when the reader is
// constructed; call next() while !done().
//
// next() reads exactly one record into an internal buffer and returns a view of
// its payload.  A record is never reassembled: it is returned as a single view,
// either into that buffer or, for an empty record, empty.  The view is valid
// only until the next call to next().
//
// Because the file size is authoritative, a truncated record (a partial header
// or payload left by a crash) is not silently treated as a clean end; it is
// reported as corruption, and the caller decides how to treat an interrupted
// tail.  The same rule as DataRecordReader applies:
//
// Return contract:
//   * ok   : one record was read and *record is set (an empty record yields an
//            empty view).
//   * !ok  : corruption or I/O error.  The reader position is unspecified, so a
//            failed reader should not be reused.
//
// Not thread-safe: use a single reader per file.
class WALReader {
  public:
    // Reads from an already-open sequential file positioned at the first byte
    // of the file header.  `reader` is borrowed and must outlive this object.
    // `file_size` is the file's byte length and bounds the record reads.  An
    // empty file is a valid, empty log.
    //
    // `max_record_size` must be at least the writer's value; a record claiming
    // more is rejected as corruption before any buffer is resized.  Zero is
    // clamped up to one byte and values above UINT32_MAX are clamped down.
    WALReader(SequentialFileReader *reader, size_t file_size, size_t max_record_size = kWALDefaultMaxRecordSize,
              bool verify_checksums = true);

    WALReader(const WALReader &) = delete;
    WALReader &operator=(const WALReader &) = delete;

    ~WALReader();

    // Reads the next record into *record.  See the class comment for the return
    // contract.  Must not be called once done() is true.
    Status next(std::string_view *record);

    // True once position() has reached the file size.
    bool done() const { return pos_ >= size_; }

    // Absolute file offset of the next record.
    size_t position() const { return pos_; }
    size_t size() const { return size_; }

  private:
    // Reads exactly `n` bytes and appends them to buffer_.
    Status read_exact_(size_t n);

    SequentialFileReader *reader_;
    size_t size_ = 0;
    size_t pos_ = 0;
    size_t max_record_size_;
    bool verify_checksums_;
    // Sticky status from the file-header read in the constructor.  A later I/O
    // error is returned by next() and is not stored here.
    Status status_;
    // Bytes of the record currently being decoded; the returned view points
    // here.
    std::string buffer_;
};

} // namespace ANCHORDB_NAMESPACE
