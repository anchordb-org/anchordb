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

#include "memtable/table.h"

#include <cassert>
#include <cstdint>
#include <string_view>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

EntryType MemtableIterator::type() const {
    MemtableEntry entry;
    if (MemtableEntry::decode_key(iter_.key(), &entry) == nullptr) {
        status_ = Status::corruption("memtable: invalid entry key");
        return EntryType::Inline;
    }
    return entry.type();
}

uint64_t MemtableIterator::timestamp() const {
    MemtableEntry entry;
    if (MemtableEntry::decode_key(iter_.key(), &entry) == nullptr) {
        status_ = Status::corruption("memtable: invalid entry key");
        return 0;
    }
    return entry.timestamp();
}

ValueRef MemtableIterator::value_ref() const {
    status_ = Status::ok();
    ValueRef ref;

    MemtableEntry entry;
    const char *value_field = MemtableEntry::decode_key(iter_.key(), &entry);
    if (value_field == nullptr) {
        status_ = Status::corruption("memtable: invalid entry key");
        return ref;
    }

    ref.type = entry.type();
    ref.timestamp = entry.timestamp();
    const std::string_view stored = MemtableEntry::decode_value(value_field);
    if (entry.type() == EntryType::Blob) {
        if (!parse_blob_handle(stored, &ref.blob)) {
            status_ = Status::corruption("memtable: invalid blob handle");
            return ValueRef();
        }
    } else {
        ref.inline_value = stored;
    }
    return ref;
}

MemtableEntry MemTable::RawIterator::entry() const {
    status_ = Status::ok();
    if (!iter_.valid()) { return MemtableEntry(); }

    MemtableEntry entry;
    const char *value_field = MemtableEntry::decode_key(iter_.key(), &entry);
    if (value_field == nullptr) {
        status_ = Status::corruption("memtable: invalid entry key");
        return MemtableEntry();
    }

    return MemtableEntry(entry.key(), entry.timestamp(), entry.type(), MemtableEntry::decode_value(value_field));
}

Status MemTable::add(uint64_t timestamp, EntryType type, std::string_view key, std::string_view value) {
    if (!is_valid_entry_type(type)) { return Status::invalid_argument("memtable: invalid entry type"); }
    // Compression applies to vLog records only; a memtable entry stores the
    // logical Blob handle and never the vLog record's storage type.
    if (type == EntryType::Compressed) {
        return Status::invalid_argument("memtable: compressed entries are vlog-only");
    }
    if (key.size() > max_key_size_) { return Status::invalid_argument("memtable: key too large"); }

    // A blob entry's value is an encoded BlobHandle supplied by the caller (the
    // DB layer, which owns the vLog); validate it so a malformed handle cannot
    // enter the memtable.
    if (type == EntryType::Blob) {
        BlobHandle handle;
        if (!parse_blob_handle(value, &handle)) {
            return Status::invalid_argument("memtable: blob entry requires an encoded BlobHandle");
        }
    }

    // Both lengths are stored as varint32; reject entries that would truncate.
    if (key.size() > UINT32_MAX || value.size() > UINT32_MAX) {
        return Status::invalid_argument("memtable: entry too large");
    }
    const MemtableEntry entry(key, timestamp, type, value);
    const size_t encoded_len = entry.encoded_size();
    char *buf = arena_.allocate(encoded_len);
    char *end = entry.encode(buf);
    assert(end == buf + encoded_len);
    table_.insert(buf);
    return Status::ok();
}

bool MemTable::get_raw(const LookUpKey &key, ValueRef *out, Status *s) {
    if (out == nullptr) {
        if (s != nullptr) { *s = Status::invalid_argument("memtable: null value ref"); }
        return false;
    }

    typename Table::Iterator iter(&table_);
    iter.seek(key.memtable_key().data());
    if (!iter.valid()) { return false; }

    MemtableEntry entry;
    const char *value_field = MemtableEntry::decode_key(iter.key(), &entry);
    if (value_field == nullptr) {
        if (s != nullptr) { *s = Status::corruption("memtable: invalid entry key"); }
        return false;
    }
    if (comparator_.compare(entry.key(), key.user_key()) != 0) { return false; }

    // Reset first so a reused output cannot retain a stale inline view (or a
    // blob handle) from a previous call.
    *out = ValueRef();
    out->type = entry.type();
    out->timestamp = entry.timestamp();
    const std::string_view stored = MemtableEntry::decode_value(value_field);
    if (entry.type() == EntryType::Blob) {
        if (!parse_blob_handle(stored, &out->blob)) {
            if (s != nullptr) { *s = Status::corruption("memtable: invalid blob handle"); }
            return false;
        }
    } else {
        out->inline_value = stored;
    }
    return true;
}

} // namespace ANCHORDB_NAMESPACE
