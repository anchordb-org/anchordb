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

#include "memtable/entry.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

char *MemtableEntry::encode_key(char *dst) const {
    assert(dst != nullptr);
    assert(is_valid_entry_type(type_));
    assert(key_.size() <= UINT32_MAX);

    if (!key_.empty()) {
        std::memcpy(dst, key_.data(), key_.size());
        dst += key_.size();
    }
    encode_fixed_64(dst, timestamp_);
    dst += kTimestampSize;
    *dst++ = static_cast<char>(static_cast<uint8_t>(type_));
    return dst;
}

char *MemtableEntry::encode(char *dst) const {
    assert(value_.size() <= UINT32_MAX);

    char *p = encode_varint_32(dst, static_cast<uint32_t>(key_.size()));
    p = encode_key(p);
    p = encode_varint_32(p, static_cast<uint32_t>(value_.size()));
    if (!value_.empty()) {
        std::memcpy(p, value_.data(), value_.size());
        p += value_.size();
    }
    return p;
}

bool MemtableEntry::decode(std::string_view encoded, MemtableEntry *out) {
    if (out == nullptr) { return false; }

    std::string_view cursor = encoded;
    uint32_t key_size = 0;
    if (!get_varint_32(&cursor, &key_size)) { return false; }
    if (cursor.size() < static_cast<size_t>(key_size) + kKeyTrailerSize) { return false; }

    out->key_ = cursor.substr(0, key_size);
    cursor.remove_prefix(key_size);
    out->timestamp_ = decode_fixed_64(cursor.data());
    out->type_ = static_cast<EntryType>(static_cast<uint8_t>(cursor[kTimestampSize]));
    if (!is_valid_entry_type(out->type_)) { return false; }
    cursor.remove_prefix(kKeyTrailerSize);

    uint32_t value_size = 0;
    if (!get_varint_32(&cursor, &value_size) || cursor.size() < static_cast<size_t>(value_size)) { return false; }

    out->value_ = cursor.substr(0, value_size);
    cursor.remove_prefix(value_size);
    return cursor.empty();
}

const char *MemtableEntry::decode_key(const char *entry, MemtableEntry *out) {
    if (entry == nullptr || out == nullptr) { return nullptr; }

    uint32_t key_size = 0;
    const char *p = get_varint_32_ptr(entry, entry + 5, &key_size);
    if (p == nullptr) { return nullptr; }

    out->value_ = std::string_view();
    out->key_ = std::string_view(p, key_size);
    p += key_size;
    out->timestamp_ = decode_fixed_64(p);
    out->type_ = static_cast<EntryType>(static_cast<uint8_t>(p[kTimestampSize]));
    if (!is_valid_entry_type(out->type_)) { return nullptr; }
    return p + kKeyTrailerSize;
}

std::string_view MemtableEntry::key_view(const char *entry) {
    assert(entry != nullptr);

    uint32_t key_size = 0;
    const char *p = get_varint_32_ptr(entry, entry + 5, &key_size);
    if (p == nullptr) { return std::string_view(); }
    return std::string_view(p, key_size);
}

std::string_view MemtableEntry::decode_value(const char *value_field) {
    assert(value_field != nullptr);

    uint32_t value_size = 0;
    const char *p = get_varint_32_ptr(value_field, value_field + 5, &value_size);
    if (p == nullptr) { return std::string_view(); }
    return std::string_view(p, value_size);
}

LookUpKey::LookUpKey(std::string_view user_key, uint64_t timestamp, EntryType type)
    : timestamp_(timestamp), type_(type) {
    assert(is_valid_entry_type(type));
    assert(user_key.size() <= UINT32_MAX);

    const size_t needed = MemtableEntry::key_encoded_size(user_key.size());
    char *dst = needed <= sizeof(space_) ? space_ : new char[needed];
    start_ = dst;
    dst = encode_varint_32(dst, static_cast<uint32_t>(user_key.size()));
    key_start_ = dst;

    const MemtableEntry entry(user_key, timestamp, type, std::string_view());
    end_ = entry.encode_key(dst);
}

} // namespace ANCHORDB_NAMESPACE
