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
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ANCHORDB_NAMESPACE {

class WritableFileWriter;
class SequentialFileReader;

// Self-describing record format used by data.bin and the WiscKey value log
// (vLog).  The stateless encode/decode functions below are the single
// implementation of this layout; RecordWriter, RecordReader and
// RecordBufferReader all call them.
//
// data.bin stores inline and tombstone records, and an index slot addresses a
// record by its (offset, size) in data.bin.  The vLog stores blob records with
// the same layout, and a blob's index slot addresses the complete vLog record
// by (file_id, record offset, record size), starting at the record header.
//
// clang-format off
/*
+------------+----------------+-----------+---------------+-----------+---------------------+-------------+
| crc32 (4B) | timestamp (8B) | type (1B) | key_size (2B) | key_bytes | value_size (Varint) | value_bytes |
+------------+----------------+-----------+---------------+-----------+---------------------+-------------+
*/
// clang-format on
//
// Field details:
//   - crc32:        masked CRC32 (see util/crc32.h) computed over every byte
//                   that follows the crc32 field (timestamp .. value_bytes).
//   - timestamp:    uint64_t, little-endian.
//   - type:         EntryType byte: tombstone (0x0), inline_ (0x1), or blob
//                   (0x2).
//   - key_size:     uint16_t byte count of key_bytes.  A key can never exceed
//                   the 64KB index-block segment limit, so a fixed 16-bit
//                   field is enough.
//   - value_size:   varint32 byte count of value_bytes.  A value can never
//                   exceed 4GB, so it fits in 32 bits.

inline constexpr size_t kDataRecordCrcSize = sizeof(uint32_t);       // 4
inline constexpr size_t kDataRecordTimestampSize = sizeof(uint64_t); // 8

// Fixed prefix before key_size: crc + timestamp + type.
inline constexpr size_t kDataRecordHeaderSize = kDataRecordCrcSize + kDataRecordTimestampSize + sizeof(uint8_t); // 13

// Fixed on-disk size of the key_size field.
inline constexpr size_t kDataRecordKeySizeSize = sizeof(uint16_t); // 2

// Largest key a record may carry.  The writer rejects a larger key and every
// decoder reports one as corruption.
inline constexpr size_t kDataRecordMaxKeySize = UINT16_MAX;

// A fully decoded record.  `key` and `value` are zero-copy views into the
// buffer that was decoded and must not outlive it.
struct ParsedDataRecord {
    std::string_view key;
    std::string_view value;
    EntryType type = EntryType::Inline;
    uint64_t timestamp = 0;

    // True when this record holds a large value that belongs in the vLog
    // instead of inline in data.bin.  In a vLog record the value bytes are
    // still in `value`; the index slot addresses the complete record.
    bool is_blob() const { return type == EntryType::Blob; }
    // True when this record marks a deleted key.
    bool is_deletion() const { return type == EntryType::Tombstone; }
};

// Returns the exact number of bytes encode_data_record() appends for a record
// with the given key and value sizes.
size_t data_record_encoded_len(size_t key_size, size_t value_size);

// Appends one record to *dst and returns the number of bytes appended.
// `value_bytes` is the raw value (for a record written to the vLog, the blob
// payload).  `key` must not exceed kDataRecordMaxKeySize and `type` must be a
// valid EntryType; both are asserted.  The masked CRC is computed and patched
// in after the payload is written.
size_t encode_data_record(std::string *dst, uint64_t timestamp, std::string_view key, std::string_view value_bytes,
                          EntryType type);

// Decodes one record from the front of *input and advances *input past it.  Any
// trailing bytes are left for the next call, so this can walk a buffer holding
// several records.  The views in *result point into the original buffer.
Status decode_data_record(std::string_view *input, ParsedDataRecord *result);

// Decodes exactly one complete record from `record` (no trailing bytes).
Status decode_data_record(std::string_view input, ParsedDataRecord *result);

// Where a record landed inside a record file: the absolute byte offset of the
// record header and the total encoded size.  This is exactly what an index slot
// records for an inline entry in data.bin and for a blob entry in a vLog file.
struct DataRecordLocation {
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
class DataRecordWriter {
  public:
    // `initial_size` must be the number of bytes already present in `dest`.
    // Pass 0 for a freshly created or truncated file; when `dest` is an
    // appendable writer for an existing file, pass that file's current size so
    // record locations continue at the real end of the file.  A `seal_size` of
    // 0 disables sealing.
    DataRecordWriter(WritableFileWriter *dest, uint64_t initial_size, uint64_t seal_size = 0);

    DataRecordWriter(const DataRecordWriter &) = delete;
    DataRecordWriter &operator=(const DataRecordWriter &) = delete;

    // Encodes one record and appends it.  `value_bytes` is the raw value (for a
    // record written to the vLog, the blob payload).  The key limit and entry
    // type are checked before anything is written.  On success fills *out (if
    // non-null) with the record's location.
    Status add(std::string_view key, std::string_view value_bytes, EntryType type, uint64_t timestamp,
               DataRecordLocation *out);

    // Appends a record that has already been encoded in this format, for
    // callers that already hold the bytes (for example a compaction copying an
    // unchanged record between data files).  The bytes are not revalidated.
    // Returns Status::invalid_argument if the record cannot fit in a 32-bit
    // location.
    Status append(std::string_view encoded_record, DataRecordLocation *out);

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
class DataRecordReader {
  public:
    // Reads from an already-open sequential file positioned at the first
    // record.  `reader` is borrowed and must outlive this object.
    explicit DataRecordReader(SequentialFileReader *reader);

    DataRecordReader(const DataRecordReader &) = delete;
    DataRecordReader &operator=(const DataRecordReader &) = delete;

    ~DataRecordReader();

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
class DataRecordBufferReader {
  public:
    explicit DataRecordBufferReader(std::string_view buffer) : buffer_(buffer) {}

    DataRecordBufferReader(const DataRecordBufferReader &) = delete;
    DataRecordBufferReader &operator=(const DataRecordBufferReader &) = delete;

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
