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

#include "util/coding.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

// Classifies a stored entry across the memtable, data records, and index
// blocks.  The numeric values are stable: every record carries the type in a
// dedicated byte, while blob marks a value stored in the vLog.
enum class EntryType : uint8_t {
    Tombstone = 0x0,
    Inline = 0x1,
    Blob = 0x2,
};

// Default upper bound on a stored user key.  Components that accept keys
// (memtable, index blocks, summary index) enforce this limit, or the effective
// limit derived from their configuration when it is smaller.
inline constexpr size_t kDefaultMaxKeySize = 4096; // 4KB

// True when `type` is one of the three defined EntryType values.
inline bool is_valid_entry_type(EntryType type) {
    return static_cast<uint8_t>(type) <= static_cast<uint8_t>(EntryType::Blob);
}

// A reference to an encoded record in a WiscKey value-log file.  The DB layer
// writes values larger than the configured threshold to the vLog and stores an
// EntryType::Blob entry whose value bytes are this handle: the memtable keeps
// the handle as given, the SSTable index carries it in its slot, and the DB
// read path resolves it.  `offset` and `size` address the whole encoded record
// (header included), so a reader can fetch exactly those bytes and decode them
// with RecordBufferReader.
struct BlobHandle {
    uint32_t file_id = 0;
    uint32_t size = 0;
    uint64_t offset = 0;
};

// On-memory encoding of a BlobHandle:
//   | file_id (4B) | size (4B) | offset (8B) |
inline constexpr size_t kBlobHandleEncodedSize = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint64_t);

// Appends the fixed-width encoding of `handle` to *dst.
inline void append_blob_handle(std::string *dst, const BlobHandle &handle) {
    put_fixed_32(dst, handle.file_id);
    put_fixed_32(dst, handle.size);
    put_fixed_64(dst, handle.offset);
}

// Decodes a BlobHandle from `encoded`, which must be exactly
// kBlobHandleEncodedSize bytes.  Returns false on a wrong-sized input.
inline bool parse_blob_handle(std::string_view encoded, BlobHandle *handle) {
    if (handle == nullptr || encoded.size() != kBlobHandleEncodedSize) { return false; }
    const char *p = encoded.data();
    handle->file_id = decode_fixed_32(p);
    handle->size = decode_fixed_32(p + sizeof(uint32_t));
    handle->offset = decode_fixed_64(p + 2 * sizeof(uint32_t));
    return true;
}

// A versioned value as produced by the read path.  Storage layers (memtable and
// SSTable reader) return this instead of materializing blob bytes: `type` says
// how to interpret the payload, `timestamp` orders the version, and the payload
// is either an inline view or a vLog handle.  Blob values are never read here;
// the DB layer resolves them through BlobResolver.
//
// `inline_value` is valid for Inline and Tombstone entries and points into
// storage owned by the producing iterator/read call.  It is empty for Blob.
struct ValueRef {
    EntryType type = EntryType::Inline;
    uint64_t timestamp = 0;
    std::string_view inline_value;
    BlobHandle blob;

    bool is_blob() const { return type == EntryType::Blob; }
    bool is_tombstone() const { return type == EntryType::Tombstone; }
};

// Value-log entries are addressed directly by the index: the index slot stores
// the vLog file id, the byte offset of the complete record within that file,
// and the full encoded record size, so no intermediate pointer is written into
// the data record.

} // namespace ANCHORDB_NAMESPACE
