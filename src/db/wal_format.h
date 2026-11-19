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

#include <cstddef>
#include <cstdint>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

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

} // namespace ANCHORDB_NAMESPACE
