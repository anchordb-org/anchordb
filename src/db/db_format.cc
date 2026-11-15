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

#include "db_format.h"
#include "util/coding.h"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

// Decodes the trailing 8 bytes of an internal key into sequence number and
// entry type. Layout: | user_key (n-8 bytes) | sequence (7 bytes,
// little-endian) | type (1 byte) |
bool parse_internal_key(std::string_view internal_key,
                        ParsedInternalKey *result) {
    const size_t n = internal_key.size();
    if (n < 8) return false;

    uint64_t num = decode_fixed_64(internal_key.data() + n - 8);
    uint8_t c = num & 0xff;
    result->sequence = num >> 8;
    result->type = static_cast<EntryType>(c);
    result->user_key = std::string_view(internal_key.data(), n - 8);
    return (c <= static_cast<uint8_t>(EntryType::Blob));
}

void append_blob_handle(std::string *dst, const BlobHandle &handle) {
    assert(dst != nullptr);
    put_fixed_32(dst, handle.file_id);
    put_fixed_32(dst, handle.size);
    put_fixed_64(dst, handle.offset);
}

bool parse_blob_handle(std::string_view encoded, BlobHandle *handle) {
    if (handle == nullptr || encoded.size() != kBlobHandleEncodedSize) {
        return false;
    }
    const char *p = encoded.data();
    handle->file_id = decode_fixed_32(p);
    handle->size = decode_fixed_32(p + sizeof(uint32_t));
    handle->offset = decode_fixed_64(p + 2 * sizeof(uint32_t));
    return true;
}

// Constructs a memtable lookup key.  The wire format is:
//   | varint32(internal_key_size) | user_key | packed(seq, EntryType) |
// The varint32 length prefix is required by the memtable's internal encoding.
LookUpKey::LookUpKey(std::string_view user_key, seq_num seq) {
    size_t usize = user_key.size();
    size_t needed = usize + 13;
    char *dst;
    if (needed <= sizeof(space_))
        dst = space_;
    else
        dst = new char[needed];
    start_ = dst;
    dst = encode_varint_32(dst, usize + 8);
    kstart_ = dst;
    std::memcpy(dst, user_key.data(), usize);
    dst += usize;
    // Use the largest EntryType so the lookup key sorts before every entry
    // stored at `seq`, regardless of that entry's actual type.
    encode_fixed_64(dst, pack_seq_and_type(seq, EntryType::Blob));
    dst += 8;
    end_ = dst;
}

} // namespace ANCHORDB_NAMESPACE
